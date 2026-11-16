# Mesa Turnip (Adreno 5xx / 6xx)

Fork of **Mesa 26.3.0** (`freedreno` / Turnip Vulkan driver) with Vulkan 1.4 support and fixes for **Adreno 5xx** and **Adreno 6xx** GPUs on **Android** and **Linux ARM64**.

Upstream Mesa README is preserved at [`readme.mdmesa`](readme.mdmesa).

---

## Android Build (`compile.sh`)

Builds `libvulkan_freedreno.so` for AArch64 Android (KGSL) using the Android NDK.

### Requirements
- Android NDK (with `aarch64-linux-android` Clang toolchain)
- `meson`, `ninja`

### Usage
```bash
# Interactive (prompts for NDK directory, or reads ANDROID_NDK_HOME / ANDROID_NDK_ROOT)
./compile.sh

# Non-interactive
./compile.sh --ndk-dir /path/to/android-ndk

# Optional flags
./compile.sh --ndk-dir /path/to/android-ndk --build-dir build-android-aarch64 --reconfigure
```

### Output
- `build-android-aarch64/src/freedreno/vulkan/libvulkan_freedreno.so`

---

## Linux ARM64 Build (`compile_linx.sh`)

Builds Mesa Turnip for **Linux AArch64**, supporting both native ARM64 hosts and cross-compilation from `x86_64`:
- **Termux `proot` / `chroot` Ubuntu & Debian** (`kgsl`, automatically builds an installable `.deb` package)
- **postmarketOS / mainline Linux** (`msm`)
- **Combined** (`kgsl,msm`, also builds a `.deb` package)

### Requirements
- `clang`, `clang++`, `lld`, `meson`, `ninja`
- `python3`, `ar`, `tar`, `xz` (and `curl` / `zstd` when bootstrapping an Ubuntu ARM64 cross-sysroot on `x86_64`)

### Usage
```bash
# Interactive setup (select target preset, platforms, and sysroot)
./compile_linx.sh

# Cross-compile on x86_64 for Termux proot/chroot Ubuntu (X11 + .deb package)
# Automatically downloads a minimal Ubuntu 22.04 arm64 sysroot into .sysroot-ubuntu-jammy-arm64/
./compile_linx.sh --preset proot --platforms x11 --ubuntu-sysroot

# Cross-compile using your own ARM64 rootfs/sysroot
./compile_linx.sh --preset proot --platforms x11,wayland --sysroot /path/to/arm64-rootfs

# Headless cross-compile (no X11/Wayland sysroot required)
./compile_linx.sh --preset proot --headless

# Native ARM64 build (e.g. directly inside Termux proot or postmarketOS)
./compile_linx.sh --preset postmarketos
```

Run `./compile_linx.sh --help` for all options (`--kmds`, `--system`, `--install`, `--deb`, `--no-deb`, `--debug`, `--jobs`, `--clean`, etc.).

### Outputs & Installing on Termux `proot` / `chroot`
- **Staged files:** `<build-dir>/stage/usr/local/` (`libvulkan_freedreno.so`, `freedreno_icd.aarch64.json`, `bin/turnip-env`)
- **Debian package:** `out-deb/mesa-vulkan-freedreno_*.deb`

Install and test inside Ubuntu/Debian `proot` or `chroot`:
```bash
sudo apt install ./mesa-vulkan-freedreno_*.deb
turnip-env vulkaninfo --summary
```
