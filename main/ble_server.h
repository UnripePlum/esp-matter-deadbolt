#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── BLE Service/Characteristic UUIDs ── */
// Service: 0000FF01-0000-1000-8000-00805F9B34FB
// Command: 0000FF02-...  (Write)
// Status:  0000FF03-...  (Read, Notify)
// Config:  0000FF04-...  (Read, Write)
// Challenge: 0000FF05-... (Read — 챌린지 nonce 발급)

/* ── BLE Command Bytes ── */
#define BLE_CMD_LOCK        0x01
#define BLE_CMD_UNLOCK      0x02
#define BLE_CMD_EXIT_OPEN   0x03
#define BLE_CMD_STATUS_REQ  0x04

/* ── BLE Status Bytes ── */
#define BLE_STATUS_LOCKED      0x01
#define BLE_STATUS_UNLOCKED    0x02
#define BLE_STATUS_OP_SUCCESS  0x10
#define BLE_STATUS_OP_FAIL     0x11
#define BLE_STATUS_AUTH_FAIL   0x12
#define BLE_STATUS_ERROR       0xFF

/* ── Challenge-Response ── */
#define BLE_CHALLENGE_SIZE     16   // 챌린지 nonce 크기
#define BLE_CHALLENGE_TTL_MS   30000 // 챌린지 유효 시간 (30초)
#define BLE_HMAC_SIZE          32   // HMAC-SHA256 출력 크기

/* ── AES-GCM 설정 ── */
#define BLE_AES_KEY_SIZE       16
#define BLE_MAX_PLAINTEXT      16   // 최대 암호화 가능 평문 크기

/* ── Public API ── */

/**
 * @brief BLE 암호화 키 초기화 (NVS에서 AES 키 + Nonce 로드)
 *        Matter 시작 전에 호출. NimBLE 불필요.
 */
esp_err_t ble_crypto_init(void);

/**
 * @brief BLE GATT 서버 초기화 (Matter가 NimBLE 초기화한 후 호출)
 */
esp_err_t ble_gatt_server_init(void);

/**
 * @brief BLE Status Characteristic로 Notify 전송 (평문)
 * @param data 2-byte 상태 데이터
 * @param len 데이터 길이
 */
void ble_notify_status(const uint8_t *data, uint16_t len);

/**
 * @brief BLE Status Characteristic로 Notify 전송 (AES-128-GCM 암호화)
 * @param plaintext 평문 데이터
 * @param len 평문 길이 (최대 BLE_MAX_PLAINTEXT)
 */
void ble_notify_status_encrypted(const uint8_t *plaintext, uint16_t len);

/**
 * @brief 현재 상태를 BLE로 전송
 */
void ble_send_current_status(void);

/**
 * @brief AES 키를 NVS에서 로드 (없으면 랜덤 생성 후 저장)
 */
esp_err_t ble_load_or_generate_key(void);

#ifdef __cplusplus
}
#endif
