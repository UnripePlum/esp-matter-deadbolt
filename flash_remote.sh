#!/bin/bash
# 로컬 PC에서 실행: ESP-IDF 없이 esptool.py만으로 플래싱
# 사용법: ./flash_remote.sh [PORT]
#
# 사전 준비:
#   pip install esptool
#
# 바이너리 파일은 GitHub Actions 아티팩트 또는 릴리즈에서 다운로드
# 이 스크립트와 같은 디렉토리에 다음 파일이 필요:
#   bootloader.bin, partition-table.bin, ota_data_initial.bin, esp-matter-deadbolt.bin

set -euo pipefail

PORT="${1:-}"
BAUD="${BAUD:-460800}"

# 포트 자동 감지
if [[ -z "$PORT" ]]; then
    if [[ "$(uname)" == "Darwin" ]]; then
        PORT=$(ls /dev/cu.usbserial* /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
    else
        PORT=$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -1 || true)
    fi
    if [[ -z "$PORT" ]]; then
        echo "[ERROR] USB 시리얼 포트를 찾을 수 없습니다."
        echo "사용법: $0 <PORT>"
        echo "예시: $0 /dev/ttyUSB0"
        exit 1
    fi
fi

# 바이너리 파일 확인
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
for f in bootloader.bin partition-table.bin ota_data_initial.bin esp-matter-deadbolt.bin; do
    if [[ ! -f "$SCRIPT_DIR/$f" ]]; then
        echo "[ERROR] $f 파일이 없습니다."
        echo "GitHub 릴리즈에서 다운로드하세요:"
        echo "  https://github.com/UnripePlum/esp-matter-deadbolt/releases"
        exit 1
    fi
done

echo "=== ESP32-S3 도어락 플래싱 ==="
echo "  Port: $PORT"
echo "  Baud: $BAUD"
echo ""

esptool.py --chip esp32s3 -p "$PORT" -b "$BAUD" \
    --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m \
    0x0     "$SCRIPT_DIR/bootloader.bin" \
    0xc000  "$SCRIPT_DIR/partition-table.bin" \
    0x1d000 "$SCRIPT_DIR/ota_data_initial.bin" \
    0x20000 "$SCRIPT_DIR/esp-matter-deadbolt.bin"

echo ""
echo "=== 플래싱 완료 ==="
echo "모니터: python -m serial.tools.miniterm $PORT 115200"
