/*
 * nori-ctl -- command-line control tool for Nori Xvision cameras.
 *
 * Subcommands:
 *   list
 *   trigger get <idx>
 *   trigger set <idx> <none|software|hardware|command>
 *   esn     get <idx> [--hex]
 *   esn     set <idx> <string>
 *   udata   get <idx> <piece> [--out <path>]
 *   udata   set <idx> <piece>  --in <path>
 *   tag     get   <idx>              [--block <n>]
 *   tag     set   <idx> <role>       [--block <n>] [--force]
 *   tag     clear <idx>              [--block <n>]
 *   shutter get <idx>
 *   shutter set <idx> <microseconds>
 *   gain    get <idx>
 *   gain    set <idx> <value>
 *   state   <idx>
 */

#include <Nori_Xvision_API/Nori_Error_Define.h>
#include <Nori_Xvision_API/Nori_Public.h>
#include <Nori_Xvision_API/Nori_Xvision_API.h>

#include "nori_tag.h"

#include <linux/v4l2-controls.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <unistd.h>

static constexpr size_t USER_DATA_SIZE = NORI_TAG_BLOCK_SIZE;
static constexpr size_t ESN_SIZE       = 64;

/* ================================================================
 *  RAII wrapper around Nori_Xvision_Init / UnInit
 * ================================================================ */

class SdkGuard {
 public:
  SdkGuard() {
    uint32_t n = 0;
    uint32_t ret = Nori_Xvision_Init(NORI_USB_DEVICE, &n);
    if (ret != NORI_OK) {
      fprintf(stderr, "Nori_Xvision_Init failed: 0x%04x\n", ret);
      return;
    }
    device_count_ = n;
    ok_ = true;
  }
  ~SdkGuard() {
    if (ok_) Nori_Xvision_UnInit();
  }
  SdkGuard(const SdkGuard&)            = delete;
  SdkGuard& operator=(const SdkGuard&) = delete;

  bool     ok()           const { return ok_; }
  uint32_t device_count() const { return device_count_; }

 private:
  uint32_t device_count_ = 0;
  bool     ok_           = false;
};

/* ================================================================
 *  Parsing helpers
 * ================================================================ */

