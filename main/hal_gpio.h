#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── GPIO Pin Assignments ── */
#define GPIO_RELAY_PIN  4   // CW-020 릴레이 IN

/* ── Initialization ── */
esp_err_t gpio_init(void);

/* ── Relay Control ── */
/**
 * @brief 데드볼트 해제 (GPIO HIGH 지속)
 */
void deadbolt_unlock(void);

/**
 * @brief 데드볼트 잠금 (GPIO LOW)
 */
void deadbolt_lock(void);

#ifdef __cplusplus
}
#endif
