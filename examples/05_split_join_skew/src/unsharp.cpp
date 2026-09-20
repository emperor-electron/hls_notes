#include "unsharp.h"

/* ===========================================================================
 * SPLIT / JOIN LATENCY SKEW -- the most common HLS video deadlock.
 *
 *                    ┌──────────────────────────────┐
 *                    │        sharp (lag 0)         │   depth = SKEW_DEPTH
 *   src ──[fanout]───┤                              ├──[combine]── dst
 *                    │  sblur ──[blur3x3]── sblurred│   lag = cols+1
 *                    └──────────────────────────────┘
 *
 * The two branches rejoin in `stage_combine`, which reads one token from each
 * per iteration. But `stage_blur` cannot produce its first output until it has
 * consumed `cols+1` inputs -- a 3x3 window centred on row y needs row y+1.
 *
 * So during startup:
 *   - combine blocks waiting on `sblurred`
 *   - therefore combine never drains `sharp`
 *   - therefore `sharp` fills to its depth
 *   - therefore fanout blocks writing `sharp`
 *   - therefore fanout never writes `sblur`
 *   - therefore blur starves and never produces its first output
 *   => circular wait. Deadlock.
 *
 * The fix is not clever: make `sharp` deep enough to hold the entire skew.
 *
 *     depth(sharp)  >=  latency_skew + 1
 *                   =   (cols + 1) + 1
 *
 * Round up and add margin for the pipeline depth of the blur stage itself
 * (a handful of beats). SKEW_DEPTH defaults to MAX_COLS + 8.
 *
 * -- THE PART THAT BITES --
 * FIFO depth is a COMPILE-TIME constant. The skew is `cols + 1`, a RUNTIME
 * value. So you must size for MAX_COLS, not for the resolution you are
 * currently testing at. A design that works perfectly at 640x480 and
 * deadlocks the moment someone selects 1080p is this bug, every time.
 *
 * -- WHY CSIM WILL NOT SAVE YOU --
 * In csim, hls::stream is an unbounded std::deque and the stages run
 * sequentially. `sharp` grows to rows*cols and everything passes. The depth
 * pragma is ignored entirely. Only cosim models the real FIFO.
 * ======================================================================== */

/* -------------------------------------------------------------------------
 * Fan-out. This stage exists ONLY to satisfy dataflow canonical-form rule 1
 * (single producer, single consumer per channel). You cannot simply read
 * `src` from two stages; you duplicate it here.
 *
 * Note the two writes are in the same iteration. HLS schedules them in the
 * same control step when both FIFOs have space, so this costs no throughput
 * -- but it does mean this stage blocks if EITHER output is full, which is
 * exactly the coupling that creates the deadlock above.
 * ---------------------------------------------------------------------- */
static void stage_fanout(gray_stream_t &src,
                         pix_stream_t &sharp, pix_stream_t &sblur,
                         ap_uint<16> rows, ap_uint<16> cols)
{
#pragma HLS INLINE off
FANOUT:
    for (ap_uint<32> i = 0, n = (ap_uint<32>)rows * cols; i < n; ++i) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=(MAX_ROWS*MAX_COLS)
#pragma HLS PIPELINE II=1
        pix_t p = src.read().data;
        sharp.write(p);
        sblur.write(p);
    }
}

/* -------------------------------------------------------------------------
 * The long-latency branch: 3x3 box blur, lag = cols + 1.
 * ---------------------------------------------------------------------- */
static void stage_blur(pix_stream_t &in, pix_stream_t &out,
                       ap_uint<16> rows, ap_uint<16> cols)
{
#pragma HLS INLINE off
    static pix_t linebuf[2][MAX_COLS];
#pragma HLS ARRAY_PARTITION variable=linebuf complete dim=1
    static pix_t win[3][3];
#pragma HLS ARRAY_PARTITION variable=win complete dim=0

    ap_uint<16> ix = 0, ox = 0, oy = 0;
    const ap_uint<32> n_in = (ap_uint<32>)rows * cols;
    const ap_uint<32> lag  = (ap_uint<32>)cols + 1;

BLUR:
    for (ap_uint<32> i = 0; i < n_in + lag; ++i) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=(MAX_ROWS*MAX_COLS + MAX_COLS + 1)
#pragma HLS PIPELINE II=1
        pix_t p = 0;
        if (i < n_in) p = in.read();

        pix_t a = linebuf[0][ix], b = linebuf[1][ix];
        linebuf[0][ix] = b;
        linebuf[1][ix] = p;
        for (int r = 0; r < 3; ++r) {
#pragma HLS UNROLL
            win[r][0] = win[r][1];
            win[r][1] = win[r][2];
        }
        win[0][2] = a; win[1][2] = b; win[2][2] = p;

        if (i >= lag) {
            ap_uint<12> sum = 0;
            for (int r = 0; r < 3; ++r) {
#pragma HLS UNROLL
                for (int c = 0; c < 3; ++c) {
#pragma HLS UNROLL
                    int rr = r, cc = c;
                    if (oy == 0        && r == 0) rr = 1;
                    if (oy == rows - 1 && r == 2) rr = 1;
                    if (ox == 0        && c == 0) cc = 1;
                    if (ox == cols - 1 && c == 2) cc = 1;
                    sum += win[rr][cc];
                }
            }
            out.write((pix_t)(((ap_uint<28>)sum * 7282 + 32768) >> 16));
            if (ox == cols - 1) { ox = 0; oy = (oy == rows - 1) ? (ap_uint<16>)0 : (ap_uint<16>)(oy + 1); }
            else                { ++ox; }
        }
        if (ix == cols - 1) ix = 0; else ++ix;
    }
}

