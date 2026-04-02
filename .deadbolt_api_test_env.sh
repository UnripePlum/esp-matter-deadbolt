#!/bin/bash

export PATH="$HOME/esp/esp-matter/connectedhomeip/connectedhomeip/.environment/gn_out:$PATH"

COMMISSIONER_NAME="${COMMISSIONER_NAME:-alpha}"
NODE_ID="${NODE_ID:-1}"
ENDPOINT_ID="${ENDPOINT_ID:-1}"

# ── Output Filters ──────────────────────────────────────────

_strip_ansi() {
  sed $'s/\x1b\\[[0-9;]*m//g'
}

_filter_read() {
  local output
  output="$(_strip_ansi)"

  local err
  err="$(printf '%s\n' "$output" | grep -m1 'Run command failure:')"
  if [[ -n "$err" ]]; then
    printf '\033[31mERROR: %s\033[0m\n' "${err##*Run command failure: }"
    return 1
  fi

  local data_line
  data_line="$(printf '%s\n' "$output" | grep -m1 'Data = ')"
  if [[ -z "$data_line" ]]; then
    printf '\033[31mERROR: no data in response\033[0m\n'
    return 1
  fi

  local num_val
  num_val="$(printf '%s\n' "$data_line" | sed -n 's/.*Data = \([0-9-]*\) .*/\1/p')"
  if [[ -n "$num_val" ]]; then
    printf '%s\n' "$num_val"
    return 0
  fi

  printf '%s\n' "$data_line" | sed 's/.*Data = //'
}

_filter_cmd() {
  local output
  output="$(_strip_ansi)"

  local err
  err="$(printf '%s\n' "$output" | grep -m1 'Run command failure:')"
  if [[ -n "$err" ]]; then
    printf '\033[31mERROR: %s\033[0m\n' "${err##*Run command failure: }"
    return 1
  fi

  local status_line
  status_line="$(printf '%s\n' "$output" | grep -m1 'Received Command Response Status')"
  if [[ -n "$status_line" ]]; then
    local cmd_status
    cmd_status="$(printf '%s' "$status_line" | sed -n 's/.*Status=\(0x[0-9a-fA-F]*\).*/\1/p')"
    if [[ "$cmd_status" == "0x0" ]]; then
      printf '\033[32mOK\033[0m\n'
    else
      printf '\033[31mFAIL (Status=%s)\033[0m\n' "$cmd_status"
      return 1
    fi
    return 0
  fi

  printf 'OK\n'
}

_filter_pair() {
  local output
  output="$(_strip_ansi)"

  if printf '%s\n' "$output" | grep -q 'commissioning completed with success'; then
    printf '\033[32mPaired successfully\033[0m\n'
    return 0
  fi

  local err
  err="$(printf '%s\n' "$output" | grep -m1 'commissioning Failure:\|Run command failure:')"
  if [[ -n "$err" ]]; then
    printf '\033[31mFAILED: %s\033[0m\n' "${err##*: }"
    return 1
  fi

  printf 'Unknown result\n'
  return 1
}

# ── Door Lock Commands ──────────────────────────────────────

lock_door() {
  printf 'Locking (node=%s endpoint=%s) ...\n' "$NODE_ID" "$ENDPOINT_ID"
  chip-tool doorlock lock-door "$NODE_ID" "$ENDPOINT_ID" \
    --timedInteractionTimeoutMs 1000 \
    --commissioner-name "$COMMISSIONER_NAME" 2>&1 | _filter_cmd
}

unlock_door() {
  printf 'Unlocking (node=%s endpoint=%s) ...\n' "$NODE_ID" "$ENDPOINT_ID"
  chip-tool doorlock unlock-door "$NODE_ID" "$ENDPOINT_ID" \
    --timedInteractionTimeoutMs 1000 \
    --commissioner-name "$COMMISSIONER_NAME" 2>&1 | _filter_cmd
}

# ── Attribute Reads ─────────────────────────────────────────

