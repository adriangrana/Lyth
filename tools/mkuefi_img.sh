#!/usr/bin/env bash
# Build a FAT12 image containing the EFI bootloader.
# Usage: mkuefi_img.sh <bootx64.efi> <grub.cfg> <output.img>
set -euo pipefail

EFI_BIN="$1"
GRUB_CFG="$2"
OUTPUT="$3"

if [[ ! -f "$EFI_BIN" ]]; then
    echo "error: EFI binary not found: $EFI_BIN" >&2
    exit 1
fi

# Size: EFI binary + grub.cfg + directory overhead.  Round up to 1440 KiB minimum.
EFI_SIZE=$(stat -c%s "$EFI_BIN")
NEED_KB=$(( (EFI_SIZE + 65536) / 1024 ))
if [[ $NEED_KB -lt 1440 ]]; then
    NEED_KB=1440
fi

dd if=/dev/zero of="$OUTPUT" bs=1K count="$NEED_KB" 2>/dev/null
mkfs.vfat -F 12 "$OUTPUT" >/dev/null 2>&1
mmd -i "$OUTPUT" ::/EFI
mmd -i "$OUTPUT" ::/EFI/BOOT
mcopy -i "$OUTPUT" "$EFI_BIN" ::/EFI/BOOT/BOOTX64.EFI
mmd -i "$OUTPUT" ::/boot
mmd -i "$OUTPUT" ::/boot/grub
mcopy -i "$OUTPUT" "$GRUB_CFG" ::/boot/grub/grub.cfg

echo "$OUTPUT (${NEED_KB} KiB)"
