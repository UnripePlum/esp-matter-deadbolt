#include "comm_layer.h"
#include "hal_gpio.h"
#include "door_controller.h"
#include "ble_server.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <esp_matter.h>
#include <esp_matter_attribute_utils.h>
#include <app/clusters/door-lock-server/door-lock-server.h>

using namespace esp_matter;
using namespace chip::app::Clusters;

static const char *TAG = "comm_layer";

/* ── State ── */
static bool     s_matter_connected = false;
static bool     s_ble_connected = false;
static uint16_t s_endpoint_id = 0;

/* ── NVS namespace for pending results ── */
#define NVS_NAMESPACE   "door_pending"
#define NVS_KEY_COUNT   "count"
#define NVS_KEY_PREFIX  "pr_"   // pr_0, pr_1, ...
#define MAX_PENDING     8

/* ═══════════════════════════════════════════════════════════
 *  연결 상태
 * ═══════════════════════════════════════════════════════════ */

bool is_matter_connected(void)   { return s_matter_connected; }
bool is_ble_connected(void)      { return s_ble_connected; }

void comm_set_matter_connected(bool connected)
{
    bool was = s_matter_connected;
    s_matter_connected = connected;
    ESP_LOGI(TAG, "Matter %s", connected ? "CONNECTED" : "DISCONNECTED");

    // 재연결 시 미전송 결과 플러시
    if (!was && connected) {
        flush_pending_results();
    }
}

void comm_set_ble_connected(bool connected)
{
    bool was = s_ble_connected;
    s_ble_connected = connected;
    ESP_LOGI(TAG, "BLE %s", connected ? "CONNECTED" : "DISCONNECTED");

    if (!was && connected) {
        flush_pending_results();
    }
}

void comm_set_endpoint_id(uint16_t endpoint_id)
{
    s_endpoint_id = endpoint_id;
}

/* ═══════════════════════════════════════════════════════════
 *  결과 보고 (Matter 우선 → BLE 폴백 → NVS 저장)
 * ═══════════════════════════════════════════════════════════ */

void report_result(op_result_t result, uint8_t attempts)
{
    // 1. Matter로 보고 시도
    if (s_matter_connected) {
        uint8_t lock_state;
        if (result == OP_RESULT_SUCCESS) {
            lock_state = door_is_locked() ? 1 : 2;  // 1=Locked, 2=Unlocked
        } else {
            lock_state = 0;  // NotFullyLocked
        }

        esp_matter_attr_val_t lock_val = esp_matter_enum8(lock_state);
        esp_err_t err = attribute::update(
            s_endpoint_id, DoorLock::Id,
            DoorLock::Attributes::LockState::Id, &lock_val);

        if (err == ESP_OK) {
            // 실패 시 DoorState도 업데이트
            if (result == OP_RESULT_FAIL_MAX_RETRY) {
                esp_matter_attr_val_t door_val = esp_matter_enum8(2); // DoorJammed
                attribute::update(s_endpoint_id, DoorLock::Id,
                    DoorLock::Attributes::DoorState::Id, &door_val);
                ESP_LOGE(TAG, "Matter 보고: NotFullyLocked + DoorJammed (시도 %d)", attempts);
            } else {
                ESP_LOGI(TAG, "Matter 보고 성공: LockState=%d (시도 %d)", lock_state, attempts);
            }
            return;
        }

        ESP_LOGW(TAG, "Matter 보고 실패 → BLE 폴백");
    }

    // 2. BLE 폴백
    if (s_ble_connected) {
        uint8_t ble_resp[2];
        ble_resp[0] = (result == OP_RESULT_SUCCESS) ? 0x10 : 0x11;
        ble_resp[1] = attempts;
        ble_notify_status_encrypted(ble_resp, sizeof(ble_resp));
        ESP_LOGI(TAG, "BLE 보고: [0x%02X, 0x%02X]", ble_resp[0], ble_resp[1]);
        return;
    }

    // 3. 둘 다 불가 → NVS 저장
    ESP_LOGE(TAG, "결과 보고 실패: Matter=%d, BLE=%d → NVS 저장",
             s_matter_connected, s_ble_connected);

    pending_result_t pending = {
        .timestamp = esp_timer_get_time(),
        .result = result,
        .attempts = attempts,
        .target_unlock = false,  // 상태에서 추론
    };
    save_pending_result_to_nvs(&pending);
}

void report_lock_state(bool locked)
{
    if (s_matter_connected) {
        uint8_t lock_state = locked ? 1 : 2;
        esp_matter_attr_val_t val = esp_matter_enum8(lock_state);
        esp_err_t err = attribute::update(
            s_endpoint_id, DoorLock::Id,
            DoorLock::Attributes::LockState::Id, &val);

        if (err == ESP_OK) return;
    }

    if (s_ble_connected) {
        uint8_t ble_resp[2];
        ble_resp[0] = locked ? BLE_STATUS_LOCKED : BLE_STATUS_UNLOCKED;
        ble_resp[1] = 0x00;
        ble_notify_status_encrypted(ble_resp, sizeof(ble_resp));
    }
}

