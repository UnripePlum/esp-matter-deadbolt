#include "ble_server.h"
#include "hal_gpio.h"
#include "door_controller.h"
#include "comm_layer.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "mbedtls/gcm.h"
#include "mbedtls/md.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ble_srv";

/* ── UUIDs ── */
static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x01, 0xFF, 0x00, 0x00);

static const ble_uuid128_t s_cmd_uuid =
    BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x02, 0xFF, 0x00, 0x00);

static const ble_uuid128_t s_status_uuid =
    BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x03, 0xFF, 0x00, 0x00);

static const ble_uuid128_t s_config_uuid =
    BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x04, 0xFF, 0x00, 0x00);

static const ble_uuid128_t s_challenge_uuid =
    BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x05, 0xFF, 0x00, 0x00);

/* ── BLE state ── */
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_status_val_handle = 0;
static bool     s_status_notify_enabled = false;

/* ═══════════════════════════════════════════════════════════
 *  AES-128-GCM 키 (NVS 프로비저닝)
 *
 *  ┌─ 부팅 시 ─────────────────────────────┐
 *  │ NVS "ble_crypto" / "aes_key" 읽기     │
 *  │  ├─ 있으면 → 로드                      │
 *  │  └─ 없으면 → esp_random()으로 생성 → 저장 │
 *  └────────────────────────────────────────┘
 * ═══════════════════════════════════════════════════════════ */

static uint8_t s_aes_key[BLE_AES_KEY_SIZE];

/* ── Nonce counter (NVS 영속, IV 재사용 방지) ── */
static uint32_t s_nonce_counter = 0;
#define NONCE_NVS_NAMESPACE  "ble_crypto"
#define NONCE_NVS_KEY        "nonce_ctr"
#define NONCE_SAVE_INTERVAL  100   // 100회마다 NVS에 저장 (쓰기 수명 보호)

static void nonce_save_to_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(NONCE_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u32(handle, NONCE_NVS_KEY, s_nonce_counter);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void nonce_load_from_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(NONCE_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_get_u32(handle, NONCE_NVS_KEY, &s_nonce_counter);
        nvs_close(handle);
    }
    /* 부팅 시 NONCE_SAVE_INTERVAL만큼 점프하여 이전 세션 IV와 겹치지 않도록 */
    s_nonce_counter += NONCE_SAVE_INTERVAL;
    nonce_save_to_nvs();
    ESP_LOGI(TAG, "Nonce counter 로드: %" PRIu32, s_nonce_counter);
}

/* ═══════════════════════════════════════════════════════════
 *  AES 키 NVS 프로비저닝
 * ═══════════════════════════════════════════════════════════ */

