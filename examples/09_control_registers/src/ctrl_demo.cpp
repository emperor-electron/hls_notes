#include "ctrl_demo.h"

/* ===========================================================================
 * RECEIVING CONFIGURATION FROM THE PS
 *
 * Three ways a value gets from an ARM core into an HLS datapath:
 *
 *   s_axilite   A memory-mapped register the PS writes over AXI4-Lite.
 *               This is what you want 95% of the time.
 *   ap_none     A raw input wire. No handshake, no register -- you drive it
 *               from PL logic (a GPIO block, a decoder, another IP).
 *   m_axi       The PS puts a descriptor in DDR and you fetch it. For
 *               configuration too large for registers (gamma LUTs, matrices).
 *
 * This example is about the first, and specifically about the part that is
 * genuinely hard: NOT reading the register, but deciding WHEN to read it.
 *
 * ---------------------------------------------------------------------------
 * THE SAMPLING PROBLEM
 *
 * With #pragma HLS INTERFACE ap_ctrl_none port=return there is no function
 * call boundary, so there is no defined moment at which arguments are
 * "passed". HLS reads each s_axilite register whenever the datapath happens to
 * need it -- which can be in the middle of a frame, and can be at a different
 * time for each register.
 *
 * Two consequences people get bitten by:
 *
 *   1. TEARING IN TIME. Write r_gain, g_gain, b_gain from software and the
 *      hardware may pick up the new r_gain and the old g_gain, because the
 *      three AXI-Lite writes are three separate bus transactions and the
 *      datapath sampled between them. One frame comes out with a colour cast.
 *
 *   2. TEARING IN SPACE. A register sampled mid-frame gives you the top of
 *      the frame processed with the old value and the bottom with the new.
 *
 * Neither is a tool bug. Both are what "no call boundary" means. The fixes are
 * below, in increasing order of strength.
 * ======================================================================== */

