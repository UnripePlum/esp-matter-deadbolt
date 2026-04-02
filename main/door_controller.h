#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Configuration ── */
#define MAX_RETRY_COUNT          3
#define VERIFY_DELAY_MS          200   // 센서 폴링 간격
#define VERIFY_TIMEOUT_MS        10000 // 센서 변화 대기 최대 시간 (GD-3000 잠금 시간 설정 고려)
#define RETRY_INTERVAL_MS        500   // 재시도 간격
#define DEFAULT_EXIT_DURATION_SEC 5
#define EXIT_MIN_SEC             3
#define EXIT_MAX_SEC             30
#define POLL_INTERVAL_MS         200   // 문상태 센서 폴링 주기
#define DEBOUNCE_COUNT           3     // 상태 확정에 필요한 연속 동일 값
#define SENSOR_STUCK_TIMEOUT_MS  600000 // 센서 고착 판정 시간 (10분, 테스트 중 여유 확보)
#define POWER_CHECK_INTERVAL_MS  5000  // 전원 모니터링 주기
#define POWER_MIN_MV             10000 // 최소 허용 전압 (10V)
#define POWER_MAX_MV             13000 // 최대 허용 전압 (13V)
#define POWER_FAULT_COUNT        3     // 전원 이상 연속 횟수

/* ── Operation Result ── */
typedef enum {
    OP_RESULT_SUCCESS = 0,
    OP_RESULT_FAIL_VERIFY,      // 센서 검증 실패
    OP_RESULT_FAIL_MAX_RETRY,   // 최대 재시도 초과
    OP_RESULT_FAIL_POWER,       // 전원 이상으로 동작 중단
    OP_RESULT_FAIL_RELAY,       // 릴레이/데드볼트 물리 고장
    OP_RESULT_FAIL_BUSY,        // 다른 동작 진행 중
} op_result_t;

/* ── Door Engine State ── */
typedef enum {
    DOOR_STATE_IDLE = 0,
    DOOR_STATE_OPERATING,
    DOOR_STATE_VERIFYING,
    DOOR_STATE_RETRY,
    DOOR_STATE_REPORT,
    DOOR_STATE_FAIL_REPORT,
} door_engine_state_t;

/* ── Error Codes (BLE Byte[1]) ── */
#define DOOR_ERR_SENSOR   0x01   // 센서 고착
#define DOOR_ERR_RELAY    0x02   // 릴레이/데드볼트 물리 고장
#define DOOR_ERR_POWER    0x03   // 12V 전원 이상
#define DOOR_ERR_TIMEOUT  0x04   // 타임아웃
#define DOOR_ERR_FORCED   0x05   // 비정상 개방 감지

/* ── Pending result for NVS storage ── */
typedef struct {
    int64_t timestamp;
    op_result_t result;
    uint8_t attempts;
    bool target_unlock;
} pending_result_t;

/* ── Public API ── */

/**
 * @brief 도어 컨트롤러 초기화 (타이머 생성)
 */
esp_err_t door_controller_init(void);

/**
 * @brief 개폐 동작 + 검증 (해제: 센서 폴링, 잠금: 12V 복원 즉시 완료)
 * @param target_unlock true=해제, false=잠금
 * @return 동작 결과
 */
op_result_t door_execute_with_retry(bool target_unlock);

/**
 * @brief 명령 큐에 추가 (논블로킹, 최신 명령 우선)
 * @param target_unlock true=해제, false=잠금
 *
 * 현재 실행 중이면 큐에 넣고 완료 후 자동 실행.
 * 큐에 이미 대기 중인 명령이 있으면 최신 명령으로 덮어씀.
 */
void door_queue_command(bool target_unlock);

/**
 * @brief 퇴실 기능 (시간 제한 열림 후 자동 잠금)
 * @param duration_sec 열림 유지 시간 (3~30초)
 */
esp_err_t door_exit_open(uint8_t duration_sec);

/**
 * @brief 현재 도어 엔진 상태 반환
 */
door_engine_state_t door_get_state(void);

/**
 * @brief 센서 신뢰 여부
 */
bool door_is_sensor_reliable(void);

/**
 * @brief 소프트웨어 잠금 상태 (릴레이 명령 기반 추적)
 * @return true=잠금, false=해제
 *
 * GD-3000 센서는 문 열림/닫힘만 감지하므로,
 * 볼트 잠금/해제는 릴레이 명령 + 자동 잠금 타이머로 소프트웨어 추적
 */
bool door_is_locked(void);

#ifdef __cplusplus
}
#endif
