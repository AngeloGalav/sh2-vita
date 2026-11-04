#!/bin/sh
# Builds sh2vita.vpk (and sh2vita.elf / eboot.bin) in the pinned VitaSDK container.
#
#   port/vita/build.sh           build
#   port/vita/build.sh clean     any other make arguments are passed through
#
# Needs Docker and git. The image carries the toolchain, vitaShaRK and math-neon. vitaGL is built
# here from a pinned commit (deps/, kept across make clean) with NO_DEBUG (no per-call error
# checks) and NO_SPLASHSCREEN (no boot splash, whose own thread on the display queue Vita3K never
# got past); vitaGL's riskier speed hacks ("may cause crashes", its README) are left out.
set -e
cd "$(dirname "$0")"
IMAGE=${SH2VITA_BUILD_IMAGE:-vitasdk/vitasdk@sha256:acd15285e1ae473e925c6f83fa41e2de45eb8b9e8ba89be7df03e194c35c11d3}
JOBS=${SH2VITA_BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}
VITAGL_REV=c2b3b8ab9cf7a3b2a12671fffc09dcb7cd68c56c
VITAGL_FLAGS="NO_DEBUG=1 NO_SPLASHSCREEN=1"
# SH2VITA_STORE_DEPTH=1: each depth buffer loaded and stored with its scene, which the half-size
# fog test (SELECT + L) needs to read the depth as a texture; its cost is still to be measured
[ "${SH2VITA_STORE_DEPTH:-0}" = "1" ] && VITAGL_FLAGS="$VITAGL_FLAGS STORE_DEPTH_STENCIL=1"
run() {
	docker run --rm -v "$PWD/../..":/work -w /work/port/vita -e HOME=/tmp --user "$(id -u):$(id -g)" "$IMAGE" "$@"
}

DEP=deps/vitagl
if [ "$(cat "$DEP/stamp" 2>/dev/null)" != "$VITAGL_REV $VITAGL_FLAGS" ]; then
	rm -rf "$DEP"
	mkdir -p "$DEP/include"
	git clone -q https://github.com/Rinnegatamante/vitaGL.git "$DEP/src"
	git -C "$DEP/src" checkout -q "$VITAGL_REV"
	run make -C "$DEP/src" -j"$JOBS" $VITAGL_FLAGS
	cp "$DEP/src/libvitaGL.a" "$DEP/"
	cp "$DEP/src/source/vitaGL.h" "$DEP/include/"
	echo "$VITAGL_REV $VITAGL_FLAGS" > "$DEP/stamp"
fi
run make -j"$JOBS" "$@"