esp_err_t ble_load_or_generate_key(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NONCE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 열기 실패: %s", esp_err_to_name(err));
        return err;
    }

    size_t key_len = BLE_AES_KEY_SIZE;
    err = nvs_get_blob(handle, "aes_key", s_aes_key, &key_len);

    if (err == ESP_ERR_NVS_NOT_FOUND || key_len != BLE_AES_KEY_SIZE) {
        /* 키 없음 → 랜덤 생성 후 저장 */
        ESP_LOGW(TAG, "AES 키 미발견 → 랜덤 생성");
        for (int i = 0; i < BLE_AES_KEY_SIZE; i += 4) {
            uint32_t r = esp_random();
            memcpy(s_aes_key + i, &r, (i + 4 <= BLE_AES_KEY_SIZE) ? 4 : BLE_AES_KEY_SIZE - i);
        }
        nvs_set_blob(handle, "aes_key", s_aes_key, BLE_AES_KEY_SIZE);
        nvs_commit(handle);
        ESP_LOGI(TAG, "AES 키 생성 및 NVS 저장 완료");
    } else if (err == ESP_OK) {
        ESP_LOGI(TAG, "AES 키 NVS에서 로드 완료");
    } else {
        ESP_LOGE(TAG, "AES 키 읽기 실패: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    nvs_close(handle);

    /* Nonce counter도 함께 로드 */
    nonce_load_from_nvs();

    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════
 *  Challenge-Response (HMAC-SHA256)
 *
 *  ┌─ Client ─────────────────────── Server ──┐
 *  │ 1. Read Challenge Char  →  nonce(16B)    │
 *  │ 2. HMAC = SHA256(key, nonce||cmd||param) │
 *  │ 3. Write Command: [cmd, param, HMAC(32)] │
 *  │ 4.                    ←  검증 후 실행     │
 *  └──────────────────────────────────────────┘
 * ═══════════════════════════════════════════════════════════ */

static uint8_t  s_challenge_nonce[BLE_CHALLENGE_SIZE];
static int64_t  s_challenge_issued_us = 0;
static bool     s_challenge_valid = false;

static void generate_challenge(void)
{
    for (int i = 0; i < BLE_CHALLENGE_SIZE; i += 4) {
        uint32_t r = esp_random();
        memcpy(s_challenge_nonce + i, &r,
               (i + 4 <= BLE_CHALLENGE_SIZE) ? 4 : BLE_CHALLENGE_SIZE - i);
    }
    s_challenge_issued_us = esp_timer_get_time();
    s_challenge_valid = true;
}

static bool verify_hmac(uint8_t cmd, uint8_t param, const uint8_t *client_hmac)
{
    if (!s_challenge_valid) {
        ESP_LOGW(TAG, "Challenge 미발급 상태");
        return false;
    }

    /* TTL 체크 */
    int64_t elapsed_ms = (esp_timer_get_time() - s_challenge_issued_us) / 1000;
    if (elapsed_ms > BLE_CHALLENGE_TTL_MS) {
        ESP_LOGW(TAG, "Challenge 만료 (%" PRId64 "ms)", elapsed_ms);
        s_challenge_valid = false;
        return false;
    }

    /* HMAC-SHA256(key, nonce || cmd || param) */
    uint8_t msg[BLE_CHALLENGE_SIZE + 2];
    memcpy(msg, s_challenge_nonce, BLE_CHALLENGE_SIZE);
    msg[BLE_CHALLENGE_SIZE] = cmd;
    msg[BLE_CHALLENGE_SIZE + 1] = param;

    uint8_t expected[BLE_HMAC_SIZE];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, s_aes_key, BLE_AES_KEY_SIZE);
    mbedtls_md_hmac_update(&ctx, msg, sizeof(msg));
    mbedtls_md_hmac_finish(&ctx, expected);
    mbedtls_md_free(&ctx);

    /* 일회용: 사용 후 무효화 */
    s_challenge_valid = false;

    /* Constant-time 비교 */
    uint8_t diff = 0;
    for (int i = 0; i < BLE_HMAC_SIZE; i++) {
        diff |= expected[i] ^ client_hmac[i];
    }

    return (diff == 0);
}

static bool cmd_requires_auth(uint8_t cmd)
{
    return (cmd == BLE_CMD_UNLOCK || cmd == BLE_CMD_EXIT_OPEN);
}

/* ── Forward declarations ── */
static int  ble_cmd_write_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg);
static int  ble_status_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg);
static int  ble_config_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg);
static int  ble_challenge_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt, void *arg);
static void ble_command_handler(uint8_t *data, uint16_t len);
static void ble_on_sync(void);
static int  ble_gap_event_cb(struct ble_gap_event *event, void *arg);

/* ── Door control task (runs BLE commands off the BLE callback stack) ── */
typedef struct {
    uint8_t cmd;
    uint8_t param;
} ble_cmd_msg_t;

static void ble_door_task(void *arg)
{
    ble_cmd_msg_t *msg = (ble_cmd_msg_t *)arg;
    ble_command_handler(&msg->cmd, 2);
    free(msg);
    vTaskDelete(NULL);
}

