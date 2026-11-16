#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

usage() {
  cat <<'EOF'
Usage: ./compile_linx.sh [options]

Build Mesa Turnip (Adreno A5xx+ Vulkan) for Linux ARM64 targets:
  - Termux proot / chroot (Ubuntu/Debian using KGSL, outputs .deb package)
  - Mainline Linux / postmarketOS (Alpine/musl or glibc using DRM msm or kgsl,msm)

Options:
  --preset NAME         Target preset: proot | postmarketos | combined
  --kmds LIST           freedreno-kmds (e.g. kgsl, msm, kgsl,msm)
  --platforms LIST      Mesa platforms (e.g. x11,wayland, wayland, x11, or "" for headless)
  --headless            Alias for --platforms=""
  --sysroot PATH        ARM64 target rootfs for cross-compiling from x86_64
  --ubuntu-sysroot      Auto-create/use local Ubuntu ARM64 sysroot (.sysroot-ubuntu-<suite>-arm64)
  --ubuntu-suite SUITE  Ubuntu suite for --ubuntu-sysroot (default: jammy)
  --triple TRIPLE       Cross target triple (default: auto-detected from sysroot or host)
  --prefix PATH         Install prefix (default: /usr/local, or /usr with --system)
  --libdir PATH         Library subdir under prefix (default: auto-detected)
  --system              Use --prefix=/usr (system-wide install)
  --destdir PATH        Stage installed runtime files (plus turnip-env) into PATH
  --install             Run meson install directly onto the live system (native builds)
  --deb                 Build .deb package (default: enabled for proot/combined)
  --no-deb              Skip .deb package creation
  --deb-dir PATH        Output directory for .deb (default: ./out-deb)
  --pkg-name NAME       Debian package name (default: mesa-vulkan-freedreno)
  --pkg-version VER     Debian package version (default: <mesa-version>-turnip1)
  --build-dir PATH      Meson build directory (default: build-linux-aarch64-<preset>)
  --debug               Build debugoptimized with assertions enabled
  --jobs N              Parallel build jobs (default: nproc)
  --reconfigure         Force meson --reconfigure
  --clean               Remove build directory before configuring
  -h, --help            Show this help message
EOF
}

PRESET=""
KMDS=""
PLATFORMS=""
PLATFORMS_SET=0
SYSROOT=""
SYSROOT_SET=0
USE_UBUNTU_SYSROOT=0
UBUNTU_SUITE="jammy"
TRIPLE=""
PREFIX=""
LIBDIR=""
SYSTEM_INSTALL=0
DESTDIR=""
DO_INSTALL=0
BUILD_DEB=""
DEB_DIR="$SCRIPT_DIR/out-deb"
PKG_NAME="mesa-vulkan-freedreno"
MESA_VER="$(cat "$SCRIPT_DIR/VERSION" 2>/dev/null || echo 26.3.0)"
PKG_VERSION="${MESA_VER}-turnip1"
BUILD_DIR=""
BUILD_TYPE="release"
B_NDEBUG="true"
JOBS="$(nproc 2>/dev/null || echo 4)"
RECONFIGURE=0
CLEAN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset)         [[ $# -ge 2 ]] || { echo "Error: --preset requires a value" >&2; exit 1; }; PRESET="$2"; shift 2 ;;
    --preset=*)       PRESET="${1#*=}"; shift ;;
    --kmds)           [[ $# -ge 2 ]] || { echo "Error: --kmds requires a value" >&2; exit 1; }; KMDS="$2"; shift 2 ;;
    --kmds=*)         KMDS="${1#*=}"; shift ;;
    --platforms)      [[ $# -ge 2 ]] || { echo "Error: --platforms requires a value" >&2; exit 1; }; PLATFORMS="$2"; PLATFORMS_SET=1; shift 2 ;;
    --platforms=*)    PLATFORMS="${1#*=}"; PLATFORMS_SET=1; shift ;;
    --headless)       PLATFORMS=""; PLATFORMS_SET=1; shift ;;
    --sysroot)        [[ $# -ge 2 ]] || { echo "Error: --sysroot requires a path" >&2; exit 1; }; SYSROOT="$2"; SYSROOT_SET=1; shift 2 ;;
    --sysroot=*)      SYSROOT="${1#*=}"; SYSROOT_SET=1; shift ;;
    --ubuntu-sysroot) USE_UBUNTU_SYSROOT=1; shift ;;
    --ubuntu-suite)   [[ $# -ge 2 ]] || { echo "Error: --ubuntu-suite requires a value" >&2; exit 1; }; UBUNTU_SUITE="$2"; shift 2 ;;
    --ubuntu-suite=*) UBUNTU_SUITE="${1#*=}"; shift ;;
    --triple)         [[ $# -ge 2 ]] || { echo "Error: --triple requires a value" >&2; exit 1; }; TRIPLE="$2"; shift 2 ;;
    --triple=*)       TRIPLE="${1#*=}"; shift ;;
    --prefix)         [[ $# -ge 2 ]] || { echo "Error: --prefix requires a path" >&2; exit 1; }; PREFIX="$2"; shift 2 ;;
    --prefix=*)       PREFIX="${1#*=}"; shift ;;
    --libdir)         [[ $# -ge 2 ]] || { echo "Error: --libdir requires a path" >&2; exit 1; }; LIBDIR="$2"; shift 2 ;;
    --libdir=*)       LIBDIR="${1#*=}"; shift ;;
    --system)         SYSTEM_INSTALL=1; shift ;;
    --destdir)        [[ $# -ge 2 ]] || { echo "Error: --destdir requires a path" >&2; exit 1; }; DESTDIR="$2"; shift 2 ;;
    --destdir=*)      DESTDIR="${1#*=}"; shift ;;
    --install)        DO_INSTALL=1; shift ;;
    --deb)            BUILD_DEB=1; shift ;;
    --no-deb)         BUILD_DEB=0; shift ;;
    --deb-dir)        [[ $# -ge 2 ]] || { echo "Error: --deb-dir requires a path" >&2; exit 1; }; DEB_DIR="$2"; shift 2 ;;
    --deb-dir=*)      DEB_DIR="${1#*=}"; shift ;;
    --pkg-name)       [[ $# -ge 2 ]] || { echo "Error: --pkg-name requires a value" >&2; exit 1; }; PKG_NAME="$2"; shift 2 ;;
    --pkg-name=*)     PKG_NAME="${1#*=}"; shift ;;
    --pkg-version)    [[ $# -ge 2 ]] || { echo "Error: --pkg-version requires a value" >&2; exit 1; }; PKG_VERSION="$2"; shift 2 ;;
    --pkg-version=*)  PKG_VERSION="${1#*=}"; shift ;;
    --build-dir)      [[ $# -ge 2 ]] || { echo "Error: --build-dir requires a path" >&2; exit 1; }; BUILD_DIR="$2"; shift 2 ;;
    --build-dir=*)    BUILD_DIR="${1#*=}"; shift ;;
    --debug)          BUILD_TYPE="debugoptimized"; B_NDEBUG="false"; shift ;;
    --jobs)           [[ $# -ge 2 ]] || { echo "Error: --jobs requires a number" >&2; exit 1; }; JOBS="$2"; shift 2 ;;
    --jobs=*)         JOBS="${1#*=}"; shift ;;
    --reconfigure)    RECONFIGURE=1; shift ;;
    --clean)          CLEAN=1; shift ;;
    -h|--help)        usage; exit 0 ;;
    *)                echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
