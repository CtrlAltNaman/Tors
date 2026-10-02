#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "diagnostic_store.h"

int main(void) {
    diagnostic_store_t store = {0};
    char line[DIAGNOSTIC_EVENT_BYTES];
    assert(!diagnostic_store_read(&store, 0, line));
    assert(diagnostic_store_first(&store) == 0);
    for (unsigned i = 0; i < DIAGNOSTIC_EVENT_COUNT + 3; ++i) {
        snprintf(line, sizeof(line), "event %u", i);
        diagnostic_store_append(&store, line);
    }
    assert(diagnostic_store_first(&store) == 3);
    assert(!diagnostic_store_read(&store, 2, line));
    for (uint64_t i = 3; i < store.total; ++i) {
        char expected[40];
        snprintf(expected, sizeof(expected), "event %u", (unsigned)i);
        assert(diagnostic_store_read(&store, i, line));
        assert(strcmp(line, expected) == 0);
    }
    assert(!diagnostic_store_read(&store, store.total, line));
    char long_line[DIAGNOSTIC_SNAPSHOT_BYTES + 50];
    memset(long_line, 'x', sizeof(long_line) - 1);
    long_line[sizeof(long_line) - 1] = '\0';
    diagnostic_store_append(&store, long_line);
    assert(diagnostic_store_read(&store, store.total - 1, line));
    assert(strlen(line) == DIAGNOSTIC_EVENT_BYTES - 1);
    diagnostic_store_snapshot(&store, long_line);
    assert(strlen(store.snapshot) == DIAGNOSTIC_SNAPSHOT_BYTES - 1);
    diagnostic_store_snapshot(&store, "RAM updated\nMIC updated\n");
    assert(strcmp(store.snapshot, "RAM updated\nMIC updated\n") == 0);
    assert(diagnostic_frame_rate(250, 5000000) == 50.0f);
    assert(fabsf(diagnostic_frame_rate(275, 5500000) - 50.0f) < 0.001f);
    assert(diagnostic_frame_rate(0, 5000000) == 0.0f);
    assert(diagnostic_frame_rate(250, 0) == 0.0f);
    puts("diagnostic store: empty/order/wrap/truncation/snapshot/frame-rate passed");
    return 0;
}
