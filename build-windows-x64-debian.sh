#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
INVOKE_DIR=$(pwd)
BUILD_DIR=${BUILD_DIR:-"$INVOKE_DIR/build-msvc-x64-release"}

MSVC_WINE_DATA_ROOT=${MSVC_WINE_DATA_ROOT:-"$BUILD_DIR/msvc-wine"}
MSVC_WINE_REPO_DIR=${MSVC_WINE_REPO_DIR:-"$BUILD_DIR/msvc-wine-src"}
MSVC_WINE_REPOSITORY=${MSVC_WINE_REPOSITORY:-https://github.com/mstorsjo/msvc-wine.git}
MSVC_ROOT=${MSVC_ROOT:-"$MSVC_WINE_DATA_ROOT/msvc"}
MSVC_CACHE_DIR=${MSVC_CACHE_DIR:-"$MSVC_WINE_DATA_ROOT/cache"}

VCPKG_ROOT=${VCPKG_ROOT:-"$BUILD_DIR/vcpkg"}
VCPKG_REPOSITORY=${VCPKG_REPOSITORY:-https://github.com/microsoft/vcpkg.git}
GENERATED_TRIPLETS_DIR=${GENERATED_TRIPLETS_DIR:-"$BUILD_DIR/triplets"}
VCPKG_INSTALL_ROOT=${VCPKG_INSTALL_ROOT:-"$BUILD_DIR/vcpkg_installed"}

WINEPREFIX_DIR=${WINEPREFIX:-"$MSVC_WINE_DATA_ROOT/wineprefix"}
XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp/wine-runtime}
TRIPLET=${TRIPLET:-x64-windows-static-msvc-wine}
OVERLAY_TRIPLETS=${OVERLAY_TRIPLETS:-"$GENERATED_TRIPLETS_DIR"}
WIN32_RESOURCES=${WIN32_RESOURCES:-.resources/images/vtm.rc}

LOCK_FILE=${LOCK_FILE:-"$BUILD_DIR/.build.lock"}

FORCE_CONFIGURE=${FORCE_CONFIGURE:-0}
SKIP_VCPKG_INSTALL=${SKIP_VCPKG_INSTALL:-0}
SKIP_APT_INSTALL=${SKIP_APT_INSTALL:-0}

run_as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    else
        sudo "$@"
    fi
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1
}

manifest_builtin_baseline() {
    python3 - "$ROOT_DIR/vcpkg.json" <<'PY'
import json
import sys

with open(sys.argv[1], 'r', encoding='utf-8') as f:
    print(json.load(f).get('builtin-baseline', ''))
PY
}

ensure_git_checkout() {
    local repo_url=$1
    local repo_dir=$2

    if [ -d "$repo_dir/.git" ]; then
        return
    fi

    if [ -e "$repo_dir" ]; then
        rm -rf "$repo_dir"
    fi

    git clone --depth=1 "$repo_url" "$repo_dir"
}

ensure_vcpkg_baseline_commit() {
    local baseline=$1

    if [ -z "$baseline" ]; then
        return
    fi

    if git -C "$VCPKG_ROOT" cat-file -e "$baseline^{commit}" >/dev/null 2>&1; then
        return
    fi

    git -C "$VCPKG_ROOT" fetch --depth=1 origin "$baseline" || \
        git -C "$VCPKG_ROOT" fetch --unshallow origin
}

generate_triplet_files() {
    local triplet_dir=$1
    local meson_cross_file=$triplet_dir/meson-windows-x64-wine.ini
    local triplet_file=$triplet_dir/$TRIPLET.cmake

    mkdir -p "$triplet_dir"

    cat >"$meson_cross_file" <<EOF
[binaries]
c = ['$MSVC_ROOT/bin/x64/cl.exe']
cpp = ['$MSVC_ROOT/bin/x64/cl.exe']
c_ld = ['$MSVC_ROOT/bin/x64/link']
cpp_ld = ['$MSVC_ROOT/bin/x64/link']
ar = ['$MSVC_ROOT/bin/x64/lib', '/machine:x64', '/nologo']
windres = ['$MSVC_ROOT/bin/x64/rc']
mt = ['$MSVC_ROOT/bin/x64/mt']
exe_wrapper = ['wine']

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

    cat >"$triplet_file" <<EOF
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE \${VCPKG_ROOT_DIR}/scripts/toolchains/windows.cmake)
set(VCPKG_MESON_CROSS_FILE $meson_cross_file)

