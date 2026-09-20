#include "feature_scan.h"

/* ===========================================================================
 * DATA-DEPENDENT TOKEN COUNTS -- the second big deadlock family.
 *
 * Example 05 deadlocked on FIFO *depth*: every count matched, the ordering was
 * just wrong. This one deadlocks on FIFO *counts*: a producer whose number of
 * writes depends on pixel values, feeding a consumer whose number of reads is
 * fixed at rows*cols.
 *
 * The rule, stated once:
 *
 *     Over one execution of a dataflow region, for every channel,
 *     #writes must equal #reads -- and both must be computable from
 *     values that BOTH stages can see.
 *
 * "Both stages can see" is the part people miss. Even if the counts happen to
 * match, if the consumer cannot know the count without inspecting the data,
 * you have not satisfied the rule; you have got lucky on one test vector.
 *
 * ---------------------------------------------------------------------------
 * WHAT CATCHES WHAT
 *
 *   csim   CATCHES this bug and fails the build. Verified on Vitis HLS
 *          2023.2 with VARIANT=0:
 *
 *            ERROR [HLS SIM]: an hls::stream is read while empty, which may
 *            result in RTL simulation hanging.
 *            ERROR: [SIM 211-100] 'csim_design' failed: nonzero return value.
 *
 *          The mechanism is not an inline check: read() blocks on a condition
 *          variable and a detector thread notices, about a second later, that
 *          more streams are blocked than there are running tasks, then
 *          abort()s. Hence the delay and the hedged wording of the message.
 *
 *          Two gaps. Data LEFT OVER in a stream is only a WARNING
 *          ("contains leftover data") and still exits 0, so over-production
 *          slips through -- and it deadlocks a downstream consumer just as
 *          effectively. And -DALLOW_EMPTY_HLS_STREAM_READS downgrades the
 *          empty read to a warning returning a default value. So grep the
 *          csim log too: hls::check_csim_log in scripts/hls_lib.tcl does,
 *          and build.tcl calls it after every csim.
 *
 *          (Do NOT define __VITIS_HLS__ yourself when compiling a testbench
 *          with plain g++. It is synthesis-only; defining it puts
 *          hls_stream.h into a "bcsim" mode that real csim does not use,
 *          where empty reads silently return default values and your test
 *          passes when it should not.)
 *
 *   csim   does NOT see example 05's depth deadlock at all: counts matched
 *          there, and csim's streams are unbounded.
 *
 *   cosim  catches both. A depth deadlock just hangs.
 *
 * So: a csim "read while empty" failure means a COUNT bug (this file). A cosim
 * hang with clean csim means a DEPTH or ORDERING bug (example 05). That
 * distinction will save you hours.
 * ======================================================================== */

/* -------------------------------------------------------------------------
 * Producer. Scans the frame and identifies "features" (pixels at or above a
 * threshold). How it reports them is the whole point.
 * ---------------------------------------------------------------------- */