void ctrl_demo(vid_stream_t &src, vid_stream_t &dst,
               ap_uint<16> rows, ap_uint<16> cols,
               ap_uint<16> r_gain, ap_uint<16> g_gain, ap_uint<16> b_gain,
               ap_uint<32> cfg_seq,
               ap_uint<1>  bypass,
               ctrl_status_t *status)
{
#pragma HLS INTERFACE axis port=src
#pragma HLS INTERFACE axis port=dst

    /* -- The control bundle ------------------------------------------------
     * `bundle=ctrl` puts every one of these into a SINGLE AXI4-Lite slave.
     * Without an explicit bundle each scalar can land in its own slave
     * interface, and your block design grows a forest of AXI ports.
     *
     * The register offsets are generated, not chosen by you. After csynth
     * they are documented in:
     *
     *   <proj>/<solution>/impl/misc/drivers/<top>_v1_0/src/x<top>_hw.h
     *
     * and the matching C driver (XCtrl_demo_Set_r_gain(), etc.) sits beside
     * it. USE THE GENERATED DRIVER, or at least the generated header -- the
     * offsets move whenever you add, remove or reorder an argument, and a
     * hand-written #define 0x10 will silently address the wrong field after
     * an innocuous signature change.
     */
#pragma HLS INTERFACE s_axilite port=rows    bundle=ctrl
#pragma HLS INTERFACE s_axilite port=cols    bundle=ctrl
#pragma HLS INTERFACE s_axilite port=r_gain  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=g_gain  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=b_gain  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=cfg_seq bundle=ctrl
#pragma HLS INTERFACE s_axilite port=bypass  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=status  bundle=ctrl

    /* Free-running: no ap_start, no ap_done, no CPU in the data path.
     * Note this is compatible with s_axilite on the scalars above -- you get
     * your registers, you just do not get a CTRL/ap_start register. */
#pragma HLS INTERFACE ap_ctrl_none port=return

    /* ---- Active (latched) configuration -------------------------------
     * These are the values the datapath actually uses. They are updated from
     * the shadow registers only at a frame boundary, and only atomically.
     *
     * static => a real register that survives the implicit restart of a
     * free-running block. */
    static ap_uint<16> act_r = 256, act_g = 256, act_b = 256;  /* Q8.8 1.0 */
    static ap_uint<32> seq_latched = 0;
    static bool        seq_init    = false;

    static ap_uint<32> c_frames  = 0;
    static ap_uint<32> c_applied = 0;

    ap_uint<16> ox = 0, oy = 0;

LOOP_ROWS:
    for (ap_uint<16> y = 0; y < rows; ++y) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_ROWS
    LOOP_COLS:
        for (ap_uint<16> x = 0; x < cols; ++x) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_COLS
#pragma HLS PIPELINE II=1

            vid_axis_t w = src.read();

            /* ---- FIX 1+2: atomic, frame-synchronous config commit -------
             *
             * The generation-counter (a.k.a. sequence-number) handshake:
             *
             *   Software:  write r_gain, g_gain, b_gain in any order
             *              then write cfg_seq = cfg_seq + 1     <- commit
             *
             *   Hardware:  at Start-Of-Frame, if cfg_seq != seq_latched,
             *              copy ALL the shadow registers at once and record
             *              the new cfg_seq.
             *
             * This solves both tearing problems with one mechanism:
             *   - ATOMIC, because the gains are only ever read on the cycle
             *     the sequence number changed, and software guarantees they
             *     were all written before it bumped the counter;
             *   - FRAME-SYNCHRONOUS, because that cycle is always the first
             *     pixel of a frame.
             *
             * Why a counter and not a single "apply" bit? Because clearing an
             * apply bit requires the PL to WRITE BACK to an s_axilite input
             * register, which HLS will not do for you -- an s_axilite scalar
             * argument is read-only from the hardware side. A monotonically
             * increasing counter needs no write-back: hardware just remembers
             * the last value it saw. Software never has to poll for an ack.
             *
             * Software must write the gains BEFORE bumping cfg_seq. On a
             * Cortex-A that means a barrier (or a `volatile` write through a
             * Device-nGnRnE mapping, which is already ordered). Writes to
             * strongly-ordered device memory are not reordered, so on Zynq
             * with the usual ioremap the ordering is free -- but say so in
             * the driver, because the next person will use a cached mapping
             * for something and wonder why config tears.
             */
            bool sof = (ox == 0 && oy == 0);
            if (sof && (!seq_init || cfg_seq != seq_latched)) {
                act_r = r_gain;
                act_g = g_gain;
                act_b = b_gain;
                seq_latched = cfg_seq;
                seq_init    = true;
                if (c_applied != (ap_uint<32>)-1) ++c_applied;
            }

            /* ---- THE HAZARD, left in on purpose -------------------------
             * `bypass` is read directly, with no latching. If software writes
             * it mid-frame, the top of the frame is processed one way and the
             * bottom the other. For a single-bit enable that is often
             * acceptable and occasionally even desirable (an emergency
             * passthrough should take effect NOW, not next frame).
             *
             * The point is that it is a CHOICE. Make it deliberately per
             * register, and write down which registers are frame-synchronous
             * and which are immediate, because your software team cannot
             * infer it from the register map. */
            rgb_t p = w.data;
            if (!bypass) {
                /* Q8.8 gain, round-to-nearest, saturate. 255 * 65535 needs 24
                 * bits before the shift; 32 is comfortable. */
                ap_uint<32> r = ((ap_uint<32>)rgb_r(p) * act_r + 128) >> 8;
                ap_uint<32> g = ((ap_uint<32>)rgb_g(p) * act_g + 128) >> 8;
                ap_uint<32> b = ((ap_uint<32>)rgb_b(p) * act_b + 128) >> 8;
                p = rgb_pack(r > 255 ? (u8_t)255 : (u8_t)r,
                             g > 255 ? (u8_t)255 : (u8_t)g,
                             b > 255 ? (u8_t)255 : (u8_t)b);
            }

            vid_axis_t o;
            o.data = p;
            AXIS_SET_KEEP(o);
            o.user = sof ? 1 : 0;
            o.last = (ox == cols - 1) ? 1 : 0;
            o.id = 0; o.dest = 0;
            dst.write(o);

            bool frame_done = false;
            if (ox == cols - 1) {
                ox = 0;
                if (oy == rows - 1) { oy = 0; frame_done = true; }
                else                { ++oy; }
            } else {
                ++ox;
            }

            if (frame_done) {
                if (c_frames != (ap_uint<32>)-1) ++c_frames;
                /* Status published once per frame, not per pixel. Four
                 * AXI-Lite writes per beat is a documented way to fail timing
                 * for no reason -- see docs/06 section 6.8. It also gives the
                 * PS a CONSISTENT snapshot rather than a mix of pre- and
                 * post-increment values. */
                status->frames_out   = c_frames;
                status->cfg_applied  = c_applied;
                status->cfg_seq_seen = seq_latched;
            }
        }
    }