set(ENV{CC} cl.exe)
set(ENV{CXX} cl.exe)
set(ENV{PATH} "$MSVC_ROOT/bin/x64:\$ENV{PATH}")
EOF
}

ensure_debian_build_deps() {
    if [ "$SKIP_APT_INSTALL" = "1" ]; then
        return
    fi

    if need_cmd git \
        && need_cmd cmake \
        && need_cmd ninja \
        && need_cmd g++ \
        && need_cmd make \
        && need_cmd python3 \
        && need_cmd msiextract \
        && need_cmd cabextract \
        && need_cmd wine; then
        return
    fi

    run_as_root apt-get update
    run_as_root apt-get install -y \
        git \
        cmake \
        ninja-build \
        zip \
        unzip \
        pkgconf \
        g++ \
        make \
        wine64 \
        python3 \
        msitools \
        cabextract \
        ca-certificates \
        winbind
}

mkdir -p "$BUILD_DIR"
exec 9>"$LOCK_FILE"
if ! flock -n 9; then
    echo "Another build is already running for $BUILD_DIR" >&2
    exit 1
fi

ensure_debian_build_deps

ensure_git_checkout "$MSVC_WINE_REPOSITORY" "$MSVC_WINE_REPO_DIR"

if [ ! -x "$MSVC_ROOT/bin/x64/cl" ]; then
    mkdir -p "$MSVC_ROOT" "$MSVC_CACHE_DIR" "$WINEPREFIX_DIR" "$XDG_RUNTIME_DIR"
    WINEPREFIX="$WINEPREFIX_DIR" wineboot --init >/dev/null 2>&1 || true
    python3 "$MSVC_WINE_REPO_DIR/vsdownload.py" \
        --accept-license \
        --architecture x64 \
        --cache "$MSVC_CACHE_DIR" \
        --dest "$MSVC_ROOT"
    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" WINEPREFIX="$WINEPREFIX_DIR" \
        "$MSVC_WINE_REPO_DIR/install.sh" "$MSVC_ROOT"
fi

export XDG_RUNTIME_DIR
export WINEPREFIX="$WINEPREFIX_DIR"
export PATH="$MSVC_ROOT/bin/x64:$PATH"
export VS170COMNTOOLS="$MSVC_ROOT/Common7/Tools/"
export VCPKG_VISUAL_STUDIO_PATH="$MSVC_ROOT"
export WIN32_RESOURCES

mkdir -p "$XDG_RUNTIME_DIR"
wineserver -k >/dev/null 2>&1 || true
generate_triplet_files "$GENERATED_TRIPLETS_DIR"

ensure_git_checkout "$VCPKG_REPOSITORY" "$VCPKG_ROOT"
ensure_vcpkg_baseline_commit "$(manifest_builtin_baseline)"

if [ ! -x "$VCPKG_ROOT/vcpkg" ]; then
    "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
fi

if [ "$SKIP_VCPKG_INSTALL" != "1" ]; then
    "$VCPKG_ROOT/vcpkg" install \
        --triplet "$TRIPLET" \
        --vcpkg-root "$VCPKG_ROOT" \
        --x-manifest-root "$ROOT_DIR" \
        --x-install-root "$VCPKG_INSTALL_ROOT" \
        --overlay-triplets="$OVERLAY_TRIPLETS"
fi

if [ "$FORCE_CONFIGURE" = "1" ] || [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_SYSTEM_NAME=Windows \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=cl \
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_TARGET_TRIPLET="$TRIPLET" \
        -DVCPKG_OVERLAY_TRIPLETS="$OVERLAY_TRIPLETS" \
        -DVCPKG_MANIFEST_DIR="$ROOT_DIR" \
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded \
        -DCMAKE_EXE_LINKER_FLAGS_RELEASE="/DEBUG /OPT:REF /OPT:ICF" \
        -DCMAKE_CXX_FLAGS_RELEASE="/MT /O2 /DNDEBUG /Zi /Zc:preprocessor /W4 /EHsc /bigobj /utf-8 /Zc:preprocessor" \
        -DVCPKG_APPLOCAL_DEPS=OFF
fi

cmake --build "$BUILD_DIR" -v

echo "Build complete:"
echo "  $BUILD_DIR/vtm.exe"
