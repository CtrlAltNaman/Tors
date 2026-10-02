#include "device_diagnostics.h"
#include "diagnostic_store.h"
#include "recordings_http.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static diagnostic_store_t store;
static portMUX_TYPE store_lock = portMUX_INITIALIZER_UNLOCKED;
static httpd_handle_t server;
static const char *TAG = "device_status";

/* Constant page lives in flash. Poll only after the previous request finishes. */
static const char page[] =
    "<!doctype html><html lang=en><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>ESP32 diagnostics</title>"
    "<style>body{font:16px system-ui;margin:24px;background:#101820;color:#eee}"
    "pre{white-space:pre-wrap;overflow-wrap:anywhere;font:14px monospace}"
    "small{color:#bbc}</style><h1>ESP32 diagnostics</h1>"
    "<small>Read-only LAN page. Refresh: 5 seconds. History resets on reboot.</small>"
    "<p><a style='color:#8df' href='/recordings'>Play / download microphone recordings</a></p>"
    "<p id=connection>Connecting...</p><pre id=logs>Waiting for device status...</pre>"
    "<script>async function poll(){const c=document.getElementById('connection');"
    "const controller=new AbortController();const timeout=setTimeout(()=>controller.abort(),4000);"
    "try{const r=await fetch('/logs',{cache:'no-store',signal:controller.signal});"
    "if(!r.ok)throw Error(r.status);document.getElementById('logs').textContent=await r.text();"
    "c.textContent='Connected - refreshed '+new Date().toLocaleTimeString();}"
    "catch(e){c.textContent='Disconnected / request failed. Displayed data may be stale.';}"
    "finally{clearTimeout(timeout);setTimeout(poll,5000);}}poll();</script></html>";

void device_diagnostics_publish(const char *text) {
    portENTER_CRITICAL(&store_lock);
    diagnostic_store_snapshot(&store, text);
    portEXIT_CRITICAL(&store_lock);
}

void device_diagnostics_event(esp_log_level_t level, const char *format, ...) {
    char line[DIAGNOSTIC_EVENT_BYTES];
    const int prefix = snprintf(line, sizeof(line), "[%llu ms] %c ",
        (unsigned long long)(esp_timer_get_time() / 1000),
        level == ESP_LOG_ERROR ? 'E' : level == ESP_LOG_WARN ? 'W' : 'I');
    if (prefix < 0 || prefix >= sizeof(line)) return;
    va_list args;
    va_start(args, format);
    vsnprintf(line + prefix, sizeof(line) - prefix, format, args);
    va_end(args);
    portENTER_CRITICAL(&store_lock);
    diagnostic_store_append(&store, line);
    portEXIT_CRITICAL(&store_lock);
    ESP_LOG_LEVEL_LOCAL(level, TAG, "%s", line);
}

static void response_headers(httpd_req_t *request, const char *type) {
    httpd_resp_set_type(request, type);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
}

static esp_err_t root_handler(httpd_req_t *request) {
    response_headers(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, page, sizeof(page) - 1);
}

static esp_err_t logs_handler(httpd_req_t *request) {
    char snapshot[DIAGNOSTIC_SNAPSHOT_BYTES];
    uint64_t first, end;
    portENTER_CRITICAL(&store_lock);
    memcpy(snapshot, store.snapshot, sizeof(snapshot));
    first = diagnostic_store_first(&store);
    end = store.total;
    portEXIT_CRITICAL(&store_lock);
    response_headers(request, "text/plain; charset=utf-8");
    if (httpd_resp_sendstr_chunk(request, snapshot[0] ? snapshot :
            "Waiting for first five-second summary...\n") != ESP_OK) return ESP_FAIL;
    if (httpd_resp_sendstr_chunk(request, "\nRecent application events (oldest first; max 12):\n") != ESP_OK)
        return ESP_FAIL;
    for (uint64_t index = first; index < end; ++index) {
        char line[DIAGNOSTIC_EVENT_BYTES];
        portENTER_CRITICAL(&store_lock);
        const bool valid = diagnostic_store_read(&store, index, line);
        portEXIT_CRITICAL(&store_lock);
        if (!valid) continue; /* An event was overwritten while a slow client read. */
        if (httpd_resp_sendstr_chunk(request, line) != ESP_OK ||
            httpd_resp_sendstr_chunk(request, "\n") != ESP_OK) return ESP_FAIL;
    }
    return httpd_resp_send_chunk(request, NULL, 0);
}

esp_err_t device_diagnostics_start(const char *ip_address) {
    if (server == NULL) {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.task_priority = 1;
        config.stack_size = 4096;
        config.max_open_sockets = 2;
        config.max_uri_handlers = 4;
        config.uri_match_fn = httpd_uri_match_wildcard;
        config.backlog_conn = 2;
        config.lru_purge_enable = true;
        config.recv_wait_timeout = 1;
        config.send_wait_timeout = 1;
        esp_err_t error = httpd_start(&server, &config);
        if (error != ESP_OK) {
            server = NULL;
            device_diagnostics_event(ESP_LOG_ERROR, "Local log server failed: %s", esp_err_to_name(error));
            return error;
        }
        const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_handler};
        const httpd_uri_t logs = {.uri = "/logs", .method = HTTP_GET, .handler = logs_handler};
        error = httpd_register_uri_handler(server, &root);
        if (error == ESP_OK) error = httpd_register_uri_handler(server, &logs);
        if (error == ESP_OK) error = recordings_http_register(server);
        if (error != ESP_OK) {
            httpd_stop(server);
            server = NULL;
            device_diagnostics_event(ESP_LOG_ERROR, "Local log route registration failed: %s", esp_err_to_name(error));
            return error;
        }
    }
    device_diagnostics_event(ESP_LOG_INFO, "Local logs: http://%s/ (read-only, trusted LAN only)", ip_address);
    return ESP_OK;
}