done

expand_path() {
  local p="$1"
  if [[ "$p" == "~/"* ]]; then
    p="$HOME/${p#\~/}"
  elif [[ "$p" == "~" ]]; then
    p="$HOME"
  fi
  printf '%s' "${p%/}"
}

bootstrap_ubuntu_sysroot() {
  local sysroot_dir="$1"
  local suite="$2"
  local platforms="$3"
  local kmds="$4"

  for tool in python3 ar tar; do
    if ! command -v "$tool" >/dev/null 2>&1; then
      echo "Error: '$tool' is required to bootstrap Ubuntu ARM64 sysroot." >&2
      exit 1
    fi
  done

  echo "==> Bootstrapping/updating Ubuntu ($suite) arm64 sysroot at $sysroot_dir..."
  mkdir -p "$sysroot_dir/.cache"

  python3 - "$sysroot_dir" "$suite" "$platforms" "$kmds" <<'PY'
import lzma
import os
import re
import subprocess
import sys
import urllib.request

sysroot, suite, platforms, kmds = sys.argv[1:5]
cache_dir = os.path.join(sysroot, ".cache")
stamp_file = os.path.join(sysroot, ".sysroot-features")
wanted_feats = sorted(set(
    ["base"]
    + (["x11"] if "x11" in platforms.split(",") else [])
    + (["wayland"] if "wayland" in platforms.split(",") else [])
    + (["drm"] if "msm" in kmds.split(",") else [])
))

if os.path.exists(stamp_file):
    with open(stamp_file, "r", encoding="utf-8") as f:
        have_feats = set(f.read().split())
    if set(wanted_feats).issubset(have_feats):
        print(f"    Ubuntu {suite} arm64 sysroot already has features: {', '.join(wanted_feats)}")
        sys.exit(0)

gcc_ver = "12" if suite == "jammy" else "13"
seeds = [
    "libc6", "libc6-dev", "linux-libc-dev",
    "libstdc++6", f"libstdc++-{gcc_ver}-dev",
    "libgcc-s1", f"libgcc-{gcc_ver}-dev",
    "zlib1g", "zlib1g-dev",
]
if "x11" in wanted_feats:
    seeds += [
        "libxcb1-dev", "libx11-dev", "libx11-xcb-dev",
        "libxcb-dri3-dev", "libxcb-present-dev", "libxcb-randr0-dev",
        "libxcb-shm0-dev", "libxcb-sync-dev", "libxcb-xfixes0-dev",
        "libxcb-keysyms1-dev", "libxshmfence-dev", "libxrandr-dev",
        "x11proto-dev",
    ]
if "wayland" in wanted_feats:
    seeds += ["libwayland-dev", "libffi-dev"]
if "drm" in wanted_feats:
    seeds += ["libdrm-dev"]

mirror = "http://ports.ubuntu.com/ubuntu-ports"
repos = [
    f"{suite}/main",
    f"{suite}/universe",
    f"{suite}-updates/main",
    f"{suite}-updates/universe",
]

def ver_key(v):
    return [int(x) if x.isdigit() else x for x in re.split(r"(\d+)", v)]

packages = {}
provides = {}

for repo in repos:
    idx_cache = os.path.join(cache_dir, repo.replace("/", "_") + "_Packages.xz")
    if not os.path.exists(idx_cache):
        url = f"{mirror}/dists/{repo}/binary-arm64/Packages.xz"
        print(f"    Fetching package index: {url}")
        urllib.request.urlretrieve(url, idx_cache)
    with lzma.open(idx_cache, "rt", encoding="utf-8", errors="replace") as f:
        cur = {}
        for line in f:
            line = line.rstrip("\n")
            if not line:
                if "Package" in cur and "Filename" in cur:
                    name = cur["Package"]
                    prev = packages.get(name)
                    if prev is None or ver_key(cur.get("Version", "0")) > ver_key(prev.get("Version", "0")):
                        packages[name] = cur
                    for p in cur.get("Provides", "").split(","):
                        parts = p.strip().split()
                        if parts:
                            prov = parts[0].split(":")[0]
                            if prov:
                                provides.setdefault(prov, name)
                cur = {}
            elif not line.startswith(" ") and ":" in line:
                k, v = line.split(":", 1)
                cur[k.strip()] = v.strip()

def resolve_pkg(name):
    name = name.split(":")[0].strip()
    if name in packages:
        return name
    return provides.get(name)

# Skip pure scripting/maintainer helpers if pulled by base libs
skip_pkgs = {"debconf", "dpkg", "perl", "perl-base", "adduser", "tzdata", "gcc-12-base", "gcc-13-base", "gcc-14-base"}

selected = []
seen = set()
queue = list(seeds)
while queue:
    req = queue.pop(0)
    pkg = resolve_pkg(req)
    if not pkg or pkg in seen or pkg in skip_pkgs:
        continue
    seen.add(pkg)
    selected.append(pkg)
    info = packages[pkg]
    deps = info.get("Pre-Depends", "") + "," + info.get("Depends", "")
    for group in deps.split(","):
        group = group.strip()
        if not group:
            continue
        for alt in group.split("|"):
            cand = alt.strip().split()[0].split(":")[0]
            if cand in skip_pkgs:
                break
            resolved = resolve_pkg(cand)
            if resolved:
                if resolved not in seen:
                    queue.append(resolved)
                break

print(f"    Downloading & extracting {len(selected)} arm64 .deb packages...")
extracted_marker_dir = os.path.join(cache_dir, "extracted")
os.makedirs(extracted_marker_dir, exist_ok=True)

for pkg in selected:
    info = packages[pkg]
    rel = info["Filename"]
    fname = os.path.basename(rel)
    deb_path = os.path.join(cache_dir, fname)
    marker = os.path.join(extracted_marker_dir, fname + ".ok")
    if os.path.exists(marker):
        continue
    if not os.path.exists(deb_path):
        urllib.request.urlretrieve(f"{mirror}/{rel}", deb_path)

    ar_list = subprocess.check_output(["ar", "t", deb_path], text=True).splitlines()
    data_member = next((m for m in ar_list if m.startswith("data.tar")), None)
    if not data_member:
        raise RuntimeError(f"No data.tar member in {deb_path}: {ar_list}")
    tar_flags = ["-x"]
    if data_member.endswith(".zst"):
        tar_flags.append("--zstd")
    elif data_member.endswith(".xz"):
        tar_flags.append("-J")
    elif data_member.endswith(".gz"):
        tar_flags.append("-z")
    elif data_member.endswith(".bz2"):
        tar_flags.append("-j")
    p1 = subprocess.Popen(["ar", "p", deb_path, data_member], stdout=subprocess.PIPE)
    p2 = subprocess.Popen(
        ["tar", *tar_flags, "--no-same-owner", "--no-same-permissions", "-C", sysroot, "-f", "-"],
        stdin=p1.stdout,
    )
    p1.stdout.close()
    if p2.wait() != 0 or p1.wait() != 0:
        raise RuntimeError(f"Failed extracting {deb_path}")
    open(marker, "w").close()

# Convert absolute symlinks inside sysroot to relative links so host cross-tools resolve inside sysroot
fixed = 0
for root, dirs, files in os.walk(sysroot):
    if ".cache" in root.split(os.sep):
        continue
    for name in dirs + files:
        p = os.path.join(root, name)
        if os.path.islink(p):
            target = os.readlink(p)
            if target.startswith("/"):
                abs_in_sysroot = os.path.join(sysroot, target.lstrip("/"))
                rel_target = os.path.relpath(abs_in_sysroot, os.path.dirname(p))
                os.unlink(p)
                os.symlink(rel_target, p)
                fixed += 1

with open(stamp_file, "w", encoding="utf-8") as f:
    f.write(" ".join(wanted_feats) + "\n")
print(f"    Ubuntu {suite} arm64 sysroot ready ({len(selected)} packages, {fixed} symlinks normalized).")
PY
}

