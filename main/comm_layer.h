#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "door_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

bool is_matter_connected(void);
bool is_ble_connected(void);
void comm_set_matter_connected(bool connected);
void comm_set_ble_connected(bool connected);
void comm_set_endpoint_id(uint16_t endpoint_id);
void report_result(op_result_t result, uint8_t attempts);
void report_lock_state(bool locked);

#ifdef __cplusplus
}
#endif