/* ═══════════════════════════════════════════════════════════
 *  GATT Service Definition
 * ═══════════════════════════════════════════════════════════ */

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // Command Characteristic (Write)
                .uuid = &s_cmd_uuid.u,
                .access_cb = ble_cmd_write_cb,
                .arg = NULL,
                .descriptors = NULL,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
                .min_key_size = 0,
                .val_handle = NULL,
            },
            {
                // Status Characteristic (Read, Notify)
                .uuid = &s_status_uuid.u,
                .access_cb = ble_status_access_cb,
                .arg = NULL,
                .descriptors = NULL,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY
                       | BLE_GATT_CHR_F_READ_ENC,
                .min_key_size = 0,
                .val_handle = &s_status_val_handle,
            },
            {
                // Config Characteristic (Read, Write)
                .uuid = &s_config_uuid.u,
                .access_cb = ble_config_access_cb,
                .arg = NULL,
                .descriptors = NULL,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE
                       | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC,
                .min_key_size = 0,
                .val_handle = NULL,
            },
            {
                // Challenge Characteristic (Read — 챌린지 nonce 발급)
                .uuid = &s_challenge_uuid.u,
                .access_cb = ble_challenge_access_cb,
                .arg = NULL,
                .descriptors = NULL,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
                .min_key_size = 0,
                .val_handle = NULL,
            },
            { 0 }, // sentinel
        },
    },
    { 0 }, // sentinel
};

/* ═══════════════════════════════════════════════════════════
 *  GATT Callbacks
 * ═══════════════════════════════════════════════════════════ */

static int ble_cmd_write_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 2) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    /* 최대 패킷 크기: cmd(1) + param(1) + HMAC(32) = 34 */
    uint8_t buf[34];
    uint16_t copy_len = (len > sizeof(buf)) ? sizeof(buf) : len;
    os_mbuf_copydata(ctxt->om, 0, copy_len, buf);

    uint8_t cmd = buf[0];
    uint8_t param = buf[1];

    /* 민감 명령은 Challenge-Response 인증 필요 */
    if (cmd_requires_auth(cmd)) {
        if (len < 2 + BLE_HMAC_SIZE) {
            ESP_LOGW(TAG, "인증 필요 명령인데 HMAC 누락 (len=%d)", len);
            uint8_t resp[2] = { BLE_STATUS_AUTH_FAIL, 0x00 };
            ble_notify_status_encrypted(resp, sizeof(resp));
            return 0;
        }
        if (!verify_hmac(cmd, param, buf + 2)) {
            ESP_LOGW(TAG, "HMAC 검증 실패: cmd=0x%02X", cmd);
            uint8_t resp[2] = { BLE_STATUS_AUTH_FAIL, 0x01 };
            ble_notify_status_encrypted(resp, sizeof(resp));
            return 0;
        }
        ESP_LOGI(TAG, "HMAC 검증 성공: cmd=0x%02X", cmd);
    }

    // Dispatch to FreeRTOS task to avoid blocking BLE stack
    ble_cmd_msg_t *msg = (ble_cmd_msg_t *)malloc(sizeof(ble_cmd_msg_t));
    if (msg) {
        msg->cmd = cmd;
        msg->param = param;
        xTaskCreate(ble_door_task, "ble_door", 4096, msg, 5, NULL);
    }

    return 0;
}

static int ble_status_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t status[2];
        status[0] = door_is_locked() ? BLE_STATUS_LOCKED : BLE_STATUS_UNLOCKED;
        status[1] = 0x00;
        os_mbuf_append(ctxt->om, status, sizeof(status));
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static uint8_t s_exit_duration_default = DEFAULT_EXIT_DURATION_SEC;

