// LD_PRELOAD hook for camerahalserver (32-bit) on Xiaomi Mi Pad 3 (cappu, MT8173).
//
// Interposes ImgSensorDrv::featureControl() in libcam.halsensor.so.
// The real function is a thin ioctl() wrapper into the MT8173 imgsensor
// kernel driver; SENSOR_FEATURE_GET_CROP_INFO (0x0C08) round-trips a
// SENSOR_WINSIZE_INFO_STRUCT through it. On this device the kernel is
// currently returning an all-zero struct (AppTsf: "SensorCrop incorrect!"),
// which produces the rotated/blurry/miscolored preview.
//
// featureControl is not dlsym-able safely by name here: our own LD_PRELOAD
// definition would shadow it under the same symbol, so instead we locate
// the neighboring exported symbol ImgSensorDrv::sendCommand and apply a
// fixed byte offset determined by disassembling the on-device
// libcam.halsensor.so (sendCommand @ 0x17601, featureControl @ 0x16d3d,
// both Thumb; delta = 0x8c4). Confirmed against the currently installed
// /vendor/lib/libcam.halsensor.so on 2026-09-11.
//
// Plain C, no libc++: the device has no libc++_shared.so anywhere under
// /vendor or /system (only vndk-sp-private libc++.so, not loadable by an
// arbitrary vendor process), so a C++ build would fail to dlopen at all.

#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <android/dlext.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

typedef int (*FeatureControlFn)(void *self, uint32_t dualCamEnum,
                                 uint32_t featureId, uint8_t *para,
                                 uint32_t *len);

// android/log.h drags in extra baggage for a plain-C standalone build;
// resolve __android_log_print ourselves so a missing liblog never breaks
// the actual hook logic, only the logging.
typedef int (*AndroidLogPrintFn)(int prio, const char *tag, const char *fmt, ...);
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_ERROR 6

static AndroidLogPrintFn resolveLogFn(void) {
  static AndroidLogPrintFn cached = NULL;
  static int tried = 0;
  if (cached || tried)
    return cached;
  tried = 1;
  void *handle = dlopen("liblog.so", RTLD_NOW);
  if (!handle)
    return NULL;
  cached = (AndroidLogPrintFn)dlsym(handle, "__android_log_print");
  return cached;
}

static void hookLog(int prio, const char *fmt, ...) {
  AndroidLogPrintFn fn = resolveLogFn();
  if (!fn)
    return;
  va_list ap;
  va_start(ap, fmt);
  char buf[256];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  fn(prio, "cappu_crop_hook", "%s", buf);
}

static const uintptr_t kSendCommandToFeatureControlOffset = 0x8c4;

static FeatureControlFn resolveOriginal(void) {
  static FeatureControlFn cached = NULL;
  static int triedOnce = 0;
  if (cached || triedOnce)
    return cached;
  triedOnce = 1;

  void *handle = dlopen("libcam.halsensor.so", RTLD_NOW);
  if (!handle) {
    hookLog(ANDROID_LOG_ERROR, "dlopen libcam.halsensor.so failed: %s", dlerror());
    return NULL;
  }

  void *sendCommandAddr =
      dlsym(handle, "_ZN12ImgSensorDrv11sendCommandE15SENSOR_DEV_ENUMjjjj");
  if (!sendCommandAddr) {
    hookLog(ANDROID_LOG_ERROR, "dlsym sendCommand failed: %s", dlerror());
    return NULL;
  }

  uintptr_t addr = (uintptr_t)sendCommandAddr - kSendCommandToFeatureControlOffset;
  cached = (FeatureControlFn)addr;
  hookLog(ANDROID_LOG_INFO, "resolved real featureControl @ %p (sendCommand @ %p)",
          (void *)addr, sendCommandAddr);
  return cached;
}

// SENSOR_WINSIZE_INFO_STRUCT tables for Samsung S5K3L8, copied from
// imgsensor_winsize_info[] in s5k3l8mipiraw_Sensor.c (kernel 3.18.123).
// Layout (16x uint16_t): full_w, full_h, x0_offset, y0_offset, w0_size,
// h0_size, scale_w, scale_h, x1_offset, y1_offset, w1_size, h1_size,
// x2_tg_offset, y2_tg_offset, w2_tg_size, h2_tg_size.
static const uint16_t kWinPreview[16] = {4208, 3120, 0,   0,   4208, 3120,
                                          2048, 1536, 0,   0,   2048, 1536,
                                          0,    0,    2048, 1536};
