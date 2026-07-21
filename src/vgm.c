#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

#include "vgm.h"

static FILE *g_f;
u32 vgm_size;
struct vgm_header vgm_hdr;

u32 rd32le(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16)
        | ((u32)p[3] << 24);
}

int vgm_open(const char *path)
{
    u8 magic[2];
    size_t got;

    g_f = fopen(path, "rb");
    if (!g_f) {
        errmsg_pair(path, strerror(errno));
        return -1;
    }
    got = fread(magic, 1, 2, g_f);
    if (got == 2 && magic[0] == 0x1F && magic[1] == 0x8B) {
#ifdef HAVE_ZLIB
        static u8 buf[16384];
        gzFile gz;
        FILE *tmp;
        int n;
        fclose(g_f);
        g_f = NULL;
        gz = gzopen(path, "rb");
        tmp = tmpfile();
        if (!gz || !tmp) {
            if (gz)
                gzclose(gz);
            if (tmp)
                fclose(tmp);
            errmsg_pair(path, "cannot decompress");
            return -1;
        }
        while ((n = gzread(gz, buf, sizeof buf)) > 0)
            fwrite(buf, 1, (size_t)n, tmp);
        if (n < 0) {
            errmsg_pair(path, "gzip decompression failed");
            gzclose(gz);
            fclose(tmp);
            return -1;
        }
        gzclose(gz);
        if (ferror(tmp)) {
            errmsg_pair(path, "cannot decompress");
            fclose(tmp);
            return -1;
        }
        g_f = tmp;
#else
        errmsg_pair(path, "gzip-compressed input (.vgz) is not "
                    "supported in this build; decompress it first");
        return -1;
#endif
    }
    if (fseek(g_f, 0L, SEEK_END)) {
        errmsg_pair(path, "not seekable");
        return -1;
    }
    vgm_size = (u32)ftell(g_f);
    return 0;
}

void vgm_close(void)
{
    if (g_f)
        fclose(g_f);
    g_f = NULL;
}

int vgm_seek(u32 ofs)
{
    if (fseek(g_f, (long)ofs, SEEK_SET)) {
        errmsg_pair("seek failed", strerror(errno));
        return -1;
    }
    return 0;
}

u32 vgm_read(u8 *buf, u32 n)
{
    return (u32)fread(buf, 1, (size_t)n, g_f);
}

int vgm_getc(void)
{
    int c = getc(g_f);
    return c == EOF ? -1 : c;
}

int vgm_rd_byte(u32 *pos)
{
    int c = getc(g_f);
    if (c == EOF) {
        char buf[40];
        sprintf(buf, "truncated VGM data at 0x%lx", (unsigned long)*pos);
        errmsg_set(buf);
        return -1;
    }
    (*pos)++;
    return c;
}

int vgm_rd_u16(u32 *pos, u32 *out)
{
    int a, b;
    if ((a = vgm_rd_byte(pos)) < 0 || (b = vgm_rd_byte(pos)) < 0)
        return -1;
    *out = (u32)a | ((u32)b << 8);
    return 0;
}

int vgm_rd_u32(u32 *pos, u32 *out)
{
    u32 lo, hi;
    if (vgm_rd_u16(pos, &lo) || vgm_rd_u16(pos, &hi))
        return -1;
    *out = lo | (hi << 16);
    return 0;
}

int vgm_skip(u32 *pos, u32 n)
{
    if (fseek(g_f, (long)n, SEEK_CUR)) {
        errmsg_pair("seek failed", strerror(errno));
        return -1;
    }
    *pos += n;
    return 0;
}

static u32 clock_field(const u8 *h, u32 field)
{
    /* Header fields only exist if the data offset leaves room for them */
    if (field + 4 <= vgm_hdr.data_ofs && field + 4 <= vgm_size)
        return rd32le(h + field) & 0x3FFFFFFFUL;    /* mask dual-chip flags */
    return 0;
}

int vgm_parse_header(void)
{
    u8 h[0x80];
    size_t got;
    u32 version, ofs;

    memset(h, 0, sizeof h);
    if (vgm_seek(0))
        return -1;
    got = fread(h, 1, sizeof h, g_f);
    if (got < 4 || memcmp(h, "Vgm ", 4)) {
        errmsg_set("not a VGM file (bad magic)");
        return -1;
    }
    if (got < 0x40) {
        errmsg_set("truncated VGM header");
        return -1;
    }
    version = rd32le(h + 0x08);
    if (version >= 0x150UL && rd32le(h + 0x34) != 0)
        vgm_hdr.data_ofs = 0x34 + rd32le(h + 0x34);
    else
        vgm_hdr.data_ofs = 0x40;

    ofs = rd32le(h + 0x1C);
    vgm_hdr.loop_ofs = ofs ? 0x1C + ofs : 0;
    ofs = rd32le(h + 0x14);
    vgm_hdr.gd3_ofs = ofs ? 0x14 + ofs : 0;

    vgm_hdr.clk2608 = clock_field(h, 0x48);
    vgm_hdr.clk2203 = clock_field(h, 0x44);
    vgm_hdr.clkay = clock_field(h, 0x74);
    vgm_hdr.ay_type =
        (0x79 <= vgm_hdr.data_ofs && vgm_size > 0x78) ? h[0x78] : 0;
    return 0;
}
