#ifndef NORI_TAG_H
#define NORI_TAG_H

/*
 * Shared tag-block format used by nori-ctl and the gst-nori plugin to give
 * visually identical Nori cameras a stable per-unit identity ("LEFT",
 * "RIGHT", ...). The tag lives in Nori user-data block 255 by default and
 * persists across power cycles and firmware updates per the SDK spec.
 *
 * Block layout (4096 bytes; only the first 76 are used):
 *   [ 0.. 7]  magic "NORICAM\0"
 *   [ 8.. 9]  version, uint16 little-endian
 *   [10..11]  role_len, uint16 little-endian, 1..NORI_TAG_ROLE_MAX
 *   [12..75]  role bytes (non-NUL)
 *   [76..  ]  reserved (zero)
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NORI_TAG_BLOCK_DEFAULT  255u
#define NORI_TAG_VERSION        1u
#define NORI_TAG_ROLE_MAX       64u
#define NORI_TAG_BLOCK_SIZE     4096u

static const uint8_t NORI_TAG_MAGIC[8] = {'N','O','R','I','C','A','M', 0};

typedef enum {
  NORI_TAG_NONE    = 0,   /* block is erased / blank                        */
  NORI_TAG_VALID   = 1,   /* magic + header decode as a well-formed tag     */
  NORI_TAG_FOREIGN = 2,   /* non-blank but not ours - do not overwrite      */
} NoriTagStatus;

typedef struct {
  NoriTagStatus status;
  uint16_t      version;
  uint16_t      role_len;
  char          role[NORI_TAG_ROLE_MAX + 1]; /* NUL-terminated for convenience */
} NoriTagData;

static inline void
nori_tag_decode (const uint8_t *buf, NoriTagData *out)
{
  memset (out, 0, sizeof (*out));

  int all_ff = 1, all_zero = 1;
  for (size_t i = 0; i < 12; i++) {
    if (buf[i] != 0xFF) all_ff = 0;
    if (buf[i] != 0x00) all_zero = 0;
  }
  if (memcmp (buf, NORI_TAG_MAGIC, sizeof (NORI_TAG_MAGIC)) != 0) {
    out->status = (all_ff || all_zero) ? NORI_TAG_NONE : NORI_TAG_FOREIGN;
    return;
  }

  uint16_t ver = (uint16_t) (buf[8]  | (buf[9]  << 8));
  uint16_t len = (uint16_t) (buf[10] | (buf[11] << 8));
  out->version = ver;
  if (ver == 0 || len == 0 || len > NORI_TAG_ROLE_MAX) {
    out->status = NORI_TAG_FOREIGN;
    return;
  }
  for (size_t i = 0; i < len; i++) {
    if (buf[12 + i] == 0) { out->status = NORI_TAG_FOREIGN; return; }
  }
  out->status   = NORI_TAG_VALID;
  out->role_len = len;
  memcpy (out->role, buf + 12, len);
  out->role[len] = 0;
}

static inline void
nori_tag_encode (uint8_t *buf, const char *role, size_t role_len)
{
  memset (buf, 0, NORI_TAG_BLOCK_SIZE);
  memcpy (buf, NORI_TAG_MAGIC, sizeof (NORI_TAG_MAGIC));
  buf[8]  = (uint8_t) (NORI_TAG_VERSION & 0xFF);
  buf[9]  = (uint8_t) ((NORI_TAG_VERSION >> 8) & 0xFF);
  buf[10] = (uint8_t) (role_len & 0xFF);
  buf[11] = (uint8_t) ((role_len >> 8) & 0xFF);
  memcpy (buf + 12, role, role_len);
}

#ifdef __cplusplus
}
#endif

#endif /* NORI_TAG_H */