lock_state() {
  local val
  val="$(chip-tool doorlock read lock-state "$NODE_ID" "$ENDPOINT_ID" \
    --commissioner-name "$COMMISSIONER_NAME" 2>&1 | _filter_read)"
  local rc=$?
  case "$val" in
    0) printf '%s (NotFullyLocked)\n' "$val" ;;
    1) printf '%s (Locked)\n' "$val" ;;
    2) printf '%s (Unlocked)\n' "$val" ;;
    3) printf '%s (Unlatched)\n' "$val" ;;
    *) printf '%s\n' "$val" ;;
  esac
  return $rc
}

door_state() {
  local val
  val="$(chip-tool doorlock read door-state "$NODE_ID" "$ENDPOINT_ID" \
    --commissioner-name "$COMMISSIONER_NAME" 2>&1 | _filter_read)"
  local rc=$?
  case "$val" in
    0) printf '%s (DoorOpen)\n' "$val" ;;
    1) printf '%s (DoorClosed)\n' "$val" ;;
    2) printf '%s (DoorJammed)\n' "$val" ;;
    3) printf '%s (DoorForcedOpen)\n' "$val" ;;
    4) printf '%s (DoorUnspecifiedError)\n' "$val" ;;
    5) printf '%s (DoorAjar)\n' "$val" ;;
    *) printf '%s\n' "$val" ;;
  esac
  return $rc
}

lock_type() {
  local val
  val="$(chip-tool doorlock read lock-type "$NODE_ID" "$ENDPOINT_ID" \
    --commissioner-name "$COMMISSIONER_NAME" 2>&1 | _filter_read)"
  local rc=$?
  case "$val" in
    0) printf '%s (DeadBolt)\n' "$val" ;;
    1) printf '%s (Magnetic)\n' "$val" ;;
    2) printf '%s (Other)\n' "$val" ;;
    3) printf '%s (Mortise)\n' "$val" ;;
    4) printf '%s (Rim)\n' "$val" ;;
    5) printf '%s (LatchBolt)\n' "$val" ;;
    6) printf '%s (CylindricalLock)\n' "$val" ;;
    7) printf '%s (TubularLock)\n' "$val" ;;
    8) printf '%s (InterconnectedLock)\n' "$val" ;;
    9) printf '%s (DeadLatch)\n' "$val" ;;
    10) printf '%s (DoorFurniture)\n' "$val" ;;
    *) printf '%s\n' "$val" ;;
  esac
  return $rc
}

actuator_enabled() {
  chip-tool doorlock read actuator-enabled "$NODE_ID" "$ENDPOINT_ID" \
    --commissioner-name "$COMMISSIONER_NAME" 2>&1 | _filter_read
}

# ── Convenience ─────────────────────────────────────────────

full_status() {
  echo "=== Lock State ==="
  lock_state
  echo "=== Door State ==="
  door_state
  echo "=== Lock Type ==="
  lock_type
  echo "=== Actuator Enabled ==="
  actuator_enabled
}

smoke() {
  echo "── Smoke Test ──────────────────────────"
  echo ""
  echo "[1/5] Lock State:"
  lock_state
  echo ""
  echo "[2/5] Lock Door:"
  lock_door
  sleep 1
  echo ""
  echo "[3/5] Verify Locked:"
  lock_state
  echo ""
  echo "[4/5] Unlock Door:"
  unlock_door
  sleep 1
  echo ""
  echo "[5/5] Verify Unlocked:"
  lock_state
  echo ""
  echo "── Smoke Test Complete ─────────────────"
}

# ── Commissioning ───────────────────────────────────────────

pair() {
  local pin="${1:-20202021}"
  printf 'Pairing node %s (pin: %s) ...\n' "$NODE_ID" "$pin"
  chip-tool pairing onnetwork "$NODE_ID" "$pin" \
    --commissioner-name "$COMMISSIONER_NAME" 2>&1 | _filter_pair
}

