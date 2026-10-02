#include "backend_client.h"

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

#include "esp_event.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "nvs_flash.h"

#include "network_config.h"
#include "device_diagnostics.h"

#define SAMPLE_RATE 16000
#define PCM_FRAME_SAMPLES 320
#define PCM_FRAME_BYTES (PCM_FRAME_SAMPLES * sizeof(int16_t))
#define AUDIO_HEADER_BYTES 16
#define AUDIO_FRAME_BYTES (AUDIO_HEADER_BYTES + PCM_FRAME_BYTES)

#define AUDIO_MAGIC 0xA5
#define AUDIO_VERSION 1
#define AUDIO_CODEC_PCM_S16LE 0
#define AUDIO_FLAG_FIRST 0x01
#define AUDIO_FLAG_LAST 0x02
#define AUDIO_FLAG_PREBUF 0x04
#define SEND_TIMEOUT_TICKS pdMS_TO_TICKS(200)

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_MAX_RETRIES 10

static const char *TAG = "backend_client";

static EventGroupHandle_t wifi_event_group;
static esp_websocket_client_handle_t websocket_client;
static atomic_bool websocket_connected;
static atomic_bool hello_acknowledged;
static atomic_bool stream_active;
static atomic_uint connection_generation;
static unsigned stream_generation;
static uint32_t active_stream_id;
static uint16_t frame_sequence;
static uint32_t sample_offset;
static size_t frame_sample_count;
static bool frame_is_prebuffer;
/* Upload staging is task-only, not DMA storage. */
static EXT_RAM_BSS_ATTR int16_t frame_samples[PCM_FRAME_SAMPLES];
static EXT_RAM_BSS_ATTR uint8_t audio_frame[AUDIO_FRAME_BYTES];

typedef struct { uint32_t audio_id, generation; bool success; } playback_ack_t;
static StaticQueue_t playback_ack_control;
static uint8_t playback_ack_storage[4 * sizeof(playback_ack_t)];
static _Atomic(QueueHandle_t) playback_acks;

static bool contains_text(const char *data, size_t length, const char *needle) {
    const size_t needle_length = strlen(needle);
    if (needle_length == 0 || needle_length > length) {
        return false;
    }

    for (size_t i = 0; i <= length - needle_length; ++i) {
        if (memcmp(data + i, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data) {
    (void)arg;

    static int retry_count;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected =
            (const wifi_event_sta_disconnected_t *)event_data;
        websocket_connected = false;
        hello_acknowledged = false;
        if (retry_count < WIFI_MAX_RETRIES) {
            esp_wifi_connect();
            retry_count++;
            device_diagnostics_event(ESP_LOG_WARN,
                     "Wi-Fi disconnected; reason=%u; retry %d/%d",
                     disconnected != NULL ? disconnected->reason : 0,
                     retry_count,
                     WIFI_MAX_RETRIES);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAILED_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got_ip = (const ip_event_got_ip_t *)event_data;
        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        if (got_ip != NULL) {
            ESP_LOGI(TAG, "Wi-Fi connected; IP: " IPSTR, IP2STR(&got_ip->ip_info.ip));
            char address[16];
            snprintf(address, sizeof(address), IPSTR, IP2STR(&got_ip->ip_info.ip));
            /* Starts independently of backend availability. Re-announces DHCP changes. */
            device_diagnostics_start(address);
        } else {
            ESP_LOGI(TAG, "Wi-Fi connected");
        }
    }
}

static esp_err_t init_wifi(void) {
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        error = nvs_flash_erase();
        if (error != ESP_OK) {
            return error;
        }
        error = nvs_flash_init();
    }
    if (error != ESP_OK) {
        return error;
    }

    error = esp_netif_init();
    if (error != ESP_OK) {
        return error;
    }

    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }

    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_FAIL;
    }

    const wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&wifi_init_config);
    if (error != ESP_OK) {
        return error;
    }

    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    error = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (error != ESP_OK) {
        return error;
    }
    error = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (error != ESP_OK) {
        return error;
    }

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password));

    error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error != ESP_OK) {
        return error;
    }
    error = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (error != ESP_OK) {
        return error;
    }
    error = esp_wifi_start();
    if (error != ESP_OK) {
        return error;
    }

    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID: %s", WIFI_SSID);
    const EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(30000));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static void websocket_event_handler(void *handler_args,
                                    esp_event_base_t event_base,
                                    int32_t event_id,
                                    void *event_data) {
    (void)handler_args;
    (void)event_base;

    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED: {
            playback_buffer_reset();
            atomic_fetch_add(&connection_generation, 1);
            websocket_connected = true;
            hello_acknowledged = false;
            char hello[256];
            const int length = snprintf(
                hello,
                sizeof(hello),
                "{\"type\":\"hello\",\"proto\":1,\"device_id\":\"%s\","
                "\"codecs\":[\"pcm_s16le\"],\"sample_rate\":%d}",
                DEVICE_ID,
                SAMPLE_RATE);
            if (length > 0 && length < (int)sizeof(hello)) {
                esp_websocket_client_send_text(websocket_client, hello, length, SEND_TIMEOUT_TICKS);
                device_diagnostics_event(ESP_LOG_INFO, "WebSocket connected; hello sent");
            }
            break;
        }
        case WEBSOCKET_EVENT_DATA:
            if (data != NULL && data->op_code == 0x1 && data->data_ptr != NULL &&
                contains_text(data->data_ptr, data->data_len, "hello_ack")) {
                hello_acknowledged = true;
                device_diagnostics_event(ESP_LOG_INFO, "Backend hello acknowledged; audio upload ready");
            } else if (data != NULL && data->op_code == 0x1 && data->data_ptr != NULL &&
                       contains_text(data->data_ptr, data->data_len, "play_start")) {
                playback_buffer_begin();
                device_diagnostics_event(ESP_LOG_INFO, "Backend requested speaker playback");
            } else if (data != NULL && data->op_code == 0x1 && data->data_ptr != NULL &&
                       contains_text(data->data_ptr, data->data_len, "play_stop")) {
                playback_buffer_end();
                device_diagnostics_event(ESP_LOG_INFO, "Backend audio received; speaker may still be draining");
            } else if (data != NULL && data->op_code == 0x2 &&
                       data->data_len >= 0 && data->payload_len >= 0 && data->payload_offset >= 0) {
                playback_buffer_receive(data->data_ptr, (size_t)data->data_len,
                    (size_t)data->payload_len, (size_t)data->payload_offset);
            }
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_ERROR:
            playback_buffer_reset();
            atomic_fetch_add(&connection_generation, 1);
            websocket_connected = false;
            hello_acknowledged = false;
            stream_active = false;
            device_diagnostics_event(ESP_LOG_WARN, "WebSocket disconnected or failed");
            break;
        default:
            break;
    }
}

