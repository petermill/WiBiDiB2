/*
 * roster.c  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Roster data model for EngineDriver HTTP access.
 * Le roster peut être initialisé avec des données de démonstration
 * (roster_init) ou chargé dynamiquement depuis le CS via le transfert
 * BiDiB MSG_STRING (roster_load, voir roster_transfer.c).
 *
 * Pierre Moulin / (base: Andreas Kuhtz)
 */

#include <stdio.h>
#include <string.h>
#include "roster.h"
#include "config.h"

static const char *TAG = "roster";

roster_t roster;

// ─── Construction interne (paramétrée par destination) ────────────────────

static bool add_entry_to(roster_t *r, const char *id, const char *roadName, const char *roadNumber,
                          const char *mfg, const char *model,
                          const char *decoderModel, const char *decoderFamily,
                          uint16_t dccAddr, bool longAddr, uint8_t maxSpeed,
                          uint8_t maxFnNum) {
    if (r->count >= ROSTER_MAX_ENTRIES) return false;
    roster_entry_t *e = &r->entries[r->count];
    memset(e, 0, sizeof(*e));
    strncpy(e->id, id, ROSTER_STRING_MAX - 1);
    strncpy(e->roadName, roadName, ROSTER_STRING_MAX - 1);
    strncpy(e->roadNumber, roadNumber, ROSTER_STRING_MAX - 1);
    strncpy(e->mfg, mfg, ROSTER_STRING_MAX - 1);
    strncpy(e->model, model, ROSTER_STRING_MAX - 1);
    strncpy(e->decoderModel, decoderModel, ROSTER_STRING_MAX - 1);
    strncpy(e->decoderFamily, decoderFamily, ROSTER_STRING_MAX - 1);
    e->dccAddress   = dccAddr;
    e->longAddress  = longAddr;
    e->maxSpeed     = maxSpeed;
    e->maxFnNum     = maxFnNum;
    r->count++;
    return true;
}

static void set_func_in(roster_t *r, uint8_t locoIndex, uint8_t fn, bool lockable,
                         bool visible, const char *label) {
    if (locoIndex >= r->count) return;
    if (fn > ROSTER_FUNC_MAX) return;
    roster_entry_t *e = &r->entries[locoIndex];
    e->functions[fn].lockable = lockable;
    e->functions[fn].visible  = visible;
    strncpy(e->functions[fn].label, label, ROSTER_STRING_MAX - 1);
}

// ─── Wrappers historiques (utilisés par roster_init) ───────────────────────

static void add_entry(const char *id, const char *roadName, const char *roadNumber,
                       const char *mfg, const char *model,
                       const char *decoderModel, const char *decoderFamily,
                       uint16_t dccAddr, bool longAddr, uint8_t maxSpeed,
                       uint8_t maxFnNum) {
    (void)add_entry_to(&roster, id, roadName, roadNumber, mfg, model,
                 decoderModel, decoderFamily, dccAddr, longAddr, maxSpeed, maxFnNum);
}

static void set_func(uint8_t locoIndex, uint8_t fn, bool lockable,
                      bool visible, const char *label) {
    set_func_in(&roster, locoIndex, fn, lockable, visible, label);
}

// ─── Roster de démonstration (fallback si rien reçu du CS) ────────────────

void roster_init(void) {
    memset(&roster, 0, sizeof(roster));

    // ── Loco 0: ESU LokSound 5 diesel ────────────────────────────────────
    add_entry("BR 185 203-1", "DB", "185 203-1", "TRIX", "TRAXX F140 AC",
              "ESU LokSound 5", "ESU LokSound 5", 6, false, 100, 6);
    set_func(0, 0,  true,  true,  "Headlight");
    set_func(0, 1,  true, true,  "Bell");
    set_func(0, 2,  false, true,  "Horn");
    set_func(0, 3,  true, true,  "Compressor");
    set_func(0, 4,  true, true,  "Cab Light");
    set_func(0, 5,  true, true,  "Ditch Lights");

    // ── Loco 1: LokSound 5 steam ─────────────────────────────────────────
    add_entry("BR 01 1066-9", "DB", "01 1066-9", "ROCO", "BR 01",
              "ESU LokSound 5", "ESU LokSound 5", 1, false, 100, 8);
    set_func(1, 0,  true,  true,  "Headlight");
    set_func(1, 1,  false, true,  "Bell");
    set_func(1, 2,  false, true,  "Whistle");
    set_func(1, 3,  true, true,  "Steam Blowoff");
    set_func(1, 4,  true, true,  "Safety Valve");
    set_func(1, 5,  true, true,  "Oil Cup");
    set_func(1, 6,  true, true,  "Generator");
    set_func(1, 7,  true, true,  "Cab Light");

    // ── Loco 2: NCE decoder, US diesel ────────────────────────────────────
    add_entry("UP 4014", "Union Pacific", "4014", "Athearn", "DD40AX Big Boy",
              "NCE DCC Gold", "NCE", 4014, true, 100, 6);
    set_func(2, 0,  true,  true,  "Headlight");
    set_func(2, 1,  true, true,  "Bell");
    set_func(2, 2,  false, true,  "Horn");
    set_func(2, 3,  true, true,  "Ditch Lights");
    set_func(2, 4,  true, true,  "Cab Light");
    set_func(2, 5,  true, true,  "Dynamic Brakes");
}

