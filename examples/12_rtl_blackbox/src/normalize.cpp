#include "normalize.h"

/* ===========================================================================
 * HLS CONTROL LOGIC AROUND A HAND-WRITTEN RTL CORE.
 *
 * This is the shape the pattern usually takes: HLS owns the sequencing,
 * buffering and bookkeeping; the RTL owns one specific operation that HLS
 * would implement badly.
 *
 * Here: a per-frame auto-normalisation. Accumulate the frame's maximum, then
 * scale every pixel of the NEXT frame by 255/max. The divide happens once per
 * frame, so a 32-cycle iterative divider is free -- whereas asking HLS for a
 * pipelined divider would cost DSPs and LUTs permanently for an operation used
 * 60 times a second.
 *
 * Note the feedback is across FRAMES, through statics, not inside a dataflow
 * region -- see docs/04 rule 3.
 * ======================================================================== */

void normalize(gray_stream_t &src, gray_stream_t &dst,
               ap_uint<16> rows, ap_uint<16> cols,
               ap_uint<32> target)
{
#pragma HLS INTERFACE axis port=src
#pragma HLS INTERFACE axis port=dst
#pragma HLS INTERFACE s_axilite port=rows bundle=ctrl
#pragma HLS INTERFACE s_axilite port=cols bundle=ctrl
#pragma HLS INTERFACE s_axilite port=target bundle=ctrl

    /* ap_ctrl_hs, NOT ap_ctrl_none -- for two independent reasons.
     *
     * 1. DESIGN: this block does per-frame work (the divide) outside the pixel
     *    loop, so it genuinely has a beginning and an end. Software pulses
     *    ap_start per frame and can poll ap_done.
     *
     * 2. TOOL: cosim refuses ap_ctrl_none unless the design is combinational,
     *    a pure II=1 pipeline, or purely stream-ported:
     *      ERROR: [COSIM 212-345] Cosim only supports the following
     *      'ap_ctrl_none' designs: (1) combinational designs; (2) pipelined
     *      design with II of 1; (3) designs with array streaming or
     *      hls_stream or AXI4 stream ports.
     *    A blackbox call after the loop is a sequential tail, so it fails that
     *    test -- and cosim is the only way to exercise the real RTL. */
#pragma HLS INTERFACE s_axilite port=return bundle=ctrl

    /* Scale factor derived from the PREVIOUS frame, in Q16. Initialised to
     * 1.0 so the first frame passes through unchanged. */
    static ap_uint<32> scale_q16 = (ap_uint<32>)1 << FX_FRAC;
    static ap_uint<8>  prev_max  = 255;

    ap_uint<8> frame_max = 0;

LOOP_ROWS:
    for (ap_uint<16> y = 0; y < rows; ++y) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_ROWS
    LOOP_COLS:
        for (ap_uint<16> x = 0; x < cols; ++x) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_COLS
#pragma HLS PIPELINE II=1
            ap_uint<8> p = src.read().data;
            if (p > frame_max) frame_max = p;

            /* Q16 multiply then shift back. The DIVIDE is not here -- it is
             * once per frame, below. This is the whole architectural point:
             * keep the expensive operation out of the per-pixel loop. */
            ap_uint<40> scaled = (ap_uint<40>)p * scale_q16;
            ap_uint<24> v = scaled >> FX_FRAC;

            gray_axis_t o;
            o.data = (v > 255) ? (ap_uint<8>)255 : (ap_uint<8>)v;
            AXIS_SET_KEEP(o);
            o.user = (y == 0 && x == 0) ? 1 : 0;
            o.last = (x == cols - 1)    ? 1 : 0;
            o.id = 0; o.dest = 0;
            dst.write(o);
        }
    }

    /* --- The blackbox call ------------------------------------------------
     * Once per frame, outside the pipelined loop.
     *
     * HLS generates the ap_ctrl_chain handshake around this: it asserts
     * ap_start, waits for ap_done, samples the outputs, asserts ap_continue.
     * The 32 cycles the divider takes are simply 32 cycles this function is
     * not producing pixels -- which is fine here because it happens during
     * what would be vertical blanking.
     *
     * A blackbox call CANNOT go inside a DATAFLOW region when its arguments
     * are scalars or pointers (only hls::stream and arrays are supported
     * there). Keeping it at the top level like this sidesteps that entirely.
     */
    prev_max = (frame_max == 0) ? (ap_uint<8>)1 : frame_max;

    /* ---- DO NOT PASS A COMPILE-TIME CONSTANT TO A BLACKBOX ------------
     * Writing fx_divide(255, prev_max, q) here produced SILENTLY WRONG RTL on
     * Vitis HLS 2023.2. HLS constant-folded the literal 255 away, and the
     * remaining ports shifted: the generated instantiation was
     *
     *     fx_divide grp_fx_divide_fu_141(
     *         ... .num(grp_fx_divide_fu_141_den),   // den's wire on num!
     *             .den(32'd0)                       // tied to zero
     *     );                                        // quot not connected AT ALL
     *
     * csim passed (it runs the C model, which never sees this). csynth issued
     * no warning. Cosim hung forever at "0 / 2 transactions".
     *
     * Keep every blackbox argument a genuine runtime value. `target` is an
     * s_axilite register, which HLS cannot fold, and it is useful anyway --
     * it makes the normalisation target configurable. */
    ap_uint<32> q;
    fx_divide(target, (ap_uint<32>)prev_max, q);
    scale_q16 = q;
}
