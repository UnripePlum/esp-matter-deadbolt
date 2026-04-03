#include "comm_layer.h"
#include "hal_gpio.h"
#include "door_controller.h"
#include "ble_server.h"

#include "esp_log.h"

#include <esp_matter.h>
#include <esp_matter_attribute_utils.h>
#include <app/clusters/door-lock-server/door-lock-server.h>

using namespace esp_matter;
using namespace chip::app::Clusters;

static const char *TAG = "comm_layer";

static bool     s_matter_connected = false;
static bool     s_ble_connected = false;
static uint16_t s_endpoint_id = 0;

bool is_matter_connected(void)   { return s_matter_connected; }
bool is_ble_connected(void)      { return s_ble_connected; }

void comm_set_matter_connected(bool connected)
{
    s_matter_connected = connected;
    ESP_LOGI(TAG, "Matter %s", connected ? "CONNECTED" : "DISCONNECTED");
}

void comm_set_ble_connected(bool connected)
{
    s_ble_connected = connected;
    ESP_LOGI(TAG, "BLE %s", connected ? "CONNECTED" : "DISCONNECTED");
}

void comm_set_endpoint_id(uint16_t endpoint_id)
{
    s_endpoint_id = endpoint_id;
}

void report_result(op_result_t result, uint8_t attempts)
{
    if (s_matter_connected) {
        uint8_t lock_state = (result == OP_RESULT_SUCCESS)
            ? (door_is_locked() ? 1 : 2)
            : 0;

        esp_matter_attr_val_t val = esp_matter_enum8(lock_state);
        esp_err_t err = attribute::update(
            s_endpoint_id, DoorLock::Id,
            DoorLock::Attributes::LockState::Id, &val);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Matter 보고: LockState=%d", lock_state);
            return;
        }
        ESP_LOGW(TAG, "Matter 보고 실패 → BLE 폴백");
    }

    if (s_ble_connected) {
        uint8_t ble_resp[2] = {
            (uint8_t)((result == OP_RESULT_SUCCESS) ? 0x10 : 0x11),
            attempts
        };
        ble_notify_status_encrypted(ble_resp, sizeof(ble_resp));
        ESP_LOGI(TAG, "BLE 보고: [0x%02X, 0x%02X]", ble_resp[0], ble_resp[1]);
        return;
    }

    ESP_LOGW(TAG, "보고 실패: 연결 없음");
}

void report_lock_state(bool locked)
{
    if (s_matter_connected) {
        esp_matter_attr_val_t val = esp_matter_enum8(locked ? 1 : 2);
        if (attribute::update(s_endpoint_id, DoorLock::Id,
                DoorLock::Attributes::LockState::Id, &val) == ESP_OK)
            return;
    }
    if (s_ble_connected) {
        uint8_t resp[2] = { (uint8_t)(locked ? 0x01 : 0x02), 0x00 };
        ble_notify_status_encrypted(resp, sizeof(resp));
    }
}
