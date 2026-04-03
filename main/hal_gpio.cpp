#include "hal_gpio.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "hal_gpio";

esp_err_t gpio_init(void)
{
    gpio_config_t relay_cfg = {
        .pin_bit_mask = (1ULL << GPIO_RELAY_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&relay_cfg);
    gpio_set_level((gpio_num_t)GPIO_RELAY_PIN, 0);  // LOW = 잠금

    ESP_LOGI(TAG, "GPIO 초기화 완료 (Relay=%d)", GPIO_RELAY_PIN);
    return ESP_OK;
}

void deadbolt_unlock(void)
{
    gpio_set_level((gpio_num_t)GPIO_RELAY_PIN, 1);
    ESP_LOGI(TAG, "해제 (GPIO HIGH)");
}

void deadbolt_lock(void)
{
    gpio_set_level((gpio_num_t)GPIO_RELAY_PIN, 0);
    ESP_LOGI(TAG, "잠금 (GPIO LOW)");
}
