/* Included AFTER the real speaker_playback_task extracted by the Python runner.
 * Only RTOS/I2S/diagnostic/ack adapters are simulated. Packet validation, queue
 * policy, PCM conversion, and the speaker task itself are production code.
 * This tests task policy and ordering, not physical DMA/acoustic timing. */
enum event_kind { PACKET, BEGIN, END };
typedef struct {
    unsigned at_ms, sequence, id;
    enum event_kind kind;
    bool last;
} event_t;
typedef struct { unsigned at_ms, returned_ms; int32_t sample; } write_t;
typedef struct {
    unsigned at_ms, writes;
    uint32_t id, generation;
    bool success;
} ack_t;

static event_t events[64];
static unsigned event_count, next_event, now_ms, stop_ms = 1200, operations;
static write_t writes[64];
static ack_t acks[8];
static unsigned write_count, ack_count, enable_times[8], enable_count, warnings;
static unsigned preloaded;
static bool dma_enabled, inject_last_after_timeout, injected;
static jmp_buf task_exit;

static void receive_packet(unsigned sequence, bool last, unsigned id) {
    uint8_t raw[PLAYBACK_WIRE_BYTES] = {0xa5, 1, 0};
    raw[3] = (sequence == 0 ? 1 : 0) | (last ? 2 : 0);
    raw[4] = (uint8_t)sequence;
    raw[5] = (uint8_t)(sequence >> 8);
    raw[6] = 0x80;
    raw[7] = 2;
    const uint32_t offset = sequence * PLAYBACK_FRAME_SAMPLES;
    for (unsigned i = 0; i < 4; ++i) {
        raw[8+i] = (uint8_t)(id >> (8*i));
        raw[12+i] = (uint8_t)(offset >> (8*i));
    }
    const int16_t sample = (int16_t)(1000 + 100*id + sequence);
    for (unsigned i = 0; i < PLAYBACK_FRAME_SAMPLES; ++i) {
        raw[16+2*i] = (uint8_t)sample;
        raw[17+2*i] = (uint8_t)((uint16_t)sample >> 8);
    }
    playback_buffer_receive(raw, sizeof(raw), sizeof(raw), 0);
}

static void schedule(unsigned at_ms, enum event_kind kind,
                     unsigned sequence, bool last, unsigned id) {
    assert(event_count < 64);
    assert(!event_count || at_ms >= events[event_count-1].at_ms);
    events[event_count++] = (event_t){at_ms, sequence, id, kind, last};
}

static void advance_to(unsigned target_ms) {
    assert(target_ms >= now_ms);
    while (next_event < event_count && events[next_event].at_ms <= target_ms &&
           events[next_event].at_ms < stop_ms) {
        const event_t event = events[next_event++];
        now_ms = event.at_ms;
        if (event.kind == BEGIN) playback_buffer_begin();
        else if (event.kind == END) playback_buffer_end();
        else receive_packet(event.sequence, event.last, event.id);
    }
    now_ms = target_ms;
    if (now_ms >= stop_ms) longjmp(task_exit, 1);
}

int64_t esp_timer_get_time(void) {
    assert(++operations < 100000); /* Fail a busy-loop regression promptly. */
    return (int64_t)now_ms * 1000;
}