HOST_ARCH="$(uname -m)"

# Interactive target selection if key options weren't specified on the CLI.
if [[ -z "$PRESET" && -z "$KMDS" ]]; then
  echo "Select Linux target environment:"
  echo "  1) Termux proot / chroot (Ubuntu/Debian, KGSL)"
  echo "  2) postmarketOS / mainline Linux (DRM msm)"
  echo "  3) Combined (kgsl,msm)"
  read -r -p "Choice [1]: " CHOICE
  case "${CHOICE:-1}" in
    1) PRESET="proot" ;;
    2) PRESET="postmarketos" ;;
    3) PRESET="combined" ;;
    *) echo "Invalid choice: '$CHOICE'" >&2; exit 1 ;;
  esac
fi

if [[ -n "$PRESET" ]]; then
  case "$PRESET" in
    proot)
      [[ -n "$KMDS" ]] || KMDS="kgsl"
      if [[ "$PLATFORMS_SET" -eq 0 ]]; then PLATFORMS="x11,wayland"; fi
      [[ -n "$BUILD_DEB" ]] || BUILD_DEB=1
      ;;
    postmarketos|pmos)
      PRESET="postmarketos"
      [[ -n "$KMDS" ]] || KMDS="msm"
      if [[ "$PLATFORMS_SET" -eq 0 ]]; then PLATFORMS="wayland,x11"; fi
      [[ -n "$BUILD_DEB" ]] || BUILD_DEB=0
      ;;
    combined)
      [[ -n "$KMDS" ]] || KMDS="kgsl,msm"
      if [[ "$PLATFORMS_SET" -eq 0 ]]; then PLATFORMS="x11,wayland"; fi
      [[ -n "$BUILD_DEB" ]] || BUILD_DEB=1
      ;;
    *)
      echo "Unknown preset: '$PRESET' (expected: proot, postmarketos, combined)" >&2
      exit 1
      ;;
  esac
