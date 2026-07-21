/* Conversion core: two-pass command walk, device table, statistics. */
#ifndef CONVERT_H
#define CONVERT_H

#include "vgm2s98.h"

#define CV_MAX_DEVICES 3

extern int cv_ndev;
extern const char *cv_dev_name[CV_MAX_DEVICES];
extern u32 cv_dev_type[CV_MAX_DEVICES], cv_dev_clock[CV_MAX_DEVICES];

extern u32 cv_loop_pos;             /* dump offset of the loop point */
extern int cv_have_loop;

extern int cv_adpcm;                /* 0 = drop DELTA-T data blocks */

int cv_set_sync(u32 num, u32 den);  /* -1 if the ratio cannot be handled */
int cv_scan(void);                  /* pass 1: chips, RAM type, hoisting */
int cv_emit(void);                  /* uploads + pass 2 + end-of-dump */
void cv_print_stats(const char *outpath, u32 size);

#endif