pair_wifi() {
  local ssid="${1:-}"
  local password="${2:-}"
  local pin="${3:-20202021}"
  local disc="${4:-3840}"
  if [[ -z "$ssid" || -z "$password" ]]; then
    echo "usage: pair_wifi <ssid> <password> [pin] [discriminator]"
    return 1
  fi
  printf 'BLE WiFi pairing node %s (ssid: %s, pin: %s, disc: %s) ...\n' "$NODE_ID" "$ssid" "$pin" "$disc"
  chip-tool pairing ble-wifi "$NODE_ID" "$ssid" "$password" "$pin" "$disc" \
    --commissioner-name "$COMMISSIONER_NAME" 2>&1 | _filter_pair
}

pair_auto() {
  local pin="${1:-20202021}"
  local disc="${2:-3840}"
  local wifi_env="$PROJECT_DIR/.wifi_credentials"

  local ssid="" password=""
  if [[ -f "$wifi_env" ]]; then
    ssid="$(grep '^WIFI_SSID=' "$wifi_env" | cut -d= -f2-)"
    password="$(grep '^WIFI_PASSWORD=' "$wifi_env" | cut -d= -f2-)"
    if [[ -n "$ssid" && -n "$password" ]]; then
      echo "저장된 크레덴셜 사용: $ssid"
    fi
  fi

  if [[ -z "$ssid" || -z "$password" ]]; then
    read -r -p "WiFi SSID: " ssid
    if [[ -z "$ssid" ]]; then
      echo "SSID가 비어있습니다."
      return 1
    fi
    read -r -s -p "WiFi 비밀번호: " password
    echo ""
    if [[ -z "$password" ]]; then
      echo "비밀번호가 비어있습니다."
      return 1
    fi
    read -r -p "이 크레덴셜을 저장할까요? (y/N): " save_yn
    if [[ "$save_yn" =~ ^[yY] ]]; then
      printf 'WIFI_SSID=%s\nWIFI_PASSWORD=%s\n' "$ssid" "$password" > "$wifi_env"
      chmod 600 "$wifi_env"
      echo "저장 완료: $wifi_env"
    fi
  fi

  printf 'Auto WiFi pairing: ssid=%s node=%s pin=%s disc=%s\n' "$ssid" "$NODE_ID" "$pin" "$disc"
  chip-tool pairing ble-wifi "$NODE_ID" "$ssid" "$password" "$pin" "$disc" \
    --commissioner-name "$COMMISSIONER_NAME" 2>&1 | _filter_pair
}

unpair() {
  printf 'Unpairing node %s ...\n' "$NODE_ID"
  chip-tool pairing unpair "$NODE_ID" \
    --commissioner-name "$COMMISSIONER_NAME" 2>&1 | _filter_pair
}

# ── Help ────────────────────────────────────────────────────

api_help() {
  cat <<'HELP'
── 도어락 제어 ────────────────────────────────────────────
  /lock                       잠금 (Matter Lock 명령)
  /unlock                     해제 (Matter Unlock 명령)

── 상태 읽기 ──────────────────────────────────────────────
  /state                      LockState (0=NotFullyLocked 1=Locked 2=Unlocked)
  /door_state                 DoorState (0=Open 1=Closed 2=Jammed 3=ForcedOpen)
  /status                     전체 상태 조회 (LockState + DoorState + LockType + Actuator)

── 테스트 ─────────────────────────────────────────────────
  /smoke                      스모크 테스트 (상태확인 → 잠금 → 확인 → 해제 → 확인)

── 커미셔닝 ───────────────────────────────────────────────
  /pair [pin]                 온네트워크 커미셔닝 (기본 pin: 20202021)
  /pair_wifi <ssid> <pw>      BLE WiFi 커미셔닝
  /pair_auto                  저장된 WiFi로 자동 BLE 커미셔닝
  /unpair                     커미셔닝 해제

── chip-tool 직접 실행 ────────────────────────────────────
  chip-tool doorlock ...      아무 chip-tool 명령 직접 실행 가능
  /any read-by-id ...         / 접두사로 쉘 명령 실행

── REPL ───────────────────────────────────────────────────
  /help                       이 도움말
  /node                       현재 node/endpoint 확인
  /exit                       종료
HELP
}