else
  [[ -n "$KMDS" ]] || KMDS="kgsl"
  [[ -n "$BUILD_DEB" ]] || BUILD_DEB=0
fi

if [[ "$PLATFORMS_SET" -eq 0 ]]; then
  DEFAULT_PLATFORMS="${PLATFORMS:-x11,wayland}"
  read -r -p "Enter Mesa platforms (comma-separated, or 'headless') [$DEFAULT_PLATFORMS]: " INPUT_PLAT
  PLAT_CHOICE="${INPUT_PLAT:-$DEFAULT_PLATFORMS}"
  if [[ "$PLAT_CHOICE" == "headless" || "$PLAT_CHOICE" == "none" ]]; then
    PLATFORMS=""
  else
    PLATFORMS="$PLAT_CHOICE"
  fi
fi

DEFAULT_UBUNTU_SYSROOT="$SCRIPT_DIR/.sysroot-ubuntu-${UBUNTU_SUITE}-arm64"

if [[ "$HOST_ARCH" != "aarch64" && "$HOST_ARCH" != "arm64" && "$USE_UBUNTU_SYSROOT" -eq 0 && "$SYSROOT_SET" -eq 0 ]]; then
  if [[ -d "$DEFAULT_UBUNTU_SYSROOT" ]]; then
    read -r -p "Enter ARM64 sysroot path (Enter for $DEFAULT_UBUNTU_SYSROOT): " INPUT_SYSROOT
    SYSROOT="${INPUT_SYSROOT:-$DEFAULT_UBUNTU_SYSROOT}"
  else
    read -r -p "Enter ARM64 sysroot path (leave empty to auto-create Ubuntu $UBUNTU_SUITE arm64 sysroot or headless): " INPUT_SYSROOT
    SYSROOT="$INPUT_SYSROOT"
  fi
  if [[ -z "$SYSROOT" && -n "$PLATFORMS" ]]; then
    read -r -p "Auto-create Ubuntu $UBUNTU_SUITE arm64 sysroot at $DEFAULT_UBUNTU_SYSROOT for '$PLATFORMS'? [Y/n]: " AUTO_BOOT
    if [[ -z "$AUTO_BOOT" || "$AUTO_BOOT" =~ ^[Yy]$ ]]; then
      USE_UBUNTU_SYSROOT=1
    fi
  fi
fi

