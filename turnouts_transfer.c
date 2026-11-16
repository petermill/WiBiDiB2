/*
 * turnouts_transfer.c  --  Pico 2W WiThrottle/BiDiB gateway
 *
 * Reassembles the turnout list sent by the CS in MSG_STRING_SET chunks
 * (namespace BIDIB_STRING_NAMESPACE_TURNOUTS):
 *   - STRING_ID 0     : text header "N;XX" (N = number of chunks, XX = CRC8 in hex)
 *   - STRING_ID 1..N  : Base64 chunks of 24 characters (18 raw bytes) of the blob
 *                       described in turnouts_blob.h
 *   - STRING_ID 255   : final status sent by the Pico: "OK;XX", "LD;XX" (blob refused) or "ER;XX" (CRC)
 *
 * Every chunk is acknowledged by an identical MSG_STRING echo (stop-and-wait), like roster_transfer.c.
 * The turnouts are not saved in flash: the CS sends them again at every subscription.
 */
#include <string.h>
#include <stdio.h>
#include "bidib_client_parser.h"
#include "turnouts_transfer.h"
#include "bidib_messages.h"
#include "bidib_client_if.h"   // bidib_get_tx_num() / send_bidib_message()
#include "config.h"
#include "crc_8bit.h"

static const char *TAG = "turn_xfer";

#define TT_CHUNK_RAW_BYTES   18          // 24 Base64 characters -> 18 raw bytes
#define TT_MAX_CHUNKS        80          // must be identical on the CS (turnouts_sender.c)
#define TT_MAX_RAW_BYTES     (TT_MAX_CHUNKS * TT_CHUNK_RAW_BYTES)
#define TT_STATUS_ID         255

_Static_assert(TB_MAX_RAW <= TT_MAX_RAW_BYTES, "turnout blob does not fit in the transfer buffer");

typedef enum { XFER_IDLE, XFER_RECEIVING } xfer_state_t;

static xfer_state_t state        = XFER_IDLE;
static uint8_t      raw_buf[TT_MAX_RAW_BYTES];
static uint32_t     write_offset = 0;
static uint8_t      total_chunks = 0;
static uint8_t      expected_crc = 0;
static uint8_t      next_chunk   = 1;
static uint8_t      last_acked   = 0xFF;   // last STRING_ID acknowledged (0xFF = none)

static int8_t b64_val(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;  // '=' padding or invalid character
}

static uint32_t b64_decode(const uint8_t *in, uint8_t len, uint8_t *out) {
    uint32_t n = 0, acc = 0;
    int bits = 0;
    for (uint8_t i = 0; i < len; i++) {
        int8_t v = b64_val(in[i]);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return n;
}

static void send_string_echo(uint8_t ns, uint8_t id, const uint8_t *data, uint8_t sz) {
    uint8_t message[32];
    uint8_t nb_data = 3 + sz;           // ns + id + size + data bytes

    message[0] = 1 + 1 + 1 + nb_data;   // addr + index + type + data
    message[1] = 0x00;                  // address towards the host, always 0
    message[2] = bidib_get_tx_num();    // index
    message[3] = MSG_STRING;            // type
    message[4] = ns;
    message[5] = id;
    message[6] = sz;
    memcpy(&message[7], data, sz);
    send_bidib_message(message);
}

static void send_status(const char *tag, uint8_t crc) {
    char s[8];
    int n = snprintf(s, sizeof(s), "%s;%02X", tag, crc);
    send_string_echo(BIDIB_STRING_NAMESPACE_TURNOUTS, TT_STATUS_ID, (const uint8_t *)s, (uint8_t)n);
}

static void reset_transfer(void) {
    state        = XFER_IDLE;
    write_offset = 0;
    next_chunk   = 1;
}

static uint8_t crc8_buffer(const uint8_t *data, uint32_t len) {
    uint8_t crc = 0x00;
    for (uint32_t i = 0; i < len; i++) crc = crc8_update(crc, data[i]);
    return crc;
}

void turnouts_transfer_handle_chunk(uint8_t string_id, const uint8_t *data, uint8_t size) {

    // guard: a Base64 chunk has at most 24 characters (message[32] on the echo side)
    if (size > 24) {
        LOG_WARN(TAG, "chunk %u: taille %u invalide", string_id, size);
        return;
    }

    if (string_id == 0) {
        // header: "N;XX"
        char hdr[25];
        uint8_t n = size < sizeof(hdr) - 1 ? size : sizeof(hdr) - 1;
        memcpy(hdr, data, n);
        hdr[n] = '\0';

        unsigned int nchunks = 0, crc = 0;
        if (sscanf(hdr, "%u;%x", &nchunks, &crc) != 2
            || nchunks == 0 || nchunks > TT_MAX_CHUNKS) {
            LOG_WARN(TAG, "header invalide: \"%s\"", hdr);
            reset_transfer();
            return;
        }
        total_chunks = (uint8_t)nchunks;
        expected_crc = (uint8_t)crc;
        write_offset = 0;
        next_chunk   = 1;
        state        = XFER_RECEIVING;
        last_acked   = 0;
        LOG_INFO(TAG, "transfert aiguillages: %u chunks, crc=0x%02X", total_chunks, expected_crc);

        send_string_echo(BIDIB_STRING_NAMESPACE_TURNOUTS, 0, data, size);
        return;
    }

    // duplicate: the CS re-sent a chunk already processed (our echo was lost): re-acknowledge only
    if (string_id == last_acked) {
        LOG_INFO(TAG, "chunk %u en double, re-ACK", string_id);
        send_string_echo(BIDIB_STRING_NAMESPACE_TURNOUTS, string_id, data, size);
        return;
    }

    if (state != XFER_RECEIVING || string_id != next_chunk) {
        LOG_WARN(TAG, "chunk %u inattendu (attendu=%u)", string_id, next_chunk);
        return;   // no echo: the CS will retry after its timeout
    }

    if (write_offset + ((uint32_t)size * 3) / 4 > TT_MAX_RAW_BYTES) {
        LOG_ERROR(TAG, "buffer overflow, transfert annule");
        reset_transfer();
        return;
    }

    write_offset += b64_decode(data, size, &raw_buf[write_offset]);
    next_chunk++;
    last_acked = string_id;
    send_string_echo(BIDIB_STRING_NAMESPACE_TURNOUTS, string_id, data, size);

    if (string_id == total_chunks) {
        uint8_t crc = crc8_buffer(raw_buf, write_offset);
        if (crc == expected_crc) {
            static tb_entry_t list[TB_MAX];
            uint8_t count = 0;
            if (tb_decode(raw_buf, write_offset, list, &count) && withrottle_turnouts_set(list, count)) {
                LOG_INFO(TAG, "aiguillages recus: %u, CRC OK", count);
                send_status("OK", crc);
            } else {
                LOG_ERROR(TAG, "liste d'aiguillages inutilisable (%lu octets)", (unsigned long)write_offset);
                send_status("LD", crc);
            }
        } else {
            LOG_ERROR(TAG, "CRC mismatch (recu=0x%02X attendu=0x%02X)", crc, expected_crc);
            send_status("ER", crc);
        }
        reset_transfer();
    }
}
