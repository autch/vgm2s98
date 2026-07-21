#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "s98.h"

static FILE *g_f;
static const char *g_path;
u32 s98_dump_len;

int s98_open(const char *path)
{
    g_f = fopen(path, "wb");
    if (!g_f) {
        errmsg_pair(path, strerror(errno));
        return -1;
    }
    g_path = path;
    return 0;
}

static void wr32(u32 v)
{
    putc((int)(v & 0xFF), g_f);
    putc((int)((v >> 8) & 0xFF), g_f);
    putc((int)((v >> 16) & 0xFF), g_f);
    putc((int)((v >> 24) & 0xFF), g_f);
}

void s98_write_header(u32 sync_num, u32 sync_den, int ndev,
                      const u32 *types, const u32 *clocks)
{
    int i;
    fwrite("S983", 1, 4, g_f);
    wr32(sync_num);
    wr32(sync_den);
    wr32(0);                        /* compressing (always 0) */
    wr32(0);                        /* tag offset, patched later */
    wr32(S98_DUMP_OFS(ndev));
    wr32(0);                        /* loop offset, patched later */
    wr32((u32)ndev);
    for (i = 0; i < ndev; i++) {
        wr32(types[i]);
        wr32(clocks[i]);
        wr32(0);
        wr32(0);
    }
}

void s98_put(int b)
{
    putc(b, g_f);
    s98_dump_len++;
}

void s98_reg(int dev_port, int addr, int value)
{
    s98_put(dev_port);
    s98_put(addr);
    s98_put(value);
}

/* Encode n syncs as S98 FF / FE+varint commands. */
void s98_syncs(u32 n)
{
    if (n == 0)
        return;
    if (n == 1) {
        s98_put(0xFF);
    } else {
        u32 v = n - 2;              /* spec: getvv() returns n + 2 */
        s98_put(0xFE);
        for (;;) {
            int b = (int)(v & 0x7F);
            v >>= 7;
            if (v) {
                s98_put(b | 0x80);
            } else {
                s98_put(b);
                break;
            }
        }
    }
}

void s98_write_tag(const u8 *tag, u32 len)
{
    fwrite(tag, 1, (size_t)len, g_f);
}

int s98_patch(u32 tag_ofs, u32 loop_ofs)
{
    if (fseek(g_f, 16L, SEEK_SET)) {
        errmsg_pair(g_path, "seek failed");
        return -1;
    }
    wr32(tag_ofs);
    if (fseek(g_f, 24L, SEEK_SET)) {
        errmsg_pair(g_path, "seek failed");
        return -1;
    }
    wr32(loop_ofs);
    return 0;
}

int s98_close(void)
{
    int bad = fflush(g_f) || ferror(g_f);
    fclose(g_f);
    g_f = NULL;
    if (bad) {
        errmsg_pair(g_path, "write error");
        return -1;
    }
    return 0;
}

void s98_abort(void)
{
    if (g_f)
        fclose(g_f);
    g_f = NULL;
}
