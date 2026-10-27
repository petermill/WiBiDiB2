/*
 * roster_store.c  —  WiBiDiB (Pico 2W)
 *
 * Disposition : les 4 derniers secteurs de la flash (16 Ko) = 2 emplacements
 * de 8 Ko (A/B). Chaque emplacement : en-tete 16 octets + blob.
 *   magic | seq | len | crc32(blob)
 * On ecrit toujours dans l'emplacement qui n'est PAS le plus recent : une
 * coupure pendant l'ecriture laisse l'ancien roster intact. Au boot, le slot
 * valide (magic + CRC-32) de plus grand seq est utilise.
 */
#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "config.h"
#include "roster.h"
#include "roster_store.h"

static const char *TAG = "roster_store";

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES   (4u * 1024u * 1024u)   // Pico 2 W
#endif

#define SLOT_SIZE       (2u * FLASH_SECTOR_SIZE)       // 8192
#define NSLOTS          2u
#define STORE_BASE      (PICO_FLASH_SIZE_BYTES - NSLOTS * SLOT_SIZE)
#define STORE_MAGIC     0x54534F52u                    // "ROST"
#define SAVE_DELAY_MS   300u    // laisse partir l'echo du dernier chunk avant de figer l'IRQ

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;
    uint32_t len;
    uint32_t crc32;
} store_hdr_t;

#define MAX_BLOB        (SLOT_SIZE - sizeof(store_hdr_t))

static uint8_t  img[SLOT_SIZE] __attribute__((aligned(4)));  // image a ecrire
static bool     pending = false;
static uint32_t pending_t0 = 0;
static uint32_t pending_slot = 0;

static uint32_t now_ms_local(void) { return to_ms_since_boot(get_absolute_time()); }

static uint32_t crc32_calc(const uint8_t *d, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *d++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

static const store_hdr_t *slot_hdr(uint32_t i) {
    return (const store_hdr_t *)(XIP_BASE + STORE_BASE + i * SLOT_SIZE);
}

static bool slot_valid(uint32_t i) {
    const store_hdr_t *h = slot_hdr(i);
    if (h->magic != STORE_MAGIC || h->len == 0 || h->len > MAX_BLOB) return false;
    return crc32_calc((const uint8_t *)(h + 1), h->len) == h->crc32;
}

// -1 si aucun slot valide
static int best_slot(void) {
    bool v0 = slot_valid(0), v1 = slot_valid(1);
    if (!v0 && !v1) return -1;
    if (v0 && !v1) return 0;
    if (v1 && !v0) return 1;
    return ((int32_t)(slot_hdr(1)->seq - slot_hdr(0)->seq) > 0) ? 1 : 0;
}

void roster_store_init_load(void) {
    int s = best_slot();
    if (s < 0) {
        LOG_INFO(TAG, "flash: aucun roster memorise");
        return;
    }
    const store_hdr_t *h = slot_hdr((uint32_t)s);
    LOG_INFO(TAG, "flash: slot %d seq=%lu len=%lu", s, (unsigned long)h->seq, (unsigned long)h->len);
    roster_load((const uint8_t *)(h + 1), h->len);
    LOG_INFO(TAG, "flash: roster recharge, %u locos", roster.count);
}

void roster_store_request_save(const uint8_t *blob, uint32_t len) {
    if (len == 0 || len > MAX_BLOB) {
        LOG_INFO(TAG, "ERR roster trop gros pour la flash: %lu octets", (unsigned long)len);
        return;
    }
    uint32_t crc = crc32_calc(blob, len);
    int best = best_slot();
    if (best >= 0) {
        const store_hdr_t *h = slot_hdr((uint32_t)best);
        if (h->len == len && h->crc32 == crc) {
            LOG_INFO(TAG, "flash: roster identique, pas d'ecriture");
            return;
        }
    }
    store_hdr_t hdr = {
        .magic = STORE_MAGIC,
        .seq   = (best < 0) ? 1u : slot_hdr((uint32_t)best)->seq + 1u,
        .len   = len,
        .crc32 = crc,
    };
    memset(img, 0xFF, sizeof(img));
    memcpy(img, &hdr, sizeof(hdr));
    memcpy(img + sizeof(hdr), blob, len);
    pending_slot = (best < 0) ? 0u : (uint32_t)(1 - best);
    pending_t0   = now_ms_local();
    pending      = true;
    LOG_INFO(TAG, "flash: ecriture programmee (slot %lu, %lu octets)",
             (unsigned long)pending_slot, (unsigned long)len);
}

static void __no_inline_not_in_flash_func(program_cb)(void *param) {
    (void)param;
    uint32_t off = STORE_BASE + pending_slot * SLOT_SIZE;
    flash_range_erase(off, SLOT_SIZE);
    flash_range_program(off, img, SLOT_SIZE);
}

void roster_store_poll(void) {
    if (!pending) return;
    if ((now_ms_local() - pending_t0) < SAVE_DELAY_MS) return;
    pending = false;

    uint32_t t0 = now_ms_local();
    int rc = flash_safe_execute(program_cb, NULL, 1000);
    uint32_t dt = now_ms_local() - t0;
    if (rc != PICO_OK) {
        LOG_INFO(TAG, "ERR flash_safe_execute rc=%d", rc);
        return;
    }
    LOG_INFO(TAG, "flash: ecrit en %lu ms, verification %s",
             (unsigned long)dt, slot_valid(pending_slot) ? "OK" : "ECHEC");
}
