#ifndef FIR_H
#define FIR_H

#include "hls_compat.h"
#include <ap_fixed.h>

/* ---------------------------------------------------------------------------
 * A streaming FIR filter -- the "hello world" of DSP in HLS, and a good
 * vehicle for four techniques that apply far beyond filters:
 *
 *   - a shift register in registers, not memory
 *   - exploiting algebraic structure (symmetry) to halve the multipliers
 *   - sizing types so one multiply fits one DSP slice
 *   - templates, so one implementation covers every tap count
 *
 * Sweep it:
 *   ./scripts/sweep.py --example 14_fir_filter \
 *       --define NTAPS=15,31 --define SYMMETRIC=0,1 --stage csynth
 * ------------------------------------------------------------------------ */

#ifndef NTAPS
#define NTAPS 31            /* odd => a true linear-phase centre tap */
#endif

#ifndef SYMMETRIC
#define SYMMETRIC 1         /* 1 = fold symmetric taps before multiplying */
#endif

/* -- Type sizing, driven by the DSP slice ------------------------------------
 * A DSP48E2 multiplier is 27x18 (18x25 on 7-series). Keep one operand <= 18
 * bits and the other <= 27 and each tap costs exactly one DSP. Let either
 * exceed that and the tool builds the multiply from several DSPs plus adders,
 * which is how a "small" filter suddenly needs 4x the DSPs.
 *
 * sample_t : Q1.15 signed, the usual fixed-point audio/sensor sample.
 * coef_t   : Q1.17 signed. 18 bits is the DSP's free operand width, so there
 *            is no reason to use fewer.
 * acc_t    : must hold NTAPS accumulated products without overflow.
 *            product = 16 + 18 = 34 bits; summing NTAPS of them needs
 *            ceil(log2(NTAPS)) more. 48 bits covers up to ~16000 taps and is
 *            exactly the DSP48 accumulator width, so it is free.
 */
typedef ap_fixed<16, 1, AP_RND_CONV, AP_SAT> sample_t;
typedef ap_fixed<18, 1, AP_RND_CONV, AP_SAT> coef_t;
typedef ap_fixed<48, 8>                      acc_t;

void fir(s16_stream_t &src, s16_stream_t &dst, ap_uint<32> n_samples);

/* Exposed so the testbench can use the same coefficients as the DUT without
 * duplicating them (a golden model that re-types its own coefficients tests
 * your typing, not your filter). */
extern const double FIR_COEF_D[NTAPS];

#endif