static void stage_scan(gray_stream_t &src, tok_stream_t &out,
                       ap_uint<16> rows, ap_uint<16> cols,
                       ap_uint<8> threshold)
{
#pragma HLS INLINE off
SCAN_ROWS:
    for (ap_uint<16> y = 0; y < rows; ++y) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_ROWS
    SCAN_COLS:
        for (ap_uint<16> x = 0; x < cols; ++x) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_COLS
#pragma HLS PIPELINE II=1
            ap_uint<8> p = src.read().data;
            bool hit = (p >= threshold);

            tok_t t;
            t.coord = ((ap_uint<32>)y << 16) | (ap_uint<32>)x;
            t.valid = hit ? 1 : 0;
            t.eos   = 0;

#if VARIANT == 0
            /* BROKEN. The number of writes depends on pixel data.
             * On a frame with no bright pixels this writes ZERO tokens and
             * the consumer below blocks on its first read, forever. */
            if (hit) out.write(t);

#elif VARIANT == 1
            /* TAGGED. Always exactly one token per pixel; the `valid` bit
             * carries the decision. Counts are now a pure function of
             * rows*cols, which both stages already have as arguments.
             *
             * Cost: the FIFO carries every pixel's token whether or not it is
             * a hit, so throughput is fixed at one pixel per beat -- you get
             * no speedup from sparsity. For a video pipeline that is exactly
             * right: the pipeline is paced by the pixel clock anyway, and a
             * data-dependent rate would just create backpressure jitter
             * upstream. Sparsity-driven speedup only pays off when the
             * downstream stage is the bottleneck AND hits are rare. */
            out.write(t);

#elif VARIANT == 2
            /* SENTINEL. Variable count, terminated by an explicit
             * end-of-stream token. The consumer becomes data-driven: it reads
             * until it sees eos, so it never reads more than was written.
             *
             * This is legal and it is what you want when the compaction ratio
             * really matters. Two things to get right:
             *   - the sentinel must be written on EVERY path out of the
             *     producer, including early exits and error paths;
             *   - the consumer must not also have a fixed trip count, or you
             *     have reintroduced the bug with extra steps. */
            if (hit) out.write(t);
#endif
        }
    }

#if VARIANT == 2
    tok_t eos;
    eos.coord = 0;
    eos.valid = 0;
    eos.eos   = 1;
    out.write(eos);
#endif
}

/* -------------------------------------------------------------------------
 * Consumer. Forwards features to an AXI4-Stream and counts them.
 *
 * Written as one function per variant rather than one function full of #if.
 * A preprocessor conditional that straddles a loop header is a real pattern in
 * the wild and a real source of "why does this compile differently in csim and
 * csynth" -- __SYNTHESIS__ and your own defines interact, and the version HLS
 * sees is not always the version you read.
 * ---------------------------------------------------------------------- */

/* Shared tail: one-beat lookahead so TLAST lands on the final feature.
 * A coordinate list with no TLAST leaves the downstream DMA waiting for a
 * packet boundary that never arrives -- a deadlock one hop OUTSIDE your IP,
 * which is the hardest kind to find. */
struct emit_ctx {
    coord_axis_t pending;
    bool         have_pending;
    bool         first;
    ap_uint<32>  n;
};

static inline void emit_push(emit_ctx &c, coord_stream_t &dst, ap_uint<32> coord)
{
#pragma HLS INLINE
    if (c.have_pending) dst.write(c.pending);
    c.pending.data = coord;
    AXIS_SET_KEEP(c.pending);
    c.pending.user = c.first ? 1 : 0;
    c.pending.last = 0;
    c.pending.id = 0;
    c.pending.dest = 0;
    c.have_pending = true;
    c.first = false;
    ++c.n;
}

static inline void emit_flush(emit_ctx &c, coord_stream_t &dst)
{
#pragma HLS INLINE
    if (c.have_pending) {
        c.pending.last = 1;
        dst.write(c.pending);
    }
}

static inline void emit_init(emit_ctx &c)
{
#pragma HLS INLINE
    c.have_pending = false;
    c.first = true;
    c.n = 0;
}

#if VARIANT == 0
/* BROKEN: fixed trip count against a data-dependent producer. */
static void stage_emit(tok_stream_t &in, coord_stream_t &dst,
                       ap_uint<16> rows, ap_uint<16> cols,
                       ap_uint<32> *n_found)
{
#pragma HLS INLINE off
    emit_ctx c; emit_init(c);
EMIT_BROKEN:
    for (ap_uint<32> i = 0, total = (ap_uint<32>)rows * cols; i < total; ++i) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=(MAX_ROWS*MAX_COLS)
#pragma HLS PIPELINE II=1
        /* Blocks forever as soon as the producer has run out of hits.
         * In csim this aborts: "ERROR [HLS SIM]: an hls::stream is read
         * while empty". In cosim it hangs. In hardware it is an IP that stops
         * passing video the first time a frame is dark. */
        tok_t t = in.read();
        emit_push(c, dst, t.coord);
    }
    emit_flush(c, dst);
    *n_found = c.n;
}

