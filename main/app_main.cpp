#include "hal_gpio.h"
#include "door_controller.h"
#include "comm_layer.h"
#include "status_led.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <esp_matter.h>
#include <esp_matter_attribute_utils.h>
#include <app/clusters/door-lock-server/door-lock-server.h>
#include <app/server/Server.h>

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::DoorLock;

static const char *TAG = "app_main";

/* door_control_task는 더 이상 사용하지 않음 — door_queue_command() 사용 */

/* ── 커스텀 클러스터 ID ── */
static const uint32_t CUSTOM_CLUSTER_CONTROL       = 0x131BFC00;
static const uint32_t CUSTOM_ATTR_FACTORY_RESET    = 0x00000000;  // uint16, write 0xDEAD → 팩토리 리셋
static const uint32_t CUSTOM_ATTR_EXIT_OPEN        = 0x00000001;  // uint8, write duration(3~30) → 퇴실
static const uint32_t CUSTOM_ATTR_OTA_TRIGGER      = 0x00000002;  // uint8, write 1 → HTTPS OTA 시작
static const uint16_t FACTORY_RESET_MAGIC          = 0xDEAD;

// GitHub Releases latest 고정 URL — 버전 업 시 URL 변경 불필요
#define OTA_FIRMWARE_URL \
    "https://github.com/UnripePlum/esp-matter-deadbolt/releases/latest/download/esp-matter-deadbolt.bin"

