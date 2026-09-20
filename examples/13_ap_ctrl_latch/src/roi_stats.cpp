#include "roi_stats.h"

/* ===========================================================================
 * ap_ctrl_hs + ap_stable: a block that is DRIVEN, not free-running.
 *
 * This is the other half of docs/09. There, config reached a free-running
 * block and the block had to defend itself against mid-frame changes. Here we
 * use the block-level handshake instead: the block declares "my configuration
 * is stable for the duration of one call", and something outside guarantees
 * it. That something is rtl/hls_cfg_latch.sv.
 *
 * The trade:
 *   ap_ctrl_none + latch-on-SOF   no external logic, block defends itself,
 *                                 costs a register per config field
 *   ap_ctrl_hs   + ap_stable      external logic guarantees stability, block
 *                                 is simpler and smaller, and you get ap_done
 *                                 (hence an interrupt) for free
 *
 * For a per-frame measurement that software consumes, the second is the right
 * shape: software needs to know when the result is valid, and that is exactly
 * what ap_done tells it.
 * ======================================================================== */

void roi_stats(gray_stream_t &src,
               ap_uint<16> rows, ap_uint<16> cols,
               ap_uint<16> roi_x, ap_uint<16> roi_y,
               ap_uint<16> roi_w, ap_uint<16> roi_h,
               roi_result_t *out)
{
#pragma HLS INTERFACE axis port=src

    /* -- ap_stable on the geometry ---------------------------------------
     * ap_stable produces a plain input wire (like ap_none) but additionally
     * PROMISES the scheduler the value will not change while the block runs.
     * The scheduler may therefore read it once and fan the result out, rather
     * than re-reading it at every point of use.
     *
     * It is a promise, not an enforcement. Violate it -- change roi_w
     * mid-frame -- and the behaviour is undefined: some comparisons may see
     * the old value and some the new, with no reproducible pattern and no
     * error anywhere. That is precisely why the latch module exists.
     *
     * NOTE these are NOT s_axilite. An ap_stable port is a top-level wire,
     * so the PS cannot write it directly; the latch module owns those wires
     * and the PS writes the latch module's registers instead.
     */
#pragma HLS INTERFACE ap_stable port=rows
#pragma HLS INTERFACE ap_stable port=cols
#pragma HLS INTERFACE ap_stable port=roi_x
#pragma HLS INTERFACE ap_stable port=roi_y
#pragma HLS INTERFACE ap_stable port=roi_w
#pragma HLS INTERFACE ap_stable port=roi_h

    /* -- Results as WIRES, not AXI-Lite ----------------------------------
     * ap_vld gives the struct as a flat output bus plus a `_ap_vld` strobe.
     *
     * This is a deliberate architectural choice: the SystemVerilog wrapper
     * owns the register map and the handshake, and HLS is pure datapath with
     * wire I/O. It is a very common shape when you already have RTL infra --
     * your existing AXI-Lite decoder, your existing interrupt logic, your
     * existing test registers -- and you do not want a second, differently
     * shaped register bank appearing inside an HLS IP.
     *
     * It also makes the block trivially testable in an RTL testbench: no AXI
     * transactions needed to configure it or to read the answer. See
     * docs/15 on design for test. */
#pragma HLS INTERFACE ap_vld port=out

    /* ap_ctrl_hs WITHOUT s_axilite on return exposes ap_start / ap_done /
     * ap_idle / ap_ready as top-level PORTS rather than register bits. That
     * is what lets rtl/hls_cfg_latch.sv drive the block directly, with no
     * processor in the loop. */
#pragma HLS INTERFACE ap_ctrl_hs port=return

    ap_uint<32> sum = 0, count = 0;
    ap_uint<8>  mn = 255, mx = 0;

    const ap_uint<32> x0 = roi_x;
    const ap_uint<32> x1 = (ap_uint<32>)roi_x + roi_w;   /* exclusive */
    const ap_uint<32> y0 = roi_y;
    const ap_uint<32> y1 = (ap_uint<32>)roi_y + roi_h;

LOOP_ROWS:
    for (ap_uint<16> y = 0; y < rows; ++y) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_ROWS
    LOOP_COLS:
        for (ap_uint<16> x = 0; x < cols; ++x) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_COLS
#pragma HLS PIPELINE II=1
            /* The whole frame is consumed regardless of the ROI. A block that
             * stopped reading once it left the ROI would leave the rest of the
             * frame in the FIFO and desynchronise everything downstream --
             * the ap_ctrl_hs equivalent of the token-count bug in docs/04. */
            ap_uint<8> p = src.read().data;

            bool in_roi = (x >= x0) && (x < x1) && (y >= y0) && (y < y1);
            if (in_roi) {
                sum   += p;
                count += 1;
                if (p < mn) mn = p;
                if (p > mx) mx = p;
            }
        }
    }

    out->sum     = sum;
    out->count   = count;
    out->min_val = (count == 0) ? (ap_uint<8>)0 : mn;
    out->max_val = mx;
}
