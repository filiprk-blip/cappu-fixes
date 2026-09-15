# cappu (Xiaomi Mi Pad 3) — Camera / Display / EGL Fixes

Fixes for a LineageOS 17.1 (Android 10) Treble GSI + custom vendor/cust setup on the Xiaomi Mi Pad 3 (codename `cappu`, MT8173, kernel 3.18.123).

Not everything here is kernel-side — see the breakdown below.

## 1. Display: black screen stuck after wake from sleep (kernel)

**Root cause:** PowerVR GPU Active Power Management (APM) puts the GPU into a low-power state that fails to properly re-sync with the display controller on wake.

**Fix:** `kernel-patches/config_kernel_user_mt8173.h` — `PVRSRV_APPHINT_ENABLEAPM` forced to `RGX_ACTIVEPM_FORCE_OFF` at compile time. No runtime toggle needed. Measured cost: ~+3.5% idle power draw.

## 2. Camera: autofocus completely non-functional (kernel)

**Root cause:** `main_lens.c`'s `g_stAF_DrvList[]` string-matches the actuator name reported by the sensor driver ("WV511AAF") to a dead driver entry that doesn't talk to real hardware correctly. The physical actuator on this unit is actually a **DW9714**, whose real I2C address (`0x1C>>1 = 0x0E`) is hardcoded inside `DW9714AF.c` — a completely separate driver that was never being reached by the WV511AAF name match.

Note: `I2C_REGISTER_ID`/`I2C_CONFIG_SETTING` board-file-style constants in `main_lens.c` are dead code on this kernel — they only compile under `CONFIG_MTK_LEGACY`, and this kernel uses `CONFIG_OF` (devicetree). Don't chase I2C address defines there.

**Fix:**
- `kernel-patches/main_lens.c` — in `g_stAF_DrvList[]`, the `CONFIG_MTK_LENS_WV511AAF_SUPPORT` entry now points at `DW9714AF_SetI2Cclient / DW9714AF_Ioctl / DW9714AF_Release` instead of the dead `WV511AAF_*` functions.
- `kernel-patches/cappu_defconfig` — added `CONFIG_MTK_LENS_WV511AAF_SUPPORT=y` (required for the above to be compiled in at all).
- `vendor-cust/ueventd.rc` — added `/dev/MAINAF 0660 system camera` (kernel defaults this node to `0600 root:root`, which blocks userspace camera HAL access).

**Verification caveat:** don't trust `getMCUInfo()` or any driver "success" return as proof the motor moved — it's a cached software variable the write path sets unconditionally, not a real I2C read-back on this driver. Confirm with an actual visual focus-pull test.

## 3. Camera: wrong rotation + wrong colors (userspace, vendor/cust)

**Root cause:** vendor camera HAL on Android 10 returns bad `SENSOR_FEATURE_GET_CROP_INFO` data (all-zero `SENSOR_WINSIZE_INFO_STRUCT`) and wrong sensor-orientation values for this sensor (S5K3L8).

**Fix:** `vendor-cust/hook-source/cappu_crop_hook.c` — an `LD_PRELOAD` interposer (compiled output in `vendor-cust/compiled/libcappu_crop_hook.so`), loaded via:
```
# init/camerahalserver.rc
service camerahalserver /vendor/bin/hw/camerahalserver
    setenv LD_PRELOAD /vendor/lib/libcappu_crop_hook.so
```
Hooks (resolved lazily via `dlopen()+dlsym()` on first real call — do not eagerly prime resolution in `init_hook()`, it stalls `camerahalserver` startup):
- `hookedFeatureControl` — patches the `SENSOR_FEATURE_GET_CROP_INFO` response
- `hookedTsfInit` — fixes the MT8173 TSF path that crashes 3A with an empty crop table on Android 10
- `hookedGetSensorOrientation`
- `hookedGetDeviceWantedOrientation` / `hookedGetDeviceSetupOrientation` — rotation fix, both cameras
- `hookedGetAWBParamDev1` — donor AWB table override for sensor dev 1

This is a cleaned-up ~650-line version; an earlier ~2500-line version had a lot of unnecessary experimental hooks (OBC, LSC, calibration data, extra color hooks) that turned out to be unneeded once the actual crop/orientation root causes were found.

## 4. OLX app crash / Flutter Impeller EGL crash (vendor blob binary patch)

`vendor-cust/compiled/libEGL_mtk.so` — patched directly at the binary level. Note: crashes of this kind only reproduce after a full reboot — an app-level relaunch alone won't show the bug or prove a fix, because Zygote caches resolved driver function pointers at boot.

## 5. Building a flashable zip with GApps — GSI partition-resize gotcha

If baking GApps into a `system_patch`/`package_extract_dir` step: don't. This GSI's `system.new.dat.br` ships undersized relative to the real `/system` partition (measured: raw partition 2.5GB, shipped filesystem image only ~1.84GB, ~57MB/23 inodes free after the base LineageOS write). The filesystem only gets `resize2fs`'d to the real physical partition size on the device's **first actual boot** — standard GSI behavior, since GSIs are generic and don't know the target partition size at build time.

Correct sequence: flash system+vendor+kernel only → boot once (triggers the resize) → reboot to recovery → flash GApps as a separate second-pass zip. After the resize, `/system` has ~425MB free. Baking GApps into the same first-pass zip reliably fails with I/O-error/ENOSPC-adjacent errors on large files, regardless of GApps variant size.

All fixes verified on real hardware (real photos, live touch-to-focus, repeated sleep/wake cycles) — not just log/return-code output.