if [[ "$USE_UBUNTU_SYSROOT" -eq 1 ]]; then
  SYSROOT="${SYSROOT:-$DEFAULT_UBUNTU_SYSROOT}"
  SYSROOT="$(expand_path "$SYSROOT")"
  bootstrap_ubuntu_sysroot "$SYSROOT" "$UBUNTU_SUITE" "$PLATFORMS" "$KMDS"
else
  SYSROOT="$(expand_path "$SYSROOT")"
  if [[ -n "$SYSROOT" ]]; then
    if [[ ! -d "$SYSROOT" ]]; then
      echo "Error: sysroot directory not found: '$SYSROOT'" >&2
      exit 1
    fi
    if [[ "$SYSROOT" == "$DEFAULT_UBUNTU_SYSROOT" && -n "$PLATFORMS" ]]; then
      bootstrap_ubuntu_sysroot "$SYSROOT" "$UBUNTU_SUITE" "$PLATFORMS" "$KMDS"
    fi
  fi
fi

if [[ -n "$SYSROOT" ]]; then
  SYSROOT="$(cd "$SYSROOT" && pwd)"
fi

if ! command -v clang >/dev/null 2>&1 || ! command -v clang++ >/dev/null 2>&1; then
  echo "Error: clang and clang++ are required (tu5_formats.cc uses C99 designated array initializers in C++)." >&2
  echo "  Debian/Ubuntu: sudo apt install -y clang lld llvm meson ninja-build" >&2
  echo "  postmarketOS/Alpine: sudo apk add clang lld llvm meson ninja" >&2
  exit 1
fi

IS_MUSL=0
if [[ -n "$SYSROOT" ]]; then
  if [[ -f "$SYSROOT/etc/alpine-release" ]] || compgen -G "$SYSROOT/lib/ld-musl-aarch64.so*" >/dev/null 2>&1; then
    IS_MUSL=1
  fi
elif [[ "$HOST_ARCH" == "aarch64" || "$HOST_ARCH" == "arm64" ]]; then
  if [[ -f /etc/alpine-release ]] || compgen -G "/lib/ld-musl-aarch64.so*" >/dev/null 2>&1; then
    IS_MUSL=1
  fi
elif [[ "$PRESET" == "postmarketos" ]]; then
  IS_MUSL=1
fi

if [[ -z "$TRIPLE" ]]; then
  if [[ "$IS_MUSL" -eq 1 ]]; then
    TRIPLE="aarch64-linux-musl"
  else
    TRIPLE="aarch64-linux-gnu"
  fi
fi

if [[ -z "$PREFIX" ]]; then
  if [[ "$SYSTEM_INSTALL" -eq 1 ]]; then
    PREFIX="/usr"
  else
    PREFIX="/usr/local"
  fi
fi

if [[ -z "$LIBDIR" ]]; then
  if [[ "$IS_MUSL" -eq 1 || -f /etc/alpine-release ]]; then
    LIBDIR="lib"
  elif [[ -n "$SYSROOT" && ! -d "$SYSROOT/usr/lib/aarch64-linux-gnu" && -d "$SYSROOT/usr/lib" ]]; then
    LIBDIR="lib"
  elif [[ "$HOST_ARCH" == "aarch64" || "$HOST_ARCH" == "arm64" ]] && [[ ! -d /usr/lib/aarch64-linux-gnu ]]; then
    LIBDIR="lib"
  else
    LIBDIR="lib/aarch64-linux-gnu"
  fi
fi
ICD_DIR="share/vulkan/icd.d"

TAG="${PRESET:-custom}-${PLATFORMS:-headless}"
TAG="${TAG//,/-}"
BUILD_DIR="${BUILD_DIR:-build-linux-aarch64-$TAG}"

if [[ "$CLEAN" -eq 1 && -d "$BUILD_DIR" ]]; then
  rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"

USE_CCACHE=0
if command -v ccache >/dev/null 2>&1; then
  USE_CCACHE=1
fi

fmt_pair() {
  if [[ "$USE_CCACHE" -eq 1 ]]; then
    printf "['ccache', '%s', '%s']" "$1" "$2"
  else
    printf "['%s', '%s']" "$1" "$2"
  fi
}

fmt_single() {
  if [[ "$USE_CCACHE" -eq 1 ]]; then
    printf "['ccache', '%s']" "$1"
  else
    printf "'%s'" "$1"
  fi
}

CROSS_ARGS=()
MESON_SYSROOT_ARGS=()

if [[ "$HOST_ARCH" != "aarch64" && "$HOST_ARCH" != "arm64" ]]; then
  CROSS_FILE="$BUILD_DIR/.cross-linux-aarch64.txt"
  NATIVE_FILE="$BUILD_DIR/.native.txt"

  HOST_AR="llvm-ar"
  HOST_STRIP="llvm-strip"
  HOST_LD="lld"
  command -v "$HOST_AR" >/dev/null 2>&1 || HOST_AR="ar"
  command -v "$HOST_STRIP" >/dev/null 2>&1 || HOST_STRIP="strip"
  command -v "$HOST_LD" >/dev/null 2>&1 || HOST_LD="bfd"

  cat > "$CROSS_FILE" <<EOF
