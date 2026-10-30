#!/bin/bash
# Build mtkloader.bin. MMU off means device memory: no SIMD, strict alignment, no builtins.
# Extra arguments go to the compiler (e.g. -DXNU_WDT_SECONDS=0).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

LLD="${LLD:-zig ld.lld}"
CFLAGS="--target=aarch64-linux-gnu -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -mgeneral-regs-only -mstrict-align -Os -Wall -Wextra"

# Two headers are made here and never kept in git (.gitignore):
#  touchfw.h   the touch controller's firmware, out of YOUR phone's hdl_firmware.img (pull-firmware.sh puts it
#              in fw-touch/): it is the vendor's, and the controller loses it at every reset, so the loader
#              carries it (tcm.h)
#  font8x16.h  the loader's 8x16 text font, rasterized on a Mac by tools/mkfont8x16.swift (from a system font,
#              which is why the bitmap is not in the repository either)
TOUCH_IMG="${TOUCH_IMG:-$ROOT/fw-touch/hdl_firmware.img}"
if [ ! -f "$HERE/touchfw.h" ]; then
	if [ -f "$TOUCH_IMG" ]; then
		python3 "$ROOT/tools/mktouchfw.py" "$TOUCH_IMG" ROMBOOT_APP_CODE "$HERE/touchfw.h"
	else
		# A32 brings its touch controller (ST Microelectronics FTS) up over I2C, not the
		# OmniVision TCM over SPI that tcm.h speaks, so no vendor image is needed to build.
		echo "mtkloader: no $TOUCH_IMG; writing an empty touchfw.h (A32 touch is I2C FTS)" >&2
		cat > "$HERE/touchfw.h" <<'EOF'
/* Placeholder: the A32's touch controller is an ST FTS on I2C6, not a TCM/SPI device.
 * Nothing must load this: TOUCH_PROBE stays 0 until a real FTS path replaces tcm.h. */
#ifndef TOUCHFW_H
#define TOUCHFW_H

#include <stdint.h>

#define TOUCH_FW_SIZE 1u
#define TOUCH_FW_FLAG 0x0000u

static const uint8_t touch_fw[TOUCH_FW_SIZE] = { 0x00 };

#endif /* TOUCHFW_H */
EOF
	fi
fi
[ -f "$HERE/font8x16.h" ] || swift "$HERE/../tools/mkfont8x16.swift" > "$HERE/font8x16.h"

clang $CFLAGS "$@" -c "$HERE/start.S" -o "$HERE/start.o"
clang $CFLAGS "$@" -c "$HERE/mtkloader.c" -o "$HERE/mtkloader.o"
$LLD -T "$HERE/link.ld" --oformat binary "$HERE/start.o" "$HERE/mtkloader.o" -o "$HERE/mtkloader.bin"
ls -la "$HERE/mtkloader.bin"
