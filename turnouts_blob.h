/*
 * turnouts_blob.h  --  list of turnouts as a compact binary blob
 *
 * Shared by the CS (GBM_CS: encode) and the Pico (WiBiDiB2: decode). No dependency.
 *
 * Blob format (version 1), all numbers little endian:
 *   u8   version (= 1)
 *   u8   count   (0..TB_MAX)
 *   count x {
 *       u16  number      turnout number as shown in Engine Driver (system name LT<number>)
 *       u8   flags       bit 0 = inverted
 *       u8   name_len    0..TB_NAME_MAX-1
 *       char name[name_len]
 *   }
 *
 * The blob travels in MSG_STRING_SET chunks, namespace BIDIB_STRING_NAMESPACE_TURNOUTS,
 * exactly like the roster (header "N;XX" in id 0, Base64 chunks of 18 bytes, echo of every
 * chunk, final status in id 255). Keep the namespace identical on both sides.
 */
#ifndef TURNOUTS_BLOB_H_
#define TURNOUTS_BLOB_H_

#include <stdint.h>
#include <stdbool.h>

#ifndef BIDIB_STRING_NAMESPACE_TURNOUTS
#define BIDIB_STRING_NAMESPACE_TURNOUTS   251   /* 250 = roster */
#endif

#define TB_VERSION   1
#define TB_MAX       32
#define TB_NAME_MAX  32                          /* including the final '\0' */
#define TB_NUM_MIN   1
#define TB_NUM_MAX   2048
#define TB_MAX_RAW   (2 + TB_MAX * (4 + TB_NAME_MAX - 1))

typedef struct {
    uint16_t number;
    uint8_t  inverted;
    char     name[TB_NAME_MAX];
} tb_entry_t;

/* returns the blob length, or 0 if count > TB_MAX or outmax is too small */
uint32_t tb_encode(const tb_entry_t *e, uint8_t count, uint8_t *out, uint32_t outmax);

/* strict decoding: false (and nothing usable in out) if the blob is malformed */
bool tb_decode(const uint8_t *in, uint32_t len, tb_entry_t *out, uint8_t *count);

#endif /* TURNOUTS_BLOB_H_ */