[binaries]
c = $(fmt_pair "clang" "--target=$TRIPLE")
cpp = $(fmt_pair "clang++" "--target=$TRIPLE")
ar = '$HOST_AR'
strip = '$HOST_STRIP'
c_ld = '$HOST_LD'
cpp_ld = '$HOST_LD'
pkg-config = 'pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

  NAT_LD="ld.lld"
  command -v "$NAT_LD" >/dev/null 2>&1 || NAT_LD="bfd"
  cat > "$NATIVE_FILE" <<EOF
[build_machine]
c = $(fmt_single "clang")
cpp = $(fmt_single "clang++")
ar = '$HOST_AR'
strip = '$HOST_STRIP'
c_ld = '$NAT_LD'
cpp_ld = '$NAT_LD'
system = 'linux'
cpu_family = '${HOST_ARCH}'
cpu = '${HOST_ARCH}'
endian = 'little'
EOF

  CROSS_ARGS=(--cross-file "$CROSS_FILE" --native-file "$NATIVE_FILE")
  unset PKG_CONFIG_PATH

  if [[ -n "$SYSROOT" ]]; then
    export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
    export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig:$SYSROOT/usr/local/lib/aarch64-linux-gnu/pkgconfig:$SYSROOT/usr/local/lib/pkgconfig"

    C_SYS_FLAGS=("--sysroot=$SYSROOT")
    CPP_SYS_FLAGS=("--sysroot=$SYSROOT")
    LINK_SYS_FLAGS=("--sysroot=$SYSROOT")

    # Point Clang explicitly at sysroot GCC runtime (crtbeginS.o/libgcc) and libstdc++ multiarch headers
    GCC_VER_DIR="$(find "$SYSROOT/usr/lib/gcc/$TRIPLE" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -V | tail -n 1 || true)"
    if [[ -n "$GCC_VER_DIR" ]]; then
      C_SYS_FLAGS+=("-B$GCC_VER_DIR")
      CPP_SYS_FLAGS+=("-B$GCC_VER_DIR")
      LINK_SYS_FLAGS+=("-B$GCC_VER_DIR" "-L$GCC_VER_DIR")
    fi
    CXX_INC_DIR="$(find "$SYSROOT/usr/include/c++" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -V | tail -n 1 || true)"
    if [[ -n "$CXX_INC_DIR" ]]; then
      CXX_VER="$(basename "$CXX_INC_DIR")"
      CPP_SYS_FLAGS+=("-isystem" "$CXX_INC_DIR")
      if [[ -d "$SYSROOT/usr/include/$TRIPLE/c++/$CXX_VER" ]]; then
        CPP_SYS_FLAGS+=("-isystem" "$SYSROOT/usr/include/$TRIPLE/c++/$CXX_VER")
      fi
    fi

    fmt_meson_array() {
      local out="["
      local item first=1
      for item in "$@"; do
        [[ "$first" -eq 1 ]] || out+=", "
        out+="'$item'"
        first=0
      done
      out+="]"
      printf '%s' "$out"
    }

    MESON_SYSROOT_ARGS+=(
      "-Dc_args=$(fmt_meson_array "${C_SYS_FLAGS[@]}")"
      "-Dcpp_args=$(fmt_meson_array "${CPP_SYS_FLAGS[@]}")"
      "-Dc_link_args=$(fmt_meson_array "${LINK_SYS_FLAGS[@]}")"
      "-Dcpp_link_args=$(fmt_meson_array "${LINK_SYS_FLAGS[@]}")"
    )
  else
    if [[ -d "/usr/aarch64-linux-gnu" ]]; then
      export PKG_CONFIG_LIBDIR="/usr/aarch64-linux-gnu/lib/pkgconfig"
      MESON_SYSROOT_ARGS+=(
        "-Dc_args=--sysroot=/usr/aarch64-linux-gnu"
        "-Dcpp_args=--sysroot=/usr/aarch64-linux-gnu"
        "-Dc_link_args=--sysroot=/usr/aarch64-linux-gnu"
        "-Dcpp_link_args=--sysroot=/usr/aarch64-linux-gnu"
      )
    else
      export PKG_CONFIG_LIBDIR="/nonexistent-cross-pkgconfig"
    fi
  fi

  # Pre-flight check for ARM64 window-system dependencies when cross-compiling
  if [[ -n "$PLATFORMS" ]] && command -v pkg-config >/dev/null 2>&1; then
    MISSING_CROSS_DEPS=()
    if [[ ",$PLATFORMS," == *",x11,"* ]] && ! pkg-config --exists xcb 2>/dev/null; then
      MISSING_CROSS_DEPS+=("xcb (libxcb1-dev, libx11-xcb-dev, libxcb-dri3-dev, libxcb-present-dev, libxshmfence-dev, libxrandr-dev)")
    fi
    if [[ ",$PLATFORMS," == *",wayland,"* ]] && ! pkg-config --exists wayland-client 2>/dev/null; then
      MISSING_CROSS_DEPS+=("wayland-client (libwayland-dev, wayland-protocols)")
    fi
    if [[ "${#MISSING_CROSS_DEPS[@]}" -gt 0 ]]; then
      echo "Error: cannot cross-compile platforms='$PLATFORMS' on $HOST_ARCH: missing ARM64 pkg-config deps:" >&2
      for dep in "${MISSING_CROSS_DEPS[@]}"; do
        echo "  - $dep" >&2
      done
      echo "" >&2
      echo "How to build:" >&2
      echo "  1) Auto-create/use the local Ubuntu arm64 sysroot:" >&2
      echo "       ./compile_linx.sh --ubuntu-sysroot" >&2
      echo "  2) Or pass your own ARM64 rootfs/sysroot:" >&2
      echo "       ./compile_linx.sh --sysroot=/path/to/arm64-rootfs" >&2
      echo "  3) Or run ./compile_linx.sh natively inside your ARM64 proot/chroot/postmarketOS environment." >&2
      echo "  4) Or build headless without X11/Wayland sysroot headers:" >&2
      echo "       ./compile_linx.sh --headless" >&2
      exit 1
    fi
  fi
