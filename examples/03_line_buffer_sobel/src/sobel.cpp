#include "sobel.h"

/* ===========================================================================
 * 3x3 Sobel edge magnitude over an AXI4-Stream, II=1, hand-rolled line buffer.
 *
 * This is THE structural pattern for every neighbourhood filter in a video
 * pipeline. Convolution, median, morphology, demosaic, bilateral -- they all
 * have this skeleton and differ only in the arithmetic at the bottom.
 *
 * Why hand-rolled instead of hls::LineBuffer / xf::cv?
 *   - hls_video.h's hls::LineBuffer<> and hls::Window<> (and their Vitis
 *     Vision xf::cv equivalents) do exactly this and are fine to use. They are
 *     also opaque when something goes wrong, and their shift semantics
 *     (shift_pixels_up vs shift_pixels_down, insert_bottom_row vs
 *     insert_top_row) are a classic source of vertically-flipped kernels.
 *   - You should be able to write this from scratch. Once you can, use the
 *     library.
 * ======================================================================== */

/* -------------------------------------------------------------------------
 * The timing skeleton.
 *
 * A 3x3 window centred on (y, x) needs pixels from rows y-1, y, y+1. Streaming
 * gives you pixels in raster order, so when you have just received (y+1, x+1)
 * you can finally emit the output for (y, x). That is a fixed lag of
 * (one line + one pixel) = cols + 1 beats.
 *
 * So the loop runs for rows*cols + cols + 1 iterations:
 *   - iterations [0, rows*cols)          read an input pixel
 *   - iterations [cols+1, rows*cols+cols+1) write an output pixel
 * and the two ranges overlap for almost the whole frame, which is what keeps
 * II=1 meaningful.
 *
 * The trailing `cols+1` iterations read nothing. They must still advance the
 * column counter so the line buffer keeps shifting correctly -- forgetting
 * that is why hand-rolled line buffers usually corrupt the bottom-right
 * corner and nothing else.
 * ---------------------------------------------------------------------- */

static inline ap_uint<8> sobel_mag(const u8_t w[3][3], ap_uint<8> threshold)
{
#pragma HLS INLINE
    /*        -1  0  +1            -1 -2 -1
     *  Gx =  -2  0  +2      Gy =   0  0  0
     *        -1  0  +1            +1 +2 +1
     *
     * Range: each is at most 4*255 = 1020, and signed, so 12 bits with sign.
     * ap_int<12> holds -2048..2047. Sized deliberately: leaving these as `int`
     * makes HLS build 32-bit adder trees that it then has to prune, and the
     * pruning is not always complete.
     */
    ap_int<12> gx = (ap_int<12>)w[0][2] + 2 * (ap_int<12>)w[1][2] + (ap_int<12>)w[2][2]
                  - (ap_int<12>)w[0][0] - 2 * (ap_int<12>)w[1][0] - (ap_int<12>)w[2][0];

    ap_int<12> gy = (ap_int<12>)w[2][0] + 2 * (ap_int<12>)w[2][1] + (ap_int<12>)w[2][2]
                  - (ap_int<12>)w[0][0] - 2 * (ap_int<12>)w[0][1] - (ap_int<12>)w[0][2];

    /* |gx| + |gy| instead of sqrt(gx^2 + gy^2).
     *
     * The L1 approximation is within ~12% of the true magnitude, costs two
     * adders and a couple of muxes, and is what essentially every hardware
     * Sobel ships. The L2 form needs two multipliers and a sqrt core -- ~8
     * cycles of latency and a DSP budget you would rather spend elsewhere.
     * If you need the real magnitude, use the alpha-max-beta-min
     * approximation (0.96*max + 0.40*min, ~4% error, still adds and shifts)
     * before you reach for sqrt. */
    ap_uint<12> ax = (gx < 0) ? (ap_uint<12>)(-gx) : (ap_uint<12>)gx;
    ap_uint<12> ay = (gy < 0) ? (ap_uint<12>)(-gy) : (ap_uint<12>)gy;
    ap_uint<13> mag = (ap_uint<13>)ax + (ap_uint<13>)ay;

    /* Saturate, do not wrap. A wrapping magnitude turns strong edges black,
     * which looks like the filter is broken in exactly the places it is
     * working hardest. */
    ap_uint<8> sat = (mag > 255) ? (ap_uint<8>)255 : (ap_uint<8>)mag;

    return (sat >= threshold) ? sat : (ap_uint<8>)0;
}

