/* Bounded writes into g_errmsg (see vgm2s98.h). */

#include <string.h>

#include "vgm2s98.h"

char g_errmsg[ERRMSG_SIZE];

void errmsg_set(const char *msg)
{
    size_t n, cap = (size_t)ERRMSG_SIZE - 1;
    if (!msg)
        msg = "";
    n = strlen(msg);
    if (n > cap)
        n = cap;
    memcpy(g_errmsg, msg, n);
    g_errmsg[n] = '\0';
}

/* Form "left: right".  Prefer a complete right-hand side; truncate left. */
void errmsg_pair(const char *left, const char *right)
{
    size_t llen, rlen, cap = (size_t)ERRMSG_SIZE - 1;

    if (!left)
        left = "";
    if (!right)
        right = "";
    llen = strlen(left);
    rlen = strlen(right);

    if (rlen >= cap) {
        errmsg_set(right);
        return;
    }
    if (rlen + 2 >= cap) {
        /* no room for ": " and any of left */
        errmsg_set(right);
        return;
    }
    if (llen + 2 + rlen > cap)
        llen = cap - 2 - rlen;

    memcpy(g_errmsg, left, llen);
    g_errmsg[llen] = ':';
    g_errmsg[llen + 1] = ' ';
    memcpy(g_errmsg + llen + 2, right, rlen);
    g_errmsg[llen + 2 + rlen] = '\0';
}