static void ota_task(void *arg)
{
    ESP_LOGI(TAG, "=== HTTPS OTA 시작: %s ===", OTA_FIRMWARE_URL);
    status_led_set_ota(true);  // 보라 느린 점멸 — 다운로드 완료 or 재부팅까지 유지

    esp_http_client_config_t http_cfg = {
        .url = OTA_FIRMWARE_URL,
        .timeout_ms = 60000,
        .max_redirection_count = 10,  // GitHub 리다이렉트 대응
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA 완료 → 재부팅");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA 실패: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

/* ═══════════════════════════════════════════════════════════
 *  CHIP Door Lock Cluster Required Callbacks
 * ═══════════════════════════════════════════════════════════ */

void emberAfDoorLockClusterInitCallback(chip::EndpointId endpoint)
{
    ESP_LOGI(TAG, "Door Lock cluster init: endpoint=%d", endpoint);
}

bool emberAfPluginDoorLockOnDoorLockCommand(chip::EndpointId endpointId,
    const chip::app::DataModel::Nullable<chip::FabricIndex> & fabricIdx,
    const chip::app::DataModel::Nullable<chip::NodeId> & nodeId,
    const chip::Optional<chip::ByteSpan> & pinCode,
    OperationErrorEnum & err)
{
    ESP_LOGI(TAG, "Matter LockDoor command: endpoint=%d", endpointId);
    door_queue_command(false);
    return true;
}

bool emberAfPluginDoorLockOnDoorUnlockCommand(chip::EndpointId endpointId,
    const chip::app::DataModel::Nullable<chip::FabricIndex> & fabricIdx,
    const chip::app::DataModel::Nullable<chip::NodeId> & nodeId,
    const chip::Optional<chip::ByteSpan> & pinCode,
    OperationErrorEnum & err)
{
    ESP_LOGI(TAG, "Matter UnlockDoor command: endpoint=%d", endpointId);
    door_queue_command(true);
    return true;
}

bool emberAfPluginDoorLockGetCredential(chip::EndpointId endpointId, uint16_t credentialIndex,
    CredentialTypeEnum credentialType, EmberAfPluginDoorLockCredentialInfo & credential)
{
    return false;  // No credentials supported
}

bool emberAfPluginDoorLockSetCredential(chip::EndpointId endpointId, uint16_t credentialIndex,
    chip::FabricIndex creator, chip::FabricIndex modifier,
    DlCredentialStatus credentialStatus, CredentialTypeEnum credentialType,
    const chip::ByteSpan & credentialData)
{
    return false;
}

bool emberAfPluginDoorLockGetUser(chip::EndpointId endpointId, uint16_t userIndex,
    EmberAfPluginDoorLockUserInfo & user)
{
    return false;
}

bool emberAfPluginDoorLockSetUser(chip::EndpointId endpointId, uint16_t userIndex,
    chip::FabricIndex creator, chip::FabricIndex modifier,
    const chip::CharSpan & userName, uint32_t uniqueId,
    UserStatusEnum userStatus, UserTypeEnum usertype,
    CredentialRuleEnum credentialRule,
    const CredentialStruct * credentials, size_t totalCredentials)
{
    return false;
}

DlStatus emberAfPluginDoorLockGetSchedule(chip::EndpointId endpointId, uint8_t weekdayIndex,
    uint16_t userIndex, EmberAfPluginDoorLockWeekDaySchedule & schedule)
{
    return DlStatus::kFailure;
}

DlStatus emberAfPluginDoorLockGetSchedule(chip::EndpointId endpointId, uint8_t yearDayIndex,
    uint16_t userIndex, EmberAfPluginDoorLockYearDaySchedule & schedule)
{
    return DlStatus::kFailure;
}

DlStatus emberAfPluginDoorLockGetSchedule(chip::EndpointId endpointId, uint8_t holidayIndex,
    EmberAfPluginDoorLockHolidaySchedule & schedule)
{
    return DlStatus::kFailure;
}

DlStatus emberAfPluginDoorLockSetSchedule(chip::EndpointId endpointId, uint8_t weekdayIndex,
    uint16_t userIndex, DlScheduleStatus status, DaysMaskMap daysMask,
    uint8_t startHour, uint8_t startMinute, uint8_t endHour, uint8_t endMinute)
{
    return DlStatus::kFailure;
}

DlStatus emberAfPluginDoorLockSetSchedule(chip::EndpointId endpointId, uint8_t yearDayIndex,
    uint16_t userIndex, DlScheduleStatus status,
    uint32_t localStartTime, uint32_t localEndTime)
{
    return DlStatus::kFailure;
}

DlStatus emberAfPluginDoorLockSetSchedule(chip::EndpointId endpointId, uint8_t holidayIndex,
    DlScheduleStatus status, uint32_t localStartTime,
    uint32_t localEndTime, OperatingModeEnum operatingMode)
{
    return DlStatus::kFailure;
}

bool emberAfPluginDoorLockOnDoorUnlockWithTimeoutCommand(chip::EndpointId endpointId,
    const chip::app::DataModel::Nullable<chip::FabricIndex> & fabricIdx,
    const chip::app::DataModel::Nullable<chip::NodeId> & nodeId,
    const chip::Optional<chip::ByteSpan> & pinCode,
    uint16_t timeout,
    OperationErrorEnum & err)
{
    ESP_LOGI(TAG, "Matter UnlockWithTimeout command: endpoint=%d, timeout=%ds", endpointId, timeout);
    uint8_t duration = (timeout > 255) ? 255 : (uint8_t)timeout;
    door_exit_open(duration);
    return true;
}

void emberAfPluginDoorLockOnAutoRelock(chip::EndpointId endpointId)
{
    ESP_LOGI(TAG, "Door Lock auto-relock: endpoint=%d", endpointId);
}

/* door_control_task 제거 — door_queue_command()가 워커 태스크를 통해 비동기 실행 */

/* ═══════════════════════════════════════════════════════════
 *  Matter Identification Callback
 * ═══════════════════════════════════════════════════════════ */

static esp_err_t app_identification_cb(identification::callback_type_t type,
                                       uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback: type=%u, effect=%u, variant=%u",
             type, effect_id, effect_variant);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════
 *  Matter Attribute Update Callback
 * ═══════════════════════════════════════════════════════════ */

static esp_err_t app_attribute_update_cb(
    callback_type_t type,
    uint16_t endpoint_id, uint32_t cluster_id,
    uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != PRE_UPDATE) return ESP_OK;

    /* 커스텀 클러스터 처리 */
    if (cluster_id == CUSTOM_CLUSTER_CONTROL) {
        if (attribute_id == CUSTOM_ATTR_FACTORY_RESET && val->val.u16 == FACTORY_RESET_MAGIC) {
            ESP_LOGE(TAG, "=== 원격 팩토리 리셋 요청 (0xDEAD) ===");
            chip::Server::GetInstance().ScheduleFactoryReset();
        } else if (attribute_id == CUSTOM_ATTR_EXIT_OPEN && val->val.u8 > 0) {
            ESP_LOGI(TAG, "퇴실 요청: %d초", val->val.u8);
            door_exit_open(val->val.u8);
        } else if (attribute_id == CUSTOM_ATTR_OTA_TRIGGER && val->val.u8 == 1) {
            ESP_LOGI(TAG, "OTA 트리거 수신");
            xTaskCreate(ota_task, "ota_task", 8192, nullptr, 5, nullptr);
        }
        return ESP_OK;
    }

    if (cluster_id != DoorLock::Id) return ESP_OK;

    if (attribute_id == DoorLock::Attributes::LockState::Id) {
        /* 릴레이 동작은 emberAf 콜백에서만 처리. 여기서 조작하면 피드백 루프 발생. */
        ESP_LOGI(TAG, "Matter LockState 속성 변경: %d (%s)",
                 val->val.u8,
                 val->val.u8 == 1 ? "Locked" :
                 val->val.u8 == 2 ? "Unlocked" : "NotFullyLocked");
    }

    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════
 *  Matter Event Callback (WiFi/Fabric 연결 상태 추적)
 * ═══════════════════════════════════════════════════════════ */

static void refresh_commissioning_led(void)
{
    auto & server = chip::Server::GetInstance();
    bool has_fabric = (server.GetFabricTable().FabricCount() > 0);
    bool window_open = server.GetCommissioningWindowManager().IsCommissioningWindowOpen();
    status_led_set_commissioning(!has_fabric);
    ESP_LOGI(TAG, "LED 커미셔닝 상태: fabric=%d window=%d", has_fabric, window_open);
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
        case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
            ESP_LOGI(TAG, "Matter 커미셔닝 완료");
            comm_set_matter_connected(true);
            refresh_commissioning_led();
            break;

        case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
            if (event->InterfaceIpAddressChanged.Type ==
                chip::DeviceLayer::InterfaceIpChangeType::kIpV4_Assigned) {
                ESP_LOGI(TAG, "WiFi IP 할당됨 → Matter 연결");
                comm_set_matter_connected(true);
                // WiFi 연결 후 Fabric이 있으면 60초간 커미셔닝 윈도우 오픈 (추가 기기 등록)
                if (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0) {
                    ESP_LOGI(TAG, "기존 Fabric 감지 → 커미셔닝 윈도우 180초 오픈 (추가 기기 등록 가능)");
                    CHIP_ERROR err = chip::Server::GetInstance().GetCommissioningWindowManager()
                        .OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(180));
                    if (err != CHIP_NO_ERROR) {
                        ESP_LOGE(TAG, "커미셔닝 윈도우 오픈 실패: %" CHIP_ERROR_FORMAT, err.Format());
                    }
                }
            }
            break;

        case chip::DeviceLayer::DeviceEventType::kWiFiConnectivityChange:
            if (event->WiFiConnectivityChange.Result ==
                chip::DeviceLayer::ConnectivityChange::kConnectivity_Lost) {
                ESP_LOGW(TAG, "WiFi 연결 끊김 → Matter 비활성");
                comm_set_matter_connected(false);
            }
            break;

        case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
            refresh_commissioning_led();
            break;

        case chip::DeviceLayer::DeviceEventType::kServerReady:
            refresh_commissioning_led();
            break;

        default:
            break;
    }
}

