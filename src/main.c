/* vgm2s98 - Convert VGM files to S98 V3 format.
 *
 * C89 port of vgm2s98.py; with the default host build (ZLIB+GD3) the
 * .s98 output is byte-identical to the Python version.  See vgm2s98.py
 * for the format and conversion notes, and convert.c for the two-pass
 * streaming design.
 *
 * Modules:
 *   vgm.c      VGM input (optionally gzipped) and header parsing
 *   convert.c  two-pass command walk, DELTA-T upload, timing, stats
 *   s98.c      S98 output: header/device table, dump bytes, syncs
 *   gd3.c      GD3 tag -> [S98] tag (UTF-16LE -> UTF-8); HAVE_GD3 only
 *   main.c     command line and orchestration
 *
 * HAVE_GD3 is off for DOS builds (make GD3=0): the UTF-16/UTF-8 tag
 * path is not worth its code size and heap use on a small DOS binary.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "convert.h"
#ifdef HAVE_GD3
#include "gd3.h"
#endif
#include "s98.h"
#include "vgm.h"

static u32 g_sync_num = 1, g_sync_den = 1000;
static int g_quiet;

static void usage(FILE *f)
{
    fputs("usage: vgm2s98 [-h] [-q] [--no-adpcm] [--sync N/D] "
          "input [output]\n", f);
}

static void help(void)
{
    usage(stdout);
    fputs("\nConvert VGM (OPNA/OPN/AY portion) to S98 V3.\n\n"
          "  input        input .vgm file"
#ifdef HAVE_ZLIB
          " (or gzipped .vgz)"
#endif
          "\n"
          "  output       output .s98 file (default: input name with .s98"
          " extension)\n"
          "  --sync N/D   S98 sync unit in seconds (default: 1/1000)\n"
          "  --no-adpcm   drop YM2608 DELTA-T data blocks instead of\n"
          "               converting them to an ADPCM RAM upload\n"
          "  -q, --quiet  suppress the conversion summary\n", stdout);
}

static int parse_sync(const char *s)
{
    const char *slash = strchr(s, '/');
    const char *p;
    if (!slash || slash == s || !slash[1] || strchr(slash + 1, '/'))
        return -1;
    for (p = s; *p; p++)
        if (*p != '/' && (*p < '0' || *p > '9'))
            return -1;
    g_sync_num = strtoul(s, NULL, 10);
    g_sync_den = strtoul(slash + 1, NULL, 10);
    if (!g_sync_num || !g_sync_den)
        return -1;
    return 0;
}

static char *default_output(const char *in)
{
    static const char *exts[4] = { ".vgm", ".vgz", ".VGM", ".VGZ" };
    size_t n = strlen(in), base = n;
    char *p;
    int i;
    for (i = 0; i < 4; i++) {
        if (n >= 4 && !strcmp(in + n - 4, exts[i])) {
            base = n - 4;
            break;
        }
    }
    p = (char *)malloc(base + 5);
    if (p) {
        memcpy(p, in, base);
        strcpy(p + base, ".s98");
    }
    return p;
}

int main(int argc, char **argv)
{
    const char *inpath = NULL, *outpath = NULL;
    char *outalloc = NULL;
    u32 dump_ofs, tag_ofs, loop_ofs;
    int i, endopts = 0, out_created = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!endopts && a[0] == '-' && a[1]) {
            if (!strcmp(a, "--")) {
                endopts = 1;
            } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
                help();
                return 0;
            } else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) {
                g_quiet = 1;
            } else if (!strcmp(a, "--no-adpcm")) {
                cv_adpcm = 0;
            } else if (!strcmp(a, "--sync") || !strncmp(a, "--sync=", 7)) {
                const char *v = a[6] ? a + 7 : (++i < argc ? argv[i] : NULL);
                if (!v || parse_sync(v)) {
                    usage(stderr);
                    fprintf(stderr, "error: --sync must look like 1/1000\n");
                    return 2;
                }
            } else {
                usage(stderr);
                fprintf(stderr, "error: unrecognized option %s\n", a);
                return 2;
            }
        } else if (!inpath) {
            inpath = a;
        } else if (!outpath) {
            outpath = a;
        } else {
            usage(stderr);
            fprintf(stderr, "error: too many arguments\n");
            return 2;
        }
    }
    if (!inpath) {
        usage(stderr);
        fprintf(stderr, "error: input file required\n");
        return 2;
    }
    if (cv_set_sync(g_sync_num, g_sync_den)) {
        fprintf(stderr, "error: unsupported --sync ratio\n");
        return 2;
    }

    if (!outpath) {
        outalloc = default_output(inpath);
        if (!outalloc) {
            fprintf(stderr, "error: out of memory\n");
            return 1;
        }
        outpath = outalloc;
    }

    if (vgm_open(inpath) || vgm_parse_header() || cv_scan()
#ifdef HAVE_GD3
        || gd3_build()
#endif
        )
        goto fail;

    if (s98_open(outpath))
        goto fail;
    out_created = 1;
    dump_ofs = S98_DUMP_OFS(cv_ndev);
    s98_write_header(g_sync_num, g_sync_den, cv_ndev, cv_dev_type,
                     cv_dev_clock);
    if (cv_emit())
        goto fail;
#ifdef HAVE_GD3
    if (gd3_tag_len)
        s98_write_tag(gd3_tag, gd3_tag_len);
    tag_ofs = gd3_tag_len ? dump_ofs + s98_dump_len : 0;
#else
    tag_ofs = 0;
#endif
    loop_ofs = cv_have_loop ? dump_ofs + cv_loop_pos : 0;
    if (s98_patch(tag_ofs, loop_ofs) || s98_close())
        goto fail;
    vgm_close();

    if (!g_quiet)
        cv_print_stats(outpath, dump_ofs + s98_dump_len
#ifdef HAVE_GD3
                       + gd3_tag_len
#endif
                       );
    free(outalloc);
#ifdef HAVE_GD3
    gd3_free();
#endif
    return 0;

fail:
    fprintf(stderr, "error: %s\n", g_errmsg);
    s98_abort();
    if (out_created)
        remove(outpath);
    vgm_close();
    free(outalloc);
#ifdef HAVE_GD3
    gd3_free();
#endif
    return 1;
}