static const uint16_t kWinCapture[16] = {4208, 3120, 0,   0,   4208, 3120,
                                          4208, 3120, 0,   0,   4208, 3120,
                                          0,    0,    4208, 3120};
static const uint16_t kWinVideo[16] = {4208, 3120, 0,   0,   4208, 3120,
                                        4208, 3120, 0,   0,   4208, 3120,
                                        0,    0,    4208, 3120};
static const uint16_t kWinHsVideo[16] = {4208, 3120, 184, 120, 3840, 2880,
                                          640,  480,  0,   0,   640,  480,
                                          0,    0,    640, 480};
static const uint16_t kWinSlimVideo[16] = {4208, 3120, 184, 480, 3840, 2160,
                                            1280, 720,  0,   0,   1280, 720,
                                            0,    0,    1280, 720};

// MSDK_SCENARIO_ID_ENUM values, from kd_imgsensor_define.h.
enum {
  kScenarioPreview = 0,
  kScenarioCaptureJpeg = 1,
  kScenarioVideoPreview = 2,
  kScenarioHighSpeedVideo = 3,
  kScenarioSlimVideo = 9,
};

static const uint16_t *pickTable(uint32_t scenarioId) {
  switch (scenarioId) {
  case kScenarioCaptureJpeg:
    return kWinCapture;
  case kScenarioVideoPreview:
    return kWinVideo;
  case kScenarioHighSpeedVideo:
    return kWinHsVideo;
  case kScenarioSlimVideo:
    return kWinSlimVideo;
  case kScenarioPreview:
  default:
    return kWinPreview;
  }
}

// SENSOR_FEATURE_ENUM values, from kd_imgsensor_define.h
// (SENSOR_FEATURE_START = 3000).
static const uint32_t kSensorFeatureGetCropInfo = 3080;    // 0x0C08
static const uint32_t kSensorFeatureSetTestPattern = 3042; // 0x0BE2
static const uint32_t kSensorFeatureSetAwbGain = 3083;      // 0x0C0B
static const uint32_t kSensorFeatureGetShutterGainAwbGain = 3048; // 0x0BE8

// Exported under the exact mangled name of the real
// ImgSensorDrv::featureControl(CAMERA_DUAL_CAMERA_SENSOR_ENUM,
// ACDK_SENSOR_FEATURE_ENUM, MUINT8*, MUINT32*), so LD_PRELOAD interposition
// redirects every caller (across .so boundaries) into this function.
int hookedFeatureControl(void *self, uint32_t dualCamEnum, uint32_t featureId,
                          uint8_t *para, uint32_t *len)
    __asm__("_ZN12ImgSensorDrv14featureControlE30CAMERA_DUAL_CAMERA_SENSOR_"
            "ENUM24ACDK_SENSOR_FEATURE_ENUMPhPj");

// Diagnostic-only: log every distinct featureId seen, with the first two
// words of para, so we can eyeball which call is feeding the ISP a bad
// color value. Not required for the crop/test-pattern fixes above.
static void logEveryFeatureOnce(uint32_t featureId, uint8_t *para) {
  static uint32_t seen[128];
  static int seenCount = 0;
  for (int i = 0; i < seenCount; i++) {
    if (seen[i] == featureId)
      return;
  }
  if (seenCount < 128)
    seen[seenCount++] = featureId;
  uint32_t w0 = 0, w1 = 0;
  if (para) {
    uint32_t *words = (uint32_t *)para;
    w0 = words[0];
    w1 = words[1];
  }
  hookLog(ANDROID_LOG_INFO, "featureControl id=%u (0x%x) para[0]=%u(0x%x) para[1]=%u(0x%x)",
          featureId, featureId, w0, w0, w1, w1);
}

