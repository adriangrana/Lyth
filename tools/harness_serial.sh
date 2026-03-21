#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LOG_DIR="$ROOT_DIR/build"
LOG_FILE="$LOG_DIR/serial_harness.log"

mkdir -p "$LOG_DIR"

cd "$ROOT_DIR"

echo "[harness] build + iso"
make clean compile create-iso >/dev/null

echo "[harness] running QEMU (serial capture to file, 30s timeout)"
rm -f "$LOG_FILE"
set +e
timeout 30s qemu-system-i386 \
  -boot d \
  -cdrom dist/lyth.iso \
  -m 128 \
  -no-reboot -no-shutdown \
  -display none \
  -serial "file:$LOG_FILE" \
  -monitor none \
  >/dev/null 2>&1
QEMU_RC=$?
set -e

if [[ $QEMU_RC -ne 0 && $QEMU_RC -ne 124 ]]; then
  echo "[harness] qemu failed rc=$QEMU_RC"
  tail -n 80 "$LOG_FILE"
  exit 1
fi

echo "[harness] validating output"
if ! grep -q "\[TEST\] boot" "$LOG_FILE"; then
  echo "[harness] missing boot test output"
  tail -n 120 "$LOG_FILE"
  exit 1
fi

if ! grep -q "ALL PASS" "$LOG_FILE"; then
  echo "[harness] boot tests reported failures"
  tail -n 120 "$LOG_FILE"
  exit 1
fi

echo "[harness] PASS (serial log: $LOG_FILE)"
echo "[harness] To stress exec manually in shell: repeat 20 exec /bin/tu_elf ..."
