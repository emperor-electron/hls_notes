#include "rgb_to_gray.h"

/* ===========================================================================
 * Luma conversion: the canonical "one pixel in, one pixel out" video kernel.
 *
 * Demonstrates:
 *   - the bounded-loop free-running pattern (cosim-able, still free-running)
 *   - runtime resolution via s_axilite, bounded by compile-time maxima
 *   - integer arithmetic instead of float, and why
 *   - regenerating TUSER/TLAST rather than copying them
 * ======================================================================== */

/* ITU-R BT.601 luma:  Y = 0.299 R + 0.587 G + 0.114 B
 *
 * Do NOT write this with floats. A float multiply-add chain synthesises to
 * three DSP-heavy FP cores, costs ~10 cycles of latency, and buys you nothing:
 * the input is 8 bits.
 *
 * Instead scale the coefficients to a power-of-two denominator. Q16 (65536)
 * gives ~5 decimal digits of coefficient accuracy, far beyond what 8-bit
 * output can resolve. The multiplies become 8x16 -> 24-bit, which fits one
 * DSP48 slice each (and HLS will often merge them).
 */
static const ap_uint<17> KR = 19595;   /* round(0.299 * 65536) */
static const ap_uint<17> KG = 38470;   /* round(0.587 * 65536) */
static const ap_uint<17> KB =  7471;   /* round(0.114 * 65536) */
/* 19595 + 38470 + 7471 = 65536 exactly -- the coefficients must sum to the
 * scale factor or white (255,255,255) will not map to 255. Rounding each one
 * independently and hoping is how you get a 1-LSB DC offset that survives all
 * the way to silicon. */

static inline u8_t luma(rgb_t p)
{
#pragma HLS INLINE
    /* Width accounting, done deliberately rather than left to int promotion:
     *   8-bit pixel x 17-bit coeff  -> 25 bits
     *   sum of three               -> 27 bits (worst case 255*65536 = 2^24)
     * Declaring the accumulator exactly 27 bits stops HLS carrying a 32-bit
     * int adder chain around. On a 1-pixel kernel this is noise; in a 3x3
     * window filter replicated 3x it is real LUTs. */
    ap_uint<27> acc = KR * rgb_r(p) + KG * rgb_g(p) + KB * rgb_b(p);

    /* Round-to-nearest, not truncate. `+ 32768` before the shift costs one
     * adder and removes a systematic -0.5 LSB bias. Truncation bias is
     * invisible on a single stage and accumulates visibly through a pipeline
     * of several such stages. */
    return (u8_t)((acc + 32768) >> 16);
}

void rgb_to_gray(vid_stream_t &src, gray_stream_t &dst,
                 ap_uint<16> rows, ap_uint<16> cols)
{
#pragma HLS INTERFACE axis port=src
#pragma HLS INTERFACE axis port=dst
#pragma HLS INTERFACE s_axilite port=rows bundle=ctrl
#pragma HLS INTERFACE s_axilite port=cols bundle=ctrl
#pragma HLS INTERFACE ap_ctrl_none port=return

    /* -- The bounded-loop free-running pattern ------------------------------
     * Under ap_ctrl_none, HLS wraps the entire function body in an implicit
     * infinite loop. So this nested for-loop still runs forever in hardware --
     * it processes a frame, "returns", and is immediately restarted.
     *
     * Compared to while(true) you get, for free:
     *   + csim terminates naturally
     *   + cosim works (the RTL does reach a completion point)
     *   + HLS knows the trip count, so the latency report is meaningful
     *
     * What you give up:
     *   - The block now assumes every frame is exactly rows*cols beats. If an
     *     upstream source emits a short frame, this block stays permanently
     *     one-frame-offset: it will consume the first lines of frame N+1 as
     *     the last lines of frame N, and the TLAST/TUSER it emits will be
     *     wrong forever after. It does not deadlock, it desynchronises, which
     *     is harder to notice. Example 07 fixes this.
     */
LOOP_ROWS:
    for (ap_uint<16> y = 0; y < rows; ++y) {
        /* max/avg/min trip counts. This directive changes NOTHING about the
         * generated hardware -- it only makes the latency report honest for a
         * runtime-variable bound. Without it HLS reports "?" and you cannot
         * tell whether you hit your frame rate. */
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_ROWS avg=MAX_ROWS

    LOOP_COLS:
        for (ap_uint<16> x = 0; x < cols; ++x) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_COLS avg=MAX_COLS
            /* PIPELINE on the inner loop. HLS will automatically flatten the
             * perfect loop nest, so the row boundary costs no extra cycles.
             * Check the synthesis log for "Loop 'LOOP_ROWS' was flattened" --
             * if flattening failed (usually because you put a statement
             * between the two `for`s, making the nest imperfect) you pay one
             * pipeline-flush per line, which at 1080p is ~1080 * depth wasted
             * cycles per frame. */
#pragma HLS PIPELINE II=1

            vid_axis_t in = src.read();

            gray_axis_t out;
            out.data = luma(in.data);
            AXIS_SET_KEEP(out);

            /* Regenerate the side channels from the loop counters rather than
             * copying in.user / in.last.
             *
             * Copying is tempting and is right only if you trust the source.
             * Regenerating means this block emits a structurally valid frame
             * even if the input's side channels are malformed -- which turns a
             * downstream deadlock into a visible picture artefact. In a
             * bring-up pipeline that trade is almost always worth it.
             *
             * The cost: it hard-codes the assumption above (every frame is
             * exactly rows x cols). Pick one and be explicit about it.
             */
            out.user = (y == 0 && x == 0) ? 1 : 0;
            out.last = (x == cols - 1)    ? 1 : 0;
            out.id   = 0;
            out.dest = 0;

            dst.write(out);
        }
    }
}
