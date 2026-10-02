#include "playback_buffer.h"
#include <stdatomic.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/queue.h"

/* Queue control/atomics stay internal. Bulk PCM storage is task-only PSRAM. */
static StaticQueue_t queue_control;
static EXT_RAM_BSS_ATTR uint8_t playback_storage[PLAYBACK_QUEUE_FRAMES * sizeof(playback_frame_t)];
static EXT_RAM_BSS_ATTR uint8_t wire[PLAYBACK_WIRE_BYTES];
static _Atomic(QueueHandle_t) queue;
static atomic_uint generation, failed_generation;
static atomic_bool ended;
static atomic_uint received, submitted, overflows, invalid, write_errors, underruns, high_water;
static atomic_uint rx_gap_max_ms, write_max_ms;
/* The fields below have a single writer: the WebSocket receive task. */
static bool accepting, have_id;
static size_t filled;
static uint32_t rx_id, expected_offset;
static uint16_t expected_sequence;
static int64_t last_rx_us;

_Static_assert(PLAYBACK_PREBUFFER_FRAMES <= PLAYBACK_QUEUE_FRAMES, "Prebuffer must fit in queue");

static void update_max(atomic_uint *counter, unsigned value) {
    unsigned previous = atomic_load(counter);
    while (value > previous && !atomic_compare_exchange_weak(counter, &previous, value)) {}
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void bad_frame(void) {
    atomic_fetch_add(&invalid, 1);
    atomic_store(&failed_generation, atomic_load(&generation));
    filled = 0;
}
bool playback_buffer_init(void) {
    if (atomic_load(&queue)) return true;
    atomic_store(&queue, xQueueCreateStatic(PLAYBACK_QUEUE_FRAMES, sizeof(playback_frame_t),
                                           playback_storage, &queue_control));
    return atomic_load(&queue) != NULL;
}
void playback_buffer_reset(void) {
    accepting = false;
    filled = 0;
    have_id = false;
    expected_offset = 0;
    expected_sequence = 0;
    last_rx_us = -1;
    atomic_fetch_add(&generation, 1);
    atomic_store(&ended, false);
    QueueHandle_t q = atomic_load(&queue);
    if (q) xQueueReset(q);
}
void playback_buffer_begin(void) { playback_buffer_reset(); accepting = true; }
void playback_buffer_end(void) { accepting = false; atomic_store(&ended, true); }

void playback_buffer_receive(const void *data, size_t size, size_t total, size_t offset) {
    QueueHandle_t q = atomic_load(&queue);
    if (!q || !accepting) return;
    if (!data || total != sizeof(wire) || offset > total || size > total-offset ||
        (offset != 0 && offset != filled)) { bad_frame(); return; }
    if (offset == 0) filled = 0;
    memcpy(wire+offset, data, size);
    filled = offset+size;
    if (filled != sizeof(wire)) return;
    filled = 0;
    if (wire[0] != 0xa5 || wire[1] != 1 || wire[2] != 0 || le16(wire+6) != 640) {
        bad_frame(); return;
    }
    const uint32_t id = le32(wire+8), sample_offset = le32(wire+12);
    const uint16_t sequence = le16(wire+4);
    if ((!have_id && !(wire[3]&1)) || (have_id && id != rx_id) ||
        sequence != expected_sequence || sample_offset != expected_offset) {
        bad_frame(); return;
    }
    const int64_t now = esp_timer_get_time();
    if (last_rx_us >= 0 && now >= last_rx_us) {
        const uint64_t gap_ms = (uint64_t)(now - last_rx_us) / 1000;
        update_max(&rx_gap_max_ms, gap_ms > UINT32_MAX ? UINT32_MAX : (unsigned)gap_ms);
    }
    last_rx_us = now;
    have_id = true;
    rx_id = id;
    ++expected_sequence;
    expected_offset += PLAYBACK_FRAME_SAMPLES;
    playback_frame_t frame = {.audio_id=id, .generation=atomic_load(&generation), .last=(wire[3]&2)!=0};
    memcpy(frame.samples, wire+16, sizeof(frame.samples));
    atomic_fetch_add(&received, 1);
    /* Bounded backpressure for brief Wi-Fi bursts; never hold the event task indefinitely. */
    if (xQueueSend(q, &frame, pdMS_TO_TICKS(40)) != pdTRUE) {
        atomic_fetch_add(&overflows, 1);
        atomic_store(&failed_generation, frame.generation);
    } else {
        const unsigned depth = uxQueueMessagesWaiting(q);
        update_max(&high_water, depth);
    }
    /* Single receive-side producer: publish the terminal state only after the
     * final enqueue and any failure accounting are visible to the consumer. */
    if (frame.last) playback_buffer_end();
}
bool playback_buffer_read(playback_frame_t *frame, TickType_t wait) {
    QueueHandle_t q = atomic_load(&queue);
    return q && frame && xQueueReceive(q, frame, wait) == pdTRUE;
}
bool playback_buffer_current(uint32_t value) { return value == atomic_load(&generation); }
bool playback_buffer_ready(uint32_t value, unsigned held_frames) {
    QueueHandle_t q = atomic_load(&queue);
    if (!q || !playback_buffer_current(value)) return false;
    const unsigned queued = uxQueueMessagesWaiting(q);
    const bool ready = held_frames >= PLAYBACK_PREBUFFER_FRAMES ||
                       queued >= PLAYBACK_PREBUFFER_FRAMES - held_frames ||
                       ((held_frames || queued) && atomic_load(&ended));
    return ready && playback_buffer_current(value);
}
bool playback_buffer_ended(void) { return atomic_load(&ended); }
bool playback_buffer_drained(uint32_t value) {
    /* Acquire the producer's terminal publication before looking at the queue:
     * no more frames can then be enqueued in this generation. A reset during
     * either observation is rejected by the final generation check. */
    if (!atomic_load(&ended)) return false;
    QueueHandle_t q = atomic_load(&queue);
    return q && uxQueueMessagesWaiting(q) == 0 && playback_buffer_current(value);
}
bool playback_buffer_failed(uint32_t value) {
    return !playback_buffer_current(value) || value == atomic_load(&failed_generation);
}
void playback_buffer_written(uint32_t value, bool success) {
    if (success) atomic_fetch_add(&submitted, 1);
    else {
        atomic_fetch_add(&write_errors, 1);
        atomic_store(&failed_generation, value);
    }
}
void playback_buffer_underrun(void) { atomic_fetch_add(&underruns, 1); }
void playback_buffer_write_timing(uint32_t duration_ms) { update_max(&write_max_ms, duration_ms); }
void playback_buffer_get_stats(playback_stats_t *s) {
    if (!s) return;
    QueueHandle_t q = atomic_load(&queue);
    *s = (playback_stats_t){.received=atomic_load(&received), .submitted=atomic_load(&submitted),
        .overflows=atomic_load(&overflows), .invalid=atomic_load(&invalid),
        .write_errors=atomic_load(&write_errors), .underruns=atomic_load(&underruns),
        .rx_gap_max_ms=atomic_load(&rx_gap_max_ms), .write_max_ms=atomic_load(&write_max_ms),
        .queued=q ? uxQueueMessagesWaiting(q) : 0, .high_water=atomic_load(&high_water)};
}
