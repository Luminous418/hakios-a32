#!/bin/bash
# Build mtkloader and pack it in front of Android 13's kernel in a 32 MiB boot image.
#
#   ./mkimg.sh [extra compiler flags...]
#
# BASE  boot image to chain to (required: pass your own, e.g. a Magisk-patched boot.img)
# OUT   resulting boot image (default: mtkloader-a13.img, next to this script)
#
# Flash with:
#   adb push OUT /data/local/tmp/boot.img
#   adb shell 'su -c "dd if=/data/local/tmp/boot.img of=/dev/block/by-name/boot; sync"'
#   adb reboot
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
BASE="${BASE:?mkimg.sh: pass BASE=/path/to/boot.img}"
OUT="${OUT:-$HERE/mtkloader-a13.img}"

"$HERE/build.sh" "$@"
python3 "$HERE/../tools/chainboot.py" "$BASE" "$HERE/mtkloader.bin" "$OUT"
ls -lh "$HERE/mtkloader.bin" "$OUT"
sha256sum "$OUT"
