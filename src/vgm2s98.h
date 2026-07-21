/* vgm2s98 - Convert VGM files to S98 V3 format.
 *
 * Common types shared by every module.  Plain C89 throughout; no
 * long long, so the code also builds with 16-bit DOS compilers.
 */
#ifndef VGM2S98_H
#define VGM2S98_H

typedef unsigned char u8;
typedef unsigned long u32;

#define VGM_SAMPLE_RATE 44100UL

/* one-line error description for the "error: ..." report (main.c) */
#define ERRMSG_SIZE 192
extern char g_errmsg[ERRMSG_SIZE];

/* Bounded writers (never overflow g_errmsg). */
void errmsg_set(const char *msg);
void errmsg_pair(const char *left, const char *right);  /* "left: right" */

#endif
