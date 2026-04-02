#include "status_led.h"
#include "door_controller.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <device.h>
#include <led_driver.h>

namespace {

/* ── Timing ── */
static constexpr uint32_t kSlowBlinkMs   = 500;    // 부팅
static constexpr uint32_t kFastBlinkMs    = 200;    // 커미셔닝
static constexpr uint32_t kFeedbackMs     = 150;    // 동작 성공/실패
static constexpr uint32_t kErrorFastMs    = 100;    // 비정상 개방
static constexpr uint32_t kLedTaskStackSize = 4096;

static constexpr uint64_t kOpSuccessDurationUs  = 900000;   // 성공 피드백 900ms
static constexpr uint64_t kOpFailDurationUs     = 2000000;  // 실패 피드백 2s
static constexpr uint64_t kErrorDurationUs      = 3000000;  // 에러 표시 3s

/* ── Colors (HSV) ── */
static constexpr uint16_t kHueRed    = 0;
static constexpr uint16_t kHueOrange = 30;
static constexpr uint16_t kHueYellow = 50;
static constexpr uint16_t kHueGreen  = 120;
static constexpr uint16_t kHueBlue   = 240;

static constexpr uint8_t kSatColor   = 100;
static constexpr uint8_t kBrightness = 10;

static const char *TAG = "status_led";

/* ── Visual State Priority (높은 순) ── */
enum class visual_state_t : uint8_t {
    BOOTING,             // 파랑 느린 깜빡임
    COMMISSIONING,       // 파랑 빠른 깜빡임
    READY_UNLOCKED,      // 초록 고정
    READY_LOCKED,        // 주황 고정
    OP_LOCKING,          // 주황 깜빡임 (2초)
    OP_UNLOCKING,        // 초록 깜빡임 (2초)
    OP_FAIL,             // 빨강 빠른 깜빡임 (일시)
    ERROR_SENSOR,        // 노랑 빠른 깜빡임 (일시)
    ERROR_FORCED,        // 빨강 매우 빠른 깜빡임 (일시)
};

enum class feedback_t : uint8_t {
    NONE,
    LOCKING,
    UNLOCKING,
    OP_FAIL,
    ERR_SENSOR,
    ERR_FORCED,
};

struct led_frame_t {
    bool on;
    uint16_t hue;
    uint8_t saturation;
    uint8_t brightness;
};

/* ── State ── */
static bool s_initialized = false;
static bool s_system_ready = false;
static bool s_commissioning_open = false;
static bool s_door_locked = false;
static feedback_t s_feedback = feedback_t::NONE;
static uint64_t s_feedback_until_us = 0;
static visual_state_t s_last_visual_state = visual_state_t::BOOTING;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_led_task = nullptr;
static led_driver_handle_t s_led_handle = nullptr;

/* ── Blink Phase ── */
static bool blink_phase_on(uint32_t period_ms, uint64_t now_us)
{
    if (period_ms == 0) return true;
    return (((now_us / 1000ULL) / period_ms) % 2ULL) == 0ULL;
}

/* ── Apply Frame to Hardware ── */
static void apply_frame(const led_frame_t &frame)
{
    if (!s_led_handle) return;

    if (!frame.on || frame.brightness == 0) {
        (void)led_driver_set_power(s_led_handle, false);
    } else {
        (void)led_driver_set_power(s_led_handle, true);
        (void)led_driver_set_hue(s_led_handle, frame.hue);
        (void)led_driver_set_saturation(s_led_handle, frame.saturation);
        (void)led_driver_set_brightness(s_led_handle, frame.brightness);
    }
}

/* ── Frame for Each State ── */
static led_frame_t frame_for_state(visual_state_t state, uint64_t now_us)
{
    switch (state) {
    case visual_state_t::BOOTING:
        return { blink_phase_on(kSlowBlinkMs, now_us), kHueBlue, kSatColor, kBrightness };
    case visual_state_t::COMMISSIONING:
        return { blink_phase_on(kFastBlinkMs, now_us), kHueBlue, kSatColor, kBrightness };
    case visual_state_t::READY_UNLOCKED:
        return { true, kHueGreen, kSatColor, kBrightness };
    case visual_state_t::READY_LOCKED:
        return { true, kHueOrange, kSatColor, kBrightness };
    case visual_state_t::OP_LOCKING:
        return { blink_phase_on(kFeedbackMs, now_us), kHueOrange, kSatColor, kBrightness };
    case visual_state_t::OP_UNLOCKING:
        return { blink_phase_on(kFeedbackMs, now_us), kHueGreen, kSatColor, kBrightness };
    case visual_state_t::OP_FAIL:
        return { blink_phase_on(kFeedbackMs, now_us), kHueRed, kSatColor, kBrightness };
    case visual_state_t::ERROR_SENSOR:
        return { blink_phase_on(kFastBlinkMs, now_us), kHueYellow, kSatColor, kBrightness };
    case visual_state_t::ERROR_FORCED:
        return { blink_phase_on(kErrorFastMs, now_us), kHueRed, kSatColor, kBrightness };
    default:
        return { false, 0, 0, 0 };
    }
}

/* ── State Resolver (우선순위 기반) ── */
static visual_state_t resolve_visual_state(uint64_t now_us)
{
    bool system_ready;
    bool commissioning_open;
    bool door_locked;
    feedback_t feedback;
    uint64_t feedback_until;

    taskENTER_CRITICAL(&s_lock);
    system_ready = s_system_ready;
    commissioning_open = s_commissioning_open;
    door_locked = s_door_locked;
    feedback = s_feedback;
    feedback_until = s_feedback_until_us;

    /* 피드백 타이머 만료 체크 */
    if (feedback != feedback_t::NONE && feedback_until != 0 && now_us >= feedback_until) {
        s_feedback = feedback_t::NONE;
        s_feedback_until_us = 0;
        feedback = feedback_t::NONE;
    }
    taskEXIT_CRITICAL(&s_lock);

    /* 우선순위 1: 도어 동작/에러 피드백 */
    switch (feedback) {
    case feedback_t::LOCKING:      return visual_state_t::OP_LOCKING;
    case feedback_t::UNLOCKING:    return visual_state_t::OP_UNLOCKING;
    case feedback_t::OP_FAIL:      return visual_state_t::OP_FAIL;
    case feedback_t::ERR_SENSOR:   return visual_state_t::ERROR_SENSOR;
    case feedback_t::ERR_FORCED:   return visual_state_t::ERROR_FORCED;
    case feedback_t::NONE:         break;
    }

    /* 우선순위 2: 시스템 상태 */
    if (!system_ready) {
        return visual_state_t::BOOTING;
    }
    if (commissioning_open) {
        return visual_state_t::COMMISSIONING;
    }

    /* 우선순위 3: 잠금 상태 표시 */
    return door_locked ? visual_state_t::READY_LOCKED : visual_state_t::READY_UNLOCKED;
}

/* ── LED Task (50ms 주기) ── */
static void led_task(void *arg)
{
    (void)arg;
    while (true) {
        const uint64_t now_us = esp_timer_get_time();
        const visual_state_t state = resolve_visual_state(now_us);
        const led_frame_t frame = frame_for_state(state, now_us);
        apply_frame(frame);

        taskENTER_CRITICAL(&s_lock);
        s_last_visual_state = state;
        taskEXIT_CRITICAL(&s_lock);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

} // namespace

/* ═══════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════ */

esp_err_t status_led_init(void)
{
    if (s_initialized) return ESP_OK;

    esp_log_level_set("led_driver_ws2812", ESP_LOG_WARN);

    led_driver_config_t config = led_driver_get_config();
    s_led_handle = led_driver_init(&config);
    if (!s_led_handle) {
        ESP_LOGE(TAG, "LED 드라이버 초기화 실패");
        return ESP_ERR_INVALID_STATE;
    }
    (void)led_driver_set_power(s_led_handle, false);

    if (xTaskCreate(led_task, "status_led", kLedTaskStackSize, nullptr, 5, &s_led_task) != pdPASS) {
        s_led_handle = nullptr;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Status LED 초기화 완료 (gpio=%d channel=%d)", config.gpio, config.channel);
    return ESP_OK;
}

void status_led_set_system_ready(bool ready)
{
    taskENTER_CRITICAL(&s_lock);
    s_system_ready = ready;
    taskEXIT_CRITICAL(&s_lock);
}

void status_led_set_commissioning(bool open)
{
    taskENTER_CRITICAL(&s_lock);
    s_commissioning_open = open;
    taskEXIT_CRITICAL(&s_lock);
}

void status_led_set_locked(bool locked)
{
    taskENTER_CRITICAL(&s_lock);
    s_door_locked = locked;
    taskEXIT_CRITICAL(&s_lock);
}

void status_led_notify_locking(void)
{
    const uint64_t now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    s_feedback = feedback_t::LOCKING;
    s_feedback_until_us = now_us + kOpFailDurationUs;  // 2초 깜빡임
    taskEXIT_CRITICAL(&s_lock);
}

void status_led_notify_unlocking(void)
{
    const uint64_t now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    s_feedback = feedback_t::UNLOCKING;
    s_feedback_until_us = now_us + kOpFailDurationUs;  // 2초 깜빡임
    taskEXIT_CRITICAL(&s_lock);
}

void status_led_notify_op_fail(void)
{
    const uint64_t now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    s_feedback = feedback_t::OP_FAIL;
    s_feedback_until_us = now_us + kOpFailDurationUs;
    taskEXIT_CRITICAL(&s_lock);
}

void status_led_notify_error(uint8_t error_code)
{
    const uint64_t now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    switch (error_code) {
    case DOOR_ERR_SENSOR:
        s_feedback = feedback_t::ERR_SENSOR;
        break;
    case DOOR_ERR_FORCED:
        s_feedback = feedback_t::ERR_FORCED;
        break;
    default:
        s_feedback = feedback_t::OP_FAIL;
        break;
    }
    s_feedback_until_us = now_us + kErrorDurationUs;
    taskEXIT_CRITICAL(&s_lock);
}

const char *status_led_get_state_str(void)
{
    visual_state_t state;
    taskENTER_CRITICAL(&s_lock);
    state = s_last_visual_state;
    taskEXIT_CRITICAL(&s_lock);

    switch (state) {
    case visual_state_t::BOOTING:          return "booting";
    case visual_state_t::COMMISSIONING:    return "commissioning";
    case visual_state_t::READY_UNLOCKED:   return "ready_unlocked";
    case visual_state_t::READY_LOCKED:     return "ready_locked";
    case visual_state_t::OP_LOCKING:       return "locking";
    case visual_state_t::OP_UNLOCKING:     return "unlocking";
    case visual_state_t::OP_FAIL:          return "op_fail";
    case visual_state_t::ERROR_SENSOR:     return "error_sensor";
    case visual_state_t::ERROR_FORCED:     return "error_forced";
    default:                               return "unknown";
    }
}
