/* S98 output: header/device table, dump bytes, sync encoding. */
#ifndef S98_H
#define S98_H

#include "vgm2s98.h"

/* S98 device type ids */
#define S98_OPN 2UL
#define S98_OPNA 4UL
#define S98_PSG_YM2149 1UL
#define S98_PSG_AY8910 15UL

#define S98_DUMP_OFS(ndev) (0x20UL + 0x10UL * (u32)(ndev))

extern u32 s98_dump_len;            /* dump bytes emitted so far */

int s98_open(const char *path);
void s98_write_header(u32 sync_num, u32 sync_den, int ndev,
                      const u32 *types, const u32 *clocks);

void s98_put(int b);                            /* one dump byte */
void s98_reg(int dev_port, int addr, int value);/* register write triple */
void s98_syncs(u32 n);                          /* n syncs (FF/FE+varint) */

void s98_write_tag(const u8 *tag, u32 len);
int s98_patch(u32 tag_ofs, u32 loop_ofs);       /* fix up header fields */
int s98_close(void);                            /* flush; -1 on I/O error */
void s98_abort(void);                           /* close without checks */

#endif
