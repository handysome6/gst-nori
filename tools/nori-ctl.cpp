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
 */

#include <Nori_Xvision_API/Nori_Error_Define.h>
#include <Nori_Xvision_API/Nori_Public.h>
#include <Nori_Xvision_API/Nori_Xvision_API.h>

#include "nori_tag.h"

#include <cerrno>
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
    "      `tag set` refuses to overwrite unrecognised data without --force.\n",
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