void sobel_3x3(gray_stream_t &src, gray_stream_t &dst,
               ap_uint<16> rows, ap_uint<16> cols, ap_uint<8> threshold)
{
#pragma HLS INTERFACE axis port=src
#pragma HLS INTERFACE axis port=dst
#pragma HLS INTERFACE s_axilite port=rows      bundle=ctrl
#pragma HLS INTERFACE s_axilite port=cols      bundle=ctrl
#pragma HLS INTERFACE s_axilite port=threshold bundle=ctrl
#pragma HLS INTERFACE ap_ctrl_none port=return

    /* -- Storage ---------------------------------------------------------
     * Two line delays. Row 0 holds y-2 relative to the incoming pixel, row 1
     * holds y-1.
     *
     * ARRAY_PARTITION dim=1 complete: split into two INDEPENDENT memories so
     * both can be accessed in the same cycle. Without this the two reads
     * serialise onto one BRAM's ports and you get II=2 (or II=3 once you
     * count the writes). This single directive is the difference between a
     * 1080p60 kernel and one that misses frame rate by 2x.
     *
     * dim=2 is deliberately NOT partitioned -- that would turn 1920 pixels
     * into 1920 registers and blow up the design.
     */
    static u8_t linebuf[2][MAX_COLS];
#pragma HLS ARRAY_PARTITION variable=linebuf complete dim=1

    /* Each line buffer row does one read and one write per iteration, at the
     * same address. That needs a dual-port RAM. HLS usually infers RAM_2P by
     * itself; state it explicitly so a tool-version change cannot silently
     * cost you an II.
     *
     * Vitis HLS 2020.1+ spells this `bind_storage`; Vivado HLS spells it
     * `resource`. Both are accepted for a while, then the old one warns.
     */
#if HLS_COMPAT_VITIS
#pragma HLS BIND_STORAGE variable=linebuf type=ram_2p impl=bram
#else
#pragma HLS RESOURCE variable=linebuf core=RAM_2P_BRAM
#endif

    /* The 3x3 window lives entirely in flip-flops. `complete` on both
     * dimensions is mandatory: all nine values are read every cycle. */
    static u8_t win[3][3];
#pragma HLS ARRAY_PARTITION variable=win complete dim=0

    /* Input and output coordinates. Kept as explicit counters rather than
     * derived with / and % from a single index -- a divide by a runtime `cols`
     * is a multi-cycle operation and will destroy II=1. */
    ap_uint<16> ix = 0, iy = 0;   /* coords of the pixel being read   */
    ap_uint<16> ox = 0, oy = 0;   /* coords of the pixel being written */
    /* These are plain locals, NOT static. Under ap_ctrl_none the function
     * restarts every frame, so they reset to 0 at each frame boundary -- which
     * is what you want if `rows`/`cols` can change at runtime. Making them
     * static would carry a partial frame's position across a resolution
     * change and desynchronise the output permanently. linebuf and win ARE
     * static because their contents must survive the II=1 pipeline, not
     * because they should survive a frame. */

    /* Total iterations = frame + pipeline lag. Computed in a width wide
     * enough for 4K: 4096*2160 needs 24 bits, +cols needs 25. */
    const ap_uint<32> n_in  = (ap_uint<32>)rows * (ap_uint<32>)cols;
    const ap_uint<32> lag   = (ap_uint<32>)cols + 1;
    const ap_uint<32> n_tot = n_in + lag;

LOOP_STREAM:
    for (ap_uint<32> i = 0; i < n_tot; ++i) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=(MAX_ROWS*MAX_COLS + MAX_COLS + 1)
#pragma HLS PIPELINE II=1

        /* --- 1. Fetch -------------------------------------------------- */
        u8_t pix = 0;
        if (i < n_in) {
            /* Blocking read: this is where backpressure from upstream shows
             * up as a pipeline stall. Note it is INSIDE a conditional, which
             * is fine -- HLS predicates the stream access. What is NOT fine
             * is making the *number* of reads depend on data; see docs/06. */
            pix = src.read().data;
        }

        /* --- 2. Line buffer shift -------------------------------------- */
        /* Read both delayed rows at the current column, then push down.
         * Read-before-write at the same address is intentional and is why we
         * asked for a dual-port RAM. */
        u8_t p_m2 = linebuf[0][ix];   /* pixel from row iy-2 */
        u8_t p_m1 = linebuf[1][ix];   /* pixel from row iy-1 */
        linebuf[0][ix] = p_m1;
        linebuf[1][ix] = pix;

        /* --- 3. Window shift ------------------------------------------- */
        /* Shift left by one column, insert the new column on the right.
         * UNROLL is redundant here (the bounds are constant so HLS unrolls
         * anyway) but stating it documents the intent and protects against a
         * future edit that makes the bound non-constant. */
    WIN_SHIFT:
        for (int r = 0; r < 3; ++r) {
#pragma HLS UNROLL
            win[r][0] = win[r][1];
            win[r][1] = win[r][2];
        }
        win[0][2] = p_m2;
        win[1][2] = p_m1;
        win[2][2] = pix;

        /* --- 4. Emit --------------------------------------------------- */
        if (i >= lag) {
            /* Output coordinates come from their own counters, incremented at
             * the bottom of this block. Deriving them as (i-lag)/cols and
             * (i-lag)%cols would be mathematically identical and would cost
             * you a runtime integer divide -- roughly 30 cycles of latency
             * and an instant II=1 failure. Never divide or modulo by a
             * runtime value in a pipelined video loop; carry a counter. */

            /* Border replication.
             *
             * The window rows map to output rows oy-1, oy, oy+1 and the
             * columns to ox-1, ox, ox+1. At a frame edge one of those does
             * not exist. We substitute the nearest valid row/column, which is
             * "replicate" (a.k.a. clamp-to-edge) padding.
             *
             * Replicate is the right default for edge detectors: zero padding
             * invents a hard black border and lights up a bright rectangle
             * around your whole image, which people then chase as a bug in
             * the kernel.
             *
             * All nine selects unroll into muxes -- about 9 * 8 LUT6, cheap.
             */
            u8_t w[3][3];
#pragma HLS ARRAY_PARTITION variable=w complete dim=0
        BORDER:
            for (int r = 0; r < 3; ++r) {
#pragma HLS UNROLL
                for (int c = 0; c < 3; ++c) {
#pragma HLS UNROLL
                    int rr = r, cc = c;
                    if (oy == 0            && r == 0) rr = 1;
                    if (oy == rows - 1     && r == 2) rr = 1;
                    if (ox == 0            && c == 0) cc = 1;
                    if (ox == cols - 1     && c == 2) cc = 1;
                    w[r][c] = win[rr][cc];
                }
            }

            gray_axis_t out;
            out.data = sobel_mag(w, threshold);
            AXIS_SET_KEEP(out);
            out.user = (oy == 0 && ox == 0) ? 1 : 0;
            out.last = (ox == cols - 1)     ? 1 : 0;
            out.id   = 0;
            out.dest = 0;

            /* Blocking write: downstream backpressure stalls the whole loop,
             * including the src.read() above, which propagates backpressure
             * upstream. That chain is automatic and is the entire reason
             * hls::stream is safe to use in a video pipeline. */
            dst.write(out);

            if (ox == cols - 1) { ox = 0; oy = (oy == rows - 1) ? (ap_uint<16>)0 : (ap_uint<16>)(oy + 1); }
            else                { ++ox; }
        }

        /* --- 5. Advance input coordinates ------------------------------ */
        /* Runs on EVERY iteration including the trailing flush ones, so the
         * line buffer keeps shifting after the last input pixel. */
        if (ix == cols - 1) { ix = 0; iy = (iy == rows - 1) ? (ap_uint<16>)0 : (ap_uint<16>)(iy + 1); }
        else                { ++ix; }
    }
}