else
  if [[ "$USE_CCACHE" -eq 1 ]]; then
    export CC="${CC:-ccache clang}"
    export CXX="${CXX:-ccache clang++}"
  else
    export CC="${CC:-clang}"
    export CXX="${CXX:-clang++}"
  fi
fi

echo "==> Configuring Mesa Turnip for Linux ARM64"
echo "    Build dir : $BUILD_DIR"
echo "    Host arch : $HOST_ARCH (target triple: $TRIPLE)"
echo "    Platforms : ${PLATFORMS:-<headless>}"
echo "    KMDs      : $KMDS"
echo "    Prefix    : $PREFIX ($LIBDIR)"
if [[ -n "$SYSROOT" ]]; then
  echo "    Sysroot   : $SYSROOT"
fi

MESON_OPTS=(
  "${MESON_SYSROOT_ARGS[@]}"
  "-Dbuildtype=$BUILD_TYPE"
  "-Db_ndebug=$B_NDEBUG"
  "-Dprefix=$PREFIX"
  "-Dlibdir=$LIBDIR"
  "-Dplatforms=$PLATFORMS"
  "-Dvulkan-drivers=freedreno"
  "-Dfreedreno-kmds=$KMDS"
  "-Dvulkan-icd-dir=$ICD_DIR"
  "-Dgallium-drivers="
  "-Dglx=disabled"
  "-Degl=disabled"
  "-Dgbm=disabled"
  "-Dllvm=disabled"
  "-Dlibunwind=disabled"
  "-Dzstd=disabled"
  "-Dxmlconfig=disabled"
  "-Dstrip=true"
)

if [[ ! -f "$BUILD_DIR/meson-private/build.dat" ]]; then
  rm -rf "$BUILD_DIR/meson-private"
  meson setup "$BUILD_DIR" "./" "${CROSS_ARGS[@]}" "${MESON_OPTS[@]}"
elif [[ "$RECONFIGURE" -eq 1 ]]; then
  meson setup "$BUILD_DIR" "./" --reconfigure "${CROSS_ARGS[@]}" "${MESON_OPTS[@]}"
else
  meson configure "$BUILD_DIR" "${MESON_OPTS[@]}"
fi

echo "==> Compiling with $JOBS jobs..."
ninja -C "$BUILD_DIR" -j "$JOBS"

STAGE_DIR="${DESTDIR:-$BUILD_DIR/stage}"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"
STAGE_DIR="$(cd "$STAGE_DIR" && pwd)"

echo "==> Staging runtime files into $STAGE_DIR..."
meson install -C "$BUILD_DIR" --destdir "$STAGE_DIR" --tags runtime --skip-subprojects --no-rebuild

ICD_JSON="$STAGE_DIR$PREFIX/$ICD_DIR/freedreno_icd.aarch64.json"
if [[ -f "$ICD_JSON" ]] && command -v python3 >/dev/null 2>&1; then
  python3 - "$ICD_JSON" "$PREFIX/$LIBDIR/libvulkan_freedreno.so" <<'PY'
import json, sys
p, lib = sys.argv[1], sys.argv[2]
with open(p, "r", encoding="utf-8") as f:
    data = json.load(f)
data.setdefault("ICD", {})["library_path"] = lib
with open(p, "w", encoding="utf-8") as f:
    json.dump(data, f, indent=4)
    f.write("\n")
PY
fi

