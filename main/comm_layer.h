#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "door_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Communication Status ── */
typedef enum {
    COMM_MATTER_CONNECTED = 0,
    COMM_MATTER_DISCONNECTED,
    COMM_BLE_CONNECTED,
    COMM_BLE_DISCONNECTED,
} comm_status_t;

/* ── Public API ── */

/**
 * @brief Matter 연결 상태 확인
 */
bool is_matter_connected(void);

/**
 * @brief BLE 클라이언트 연결 상태 확인
 */
bool is_ble_connected(void);

/**
 * @brief Matter 연결 상태 설정 (콜백에서 호출)
 */
void comm_set_matter_connected(bool connected);

/**
 * @brief BLE 연결 상태 설정 (콜백에서 호출)
 */
void comm_set_ble_connected(bool connected);

/**
 * @brief 도어 동작 결과 보고 (Matter 우선, BLE 폴백)
 * @param result 동작 결과
 * @param attempts 시도 횟수
 */
void report_result(op_result_t result, uint8_t attempts);

/**
 * @brief LockState 보고 (폴링 기반 상태 변경 시)
 * @param locked true=잠김, false=열림
 */
void report_lock_state(bool locked);

/**
 * @brief 에러 보고 (센서 이상, 릴레이 이상, 전원 이상, 비정상 개방)
 * @param err_data 2-byte 에러 데이터 [0xFF, error_code]
 */
void report_error(const uint8_t *err_data);

/**
 * @brief 미전송 결과를 NVS에 저장
 */
void save_pending_result_to_nvs(const pending_result_t *pending);

/**
 * @brief NVS에서 미전송 결과 로드 및 전송 시도 (재연결 시 호출)
 */
void flush_pending_results(void);

/**
 * @brief Matter endpoint ID 설정
 */
void comm_set_endpoint_id(uint16_t endpoint_id);

#ifdef __cplusplus
}
#endif
