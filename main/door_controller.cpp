#include "door_controller.h"
#include "hal_gpio.h"
#include "comm_layer.h"
#include "status_led.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <inttypes.h>

static const char *TAG = "door_ctrl";

/* ── State ── */
static door_engine_state_t g_door_engine_state = DOOR_STATE_IDLE;
static esp_timer_handle_t  s_exit_timer = NULL;
static esp_timer_handle_t  s_poll_timer = NULL;
static esp_timer_handle_t  s_power_timer = NULL;

/* ── Command Queue ── */
typedef struct {
    bool target_unlock;
} door_cmd_t;
static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t  s_worker_task = NULL;

/* ── Mutex: 도어 동작 직렬화 (동시 명령 방지) ── */
static SemaphoreHandle_t s_door_mutex = NULL;

/* ── Software Lock State (볼트 위치 소프트웨어 추적) ── */
static bool     g_lock_state = true;           // true=잠금, false=해제 (부팅 시 잠금)
static esp_timer_handle_t s_autolock_timer = NULL;  // 문 닫힘 후 자동 잠금 타이머

/* ── Door Sensor State (문 열림/닫힘) ── */
static bool     g_sensor_reliable = true;
static uint32_t g_last_sensor_change_ms = 0;
static uint32_t g_ops_since_last_change = 0;

/* ── Power monitoring ── */
static uint8_t  s_power_fault_consecutive = 0;
static bool     s_power_ok = true;

/* ── Forward declarations ── */
static void exit_timer_cb(void *arg);
static void door_status_poll_cb(void *arg);
static void autolock_timer_cb(void *arg);
static void power_monitor_cb(void *arg);
static void check_sensor_health(void);

/* ═══════════════════════════════════════════════════════════
 *  도어 제어 엔진: 개폐 + 검증 + 재시도 (최대 3회)
 * ═══════════════════════════════════════════════════════════ */