mkdir -p "$STAGE_DIR$PREFIX/bin"
cat > "$STAGE_DIR$PREFIX/bin/turnip-env" <<EOF
#!/usr/bin/env bash
export VK_DRIVER_FILES="$PREFIX/$ICD_DIR/freedreno_icd.aarch64.json"
export VK_ICD_FILENAMES="\$VK_DRIVER_FILES"
if [[ \$# -eq 0 ]]; then
  echo "export VK_DRIVER_FILES=\$VK_DRIVER_FILES"
  echo "export VK_ICD_FILENAMES=\$VK_ICD_FILENAMES"
else
  exec "\$@"
fi
EOF
chmod 0755 "$STAGE_DIR$PREFIX/bin/turnip-env"

DEB_FILE=""
if [[ "$BUILD_DEB" -eq 1 ]]; then
  DEB_PKG_ROOT="$BUILD_DIR/.deb-stage"
  rm -rf "$DEB_PKG_ROOT"
  cp -a "$STAGE_DIR" "$DEB_PKG_ROOT"
  mkdir -p "$DEB_PKG_ROOT/DEBIAN"

  DEPENDS="libc6, libstdc++6, zlib1g"
  if [[ ",$PLATFORMS," == *",x11,"* ]]; then
    DEPENDS+=", libx11-6, libx11-xcb1, libxcb1, libxcb-dri3-0, libxcb-present0, libxcb-randr0, libxcb-shm0, libxcb-sync1, libxcb-xfixes0, libxcb-keysyms1, libxshmfence1"
  fi
  if [[ ",$PLATFORMS," == *",wayland,"* ]]; then
    DEPENDS+=", libwayland-client0"
  fi
  if [[ ",$KMDS," == *",msm,"* ]]; then
    DEPENDS+=", libdrm2"
  fi

  INSTALLED_SIZE="$(du -sk "$DEB_PKG_ROOT" --exclude=DEBIAN | awk '{print $1}')"
  {
    echo "Package: $PKG_NAME"
    echo "Version: $PKG_VERSION"
    echo "Section: libs"
    echo "Priority: optional"
    echo "Architecture: arm64"
    echo "Installed-Size: $INSTALLED_SIZE"
    echo "Maintainer: Freedreno Turnip <noreply@local>"
    if [[ "$SYSTEM_INSTALL" -eq 1 ]]; then
      echo "Conflicts: mesa-vulkan-drivers"
      echo "Replaces: mesa-vulkan-drivers"
      echo "Provides: mesa-vulkan-drivers"
    fi
    echo "Depends: $DEPENDS"
    echo "Description: Mesa $MESA_VER Turnip Vulkan driver (Adreno A5xx+/A6xx)"
    echo " Built for Linux arm64 (Termux proot / chroot Ubuntu/Debian)."
    echo " KMDs: $KMDS | Platforms: ${PLATFORMS:-headless}"
  } > "$DEB_PKG_ROOT/DEBIAN/control"

  mkdir -p "$DEB_DIR"
  DEB_DIR_ABS="$(cd "$DEB_DIR" && pwd)"
  DEB_FILE="$DEB_DIR_ABS/${PKG_NAME}_${PKG_VERSION}_${TAG}_arm64.deb"
  rm -f "$DEB_FILE"

  echo "==> Creating .deb package: $DEB_FILE..."
  if command -v dpkg-deb >/dev/null 2>&1; then
    dpkg-deb --build --root-owner-group "$DEB_PKG_ROOT" "$DEB_FILE"
  else
    TMP_AR="$BUILD_DIR/.tmp-deb"
    rm -rf "$TMP_AR"
    mkdir -p "$TMP_AR"
    printf '2.0\n' > "$TMP_AR/debian-binary"
    tar --numeric-owner --owner=0 --group=0 -C "$DEB_PKG_ROOT/DEBIAN" -cJf "$TMP_AR/control.tar.xz" .
    tar --numeric-owner --owner=0 --group=0 --exclude='./DEBIAN' -C "$DEB_PKG_ROOT" -cJf "$TMP_AR/data.tar.xz" .
    (cd "$TMP_AR" && ar rcS "$DEB_FILE" debian-binary control.tar.xz data.tar.xz)
    rm -rf "$TMP_AR"
  fi
fi

if [[ "$DO_INSTALL" -eq 1 ]]; then
  if [[ "$HOST_ARCH" != "aarch64" && "$HOST_ARCH" != "arm64" ]]; then
    echo "Warning: skipping live '--install' on non-aarch64 host ($HOST_ARCH); staged files are in $STAGE_DIR" >&2
  else
    meson install -C "$BUILD_DIR" --tags runtime --skip-subprojects --no-rebuild
    echo "Installed live into $PREFIX"
  fi
fi

echo "==> Done:"
echo "    Driver : $STAGE_DIR$PREFIX/$LIBDIR/libvulkan_freedreno.so"
echo "    ICD    : $STAGE_DIR$PREFIX/$ICD_DIR/freedreno_icd.aarch64.json"
echo "    Helper : $STAGE_DIR$PREFIX/bin/turnip-env"
if [[ -n "$DEB_FILE" ]]; then
  echo "    Deb    : $DEB_FILE"
fi
