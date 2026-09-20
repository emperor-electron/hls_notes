#ifndef FRAME_RESYNC_H
#define FRAME_RESYNC_H

#include "hls_compat.h"

#ifndef MAX_COLS
#define MAX_COLS 1920
#endif
#ifndef MAX_ROWS
#define MAX_ROWS 1080
#endif

/* Status counters, readable over AXI4-Lite. Saturating, never wrapping --
 * a counter that wraps to 0 looks identical to "no errors", which is the
 * worst possible failure mode for a diagnostic. */
struct resync_status_t {
    ap_uint<32> frames_out;    /* complete frames emitted                   */
    ap_uint<32> resyncs;       /* unexpected SOF -> counters forced to 0    */
    ap_uint<32> sof_dropped;   /* beats discarded while hunting for SOF     */
    ap_uint<32> eol_mismatch;  /* input TLAST did not land where we expect  */
};

void frame_resync(vid_stream_t &src, vid_stream_t &dst,
                  ap_uint<16> rows, ap_uint<16> cols,
                  resync_status_t *status);

#endif
