#include "histogram.h"

/* ===========================================================================
 * hist[v]++ : THE READ-MODIFY-WRITE DEPENDENCE, MEASURED
 *
 * In one iteration you read hist[v], add one, and write it back. If the NEXT
 * iteration has the same v it must see the value this one wrote. HLS cannot
 * know whether two iterations collide -- v is data -- so classically it had to
 * assume they always do, serialising the loop to II=2 or 3.
 *
 * ---------------------------------------------------------------------------
 * THE FOLKLORE IS OUT OF DATE. MEASURE BEFORE YOU "FIX".
 *
 * On Vitis HLS 2023.2 (xczu7ev) ALL FOUR strategies below reach II=1,
 * including the completely naive one, and including when the table is forced
 * into BRAM. The tool inserts the accumulator forwarding by itself. The
 * classic "histograms are II=2, add #pragma HLS DEPENDENCE" advice simply
 * does not reproduce.
 *
 * What the dependence costs now is TIMING, not throughput. Measured:
 *
 *   METHOD          II   est. period   pipeline depth   correct?
 *   0 NAIVE          1     3.680 ns          2            yes
 *   1 FORWARD        1     3.680 ns          2            yes
 *   2 BANKED         1     2.443 ns          4            yes
 *   3 UNSAFE         1     2.443 ns          4            NO
 *
 * against a 3.33 ns target -- so 0 and 1 MISS TIMING and 2 and 3 meet it.
 *
 * The reason is visible in the reported critical path. For METHOD=0:
 *
 *   icmp (loop counter)        1.016 ns
 *   axis read                  1.000 ns
 *   load from array 'hist'     1.237 ns     <-- the RMW, closed in one cycle
 *                              --------
 *                              3.680 ns
 *
 * For METHOD=2 the array load is NOT on the critical path at all; banking
 * gave the scheduler freedom to push it into a later stage (depth 4 instead
 * of 2), leaving just the counter compare and the stream read.
 *
 * So the useful framing is not "how do I get II=1" -- you already have it --
 * but "how do I stop the read-modify-write sitting in a single cycle". Adding
 * pipeline stages (banking) does it correctly; asserting the dependency away
 * does it incorrectly.
 *
 * ---------------------------------------------------------------------------
 * The meta-lesson, which is the real reason this example exists: HLS folklore
 * ages badly. Sweep the variants on YOUR tool version and read the critical
 * path before you complicate working code.
 * ======================================================================== */

