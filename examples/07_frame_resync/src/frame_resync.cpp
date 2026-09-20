#include "frame_resync.h"

/* ===========================================================================
 * FRAME RESYNCHRONISATION -- how a block survives a malformed upstream.
 *
 * Examples 05 and 06 were about deadlocks *inside* your IP. This one is about
 * the failure mode that looks like a deadlock from the outside but is not:
 * permanent DESYNCHRONISATION.
 *
 * Every counted block in this repo (examples 02-06) assumes each frame is
 * exactly rows*cols beats. If an upstream source ever delivers a short frame
 * -- a sensor glitch, a mode change, a DMA underrun, a cable reconnect -- a
 * counted block does not hang and does not error. It silently consumes the
 * first N lines of frame K+1 as the last N lines of frame K, and every frame
 * after that is wrong. Forever. A power cycle is the only fix.
 *
 * That is worse than a deadlock. A deadlock at least stops and tells you.
 *
 * This block is counted (so its output geometry is always exactly rows x cols,
 * which is what downstream needs) but it RESYNCHRONISES on the input's
 * Start-Of-Frame. One bad frame costs you one bad frame.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS CANNOT DEADLOCK
 *
 * Per iteration it performs at most one blocking read and at most one blocking
 * write, in that order, with no other stage depending on it. There is no cycle
 * in the wait graph:
 *
 *     upstream stalls  -> we stall on read   -> we assert backpressure. Fine.
 *     downstream stalls-> we stall on write  -> we stop reading.        Fine.
 *
 * Both are recoverable the instant the other side moves. A deadlock needs a
 * CYCLE in the blocking graph, and a straight-line 1-in-1-out block cannot
 * form one by itself. It can only participate in a cycle created elsewhere
 * (a fan-out that rejoins -- example 05).
 * ======================================================================== */

