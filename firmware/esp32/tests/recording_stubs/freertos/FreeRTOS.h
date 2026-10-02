#pragma once
#define portMUX_INITIALIZER_UNLOCKED 0
typedef int portMUX_TYPE;
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define pdPASS 1