QueueHandle_t xQueueCreateStatic(unsigned length, unsigned size,
                                uint8_t *data, StaticQueue_t *q) {
    *q = (StaticQueue_t){.length=length, .size=size, .data=data};
    return q;
}
BaseType_t xQueueReset(QueueHandle_t q) { q->head=q->count=0; return pdTRUE; }
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) { return q->count; }
BaseType_t xQueueSend(QueueHandle_t q, const void *src, TickType_t wait) {
    (void)wait;
    if (q->count == q->length) return pdFALSE;
    memcpy(q->data + ((q->head+q->count)%q->length)*q->size, src, q->size);
    ++q->count;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *dest, TickType_t wait) {
    if (!q->count && wait) {
        const unsigned deadline = now_ms + wait * portTICK_PERIOD_MS;
        while (!q->count && next_event < event_count &&
               events[next_event].at_ms <= deadline) {
            advance_to(events[next_event].at_ms);
        }
        if (!q->count) {
            advance_to(deadline);
            if (inject_last_after_timeout && !injected) {
                /* Timeout has already been selected. Producer commits LAST
                 * before the speaker gets to inspect the terminal state. */
                injected = true;
                receive_packet(5, true, 1);
            }
            return pdFALSE;
        }
    }
    if (!q->count) return pdFALSE;
    memcpy(dest, q->data + q->head*q->size, q->size);
    q->head = (q->head+1)%q->length;
    --q->count;
    return pdTRUE;
}

static void vTaskDelay(TickType_t ticks) {
    assert(ticks > 0);
    advance_to(now_ms + ticks * portTICK_PERIOD_MS);
}
static esp_err_t i2s_channel_preload_data(void *handle, const void *src,
                                       size_t size, size_t *loaded) {
    (void)handle;
    assert(!dma_enabled && size == PLAYBACK_FRAME_SAMPLES * sizeof(int32_t));
    const uint8_t *bytes = src;
    for (size_t i = 0; i < size; ++i) assert(bytes[i] == 0);
    ++preloaded;
    *loaded = size;
    return ESP_OK;
}
static esp_err_t i2s_channel_enable(void *handle) {
    (void)handle;
    assert(!dma_enabled && preloaded == I2S_DMA_DESCRIPTORS);
    assert(enable_count < 8);
    enable_times[enable_count++] = now_ms;
    preloaded = 0;
    dma_enabled = true;
    return ESP_OK;
}
static esp_err_t i2s_channel_disable(void *handle) {
    (void)handle;
    assert(dma_enabled);
    dma_enabled = false;
    return ESP_OK;
}
static esp_err_t i2s_channel_write(void *handle, const void *src, size_t size,
                                 size_t *written, uint32_t timeout_ms) {
    (void)handle;
    (void)timeout_ms;
    assert(dma_enabled && size == PLAYBACK_FRAME_SAMPLES * sizeof(int32_t));
    assert(write_count < 64);
    const int32_t *samples = src;
    for (unsigned i = 1; i < PLAYBACK_FRAME_SAMPLES; ++i)
        assert(samples[i] == samples[0]);
    write_t *write = &writes[write_count++];
    *write = (write_t){.at_ms=now_ms, .sample=samples[0]};
    /* Model bounded, frame-paced writes; physical DMA is intentionally absent. */
    advance_to(now_ms + AUDIO_FRAME_MS);
    write->returned_ms = now_ms;
    *written = size;
    return ESP_OK;
}
static void backend_client_queue_playback_stop(uint32_t id, uint32_t generation,
                                              bool success) {
    assert(ack_count < 8 && !dma_enabled);
    acks[ack_count++] = (ack_t){now_ms, write_count, id, generation, success};
}
static void device_diagnostics_event(int level, const char *format, ...) {
    (void)format;
    assert(level == ESP_LOG_WARN);
    ++warnings;
}

