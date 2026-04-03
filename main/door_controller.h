#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Configuration ── */
#define DEFAULT_EXIT_DURATION_SEC 5
#define EXIT_MIN_SEC             3
#define EXIT_MAX_SEC             30

/* ── Operation Result ── */
typedef enum {
    OP_RESULT_SUCCESS = 0,
    OP_RESULT_FAIL_BUSY,
} op_result_t;

/* ── Public API ── */
esp_err_t door_controller_init(void);
op_result_t door_execute(bool target_unlock);
void door_queue_command(bool target_unlock);
esp_err_t door_exit_open(uint8_t duration_sec);
bool door_is_locked(void);

#ifdef __cplusplus
}
#endif
