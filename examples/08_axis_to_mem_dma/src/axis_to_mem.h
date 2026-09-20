#ifndef AXIS_TO_MEM_H
#define AXIS_TO_MEM_H

#include "hls_compat.h"

#ifndef MAX_COLS
#define MAX_COLS 1920
#endif
#ifndef MAX_ROWS
#define MAX_ROWS 1080
#endif

/* Memory word. 32 bits per pixel (xRGB8888) rather than a packed 24 bits,
 * because:
 *   - AXI bursts want power-of-two element sizes; a 24-bit element makes every
 *     line start unaligned and kills burst inference;
 *   - the Xilinx Video Frame Buffer Write IP uses the same layout for RGB8;
 *   - one wasted byte per pixel is 8 MB/s at 1080p60, which is nothing next to
 *     the ~500 MB/s the frame itself costs.
 * Pack to 24 bits only when bandwidth is genuinely the limit, and then pack a
 * whole burst, not individual pixels. */
typedef ap_uint<32> memword_t;

/* NOTE the plain `int` geometry arguments, where the rest of the repo uses
 * ap_uint<16>. A loop that contains a DATAFLOW region must have a bound that
 * is literally a constant or a function argument -- `y < (int)rows` with an
 * ap_uint<16> `rows` counts as neither, and earns:
 *   WARNING: [HLS 214-110] As the loop bound is not a constant or function
 *            argument, the compiler may not successfully process the
 *            dataflow loop
 * "may not successfully process" means a silent fallback to sequential
 * execution. Use int here. It costs nothing -- the AXI-Lite registers are
 * 32-bit either way. */
void axis_to_mem(vid_stream_t &src, memword_t *mem,
                 int rows, int cols, int stride);

#endif
