#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t status_led_init(void);
void status_led_set_system_ready(bool ready);
void status_led_set_commissioning(bool open);
void status_led_set_locked(bool locked);
void status_led_notify_locking(void);
void status_led_notify_unlocking(void);
void status_led_notify_op_fail(void);
void status_led_set_ota(bool active);
const char *status_led_get_state_str(void);

#ifdef __cplusplus
}
#endif
