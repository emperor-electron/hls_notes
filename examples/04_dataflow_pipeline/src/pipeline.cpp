#include "pipeline.h"

/* ===========================================================================
 * A three-stage DATAFLOW video pipeline.
 *
 *   src ->[ingest]-> s0 ->[blur3x3]-> s1 ->[threshold]-> s2 ->[egress]-> dst
 *
 * DATAFLOW makes these four functions run CONCURRENTLY, connected by FIFOs.
 * Without it they run sequentially: stage 2 does not start until stage 1 has
 * finished the entire frame, which for video means you need a full frame
 * buffer between every stage and your latency is measured in frames.
 *
 * Everything difficult about DATAFLOW comes from its canonical-form rules.
 * They are listed at the bottom of this file; read them before editing.
 * ======================================================================== */

/* -------------------------------------------------------------------------
 * Stage 1 -- ingest: strip AXI side channels, convert RGB to luma.
 *
 * Every stage is `static` so it cannot be called from outside the dataflow
 * region, and takes streams by reference. A dataflow stage must NOT be
 * inlined -- HLS needs it to stay a separate process. It normally keeps them
 * separate automatically, but `#pragma HLS INLINE off` makes that explicit and
 * survives a future -O change.
 * ---------------------------------------------------------------------- */
static void stage_ingest(vid_stream_t &src, pix_stream_t &out,
                         ap_uint<16> rows, ap_uint<16> cols)
{
#pragma HLS INLINE off
INGEST_ROWS:
    for (ap_uint<16> y = 0; y < rows; ++y) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_ROWS
    INGEST_COLS:
        for (ap_uint<16> x = 0; x < cols; ++x) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_COLS
#pragma HLS PIPELINE II=1
            rgb_t p = src.read().data;
            ap_uint<27> acc = 19595 * rgb_r(p) + 38470 * rgb_g(p) + 7471 * rgb_b(p);
            out.write((pix_t)((acc + 32768) >> 16));
        }
    }
}

/* -------------------------------------------------------------------------
 * Stage 2 -- 3x3 box blur. Same line-buffer skeleton as example 03.
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

BLUR_STREAM:
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
            /* /9 as a multiply-by-reciprocal: 7282/65536 ~= 1/9.0003.
             * A literal `/9` on a runtime value instantiates a divider.
             * HLS does constant-fold division by a *literal* 9 into exactly
             * this, but writing it out means you can see the rounding. */
            out.write((pix_t)(((ap_uint<28>)sum * 7282 + 32768) >> 16));

            if (ox == cols - 1) { ox = 0; oy = (oy == rows - 1) ? (ap_uint<16>)0 : (ap_uint<16>)(oy + 1); }
            else                { ++ox; }
        }

        if (ix == cols - 1) ix = 0; else ++ix;
    }
}

/* -------------------------------------------------------------------------
 * Stage 3 -- threshold.
 * ---------------------------------------------------------------------- */
static void stage_threshold(pix_stream_t &in, pix_stream_t &out,
                            ap_uint<16> rows, ap_uint<16> cols,
                            ap_uint<8> threshold)
{
#pragma HLS INLINE off
THR_LOOP:
    for (ap_uint<32> i = 0, n = (ap_uint<32>)rows * cols; i < n; ++i) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=(MAX_ROWS*MAX_COLS)
#pragma HLS PIPELINE II=1
        pix_t p = in.read();
        out.write((p >= threshold) ? (pix_t)255 : (pix_t)0);
    }
}

/* -------------------------------------------------------------------------
 * Stage 4 -- egress: reattach AXI side channels.
 * ---------------------------------------------------------------------- */
static void stage_egress(pix_stream_t &in, gray_stream_t &dst,
                         ap_uint<16> rows, ap_uint<16> cols)
{
#pragma HLS INLINE off
EGRESS_ROWS:
    for (ap_uint<16> y = 0; y < rows; ++y) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_ROWS
    EGRESS_COLS:
        for (ap_uint<16> x = 0; x < cols; ++x) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_COLS
#pragma HLS PIPELINE II=1
            gray_axis_t w;
            w.data = in.read();
            AXIS_SET_KEEP(w);
            w.user = (y == 0 && x == 0) ? 1 : 0;
            w.last = (x == cols - 1)    ? 1 : 0;
            w.id = 0; w.dest = 0;
            dst.write(w);
        }
    }
}

/* =========================================================================== */