op_result_t door_execute_with_retry(bool target_unlock)
{
    /* ── Mutex: 동시 명령 직렬화 ── */
    if (xSemaphoreTakeRecursive(s_door_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "도어 동작 진행 중 → 명령 거부");
        report_result(OP_RESULT_FAIL_BUSY, 0);
        return OP_RESULT_FAIL_BUSY;
    }

    if (!s_power_ok) {
        ESP_LOGE(TAG, "전원 이상으로 도어 동작 중단");
        report_result(OP_RESULT_FAIL_POWER, 0);
        xSemaphoreGiveRecursive(s_door_mutex);
        return OP_RESULT_FAIL_POWER;
    }

    g_door_engine_state = DOOR_STATE_OPERATING;

    // 1. LED 피드백 시작 (명령 수신 즉시)
    if (target_unlock) {
        status_led_notify_unlocking();
    } else {
        status_led_notify_locking();
    }

    // 2. 릴레이 동작
    deadbolt_set(target_unlock);

    /*
     * GD-3000 동작 특성:
     *   센서(NO/COM) = 자석(문 열림/닫힘) 감지, 볼트 위치 감지 불가
     *   잠금: 12V 복원 → GD-3000이 자석 감지 + 타이머 후 자동 잠금
     *   해제: 12V 차단 → 볼트 즉시 후퇴
     *
     * 볼트 잠금/해제는 소프트웨어로 추적 (g_lock_state)
     * 잠금 명령: 12V 복원, 문 닫힘 + 3초 후 g_lock_state = true (자동 잠금 타이머)
     * 해제 명령: 12V 차단, 즉시 g_lock_state = false
     */

    if (target_unlock) {
        // 해제: 즉시 상태 변경
        g_lock_state = false;
        ESP_LOGI(TAG, "해제: 12V 차단 완료 → LockState=Unlocked");
    } else {
        // 잠금: 12V 복원. 문 닫힘 상태이면 자동 잠금 타이머 시작
        if (is_door_closed()) {
            esp_timer_stop(s_autolock_timer);
            esp_timer_start_once(s_autolock_timer,
                (uint64_t)DEFAULT_EXIT_DURATION_SEC * 1000000ULL);  // 잠금 시간 설정과 동일
            ESP_LOGI(TAG, "잠금: 12V 복원, 문 닫힘 → %d초 후 자동 잠금", DEFAULT_EXIT_DURATION_SEC);
        } else {
            ESP_LOGI(TAG, "잠금: 12V 복원, 문 열림 → 문 닫힐 때 자동 잠금 대기");
        }
    }

    // 3. 결과 보고
    g_door_engine_state = DOOR_STATE_REPORT;
    report_result(OP_RESULT_SUCCESS, 1);
    status_led_set_locked(g_lock_state);
    g_door_engine_state = DOOR_STATE_IDLE;
    xSemaphoreGiveRecursive(s_door_mutex);
    return OP_RESULT_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════
 *  퇴실 기능 (시간 제한 열림)
 * ═══════════════════════════════════════════════════════════ */

static void exit_lock_task(void *arg)
{
    ESP_LOGI(TAG, "퇴실 시간 만료 → 자동 잠금");
    op_result_t result = door_execute_with_retry(false);
    if (result != OP_RESULT_SUCCESS) {
        ESP_LOGE(TAG, "자동 잠금 실패! → 보안 경고");
    }
    vTaskDelete(NULL);
}

static void exit_timer_cb(void *arg)
{
    /* 별도 태스크로 실행하여 esp_timer 태스크 블로킹 방지 */
    xTaskCreate(exit_lock_task, "exit_lock", 4096, NULL, 5, NULL);
}

/**
 * @brief EXIT 릴레이로 해제 시도 (mutex 내부에서 호출)
 * @return true=성공, false=실패
 */
static bool try_exit_relay_unlock(void)
{
    /* EXIT 펄스 1회 → GD-3000 퇴실 회로 작동
     * 센서는 자석(문 열림/닫힘)만 감지하므로 볼트 검증 불가 → 즉시 성공 */
    deadbolt_exit(true);
    vTaskDelay(pdMS_TO_TICKS(VERIFY_DELAY_MS));
    deadbolt_exit(false);
    ESP_LOGI(TAG, "EXIT 릴레이 펄스 완료");
    return true;
}

esp_err_t door_exit_open(uint8_t duration_sec)
{
    if (duration_sec < EXIT_MIN_SEC || duration_sec > EXIT_MAX_SEC) {
        duration_sec = DEFAULT_EXIT_DURATION_SEC;
    }

    /* ── Mutex: 동시 명령 직렬화 ── */
    if (xSemaphoreTakeRecursive(s_door_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "도어 동작 진행 중 → 퇴실 명령 거부");
        report_result(OP_RESULT_FAIL_BUSY, 0);
        return ESP_FAIL;
    }

    // 1. EXIT 릴레이(GPIO 6)로 해제 시도
    bool opened = try_exit_relay_unlock();

    // 2. EXIT 실패 → 전원 제어(GPIO 4) 폴백 (recursive mutex로 재진입 허용)
    if (!opened) {
        ESP_LOGW(TAG, "EXIT 릴레이 실패 → 전원 제어 폴백");
        op_result_t result = door_execute_with_retry(true);
        if (result != OP_RESULT_SUCCESS) {
            ESP_LOGE(TAG, "퇴실 해제 실패 (EXIT + 전원 모두) → 타이머 미시작");
            xSemaphoreGiveRecursive(s_door_mutex);
            return ESP_FAIL;
        }
    } else {
        report_result(OP_RESULT_SUCCESS, 1);
    }

    xSemaphoreGiveRecursive(s_door_mutex);

    // 3. 자동 잠금 타이머 시작 (전원 제어로 잠금)
    esp_timer_stop(s_exit_timer);  // 기존 타이머 취소 (안전)
    esp_timer_start_once(s_exit_timer, (uint64_t)duration_sec * 1000000ULL);

    ESP_LOGI(TAG, "퇴실 열림: %d초 후 자동 잠금", duration_sec);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════
 *  문상태 폴링 (200ms 주기)
 * ═══════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════
 *  자동 잠금 타이머 콜백 (문 닫힘 후 3초)
 * ═══════════════════════════════════════════════════════════ */

static void autolock_timer_cb(void *arg)
{
    if (is_door_closed() && !g_lock_state) {
        g_lock_state = true;
        report_lock_state(true);
        status_led_set_locked(true);
        ESP_LOGI(TAG, "자동 잠금 완료 (문 닫힘 + 타이머 만료)");
    }
}

/* ═══════════════════════════════════════════════════════════
 *  문 열림/닫힘 폴링 (200ms 주기, 자석 센서)
 * ═══════════════════════════════════════════════════════════ */

static void door_status_poll_cb(void *arg)
{
    static bool last_closed = true;
    static bool initialized = false;
    static uint8_t debounce_count = 0;

    bool current_closed = is_door_closed();

    // 최초 실행 시 실제 센서 값으로 초기화
    if (!initialized) {
        last_closed = current_closed;
        initialized = true;
        ESP_LOGI(TAG, "센서 초기값: 문 %s", current_closed ? "닫힘" : "열림");
        return;
    }

    // 디바운스: 연속 DEBOUNCE_COUNT회 동일 값이면 상태 확정
    if (current_closed != last_closed) {
        debounce_count++;
        if (debounce_count >= DEBOUNCE_COUNT) {
            bool was_closed = last_closed;
            last_closed = current_closed;
            debounce_count = 0;

            // 센서 변화 기록
            g_last_sensor_change_ms = (uint32_t)(esp_timer_get_time() / 1000);
            g_ops_since_last_change = 0;

            if (current_closed) {
                // 문 열림 → 닫힘 전환
                ESP_LOGI(TAG, "문 닫힘 감지");

                // 12V가 인가 중(잠금 명령 상태)이면 자동 잠금 타이머 시작
                // deadbolt_set(false) = GPIO LOW = 릴레이 OFF = 12V 인가
                if (!g_lock_state) {
                    // 해제 상태에서 문 닫힘 → 잠금 대기 없음 (12V 차단 중이므로 잠금 안 됨)
                    ESP_LOGI(TAG, "문 닫힘, 해제 상태 유지");
                } else {
                    // 이미 잠금 상태면 변화 없음
                }
            } else {
                // 문 닫힘 → 열림 전환
                ESP_LOGI(TAG, "문 열림 감지");

                // 자동 잠금 타이머 취소
                esp_timer_stop(s_autolock_timer);

                // 잠금 상태에서 문이 열렸다면 → 비정상 개방 감지
                if (g_lock_state && g_door_engine_state == DOOR_STATE_IDLE) {
                    ESP_LOGE(TAG, "비정상 개방 감지! (잠금 상태에서 문 열림)");
                    uint8_t err_data[2] = { 0xFF, DOOR_ERR_FORCED };
                    report_error(err_data);
                    status_led_notify_error(DOOR_ERR_FORCED);
                }
            }

            // Matter DoorState 보고 (DoorOpen=0, DoorClosed=1)
            // report_lock_state는 DoorState가 아닌 LockState를 보고하므로 별도 처리 필요
            // 현재는 로그만 출력
        }
    } else {
        debounce_count = 0;
    }
}

/* 센서 고착 체크 제거: 센서는 자석(문 열림/닫힘) 감지이므로
 * 문이 계속 닫혀있는 것은 정상 상태이며 "고착"이 아님 */

/* ═══════════════════════════════════════════════════════════
 *  전원 모니터링 (5초 주기)
 * ═══════════════════════════════════════════════════════════ */

static void power_monitor_cb(void *arg)
{
    uint32_t mv = read_power_voltage_mv();
    if (mv == 0) return;  // ADC 미사용

    if (mv < POWER_MIN_MV || mv > POWER_MAX_MV) {
        s_power_fault_consecutive++;
        ESP_LOGW(TAG, "전원 이상: %" PRIu32 "mV (연속 %d/%d)", mv,
                 s_power_fault_consecutive, POWER_FAULT_COUNT);

        if (s_power_fault_consecutive >= POWER_FAULT_COUNT) {
            s_power_ok = false;
            ESP_LOGE(TAG, "전원 이상 확정! 도어 동작 일시 중단");
            uint8_t err_data[2] = { 0xFF, DOOR_ERR_POWER };
            report_error(err_data);
        }
    } else {
        if (!s_power_ok) {
            ESP_LOGI(TAG, "전원 복구: %" PRIu32 "mV", mv);
        }
        s_power_fault_consecutive = 0;
        s_power_ok = true;
    }
}

/* ═══════════════════════════════════════════════════════════
 *  초기화
 * ═══════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════
 *  명령 큐 워커 (순차 실행, 최신 명령 우선)
 * ═══════════════════════════════════════════════════════════ */

static void door_worker_task(void *arg)
{
    door_cmd_t cmd;
    while (true) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "큐에서 명령 실행: %s", cmd.target_unlock ? "해제" : "잠금");
            door_execute_with_retry(cmd.target_unlock);
        }
    }
}

