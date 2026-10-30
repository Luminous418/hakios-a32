#!/bin/bash
# Build a32diag.img: a32diag packed in front of Android's kernel in the A32's boot image.
#
#   ANDROID_BOOT=...  the stock/Magisk boot.img to chain to (no default: pass your own)
#
# Output: a32diag.bin (bare-metal) and a32diag.img (Android boot image v2, <= 32 MB).
# Both are generated and git ignores them.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
LLD="${LLD:-zig ld.lld}"
CFLAGS="--target=aarch64-linux-gnu -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -mgeneral-regs-only -mstrict-align -Os -Wall -Wextra -I$HERE"

clang $CFLAGS -c "$HERE/fbdiag_start.S" -o "$HERE/fbdiag_start.o"
clang $CFLAGS -c "$HERE/a32diag.c" -o "$HERE/a32diag.o"
$LLD -T "$HERE/link.ld" --oformat binary "$HERE/fbdiag_start.o" "$HERE/a32diag.o" -o "$HERE/a32diag.bin"

BOOT="${ANDROID_BOOT:?build.sh: pass ANDROID_BOOT=/path/to/boot.img}"
[ -f "$BOOT" ] || { echo "build.sh: no boot image at $BOOT" >&2; exit 1; }
python3 "$ROOT/tools/chainboot.py" "$BOOT" "$HERE/a32diag.bin" "$HERE/a32diag.img"
ls -lh "$HERE/a32diag.bin" "$HERE/a32diag.img"