int hookedFeatureControl(void *self, uint32_t dualCamEnum, uint32_t featureId,
                          uint8_t *para, uint32_t *len) {
  logEveryFeatureOnce(featureId, para);

  if ((featureId == kSensorFeatureSetAwbGain ||
       featureId == kSensorFeatureGetShutterGainAwbGain) &&
      para) {
    uint32_t *words = (uint32_t *)para;
    hookLog(ANDROID_LOG_INFO,
            "AWB-related feature id=%u words: %u %u %u %u %u %u",
            featureId, words[0], words[1], words[2], words[3], words[4],
            words[5]);
  }

  // The mismatched GC8024 tuning data still causes something upstream to
  // request the sensor's built-in test pattern (color bars) instead of live
  // image data. Force it off unconditionally before forwarding the call, so
  // whatever value the caller asked for never reaches the kernel.
  if (featureId == kSensorFeatureSetTestPattern && para) {
    uint32_t *words = (uint32_t *)para;
    if (words[0] != 0) {
      hookLog(ANDROID_LOG_INFO,
              "SET_TEST_PATTERN requested value=%u, forcing off", words[0]);
      words[0] = 0;
    }
  }

  FeatureControlFn original = resolveOriginal();
  int ret = original ? original(self, dualCamEnum, featureId, para, len) : -1;

  if (featureId == kSensorFeatureGetCropInfo && para) {
    uint32_t *words = (uint32_t *)para;
    uint32_t scenarioId = words[0];
    uint16_t *wininfo = (uint16_t *)(uintptr_t)words[1];

    if (wininfo && wininfo[0] == 0) {
      const uint16_t *fixed = pickTable(scenarioId);
      memcpy(wininfo, fixed, sizeof(uint16_t) * 16);
      hookLog(ANDROID_LOG_INFO,
              "GET_CROP_INFO scenario=%u returned zeroed struct, patched to %ux%u",
              scenarioId, fixed[0], fixed[1]);
    } else if (wininfo) {
      hookLog(ANDROID_LOG_INFO,
              "GET_CROP_INFO scenario=%u already non-zero (%ux%u), leaving alone",
              scenarioId, wininfo[0], wininfo[1]);
    }
  }

  return ret;
}

// --- AppTsf::TsfInit(void*, void*) in libcamalgo.so -----------------------
//
// This is the actual source of "[TsfInit][Error] SensorCrop incorrect!" /
// full_width(0)... . TsfInit does NOT call into ImgSensorDrv itself: it
// reads 8 sequential uint32_t fields directly out of the struct pointed to
// by its second argument, at byte offsets 0x9c/0xa0/0xa4/0xa8/0xac/0xb0/
// 0xb4/0xb8 (confirmed by disassembling the on-device libcamalgo.so around
// 0x9e560-0x9e620: those exact offsets off r5 are copied onto the stack and
// then each checked non-zero, erroring out to the log lines above when any
// of them is 0). Whatever upstream code is supposed to populate that struct
// (almost certainly derived from libcameracustom.so's static per-sensor
// table) is leaving it zeroed on this device.
//
// We don't know the individual field semantics beyond "8 uint32 words used
// in pairwise non-zero / bounds checks", so we fill all 8 with the sensor's
// full native resolution and a zero/full crop window (full_w, full_h,
// full_w, full_h, 0, 0, full_w, full_h) -- a trivially self-consistent
// "no crop" identity that satisfies every bounds check we found regardless
// of exact field naming, and only kicks in when the struct is actually
// zeroed (field 0 == 0), so it never overrides a value that's already sane.

typedef void (*TsfInitFn)(void *self, void *info);

static const uintptr_t kGetInstanceToTsfInitOffset = 0x1cc;

static TsfInitFn resolveOriginalTsfInit(void) {
  static TsfInitFn cached = NULL;
  static int triedOnce = 0;
  if (cached || triedOnce)
    return cached;
  triedOnce = 1;

  void *handle = dlopen("libcamalgo.so", RTLD_NOW);
  if (!handle) {
    hookLog(ANDROID_LOG_ERROR, "dlopen libcamalgo.so failed: %s", dlerror());
    return NULL;
  }

  void *getInstanceAddr = dlsym(handle, "_ZN6AppTsf11getInstanceEv");
  if (!getInstanceAddr) {
    hookLog(ANDROID_LOG_ERROR, "dlsym AppTsf::getInstance failed: %s", dlerror());
    return NULL;
  }

  uintptr_t addr = (uintptr_t)getInstanceAddr + kGetInstanceToTsfInitOffset;
  cached = (TsfInitFn)addr;
  hookLog(ANDROID_LOG_INFO, "resolved real AppTsf::TsfInit @ %p (getInstance @ %p)",
          (void *)addr, getInstanceAddr);
  return cached;
}

