/* VGM input: seekable file access (transparently gunzipped when built
 * with zlib) and header parsing. */
#ifndef VGM_H
#define VGM_H

#include "vgm2s98.h"

struct vgm_header {
    u32 data_ofs;
    u32 loop_ofs, gd3_ofs;          /* absolute file offsets; 0 = absent */
    u32 clk2608, clk2203, clkay;    /* 0 = header field absent/zero */
    int ay_type;
};

extern struct vgm_header vgm_hdr;
extern u32 vgm_size;

int vgm_open(const char *path);
void vgm_close(void);
int vgm_parse_header(void);

int vgm_seek(u32 ofs);
u32 vgm_read(u8 *buf, u32 n);       /* returns the count actually read */
int vgm_getc(void);                 /* next byte, or -1 at EOF */

/* sequential readers for the command walker; they keep *pos in step
 * with the file position and report truncation via g_errmsg */
int vgm_rd_byte(u32 *pos);
int vgm_rd_u16(u32 *pos, u32 *out);
int vgm_rd_u32(u32 *pos, u32 *out);
int vgm_skip(u32 *pos, u32 n);

u32 rd32le(const u8 *p);

#endif
