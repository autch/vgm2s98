/* The converter makes two passes over the seekable input stream:
 *   pass 1  scans the command stream for the chips used, the DELTA-T
 *           RAM type (first $01 write) and the data blocks that appear
 *           before the first wait (hoisted ahead of the loop point),
 *   pass 2  emits the S98 dump.
 * Nothing is buffered in memory for the dump path (GD3 tags, when
 * built with HAVE_GD3, are the only heap allocation), so the same code
 * can also run under 16-bit DOS memory models.
 *
 * Sync timing avoids 64-bit arithmetic: the running-total quantization
 * floor(samples * den / (44100 * num)) is computed incrementally with a
 * carried remainder, which never overflows 32 bits for sane --sync
 * ratios (the ratio is reduced first and range-checked by
 * cv_set_sync()).
 */

#include <stdio.h>
#include <string.h>

#include "convert.h"
#include "s98.h"
#include "vgm.h"

/* Default chip clocks used when the VGM header field is absent/zero */
#define DEF_CLOCK_YM2608 7987200UL
#define DEF_CLOCK_YM2203 3993600UL
#define DEF_CLOCK_AY8910 1789750UL

#define MAX_HOIST 64

int cv_adpcm = 1;

/* pass 1 results */
static int use2608, use2203, useay;
static int ram_type = 0x02, ram_found;

struct hoist { u32 start, ofs, len; };
static struct hoist hoist[MAX_HOIST];
static int nhoist;

/* device table */
int cv_ndev;
const char *cv_dev_name[CV_MAX_DEVICES];
u32 cv_dev_type[CV_MAX_DEVICES], cv_dev_clock[CV_MAX_DEVICES];
static int idx2608 = -1, idx2203 = -1, idxay = -1;

/* timing state (reduced sync ratio: rden / D) */
static u32 rden, D;
static u32 acc, base_syncs, extra_syncs, emitted_syncs;
static u32 total_samples, adpcm_bytes;

u32 cv_loop_pos;
int cv_have_loop;

/* skipped-command counters */
static u32 skip_cmd[256], skip_blk[256];
static u32 skip_ay2, skip_dac, skip_pcm;
static u32 skip_adp_off, skip_adp_nochip;