#ifndef __SYNTHESIS__
    /* csim only: make test cases independent, and publish the final counters
     * so a TB that sends a partial frame still sees them. Hardware has no
     * equivalent -- these persist until reset. */
    status->frames_out   = c_frames;
    status->cfg_applied  = c_applied;
    status->cfg_seq_seen = seq_latched;
#endif
}

/* ===========================================================================
 * THE OTHER INTERFACE MODES, AND WHEN THEY ARE RIGHT
 *
 * ap_none -- a bare input wire.
 *
 *     #pragma HLS INTERFACE ap_none port=mode
 *
 *   No register, no handshake, no AXI. The signal appears as a top-level
 *   input port you wire up in the block design (from an AXI GPIO, a constant,
 *   a pin, or another IP's output). Use it when the value comes from PL, not
 *   from software. HLS samples it combinationally whenever it likes, so every
 *   caveat above about mid-frame sampling applies, minus the atomicity
 *   problem (there is no multi-word write to tear).
 *
 * ap_stable -- "I promise this does not change while the block is running."
 *
 *     #pragma HLS INTERFACE ap_stable port=cols
 *
 *   Same RTL port as ap_none, but it tells the SCHEDULER the value is
 *   constant, so it may read it once and reuse it instead of re-reading it
 *   every cycle. That can remove a fanout bottleneck on a value used in many
 *   places (a resolution used by every loop bound, say).
 *
 *   It is a PROMISE, not an enforcement. Break it -- change the value while
 *   the block is streaming -- and the behaviour is undefined: part of the
 *   design may see the old value and part the new, with no pattern you can
 *   predict or reproduce. Only use it for values software writes once at
 *   configuration time and then leaves alone, and only if your driver
 *   actually guarantees that (typically: stop the pipeline, write, restart).
 *
 * ap_vld / ap_ack / ap_hs -- a scalar with its own handshake.
 *
 *   Rarely what you want for configuration. ap_vld gives you a data+valid
 *   pair and lets you detect "a new value arrived", which sounds appealing --
 *   but on a free-running block you then have to decide what to do if it
 *   arrives mid-frame, and you are back to the problem above. The generation
 *   counter is simpler and needs no extra ports.
 *
 * m_axi -- configuration too big for registers.
 *
 *   A 256-entry gamma LUT is 1 KB. Do not make that 256 AXI-Lite registers.
 *   Put it in DDR, give the PS a pointer register, and burst it into a BRAM
 *   at Start-Of-Frame. Note that this reintroduces every hazard in
 *   example 08: you are now blocking on the interconnect from inside your
 *   data path. Double-buffer the LUT in BRAM (load into the inactive copy,
 *   swap at SOF) so a slow DDR read can never stall the pixel stream.
 * ======================================================================== */
