#ifndef HISTOGRAM_H
#define HISTOGRAM_H

#include "hls_compat.h"

/* ---------------------------------------------------------------------------
 * The read-modify-write dependence problem, in its purest form.
 *
 *     hist[data[i]]++;
 *
 * Three lines of C, and the single most instructive II problem in HLS. It is
 * not specific to histograms -- the same shape appears in scatter-adds,
 * sparse accumulation, reference counting, binning, graph algorithms and
 * anything that updates a table at a data-dependent index.
 *
 * Sweep the four strategies:
 *   ./scripts/sweep.py --example 16_histogram_dependence \
 *       --define METHOD=0,1,2,3 --define STORAGE=0,1 --stage csynth
 * ------------------------------------------------------------------------ */

/* Default is 2 (banked): it is the only strategy that is both CORRECT and
 * meets the repo's 3.33 ns target. Use 0 (naive) if your clock has slack --
 * it is smaller and clearer. See the measured table in histogram.cpp. */
#ifndef METHOD
#define METHOD 2
#endif
/*  0  NAIVE      hist[v]++ with no help    -> II=1, 3.680 ns  (correct)
 *  1  FORWARD    manual accumulator bypass -> II=1, 3.680 ns  (correct, redundant on 2023.2)
 *  2  BANKED     N banks summed at the end -> II=1, 2.443 ns  (correct, 2x LUT)
 *  3  UNSAFE     assert no dependence      -> II=1, 2.443 ns  (LOSES COUNTS)
 *
 * All four reach II=1 on Vitis HLS 2023.2 -- the classic "histogram is II=2"
 * advice does not reproduce. The difference is timing. See histogram.cpp.
 */

#ifndef NBINS
#define NBINS 256
#endif

#ifndef NBANKS
#define NBANKS 4          /* METHOD=2 only */
#endif

typedef ap_uint<32> count_t;

void histogram(byte_stream_t &src, count_t hist_out[NBINS],
               ap_uint<32> n_samples);

#endif