static bool parse_uint(const char *s, uint32_t *out) {
  if (!s || !*s) return false;
  char *end = nullptr;
  errno = 0;
  unsigned long v = strtoul(s, &end, 0);
  if (errno != 0 || *end != '\0' || v > UINT32_MAX) return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

static bool parse_trigger(const char *s, E_TRIGGER_MODE *out) {
  if (!strcmp(s, "none"))     { *out = NON_TRIIGER_MODE;      return true; }
  if (!strcmp(s, "software")) { *out = SOFTWARE_TRIIGER_MODE; return true; }
  if (!strcmp(s, "hardware")) { *out = HARDWARE_TRIGGER_MODE; return true; }
  if (!strcmp(s, "command"))  { *out = COMMAND_TRIGGER_MODE;  return true; }
  return false;
}

static const char *trigger_name(E_TRIGGER_MODE m) {
  switch (m) {
    case NON_TRIIGER_MODE:      return "none";
    case SOFTWARE_TRIIGER_MODE: return "software";
    case HARDWARE_TRIGGER_MODE: return "hardware";
    case COMMAND_TRIGGER_MODE:  return "command";
  }
  return "unknown";
}

static const char *mirror_flip_name(E_SENSOR_MIRROR_FLIP m) {
  switch (m) {
    case Normal:         return "Normal";
    case MIRROR_EN:      return "Mirror";
    case FLIP_EN:        return "Flip";
    case MIRROR_FLIP_EN: return "Mirror+Flip";
  }
  return "unknown";
}

/* Render role for table display: replace non-printable bytes with '.',
 * truncate to `width` with an ellipsis. */
static std::string sanitize_role(const std::string &r, size_t width) {
  std::string s;
  s.reserve(r.size());
  for (char c : r) {
    unsigned char u = static_cast<unsigned char>(c);
    s.push_back((u >= 0x20 && u <= 0x7E) ? c : '.');
  }
  if (s.size() > width) {
    if (width >= 3) s = s.substr(0, width - 3) + "...";
    else s = s.substr(0, width);
  }
  return s;
}

static bool check_index(uint32_t idx, uint32_t count) {
  if (count == 0) {
    fprintf(stderr, "No Nori devices found.\n");
    return false;
  }
  if (idx >= count) {
    fprintf(stderr, "Device index %u out of range (%u device(s) present, valid 0..%u).\n",
            idx, count, count - 1);
    return false;
  }
  return true;
}

/* ================================================================
 *  Subcommands
 * ================================================================ */

static int cmd_list() {
  SdkGuard sdk;
  if (!sdk.ok()) return 2;

  uint32_t n = sdk.device_count();
  if (n == 0) {
    printf("No Nori devices found.\n");
    return 0;
  }

  printf("%-3s  %-9s  %-28s  %-20s  %-7s  %-32s  %s\n",
         "#", "VID:PID", "Product", "Serial", "Bus:Dev", "Location", "Tag");
  for (uint32_t i = 0; i < n; ++i) {
    DEVICE_INFO info;
    memset(&info, 0, sizeof(info));
    uint32_t ret = Nori_Xvision_GetDeviceInfo(i, &info);
    if (ret != NORI_OK) {
      fprintf(stderr, "GetDeviceInfo(%u) failed: 0x%04x\n", i, ret);
      continue;
    }
    char busdev[32];
    snprintf(busdev, sizeof(busdev), "%u:%u", info.busnum, info.devnum);

    std::string tag_col;
    BYTE tbuf[USER_DATA_SIZE];
    uint32_t rret = Nori_Xvision_GetUserData(i, NORI_TAG_BLOCK_DEFAULT, tbuf);
    if (rret != NORI_OK) {
      char err[48];
      snprintf(err, sizeof(err), "(read err 0x%x)", rret);
      tag_col = err;
    } else {
      NoriTagData t;
      nori_tag_decode(tbuf, &t);
      switch (t.status) {
        case NORI_TAG_NONE:    tag_col = "-"; break;
        case NORI_TAG_VALID:   tag_col = sanitize_role(t.role, 28); break;
        case NORI_TAG_FOREIGN: tag_col = "(foreign content)"; break;
      }
    }

    printf("%-3u  %04x:%04x  %-28s  %-20s  %-7s  %-32s  %s\n",
           i,
           info.idVendor, info.idProduct,
           reinterpret_cast<const char *>(info.iProduct),
           reinterpret_cast<const char *>(info.iSerialNumber),
           busdev,
           info.location,
           tag_col.c_str());
  }
  return 0;
}

static int cmd_tag_get(uint32_t idx, uint32_t block) {
  SdkGuard sdk;
  if (!sdk.ok()) return 2;
  if (!check_index(idx, sdk.device_count())) return 2;

  BYTE buf[USER_DATA_SIZE];
  uint32_t ret = Nori_Xvision_GetUserData(idx, block, buf);
  if (ret != NORI_OK) {
    fprintf(stderr, "GetUserData(block=%u) failed: 0x%04x\n", block, ret);
    return 2;
  }
  NoriTagData t;
  nori_tag_decode(buf, &t);
  switch (t.status) {
    case NORI_TAG_NONE:
      printf("(untagged)\n");
      return 0;
    case NORI_TAG_VALID:
      printf("%s\n", t.role);
      return 0;
    case NORI_TAG_FOREIGN:
      fprintf(stderr,
              "Block %u holds unrecognised content (no NORICAM magic or bad header).\n"
              "Use `tag clear` to erase, or `--force` on `tag set` to overwrite.\n",
              block);
      return 2;
  }
  return 2;
}

static int cmd_tag_set(uint32_t idx, const std::string &role,
                       uint32_t block, bool force) {
  if (role.empty()) {
    fprintf(stderr, "Role must not be empty.\n");
    return 1;
  }
  if (role.size() > NORI_TAG_ROLE_MAX) {
    fprintf(stderr, "Role too long: %zu bytes (max %zu).\n",
            role.size(), (size_t)NORI_TAG_ROLE_MAX);
    return 1;
  }
  if (role.find('\0') != std::string::npos) {
    fprintf(stderr, "Role must not contain NUL bytes.\n");
    return 1;
  }

  SdkGuard sdk;
  if (!sdk.ok()) return 2;
  if (!check_index(idx, sdk.device_count())) return 2;

  /* Pre-check: refuse to overwrite foreign content unless --force. */
  {
    BYTE buf[USER_DATA_SIZE];
    uint32_t ret = Nori_Xvision_GetUserData(idx, block, buf);
    if (ret != NORI_OK) {
      fprintf(stderr, "GetUserData(block=%u) failed: 0x%04x\n", block, ret);
      return 2;
    }
    NoriTagData t;
    nori_tag_decode(buf, &t);
    if (t.status == NORI_TAG_FOREIGN && !force) {
      fprintf(stderr,
              "Block %u on device %u holds unrecognised data; refusing to overwrite.\n"
              "Pass --force if you really want to write the tag.\n",
              block, idx);
      return 2;
    }
  }

  BYTE buf[USER_DATA_SIZE];
  nori_tag_encode(buf, role.data(), role.size());
  uint32_t ret = Nori_Xvision_SetUserData(idx, block, buf);
  if (ret != NORI_OK) {
    fprintf(stderr, "SetUserData(block=%u) failed: 0x%04x\n", block, ret);
    return 2;
  }

  /* Read back and verify. */
  BYTE vbuf[USER_DATA_SIZE];
  ret = Nori_Xvision_GetUserData(idx, block, vbuf);
  if (ret != NORI_OK) {
    fprintf(stderr, "Write OK but verification read failed: 0x%04x\n", ret);
    return 2;
  }
  NoriTagData v;
  nori_tag_decode(vbuf, &v);
  if (v.status != NORI_TAG_VALID || role != v.role) {
    fprintf(stderr, "Write/read-back mismatch: tag did not persist as expected.\n");
    return 2;
  }
  printf("OK: device %u tagged as \"%s\" in block %u.\n",
         idx, role.c_str(), block);
  return 0;
}

static int cmd_tag_clear(uint32_t idx, uint32_t block) {
  SdkGuard sdk;
  if (!sdk.ok()) return 2;
  if (!check_index(idx, sdk.device_count())) return 2;

  BYTE buf[USER_DATA_SIZE];
  memset(buf, 0, sizeof(buf));
  uint32_t ret = Nori_Xvision_SetUserData(idx, block, buf);
  if (ret != NORI_OK) {
    fprintf(stderr, "SetUserData(block=%u) failed: 0x%04x\n", block, ret);
    return 2;
  }
  printf("OK: device %u tag cleared in block %u.\n", idx, block);
  return 0;
}

static int cmd_trigger_get(uint32_t idx) {
  SdkGuard sdk;
  if (!sdk.ok()) return 2;
  if (!check_index(idx, sdk.device_count())) return 2;

  E_TRIGGER_MODE mode = NON_TRIIGER_MODE;
  uint32_t ret = Nori_Xvision_GetTriggerMode(idx, &mode);
  if (ret != NORI_OK) {
    fprintf(stderr, "GetTriggerMode failed: 0x%04x\n", ret);
    return 2;
  }
  printf("%s\n", trigger_name(mode));
  return 0;
}

static int cmd_trigger_set(uint32_t idx, E_TRIGGER_MODE mode) {
  SdkGuard sdk;
  if (!sdk.ok()) return 2;
  if (!check_index(idx, sdk.device_count())) return 2;

  uint32_t ret = Nori_Xvision_SetTriggerMode(idx, mode);
  if (ret != NORI_OK) {
    fprintf(stderr, "SetTriggerMode failed: 0x%04x\n", ret);
    return 2;
  }
  return 0;
}

/* Switch the camera to manual exposure so sensor shutter/gain writes take
 * effect. The Nori SDK requires this before SetSensorShutter / SetSensorGain.
 * Values match the plugin: 1 = manual, 3 = aperture priority (auto). */
static bool set_exposure_manual(uint32_t idx) {
  uint32_t ret = Nori_Xvision_SetProcessingUnitControl(
      idx, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL);
  if (ret != NORI_OK) {
    fprintf(stderr, "SetProcessingUnitControl(EXPOSURE_AUTO=manual) failed: 0x%04x\n", ret);
    return false;
  }
  return true;
}

static int cmd_shutter_get(uint32_t idx) {
  SdkGuard sdk;
  if (!sdk.ok()) return 2;
  if (!check_index(idx, sdk.device_count())) return 2;

  uint32_t value = 0;
  uint32_t ret = Nori_Xvision_GetSensorShutter(idx, &value);
  if (ret != NORI_OK) {
    fprintf(stderr, "GetSensorShutter failed: 0x%04x\n", ret);
    return 2;
  }
  printf("%u\n", value);
  return 0;
}

static int cmd_shutter_set(uint32_t idx, uint32_t value) {
  SdkGuard sdk;
  if (!sdk.ok()) return 2;
  if (!check_index(idx, sdk.device_count())) return 2;

  if (!set_exposure_manual(idx)) return 2;

  uint32_t ret = Nori_Xvision_SetSensorShutter(idx, value);
  if (ret != NORI_OK) {
    fprintf(stderr, "SetSensorShutter(%u) failed: 0x%04x\n", value, ret);
    return 2;
  }
  return 0;
}

static int cmd_gain_get(uint32_t idx) {
  SdkGuard sdk;
  if (!sdk.ok()) return 2;
  if (!check_index(idx, sdk.device_count())) return 2;

  uint32_t cur = 0, mn = 0, mx = 0, step = 0;
  uint32_t ret = Nori_Xvision_GetSensorGain(idx, &cur, &mn, &mx, &step);
  if (ret != NORI_OK) {
    fprintf(stderr, "GetSensorGain failed: 0x%04x\n", ret);
    return 2;
  }
  printf("cur=%u min=%u max=%u step=%u\n", cur, mn, mx, step);
  return 0;
}

static int cmd_gain_set(uint32_t idx, uint32_t value) {
  SdkGuard sdk;
  if (!sdk.ok()) return 2;
  if (!check_index(idx, sdk.device_count())) return 2;

  if (!set_exposure_manual(idx)) return 2;

  uint32_t ret = Nori_Xvision_SetSensorGain(idx, value);
  if (ret != NORI_OK) {
    fprintf(stderr, "SetSensorGain(%u) failed: 0x%04x\n", value, ret);
    return 2;
  }
  return 0;
}

/* ================================================================
 *  state -- sectioned dump of all getter-visible camera state
 * ================================================================ */

struct PuEntry {
  int32_t     cid;
  const char *name;
};

/* V4L2 UVC Processing Unit controls the Nori SDK exposes.
 * Unsupported ones print (unsupported) rather than aborting the dump. */
static const PuEntry kPuControls[] = {
  { V4L2_CID_BRIGHTNESS,              "BRIGHTNESS"              },
  { V4L2_CID_CONTRAST,                "CONTRAST"                },
  { V4L2_CID_SATURATION,              "SATURATION"              },
  { V4L2_CID_SHARPNESS,               "SHARPNESS"               },
  { V4L2_CID_HUE,                     "HUE"                     },
  { V4L2_CID_GAMMA,                   "GAMMA"                   },
  { V4L2_CID_GAIN,                    "GAIN"                    },
  { V4L2_CID_EXPOSURE_ABSOLUTE,       "EXPOSURE_ABSOLUTE"       },
  { V4L2_CID_EXPOSURE_AUTO,           "EXPOSURE_AUTO"           },
  { V4L2_CID_EXPOSURE_AUTO_PRIORITY,  "EXPOSURE_AUTO_PRIORITY"  },
  { V4L2_CID_AUTO_WHITE_BALANCE,      "AUTO_WHITE_BALANCE"      },
  { V4L2_CID_WHITE_BALANCE_TEMPERATURE,"WHITE_BALANCE_TEMPERATURE"},
  { V4L2_CID_BACKLIGHT_COMPENSATION,  "BACKLIGHT_COMPENSATION"  },
  { V4L2_CID_POWER_LINE_FREQUENCY,    "POWER_LINE_FREQUENCY"    },
};

static void print_section(const char *title) {
  printf("\n[%s]\n", title);
}

/* Two-column key/value line used throughout. */
static void print_kv(const char *key, const char *fmt, ...) {
  printf("  %-22s : ", key);
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
}

/* Print "err 0xNNNN" in place of a value when a getter fails. */
static void print_kv_err(const char *key, uint32_t ret) {
  printf("  %-22s : err 0x%04x\n", key, ret);
}

static int cmd_state(uint32_t idx) {
  SdkGuard sdk;
  if (!sdk.ok()) return 2;
  if (!check_index(idx, sdk.device_count())) return 2;

  printf("======================================================================\n");
  printf(" Nori camera state -- device %u\n", idx);
  printf("======================================================================\n");

  /* --- Sensor --- */
  print_section("Sensor");
  {
    uint32_t us = 0;
    uint32_t r = Nori_Xvision_GetSensorShutter(idx, &us);
    if (r == NORI_OK) print_kv("shutter (us)", "%u", us);
    else              print_kv_err("shutter (us)", r);
  }
  {
    uint32_t cur = 0, mn = 0, mx = 0, step = 0;
    uint32_t r = Nori_Xvision_GetSensorGain(idx, &cur, &mn, &mx, &step);
    if (r == NORI_OK) print_kv("gain", "cur=%u min=%u max=%u step=%u", cur, mn, mx, step);
    else              print_kv_err("gain", r);
  }
  {
    uint32_t bl = 0;
    uint32_t r = Nori_Xvision_GetSensorBlackLevel(idx, &bl);
    if (r == NORI_OK) print_kv("black level", "%u", bl);
    else              print_kv_err("black level", r);
  }
  {
    E_SENSOR_MIRROR_FLIP mf = Normal;
    uint32_t r = Nori_Xvision_GetSensorMirrorFlip(idx, &mf);
    if (r == NORI_OK) print_kv("mirror/flip", "%s", mirror_flip_name(mf));
    else              print_kv_err("mirror/flip", r);
  }

  /* --- Image --- */
  print_section("Image");
  {
    XY_OFFSET off = {0, 0};
    uint32_t r = Nori_Xvision_GetImageOffset(idx, &off);
    if (r == NORI_OK) print_kv("offset (x,y)", "%u, %u", off.width_offset, off.height_offset);
    else              print_kv_err("offset (x,y)", r);
  }
  {
    uint32_t v = 0;
    uint32_t r = Nori_Xvision_GetLensCorrectionEnable(idx, &v);
    if (r == NORI_OK) print_kv("lens correction", "%s", v ? "on" : "off");
    else              print_kv_err("lens correction", r);
  }

  /* --- Trigger --- */
  print_section("Trigger");
  {
    E_TRIGGER_MODE m = NON_TRIIGER_MODE;
    uint32_t r = Nori_Xvision_GetTriggerMode(idx, &m);
    if (r == NORI_OK) print_kv("mode", "%s", trigger_name(m));
    else              print_kv_err("mode", r);
  }

  /* --- White balance --- */
  print_section("White balance");
  {
    WB_RGB_GAIN g = {0, 0, 0};
    uint32_t r = Nori_Xvision_GetWhiteBalanceGain(idx, &g);
    if (r == NORI_OK) print_kv("RGB gain", "R=%u G=%u B=%u", g.gain_r, g.gain_g, g.gain_b);
    else              print_kv_err("RGB gain", r);
  }

  /* --- One-shot auto status --- */
  print_section("One-shot auto status");
  {
    uint32_t v = 0;
    uint32_t r = Nori_Xvision_GetSingleAutoWhiteBalance(idx, &v);
    if (r == NORI_OK) print_kv("auto WB", "%u", v);
    else              print_kv_err("auto WB", r);
  }
  {
    uint32_t v = 0;
    uint32_t r = Nori_Xvision_GetSingleAutoFocus(idx, &v);
    if (r == NORI_OK) print_kv("auto focus", "%u", v);
    else              print_kv_err("auto focus", r);
  }
  {
    uint32_t v = 0;
    uint32_t r = Nori_Xvision_GetSingleAutoExposure(idx, &v);
    if (r == NORI_OK) print_kv("auto exposure", "%u", v);
    else              print_kv_err("auto exposure", r);
  }

  /* --- UVC Processing Unit controls --- */
  print_section("UVC Processing Unit");
  printf("  %-26s %10s %8s %8s %10s %10s %10s\n",
         "control", "cur", "flags", "step", "min", "max", "default");
  printf("  %-26s %10s %8s %8s %10s %10s %10s\n",
         "--------------------------",
         "----------", "--------", "--------", "----------", "----------", "----------");
  for (const auto &e : kPuControls) {
    int32_t cur = 0, flags = 0, step = 0, mn = 0, mx = 0, def = 0;
    uint32_t r = Nori_Xvision_GetProcessingUnitControl(
        idx, e.cid, &cur, &flags, &step, &mn, &mx, &def);
    if (r == NORI_OK) {
      printf("  %-26s %10d %8d %8d %10d %10d %10d\n",
             e.name, cur, flags, step, mn, mx, def);
    } else {
      printf("  %-26s (unsupported: 0x%04x)\n", e.name, r);
    }
  }

  putchar('\n');
  return 0;
}

static int cmd_esn_get(uint32_t idx, bool hex) {
  SdkGuard sdk;
  if (!sdk.ok()) return 2;
  if (!check_index(idx, sdk.device_count())) return 2;

  BYTE buf[ESN_SIZE];
  memset(buf, 0, sizeof(buf));
  uint32_t ret = Nori_Xvision_GetUserESN(idx, buf);
  if (ret != NORI_OK) {
    fprintf(stderr, "GetUserESN failed: 0x%04x\n", ret);
    return 2;
  }

  if (hex) {
    for (size_t i = 0; i < ESN_SIZE; ++i) printf("%02x", buf[i]);
    putchar('\n');
  } else {
    size_t len = 0;
    while (len < ESN_SIZE && buf[len] != 0) ++len;
    if (len == 0) {
      printf("(empty)\n");
    } else {
      fwrite(buf, 1, len, stdout);
      putchar('\n');
    }
  }
  return 0;
}

static int cmd_esn_set(uint32_t idx, const char *value) {
  size_t len = strlen(value);
  if (len > ESN_SIZE) {
    fprintf(stderr, "ESN too long: %zu bytes (max %zu).\n", len, ESN_SIZE);
    return 1;
  }

  SdkGuard sdk;
  if (!sdk.ok()) return 2;
  if (!check_index(idx, sdk.device_count())) return 2;

  BYTE buf[ESN_SIZE];
  memset(buf, 0, sizeof(buf));
  memcpy(buf, value, len);

  uint32_t ret = Nori_Xvision_SetUserESN(idx, buf);
  if (ret != NORI_OK) {
    fprintf(stderr,
            "SetUserESN failed: 0x%04x (note: Gen60 does not support writing ESN).\n",
            ret);
    return 2;
  }
  return 0;
}

static int cmd_udata_get(uint32_t idx, uint32_t piece, const char *outpath) {
  if (!outpath && isatty(fileno(stdout))) {
    fprintf(stderr,
            "Refusing to write %zu binary bytes to a terminal. "
            "Use --out <path> or redirect stdout.\n",
            USER_DATA_SIZE);
    return 1;
  }

  SdkGuard sdk;
  if (!sdk.ok()) return 2;
  if (!check_index(idx, sdk.device_count())) return 2;

  BYTE buf[USER_DATA_SIZE];
  memset(buf, 0, sizeof(buf));
  uint32_t ret = Nori_Xvision_GetUserData(idx, piece, buf);
  if (ret != NORI_OK) {
    fprintf(stderr, "GetUserData failed: 0x%04x\n", ret);
    return 2;
  }

  FILE *out = outpath ? fopen(outpath, "wb") : stdout;
  if (!out) {
    fprintf(stderr, "Cannot open %s for writing: %s\n", outpath, strerror(errno));
    return 2;
  }
  size_t w = fwrite(buf, 1, USER_DATA_SIZE, out);
  if (outpath) fclose(out);
  if (w != USER_DATA_SIZE) {
    fprintf(stderr, "Short write: %zu of %zu bytes.\n", w, USER_DATA_SIZE);
    return 2;
  }
  return 0;
}

static int cmd_udata_set(uint32_t idx, uint32_t piece, const char *inpath) {
  FILE *in = fopen(inpath, "rb");
  if (!in) {
    fprintf(stderr, "Cannot open %s: %s\n", inpath, strerror(errno));
    return 1;
  }

  BYTE buf[USER_DATA_SIZE];
  memset(buf, 0, sizeof(buf));

  size_t r = fread(buf, 1, USER_DATA_SIZE, in);
  char extra;
  size_t tail = fread(&extra, 1, 1, in);
  fclose(in);

  if (tail > 0) {
    fprintf(stderr, "Input %s is larger than %zu bytes.\n", inpath, USER_DATA_SIZE);
    return 1;
  }
  if (r < USER_DATA_SIZE) {
    fprintf(stderr,
            "Note: %s is %zu bytes, zero-padded to %zu.\n",
            inpath, r, USER_DATA_SIZE);
  }

  SdkGuard sdk;
  if (!sdk.ok()) return 2;
  if (!check_index(idx, sdk.device_count())) return 2;

  uint32_t ret = Nori_Xvision_SetUserData(idx, piece, buf);
  if (ret != NORI_OK) {
    fprintf(stderr, "SetUserData failed: 0x%04x\n", ret);
    return 2;
  }
  return 0;
}

/* ================================================================
 *  Argument dispatch
 * ================================================================ */

static void usage() {
  fprintf(stderr,
    "Usage: nori-ctl <command> [args]\n"
    "\n"
    "  list\n"
    "      List connected Nori cameras.\n"
    "\n"
    "  trigger get <idx>\n"
    "  trigger set <idx> <none|software|hardware|command>\n"
    "\n"
    "  esn get <idx> [--hex]\n"
    "  esn set <idx> <string>\n"
    "      64-byte serial. Default treats bytes as a NUL-terminated string.\n"
    "      --hex dumps all 64 raw bytes as hex.\n"
    "      Note: Gen60 devices cannot write ESN.\n"
    "\n"
    "  udata get <idx> <piece> [--out <path>]\n"
    "  udata set <idx> <piece>  --in <path>\n"
    "      4096-byte user-data block. Piece: 0..255 (Gen60), 0..63 (Gen56).\n"
    "      get: binary to stdout unless --out is given (refuses to spew on a TTY).\n"
    "      set: reads from file; short files zero-padded, over-sized rejected.\n"
    "\n"
    "  tag get   <idx>            [--block <n>]\n"
    "  tag set   <idx> <role>     [--block <n>] [--force]\n"
    "  tag clear <idx>            [--block <n>]\n"
    "      Per-camera identity tag stored in user-data block %u by default.\n"
    "      Survives power cycle and firmware update (per SDK spec).\n"
    "      `tag set` refuses to overwrite unrecognised data without --force.\n"
    "\n"
    "  shutter get <idx>\n"
    "  shutter set <idx> <microseconds>\n"
    "      Sensor shutter / exposure time. `set` switches exposure to manual\n"
    "      mode first (required by SDK).\n"
    "\n"
    "  gain get <idx>\n"
    "  gain set <idx> <value>\n"
    "      Sensor analogue gain multiplier. `get` prints cur/min/max/step.\n"
    "      `set` switches exposure to manual mode first (required by SDK).\n"
    "\n"
    "  state <idx>\n"
    "      Dump camera state in sectioned tables: sensor (shutter/gain/black\n"
    "      level/mirror-flip), image (offset/lens correction), trigger, white\n"
    "      balance, one-shot auto status, and all UVC PU controls.\n",
    NORI_TAG_BLOCK_DEFAULT);
}

static bool need_args(int argc, int needed) {
  if (argc < needed) { usage(); return false; }
  return true;
}

int main(int argc, char *argv[]) {
  if (argc < 2) { usage(); return 1; }
  const std::string cmd = argv[1];

  if (cmd == "-h" || cmd == "--help" || cmd == "help") { usage(); return 0; }

  if (cmd == "list") {
    if (argc != 2) { usage(); return 1; }
    return cmd_list();
  }

  if (cmd == "trigger") {
    if (!need_args(argc, 4)) return 1;
    const std::string sub = argv[2];
    uint32_t idx;
    if (!parse_uint(argv[3], &idx)) {
      fprintf(stderr, "Invalid device index: %s\n", argv[3]);
      return 1;
    }
    if (sub == "get") {
      if (argc != 4) { usage(); return 1; }
      return cmd_trigger_get(idx);
    }
    if (sub == "set") {
      if (argc != 5) { usage(); return 1; }
      E_TRIGGER_MODE mode;
      if (!parse_trigger(argv[4], &mode)) {
        fprintf(stderr, "Unknown trigger mode: %s (want none|software|hardware|command)\n",
                argv[4]);
        return 1;
      }
      return cmd_trigger_set(idx, mode);
    }
    usage();
    return 1;
  }

  if (cmd == "esn") {
    if (!need_args(argc, 4)) return 1;
    const std::string sub = argv[2];
    uint32_t idx;
    if (!parse_uint(argv[3], &idx)) {
      fprintf(stderr, "Invalid device index: %s\n", argv[3]);
      return 1;
    }
    if (sub == "get") {
      bool hex = false;
      for (int i = 4; i < argc; ++i) {
        if (!strcmp(argv[i], "--hex")) { hex = true; }
        else { usage(); return 1; }
      }
      return cmd_esn_get(idx, hex);
    }
    if (sub == "set") {
      if (argc != 5) { usage(); return 1; }
      return cmd_esn_set(idx, argv[4]);
    }
    usage();
    return 1;
  }

  if (cmd == "udata") {
    if (!need_args(argc, 5)) return 1;
    const std::string sub = argv[2];
    uint32_t idx, piece;
    if (!parse_uint(argv[3], &idx))   { fprintf(stderr, "Invalid device index: %s\n", argv[3]); return 1; }
    if (!parse_uint(argv[4], &piece)) { fprintf(stderr, "Invalid piece index: %s\n", argv[4]);  return 1; }

    if (sub == "get") {
      const char *outpath = nullptr;
      for (int i = 5; i < argc; ++i) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) { outpath = argv[++i]; }
        else { usage(); return 1; }
      }
      return cmd_udata_get(idx, piece, outpath);
    }
    if (sub == "set") {
      const char *inpath = nullptr;
      for (int i = 5; i < argc; ++i) {
        if (!strcmp(argv[i], "--in") && i + 1 < argc) { inpath = argv[++i]; }
        else { usage(); return 1; }
      }
      if (!inpath) { usage(); return 1; }
      return cmd_udata_set(idx, piece, inpath);
    }
    usage();
    return 1;
  }

  if (cmd == "state") {
    if (argc != 3) { usage(); return 1; }
    uint32_t idx;
    if (!parse_uint(argv[2], &idx)) {
      fprintf(stderr, "Invalid device index: %s\n", argv[2]);
      return 1;
    }
    return cmd_state(idx);
  }

  if (cmd == "shutter" || cmd == "gain") {
    if (!need_args(argc, 4)) return 1;
    const std::string sub = argv[2];
    uint32_t idx;
    if (!parse_uint(argv[3], &idx)) {
      fprintf(stderr, "Invalid device index: %s\n", argv[3]);
      return 1;
    }
    if (sub == "get") {
      if (argc != 4) { usage(); return 1; }
      return (cmd == "shutter") ? cmd_shutter_get(idx) : cmd_gain_get(idx);
    }
    if (sub == "set") {
      if (argc != 5) { usage(); return 1; }
      uint32_t value;
      if (!parse_uint(argv[4], &value)) {
        fprintf(stderr, "Invalid %s value: %s\n", cmd.c_str(), argv[4]);
        return 1;
      }
      return (cmd == "shutter") ? cmd_shutter_set(idx, value)
                                : cmd_gain_set(idx, value);
    }
    usage();
    return 1;
  }

  if (cmd == "tag") {
    if (!need_args(argc, 4)) return 1;
    const std::string sub = argv[2];
    uint32_t idx;
    if (!parse_uint(argv[3], &idx)) {
      fprintf(stderr, "Invalid device index: %s\n", argv[3]);
      return 1;
    }

    /* Shared --block parsing for get/clear (positional optional starts at 4). */
    auto parse_block_flag = [](int start, int argc_, char **argv_,
                               uint32_t *block_out) -> int {
      for (int i = start; i < argc_; ++i) {
        if (!strcmp(argv_[i], "--block") && i + 1 < argc_) {
          if (!parse_uint(argv_[++i], block_out)) {
            fprintf(stderr, "Invalid block index: %s\n", argv_[i]);
            return -1;
          }
        } else {
          usage();
          return -1;
        }
      }
      return 0;
    };

    if (sub == "get") {
      uint32_t block = NORI_TAG_BLOCK_DEFAULT;
      if (parse_block_flag(4, argc, argv, &block) < 0) return 1;
      return cmd_tag_get(idx, block);
    }
    if (sub == "clear") {
      uint32_t block = NORI_TAG_BLOCK_DEFAULT;
      if (parse_block_flag(4, argc, argv, &block) < 0) return 1;
      return cmd_tag_clear(idx, block);
    }
    if (sub == "set") {
      if (argc < 5) { usage(); return 1; }
      std::string role = argv[4];
      uint32_t block = NORI_TAG_BLOCK_DEFAULT;
      bool force = false;
      for (int i = 5; i < argc; ++i) {
        if (!strcmp(argv[i], "--force")) { force = true; }
        else if (!strcmp(argv[i], "--block") && i + 1 < argc) {
          if (!parse_uint(argv[++i], &block)) {
            fprintf(stderr, "Invalid block index: %s\n", argv[i]);
            return 1;
          }
        } else {
          usage();
          return 1;
        }
      }
      return cmd_tag_set(idx, role, block, force);
    }
    usage();
    return 1;
  }

  usage();
  return 1;
}
