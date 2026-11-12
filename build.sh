#!/bin/sh
# Cross-build n72-ui statically for the iPod touch 2G (ARM1176; Alpine armhf:
# VFP, hard float, musl). Run in the n72-alpine container:
#   sh n72-ui/build.sh  (from the workspace root)
# Kernel uapi headers: linux-build-alpine/usr/include (`make O=../linux-build-alpine ARCH=arm headers`).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
UI="$ROOT/n72-ui"
LVGL="$ROOT/lvgl-src/lvgl"
OUT="$ROOT/n72-ui-build"
CROSS=${CROSS:-armv6-alpine-linux-musleabihf-}
CC=${CROSS}gcc
PG=
# PROFILE=1: gprof build (n72-ui-build-prof/, not stripped; gmon.out on a clean exit, e.g. --bench)
[ "$PROFILE" = 1 ] && { OUT="$ROOT/n72-ui-build-prof"; PG=-pg; }
# link-time optimisation: LVGL's drawing inlined across files (text 11-19% faster on the device)
LTO=-flto=auto
CFLAGS="$PG $LTO -O2 -g -idirafter $ROOT/linux-build-alpine/usr/include -idirafter $ROOT/linux-s5l87xx/include/uapi -mcpu=arm1176jzf-s -mfpu=vfpv2 -mfloat-abi=hard -ffunction-sections -fdata-sections -DLV_CONF_INCLUDE_SIMPLE -I$UI -I$ROOT/lvgl-src -I$ROOT/n72-gles/include"
mkdir -p "$OUT/obj"
# objects from another compiler or other flags (the old armel build) are not reused
if [ "$(cat "$OUT/obj/.flags" 2>/dev/null)" != "$CC $CFLAGS" ]; then
	rm -f "$OUT"/obj/*.o
	echo "$CC $CFLAGS" > "$OUT/obj/.flags"
fi
SRCS=$(find "$LVGL/src" -name '*.c')
obj() { echo "$OUT/obj/$(echo "${1#$LVGL/}" | tr '/' '_' | sed 's/\.c$/.o/')"; }
OBJS=$(for f in $SRCS; do obj "$f"; done)
for f in $SRCS; do
	o=$(obj "$f")
	if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "$UI/lv_conf.h" -nt "$o" ] || [ "$UI/n72_blend.h" -nt "$o" ]; then
		echo "$CC -c $f -o $o $CFLAGS"
	fi
done | xargs -P "$(nproc)" -I{} sh -c '{}'
$CC $CFLAGS -c "$UI/main.c" -o "$OUT/obj/main.o"
$CC $CFLAGS -c "$UI/mbx_gpu.c" -o "$OUT/obj/mbx_gpu.o"
$CC $PG $LTO -static -Wl,--gc-sections -o "$OUT/n72-ui" "$OUT/obj/main.o" "$OUT/obj/mbx_gpu.o" $OBJS $ROOT/n72-gles/build/libGLESv1_CM.a -lm -lpthread
cp "$OUT/n72-ui" "$OUT/n72-ui.debug"	# unstripped, for crash reports (addr2line)
[ -n "$PG" ] || ${CROSS}strip "$OUT/n72-ui"
ls -l "$OUT/n72-ui"