static bool send_text(const char *text) {
    if (!websocket_connected || websocket_client == NULL) {
        return false;
    }

    const int length = (int)strlen(text);
    return esp_websocket_client_send_text(websocket_client, text, length, SEND_TIMEOUT_TICKS) == length;
}

static void put_le16(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t)(value & 0xFF);
    destination[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)(value & 0xFF);
    destination[1] = (uint8_t)((value >> 8) & 0xFF);
    destination[2] = (uint8_t)((value >> 16) & 0xFF);
    destination[3] = (uint8_t)(value >> 24);
}

static bool send_audio_frame(uint8_t flags) {
    if (!backend_client_is_ready() || stream_generation != atomic_load(&connection_generation))
        return false;
    audio_frame[0] = AUDIO_MAGIC;
    audio_frame[1] = AUDIO_VERSION;
    audio_frame[2] = AUDIO_CODEC_PCM_S16LE;
    audio_frame[3] = flags;
    put_le16(&audio_frame[4], frame_sequence);
    put_le16(&audio_frame[6], PCM_FRAME_BYTES);
    put_le32(&audio_frame[8], active_stream_id);
    put_le32(&audio_frame[12], sample_offset);
    memcpy(&audio_frame[AUDIO_HEADER_BYTES], frame_samples, PCM_FRAME_BYTES);

    const int sent = esp_websocket_client_send_bin(
        websocket_client,
        (const char *)audio_frame,
        sizeof(audio_frame),
        SEND_TIMEOUT_TICKS);
    if (sent != sizeof(audio_frame)) {
        device_diagnostics_event(ESP_LOG_ERROR, "Audio frame send failed: %d", sent);
        return false;
    }

    frame_sequence++;
    sample_offset += PCM_FRAME_SAMPLES;
    return true;
}

esp_err_t backend_client_init(void) {
    if (!playback_buffer_init()) return ESP_ERR_NO_MEM;
    playback_acks = xQueueCreateStatic(4, sizeof(playback_ack_t),
        playback_ack_storage, &playback_ack_control);
    if (playback_acks == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = init_wifi();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed: %s", esp_err_to_name(error));
        return error;
    }

    const esp_websocket_client_config_t websocket_config = {
        .uri = BACKEND_URI,
        .buffer_size = 2048,
        .network_timeout_ms = 5000,
        .reconnect_timeout_ms = 5000,
    };
    websocket_client = esp_websocket_client_init(&websocket_config);
    if (websocket_client == NULL) {
        return ESP_FAIL;
    }

    error = esp_websocket_register_events(
        websocket_client,
        WEBSOCKET_EVENT_ANY,
        websocket_event_handler,
        NULL);
    if (error != ESP_OK) {
        return error;
    }

    error = esp_websocket_client_start(websocket_client);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket start failed: %s", esp_err_to_name(error));
        return error;
    }

    ESP_LOGI(TAG, "Backend client started: %s", BACKEND_URI);
    return ESP_OK;
}

bool backend_client_is_ready(void) {
    return websocket_connected && hello_acknowledged;
}