void door_queue_command(bool target_unlock)
{
    door_cmd_t cmd = { .target_unlock = target_unlock };
    if (s_cmd_queue) {
        xQueueOverwrite(s_cmd_queue, &cmd);
        ESP_LOGI(TAG, "명령 큐 등록: %s", target_unlock ? "해제" : "잠금");
    }
}

esp_err_t door_controller_init(void)
{
    g_last_sensor_change_ms = (uint32_t)(esp_timer_get_time() / 1000);

    /* ── Recursive Mutex 생성 (door_exit_open 폴백에서 재진입 허용) ── */
    s_door_mutex = xSemaphoreCreateRecursiveMutex();
    if (s_door_mutex == NULL) {
        ESP_LOGE(TAG, "도어 mutex 생성 실패!");
        return ESP_FAIL;
    }

    // 퇴실 자동잠금 타이머 (1회성, 아직 시작 안 함)
    esp_timer_create_args_t exit_args = {
        .callback = exit_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "exit_lock",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&exit_args, &s_exit_timer), TAG,
                        "퇴실 타이머 생성 실패");

    // 자동 잠금 타이머 (문 닫힘 감지 후 GD-3000 잠금 시간과 동기화)
    esp_timer_create_args_t autolock_args = {
        .callback = autolock_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "autolock",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&autolock_args, &s_autolock_timer), TAG,
                        "자동 잠금 타이머 생성 실패");

    // 문상태 폴링 타이머 (200ms 주기)
    esp_timer_create_args_t poll_args = {
        .callback = door_status_poll_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "door_poll",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&poll_args, &s_poll_timer), TAG,
                        "폴링 타이머 생성 실패");
    ESP_RETURN_ON_ERROR(
        esp_timer_start_periodic(s_poll_timer, POLL_INTERVAL_MS * 1000),
        TAG, "폴링 타이머 시작 실패");

    // 전원 모니터링 비활성화 (분압 회로 미연결)
    // TODO: 분압 회로(R1=100k, R2=10k) 연결 후 아래 주석 해제
    // esp_timer_create_args_t power_args = {
    //     .callback = power_monitor_cb,
    //     .arg = NULL,
    //     .dispatch_method = ESP_TIMER_TASK,
    //     .name = "pwr_mon",
    // };
    // ESP_RETURN_ON_ERROR(esp_timer_create(&power_args, &s_power_timer), TAG,
    //                     "전원 모니터링 타이머 생성 실패");
    // ESP_RETURN_ON_ERROR(
    //     esp_timer_start_periodic(s_power_timer, POWER_CHECK_INTERVAL_MS * 1000),
    //     TAG, "전원 모니터링 타이머 시작 실패");

    // 명령 큐 (크기 1, 최신 명령 덮어쓰기)
    s_cmd_queue = xQueueCreate(1, sizeof(door_cmd_t));
    if (s_cmd_queue == NULL) {
        ESP_LOGE(TAG, "명령 큐 생성 실패!");
        return ESP_FAIL;
    }

    // 워커 태스크 (큐에서 명령을 꺼내 순차 실행)
    if (xTaskCreate(door_worker_task, "door_worker", 4096, NULL, 5, &s_worker_task) != pdPASS) {
        ESP_LOGE(TAG, "워커 태스크 생성 실패!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "도어 컨트롤러 초기화 완료");
    ESP_LOGI(TAG, "  검증 폴링: %dms 간격, 최대 %dms",
             VERIFY_DELAY_MS, VERIFY_TIMEOUT_MS);
    ESP_LOGI(TAG, "  폴링: %dms, 퇴실 기본: %d초",
             POLL_INTERVAL_MS, DEFAULT_EXIT_DURATION_SEC);

    return ESP_OK;
}

door_engine_state_t door_get_state(void)
{
    return g_door_engine_state;
}

bool door_is_sensor_reliable(void)
{
    return g_sensor_reliable;
}

bool door_is_locked(void)
{
    return g_lock_state;
}
