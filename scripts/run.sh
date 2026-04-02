#!/usr/bin/env bash
# esp-matter-deadbolt 빌드/플래시/모니터 실행 스크립트
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="esp32s3"
PORT="${PORT:-/dev/tty.usbserial*}"
BAUD="${BAUD:-460800}"

# ESP-IDF 환경 로드
export_idf() {
    if [[ -z "${IDF_PATH:-}" ]]; then
        local idf_export="$HOME/esp/esp-idf/export.sh"
        if [[ -f "$idf_export" ]]; then
            source "$idf_export" > /dev/null 2>&1
        else
            echo "[ERROR] ESP-IDF not found. Set IDF_PATH or install ESP-IDF."
            exit 1
        fi
    fi
    export ESP_MATTER_PATH="${ESP_MATTER_PATH:-$HOME/esp/esp-matter}"
}

# 포트 자동 감지
detect_port() {
    if [[ "$PORT" == *"*"* ]]; then
        local detected
        detected=$(ls /dev/tty.usbserial* /dev/tty.usbmodem* /dev/cu.usbserial* 2>/dev/null | head -1 || true)
        if [[ -n "$detected" ]]; then
            PORT="$detected"
        else
            echo "[WARN] USB 시리얼 포트를 찾을 수 없습니다. -p 옵션으로 지정하세요."
            return 1
        fi
    fi
    echo "[INFO] Port: $PORT"
}

usage() {
    cat <<EOF
사용법: $(basename "$0") [명령] [옵션]

명령:
  build       빌드만 수행
  flash       빌드 + 플래시
  monitor     시리얼 모니터
  run         빌드 + 플래시 + 모니터 (기본값)
  clean       빌드 디렉토리 정리
  fullclean   빌드 디렉토리 완전 삭제
  erase       플래시 전체 삭제
  size        바이너리 크기 분석
  menuconfig  SDK 설정 메뉴

옵션:
  -p PORT     시리얼 포트 (기본: 자동 감지)
  -b BAUD     전송 속도 (기본: 460800)
  -j JOBS     병렬 빌드 수 (기본: CPU 코어 수)
  -h          도움말
EOF
}

cmd_build() {
    echo "=== 빌드 시작 ==="
    cd "$PROJECT_DIR"
    idf.py -DIDF_TARGET="$TARGET" build
    echo "=== 빌드 완료 ==="
    idf.py size
}

cmd_flash() {
    detect_port || exit 1
    echo "=== 플래시 시작 ($PORT @ $BAUD) ==="
    cd "$PROJECT_DIR"
    idf.py -DIDF_TARGET="$TARGET" -p "$PORT" -b "$BAUD" flash
    echo "=== 플래시 완료 ==="
}

cmd_monitor() {
    detect_port || exit 1
    echo "=== 모니터 시작 (종료: Ctrl+]) ==="
    cd "$PROJECT_DIR"
    idf.py -p "$PORT" monitor
}

cmd_run() {
    detect_port || exit 1
    echo "=== 빌드 + 플래시 + 모니터 ==="
    cd "$PROJECT_DIR"
    idf.py -DIDF_TARGET="$TARGET" -p "$PORT" -b "$BAUD" build flash monitor
}

cmd_clean() {
    echo "=== 빌드 정리 ==="
    cd "$PROJECT_DIR"
    idf.py fullclean
    echo "=== 정리 완료 ==="
}

cmd_fullclean() {
    echo "=== 빌드 디렉토리 완전 삭제 ==="
    cd "$PROJECT_DIR"
    rm -rf build
    echo "=== 삭제 완료 ==="
}

cmd_erase() {
    detect_port || exit 1
    echo "=== 플래시 전체 삭제 ($PORT) ==="
    cd "$PROJECT_DIR"
    idf.py -p "$PORT" erase-flash
    echo "=== 삭제 완료 ==="
}

cmd_size() {
    cd "$PROJECT_DIR"
    idf.py size
    idf.py size-components
}

cmd_menuconfig() {
    cd "$PROJECT_DIR"
    idf.py menuconfig
}

# --- main ---
export_idf

COMMAND="${1:-run}"
shift 2>/dev/null || true

while getopts "p:b:j:h" opt; do
    case $opt in
        p) PORT="$OPTARG" ;;
        b) BAUD="$OPTARG" ;;
        j) export MAKEFLAGS="-j$OPTARG" ;;
        h) usage; exit 0 ;;
        *) usage; exit 1 ;;
    esac
done

case "$COMMAND" in
    build)      cmd_build ;;
    flash)      cmd_flash ;;
    monitor)    cmd_monitor ;;
    run)        cmd_run ;;
    clean)      cmd_clean ;;
    fullclean)  cmd_fullclean ;;
    erase)      cmd_erase ;;
    size)       cmd_size ;;
    menuconfig) cmd_menuconfig ;;
    -h|--help)  usage ;;
    *)          echo "[ERROR] 알 수 없는 명령: $COMMAND"; usage; exit 1 ;;
esac