static const uint32_t kS5k3l8FullWidth = 4208;
static const uint32_t kS5k3l8FullHeight = 3120;

void hookedTsfInit(void *self, void *info)
    __asm__("_ZN6AppTsf7TsfInitEPvS0_");

// D65 AWB reference gain fields, found 2026-09-11 evening by tracing the
// exact source of the logcat line "[1][updateAWBParam] rD65Gain: R=999,
// G=512, B=695" back to its origin: TsfInit (this same function) loads
// these three int32 fields from *this same* `info` struct at offsets
// 0x90/0x94/0x98 -- immediately before the crop fields at 0x9c already
// patched above -- and logs/uses them verbatim as the D65 white-point
// reference. G=512 is neutral (Q9 fixed point, 512=1.0x) but R=999
// (~1.95x) and B=695 (~1.36x) are not, and G being the only correct one
// matches every other symptom today: this is GC8024's calibration point,
// not S5K3L8's. Unlike the crop fields, these are *never* zero (so the
// existing "invalid -> defaults to 512" fallback the real code already
// has for a zero reading never triggers), so force them to the same
// neutral 512/512/512 the code itself uses as its own fallback value,
// unconditionally. Matches the code's own established "safe" value,
// rather than inventing a new one.
static const uint32_t kNeutralD65Gain = 512;

void hookedTsfInit(void *self, void *info) {
  if (info) {
    uint32_t *fields = (uint32_t *)((uint8_t *)info + 0x9c);
    if (fields[0] == 0) {
      uint32_t fixed[8] = {
          kS5k3l8FullWidth,  kS5k3l8FullHeight, // full_width, full_height
          kS5k3l8FullWidth,  kS5k3l8FullHeight, // resize_width, resize_height
          0,                 0,                 // crop_x_offset, crop_y_offset
          kS5k3l8FullWidth,  kS5k3l8FullHeight, // crop_width, crop_height
      };
      memcpy(fields, fixed, sizeof(fixed));
      hookLog(ANDROID_LOG_INFO,
              "TsfInit crop struct @ %p was zeroed, patched to full %ux%u",
              (void *)fields, kS5k3l8FullWidth, kS5k3l8FullHeight);
    }

    uint32_t *d65 = (uint32_t *)((uint8_t *)info + 0x90);
    if (d65[0] != kNeutralD65Gain || d65[2] != kNeutralD65Gain) {
      hookLog(ANDROID_LOG_INFO,
              "TsfInit D65 gain @ %p was R=%u G=%u B=%u, forcing neutral %u/%u/%u",
              (void *)d65, d65[0], d65[1], d65[2], kNeutralD65Gain, kNeutralD65Gain,
              kNeutralD65Gain);
      d65[0] = kNeutralD65Gain;
      d65[1] = kNeutralD65Gain;
      d65[2] = kNeutralD65Gain;
    }
  }

  TsfInitFn original = resolveOriginalTsfInit();
  if (original)
    original(self, info);
}

// --- NSCamCustomSensor::getSensorOrientation() -----------------------------
//
// Diagnostic only: this is a zero-arg leaf getter (project-level mount
// angle, not entangled in the DMA/ISP hardware pipeline the way
// IspTuningCustom::createInstance was), so unlike the CCM object it's cheap
// to compare broken-vs-donor here before touching anything. Log both.

typedef uint32_t (*GetSensorOrientationFn)(void);

static const char *kGetSensorOrientationSym =
    "_ZN17NSCamCustomSensor20getSensorOrientationEv";

static GetSensorOrientationFn resolveOriginalGetSensorOrientation(void) {
  static GetSensorOrientationFn cached = NULL;
  static int triedOnce = 0;
  if (cached || triedOnce)
    return cached;
  triedOnce = 1;
  void *handle = dlopen("libcameracustom.so", RTLD_NOW);
  if (!handle) {
    hookLog(ANDROID_LOG_ERROR, "dlopen libcameracustom.so failed (orientation): %s",
            dlerror());
    return NULL;
  }
  void *addr = dlsym(handle, kGetSensorOrientationSym);
  if (!addr) {
    hookLog(ANDROID_LOG_ERROR, "dlsym getSensorOrientation failed: %s", dlerror());
    return NULL;
  }
  cached = (GetSensorOrientationFn)addr;
  return cached;
}

