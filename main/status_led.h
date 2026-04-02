#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LED 상태 표시기 초기화 (FreeRTOS 태스크 생성)
 */
esp_err_t status_led_init(void);

/**
 * @brief 시스템 준비 상태 설정 (부팅 완료 시 true)
 */
void status_led_set_system_ready(bool ready);

/**
 * @brief Matter 커미셔닝 창 상태 설정
 */
void status_led_set_commissioning(bool open);

/**
 * @brief 도어 잠금 상태 설정 (잠금=빨강, 해제=초록)
 */
void status_led_set_locked(bool locked);

/**
 * @brief 잠금 진행 피드백 (주황 깜빡임 2초 → 주황 고정)
 */
void status_led_notify_locking(void);

/**
 * @brief 해제 진행 피드백 (초록 깜빡임 2초 → 초록 고정)
 */
void status_led_notify_unlocking(void);

/**
 * @brief 도어 동작 실패 피드백 (2s 빨강 깜빡임)
 */
void status_led_notify_op_fail(void);

/**
 * @brief 에러 표시 (센서 고착 = 노랑, 비정상 개방 = 빨강 빠른 깜빡임)
 */
void status_led_notify_error(uint8_t error_code);

/**
 * @brief 현재 LED 상태 문자열 반환 (디버그용)
 */
const char *status_led_get_state_str(void);

#ifdef __cplusplus
}
#endif