static u32 gcd(u32 a, u32 b)
{
    while (b) {
        u32 t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* Reduce the sync ratio for the internal timing arithmetic (the S98
 * header keeps the values as given; the quantization result is
 * invariant under common factors) and range-check it so that the
 * 32-bit remainder-carry never overflows. */
int cv_set_sync(u32 num, u32 den)
{
    u32 g = gcd(num, den);
    num /= g;
    den /= g;
    if (num > 0xFFFFFFFFUL / VGM_SAMPLE_RATE)
        return -1;
    D = VGM_SAMPLE_RATE * num;
    g = gcd(den, D);
    den /= g;
    D /= g;
    if (den > (0xFFFFFFFFUL - (D - 1)) / 65535UL)
        return -1;
    rden = den;
    return 0;
}

static void wait_ev(int pass, u32 w, int *seen_wait)
{
    *seen_wait = 1;
    if (pass != 2)
        return;
    total_samples += w;
    acc += w * rden;
    base_syncs += acc / D;
    acc %= D;
    if (base_syncs + extra_syncs > emitted_syncs) {
        s98_syncs(base_syncs + extra_syncs - emitted_syncs);
        emitted_syncs = base_syncs + extra_syncs;
    }
}

/* Emit a DELTA-T RAM upload; the blob is read from the current input
 * position.  One sync is inserted every 256 data bytes; the inserted
 * syncs extend the timeline (extra syncs) instead of being absorbed by
 * the following waits (see vgm2s98.py for why). */
static int upload_block(u32 start, u32 len, u32 *pos)
{
    int ext = idx2608 * 2 + 1;
    int shift = (ram_type & 0x03) ? 5 : 2;
    u32 unit = start >> shift;
    u32 i;

    s98_reg(ext, 0x00, 0x01);       /* reset */
    s98_reg(ext, 0x00, 0x60);       /* memory write mode (REC|MEMDATA) */
    s98_reg(ext, 0x01, ram_type);
    s98_reg(ext, 0x02, (int)(unit & 0xFF));
    s98_reg(ext, 0x03, (int)((unit >> 8) & 0xFF));
    s98_reg(ext, 0x04, 0xFF);       /* STOP = max */
    s98_reg(ext, 0x05, 0xFF);
    s98_reg(ext, 0x0C, 0xFF);       /* LIMIT = max */
    s98_reg(ext, 0x0D, 0xFF);
    for (i = 0; i < len; i++) {
        int c = vgm_getc();
        if (c < 0) {
            errmsg_set("truncated DELTA-T data block");
            return -1;
        }
        s98_reg(ext, 0x08, c);
        if (((i + 1) & 0xFF) == 0) {
            s98_put(0xFF);
            emitted_syncs++;
            extra_syncs++;
        }
    }
    s98_reg(ext, 0x00, 0x00);       /* leave memory mode */
    adpcm_bytes += len;
    if (pos)
        *pos += len;
    return 0;
}

/* Byte length (including the command byte) of VGM commands we skip;
 * 0 = unknown command. */
static long skip_len(int cmd)
{
    if (cmd >= 0x30 && cmd <= 0x3F) return 2;
    if (cmd >= 0x40 && cmd <= 0x4E) return 3;
    if (cmd == 0x4F || cmd == 0x50) return 2;
    if (cmd >= 0x51 && cmd <= 0x5F) return 3;
    if (cmd == 0x64) return 4;
    if (cmd >= 0x70 && cmd <= 0x8F) return 1;
    if (cmd == 0x90 || cmd == 0x91) return 5;
    if (cmd == 0x92) return 6;
    if (cmd == 0x93) return 11;
    if (cmd == 0x94) return 2;
    if (cmd == 0x95) return 5;
    if (cmd >= 0xA0 && cmd <= 0xBF) return 3;
    if (cmd >= 0xC0 && cmd <= 0xDF) return 4;
    if (cmd >= 0xE0) return 5;
    return 0;
}

/* Walk the VGM command stream.  pass 1 only collects information,
 * pass 2 emits the dump.  Both passes take identical routes so that
 * pass 2 can identify the hoisted data blocks by their ordinal. */
static int walk(int pass)
{
    u32 pos = vgm_hdr.data_ofs;
    u32 loop_in = vgm_hdr.loop_ofs;
    u32 block_no = 0;
    int seen_wait = 0, ended = 0;

    if (vgm_seek(pos))
        return -1;
    while (pos < vgm_size) {
        u32 cmd_pos = pos;
        int cmd, a, v;

        if (loop_in && pos == loop_in) {
            if (pass == 2) {
                cv_loop_pos = s98_dump_len;
                cv_have_loop = 1;
            }
            loop_in = 0;
        }
        if ((cmd = vgm_rd_byte(&pos)) < 0)
            return -1;

        if (cmd == 0x66) {              /* end of sound data */
            ended = 1;
            break;
        } else if (cmd == 0x55) {       /* YM2203 */
            if ((a = vgm_rd_byte(&pos)) < 0 || (v = vgm_rd_byte(&pos)) < 0)
                return -1;
            if (pass == 1)
                use2203 = 1;
            else
                s98_reg(idx2203 * 2, a, v);
        } else if (cmd == 0x56 || cmd == 0x57) {    /* YM2608 port 0/1 */
            if ((a = vgm_rd_byte(&pos)) < 0 || (v = vgm_rd_byte(&pos)) < 0)
                return -1;
            if (pass == 1) {
                use2608 = 1;
                if (cmd == 0x57 && a == 0x01 && !ram_found) {
                    ram_type = v & 0x03;
                    ram_found = 1;
                }
            } else {
                s98_reg(idx2608 * 2 + (cmd - 0x56), a, v);
            }
        } else if (cmd == 0xA0) {       /* AY8910 (bit7 of addr = 2nd chip) */
            if ((a = vgm_rd_byte(&pos)) < 0 || (v = vgm_rd_byte(&pos)) < 0)
                return -1;
            if (a & 0x80) {
                if (pass == 1)
                    skip_ay2++;
            } else if (pass == 1) {
                useay = 1;
            } else {
                s98_reg(idxay * 2, a, v);
            }
        } else if (cmd == 0x61) {       /* wait nnnn samples */
            u32 w;
            if (vgm_rd_u16(&pos, &w))
                return -1;
            wait_ev(pass, w, &seen_wait);
        } else if (cmd == 0x62) {       /* wait 1/60 s */
            wait_ev(pass, 735, &seen_wait);
        } else if (cmd == 0x63) {       /* wait 1/50 s */
            wait_ev(pass, 882, &seen_wait);
        } else if (cmd >= 0x70 && cmd <= 0x7F) {    /* wait n+1 samples */
            wait_ev(pass, (u32)(cmd & 0x0F) + 1, &seen_wait);
        } else if (cmd >= 0x80 && cmd <= 0x8F) {    /* YM2612 DAC + wait n */
            if (pass == 1)
                skip_dac++;
            if (cmd & 0x0F)
                wait_ev(pass, (u32)(cmd & 0x0F), &seen_wait);
        } else if (cmd == 0x67) {       /* data block */
            u32 size, next_pos;
            int marker, btype;
            if (cmd_pos + 7 > vgm_size)
                goto broken;
            if ((marker = vgm_rd_byte(&pos)) < 0)
                return -1;
            if (marker != 0x66)
                goto broken;
            if ((btype = vgm_rd_byte(&pos)) < 0)
                return -1;
            if (vgm_rd_u32(&pos, &size))
                return -1;
            size &= 0x7FFFFFFFUL;
            next_pos = cmd_pos + 7 + size;
            if (next_pos < cmd_pos || next_pos > vgm_size)
                next_pos = vgm_size;
            if (btype == 0x81 && size > 8) {
                /* YM2608 DELTA-T memory image:
                 * payload = dword memory size, dword start address, data */
                u32 start, blob_len;
                if (cmd_pos + 15 > vgm_size) {
                    char buf[40];
                    sprintf(buf, "truncated VGM data at 0x%lx",
                            (unsigned long)pos);
                    errmsg_set(buf);
                    return -1;
                }
                if (vgm_skip(&pos, 4))
                    return -1;
                if (vgm_rd_u32(&pos, &start))
                    return -1;
                blob_len = size - 8;
                if (blob_len > vgm_size - pos)
                    blob_len = vgm_size - pos;
                if (pass == 1) {
                    if (!seen_wait) {
                        if (nhoist >= MAX_HOIST) {
                            errmsg_set(
                                "too many data blocks before first wait");
                            return -1;
                        }
                        hoist[nhoist].start = start;
                        hoist[nhoist].ofs = pos;
                        hoist[nhoist].len = blob_len;
                        nhoist++;
                    }
                } else if (block_no >= (u32)nhoist) {
                    /* mid-stream block (rare); hoisted ones were already
                     * handled ahead of the walk */
                    if (!cv_adpcm)
                        skip_adp_off++;
                    else if (idx2608 < 0)
                        skip_adp_nochip++;
                    else if (upload_block(start, blob_len, &pos))
                        return -1;
                }
                block_no++;
            } else {
                if (pass == 1)
                    skip_blk[btype]++;
            }
            if (next_pos > pos && vgm_skip(&pos, next_pos - pos))
                return -1;
        } else if (cmd == 0x68) {       /* PCM RAM write */
            if (pass == 1)
                skip_pcm++;
            if (vgm_skip(&pos, 11))
                return -1;
        } else {
            long n = skip_len(cmd);
            if (n == 0) {
                char buf[48];
                sprintf(buf, "unknown VGM command 0x%02x at 0x%lx",
                        cmd, (unsigned long)cmd_pos);
                errmsg_set(buf);
                return -1;
            }
            if (pass == 1)
                skip_cmd[cmd]++;
            if (n > 1 && vgm_skip(&pos, (u32)(n - 1)))
                return -1;
        }
        continue;
broken:
        {
            char buf[40];
            sprintf(buf, "broken data block at 0x%lx",
                    (unsigned long)cmd_pos);
            errmsg_set(buf);
        }
        return -1;
    }
    if (!ended && pass == 1)
        fprintf(stderr, "warning: no end-of-data (0x66) command\n");
    return 0;
}

static void build_devices(void)
{
    if (use2608) {
        idx2608 = cv_ndev;
        cv_dev_name[cv_ndev] = "ym2608";
        cv_dev_type[cv_ndev] = S98_OPNA;
        cv_dev_clock[cv_ndev] =
            vgm_hdr.clk2608 ? vgm_hdr.clk2608 : DEF_CLOCK_YM2608;
        cv_ndev++;
    }
    if (use2203) {
        idx2203 = cv_ndev;
        cv_dev_name[cv_ndev] = "ym2203";
        cv_dev_type[cv_ndev] = S98_OPN;
        cv_dev_clock[cv_ndev] =
            vgm_hdr.clk2203 ? vgm_hdr.clk2203 : DEF_CLOCK_YM2203;
        cv_ndev++;
    }
    if (useay) {
        idxay = cv_ndev;
        cv_dev_name[cv_ndev] = "ay8910";
        cv_dev_type[cv_ndev] =
            (vgm_hdr.ay_type == 0x10) ? S98_PSG_YM2149 : S98_PSG_AY8910;
        cv_dev_clock[cv_ndev] =
            vgm_hdr.clkay ? vgm_hdr.clkay : DEF_CLOCK_AY8910;
        cv_ndev++;
    }
}

int cv_scan(void)
{
    if (walk(1))
        return -1;
    if (!use2608 && !use2203 && !useay) {
        errmsg_set("no supported chip commands (YM2608/YM2203/AY8910)");
        return -1;
    }
    build_devices();
    return 0;
}

int cv_emit(void)
{
    int i;

    /* Hoisted pre-first-wait DELTA-T blocks go first, ahead of the
     * loop point (see vgm2s98.py for why). */
    for (i = 0; i < nhoist; i++) {
        if (!cv_adpcm) {
            skip_adp_off++;
        } else if (idx2608 < 0) {
            skip_adp_nochip++;
        } else {
            if (vgm_seek(hoist[i].ofs))
                return -1;
            if (upload_block(hoist[i].start, hoist[i].len, NULL))
                return -1;
        }
    }
    if (walk(2))
        return -1;
    s98_put(0xFD);                  /* end of dump */
    return 0;
}

void cv_print_stats(const char *outpath, u32 size)
{
    /* Duration as sec.tenths without floating point (LSI C intlib). */
    u32 sec = total_samples / VGM_SAMPLE_RATE;
    u32 rem = total_samples % VGM_SAMPLE_RATE;
    u32 tenths = (rem * 10UL + VGM_SAMPLE_RATE / 2UL) / VGM_SAMPLE_RATE;
    int i;

    if (tenths >= 10UL) {
        sec++;
        tenths = 0;
    }
    printf("%s: %lu bytes, %lu.%lus, %lu syncs, devices: ",
           outpath, (unsigned long)size,
           (unsigned long)sec, (unsigned long)tenths,
           (unsigned long)emitted_syncs);
    for (i = 0; i < cv_ndev; i++)
        printf("%s%s(%luHz)", i ? ", " : "", cv_dev_name[i],
               (unsigned long)cv_dev_clock[i]);
    printf(", loop: %s\n", cv_have_loop ? "yes" : "no");
    if (adpcm_bytes)
        printf("  ADPCM RAM upload: %lu bytes "
               "(requires DELTA-T RAM on real hardware)\n",
               (unsigned long)adpcm_bytes);
    /* same (ASCII-sorted) order as the Python version */
    if (skip_adp_off)
        printf("  skipped: ADPCM block (disabled) x%lu\n",
               (unsigned long)skip_adp_off);
    if (skip_adp_nochip)
        printf("  skipped: ADPCM block (no YM2608) x%lu\n",
               (unsigned long)skip_adp_nochip);
    if (skip_ay2)
        printf("  skipped: AY8910 #2 x%lu\n", (unsigned long)skip_ay2);
    if (skip_pcm)
        printf("  skipped: PCM RAM write x%lu\n", (unsigned long)skip_pcm);
    if (skip_dac)
        printf("  skipped: YM2612 DAC x%lu\n", (unsigned long)skip_dac);
    for (i = 0; i < 256; i++)
        if (skip_cmd[i])
            printf("  skipped: cmd 0x%02x x%lu\n", i,
                   (unsigned long)skip_cmd[i]);
    for (i = 0; i < 256; i++)
        if (skip_blk[i])
            printf("  skipped: data block type 0x%02x x%lu\n", i,
                   (unsigned long)skip_blk[i]);
}
