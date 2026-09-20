#ifndef FEATURE_SCAN_H
#define FEATURE_SCAN_H

#include "hls_compat.h"

#ifndef MAX_COLS
#define MAX_COLS 64
#endif
#ifndef MAX_ROWS
#define MAX_ROWS 64
#endif

/* ---------------------------------------------------------------------------
 * VARIANT selects which implementation is compiled. Sweep it:
 *
 *   ./scripts/sweep.py --example 06_rate_change_deadlock \
 *       --define VARIANT=0,1,2 --stage csim
 *
 *   0  BROKEN   -- data-dependent token count, fixed-count consumer
 *   1  TAGGED   -- always emit a token, carry a valid bit   (recommended)
 *   2  SENTINEL -- variable count terminated by an end-of-stream token
 * ------------------------------------------------------------------------ */
#ifndef VARIANT
#define VARIANT 1
#endif

/* A detected feature: packed {y[31:16], x[15:0]}. */
typedef ap_axiu<32, 1, 1, 1>     coord_axis_t;
typedef hls::stream<coord_axis_t> coord_stream_t;

/* Internal channel payload. `valid` is only used by VARIANT 1, `eos` only by
 * VARIANT 2. Both cost one bit of FIFO width -- which is the entire price of
 * not deadlocking. */
struct tok_t {
    ap_uint<32> coord;
    ap_uint<1>  valid;
    ap_uint<1>  eos;
};
typedef hls::stream<tok_t> tok_stream_t;

void feature_scan(gray_stream_t &src, coord_stream_t &dst,
                  ap_uint<16> rows, ap_uint<16> cols,
                  ap_uint<8> threshold, ap_uint<32> *n_found);

#endif