void *hookedGetSensorOrientation(void)
    __asm__("_ZN17NSCamCustomSensor20getSensorOrientationEv");

// Confirmed on-device 2026-09-11: broken build's static orientation value is
// 0, the S5K3L8 donor's is 90 -- matches the still-rotated preview exactly.
// This getter is a stateless leaf (no DMA/hardware pipeline entanglement
// like IspTuningCustom::createInstance had), so replacing its return value
// outright is safe: point callers at our own static 90 instead of whatever
// the broken build's internal (wrong) constant lives at.
static const uint32_t kCorrectSensorOrientationDeg = 90;

void *hookedGetSensorOrientation(void) {
  hookLog(ANDROID_LOG_INFO,
          "getSensorOrientation: overriding to %u (see earlier log for broken/donor raw values)",
          kCorrectSensorOrientationDeg);
  return (void *)(uintptr_t)&kCorrectSensorOrientationDeg;
}

// --- MetadataProvider::getDeviceWantedOrientation() / getDeviceSetupOrientation() ---
//
// THE REAL fix point, found on-device 2026-09-11 after the whole
// IMetadata/constructCustStaticMetadata investigation above turned out to
// be for a path this device doesn't actually use (it registers as
// LEGACY_JPEG hardware level -- confirmed via
// "CameraManager: Using legacy camera HAL." / "Camera support level:
// LEGACY_JPEG" in logcat, and `dumpsys media.camera` showing
// "Facing: Back / Orientation: 0" no matter what we wrote into the
// mtkcam IMetadata object). The actual, authoritative source matching
// dumpsys exactly is this logcat line from mtkcam-devicemgr, emitted at
// every camerahalserver boot:
//   [logLocked] [00] -> orientation(wanted/setup)=(  0/0  ) BACK ...
//   [logLocked] [01] -> orientation(wanted/setup)=(180/180) FRONT ...
// which are exactly this class's getDeviceWantedOrientation()/
// getDeviceSetupOrientation() return values (both disassembled earlier
// today: pure system-property reads via a property-name String8 member
// at this+0x18 whose literal text we never located, each falling back to
// atoi-default 0 when unset -- which it is here). Overriding the return
// value directly sidesteps needing that property name at all.
//
// getDeviceWantedOrientation()'s own body already branches on
// *(this+4)==0 to decide "back camera -> 90, else -> 270" for the
// property-is-set case, so that offset is a validated way (from the real
// function's own logic, not guessed) to tell back from front here.

typedef uint32_t (*GetDeviceOrientationFn)(void *this_);

static const char *kGetDeviceWantedOrientationSym =
    "_ZNK7android18NSMetadataProvider16MetadataProvider26getDeviceWantedOrientationEv";
static const char *kGetDeviceSetupOrientationSym =
    "_ZNK7android18NSMetadataProvider16MetadataProvider25getDeviceSetupOrientationEv";

static GetDeviceOrientationFn resolveOriginalGetDeviceWantedOrientation(void) {
  static GetDeviceOrientationFn cached = NULL;
  static int tried = 0;
  if (cached || tried) return cached;
  tried = 1;
  void *handle = dlopen("libcam.metadataprovider.so", RTLD_NOW);
  if (!handle) return NULL;
  cached = (GetDeviceOrientationFn)dlsym(handle, kGetDeviceWantedOrientationSym);
  return cached;
}
static GetDeviceOrientationFn resolveOriginalGetDeviceSetupOrientation(void) {
  static GetDeviceOrientationFn cached = NULL;
  static int tried = 0;
  if (cached || tried) return cached;
  tried = 1;
  void *handle = dlopen("libcam.metadataprovider.so", RTLD_NOW);
  if (!handle) return NULL;
  cached = (GetDeviceOrientationFn)dlsym(handle, kGetDeviceSetupOrientationSym);
  return cached;
}

