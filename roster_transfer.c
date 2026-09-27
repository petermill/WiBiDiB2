/*
 * roster_transfer.c  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Réassemble le roster transmis par le CS en morceaux via MSG_STRING_SET
 * (namespace BIDIB_STRING_NAMESPACE_ROSTER) :
 *   - STRING_ID 0   : en-tête texte "N;XX" (N=nb chunks décimal, XX=CRC8 hex)
 *   - STRING_ID 1..N: chunks Base64 de 24 caractères (18 octets bruts) du
 *                     blob binaire décrit dans roster.c / roster_load()
 *
 * Chaque chunk reçu est acquitté par un écho MSG_STRING identique (même
 * convention que user_string_store()/bidib_send_string_username()) ; le CS
 * attend cet écho avant d'envoyer le chunk suivant (stop-and-wait).
 */
#include <string.h>
#include <stdio.h>
#include "bidib_client_parser.h"
#include "roster_transfer.h"
#include "roster.h"
#include "bidib_messages.h"
#include "bidib_client_if.h"   // bidib_get_tx_num() / send_bidib_message()
#include "config.h"
#include "crc_8bit.h"         

static const char *TAG = "roster_xfer";

#define ROSTER_CHUNK_RAW_BYTES   18      // 24 car. base64 -> 18 octets bruts
#define ROSTER_MAX_CHUNKS        254     // STRING_ID 1..254 (0 = en-tête)
#define ROSTER_MAX_RAW_BYTES     (ROSTER_MAX_CHUNKS * ROSTER_CHUNK_RAW_BYTES)

typedef enum { XFER_IDLE, XFER_RECEIVING } xfer_state_t;

static xfer_state_t state        = XFER_IDLE;
static uint8_t       raw_buf[ROSTER_MAX_RAW_BYTES];
static uint32_t       write_offset = 0;
static uint8_t        total_chunks = 0;
static uint8_t        expected_crc = 0;
static uint8_t         next_chunk  = 1;

// ─── Décodage Base64 (RFC 4648, sans retours à la ligne) ───────────────────

static int8_t b64_val(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;  // '=' de padding ou caractère invalide
}

// Décode `in` (len caractères) dans out, retourne le nb d'octets décodés.
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

// ─── Emission de l'écho MSG_STRING (sert d'accusé de réception) ───────────

static void send_string_echo(uint8_t ns, uint8_t id, const uint8_t *data, uint8_t sz) {
    uint8_t message[32];
    uint8_t nb_data = 3 + sz;           // ns + id + size + octets de donnees

    message[0] = 1 + 1 + 1 + nb_data;   // addr + index + type + data
    message[1] = 0x00;                  // addr vers le host, toujours 0
    message[2] = bidib_get_tx_num();    // index
    message[3] = MSG_STRING;            // type
    message[4] = ns;
    message[5] = id;
    message[6] = sz;
    memcpy(&message[7], data, sz);

    send_bidib_message(message);
}

static void reset_transfer(void) {
    state        = XFER_IDLE;
    write_offset = 0;
    next_chunk   = 1;
}

// ─── Point d'entrée appelé depuis bidib_client_parser.c ───────────────────
static uint8_t crc8_buffer(const uint8_t *data, uint32_t len) {
    uint8_t crc = 0x00;   // valeur initiale standard Dallas/iButton
    for (uint32_t i = 0; i < len; i++) {
        crc = crc8_update(crc, data[i]);
    }
    return crc;
}

void roster_transfer_handle_chunk(uint8_t string_id, const uint8_t *data, uint8_t size) {

    if (string_id == 0) {
        // En-tête : "N;XX"
        char hdr[25];
        uint8_t n = size < sizeof(hdr) - 1 ? size : sizeof(hdr) - 1;
        memcpy(hdr, data, n);
        hdr[n] = '\0';

        unsigned int nchunks = 0, crc = 0;
        if (sscanf(hdr, "%u;%x", &nchunks, &crc) != 2
            || nchunks == 0 || nchunks > ROSTER_MAX_CHUNKS) {
            LOG_WARN(TAG, "roster header invalide: \"%s\"", hdr);
            reset_transfer();
            return;
        }
        total_chunks = (uint8_t)nchunks;
        expected_crc = (uint8_t)crc;
        write_offset = 0;
        next_chunk   = 1;
        state        = XFER_RECEIVING;
        LOG_INFO(TAG, "roster transfer start: %u chunks, crc=0x%02X", total_chunks, expected_crc);

        send_string_echo(BIDIB_STRING_NAMESPACE_ROSTER, 0, data, size);
        return;
    }

    if (state != XFER_RECEIVING || string_id != next_chunk) {
        LOG_WARN(TAG, "roster chunk %u inattendu (attendu=%u)", string_id, next_chunk);
        return;   // pas d'écho -> le CS retentera après timeout
    }

    if (write_offset + ROSTER_CHUNK_RAW_BYTES > ROSTER_MAX_RAW_BYTES) {
        LOG_ERROR(TAG, "roster buffer overflow, transfert annule");
        reset_transfer();
        return;
    }

    write_offset += b64_decode(data, size, &raw_buf[write_offset]);
    next_chunk++;
    send_string_echo(BIDIB_STRING_NAMESPACE_ROSTER, string_id, data, size);

    if (string_id == total_chunks) {
        uint8_t crc = crc8_buffer(raw_buf, write_offset);  
        if (crc == expected_crc) {
            LOG_INFO(TAG, "roster transfer complete: %lu octets, CRC OK", write_offset);
            roster_load(raw_buf, write_offset);
            LOG_INFO(TAG, "roster loaded: %u locos, first=%s (addr=%u)",
            roster.count,
            roster.count > 0 ? roster.entries[0].id : "(vide)",
            roster.count > 0 ? roster.entries[0].dccAddress : 0);
        } else {
            LOG_ERROR(TAG, "roster CRC mismatch (recu=0x%02X attendu=0x%02X)", crc, expected_crc);
        }
        reset_transfer();
    }
}