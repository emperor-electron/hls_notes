#ifndef UNSHARP_H
#define UNSHARP_H

#include "hls_compat.h"

#ifndef MAX_COLS
#define MAX_COLS 512
#endif
#ifndef MAX_ROWS
#define MAX_ROWS 512
#endif

/* ---------------------------------------------------------------------------
 * THE KNOB THIS EXAMPLE EXISTS FOR.
 *
 * SKEW_DEPTH is the depth of the FIFO on the *undelayed* branch of a
 * split/join. Set it too small and the design deadlocks -- not slowly, not
 * intermittently, but every single time, in hardware, while passing csim.
 *
 * The correct value is derived in src/unsharp.cpp. Default is sized for
 * MAX_COLS=512. Sweep it to find the cliff:
 *
 *   ./scripts/sweep.py --example 05_split_join_skew \
 *       --define SKEW_DEPTH=2,64,256,512,520,600 --stage cosim --timeout 900
 *
 * Everything at or below ~cols+1 will report HANG. That is the point.
 * ------------------------------------------------------------------------ */
#ifndef SKEW_DEPTH
#define SKEW_DEPTH (MAX_COLS + 8)
#endif

typedef ap_uint<8> pix_t;
typedef hls::stream<pix_t> pix_stream_t;

void unsharp_mask(gray_stream_t &src, gray_stream_t &dst,
                  ap_uint<16> rows, ap_uint<16> cols, ap_uint<8> amount);

#endif
