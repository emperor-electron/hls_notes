#ifndef ROI_STATS_H
#define ROI_STATS_H

#include "hls_compat.h"

#ifndef MAX_COLS
#define MAX_COLS 1920
#endif
#ifndef MAX_ROWS
#define MAX_ROWS 1080
#endif

struct roi_result_t {
    ap_uint<32> sum;      /* sum of pixels inside the ROI */
    ap_uint<32> count;    /* number of pixels inside the ROI */
    ap_uint<8>  min_val;
    ap_uint<8>  max_val;
};

/* Per-frame ROI statistics.
 *
 * Geometry arrives as ap_stable inputs: the contract is that they do NOT
 * change between ap_start and ap_done. Holding to that contract is the job of
 * rtl/hls_cfg_latch.sv -- see the example README. */
void roi_stats(gray_stream_t &src,
               ap_uint<16> rows, ap_uint<16> cols,
               ap_uint<16> roi_x, ap_uint<16> roi_y,
               ap_uint<16> roi_w, ap_uint<16> roi_h,
               roi_result_t *out);

#endif