void frame_resync(vid_stream_t &src, vid_stream_t &dst,
                  ap_uint<16> rows, ap_uint<16> cols,
                  resync_status_t *status)
{
#pragma HLS INTERFACE axis port=src
#pragma HLS INTERFACE axis port=dst
#pragma HLS INTERFACE s_axilite port=rows   bundle=ctrl
#pragma HLS INTERFACE s_axilite port=cols   bundle=ctrl
#pragma HLS INTERFACE s_axilite port=status bundle=ctrl
#pragma HLS INTERFACE ap_ctrl_none port=return

    /* State that must survive the implicit restart of a free-running block. */
    static bool        synced       = false;
    static ap_uint<16> ox           = 0;
    static ap_uint<16> oy           = 0;
    static ap_uint<32> c_frames     = 0;
    static ap_uint<32> c_resyncs    = 0;
    static ap_uint<32> c_dropped    = 0;
    static ap_uint<32> c_eolmiss    = 0;

    /* ONE-BEAT OUTPUT LOOKAHEAD.
     *
     * This is the trick that makes graceful recovery possible. By holding the
     * most recent output beat back by one cycle, we can go back and assert
     * TLAST on it after the fact -- when we discover, one beat too late, that
     * the frame ended early.
     *
     * Without it, an early SOF leaves the downstream consumer parked in the
     * middle of a packet waiting for a TLAST that will never come. That IS a
     * deadlock, just one hop outside your module, and it is the reason
     * "my IP is fine, the DMA is broken" tickets exist.
     *
     * Cost: one beat of latency and ~30 flip-flops. Pay it.
     */
    static vid_axis_t pending;
    static bool       have_pending = false;

LOOP:
    HLS_FOREVER_ON(src) {
#pragma HLS PIPELINE II=1

        vid_axis_t w = src.read();

        /* ---- 1. Hunt for Start-Of-Frame -------------------------------
         * Until we have seen a TUSER we do not know where we are in the
         * raster, so we discard. Discarding means output beats != input
         * beats, which is FINE here because dst is a real AXI4-Stream (a
         * data-driven interface) -- and would be ILLEGAL if dst were an
         * internal dataflow channel with a counted consumer. See example 06.
         */
        if (!synced) {
            if (!w.user) {
                if (c_dropped != (ap_uint<32>)-1) ++c_dropped;  /* saturate */
                continue;
            }
            synced = true;
            ox = 0;
            oy = 0;
        }

        /* ---- 2. Resynchronise on an unexpected SOF --------------------- */
        if (w.user && !(ox == 0 && oy == 0)) {
            if (c_resyncs != (ap_uint<32>)-1) ++c_resyncs;

            /* Close the packet we are half-way through so downstream is not
             * left waiting on a TLAST forever.
             *
             * NOTE what this does NOT do: it does not write `pending` here.
             * It retroactively sets TLAST on the held beat and lets the single
             * write in step 5 emit it.
             *
             * The obvious version --
             *     if (have_pending) { pending.last = 1; dst.write(pending); }
             * -- is functionally identical and costs you II=1:
             *
             *   WARNING: [HLS 200-880] The II Violation ... Unable to enforce
             *   a carried dependence constraint (II = 1, distance = 1,
             *   offset = 1) between axis write operation 'dst_V_data_V_write'
             *   ... and axis write operation 'dst_V_data_V_write' ...
             *
             * HLS cannot schedule two writes to the same AXI-Stream port in a
             * single II=1 iteration, so it silently settles for II=2 and your
             * throughput halves. This is the general rule and it is worth
             * internalising:
             *
             *     AT MOST ONE read and ONE write per stream port per
             *     iteration of a pipelined loop.
             *
             * When you need a second access on an exceptional path, fold it
             * into the existing one -- here, by mutating the held beat instead
             * of emitting it early. */
            if (have_pending) {
                pending.last = 1;
            }
            ox = 0;
            oy = 0;
        }

        /* ---- 3. Consistency check (diagnostic only) --------------------
         * We do NOT act on the input's TLAST -- our own counters define the
         * output geometry. But a mismatch is worth counting: a non-zero
         * eol_mismatch with zero resyncs means the upstream's line length
         * disagrees with the `cols` register, which is a configuration bug,
         * not a glitch. That distinction turns a two-day debug into a
         * two-minute one. */
        {
            ap_uint<1> expect_last = (ox == cols - 1) ? 1 : 0;
            if (w.last != expect_last) {
                if (c_eolmiss != (ap_uint<32>)-1) ++c_eolmiss;
            }
        }

        /* ---- 4. Build the clean output beat ---------------------------- */
        vid_axis_t o;
        o.data = w.data;
        AXIS_SET_KEEP(o);
        o.user = (ox == 0 && oy == 0) ? 1 : 0;
        o.last = (ox == cols - 1)     ? 1 : 0;
        o.id   = 0;
        o.dest = 0;

        /* ---- 5. Push through the lookahead register -------------------- */
        if (have_pending) dst.write(pending);
        pending      = o;
        have_pending = true;

        /* ---- 6. Advance -------------------------------------------------
         * Note the frame counter increments on the LAST pixel of the last
         * row, i.e. when the frame is complete, not when the next one starts.
         * Software polling `frames_out` then sees a number it can trust. */
        bool frame_done = false;
        if (ox == cols - 1) {
            ox = 0;
            if (oy == rows - 1) {
                oy = 0;
                if (c_frames != (ap_uint<32>)-1) ++c_frames;
                frame_done = true;
            } else {
                ++oy;
            }
        } else {
            ++ox;
        }

        /* ---- 7. Publish the status registers --------------------------
         * ONCE PER FRAME, not once per pixel.
         *
         * Writing four 32-bit AXI-Lite registers on every beat put the
         * estimated period at 3.501 ns against a 3.33 ns target -- a timing
         * failure caused entirely by diagnostics. Diagnostic counters at
         * 60 Hz are plenty; nobody polls them at 148.5 MHz.
         *
         * A frame-boundary update also gives software a CONSISTENT snapshot:
         * update per-pixel and a reader can catch `frames_out` from after the
         * increment and `resyncs` from before it, and conclude a frame was
         * clean when it was not. */
        if (frame_done) {
            status->frames_out   = c_frames;
            status->resyncs      = c_resyncs;
            status->sof_dropped  = c_dropped;
            status->eol_mismatch = c_eolmiss;
        }
    }

#ifndef __SYNTHESIS__
    /* csim only: flush the lookahead so the testbench sees the whole frame.
     * In hardware the block never exits this function, so the pending beat is
     * simply the newest pixel and is emitted as soon as the next one arrives.
     * A free-running video block that stops one pixel short is invisible;
     * a testbench that is one pixel short fails loudly. */
    if (have_pending) {
        dst.write(pending);
        have_pending = false;
    }
    /* Publish the final counters. In hardware they are published at each frame
     * boundary (step 7); a csim case that feeds a partial frame would
     * otherwise read stale values and the TB would report a phantom failure. */
    status->frames_out   = c_frames;
    status->resyncs      = c_resyncs;
    status->sof_dropped  = c_dropped;
    status->eol_mismatch = c_eolmiss;
    /* Reset EVERY static so back-to-back csim test cases are independent --
     * including have_pending and the status counters. Missing one is a real
     * trap: leaving the counters alone makes case N's `resyncs` leak into
     * case N+1, and you spend an afternoon debugging a resync that never
     * happened. Leaving `have_pending` alone leaks a stale pixel across cases.
     *
     * In hardware there is no equivalent -- these persist until reset, and the
     * counters are deliberately cumulative-since-reset -- which is exactly why
     * the testbench must exercise the "second frame" path explicitly rather
     * than relying on a fresh start. */
    synced = false;
    ox = oy = 0;
    c_frames = c_resyncs = c_dropped = c_eolmiss = 0;
#endif
}

/* ===========================================================================
 * WHAT THIS BLOCK DELIBERATELY DOES NOT DO
 *
 * 1. NO TIMEOUT ON A BLOCKING READ.
 *    There is no way to express "read with a 1000-cycle timeout" in HLS, and
 *    you should not want one. A stalled read is correct backpressure
 *    behaviour, not a fault. If you genuinely need a watchdog (e.g. to raise
 *    an interrupt when the sensor stops), put it in a SEPARATE always-running
 *    block or in PL logic outside HLS, counting cycles since the last TVALID.
 *    Do not try to build it inside the data path -- you will end up using
 *    read_nb() in a spin loop, which burns the pipeline slot and still cannot
 *    distinguish "slow" from "stopped".
 *
 * 2. NO LINE-LENGTH DISCOVERY.
 *    It would be possible to learn `cols` from the first TLAST. It would also
 *    make the output geometry depend on input data, which means downstream
 *    line buffers (sized at compile time from MAX_COLS) could overflow. Take
 *    the geometry from a register; use the input's side channels only to
 *    align to it.
 *
 * 3. NO DROPPING OF PARTIAL FRAMES.
 *    A frame interrupted mid-way is still emitted, just truncated and properly
 *    terminated. Buffering a whole frame to decide whether to emit it costs a
 *    frame of latency and a frame buffer. If your application must never show
 *    a torn frame, do that at the frame-buffer stage (example 08), not here.
 * ======================================================================== */
