#ifndef CTRL_DEMO_H
#define CTRL_DEMO_H

#include "hls_compat.h"

#ifndef MAX_COLS
#define MAX_COLS 1920
#endif
#ifndef MAX_ROWS
#define MAX_ROWS 1080
#endif

/* Status block read back by the PS over AXI4-Lite.
 *
 * A struct on an s_axilite port is flattened into consecutive 32-bit registers
 * in declaration order. That ordering is an ABI: insert a field in the middle
 * and every offset after it moves, silently breaking a driver you did not
 * rebuild. Append new fields at the END, always. */
struct ctrl_status_t {
    ap_uint<32> frames_out;    /* frames completed since reset          */
    ap_uint<32> cfg_applied;   /* how many times config was latched     */
    ap_uint<32> cfg_seq_seen;  /* the generation counter we last latched */
};

void ctrl_demo(vid_stream_t &src, vid_stream_t &dst,
               ap_uint<16> rows, ap_uint<16> cols,
               ap_uint<16> r_gain, ap_uint<16> g_gain, ap_uint<16> b_gain,
               ap_uint<32> cfg_seq,
               ap_uint<1>  bypass,
               ctrl_status_t *status);

#endif