static void initial_five(void) {
    for (unsigned i = 0; i < 5; ++i) schedule(0, PACKET, i, false, 1);
}
static void assert_pcm_sequence(unsigned first, unsigned count, unsigned id) {
    for (unsigned i = 0; i < count; ++i) {
        const int16_t sample = (int16_t)(1000 + 100*id + i);
        assert(writes[first+i].sample == speaker_pcm_to_i2s(sample));
    }
}
static void assert_one_drained_ack(unsigned frames, unsigned id) {
    assert(write_count == frames);
    assert(ack_count == 1);
    assert(acks[0].success && acks[0].id == id);
    assert(playback_buffer_current(acks[0].generation));
    assert(acks[0].writes == frames);
    assert(acks[0].at_ms >= writes[frames-1].returned_ms + SPEAKER_DRAIN_MS);
    assert(!atomic_load(&playback_active));
    playback_stats_t stats;
    playback_buffer_get_stats(&stats);
    assert(stats.queued == 0 && stats.submitted == frames);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *scenario = argv[1];
    assert(playback_buffer_init());
    playback_buffer_begin();
    if (!strcmp(scenario, "five")) {
        for (unsigned i = 0; i < 6; ++i) schedule(20*i, PACKET, i, i==5, 1);
    } else if (!strcmp(scenario, "short")) {
        schedule(0, PACKET, 0, true, 1);
    } else if (!strcmp(scenario, "explicit_stop")) {
        schedule(0, PACKET, 0, false, 1);
        schedule(30, END, 0, false, 1);
    } else if (!strcmp(scenario, "fallback")) {
        schedule(0, PACKET, 0, false, 1);
    } else if (!strcmp(scenario, "steady")) {
        initial_five();
        schedule(120, PACKET, 5, false, 1);
        schedule(160, PACKET, 6, false, 1);
        schedule(200, PACKET, 7, true, 1);
    } else if (!strcmp(scenario, "restart")) {
        initial_five();
        for (unsigned i = 5; i < 11; ++i)
            schedule(400+20*(i-5), PACKET, i, i==10, 1);
    } else if (!strcmp(scenario, "reset")) {
        schedule(0, PACKET, 0, false, 1);
        schedule(30, BEGIN, 0, false, 2);
        schedule(40, PACKET, 0, true, 2);
    } else {
        assert(!strcmp(scenario, "timeout_last_race"));
        initial_five();
        inject_last_after_timeout = true;
    }
    if (setjmp(task_exit) == 0) {
        advance_to(0);
        speaker_playback_task(NULL);
        assert(!"speaker task unexpectedly returned");
    }
    if (!strcmp(scenario, "five")) {
        assert(enable_count == 1 && enable_times[0] == 80);
        assert(writes[0].at_ms == 80);
        assert_one_drained_ack(6, 1);
        assert_pcm_sequence(0, 6, 1);
    } else if (!strcmp(scenario, "short")) {
        assert(enable_count == 1 && enable_times[0] == 0);
        assert_one_drained_ack(1, 1);
    } else if (!strcmp(scenario, "explicit_stop")) {
        assert(enable_count == 1 && enable_times[0] == 30);
        assert_one_drained_ack(1, 1);
    } else if (!strcmp(scenario, "fallback")) {
        assert(enable_count == 1 && enable_times[0] == 250);
        assert(warnings == 1 && write_count == 1 && ack_count == 0);
        assert_pcm_sequence(0, 1, 1);
        assert(!dma_enabled);
    } else if (!strcmp(scenario, "steady")) {
        assert(enable_count == 1 && enable_times[0] == 0);
        assert(writes[5].at_ms == 120 && writes[6].at_ms == 160);
        assert_one_drained_ack(8, 1);
        assert_pcm_sequence(0, 8, 1);
    } else if (!strcmp(scenario, "restart")) {
        assert(enable_count == 2 && enable_times[0] == 0 && enable_times[1] == 480);
        assert(writes[5].at_ms == 480);
        assert_one_drained_ack(11, 1);
        assert_pcm_sequence(0, 11, 1);
    } else if (!strcmp(scenario, "reset")) {
        assert(enable_count == 1 && enable_times[0] == 40);
        assert_one_drained_ack(1, 2);
        assert_pcm_sequence(0, 1, 2);
    } else {
        assert(injected);
        assert_one_drained_ack(6, 1);
        assert_pcm_sequence(0, 6, 1);
    }
    if (strcmp(scenario, "fallback")) assert(warnings == 0);
    printf("%s: policy passed (%u writes, %u acknowledgements)\n",
           scenario, write_count, ack_count);
    return 0;
}
