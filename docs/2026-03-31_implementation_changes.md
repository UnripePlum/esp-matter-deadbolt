# ESP32-S3 Matter 도어락 — 구현 변경 사항 (2026-03-31)

**기준:** 2026-03-30 빌드 대비 변경
**빌드 결과:** 1.38MB (이전 1.35MB, +30KB)

---

## 변경 요약

엔지니어링 리뷰에서 발견된 P0 3건, P1 3건, P2 2건 버그 수정 + 신규 기능 3건 구현.

| # | 분류 | 변경 내용 | 파일 |
|---|------|----------|------|
| 1 | P0 버그 | 도어 동작 직렬화 (FreeRTOS mutex) | door_controller.cpp |
| 2 | P0 버그 | exit_timer_cb → xTaskCreate 분리 (타이머 블로킹 방지) | door_controller.cpp |
| 3 | P0 버그 | flush_pending_results NVS 재저장 방지 | comm_layer.cpp |
| 4 | P1 보안 | BLE nonce counter NVS 영속 (IV 재사용 방지) | ble_server.cpp |
| 5 | P1 보안 | report_lock_state BLE 암호화 전환 | comm_layer.cpp |
| 6 | P1 동시성 | attribute_update_cb + emberAf 중복 실행 방지 (mutex) | app_main.cpp |
| 7 | P2 로직 | NVS 미전송 큐 ring buffer 패턴 | comm_layer.cpp |
| 8 | P2 안전 | ciphertext 버퍼 크기 guard | ble_server.cpp |
| 9 | 신규 | HAL 릴레이 통전 시간 상한 (500ms, 솔레노이드 과열 보호) | hal_gpio.cpp/h |
| 10 | 신규 | AES-128-GCM 키 NVS 프로비저닝 (하드코딩 제거) | ble_server.cpp |
| 11 | 신규 | ERR_RELAY 감지 (센서 변화 없음 → 릴레이 물리 고장 판정) | door_controller.cpp/h |
| 12 | 신규 | BLE Challenge-Response (HMAC-SHA256) | ble_server.cpp/h |

---

## 상세 변경

### 1. 도어 동작 직렬화 (Mutex)

**문제:** Matter Lock과 BLE Unlock이 동시에 들어오면 두 개의 태스크가 릴레이를 동시에 제어. 데드볼트 물리 손상 위험.

**수정:** `door_controller.cpp`에 `SemaphoreHandle_t s_door_mutex` 추가. `door_execute_with_retry()` 진입 시 3초 타임아웃으로 mutex 획득. 실패 시 `OP_RESULT_FAIL_BUSY` 반환.

### 2. exit_timer_cb 블로킹 해소

**문제:** 퇴실 자동잠금 타이머 콜백에서 `door_execute_with_retry()`를 직접 호출. `vTaskDelay`가 esp_timer 태스크를 블로킹하여 모든 타이머(센서 폴링, 전원 모니터) 최대 1.9초 정지.

**수정:** `exit_timer_cb`에서 `xTaskCreate(exit_lock_task, ...)`로 별도 태스크 생성. 타이머 콜백은 즉시 반환.

### 3. flush NVS 재저장 방지

**문제:** `flush_pending_results()`가 `report_result()`를 호출하면, Matter/BLE 모두 불가 시 다시 `save_pending_result_to_nvs()`가 호출되어 동일 데이터 무한 재저장. NVS 쓰기 수명 소모.

**수정:** `try_report_without_nvs()` 내부 함수 추가. flush 시에는 NVS 저장 없이 Matter/BLE 전송만 시도. 실패분은 NVS에 그대로 유지.

### 4. BLE Nonce Counter NVS 영속

**문제:** `s_nonce_counter`가 static 변수로 부팅 시 0 초기화. 동일 키 + 동일 IV → AES-GCM 보안 무효화.

**수정:**
- 부팅 시 NVS에서 카운터 로드 + `NONCE_SAVE_INTERVAL`(100) 만큼 점프
- 100회 Notify마다 NVS 저장 (쓰기 수명 보호)
- NVS namespace: `ble_crypto`, key: `nonce_ctr`

### 5. report_lock_state BLE 암호화

**문제:** `report_lock_state()`가 `ble_notify_status()`(평문) 사용. 문상태 변경이 BLE 스니핑에 노출.

**수정:** `ble_notify_status_encrypted()`로 변경 (comm_layer.cpp:140).

### 6. attribute_update_cb 중복 방지

**문제:** Matter Lock 명령 시 `emberAfPluginDoorLockOnDoorLockCommand`과 `app_attribute_update_cb` 양쪽에서 `door_control_task` 생성. 한 명령에 릴레이 2회 동작.

