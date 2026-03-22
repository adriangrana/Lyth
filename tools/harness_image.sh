#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LOG_DIR="$ROOT_DIR/build"
LOG_FILE="$LOG_DIR/image_harness.log"

mkdir -p "$LOG_DIR"

cd "$ROOT_DIR"

echo "[image-harness] build autotest iso"
make clean create-autotest-iso >/dev/null

echo "[image-harness] running QEMU headless (serial capture to file, 30s timeout)"
rm -f "$LOG_FILE"
set +e
timeout 30s qemu-system-i386 \
  -boot d \
  -cdrom dist/lyth-autotest.iso \
  -m 128 \
  -no-reboot -no-shutdown \
  -display none \
  -serial "file:$LOG_FILE" \
  -monitor none \
  >/dev/null 2>&1
QEMU_RC=$?
set -e

if [[ $QEMU_RC -ne 0 && $QEMU_RC -ne 124 ]]; then
  echo "[image-harness] qemu failed rc=$QEMU_RC"
  tail -n 120 "$LOG_FILE"
  exit 1
fi

echo "[image-harness] validating serial output"
grep -q "\[TEST\] boot: ALL PASS" "$LOG_FILE"
grep -q "\[AUTOTEST\] begin" "$LOG_FILE"
grep -q "Kernel hobby en C + ASM" "$LOG_FILE"
grep -q "mkdir - crea directorio: mkdir <ruta>" "$LOG_FILE"
grep -Eq '^4 $|^4[[:space:]]*$' "$LOG_FILE"
grep -q "shmread verificado correctamente" "$LOG_FILE"
grep -q "stackok terminado correctamente" "$LOG_FILE"
grep -q "stack guard page hit" "$LOG_FILE"
grep -q "\[AUTOTEST\] end" "$LOG_FILE"

echo "[image-harness] PASS (serial log: $LOG_FILE)"
