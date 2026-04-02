#include "hal_gpio.h"
#include "door_controller.h"
#include "ble_server.h"
#include "comm_layer.h"
#include "status_led.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
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
    if (cluster_id != DoorLock::Id) return ESP_OK;

    if (attribute_id == DoorLock::Attributes::LockState::Id) {
        /*
         * 릴레이 동작은 여기서 하지 않음.
         * Matter Lock/Unlock 명령은 emberAfPluginDoorLockOnDoor{Lock,Unlock}Command 콜백이 처리.
         *
         * 이 콜백에서 릴레이를 조작하면 피드백 루프 발생:
         *   report_result() → attribute::update() → 이 콜백 → deadbolt_set()
         *   → 센서 불일치 시 실패 → report(NotFullyLocked) → 이 콜백 → 무한 반복
         *
         * LockState attribute는 상태 반영용이며, 제어 입력이 아님.
         */
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
    status_led_set_commissioning((!has_fabric) || window_open);
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

    ESP_LOGI(TAG, "Door Lock endpoint 생성: ID=%d", endpoint_id);
}

/* ═══════════════════════════════════════════════════════════
 *  app_main
 * ═══════════════════════════════════════════════════════════ */

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-S3 Matter Door Lock 시작 ===");

    // 0. 최우선: 릴레이 GPIO 즉시 LOW 고정 (NC 배선 Fail-Safe)
    //    gpio_init()보다 먼저 실행 — ADC/센서/LED 초기화 지연 방지
    //    부트로더→app_main 사이 GPIO 부동(4V↔0V) → BC547 떨림 → 릴레이 채터링 방지
    gpio_reset_pin((gpio_num_t)GPIO_RELAY_LOCK);
    gpio_set_direction((gpio_num_t)GPIO_RELAY_LOCK, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)GPIO_RELAY_LOCK, 0);  // LOW = 릴레이 OFF = NC 단락 = 12V 인가 = 잠금 유지

    // 0-1. 나머지 GPIO 전체 초기화 (ADC, 센서, LED, EXIT 릴레이, 안전 타이머)
    gpio_init();

    // 1. NVS 초기화
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 파티션 초기화 필요 → erase & re-init");
        nvs_flash_erase();
        nvs_flash_init();
    }

    // 2. BLE 암호화 키 로드 (NVS만 사용, NimBLE 불필요)
    ble_crypto_init();

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

    // 7. BLE GATT 서버 등록 (Matter가 NimBLE 초기화한 후)
    ble_gatt_server_init();

    // 8. Status LED 초기화 + 시스템 준비 완료
    status_led_init();
    status_led_set_locked(door_is_locked());
    status_led_set_commissioning(true);  // 초기: 커미셔닝 대기 (파랑 빠른 깜빡임)
    status_led_set_system_ready(true);

    ESP_LOGI(TAG, "=== Matter Door Lock 시작 완료 ===");
    ESP_LOGI(TAG, "  Primary:  WiFi over Matter (Door Lock Cluster)");
    ESP_LOGI(TAG, "  Fallback: BLE GATT Server (AES-128-GCM)");
    ESP_LOGI(TAG, "  릴레이:   #1=GPIO%d(전원), #2=GPIO%d(EXIT)", GPIO_RELAY_LOCK, GPIO_RELAY_EXIT);
    ESP_LOGI(TAG, "  검증:     문상태 센서 + 최대 %d회 재시도", MAX_RETRY_COUNT);
    ESP_LOGI(TAG, "  폴링:     %dms 주기, 디바운스 %d회", POLL_INTERVAL_MS, DEBOUNCE_COUNT);
}