/* ═══════════════════════════════════════════════════════════
 *  Matter Door Lock Endpoint 설정
 * ═══════════════════════════════════════════════════════════ */

static void add_custom_control_cluster(endpoint_t *ep)
{
    cluster_t *cluster = cluster::create(ep, CUSTOM_CLUSTER_CONTROL,
                                          CLUSTER_FLAG_SERVER);
    if (!cluster) {
        ESP_LOGE(TAG, "커스텀 클러스터 생성 실패");
        return;
    }

    /* Factory Reset: uint16, write 0xDEAD → 팩토리 리셋 */
    esp_matter_attr_val_t reset_val = esp_matter_uint16(0);
    attribute::create(cluster, CUSTOM_ATTR_FACTORY_RESET,
                      ATTRIBUTE_FLAG_WRITABLE, reset_val);

    /* Exit Open: uint8, write duration(3~30) → 퇴실 열림 후 자동 잠금 */
    esp_matter_attr_val_t exit_val = esp_matter_uint8(0);
    attribute::create(cluster, CUSTOM_ATTR_EXIT_OPEN,
                      ATTRIBUTE_FLAG_WRITABLE, exit_val);

    /* OTA Trigger: uint8, write 1 → HTTPS OTA 시작 */
    esp_matter_attr_val_t trigger_val = esp_matter_uint8(0);
    attribute::create(cluster, CUSTOM_ATTR_OTA_TRIGGER,
                      ATTRIBUTE_FLAG_WRITABLE, trigger_val);

    ESP_LOGI(TAG, "커스텀 클러스터 등록 (0x%08lX): factory_reset(0), exit_open(1), ota_trigger(2)",
             (unsigned long)CUSTOM_CLUSTER_CONTROL);
}