/* -------------------------------------------------------------------------
 * The join. Reads one token from each branch per iteration.
 *
 * Unsharp mask:  out = clamp( orig + amount * (orig - blur) / 256 )
 * ---------------------------------------------------------------------- */
static void stage_combine(pix_stream_t &sharp, pix_stream_t &blurred,
                          gray_stream_t &dst,
                          ap_uint<16> rows, ap_uint<16> cols,
                          ap_uint<8> amount)
{
#pragma HLS INLINE off
    ap_uint<16> ox = 0, oy = 0;
COMBINE:
    for (ap_uint<32> i = 0, n = (ap_uint<32>)rows * cols; i < n; ++i) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=(MAX_ROWS*MAX_COLS)
#pragma HLS PIPELINE II=1

        /* READ ORDER MATTERS -- but not the way people assume.
         *
         * Swapping these two lines does NOT fix the deadlock. Both reads must
         * complete before the iteration retires, so whichever is "first" in C,
         * the iteration blocks until both tokens are available. The only fix
         * is FIFO depth.
         *
         * Read order DOES matter when the two producers are themselves
         * coupled -- see example 06. */
        pix_t s = sharp.read();
        pix_t b = blurred.read();

        ap_int<18> hi   = (ap_int<18>)s - (ap_int<18>)b;
        ap_int<18> adj  = (ap_int<18>)s + ((hi * (ap_int<18>)amount) >> 8);
        if (adj < 0)   adj = 0;
        if (adj > 255) adj = 255;

        gray_axis_t w;
        w.data = (pix_t)adj;
        AXIS_SET_KEEP(w);
        w.user = (oy == 0 && ox == 0) ? 1 : 0;
        w.last = (ox == cols - 1)     ? 1 : 0;
        w.id = 0; w.dest = 0;
        dst.write(w);

        if (ox == cols - 1) { ox = 0; ++oy; } else { ++ox; }
    }
}

/* ======================================================================== */

void unsharp_mask(gray_stream_t &src, gray_stream_t &dst,
                  ap_uint<16> rows, ap_uint<16> cols, ap_uint<8> amount)
{
#pragma HLS INTERFACE axis port=src
#pragma HLS INTERFACE axis port=dst
#pragma HLS INTERFACE s_axilite port=rows   bundle=ctrl
#pragma HLS INTERFACE s_axilite port=cols   bundle=ctrl
#pragma HLS INTERFACE s_axilite port=amount bundle=ctrl
#pragma HLS INTERFACE ap_ctrl_none port=return

#pragma HLS DATAFLOW

    pix_stream_t sharp("sharp"), sblur("sblur"), sblurred("sblurred");

    /* The whole lesson in one pragma.
     *
     * depth must cover (cols + 1) at the MAXIMUM supported resolution, not the
     * one you happen to be simulating. Sized at MAX_COLS + 8.
     *
     * Cost check before you panic about BRAM: 8 bits x 520 deep = 4160 bits,
     * one BRAM18. A 1920-wide design costs 8 x 1928 = 15 Kb, still one BRAM18
     * (which holds 18 Kb). Skew FIFOs on 8-bit video are cheap. They stop
     * being cheap when the branch carries a 24-bit RGB pixel at 4K
     * (24 x 4104 = 98 Kb ~= 6 BRAM18) -- at which point the right move is to
     * push the fan-out point later so less data is in flight, not to shave the
     * depth and hope.
     */
#pragma HLS STREAM variable=sharp    depth=SKEW_DEPTH
#pragma HLS STREAM variable=sblur    depth=8
#pragma HLS STREAM variable=sblurred depth=8

    stage_fanout (src, sharp, sblur, rows, cols);
    stage_blur   (sblur, sblurred, rows, cols);
    stage_combine(sharp, sblurred, dst, rows, cols, amount);
}