static const uint32_t kRearOrientationFix = 90;
// 2026-09-12: front camera reported live as "rotated 90 left" with the
// original unfixed value of 180 (mtkcam-devicemgr boot log: "[01] ->
// orientation(wanted/setup)=(180/180) FRONT"). Same symptom the rear camera
// had when it was wrongly reporting 0 and needed +90 to reach the correct
// 90. Trying the standard, extremely common front/rear orientation pairing
// (90 rear / 270 front) as the first guess rather than 180-90=90, since
// swapping to the "other" 90-based value (270) is the far more typical
// device convention than accidentally landing on the same value as rear.
static const uint32_t kFrontOrientationFix = 270;

void *hookedGetDeviceWantedOrientation(void *this_)
    __asm__("_ZNK7android18NSMetadataProvider16MetadataProvider26getDeviceWantedOrientationEv");
void *hookedGetDeviceWantedOrientation(void *this_) {
  uint32_t facing = *(uint32_t *)((uint8_t *)this_ + 4);
  if (facing == 0) {
    hookLog(ANDROID_LOG_INFO, "getDeviceWantedOrientation(this=%p, facing=0/back): overriding to %u",
            this_, kRearOrientationFix);
    return (void *)(uintptr_t)kRearOrientationFix;
  }
  if (facing == 1) {
    hookLog(ANDROID_LOG_INFO, "getDeviceWantedOrientation(this=%p, facing=1/front): overriding to %u",
            this_, kFrontOrientationFix);
    return (void *)(uintptr_t)kFrontOrientationFix;
  }
  GetDeviceOrientationFn original = resolveOriginalGetDeviceWantedOrientation();
  uint32_t ret = original ? original(this_) : 0;
  hookLog(ANDROID_LOG_INFO, "getDeviceWantedOrientation(this=%p, facing=%u): unchanged, %u", this_,
          facing, ret);
  return (void *)(uintptr_t)ret;
}

void *hookedGetDeviceSetupOrientation(void *this_)
    __asm__("_ZNK7android18NSMetadataProvider16MetadataProvider25getDeviceSetupOrientationEv");
void *hookedGetDeviceSetupOrientation(void *this_) {
  uint32_t facing = *(uint32_t *)((uint8_t *)this_ + 4);
  if (facing == 0) {
    hookLog(ANDROID_LOG_INFO, "getDeviceSetupOrientation(this=%p, facing=0/back): overriding to %u",
            this_, kRearOrientationFix);
    return (void *)(uintptr_t)kRearOrientationFix;
  }
  if (facing == 1) {
    hookLog(ANDROID_LOG_INFO, "getDeviceSetupOrientation(this=%p, facing=1/front): overriding to %u",
            this_, kFrontOrientationFix);
    return (void *)(uintptr_t)kFrontOrientationFix;
  }
  GetDeviceOrientationFn original = resolveOriginalGetDeviceSetupOrientation();
  uint32_t ret = original ? original(this_) : 0;
  hookLog(ANDROID_LOG_INFO, "getDeviceSetupOrientation(this=%p, facing=%u): unchanged, %u", this_,
          facing, ret);
  return (void *)(uintptr_t)ret;
}

// --- getAWBParam<sensorDev=1>() (rear camera) constant override ------------
//
// Same "return a pointer to a different compile-time constant" pattern as
// getSensorOrientation above, applied to a different, much larger constant:
// NSIspTuning::getAWBParam<ESensorDev_T::1>() in libcameracustom.so returns
// a reference to a fixed 1396-byte AWB_PARAM_T struct (confirmed by
// disassembly: the same 3-instruction "ldr/add/bx" PC-relative
// return-a-constant idiom as getSensorOrientation, and confirmed the size
// is exactly 1396 by diffing the address deltas between the sensorDev=1/2/4
// variants, which are laid out contiguously). Diffing this device's broken
// (GC8024) struct against the S5K3L8 donor's byte-for-byte: only 21 of 1396
// bytes differ, all in small int32 tables after offset 0x190 (looks like
// AWB convergence/threshold tables, not the CCM matrix itself -- CCM has no
// equivalent simple constant, see the dlsym section below). Low risk: this
// is a pure data swap, no donor *code* ever executes, unlike the
// createInstance/constructCustStaticMetadata attempts that broke things
// earlier today.
//
// UNTESTED as of writing this comment -- confirm on-device before trusting.