static int ble_config_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        os_mbuf_append(ctxt->om, &s_exit_duration_default, 1);
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t val;
        os_mbuf_copydata(ctxt->om, 0, 1, &val);
        if (val >= EXIT_MIN_SEC && val <= EXIT_MAX_SEC) {
            s_exit_duration_default = val;
            ESP_LOGI(TAG, "퇴실 기본시간 변경: %d초", val);
        }
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static int ble_challenge_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        generate_challenge();
        os_mbuf_append(ctxt->om, s_challenge_nonce, BLE_CHALLENGE_SIZE);
        ESP_LOGI(TAG, "Challenge nonce 발급 (유효시간: %dms)", BLE_CHALLENGE_TTL_MS);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* ═══════════════════════════════════════════════════════════
 *  BLE Command Handler
 * ═══════════════════════════════════════════════════════════ */

static void ble_command_handler(uint8_t *data, uint16_t len)
{
    if (len < 2) return;

    uint8_t cmd = data[0];
    uint8_t param = data[1];

    ESP_LOGI(TAG, "BLE 명령 수신: cmd=0x%02X param=0x%02X", cmd, param);

    switch (cmd) {
        case BLE_CMD_LOCK:
            door_queue_command(false);
            break;

        case BLE_CMD_UNLOCK:
            door_queue_command(true);
            break;

        case BLE_CMD_EXIT_OPEN:
            door_exit_open(param);
            break;

        case BLE_CMD_STATUS_REQ:
            ble_send_current_status();
            break;

        default:
            ESP_LOGW(TAG, "알 수 없는 BLE 명령: 0x%02X", cmd);
            break;
    }
}

/* ═══════════════════════════════════════════════════════════
 *  BLE Notify (평문 / AES-GCM 암호화)
 * ═══════════════════════════════════════════════════════════ */

void ble_notify_status(const uint8_t *data, uint16_t len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_status_notify_enabled) {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om) {
        ble_gatts_notify_custom(s_conn_handle, s_status_val_handle, om);
    }
}

void ble_notify_status_encrypted(const uint8_t *plaintext, uint16_t len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_status_notify_enabled) {
        return;
    }

    /* Guard: 최대 평문 크기 제한 */
    if (len > BLE_MAX_PLAINTEXT) {
        ESP_LOGE(TAG, "평문 크기 초과: %d > %d", len, BLE_MAX_PLAINTEXT);
        return;
    }

    // IV: counter(4B) + random(8B) = 12 bytes
    uint8_t iv[12];
    s_nonce_counter++;
    memcpy(iv, &s_nonce_counter, 4);
    uint32_t rand1 = esp_random();
    uint32_t rand2 = esp_random();
    memcpy(iv + 4, &rand1, 4);
    memcpy(iv + 8, &rand2, 4);

    /* 주기적 NVS 저장 (쓰기 수명 보호) */
    if ((s_nonce_counter % NONCE_SAVE_INTERVAL) == 0) {
        nonce_save_to_nvs();
    }

    // AES-128-GCM encrypt
    uint8_t ciphertext[BLE_MAX_PLAINTEXT];
    uint8_t tag[16];

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, s_aes_key, 128);
    mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT,
                               len, iv, sizeof(iv),
                               NULL, 0,
                               plaintext, ciphertext,
                               sizeof(tag), tag);
    mbedtls_gcm_free(&gcm);

    // Packet: IV(12) + Ciphertext(len) + Tag(16)
    uint8_t packet[12 + BLE_MAX_PLAINTEXT + 16];
    uint16_t pkt_len = 12 + len + 16;
    memcpy(packet, iv, 12);
    memcpy(packet + 12, ciphertext, len);
    memcpy(packet + 12 + len, tag, 16);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(packet, pkt_len);
    if (om) {
        ble_gatts_notify_custom(s_conn_handle, s_status_val_handle, om);
        ESP_LOGD(TAG, "BLE 암호화 Notify 전송: %d bytes", pkt_len);
    }
}