void histogram(byte_stream_t &src, count_t hist_out[NBINS],
               ap_uint<32> n_samples)
{
#pragma HLS INTERFACE axis port=src
#pragma HLS INTERFACE s_axilite port=hist_out   bundle=ctrl
#pragma HLS INTERFACE s_axilite port=n_samples  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=return     bundle=ctrl

#if METHOD == 2
    /* ---- METHOD 2: BANKED ------------------------------------------------
     * NBANKS independent copies of the histogram, round-robined by iteration.
     * Consecutive iterations touch different banks, so a collision on the
     * same bin no longer collides in the same memory. The banks are summed
     * at the end.
     *
     * Costs NBANKS times the memory and an NBINS*NBANKS-step merge, and the
     * merge itself is a serial pass you have to pay per frame/block. Use it
     * when the data really is adversarial (long runs of one value) or when
     * you need more than one update per cycle.
     */
    static count_t hist[NBANKS][NBINS];
#pragma HLS ARRAY_PARTITION variable=hist complete dim=1
#else
    static count_t hist[NBINS];
#endif

    /* STORAGE decides whether the dependence bites at all.
     *   0 = auto   -- HLS picks, and for a small table it picks LUTRAM
     *                 (1-cycle), where the read-modify-write closes in one
     *                 cycle and there is nothing to fix.
     *   1 = bram   -- 2-cycle read latency, so the naive form genuinely
     *                 cannot reach II=1 and the strategies below matter.
     * This knob exists because the answer to "is hist[v]++ slow?" is
     * "it depends on where the table landed", which is not obvious. */
#ifndef STORAGE
#define STORAGE 0
#endif
#if STORAGE == 1
  #if METHOD == 2
    #pragma HLS BIND_STORAGE variable=hist type=ram_2p impl=bram
  #else
    #pragma HLS BIND_STORAGE variable=hist type=ram_2p impl=bram
  #endif
#endif

    /* Zero the table. This is not free: NBINS cycles per call, every call.
     * For a 256-bin histogram at 60 Hz that is nothing; for a 64K-bin one it
     * is a real cost, and the usual answer is to clear it lazily with a
     * generation tag rather than rewriting it. */
CLEAR:
    for (int b = 0; b < NBINS; ++b) {
#pragma HLS PIPELINE II=1
#if METHOD == 2
        for (int k = 0; k < NBANKS; ++k) {
#pragma HLS UNROLL
            hist[k][b] = 0;
        }
#else
        hist[b] = 0;
#endif
    }

#if METHOD == 1
    /* METHOD 1 state: the value and bin of the previous iteration. */
    ap_uint<8> prev_bin = 0;
    count_t    prev_val = 0;
    bool       have_prev = false;
#endif

ACCUM:
    for (ap_uint<32> i = 0; i < n_samples; ++i) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=65536
#pragma HLS PIPELINE II=1

        ap_uint<8> v = src.read().data;

#if METHOD == 0
        /* ---- NAIVE ------------------------------------------------------
         * Correct, and on 2023.2 already II=1 -- the tool forwards the
         * accumulator for you. The only thing wrong with it is that the array
         * load lands on the critical path (3.680 ns vs a 3.33 ns target).
         *
         * If your clock has the slack, SHIP THIS. It is the smallest and
         * clearest of the four, and every alternative below is a response to
         * a timing problem you may not have. */
        hist[v] = hist[v] + 1;

#elif METHOD == 1
        /* ---- FORWARDING (accumulator bypass) -----------------------------
         * The fix that costs almost nothing and is always correct.
         *
         * The ONLY case that needs the memory round trip is when this
         * iteration hits the same bin as the previous one. So keep the
         * previous bin's value in a register and forward it:
         *
         *   same bin as last time?  ->  use the register (no read needed)
         *   different bin?          ->  read the memory (guaranteed no conflict)
         *
         * Now the read and the write in one iteration are provably to
         * different addresses whenever the read matters, and HLS can schedule
         * II=1. Cost: one comparator, one mux, two registers.
         *
         * This generalises: whenever a loop-carried dependency only bites on
         * a *rare, detectable* condition, detect it and bypass rather than
         * telling the scheduler to ignore it.
         *
         * MEASURED CAVEAT: on 2023.2 this produces the same II (1) and the
         * same period (3.680 ns) as the naive version, because HLS was
         * already doing exactly this. Writing it by hand costs ~50 FF and
         * ~70 LUT and buys nothing. It is here because it is the right
         * technique on older tools, and because the PATTERN -- detect the
         * collision and bypass -- is the one to reach for when the tool
         * cannot do it for you. */
        count_t cur = (have_prev && v == prev_bin) ? prev_val : hist[v];
        count_t nxt = cur + 1;
        hist[v]   = nxt;
        prev_bin  = v;
        prev_val  = nxt;
        have_prev = true;

#elif METHOD == 2
        /* ---- BANKED ------------------------------------------------------ */
        ap_uint<8> bank = (ap_uint<8>)(i & (NBANKS - 1));
        hist[bank][v] = hist[bank][v] + 1;

#else
        /* ---- METHOD 3: THE TRAP ------------------------------------------
         * #pragma HLS DEPENDENCE tells the scheduler to ASSUME no dependency
         * exists. Here one demonstrably does -- two samples with the same
         * value collide -- so this produces RTL that disagrees with csim and
         * undercounts repeated values.
         *
         * It is included, and enabled only under METHOD=3, because this is
         * the "fix" that gets suggested most often. It reaches II=1. It also
         * silently loses counts. The testbench proves it with a run of
         * identical samples, which is exactly the input a random test vector
         * will not contain.
         *
         * DEPENDENCE is legitimate when you can PROVE independence -- e.g.
         * you know indices are distinct by construction. "The tool complained
         * and this made it stop" is not a proof.
         */
#pragma HLS DEPENDENCE variable=hist inter false
        hist[v] = hist[v] + 1;
#endif
    }

    /* Publish the result. */
OUT:
    for (int b = 0; b < NBINS; ++b) {
#pragma HLS PIPELINE II=1
#if METHOD == 2
        count_t s = 0;
        for (int k = 0; k < NBANKS; ++k) {
#pragma HLS UNROLL
            s += hist[k][b];
        }
        hist_out[b] = s;
#else
        hist_out[b] = hist[b];
#endif
    }
}
