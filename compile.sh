#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

usage() {
  cat <<'EOF'
Usage: ./compile.sh [options]

Options:
  --ndk-dir PATH     Path to Android NDK (otherwise prompted interactively)
  --build-dir PATH   Meson build directory (default: build-android-aarch64)
  --reconfigure      Force meson --reconfigure before compiling
  -h, --help         Show this help message
EOF
}

RECONFIGURE=0
NDK_DIR=""
BUILD_DIR="build-android-aarch64"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --reconfigure)
      RECONFIGURE=1
      shift
      ;;
    --ndk-dir)
      [[ $# -ge 2 ]] || { echo "Error: --ndk-dir requires a path" >&2; exit 1; }
      NDK_DIR="$2"
      shift 2
      ;;
    --ndk-dir=*)
      NDK_DIR="${1#*=}"
      shift
      ;;
    --build-dir)
      [[ $# -ge 2 ]] || { echo "Error: --build-dir requires a path" >&2; exit 1; }
      BUILD_DIR="$2"
      shift 2
      ;;
    --build-dir=*)
      BUILD_DIR="${1#*=}"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

DEFAULT_NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [[ -z "$NDK_DIR" ]]; then
  if [[ -n "$DEFAULT_NDK" ]]; then
    read -r -p "Enter Android NDK directory [$DEFAULT_NDK]: " INPUT_NDK
    NDK_DIR="${INPUT_NDK:-$DEFAULT_NDK}"
  else
    read -r -p "Enter Android NDK directory: " NDK_DIR
  fi
fi

# Expand leading ~/ if entered interactively.
if [[ "$NDK_DIR" == "~/"* ]]; then
  NDK_DIR="$HOME/${NDK_DIR#\~/}"
elif [[ "$NDK_DIR" == "~" ]]; then
  NDK_DIR="$HOME"
fi
NDK_DIR="${NDK_DIR%/}"

if [[ -z "$NDK_DIR" || ! -d "$NDK_DIR" ]]; then
  echo "Error: Android NDK directory not found: '$NDK_DIR'" >&2
  exit 1
fi

NDK_DIR="$(cd "$NDK_DIR" && pwd)"

PREBUILT_DIR=""
for cand in "$NDK_DIR"/toolchains/llvm/prebuilt/*; do
  if [[ -x "$cand/bin/aarch64-linux-android34-clang" ]]; then
    PREBUILT_DIR="$cand"
    break
  fi
done

if [[ -z "$PREBUILT_DIR" ]]; then
  echo "Error: could not find toolchains/llvm/prebuilt/*/bin/aarch64-linux-android34-clang under '$NDK_DIR'" >&2
  exit 1
fi

if [[ ! -x "$PREBUILT_DIR/bin/aarch64-linux-android-strip" && ! -x "$PREBUILT_DIR/bin/llvm-strip" ]]; then
  echo "Error: NDK strip binary not found in '$PREBUILT_DIR/bin'" >&2
  exit 1
fi

STRIP_BIN="$PREBUILT_DIR/bin/aarch64-linux-android-strip"
[[ -x "$STRIP_BIN" ]] || STRIP_BIN="$PREBUILT_DIR/bin/llvm-strip"

mkdir -p "$BUILD_DIR"

USE_CCACHE=0
if command -v ccache >/dev/null 2>&1; then
  USE_CCACHE=1
fi

fmt_compiler() {
  if [[ "$USE_CCACHE" -eq 1 ]]; then
    printf "['ccache', '%s']" "$1"
  else
    printf "'%s'" "$1"
  fi
}

fmt_cpp() {
  local bin="$1"
  shift
  local extra=""
  for flag in "$@"; do
    extra+=", '$flag'"
  done
  if [[ "$USE_CCACHE" -eq 1 ]]; then
    printf "['ccache', '%s'%s]" "$bin" "$extra"
  else
    printf "['%s'%s]" "$bin" "$extra"
  fi
}

CROSS_FILE="$BUILD_DIR/.cross-android-aarch64.txt"
NATIVE_FILE="$BUILD_DIR/.native.txt"
NDK_STAMP="$BUILD_DIR/.ndk-path"

PKG_CONFIG_PATH_VAL="$NDK_DIR/pkg-config"
if command -v pkg-config >/dev/null 2>&1; then
  PKG_CFG_BIN="$(command -v pkg-config)"
else
  PKG_CFG_BIN="/usr/bin/pkg-config"
fi

cat > "$CROSS_FILE" <<EOF
[constants]
ndk_path = '$NDK_DIR'
bin_path = '$PREBUILT_DIR/bin'

[binaries]
ar = bin_path / 'llvm-ar'
c = $(fmt_compiler "$PREBUILT_DIR/bin/aarch64-linux-android34-clang")
cpp = $(fmt_cpp "$PREBUILT_DIR/bin/aarch64-linux-android34-clang++" \
  '-fno-exceptions' '-fno-unwind-tables' '-fno-asynchronous-unwind-tables' \
  '--start-no-unused-arguments' '-static-libstdc++' '--end-no-unused-arguments')
c_ld = 'lld'
cpp_ld = 'lld'
strip = '$STRIP_BIN'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=$PKG_CONFIG_PATH_VAL', '$PKG_CFG_BIN']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
EOF

if command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
  HOST_CC="clang"
  HOST_CXX="clang++"
  HOST_AR="llvm-ar"
  HOST_STRIP="llvm-strip"
  HOST_LD="ld.lld"
else
  HOST_CC="gcc"
  HOST_CXX="g++"
  HOST_AR="ar"
  HOST_STRIP="strip"
  HOST_LD="bfd"
fi
command -v "$HOST_AR" >/dev/null 2>&1 || HOST_AR="ar"
command -v "$HOST_STRIP" >/dev/null 2>&1 || HOST_STRIP="strip"
command -v "$HOST_LD" >/dev/null 2>&1 || HOST_LD="bfd"

cat > "$NATIVE_FILE" <<EOF
[build_machine]
c = $(fmt_compiler "$HOST_CC")
cpp = $(fmt_compiler "$HOST_CXX")
ar = '$HOST_AR'
strip = '$HOST_STRIP'
c_ld = '$HOST_LD'
cpp_ld = '$HOST_LD'
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

PREV_NDK=""
if [[ -f "$NDK_STAMP" ]]; then
  PREV_NDK="$(cat "$NDK_STAMP")"
fi
if [[ -n "$PREV_NDK" && "$PREV_NDK" != "$NDK_DIR" ]]; then
  RECONFIGURE=1
fi

SETUP_ARGS=(
  --cross-file "$CROSS_FILE"
  --native-file "$NATIVE_FILE"
  -Dbuildtype=release
  -Dplatforms=android
  -Dplatform-sdk-version=34
  -Dandroid-stub=true
  -Dandroid-strict=false
  -Dandroid-libbacktrace=disabled
  -Degl=disabled
  -Dgallium-drivers=
  -Dvulkan-drivers=freedreno
  -Dfreedreno-kmds=kgsl
)

MESADIR="./"
if [[ ! -d "$BUILD_DIR/meson-private" ]]; then
  meson setup "$BUILD_DIR" "$MESADIR" "${SETUP_ARGS[@]}"
elif [[ "$RECONFIGURE" -eq 1 ]]; then
  meson setup "$BUILD_DIR" "$MESADIR" --reconfigure "${SETUP_ARGS[@]}"
fi

printf '%s' "$NDK_DIR" > "$NDK_STAMP"

meson compile -C "$BUILD_DIR"

echo "Built: $SCRIPT_DIR/$BUILD_DIR/src/freedreno/vulkan/libvulkan_freedreno.so"
