#!/bin/sh
# Cross-build n72-ui statically for the iPod touch 2G (ARM1176; Alpine armhf:
# VFP, hard float, musl). Run in the n72-alpine container:
#   sh n72-ui/build.sh  (from the workspace root)
# Kernel uapi headers: linux-build-alpine/usr/include (`make O=../linux-build-alpine ARCH=arm headers`).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
UI=$(cd "$(dirname "$0")" && pwd)
# GLES=dir: another n72-gles checkout (its build/libGLESv1_CM.a and include/)
GLES=${GLES:-$ROOT/n72-gles}
LVGL="$ROOT/lvgl-src/lvgl"
OUT="$ROOT/$(basename "$UI")-build"
CROSS=${CROSS:-armv6-alpine-linux-musleabihf-}
CC=${CROSS}gcc
PG=
# PROFILE=1: gprof build (n72-ui-build-prof/, not stripped; gmon.out on a clean exit, e.g. --bench)
[ "$PROFILE" = 1 ] && { OUT="$ROOT/$(basename "$UI")-build-prof"; PG=-pg; }
# link-time optimisation: LVGL's drawing inlined across files (text 11-19% faster on the device)
LTO=-flto=auto
# OPT: the optimisation level (-O2; -O3 to compare)
OPT=${OPT:--O2}
# Profile-guided build (pgo/README):
#   PGO_GEN=1 [PGO_RT=libclang_rt.profile-armhf.a]  instrumented (n72-ui-build-pgogen/)
#   otherwise pgo/n72ui.profdata is used when it is there (NOPGO=1: not)
PGOC= PGOL=
if [ "$PGO_GEN" = 1 ]; then
	OUT="$ROOT/$(basename "$UI")-build-pgogen"
	PGO_RT=${PGO_RT:-$ROOT/n72-gles/pgo-rt/libclang_rt.profile-armhf.a}
	PGOC="-fprofile-generate -DN72_PGO_GEN"
	PGOL="-Wl,-u,__llvm_profile_runtime $PGO_RT"
elif [ -f "$UI/pgo/n72ui.profdata" ] && [ "$NOPGO" != 1 ]; then
	PGOC="-fprofile-use=$UI/pgo/n72ui.profdata -mllvm -hot-cold-split=true -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin"
	PGOL="-Wl,-mllvm,-hot-cold-split=true -Wl,-z,keep-text-section-prefix"
fi
# (comparison builds beside the usual one: n72-ui-build-O3, -nopgo)
[ "$OPT" != -O2 ] && OUT="$OUT-${OPT#-}"
[ "$NOPGO" = 1 ] && OUT="$OUT-nopgo"
CFLAGS="$PG $LTO $OPT $PGOC -g -idirafter $ROOT/linux-build-alpine/usr/include -idirafter $ROOT/linux-s5l87xx/include/uapi -mcpu=arm1176jzf-s -mfpu=vfpv2 -mfloat-abi=hard -ffunction-sections -fdata-sections -DLV_CONF_INCLUDE_SIMPLE -I$UI -I$ROOT/lvgl-src -I$GLES/include"
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
# (with LTO the instrumentation and the profile go in at compile time; the
# link only adds the runtime, or the layout by the profile)
$CC $PG $LTO $OPT -static -Wl,--gc-sections $PGOL -o "$OUT/n72-ui" "$OUT/obj/main.o" "$OUT/obj/mbx_gpu.o" $OBJS $GLES/build/libGLESv1_CM.a -lm -lpthread
cp "$OUT/n72-ui" "$OUT/n72-ui.debug"	# unstripped, for crash reports (addr2line)
[ -n "$PG" ] || ${CROSS}strip "$OUT/n72-ui"
ls -l "$OUT/n72-ui"
