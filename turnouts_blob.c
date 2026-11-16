/*
 * turnouts_blob.c  --  see turnouts_blob.h
 */
#include <string.h>
#include "turnouts_blob.h"

uint32_t tb_encode(const tb_entry_t *e, uint8_t count, uint8_t *out, uint32_t outmax)
{
    if (count > TB_MAX) return 0;

    uint32_t need = 2;
    for (uint8_t i = 0; i < count; i++)
        need += 4 + (uint32_t)strnlen(e[i].name, TB_NAME_MAX - 1);
    if (need > outmax) return 0;

    uint8_t *p = out;
    *p++ = TB_VERSION;
    *p++ = count;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t n = (uint8_t)strnlen(e[i].name, TB_NAME_MAX - 1);
        *p++ = (uint8_t)(e[i].number & 0xFF);
        *p++ = (uint8_t)(e[i].number >> 8);
        *p++ = e[i].inverted ? 0x01 : 0x00;
        *p++ = n;
        memcpy(p, e[i].name, n);
        p += n;
    }
    return (uint32_t)(p - out);
}

static bool name_char_ok(uint8_t c)
{
    /* control characters and the WiThrottle separators are never accepted */
    if (c < 0x20 || c == 0x7F) return false;
    return !(c == '"' || c == '|' || c == '{' || c == '}' || c == '[' || c == ']');
}

bool tb_decode(const uint8_t *in, uint32_t len, tb_entry_t *out, uint8_t *count)
{
    if (len < 2 || in[0] != TB_VERSION) return false;
    uint8_t n = in[1];
    if (n > TB_MAX) return false;

    const uint8_t *p = in + 2, *end = in + len;
    for (uint8_t i = 0; i < n; i++) {
        if (end - p < 4) return false;
        uint16_t num   = (uint16_t)(p[0] | (p[1] << 8));
        uint8_t  flags = p[2];
        uint8_t  nl    = p[3];
        p += 4;
        if (num < TB_NUM_MIN || num > TB_NUM_MAX) return false;
        if (nl > TB_NAME_MAX - 1 || end - p < nl) return false;
        for (uint8_t k = 0; k < nl; k++)
            if (!name_char_ok(p[k])) return false;
        for (uint8_t k = 0; k < i; k++)
            if (out[k].number == num) return false;          /* duplicate number */

        out[i].number   = num;
        out[i].inverted = (flags & 0x01) ? 1 : 0;
        memcpy(out[i].name, p, nl);
        out[i].name[nl] = '\0';
        p += nl;
    }
    if (p != end) return false;                               /* trailing bytes */
    *count = n;
    return true;
}
