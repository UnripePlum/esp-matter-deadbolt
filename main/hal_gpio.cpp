#include "hal_gpio.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "hal_gpio";

/* ── ADC handle for power monitoring ── */
static adc_oneshot_unit_handle_t s_adc_handle = NULL;

/* ── Relay #2 (EXIT) safety timer (펄스 과열 보호) ── */
static esp_timer_handle_t s_exit_relay_safety_timer = NULL;
/* 릴레이 #1(전원 제어)은 NC 배선으로 잠금 유지에 지속 활성 필요 → 안전 타이머 없음 */

static void exit_relay_safety_timer_cb(void *arg)
{
    gpio_set_level((gpio_num_t)GPIO_RELAY_EXIT, 0);
    ESP_LOGW(TAG, "릴레이 #2 안전 타이머: 최대 통전 시간 초과 → 강제 OFF");
}

/* ── 분압비: R1=100k, R2=10k → Vout = Vin * 10/110 ── */
#define DIVIDER_RATIO_NUM   110
#define DIVIDER_RATIO_DEN   10
#define ADC_ATTEN           ADC_ATTEN_DB_12    // 0~3.1V range
#define ADC_MAX_RAW         4095
#define ADC_REF_MV          3100

esp_err_t gpio_init(void)
{
    /* ── Relay output (GPIO 4) ── */
    gpio_config_t relay_cfg = {
        .pin_bit_mask = (1ULL << GPIO_RELAY_LOCK),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&relay_cfg);
    gpio_set_level((gpio_num_t)GPIO_RELAY_LOCK, 0);  // 초기: 잠금 유지 (릴레이 OFF → NC 단락 → 12V 인가)

    /* ── EXIT relay output (GPIO 6) ── */
    gpio_config_t exit_relay_cfg = {
        .pin_bit_mask = (1ULL << GPIO_RELAY_EXIT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&exit_relay_cfg);
    gpio_set_level((gpio_num_t)GPIO_RELAY_EXIT, 0);  // 초기: EXIT 개방

    /* ── Door sensor input (GPIO 5) with pull-up ── */
    gpio_config_t sensor_cfg = {
        .pin_bit_mask = (1ULL << GPIO_DOOR_SENSOR),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&sensor_cfg);

    /* ── Status LED (GPIO 48) ── */
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << GPIO_STATUS_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level((gpio_num_t)GPIO_STATUS_LED, 0);

    /* ── ADC for 12V power monitoring ── */
    adc_oneshot_unit_init_cfg_t adc_init = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t err = adc_oneshot_new_unit(&adc_init, &s_adc_handle);
    if (err == ESP_OK) {
        adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_12,
        };
        adc_oneshot_config_channel(s_adc_handle, ADC_POWER_CHANNEL, &chan_cfg);
    } else {
        ESP_LOGW(TAG, "ADC 초기화 실패: %s (전원 모니터링 비활성)", esp_err_to_name(err));
    }

    /* ── Relay safety timer (솔레노이드 과열 보호) ── */
    relay_safety_timer_init();

    ESP_LOGI(TAG, "GPIO 초기화 완료 (Relay1=%d, Relay2=%d, Sensor=%d, LED=%d)",
             GPIO_RELAY_LOCK, GPIO_RELAY_EXIT, GPIO_DOOR_SENSOR, GPIO_STATUS_LED);
    return ESP_OK;
}

void relay_safety_timer_init(void)
{
    /* 릴레이 #1(NC 배선): 잠금 유지에 지속 활성 필요 → 안전 타이머 없음 */

    /* 릴레이 #2(EXIT, NO 배선): 펄스 방식 → 안전 타이머 유지 */
    esp_timer_create_args_t args = {
        .callback = exit_relay_safety_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "relay2_safe",
    };
    esp_timer_create(&args, &s_exit_relay_safety_timer);
}

void deadbolt_set(bool unlock)
{
    /*
     * NC 배선 + GD-3000 (12V 인가=잠금 유지, 12V 차단=해제):
     *   unlock=true  → GPIO HIGH → 릴레이 ON  → NC 개방 → 12V 차단 → 해제
     *   unlock=false → GPIO LOW  → 릴레이 OFF → NC 단락 → 12V 인가 → 잠금 유지
     *
     * 잠금: 자석 감지 + 타이머 후 GD-3000이 자동 잠금 (12V 인가 상태)
     * 해제: 릴레이 ON으로 12V 차단 → 볼트 후퇴
     * Fail-Safe: 정전 → 12V 없음 → 해제
     * Fail-Secure: 스위치 OFF → 릴레이 OFF → 12V 유지 → 잠금
     */
    int level = unlock ? 1 : 0;
    gpio_set_level((gpio_num_t)GPIO_RELAY_LOCK, level);

    ESP_LOGD(TAG, "데드볼트 %s (GPIO %d = %d)", unlock ? "해제(12V차단)" : "잠금(12V인가)",
             GPIO_RELAY_LOCK, level);
}

void deadbolt_exit(bool activate)
{
    int level = activate ? 1 : 0;

    /* 릴레이 OFF 시 안전 타이머 취소 */
    if (!activate && s_exit_relay_safety_timer) {
        esp_timer_stop(s_exit_relay_safety_timer);
    }

    gpio_set_level((gpio_num_t)GPIO_RELAY_EXIT, level);

    /* 릴레이 ON 시 안전 타이머 시작 (과열 보호) */
    if (activate && s_exit_relay_safety_timer) {
        esp_timer_stop(s_exit_relay_safety_timer);
        esp_timer_start_once(s_exit_relay_safety_timer, (uint64_t)RELAY_MAX_ON_MS * 1000);
    }

    ESP_LOGD(TAG, "EXIT 릴레이 %s (GPIO %d = %d)", activate ? "ON(단락)" : "OFF(개방)",
             GPIO_RELAY_EXIT, level);
}

bool is_door_closed(void)
{
    int level = gpio_get_level((gpio_num_t)GPIO_DOOR_SENSOR);
    // LOW = 문 닫힘 (자석 붙음), HIGH = 문 열림 (자석 떨어짐)
    return (level == 0);
}

void status_led_set(bool on)
{
    gpio_set_level((gpio_num_t)GPIO_STATUS_LED, on ? 1 : 0);
}

uint32_t read_power_voltage_mv(void)
{
    if (s_adc_handle == NULL) {
        return 0;
    }

    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc_handle, ADC_POWER_CHANNEL, &raw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC 읽기 실패: %s", esp_err_to_name(err));
        return 0;
    }

    // ADC raw → mV → 실제 전압 (분압비 역산)
    uint32_t adc_mv = (uint32_t)raw * ADC_REF_MV / ADC_MAX_RAW;
    uint32_t actual_mv = adc_mv * DIVIDER_RATIO_NUM / DIVIDER_RATIO_DEN;
    return actual_mv;
}
