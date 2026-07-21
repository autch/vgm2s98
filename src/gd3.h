/* GD3 tag -> [S98] tag (UTF-8 with BOM) conversion.
 * Compiled only when HAVE_GD3 is defined (make GD3=1, the host default).
 * Omit for PC-98 DOS builds: conversion cost is not worth it there. */
#ifndef GD3_H
#define GD3_H

#include "vgm2s98.h"

extern u8 *gd3_tag;                 /* NULL / empty if the VGM has no GD3 */
extern u32 gd3_tag_len;

int gd3_build(void);                /* -1 only on out-of-memory/read error */
void gd3_free(void);

#endif