#elif VARIANT == 1
/* TAGGED: exactly rows*cols tokens, one per pixel, `valid` carries the hit. */
static void stage_emit(tok_stream_t &in, coord_stream_t &dst,
                       ap_uint<16> rows, ap_uint<16> cols,
                       ap_uint<32> *n_found)
{
#pragma HLS INLINE off
    emit_ctx c; emit_init(c);
EMIT_TAGGED:
    for (ap_uint<32> i = 0, total = (ap_uint<32>)rows * cols; i < total; ++i) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=(MAX_ROWS*MAX_COLS)
#pragma HLS PIPELINE II=1
        tok_t t = in.read();
        if (t.valid) emit_push(c, dst, t.coord);
    }
    emit_flush(c, dst);
    *n_found = c.n;
}

#else
/* SENTINEL: data-driven, reads until end-of-stream. */
static void stage_emit(tok_stream_t &in, coord_stream_t &dst,
                       ap_uint<16> rows, ap_uint<16> cols,
                       ap_uint<32> *n_found)
{
#pragma HLS INLINE off
    (void)rows; (void)cols;
    emit_ctx c; emit_init(c);
EMIT_UNTIL_EOS:
    while (true) {
        /* LOOP_TRIPCOUNT is advisory only -- it changes no hardware. Without
         * it the latency report reads "?" and you cannot tell whether the
         * block meets frame rate. */
#pragma HLS LOOP_TRIPCOUNT min=1 max=(MAX_ROWS*MAX_COLS + 1)
#pragma HLS PIPELINE II=1
        tok_t t = in.read();
        if (t.eos) break;
        emit_push(c, dst, t.coord);
    }
    emit_flush(c, dst);
    *n_found = c.n;
}
#endif

/* ======================================================================== */

void feature_scan(gray_stream_t &src, coord_stream_t &dst,
                  ap_uint<16> rows, ap_uint<16> cols,
                  ap_uint<8> threshold, ap_uint<32> *n_found)
{
#pragma HLS INTERFACE axis port=src
#pragma HLS INTERFACE axis port=dst
#pragma HLS INTERFACE s_axilite port=rows      bundle=ctrl
#pragma HLS INTERFACE s_axilite port=cols      bundle=ctrl
#pragma HLS INTERFACE s_axilite port=threshold bundle=ctrl
#pragma HLS INTERFACE s_axilite port=n_found   bundle=ctrl
#pragma HLS INTERFACE ap_ctrl_none port=return

#pragma HLS DATAFLOW

    tok_stream_t feats("feats");
#pragma HLS STREAM variable=feats depth=16

    /* Note: nothing but channel declarations, pragmas and stage calls in this
     * body. Writing `*n_found = n;` here after the stage calls would break
     * canonical form (rule 4), so the pointer is passed INTO the stage. */
    stage_scan(src, feats, rows, cols, threshold);
    stage_emit(feats, dst, rows, cols, n_found);
}

/* ===========================================================================
 * THE OTHER WAY TO GET THIS WRONG: non-blocking access.
 *
 * It is tempting to "fix" a count mismatch with write_nb / read_nb:
 *
 *     if (!out.full())  out.write_nb(t);        // DON'T
 *     if (!in.empty())  v = in.read_nb();       // DON'T
 *
 * This does not fix anything; it converts a deadlock into silent data loss,
 * which is strictly worse. The pipeline no longer hangs, so nothing alerts
 * you, and you drop pixels under backpressure -- at a rate that depends on
 * downstream timing, so it is not reproducible.
 *
 * Legitimate uses of non-blocking access are narrow:
 *   - draining a stream at the end of a frame during error recovery
 *     (example 07)
 *   - a genuinely optional side channel, e.g. a statistics output that the
 *     host may or may not be reading, where dropping IS the specified
 *     behaviour and the drop is counted and reported
 *
 * If you cannot state, in one sentence, what the system does when the
 * non-blocking access fails, you want a blocking access.
 * ======================================================================== */