void report_error(const uint8_t *err_data)
{
    // Matter: DoorState 에러 매핑
    if (s_matter_connected) {
        uint8_t door_state = 0;
        switch (err_data[1]) {
            case DOOR_ERR_FORCED: door_state = 4; break; // ForcedOpen
            case DOOR_ERR_SENSOR: door_state = 5; break; // Undefined (에러)
            case DOOR_ERR_RELAY:  door_state = 2; break; // DoorJammed
            case DOOR_ERR_POWER:  door_state = 5; break; // Undefined
            default:         door_state = 5; break;
        }
        esp_matter_attr_val_t val = esp_matter_enum8(door_state);
        attribute::update(s_endpoint_id, DoorLock::Id,
            DoorLock::Attributes::DoorState::Id, &val);
    }

    // BLE 에러 전송
    if (s_ble_connected) {
        ble_notify_status_encrypted(err_data, 2);
    }

    ESP_LOGE(TAG, "에러 보고: [0x%02X, 0x%02X]", err_data[0], err_data[1]);
}

/* ═══════════════════════════════════════════════════════════
 *  flush 전용 보고 (NVS 재저장 없이 Matter/BLE로만 전송)
 * ═══════════════════════════════════════════════════════════ */

static bool try_report_without_nvs(op_result_t result, uint8_t attempts)
{
    if (s_matter_connected) {
        uint8_t lock_state;
        if (result == OP_RESULT_SUCCESS) {
            lock_state = door_is_locked() ? 1 : 2;
        } else {
            lock_state = 0;
        }

        esp_matter_attr_val_t lock_val = esp_matter_enum8(lock_state);
        esp_err_t err = attribute::update(
            s_endpoint_id, DoorLock::Id,
            DoorLock::Attributes::LockState::Id, &lock_val);

        if (err == ESP_OK) return true;
    }

    if (s_ble_connected) {
        uint8_t ble_resp[2];
        ble_resp[0] = (result == OP_RESULT_SUCCESS) ? 0x10 : 0x11;
        ble_resp[1] = attempts;
        ble_notify_status_encrypted(ble_resp, sizeof(ble_resp));
        return true;
    }

    return false;
}

/* ═══════════════════════════════════════════════════════════
 *  NVS 미전송 결과 관리
 * ═══════════════════════════════════════════════════════════ */

/*
 * NVS 미전송 큐: Ring buffer 패턴
 *
 * ┌─ write_idx ─┐
 * │ 0 1 2 3 4 5 6 7 │  (MAX_PENDING = 8)
 * └─────────────────┘
 * write_idx: 다음에 쓸 위치 (0~7 순환)
 * 가득 차면 가장 오래된 항목부터 덮어씀
 */
void save_pending_result_to_nvs(const pending_result_t *pending)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 열기 실패: %s", esp_err_to_name(err));
        return;
    }

    uint8_t write_idx = 0;
    uint8_t count = 0;
    nvs_get_u8(handle, "write_idx", &write_idx);
    nvs_get_u8(handle, NVS_KEY_COUNT, &count);

    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_KEY_PREFIX, write_idx);
    nvs_set_blob(handle, key, pending, sizeof(pending_result_t));

    write_idx = (write_idx + 1) % MAX_PENDING;
    if (count < MAX_PENDING) count++;

    nvs_set_u8(handle, "write_idx", write_idx);
    nvs_set_u8(handle, NVS_KEY_COUNT, count);
    nvs_commit(handle);
    nvs_close(handle);

    if (count >= MAX_PENDING) {
        ESP_LOGW(TAG, "NVS 미전송 큐 가득참 → 오래된 항목 덮어씀 (idx=%d)", write_idx);
    } else {
        ESP_LOGI(TAG, "NVS 저장 완료 (총 %d건 미전송)", count);
    }
}

void flush_pending_results(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return;

    uint8_t count = 0;
    uint8_t write_idx = 0;
    nvs_get_u8(handle, NVS_KEY_COUNT, &count);
    nvs_get_u8(handle, "write_idx", &write_idx);

    if (count == 0) {
        nvs_close(handle);
        return;
    }

    ESP_LOGI(TAG, "미전송 결과 %d건 전송 시도", count);

    /* Ring buffer 읽기: write_idx에서 count만큼 역산하여 가장 오래된 것부터 */
    uint8_t read_idx = (write_idx + MAX_PENDING - count) % MAX_PENDING;
    uint8_t sent = 0;

    for (uint8_t i = 0; i < count; i++) {
        uint8_t idx = (read_idx + i) % MAX_PENDING;
        char key[16];
        snprintf(key, sizeof(key), "%s%d", NVS_KEY_PREFIX, idx);

        pending_result_t pending;
        size_t len = sizeof(pending);
        err = nvs_get_blob(handle, key, &pending, &len);
        if (err != ESP_OK) continue;

        /* NVS 재저장 없이 Matter/BLE로만 전송 */
        if (try_report_without_nvs(pending.result, pending.attempts)) {
            nvs_erase_key(handle, key);
            sent++;
        } else {
            ESP_LOGW(TAG, "미전송 결과 전송 실패 (idx=%d) → NVS에 유지", idx);
        }
    }

    /* 성공적으로 전송된 만큼 count 감소 */
    uint8_t remaining = count - sent;
    nvs_set_u8(handle, NVS_KEY_COUNT, remaining);
    if (remaining == 0) {
        nvs_set_u8(handle, "write_idx", 0);  // 큐 비었으면 인덱스 리셋
    }
    nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "미전송 결과: %d건 전송, %d건 잔여", sent, remaining);
}