// ─── Accesseurs ─────────────────────────────────────────────────────────

const roster_entry_t* roster_get(uint8_t index) {
    if (index >= roster.count) return NULL;
    return &roster.entries[index];
}

int roster_find_by_id(const char* id) {
    for (uint8_t i = 0; i < roster.count; i++) {
        if (strcmp(roster.entries[i].id, id) == 0) return i;
    }
    return -1;
}

// ─── Chargement dynamique depuis le CS (via roster_transfer.c) ────────────
//
// Format binaire attendu dans buf[0..len-1] :
//   LOCO_COUNT(u8) LOCO_ENTRY[LOCO_COUNT]
//   LOCO_ENTRY := STR(id) STR(roadName) STR(roadNumber) STR(mfg) STR(model)
//                 STR(decoderModel) STR(decoderFamily)
//                 dccAddress(u16 LE) flags(u8, bit0=longAddress)
//                 maxSpeed(u8) maxFnNum(u8)
//                 funcCount(u8) FUNC_ENTRY[funcCount]
//   FUNC_ENTRY  := fn(u8) flags(u8, bit0=lockable, bit1=visible) STR(label)
//   STR         := len(u8) bytes[len]   (pas de terminateur nul)

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    bool err;
} reader_t;

static uint8_t rd_u8(reader_t *r) {
    if (r->p >= r->end) { r->err = true; return 0; }
    return *r->p++;
}

static uint16_t rd_u16(reader_t *r) {
    uint8_t lo = rd_u8(r);
    uint8_t hi = rd_u8(r);
    return (uint16_t)lo | ((uint16_t)hi << 8);
}

static void rd_str(reader_t *r, char *out, uint8_t outmax) {
    uint8_t n = rd_u8(r);
    if (r->err) { out[0] = '\0'; return; }
    if (r->p + n > r->end) { r->err = true; out[0] = '\0'; return; }
    uint8_t copy = n < (outmax - 1) ? n : (outmax - 1);
    memcpy(out, r->p, copy);
    out[copy] = '\0';
    r->p += n;   // avance toujours de n, même si la copie a été tronquée
}

bool roster_load(const uint8_t *buf, uint32_t len) {
    reader_t r = { buf, buf + len, false };
    // static : roster_t est gros (ROSTER_MAX_ENTRIES x entree), la pile du Pico
    // est petite. Pas de reentrance ici (appel unique depuis roster_transfer).
    static roster_t tmp;
    memset(&tmp, 0, sizeof(tmp));

    uint8_t loco_count = rd_u8(&r);
    uint8_t dropped = 0;
    for (uint8_t i = 0; i < loco_count && !r.err; i++) {
        char id[ROSTER_STRING_MAX], roadName[ROSTER_STRING_MAX], roadNumber[ROSTER_STRING_MAX];
        char mfg[ROSTER_STRING_MAX], model[ROSTER_STRING_MAX];
        char decoderModel[ROSTER_STRING_MAX], decoderFamily[ROSTER_STRING_MAX];

        rd_str(&r, id, sizeof(id));
        rd_str(&r, roadName, sizeof(roadName));
        rd_str(&r, roadNumber, sizeof(roadNumber));
        rd_str(&r, mfg, sizeof(mfg));
        rd_str(&r, model, sizeof(model));
        rd_str(&r, decoderModel, sizeof(decoderModel));
        rd_str(&r, decoderFamily, sizeof(decoderFamily));

        uint16_t dccAddr   = rd_u16(&r);
        uint8_t  flags     = rd_u8(&r);
        uint8_t  maxSpeed  = rd_u8(&r);
        uint8_t  maxFnNum  = rd_u8(&r);
        uint8_t  funcCount = rd_u8(&r);
        if (r.err) break;

        // Si le roster est plein, la loco est ignoree ET ses fonctions aussi
        // (sinon elles ecraseraient celles de la derniere loco valide).
        bool added = add_entry_to(&tmp, id, roadName, roadNumber, mfg, model,
                                  decoderModel, decoderFamily, dccAddr,
                                  (flags & 0x01) != 0, maxSpeed, maxFnNum);
        if (!added) dropped++;

        for (uint8_t f = 0; f < funcCount && !r.err; f++) {
            uint8_t fn     = rd_u8(&r);
            uint8_t fflags = rd_u8(&r);
            char label[ROSTER_STRING_MAX];
            rd_str(&r, label, sizeof(label));
            if (r.err) break;
            if (added) {
                set_func_in(&tmp, tmp.count - 1, fn,
                            (fflags & 0x01) != 0,   // lockable
                            (fflags & 0x02) != 0,   // visible
                            label);
            }
        }
    }

    if (r.err) {
        LOG_ERROR(TAG, "roster_load: donnees corrompues, roster actif inchange");
        return false;
    }
    if (dropped) {
        LOG_WARN(TAG, "roster_load: %u locos ignorees (max %u)", dropped, (unsigned)ROSTER_MAX_ENTRIES);
    }

    memcpy(&roster, &tmp, sizeof(roster));
    LOG_INFO(TAG, "roster_load: %u locos charges", roster.count);
    return true;
}