static void setup_matter_door_lock(node_t *node)
{
    endpoint::door_lock::config_t lock_config;
    lock_config.door_lock.lock_state = door_is_locked() ? 1 : 2;
    lock_config.door_lock.lock_type = 0;          // DeadBolt
    lock_config.door_lock.actuator_enabled = true;

    endpoint_t *ep = endpoint::door_lock::create(node, &lock_config,
                                                  ENDPOINT_FLAG_NONE, NULL);
    uint16_t endpoint_id = endpoint::get_id(ep);
    comm_set_endpoint_id(endpoint_id);


    /* 커스텀 제어 클러스터 추가 (factory_reset + exit_open) */
    add_custom_control_cluster(ep);

    ESP_LOGI(TAG, "Door Lock endpoint 생성: ID=%d", endpoint_id);
}

/* ═══════════════════════════════════════════════════════════
 *  app_main
 * ═══════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════
 *  BOOT 버튼 팩토리 리셋 (GPIO 0, 5초 길게 누르기)
 * ═══════════════════════════════════════════════════════════ */

#define BOOT_BUTTON_GPIO      0
#define FACTORY_RESET_HOLD_MS 5000
#define BOOT_POLL_MS          100

static void factory_reset_task(void *arg)
{
    // BOOT 버튼 (GPIO 0): LOW = 눌림, HIGH = 안 눌림
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    uint32_t hold_ms = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_MS));

        if (gpio_get_level((gpio_num_t)BOOT_BUTTON_GPIO) == 0) {
            // 버튼 눌림
            hold_ms += BOOT_POLL_MS;

            if (hold_ms == 2000) {
                ESP_LOGW(TAG, "BOOT 버튼 2초... 계속 누르면 팩토리 리셋");
                status_led_notify_op_fail();  // 빨강 깜빡임 경고
            }

            if (hold_ms >= FACTORY_RESET_HOLD_MS) {
                ESP_LOGE(TAG, "=== 팩토리 리셋 실행! ===");

                // 1. Matter fabric 제거
                chip::Server::GetInstance().ScheduleFactoryReset();

                // ScheduleFactoryReset이 NVS 삭제 + 재부팅 처리
                // 여기까지 오면 재부팅 대기
                while (true) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
        } else {
            // 버튼 놓음
            if (hold_ms > 0 && hold_ms < FACTORY_RESET_HOLD_MS) {
                ESP_LOGI(TAG, "BOOT 버튼 놓음 (%"PRIu32"ms, 리셋 취소)", hold_ms);
            }
            hold_ms = 0;
        }
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-S3 Matter Door Lock 시작 ===");

    // 0. 최우선: 릴레이 GPIO 즉시 HIGH (Low Trigger → 릴레이 OFF → 잠금)
    gpio_reset_pin((gpio_num_t)GPIO_RELAY_PIN);
    gpio_set_direction((gpio_num_t)GPIO_RELAY_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)GPIO_RELAY_PIN, 0);  // LOW = 잠금

    // 0-1. GPIO 전체 초기화
    gpio_init();

    // 1. NVS 초기화
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 파티션 초기화 필요 → erase & re-init");
        nvs_flash_erase();
        nvs_flash_init();
    }

    // 2. (BLE는 Matter 커미셔닝 전용 — CHIP SDK가 관리)

    // 3. Matter 노드 생성
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (node == NULL) {
        ESP_LOGE(TAG, "Matter 노드 생성 실패!");
        return;
    }

    // 4. Door Lock 엔드포인트 생성
    setup_matter_door_lock(node);

    // 5. 도어 컨트롤러 초기화 (폴링/타이머)
    door_controller_init();

    // 6. Matter 시작 (NimBLE 초기화 포함)
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Matter 시작 실패: %s", esp_err_to_name(err));
        return;
    }

    // 7. (BLE GATT 제거 — Matter 커미셔닝 전용)

    // 8. Status LED 초기화 + 시스템 준비 완료
    status_led_init();
    status_led_set_locked(door_is_locked());
    status_led_set_commissioning(true);  // 초기: 커미셔닝 대기 (파랑 빠른 깜빡임)
    status_led_set_system_ready(true);

    // 9. BOOT 버튼 팩토리 리셋 태스크
    xTaskCreate(factory_reset_task, "factory_rst", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "=== Matter Door Lock 시작 완료 ===");
    ESP_LOGI(TAG, "  Primary:  WiFi over Matter (Door Lock Cluster)");
    ESP_LOGI(TAG, "  Fallback: BLE GATT Server (AES-128-GCM)");
    ESP_LOGI(TAG, "  릴레이:   GPIO%d (CW-020, LOW=잠금 HIGH=해제)", GPIO_RELAY_PIN);
}