static const unsigned char kDonorAwbParamDev1[] = {
  0x00, 0x02, 0x00, 0x00, 0xff, 0x1f, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
  0x13, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x42, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x00,
  0x42, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x00,
  0x42, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x42, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00,
  0x3c, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00,
  0x3c, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00, 0x00,
  0x5a, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x42, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0xe1, 0x00, 0x00, 0x00,
  0x15, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00,
  0x32, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00,
  0x32, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00,
  0x32, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00,
  0x37, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00,
  0x46, 0x00, 0x00, 0x00, 0x4b, 0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00,
  0x55, 0x00, 0x00, 0x00, 0x5a, 0x00, 0x00, 0x00, 0x5f, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00,
  0x32, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00,
  0x32, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00,
  0x32, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00,
  0x32, 0x00, 0x00, 0x00, 0x37, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00,
  0x41, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00, 0x00, 0x4b, 0x00, 0x00, 0x00,
  0x50, 0x00, 0x00, 0x00, 0x55, 0x00, 0x00, 0x00, 0x5a, 0x00, 0x00, 0x00,
  0x5f, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00,
  0xb0, 0x04, 0x00, 0x00, 0xb0, 0x04, 0x00, 0x00, 0xb0, 0x04, 0x00, 0x00,
  0xb0, 0x04, 0x00, 0x00, 0xb0, 0x04, 0x00, 0x00, 0xb0, 0x04, 0x00, 0x00,
  0xb0, 0x04, 0x00, 0x00, 0xb0, 0x04, 0x00, 0x00, 0xb0, 0x04, 0x00, 0x00,
  0xb0, 0x04, 0x00, 0x00, 0xb0, 0x04, 0x00, 0x00, 0x1a, 0x04, 0x00, 0x00,
  0x84, 0x03, 0x00, 0x00, 0x84, 0x03, 0x00, 0x00, 0x84, 0x03, 0x00, 0x00,
  0x84, 0x03, 0x00, 0x00, 0x84, 0x03, 0x00, 0x00, 0x84, 0x03, 0x00, 0x00,
  0x84, 0x03, 0x00, 0x00, 0x84, 0x03, 0x00, 0x00, 0x84, 0x03, 0x00, 0x00,
  0x0b, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
  0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
  0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
  0x40, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
  0x0b, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
  0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
  0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
  0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
  0x0b, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
  0x80, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00,
  0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
  0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
  0x0b, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
  0x80, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
  0x20, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
  0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x69, 0x00, 0x00, 0x00,
  0x87, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x82, 0x00, 0x00, 0x00, 0x78, 0x00, 0x00, 0x00,
  0xa0, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x82, 0x00, 0x00, 0x00, 0x78, 0x00, 0x00, 0x00,
  0xa0, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7d, 0x00, 0x00, 0x00,
  0xa5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x9b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7d, 0x00, 0x00, 0x00,
  0x9b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x9b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x82, 0x00, 0x00, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x91, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
  0xc0, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
  0xc0, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
  0x78, 0x00, 0x00, 0x00, 0x5a, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xfe, 0x00, 0x00, 0x00,
  0xfe, 0x00, 0x00, 0x00, 0xfe, 0x00, 0x00, 0x00, 0xff, 0x0f, 0x00, 0x00,
  0xff, 0x0f, 0x00, 0x00, 0xff, 0x0f, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};
static const unsigned int kDonorAwbParamDev1Len = 1396;

void *hookedGetAWBParamDev1(void)
    __asm__("_Z11getAWBParamILN11NSIspTuning12ESensorDev_TE1EERK11AWB_PARAM_Tv");
void *hookedGetAWBParamDev1(void) {
  hookLog(ANDROID_LOG_INFO, "getAWBParam<rear> overridden with donor S5K3L8 AWB_PARAM_T (%u bytes)",
          kDonorAwbParamDev1Len);
  return (void *)(uintptr_t)kDonorAwbParamDev1;
}

__attribute__((constructor)) static void init_hook(void) {
  resolveOriginal();
  resolveOriginalTsfInit();
  resolveOriginalGetSensorOrientation();
  resolveOriginalGetDeviceWantedOrientation();
  resolveOriginalGetDeviceSetupOrientation();
}
