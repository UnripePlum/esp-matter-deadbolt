#include "comm_layer.h"
#include "door_controller.h"

#include "esp_log.h"

#include <esp_matter.h>
#include <esp_matter_attribute_utils.h>
#include <app/clusters/door-lock-server/door-lock-server.h>

using namespace esp_matter;
using namespace chip::app::Clusters;

static const char *TAG = "comm_layer";

static bool     s_matter_connected = false;
static uint16_t s_endpoint_id = 0;

bool is_matter_connected(void)   { return s_matter_connected; }

void comm_set_matter_connected(bool connected)
{
    s_matter_connected = connected;
    ESP_LOGI(TAG, "Matter %s", connected ? "CONNECTED" : "DISCONNECTED");
}

void comm_set_endpoint_id(uint16_t endpoint_id)
{
    s_endpoint_id = endpoint_id;
}

void report_result(op_result_t result, uint8_t attempts)
{
    if (s_matter_connected) {
        uint8_t lock_val = (result == OP_RESULT_SUCCESS)
            ? (door_is_locked() ? 1 : 2)
            : 0;

        esp_matter_attr_val_t val = esp_matter_enum8(lock_val);
        attribute::update(s_endpoint_id, DoorLock::Id,
                         DoorLock::Attributes::LockState::Id, &val);
        ESP_LOGI(TAG, "Matter 보고: LockState=%d", lock_val);
    } else {
        ESP_LOGW(TAG, "보고 실패: Matter 미연결");
    }
}

void report_lock_state(bool locked)
{
    if (s_matter_connected) {
        esp_matter_attr_val_t val = esp_matter_enum8(locked ? 1 : 2);
        attribute::update(s_endpoint_id, DoorLock::Id,
                         DoorLock::Attributes::LockState::Id, &val);
    }
}
