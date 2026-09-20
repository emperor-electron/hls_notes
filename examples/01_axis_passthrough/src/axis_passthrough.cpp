#include "axis_passthrough.h"

/* ===========================================================================
 * The smallest useful AXI4-Stream module, and every decision in it matters.
 * ======================================================================== */

void axis_passthrough(vid_stream_t &src, vid_stream_t &dst)
{
    /* -- Port-level interfaces ---------------------------------------------
     * `axis` on an hls::stream<ap_axiu<...>> gives you a full AXI4-Stream
     * slave/master pair: TDATA/TVALID/TREADY plus the side channels present in
     * the ap_axiu template arguments.
     *
     * The direction is inferred from how the function uses the argument, not
     * declared. If you read from it you get a slave (s_axis), if you write you
     * get a master (m_axis). A stream you both read and write is an error.
     */
#pragma HLS INTERFACE axis port=src
#pragma HLS INTERFACE axis port=dst

    /* -- Block-level protocol ----------------------------------------------
     * ap_ctrl_none removes ap_start/ap_done/ap_idle/ap_ready. The RTL becomes
     * free-running: it processes whatever arrives, forever, with no CPU
     * involvement.
     *
     * This is almost always what you want in a video pipeline. With the
     * default ap_ctrl_hs, the block processes exactly one "call" per ap_start
     * pulse and then stops -- so unless something drives ap_start every frame,
     * your pipeline delivers one frame and hangs. That is the #1 "my HLS video
     * IP produces one frame then dies" bug, and it is not a deadlock: the
     * block is idle and waiting to be told to go again.
     *
     * The cost: no s_axilite control register, so no runtime configuration.
     * See axis_passthrough_ctrl() below for the hybrid.
     */
#pragma HLS INTERFACE ap_ctrl_none port=return

    /* -- The loop ----------------------------------------------------------
     * An infinite loop is correct and intentional here. With ap_ctrl_none the
     * function never "returns" in hardware; HLS synthesises the loop body into
     * a free-running datapath.
     *
     * Two things make this safe:
     *   1. src.read() is BLOCKING. In RTL it means "assert TREADY, wait for
     *      TVALID". The pipeline stalls automatically when there is no input.
     *   2. dst.write() is BLOCKING. In RTL it means "assert TVALID, wait for
     *      TREADY". The pipeline stalls automatically under backpressure.
     *
     * You do not write any backpressure logic. HLS generates the stall network
     * from the blocking semantics. Everything hard about backpressure comes
     * from cases where those stalls interact badly -- see docs/05 and docs/06.
     */
LOOP_PIXELS:
    HLS_FOREVER_ON(src) {
        /* II=1: one pixel in, one pixel out, every cycle it is not stalled.
         * `rewind` is meaningless on a while(true); do not add it.
         *
         * HLS_FOREVER_ON expands to while(true) under synthesis and to
         * while(!src.empty()) under csim -- see common/hls_compat.h. */
#pragma HLS PIPELINE II=1

        vid_axis_t w = src.read();

        /* Copy the whole word so TLAST/TUSER/TKEEP/TDEST all propagate.
         * A surprising amount of broken video IP fails here: people copy
         * .data and .last but forget .user, and then the downstream Video
         * Frame Buffer Write never sees Start-Of-Frame and produces nothing
         * while looking perfectly healthy on a scope. */
        dst.write(w);
    }
}

/* ===========================================================================
 * Hybrid: free-running data path, AXI4-Lite control.
 *
 * You usually DO want a control register (enable, bypass, thresholds). The
 * trick is that you can have s_axilite ports while keeping ap_ctrl_none, as
 * long as the *return* port is ap_ctrl_none and the scalars are s_axilite.
 * ======================================================================== */

void axis_passthrough_ctrl(vid_stream_t &src, vid_stream_t &dst,
                           ap_uint<1> bypass)
{
#pragma HLS INTERFACE axis port=src
#pragma HLS INTERFACE axis port=dst

    /* The scalar lives in an AXI4-Lite register bank at `bundle=ctrl`.
     * Because the return port is ap_ctrl_none there is no CTRL/ap_start
     * register in that bank -- just your own fields plus the interrupt
     * registers, which you can ignore.
     *
     * Sampling semantics: with ap_ctrl_none there is no "function call
     * boundary", so HLS reads the register whenever the datapath needs it,
     * i.e. potentially mid-frame. If a mid-frame change would corrupt a frame,
     * latch the register on Start-Of-Frame yourself (shown below) rather than
     * hoping the software writes it during blanking.
     */
#pragma HLS INTERFACE s_axilite port=bypass bundle=ctrl
#pragma HLS INTERFACE ap_ctrl_none port=return

    static ap_uint<1> bypass_latched = 0;
    /* `static` inside an ap_ctrl_none function is a register that persists
     * across "calls" -- exactly what we want for a latched config value. */
#pragma HLS RESET variable=bypass_latched

LOOP_PIXELS:
    HLS_FOREVER_ON(src) {
#pragma HLS PIPELINE II=1
        vid_axis_t w = src.read();

        /* Latch config at Start-Of-Frame so a mid-frame register write cannot
         * produce a half-old half-new frame. */
        if (w.user) bypass_latched = bypass;

        if (!bypass_latched) {
            /* Trivial "processing" so the two paths differ: invert. */
            w.data = ~w.data;
        }
        dst.write(w);
    }
}
