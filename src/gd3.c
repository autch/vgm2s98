#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gd3.h"
#include "vgm.h"

u8 *gd3_tag;
u32 gd3_tag_len;

static char *utf8_put(char *p, u32 cp)
{
    if (cp < 0x80UL) {
        *p++ = (char)cp;
    } else if (cp < 0x800UL) {
        *p++ = (char)(0xC0 | (cp >> 6));
        *p++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000UL) {
        *p++ = (char)(0xE0 | (cp >> 12));
        *p++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *p++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *p++ = (char)(0xF0 | (cp >> 18));
        *p++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *p++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *p++ = (char)(0x80 | (cp & 0x3F));
    }
    return p;
}

static const char *pick(const char *jp, const char *en)
{
    return jp[0] ? jp : en;
}

/* Build the [S98] tag from the GD3 tag, if any.  Only fails on
 * out-of-memory or a read error; a missing/bad GD3 just means no tag. */
int gd3_build(void)
{
    static const char *keys[7] = {
        "title", "game", "artist", "system", "year", "s98by", "comment"
    };
    const char *fields[11], *vals[7];
    u8 gh[12], *payload, *dec;
    char *p;
    u32 length, avail, plen, nunits, i;
    u32 need;

    if (!vgm_hdr.gd3_ofs || vgm_hdr.gd3_ofs + 12 > vgm_size)
        return 0;
    if (vgm_seek(vgm_hdr.gd3_ofs) || vgm_read(gh, 12) != 12)
        return 0;
    if (memcmp(gh, "Gd3 ", 4))
        return 0;
    length = rd32le(gh + 8);
    avail = vgm_size - (vgm_hdr.gd3_ofs + 12);
    plen = length < avail ? length : avail;

    payload = (u8 *)malloc(plen ? plen : 1);
    nunits = plen / 2;
    dec = (u8 *)malloc(3 * nunits + 8);
    if (!payload || !dec) {
        errmsg_set("out of memory");
        free(payload);
        free(dec);
        return -1;
    }
    if (vgm_read(payload, plen) != plen) {
        errmsg_set("read error in GD3 tag");
        free(payload);
        free(dec);
        return -1;
    }

    /* decode UTF-16LE with U+FFFD replacement (Python errors="replace") */
    p = (char *)dec;
    i = 0;
    while (i < nunits) {
        u32 cp;
        unsigned u = payload[2 * i] | ((unsigned)payload[2 * i + 1] << 8);
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 1 < nunits) {
                unsigned lo = payload[2 * i + 2]
                    | ((unsigned)payload[2 * i + 3] << 8);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000UL
                        + (((u32)(u - 0xD800) << 10) | (lo - 0xDC00));
                    i += 2;
                } else {
                    cp = 0xFFFDUL;
                    i++;
                }
            } else {
                cp = 0xFFFDUL;
                i++;
            }
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            cp = 0xFFFDUL;
            i++;
        } else {
            cp = u;
            i++;
        }
        p = utf8_put(p, cp);
    }
    if (plen & 1)
        p = utf8_put(p, 0xFFFDUL);
    *p = '\0';
    free(payload);

    /* split on NUL into up to 11 fields; missing fields are "" */
    for (i = 0; i < 11; i++)
        fields[i] = "";
    {
        char *q = (char *)dec, *endp = p;
        u32 nf = 0;
        while (nf < 11) {
            fields[nf++] = q;
            q += strlen(q);
            if (q >= endp)
                break;
            q++;
        }
    }
    /* GD3 order: track en/jp, game en/jp, system en/jp, author en/jp,
     * date, ripper, notes */
    vals[0] = pick(fields[1], fields[0]);
    vals[1] = pick(fields[3], fields[2]);
    vals[2] = pick(fields[7], fields[6]);
    vals[3] = pick(fields[5], fields[4]);
    vals[4] = fields[8];
    vals[5] = fields[9];
    vals[6] = "converted by vgm2s98";

    need = 8 + 1;                   /* "[S98]" + BOM ... final NUL */
    for (i = 0; i < 7; i++)
        if (vals[i][0])
            need += (u32)strlen(keys[i]) + 1 + (u32)strlen(vals[i]) + 1;
    gd3_tag = (u8 *)malloc(need);
    if (!gd3_tag) {
        errmsg_set("out of memory");
        free(dec);
        return -1;
    }
    p = (char *)gd3_tag;
    memcpy(p, "[S98]\357\273\277", 8);
    p += 8;
    for (i = 0; i < 7; i++) {
        size_t l;
        if (!vals[i][0])
            continue;
        l = strlen(keys[i]);
        memcpy(p, keys[i], l);
        p += l;
        *p++ = '=';
        l = strlen(vals[i]);
        memcpy(p, vals[i], l);
        p += l;
        *p++ = '\x0a';
    }
    *p++ = '\0';
    gd3_tag_len = (u32)(p - (char *)gd3_tag);
    free(dec);
    return 0;
}

void gd3_free(void)
{
    free(gd3_tag);
    gd3_tag = NULL;
    gd3_tag_len = 0;
}
