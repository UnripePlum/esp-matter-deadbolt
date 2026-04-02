#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── GPIO Pin Assignments ── */
#define GPIO_RELAY_LOCK      4    // 릴레이 출력 (잠금/해제)
#define GPIO_DOOR_SENSOR     5    // 문상태 센서 입력
#define GPIO_RELAY_EXIT      6    // 퇴실 릴레이 (EXIT 단락 → GD-3000 내부 퇴실 회로 작동)
#define GPIO_STATUS_LED      48   // 상태 LED

/* ── ADC for 12V Power Monitoring ── */
#define ADC_POWER_CHANNEL    ADC_CHANNEL_0  // 12V 분압 입력

/* ── Relay Safety ── */
#define RELAY_MAX_ON_MS      500   // 솔레노이드 최대 통전 시간 (과열 보호)

/* ── Initialization ── */
esp_err_t gpio_init(void);

/* ── Relay Control ── */
/**
 * @brief 데드볼트 릴레이 제어 (NC 배선, GD-3000: 12V 차단=해제)
 * @param unlock true=릴레이 ON(NC 개방 → 12V 차단 → 볼트 후퇴 → 해제)
 *               false=릴레이 OFF(NC 단락 → 12V 인가 → 잠금 유지)
 *
 * NC 배선 + GD-3000 (자석 감지 + 타이머 → 자동 잠금, 12V 차단 → 해제):
 *   GPIO LOW  → 릴레이 OFF → NC 단락 → 12V 인가 → 잠금 유지
 *   GPIO HIGH → 릴레이 ON  → NC 개방 → 12V 차단 → 해제
 *   정전 (12V 없음)         → 해제 (Fail-Safe)
 *   스위치 OFF (5V만 차단)  → 릴레이 OFF → 12V 유지 → 잠금 (Fail-Secure)
 */
void deadbolt_set(bool unlock);

/**
 * @brief 릴레이 안전 타이머 초기화 (gpio_init 내부에서 호출됨)
 */
void relay_safety_timer_init(void);

/**
 * @brief EXIT 릴레이 제어 (릴레이 #2, GD-3000 EXIT 단락)
 * @param activate true=릴레이 ON(EXIT 1-2 단락, 퇴실 회로 작동), false=릴레이 OFF(EXIT 개방)
 *
 * 전원 제어(릴레이 #1)와 독립적인 제2 제어 경로.
 * 하나가 고장 나도 다른 쪽으로 문을 열 수 있다.
 */
void deadbolt_exit(bool activate);

/* ── Sensor Reading ── */
/**
 * @brief 문 닫힘 센서 읽기 (자석 감지)
 * @return true=문 닫힘(자석 붙음, LOW), false=문 열림(자석 떨어짐, HIGH)
 *
 * 주의: 이 센서는 볼트 위치(잠금/해제)가 아니라 문 열림/닫힘을 감지합니다.
 * 잠금 상태는 door_controller의 소프트웨어 추적을 사용하세요.
 */
bool is_door_closed(void);

/* ── LED Control ── */
void status_led_set(bool on);

/* ── Power Monitoring ── */
/**
 * @brief 12V 전원 전압 읽기 (분압 후 ADC)
 * @return 추정 전압 (mV 단위), 예: 12000 = 12.0V
 */
uint32_t read_power_voltage_mv(void);

#ifdef __cplusplus
}
#endif