bool backend_client_start_stream(uint32_t stream_id,
                                 uint32_t prebuffer_ms,
                                 uint32_t live_sample_offset,
                                 int64_t detection_time_ms) {
    if (!backend_client_is_ready() || stream_active) {
        return false;
    }
    stream_generation = atomic_load(&connection_generation);

    char start_message[256];
    const int length = snprintf(
        start_message,
        sizeof(start_message),
        "{\"type\":\"start\",\"stream_id\":%lu,\"codec\":\"pcm_s16le\","
        "\"sample_rate\":%d,\"channels\":1,\"frame_ms\":20,"
        "\"prebuffer_ms\":%lu,\"live_sample_offset\":%lu,\"t_detect_ms\":%lld}",
        (unsigned long)stream_id,
        SAMPLE_RATE,
        (unsigned long)prebuffer_ms,
        (unsigned long)live_sample_offset,
        (long long)detection_time_ms);
    if (length <= 0 || length >= (int)sizeof(start_message) ||
        !send_text(start_message) || stream_generation != atomic_load(&connection_generation)) {
        ESP_LOGE(TAG, "Could not send backend start message");
        return false;
    }

    active_stream_id = stream_id;
    frame_sequence = 0;
    sample_offset = 0;
    frame_sample_count = 0;
    frame_is_prebuffer = false;
    stream_active = true;
    return true;
}

bool backend_client_send_samples(const int16_t *samples, size_t sample_count,
                                 bool prebuffer) {
    if (!stream_active || samples == NULL) {
        return false;
    }

    while (sample_count > 0) {
        const size_t room = PCM_FRAME_SAMPLES - frame_sample_count;
        const size_t copy_count = sample_count < room ? sample_count : room;
        memcpy(&frame_samples[frame_sample_count], samples, copy_count * sizeof(int16_t));
        frame_sample_count += copy_count;
        frame_is_prebuffer = frame_is_prebuffer || prebuffer;
        samples += copy_count;
        sample_count -= copy_count;

        if (frame_sample_count == PCM_FRAME_SAMPLES) {
            uint8_t flags = frame_sequence == 0 ? AUDIO_FLAG_FIRST : 0;
            if (frame_is_prebuffer) {
                flags |= AUDIO_FLAG_PREBUF;
            }
            if (!send_audio_frame(flags)) {
                stream_active = false;
                return false;
            }
            frame_sample_count = 0;
            frame_is_prebuffer = false;
        }
    }

    return true;
}

bool backend_client_stop_stream(const char *reason) {
    if (!stream_active) {
        return false;
    }
    if (!backend_client_is_ready() || stream_generation != atomic_load(&connection_generation)) {
        stream_active = false;
        frame_sample_count = 0;
        return false;
    }

    bool success = true;
    if (frame_sample_count > 0) {
        memset(&frame_samples[frame_sample_count], 0,
               (PCM_FRAME_SAMPLES - frame_sample_count) * sizeof(int16_t));
        uint8_t flags = (frame_sequence == 0 ? AUDIO_FLAG_FIRST : 0) | AUDIO_FLAG_LAST;
        if (frame_is_prebuffer) {
            flags |= AUDIO_FLAG_PREBUF;
        }
        success = send_audio_frame(flags);
    }

    char stop_message[160];
    const int length = snprintf(
        stop_message,
        sizeof(stop_message),
        "{\"type\":\"stop\",\"stream_id\":%lu,\"reason\":\"%s\"}",
        (unsigned long)active_stream_id,
        reason != NULL ? reason : "button");
    if (length <= 0 || length >= (int)sizeof(stop_message) || !send_text(stop_message)) {
        success = false;
    }

    stream_active = false;
    frame_sample_count = 0;
    frame_is_prebuffer = false;
    return success;
}

bool backend_client_send_metrics(const char *json) {
    return json != NULL && send_text(json);
}

void backend_client_queue_playback_stop(uint32_t audio_id, uint32_t generation, bool success) {
    playback_ack_t ack = {.audio_id=audio_id, .generation=generation, .success=success};
    QueueHandle_t q = atomic_load(&playback_acks);
    if (q && xQueueSend(q, &ack, 0) != pdTRUE)
        device_diagnostics_event(ESP_LOG_WARN, "Playback completion queue full");
}

void backend_client_service_playback(void) {
    QueueHandle_t q = atomic_load(&playback_acks);
    playback_ack_t ack;
    if (!q || xQueueReceive(q, &ack, 0) != pdTRUE || !playback_buffer_current(ack.generation)) return;
    char message[128];
    const int length = snprintf(
        message,
        sizeof(message),
        "{\"type\":\"play_stop\",\"audio_id\":%lu,\"reason\":\"%s\"}",
        (unsigned long)ack.audio_id, ack.success ? "complete" : "playback_error");
    if (length <= 0 || length >= (int)sizeof(message) || !send_text(message))
        device_diagnostics_event(ESP_LOG_WARN, "Playback completion acknowledgement failed");
    else
        device_diagnostics_event(ack.success ? ESP_LOG_INFO : ESP_LOG_WARN,
            "Speaker audio %lu drained: %s", (unsigned long)ack.audio_id,
            ack.success ? "complete" : "playback_error (see PLAY counters)");
}
