#ifndef PIPELINE_H
#define PIPELINE_H

#include "hls_compat.h"

#ifndef MAX_COLS
#define MAX_COLS 1920
#endif
#ifndef MAX_ROWS
#define MAX_ROWS 1080
#endif

/* Internal stage-to-stage element type.
 *
 * NOTE this is a bare pixel, not an ap_axiu. Inside a DATAFLOW region you are
 * not on an AXI bus -- you are on an HLS FIFO with only data/valid/ready, so
 * carrying TKEEP/TSTRB/TID/TDEST between stages wastes FIFO width for nothing.
 * Strip the side channels at the input edge, regenerate them at the output
 * edge. On a 1080p pipeline with a depth-2048 channel that is the difference
 * between 8 bits x 2048 (1 BRAM) and 56 bits x 2048 (4 BRAM). */
typedef ap_uint<8> pix_t;
typedef hls::stream<pix_t> pix_stream_t;

void video_pipeline(vid_stream_t &src, gray_stream_t &dst,
                    ap_uint<16> rows, ap_uint<16> cols,
                    ap_uint<8> threshold);

#endif
