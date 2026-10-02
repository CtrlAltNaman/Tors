#pragma once
#include <stdbool.h>
#include <stdint.h>

#define DIAGNOSTIC_EVENT_COUNT 12
#define DIAGNOSTIC_EVENT_BYTES 160
#define DIAGNOSTIC_SNAPSHOT_BYTES 1536

/* Pure bounded storage. Caller must synchronize concurrent access. */
typedef struct {
    char events[DIAGNOSTIC_EVENT_COUNT][DIAGNOSTIC_EVENT_BYTES];
    char snapshot[DIAGNOSTIC_SNAPSHOT_BYTES];
    uint64_t total;
} diagnostic_store_t;

void diagnostic_store_append(diagnostic_store_t *store, const char *line);
uint64_t diagnostic_store_first(const diagnostic_store_t *store);
bool diagnostic_store_read(const diagnostic_store_t *store, uint64_t index,
                           char line[DIAGNOSTIC_EVENT_BYTES]);
void diagnostic_store_snapshot(diagnostic_store_t *store, const char *text);
float diagnostic_frame_rate(uint64_t frames, int64_t elapsed_us);
