#!/bin/sh
# Cross-build n72-ui statically for the iPod touch 2G (ARM1176; Alpine armhf:
# VFP, hard float, musl) on the Mac:
#   sh n72-ui/build.sh                 the cross gcc (n72-alpine/toolchain) -> n72-ui-build/
#   N72_CC=clang sh n72-ui/build.sh    Homebrew clang + lld -> n72-ui-build-clang/ (the build
#                                      pgo/n72ui.profdata, a clang profile, applies to)
# Kernel uapi headers: KHDR, linux-build-mac/usr/include (`sh n72-alpine/toolchain/kernel.sh headers`)
# or the last container build's linux-build-alpine/usr/include.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
UI=$(cd "$(dirname "$0")" && pwd)
# GLES=dir: another n72-gles checkout (its build/libn72mbxmem.a and include/)
GLES=${GLES:-$ROOT/n72-gles}
GLES=$(cd "$GLES" && pwd)	# (the build changes directory further down: no relative paths)
LVGL="$ROOT/lvgl-src/lvgl"
OUT="$ROOT/$(basename "$UI")-build"
CROSS=${CROSS:-armv6-alpine-linux-musleabihf-}
command -v "${CROSS}gcc" >/dev/null || . "$ROOT/n72-alpine/toolchain/env.sh"
N72_CC=${N72_CC:-gcc}
CC=${CROSS}$N72_CC
# what $CC really is (a "...-gcc" given through CROSS may be a clang wrapper)
case "$($CC --version 2>/dev/null)" in *clang*) KIND=clang ;; *) KIND=gcc ;; esac
[ "$KIND" = clang ] && [ "$N72_CC" = clang ] && OUT="$OUT-clang"
# n72-gles' MBX memory helper static library of the same compiler (build/
# for gcc, build-clang/ for N72_CC=clang)
GLESLIB=$GLES/build/libn72mbxmem.a
[ "$N72_CC" = clang ] && [ -f "$GLES/build-clang/libn72mbxmem.a" ] && GLESLIB=$GLES/build-clang/libn72mbxmem.a
KHDR=${KHDR:-$ROOT/linux-build-mac/usr/include}
[ -f "$KHDR/linux/types.h" ] || KHDR=$ROOT/linux-build-alpine/usr/include
KHDR=$(cd "$KHDR" && pwd)
[ -z "$PGO_RT" ] || case $PGO_RT in /*) ;; *) PGO_RT=$PWD/$PGO_RT ;; esac
PG=
# PROFILE=1: gprof build (n72-ui-build-prof/, not stripped; gmon.out on a clean exit)
[ "$PROFILE" = 1 ] && { OUT="$OUT-prof"; PG=-pg; }
DBG=
# N72_DEBUG=1: enable --bench, frame timing/trace logs, and GPU stat counters
[ "$N72_DEBUG" = 1 ] && { OUT="$OUT-debug"; DBG=-DN72_DEBUG; }
# link-time optimisation: LVGL's drawing inlined across files (text 11-19% faster on the device,
# measured with clang). NOLTO=1: without, for comparing (n72-ui-build*-nolto/).
LTO=-flto=auto
[ "$NOLTO" = 1 ] && LTO=
# OPT: the optimisation level (-O3 default with -flto=auto -fno-stack-protector; -O2 to compare).
# GCC 16 inherits Alpine's -fstack-protector-strong (~7% slower row/canvas text).
OPT=${OPT:--O3}
SSP=-fno-stack-protector
# Profile-guided build (pgo/README):
#   PGO_GEN=1 [PGO_RT=libclang_rt.profile-armhf.a]  instrumented (n72-ui-build*-pgogen/)
#   otherwise the profile is used when it is there (NOPGO=1: not):
#     clang  pgo/n72ui.profdata
#     gcc    pgo/gcc/*.gcda, the files the instrumented build wrote on the device
#            under PGO_DIR (/tmp/n72ui-pgo). When no gcc profile exists yet,
#            the gcc build skips profile guidance.
#            gcc names a .gcda after the object's path. The objects are
#            compiled from inside the output folder under relative names, with
#            -fprofile-prefix-path, so that the name is obj#NAME.gcda whichever
#            folder the build is in (the instrumented build's is another one
#            than the profiled build's). A missing file and a function changed
#            since the profile are warnings in the build's output: read them.
PGOC= PGOL=
PGO_DIR=${PGO_DIR:-/tmp/n72ui-pgo}
# clang names a static function in a profile by its source file; by the file's
# base name here, so that a profile holds wherever the tree is checked out
# (with the full path, the default, a profile taken from a build under /work
# gave the static functions nothing in a build under another path: silently)
PGOSF="-mllvm -static-func-full-module-prefix=false"
if [ "$PGO_GEN" = 1 ]; then
	OUT="$OUT-pgogen"
	if [ "$KIND" = clang ]; then
		PGO_RT=${PGO_RT:-$ROOT/n72-gles/pgo-rt/libclang_rt.profile-armhf.a}
		PGOC="-fprofile-generate $PGOSF -DN72_PGO_GEN"
		PGOL="-Wl,-u,__llvm_profile_runtime -Wl,-mllvm,-static-func-full-module-prefix=false $PGO_RT"
	else
		GCCGEN=1
		PGOL="-fprofile-generate=$PGO_DIR"
	fi
elif [ "$NOPGO" = 1 ]; then
	:
elif [ "$KIND" = clang ] && [ -f "$UI/pgo/n72ui.profdata" ]; then
	PGOC="-fprofile-use=$UI/pgo/n72ui.profdata $PGOSF -mllvm -hot-cold-split=true -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin"
	PGOL="-Wl,-mllvm,-hot-cold-split=true -Wl,-mllvm,-static-func-full-module-prefix=false -Wl,-z,keep-text-section-prefix"
elif [ "$KIND" = gcc ] && ls "$UI"/pgo/gcc/*.gcda >/dev/null 2>&1; then
	GCCUSE=1
fi
# (comparison builds beside the usual one: n72-ui-build-O2, -nopgo)
[ "$OPT" != -O3 ] && OUT="$OUT-${OPT#-}"
[ "$NOPGO" = 1 ] && OUT="$OUT-nopgo"
[ "$NOLTO" = 1 ] && OUT="$OUT-nolto"
# (gcc's profile flags name the output folder, which is final only here)
[ -n "$GCCGEN" ] && PGOC="-fprofile-generate=$PGO_DIR -fprofile-prefix-path=$OUT -fprofile-update=prefer-atomic -DN72_PGO_GEN"
[ -n "$GCCUSE" ] && PGOC="-fprofile-use=$UI/pgo/gcc -fprofile-prefix-path=$OUT -fprofile-correction -Wno-error=coverage-mismatch"
CFLAGS="$PG $LTO $OPT $SSP $PGOC $DBG -g -idirafter $KHDR -mcpu=arm1176jzf-s -mfpu=vfpv2 -mfloat-abi=hard -ffunction-sections -fdata-sections -DLV_CONF_INCLUDE_SIMPLE -I$UI -I$ROOT/lvgl-src -I$GLES/include"
mkdir -p "$OUT/obj"
# objects from another compiler or other flags (the old armel build) are not reused
if [ "$(cat "$OUT/obj/.flags" 2>/dev/null)" != "$CC $CFLAGS" ]; then
	rm -f "$OUT"/obj/*.o
	echo "$CC $CFLAGS" > "$OUT/obj/.flags"
fi
SRCS=$(find "$LVGL/src" -name '*.c')
cd "$OUT"	# objects by relative names (the gcc profile's file names, above)
obj() { echo "obj/$(echo "${1#$LVGL/}" | tr '/' '_' | sed 's/\.c$/.o/')"; }
OBJS=$(for f in $SRCS; do obj "$f"; done)
for f in $SRCS; do
	o=$(obj "$f")
	if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "$UI/lv_conf.h" -nt "$o" ] || [ "$UI/n72_blend.h" -nt "$o" ]; then
		echo "$CC -c $f -o $o $CFLAGS"
	fi
done | tr '\n' '\0' | xargs -0 -n 1 -P "$(nproc 2>/dev/null || sysctl -n hw.ncpu)" sh -c	# (not -I{}: BSD xargs caps that at 255 bytes)
# (the launcher's own sources warning-free under -Wall; LVGL's are not checked.
# snprintf cutting a long name short is meant: gcc reports it only without LTO)
WARN="-Wall -Wunused"; [ "$KIND" = gcc ] && WARN="$WARN -Wno-format-truncation"
# the launcher's own sources (audio.c, kbd.c, weather.c ... are #included by these).
# The link order matters on ARM1176 (16 KiB I-cache); verify text timings
# if reordering sources.
UISRCS="mbx_gpu mbx_draw mbx_kms main sleep ios_motion ui_video ui_turn ui_paint ui_logs ui_settings ui_bt ui_wifi ui_files ui_home ui_sys ui_nav ui_m3"
[ "$N72_DEBUG" = 1 ] && UISRCS="$UISRCS bench"
for s in $UISRCS; do
	$CC $CFLAGS $WARN -c "$UI/$s.c" -o obj/$s.o
done
UIOBJS=$(for s in $UISRCS; do echo obj/$s.o; done)
# (with LTO the instrumentation and the profile go in at compile time; the
# link only adds the runtime, or the layout by the profile)
$CC $PG $LTO $OPT $SSP -static -Wl,--gc-sections $PGOL -o "$OUT/n72-ui" $UIOBJS $OBJS $GLESLIB -lm -lpthread
cp "$OUT/n72-ui" "$OUT/n72-ui.debug"	# unstripped, for crash reports (addr2line)
[ -n "$PG" ] || ${CROSS}strip "$OUT/n72-ui"
ls -l "$OUT/n72-ui"