void video_pipeline(vid_stream_t &src, gray_stream_t &dst,
                    ap_uint<16> rows, ap_uint<16> cols,
                    ap_uint<8> threshold)
{
#pragma HLS INTERFACE axis port=src
#pragma HLS INTERFACE axis port=dst
#pragma HLS INTERFACE s_axilite port=rows      bundle=ctrl
#pragma HLS INTERFACE s_axilite port=cols      bundle=ctrl
#pragma HLS INTERFACE s_axilite port=threshold bundle=ctrl
#pragma HLS INTERFACE ap_ctrl_none port=return

#pragma HLS DATAFLOW

    /* Channel declarations must be the FIRST statements in the dataflow
     * region, before any stage call. */
    pix_stream_t s0("s0"), s1("s1"), s2("s2");

    /* -- FIFO depth ------------------------------------------------------
     * The default is 2. For a chain of same-rate stages that is enough: each
     * stage consumes one token per token produced, so the FIFO only has to
     * absorb one stage's pipeline latency.
     *
     * You need MORE than 2 when the stages are not rate-matched at the token
     * level. Two cases show up constantly in video:
     *
     *   (a) A stage that produces its output in bursts (e.g. writes a whole
     *       line, then stalls). Depth must cover the burst.
     *   (b) A fan-out where two branches rejoin after different latencies.
     *       Depth must cover the latency SKEW. That is example 05, and
     *       getting it wrong is a deadlock, not a slowdown.
     *
     * Here everything is 1-in-1-out, so 8 is already generous. It is cheap
     * insurance: at 8 bits x 8 deep these are LUTRAM (SRL), not BRAM.
     *
     * s1 is deeper because stage_blur emits nothing for the first `cols+1`
     * beats and then emits one per beat -- upstream keeps producing during
     * that window, so s0 must absorb the fill. cols+1 at 1920 would be a
     * BRAM; 64 is enough because stage_blur is still READING during its fill,
     * so s0 never actually backs up. Sizing FIFOs by "what could back up"
     * rather than "what does" is how you accidentally spend all your BRAM.
     */
#pragma HLS STREAM variable=s0 depth=64
#pragma HLS STREAM variable=s1 depth=8
#pragma HLS STREAM variable=s2 depth=8

    stage_ingest   (src, s0, rows, cols);
    stage_blur     (s0,  s1, rows, cols);
    stage_threshold(s1,  s2, rows, cols, threshold);
    stage_egress   (s2,  dst, rows, cols);
}

/* ===========================================================================
 * DATAFLOW CANONICAL FORM -- the rules you must not break
 * ===========================================================================
 *
 * If HLS cannot prove your region is canonical it either refuses to apply
 * DATAFLOW (you get a warning and a sequential design that is 4x too slow) or
 * applies it and produces something that deadlocks. Enable
 * `config_dataflow -strict_mode error` so the first case is loud.
 *
 * 1. SINGLE PRODUCER, SINGLE CONSUMER.
 *    Each channel is written by exactly one stage and read by exactly one
 *    stage. If two stages need the same data, duplicate it with an explicit
 *    fan-out stage (example 05). This is the rule people break most.
 *
 * 2. NO BYPASS.
 *    Data must flow stage 1 -> 2 -> 3. A channel from stage 1 straight to
 *    stage 3, skipping 2, breaks the form. Pass it THROUGH stage 2 even if
 *    stage 2 does nothing with it. Yes, that costs a FIFO. Pay it.
 *
 * 3. NO FEEDBACK.
 *    A channel from a later stage back to an earlier one is not allowed.
 *    If you genuinely need feedback (e.g. auto-exposure), close the loop
 *    OUTSIDE the dataflow region, across frames, through a static.
 *
 * 4. STAGE CALLS ARE THE ONLY STATEMENTS.
 *    Between the channel declarations and the end of the function there
 *    should be nothing but stage calls. A stray assignment, a conditional
 *    around a stage call, or a loop over stage calls all break canonical
 *    form. Scalars that stages need are passed as arguments -- and note that
 *    `rows`, `cols`, `threshold` are read by several stages, which is legal
 *    precisely because they are scalars, not channels.
 *
 * 5. EVERY STAGE MUST RUN, EVERY TIME.
 *    `if (cond) stage_b(...)` is illegal. Push the condition INSIDE the
 *    stage and have it pass data through unchanged when disabled -- it must
 *    still consume and produce the same number of tokens. See example 06.
 *
 * 6. TOKEN COUNTS MUST MATCH.
 *    Over one execution of the region, the number of writes to a channel must
 *    equal the number of reads from it. If stage A writes rows*cols tokens and
 *    stage B reads rows*cols+1, stage B blocks forever on the last read and
 *    the whole region hangs. Note that `rows` and `cols` are passed to every
 *    stage separately -- if one stage latched an old value you would get
 *    exactly this mismatch, which is why they are function arguments rather
 *    than statics.
 *
 * 7. NO STATIC SHARED BETWEEN STAGES.
 *    A static inside one stage is fine (the line buffer above). A static at
 *    file scope touched by two stages creates an invisible dependency that
 *    DATAFLOW does not model, and the stages will race.
 * ======================================================================== */
