#pragma once
#include "esp_err.h"
#include "esp_log.h"

/* Task context only. No network I/O in publish/event; never use from an ISR. */
void device_diagnostics_publish(const char *text);
void device_diagnostics_event(esp_log_level_t level, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
/* Call on each GOT_IP event: starts once and announces the current address. */
esp_err_t device_diagnostics_start(const char *ip_address);