void ble_send_current_status(void)
{
    uint8_t status[2];
    status[0] = door_is_locked() ? BLE_STATUS_LOCKED : BLE_STATUS_UNLOCKED;
    status[1] = 0x00;
    ble_notify_status_encrypted(status, sizeof(status));
}

/* ═══════════════════════════════════════════════════════════
 *  GAP Event Handler
 * ═══════════════════════════════════════════════════════════ */

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn_handle = event->connect.conn_handle;
                comm_set_ble_connected(true);
                ESP_LOGI(TAG, "BLE 클라이언트 연결됨 (handle=%d)", s_conn_handle);

                // Request security (LE Secure Connections + MITM + Bonding)
                ble_gap_security_initiate(s_conn_handle);
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_status_notify_enabled = false;
            s_challenge_valid = false;  // 연결 해제 시 챌린지 무효화
            comm_set_ble_connected(false);
            ESP_LOGI(TAG, "BLE 클라이언트 연결 해제");

            // 다시 광고 시작
            ble_on_sync();
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == s_status_val_handle) {
                s_status_notify_enabled = event->subscribe.cur_notify;
                ESP_LOGI(TAG, "Status Notify %s",
                         s_status_notify_enabled ? "활성화" : "비활성화");
            }
            break;

        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI(TAG, "BLE 암호화 %s",
                     event->enc_change.status == 0 ? "성공" : "실패");
            break;

        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            // 이미 bonded된 기기는 기존 bond 삭제 후 재페어링
            struct ble_gap_conn_desc desc;
            ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
            ble_store_util_delete_peer(&desc.peer_id_addr);
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }

        default:
            break;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  BLE Advertising
 * ═══════════════════════════════════════════════════════════ */

static void ble_on_sync(void)
{
    // Set device name
    ble_svc_gap_device_name_set("MatterDoorLock");

    // Start advertising
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0,
        .itvl_max = 0,
        .channel_map = 0,
        .filter_policy = 0,
        .high_duty_cycle = 0,
    };

    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)ble_svc_gap_device_name();
    fields.name_len = strlen(ble_svc_gap_device_name());
    fields.name_is_complete = 1;

    ble_gap_adv_set_fields(&fields);
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                      &adv_params, ble_gap_event_cb, NULL);

    ESP_LOGI(TAG, "BLE 광고 시작");
}

/* ═══════════════════════════════════════════════════════════
 *  BLE GATT Server Init
 * ═══════════════════════════════════════════════════════════ */

esp_err_t ble_crypto_init(void)
{
    /* AES 키 로드/생성 + Nonce counter 로드 (NimBLE 불필요, NVS만 사용) */
    esp_err_t key_err = ble_load_or_generate_key();
    if (key_err != ESP_OK) {
        ESP_LOGE(TAG, "AES 키 초기화 실패!");
        return key_err;
    }
    ESP_LOGI(TAG, "BLE 암호화 키 초기화 완료");
    return ESP_OK;
}

esp_err_t ble_gatt_server_init(void)
{
    /* Matter가 이미 NimBLE를 초기화한 상태에서 호출됨 */

    // NimBLE host config (커스텀 BLE 폴백용)
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;           // LE Secure Connections
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sync_cb = ble_on_sync;

    // 커스텀 GATT 서비스 등록 (Matter의 NimBLE 위에 얹기)
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT count 실패: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT add 실패: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BLE GATT 서버 초기화 완료 (Matter NimBLE 위에 등록)");
    ESP_LOGI(TAG, "  Service:   0000FF01-...");
    ESP_LOGI(TAG, "  Command:   0000FF02-... (Write, HMAC 인증)");
    ESP_LOGI(TAG, "  Status:    0000FF03-... (Read/Notify, AES-GCM)");
    ESP_LOGI(TAG, "  Config:    0000FF04-... (Read/Write)");
    ESP_LOGI(TAG, "  Challenge: 0000FF05-... (Read, 일회용 nonce)");

    return ESP_OK;
}
