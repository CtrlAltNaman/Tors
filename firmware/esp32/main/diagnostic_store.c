#include "diagnostic_store.h"
#include <stddef.h>
#include <string.h>

static void bounded_copy(char *destination, const char *source, size_t capacity) {
    size_t i = 0;
    while (i + 1 < capacity && source[i] != '\0') {
        destination[i] = source[i];
        i++;
    }
    destination[i] = '\0';
}

void diagnostic_store_append(diagnostic_store_t *store, const char *line) {
    bounded_copy(store->events[store->total % DIAGNOSTIC_EVENT_COUNT], line,
                 DIAGNOSTIC_EVENT_BYTES);
    store->total++;
}

uint64_t diagnostic_store_first(const diagnostic_store_t *store) {
    return store->total > DIAGNOSTIC_EVENT_COUNT ? store->total - DIAGNOSTIC_EVENT_COUNT : 0;
}

bool diagnostic_store_read(const diagnostic_store_t *store, uint64_t index,
                           char line[DIAGNOSTIC_EVENT_BYTES]) {
    if (index < diagnostic_store_first(store) || index >= store->total) return false;
    memcpy(line, store->events[index % DIAGNOSTIC_EVENT_COUNT], DIAGNOSTIC_EVENT_BYTES);
    return true;
}

void diagnostic_store_snapshot(diagnostic_store_t *store, const char *text) {
    bounded_copy(store->snapshot, text, sizeof(store->snapshot));
}

float diagnostic_frame_rate(uint64_t frames, int64_t elapsed_us) {
    return elapsed_us > 0 ? (float)((double)frames * 1000000.0 / (double)elapsed_us) : 0;
}
