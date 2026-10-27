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
#include "roster_store.h"

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
static uint8_t         last_acked  = 0xFF;   // dernier STRING_ID acquitte (0xFF = aucun)

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
LOG_INFO(TAG, "send_string_echo: plength=%u, ns=%u, id=%u, size=%u", message[0], message[4], message[5], message[6]);
    send_bidib_message(message);
}

#define ROSTER_STATUS_ID   255

static void send_status(const char *tag, uint8_t crc) {
    char s[8];
    int n = snprintf(s, sizeof(s), "%s;%02X", tag, crc);
    send_string_echo(BIDIB_STRING_NAMESPACE_ROSTER, ROSTER_STATUS_ID,
                     (const uint8_t *)s, (uint8_t)n);
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

void roster_dump(void) {
    printf("[roster] === %u locos ===\n", roster.count);
    for (uint8_t i = 0; i < roster.count; i++) {
        const roster_entry_t *e = &roster.entries[i];
        printf("[roster] #%02u %-16s addr=%u%s road=%s/%s mfg=%s model=%s dec=%s/%s vmax=%u fnmax=%u\n",
               i, e->id, e->dccAddress, e->longAddress ? "L" : "S",
               e->roadName, e->roadNumber, e->mfg, e->model,
               e->decoderModel, e->decoderFamily, e->maxSpeed, e->maxFnNum);
        for (uint8_t f = 0; f <= ROSTER_FUNC_MAX; f++) {
            if (e->functions[f].label[0] == '\0') continue;
            printf("[roster]      F%-2u %c%c %s\n", f,
                   e->functions[f].lockable ? 'L' : '-',
                   e->functions[f].visible  ? 'V' : '-',
                   e->functions[f].label);
        }
    }
}

void roster_transfer_handle_chunk(uint8_t string_id, const uint8_t *data, uint8_t size) {

    // Garde : un chunk Base64 fait au plus 24 caracteres (message[32] cote echo)
    if (size > 24) {
        LOG_WARN(TAG, "roster chunk %u: taille %u invalide", string_id, size);
        return;
    }

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
        last_acked   = 0;
        LOG_INFO(TAG, "roster transfer start: %u chunks, crc=0x%02X", total_chunks, expected_crc);

        send_string_echo(BIDIB_STRING_NAMESPACE_ROSTER, 0, data, size);
        return;
    }

    // Doublon : le CS a retransmis un chunk deja traite (notre echo s'est perdu).
    // On re-acquitte sans le retraiter, meme si le transfert est deja termine.
    if (string_id == last_acked) {
        LOG_INFO(TAG, "roster chunk %u en double, re-ACK", string_id);
        send_string_echo(BIDIB_STRING_NAMESPACE_ROSTER, string_id, data, size);
        return;
    }

    if (state != XFER_RECEIVING || string_id != next_chunk) {
        LOG_WARN(TAG, "roster chunk %u inattendu (attendu=%u)", string_id, next_chunk);
        return;   // pas d'écho -> le CS retentera après timeout
    }

    if (write_offset + ((uint32_t)size * 3) / 4 > ROSTER_MAX_RAW_BYTES) {
        LOG_ERROR(TAG, "roster buffer overflow, transfert annule");
        reset_transfer();
        return;
    }

    write_offset += b64_decode(data, size, &raw_buf[write_offset]);
    next_chunk++;
    last_acked = string_id;
    send_string_echo(BIDIB_STRING_NAMESPACE_ROSTER, string_id, data, size);

    if (string_id == total_chunks) {
        uint8_t crc = crc8_buffer(raw_buf, write_offset);  
        if (crc == expected_crc) {
            LOG_INFO(TAG, "roster transfer complete: %lu octets, CRC OK", write_offset);
          bool loaded = roster_load(raw_buf, write_offset);   // si roster_load est void : bool loaded = true; roster_load(...);
            if (loaded) {
                roster_store_request_save(raw_buf, write_offset);
                send_status("OK", crc);
            } else {
                send_status("LD", crc);
            }
            LOG_INFO(TAG, "roster loaded: %u locos", roster.count);
        } else {
            LOG_ERROR(TAG, "roster CRC mismatch (recu=0x%02X attendu=0x%02X)", crc, expected_crc);
            send_status("ER", crc);
        }
        reset_transfer();
    }
}