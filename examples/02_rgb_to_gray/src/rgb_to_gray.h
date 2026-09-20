#ifndef RGB_TO_GRAY_H
#define RGB_TO_GRAY_H

#include "hls_compat.h"

/* Compile-time maxima. HLS needs a constant trip count to bound loops and size
 * memories; runtime rows/cols then select how much of that bound is used.
 * Set these to the LARGEST resolution the IP must support -- they drive BRAM
 * and the reported latency, so do not leave them at 4K if you only do 1080p. */
#ifndef MAX_COLS
#define MAX_COLS 1920
#endif
#ifndef MAX_ROWS
#define MAX_ROWS 1080
#endif

void rgb_to_gray(vid_stream_t &src, gray_stream_t &dst,
                 ap_uint<16> rows, ap_uint<16> cols);

#endif
