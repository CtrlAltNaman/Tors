#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "playback_buffer.h"
#include "freertos/queue.h"
static bool consume_on_wait;
static TickType_t last_wait;
static int64_t clock_us;
static bool reset_on_depth_read;
static unsigned depth_reads;
int64_t esp_timer_get_time(void) { return clock_us; }
QueueHandle_t xQueueCreateStatic(unsigned n, unsigned size, uint8_t *data, StaticQueue_t *q) {
    *q = (StaticQueue_t){.length=n, .size=size, .data=data}; return q;
}
BaseType_t xQueueReset(QueueHandle_t q) { q->head=q->count=0; return pdTRUE; }
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) {
    ++depth_reads;
    if (reset_on_depth_read) {
        reset_on_depth_read = false;
        playback_buffer_reset();
    }
    return q->count;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *dest, TickType_t wait) {
    (void)wait;
    if (!q->count) return pdFALSE;
    memcpy(dest, q->data + q->head*q->size, q->size);
    q->head=(q->head+1)%q->length; --q->count; return pdTRUE;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *src, TickType_t wait) {
    last_wait=wait;
    const playback_frame_t *frame = src;
    // A consumer must not observe end-of-clip before this send commits/fails.
    if (frame->last) assert(!playback_buffer_ended());
    if (q->count==q->length && wait && consume_on_wait) {
        playback_frame_t discarded;
        assert(xQueueReceive(q, &discarded, 0)); consume_on_wait=false;
    }
    if (q->count==q->length) return pdFALSE;
    memcpy(q->data + ((q->head+q->count)%q->length)*q->size, src, q->size);
    ++q->count;
    if (frame->last) assert(!playback_buffer_ended());
    return pdTRUE;
}
static void packet(uint8_t raw[656], unsigned seq, bool last) {
    memset(raw, 0, 656); raw[0]=0xa5; raw[1]=1;
    raw[3]=(seq==0?1:0)|(last?2:0); raw[4]=seq; raw[5]=seq>>8;
    raw[6]=0x80; raw[7]=2; raw[8]=42;
    uint32_t offset=seq*320;
    for (unsigned i=0;i<4;++i) raw[12+i]=(uint8_t)(offset>>(i*8));
    for (unsigned i=0;i<640;++i) raw[16+i]=(uint8_t)(seq+i);
}
static void receive(const uint8_t *raw) { playback_buffer_receive(raw, 656, 656, 0); }
int main(void) {
    uint8_t raw[656]; playback_frame_t frame; playback_stats_t stats;
    assert(PLAYBACK_QUEUE_FRAMES == 64);
    assert(playback_buffer_init()); playback_buffer_begin();
    for (unsigned i=0;i<PLAYBACK_QUEUE_FRAMES;++i) { packet(raw,i,i==PLAYBACK_QUEUE_FRAMES-1); receive(raw); }
    playback_buffer_get_stats(&stats);
    assert(stats.queued==PLAYBACK_QUEUE_FRAMES && stats.overflows==0 && stats.received==PLAYBACK_QUEUE_FRAMES);
    for (unsigned i=0;i<PLAYBACK_QUEUE_FRAMES;++i) {
        assert(playback_buffer_read(&frame,0));
        assert(frame.audio_id==42 && frame.last==(i==PLAYBACK_QUEUE_FRAMES-1));
        assert(((uint8_t *)frame.samples)[0]==i);
    }
    assert(!playback_buffer_read(&frame,0)); assert(playback_buffer_ended());
    assert(playback_buffer_drained(frame.generation));
    playback_buffer_begin();
    for(unsigned i=0;i<PLAYBACK_QUEUE_FRAMES;++i) { packet(raw,i,false); receive(raw); }
    consume_on_wait=true; packet(raw,PLAYBACK_QUEUE_FRAMES,false); receive(raw);
    assert(last_wait==40);
    playback_buffer_get_stats(&stats); assert(stats.overflows==0 && stats.queued==PLAYBACK_QUEUE_FRAMES);
    packet(raw,PLAYBACK_QUEUE_FRAMES+1,true); receive(raw); // Full even after bounded wait: observable loss.
    playback_buffer_get_stats(&stats); assert(stats.overflows==1);
    assert(playback_buffer_ended());
    assert(playback_buffer_read(&frame,0)); assert(playback_buffer_failed(frame.generation));
    assert(!playback_buffer_drained(frame.generation));
    // LAST overflow still publishes ended, but only after loss is accounted for.
    while (playback_buffer_read(&frame,0)) {}
    assert(playback_buffer_drained(frame.generation));
    assert(playback_buffer_failed(frame.generation));
    uint32_t old_generation=frame.generation;
    playback_buffer_begin(); assert(!playback_buffer_current(old_generation));
    assert(!playback_buffer_read(&frame,0));
    packet(raw,0,true);
    playback_buffer_receive(raw,10,656,0);
    assert(!playback_buffer_read(&frame,0));
    playback_buffer_receive(raw+10,646,656,10);
    assert(playback_buffer_read(&frame,0) && frame.last);
    playback_buffer_begin();
    raw[0]=0; receive(raw);
    playback_buffer_get_stats(&stats); assert(stats.invalid==1);
    assert(!playback_buffer_read(&frame,0));
    playback_buffer_begin(); packet(raw,0,false); receive(raw);
    packet(raw,2,true); receive(raw); // Missing sequence/offset.
    playback_buffer_get_stats(&stats); assert(stats.invalid==2);
    assert(playback_buffer_read(&frame,0)); assert(playback_buffer_failed(frame.generation));
    playback_buffer_reset(); assert(!playback_buffer_current(frame.generation));
    assert(!playback_buffer_read(&frame,0));
    // Paced long stream: consumer keeps up, no extra drops.
    playback_buffer_begin();
    for(unsigned i=0;i<1000;++i) {
        packet(raw,i,i==999); receive(raw);
        assert(playback_buffer_read(&frame,0)); playback_buffer_written(frame.generation,true);
    }
    playback_buffer_get_stats(&stats); assert(stats.overflows==1 && stats.submitted==1000);
    playback_buffer_written(frame.generation,false); playback_buffer_get_stats(&stats); assert(stats.write_errors==1);

    // Consumer holds the first frame while waiting for four more, without losing it.
    playback_buffer_begin(); packet(raw,0,false); receive(raw);
    assert(playback_buffer_read(&frame,0));
    assert(!playback_buffer_ready(frame.generation,1));
    for (unsigned i=1;i<PLAYBACK_PREBUFFER_FRAMES;++i) {
        clock_us += 20000; packet(raw,i,false); receive(raw);
        assert(playback_buffer_ready(frame.generation,1) == (i==PLAYBACK_PREBUFFER_FRAMES-1));
    }
    assert(((uint8_t *)frame.samples)[0]==0);
    // Timing high water observes a short arrival gap that the old 250ms counter misses.
    clock_us+=70000; packet(raw,5,false); receive(raw);
    playback_buffer_write_timing(21); playback_buffer_write_timing(4);
    playback_buffer_get_stats(&stats);
    assert(stats.rx_gap_max_ms==70 && stats.write_max_ms==21);
    old_generation=frame.generation;
    playback_buffer_reset(); assert(!playback_buffer_ready(old_generation,5));

    // A one-packet final clip starts immediately; inter-clip idle is not an RX gap.
    clock_us+=10000000;
    playback_buffer_begin(); packet(raw,0,true); receive(raw);
    assert(playback_buffer_read(&frame,0) && playback_buffer_ready(frame.generation,1));
    assert(!playback_buffer_ready(frame.generation,0));
    playback_buffer_get_stats(&stats); assert(stats.rx_gap_max_ms==70);
    // Explicit stop releases a short clip even if its final packet lacked LAST.
    playback_buffer_begin(); packet(raw,0,false); receive(raw);
    assert(playback_buffer_read(&frame,0)); assert(!playback_buffer_ready(frame.generation,1));
    playback_buffer_end(); assert(playback_buffer_ready(frame.generation,1));
    assert(playback_buffer_drained(frame.generation));

    // A timed-out consumer must acquire ended BEFORE inspecting queue depth.
    playback_buffer_begin(); packet(raw,0,false); receive(raw);
    assert(playback_buffer_read(&frame,0));
    unsigned previous_depth_reads = depth_reads;
    assert(!playback_buffer_drained(frame.generation));
    assert(depth_reads == previous_depth_reads);
    // LAST commits just after the read timed out: ended alone is insufficient.
    packet(raw,1,true); receive(raw);
    assert(playback_buffer_ended());
    assert(!playback_buffer_drained(frame.generation));
    assert(playback_buffer_read(&frame,0) && frame.last);
    assert(playback_buffer_drained(frame.generation));
    assert(!playback_buffer_drained(frame.generation-1));
    // Reset between ended/depth observation and the final generation check.
    reset_on_depth_read = true;
    assert(!playback_buffer_drained(frame.generation));
    assert(!reset_on_depth_read);
    puts("Playback: FIFO/bursts, prebuffer, short clips, terminal publication/drain, reset, timing and protocol passed");
}
