#ifndef SOBEL_H
#define SOBEL_H

#include "hls_compat.h"

/* MAX_COLS sets the line-buffer depth and therefore the BRAM cost:
 *   2 lines x MAX_COLS x 8 bits.
 * At MAX_COLS=1920 that is 2 x 1920 x 8 = 30 Kb ~= 2 BRAM18.
 * At MAX_COLS=4096 it is ~4 BRAM18. This is why you do NOT leave MAX_COLS at
 * 4096 "just in case" on a design that only ever runs 1080p -- for a 3x3 it
 * is cheap, but for a 15x15 window you are buying 14 line buffers. */
#ifndef MAX_COLS
#define MAX_COLS 1920
#endif
#ifndef MAX_ROWS
#define MAX_ROWS 1080
#endif

void sobel_3x3(gray_stream_t &src, gray_stream_t &dst,
               ap_uint<16> rows, ap_uint<16> cols, ap_uint<8> threshold);

#endif