**수정:** 두 경로 모두 유지하되 (fabric 직접 쓰기 지원), 이슈 1의 mutex가 중복 실행 방지. 두 번째 태스크는 mutex 대기 후 실행되지만, 이미 원하는 상태이면 센서 검증에서 즉시 성공.

### 7. NVS Ring Buffer

**문제:** 큐 가득 참 시 `count = MAX_PENDING - 1`로 설정하여 마지막 인덱스만 반복 덮어씀. 오래된 데이터 영구 잔류.

**수정:** `write_idx` 변수 추가. 순환 인덱스로 가장 오래된 항목부터 덮어씀.

### 8. Ciphertext 버퍼 Guard

**문제:** `ciphertext[16]` 고정 크기. `len` 파라미터가 16 초과 시 버퍼 오버플로.

**수정:** `BLE_MAX_PLAINTEXT`(16) 정의. `len > BLE_MAX_PLAINTEXT` 시 early return.

### 9. HAL 릴레이 통전 시간 상한

**신규:** GD-3000 솔레노이드 과열 보호. `deadbolt_set(true)` 호출 시 500ms one-shot 타이머 시작. 타이머 만료 시 강제 OFF.

### 10. AES-128-GCM 키 NVS 프로비저닝

**신규:** 하드코딩 키 제거. 첫 부팅 시 `esp_random()`으로 16바이트 키 생성 → NVS 저장. 이후 부팅에서는 NVS에서 로드.

**주의:** NVS 평문 저장. 양산 시 flash encryption 필수.

### 11. ERR_RELAY 감지

**신규:** 재시도 3회 동안 센서 값이 한 번도 변하지 않으면 `OP_RESULT_FAIL_RELAY` + `DOOR_ERR_RELAY`(0x02) 보고. 기존 `OP_RESULT_FAIL_MAX_RETRY`와 구분.

### 12. BLE Challenge-Response (HMAC-SHA256)

**신규:** UNLOCK(0x02)과 EXIT_OPEN(0x03) 명령에 인증 필요.

**프로토콜:**
1. 클라이언트가 Challenge Characteristic(FF05) Read → 16바이트 nonce 수신
2. 클라이언트가 `HMAC-SHA256(AES_KEY, nonce || cmd || param)` 계산
3. Command Characteristic(FF02)에 `[cmd, param, HMAC(32)]` (34바이트) Write
4. 서버가 HMAC 검증 후 명령 실행

**보안 특성:**
- 일회용 nonce (사용 후 무효화)
- 30초 TTL
- BLE 연결 해제 시 자동 무효화
- Constant-time HMAC 비교 (타이밍 공격 방지)
- LOCK(0x01), STATUS_REQ(0x04)는 인증 불필요

---

## 아키텍처 (수정 후)

```
┌─────────────────────────────────────────────────────────┐
│                    app_main.cpp                          │
│  Matter 노드 + CHIP 콜백 + attribute_update_cb          │
│  (중복 실행 → mutex가 직렬화)                             │
├──────────┬──────────────┬───────────────────────────────┤
│          │              │                                │
│  ┌───────▼────────┐  ┌─▼──────────────────────────┐    │
│  │ door_controller │  │ comm_layer                  │    │
│  │ + FreeRTOS mutex│  │ Matter→BLE(암호화)→NVS 폴백  │    │
│  │ + ERR_RELAY 감지│  │ + ring buffer NVS            │    │
│  │ + exit xTask   │  │ + flush 재저장 방지            │    │
│  └───────┬────────┘  └──────────┬───────────────────┘    │
│          │                       │                        │
│  ┌───────▼───────────────────────▼────────────────────┐  │
│  │              ble_server.cpp                         │  │
│  │  GATT + AES-128-GCM (NVS 키) + nonce NVS 영속     │  │
│  │  + Challenge-Response (HMAC-SHA256)                 │  │
│  │  + FF05 Challenge Characteristic                    │  │
│  └────────────────────────────────────────────────────┘  │
│  ┌────────────────────────────────────────────────────┐  │
│  │              hal_gpio.cpp                           │  │
│  │  릴레이 + 안전 타이머(500ms) │ 센서 │ LED │ ADC    │  │
│  └────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

---

## 남은 작업 (TODO)

| 항목 | 우선순위 | 비고 |
|------|---------|------|
| Flash encryption (NVS 암호화) | 양산 전 필수 | eFuse 비가역 |
| Matter 인증 테스트 | 상용 배포 시 | Matter Test Harness |
| OTA E2E 테스트 | 배포 전 | 파티션 준비됨 (29% 여유) |
| Task watchdog 등록 | 권장 | door_control_task에 esp_task_wdt 적용 |
