#include "fir.h"

/* ===========================================================================
 * Coefficients: a windowed-sinc low-pass, cutoff 0.25 * fs/2.
 *
 * Generated once, offline, and pasted in as `const double`. HLS converts them
 * to coef_t at COMPILE TIME, so the doubles cost nothing -- there is no
 * floating-point hardware anywhere in the result. Writing them as doubles
 * keeps them readable and lets the testbench share them.
 *
 * The filter is LINEAR PHASE: h[i] == h[NTAPS-1-i]. That symmetry is what
 * the SYMMETRIC path below exploits.
 * ======================================================================== */

/* Only the tap counts with a checked-in coefficient table are supported.
 * A silent fallback to an all-zero filter would "work" -- it synthesises, it
 * streams, and it outputs silence -- which is a much worse failure than not
 * compiling. Make unsupported configurations a build error. */
#if (NTAPS != 15) && (NTAPS != 31)
#error "NTAPS must be 15 or 31 (no coefficient table for other lengths)"
#endif

/* A constexpr-style table built by the preprocessor would be unreadable, so
 * the coefficients are computed once at static-init time in the testbench's
 * world and hard-coded here for synthesis. Keeping them in one array that
 * both sides read is the point. */
const double FIR_COEF_D[NTAPS] = {
#if NTAPS == 15
   0.00111830, -0.00389476, -0.01603492, -0.02036377,  0.02095181,  0.12449781,
   0.24450683,  0.29843741,  0.24450683,  0.12449781,  0.02095181, -0.02036377,
  -0.01603492, -0.00389476,  0.00111830
#else /* 31 */
   0.00169486,  0.00120149, -0.00090473, -0.00422755, -0.00542704,  0.00000000,
   0.01136510,  0.01858422,  0.00825010, -0.02123646, -0.04893921, -0.03959026,
   0.02985813,  0.14510695,  0.25451078,  0.29950724,  0.25451078,  0.14510695,
   0.02985813, -0.03959026, -0.04893921, -0.02123646,  0.00825010,  0.01858422,
   0.01136510,  0.00000000, -0.00542704, -0.00422755, -0.00090473,  0.00120149,
   0.00169486
#endif
};

/* The coefficient ROM in hardware. `static const` means HLS builds it as
 * constants wired into the multipliers, not a memory -- so it costs nothing
 * and needs no partitioning directive.
 *
 * If you need RUNTIME-reloadable coefficients, make this a non-const static
 * array written from an s_axilite or stream port, and partition it completely.
 * That turns free constants into real registers plus real multipliers; it is
 * the single biggest cost difference between a fixed and a programmable FIR,
 * and worth knowing before you promise "programmable". */
static const coef_t COEF[NTAPS] = {
#if NTAPS == 15
   0.00111830, -0.00389476, -0.01603492, -0.02036377,  0.02095181,  0.12449781,
   0.24450683,  0.29843741,  0.24450683,  0.12449781,  0.02095181, -0.02036377,
  -0.01603492, -0.00389476,  0.00111830
#else /* 31 */
   0.00169486,  0.00120149, -0.00090473, -0.00422755, -0.00542704,  0.00000000,
   0.01136510,  0.01858422,  0.00825010, -0.02123646, -0.04893921, -0.03959026,
   0.02985813,  0.14510695,  0.25451078,  0.29950724,  0.25451078,  0.14510695,
   0.02985813, -0.03959026, -0.04893921, -0.02123646,  0.00825010,  0.01858422,
   0.01136510,  0.00000000, -0.00542704, -0.00422755, -0.00090473,  0.00120149,
   0.00169486
#endif
};

void fir(s16_stream_t &src, s16_stream_t &dst, ap_uint<32> n_samples)
{
#pragma HLS INTERFACE axis port=src
#pragma HLS INTERFACE axis port=dst
#pragma HLS INTERFACE s_axilite port=n_samples bundle=ctrl
#pragma HLS INTERFACE ap_ctrl_none port=return

    /* -- The delay line --------------------------------------------------
     * A shift register, held entirely in flip-flops.
     *
     * `complete` partitioning is mandatory: every tap is read in the same
     * cycle. Without it HLS builds a BRAM and the MAC serialises, taking
     * NTAPS cycles per sample instead of one.
     *
     * Note this is the same structure as a video line buffer (example 03)
     * with the "line" being one sample deep. Neighbourhood access over a
     * stream is one pattern, whatever the data means.
     */
    static sample_t shift[NTAPS];
#pragma HLS ARRAY_PARTITION variable=shift complete dim=1

FIR_SAMPLES:
    for (ap_uint<32> n = 0; n < n_samples; ++n) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=65536
#pragma HLS PIPELINE II=1

        s16_axis_t in = src.read();

        /* Reinterpret the 16 raw bits as Q1.15. `.range()` is a bit-copy, not
         * a numeric conversion -- exactly what you want at a protocol edge
         * where the wire carries a raw two's-complement sample. Assigning
         * `x = in.data` instead would convert an INTEGER 12345 into the fixed
         * point value 12345.0 and saturate. This confusion is the most common
         * fixed-point-on-a-bus bug. */
        sample_t x;
        x.range(15, 0) = in.data.range(15, 0);

        /* Shift down. Fully unrolled into a register cascade. */
    SHIFT:
        for (int i = NTAPS - 1; i > 0; --i) {
#pragma HLS UNROLL
            shift[i] = shift[i - 1];
        }
        shift[0] = x;

        acc_t acc = 0;

#if SYMMETRIC
        /* -- Folded (linear-phase) form ----------------------------------
         * Because h[i] == h[NTAPS-1-i], we can add the two samples that
         * share a coefficient FIRST and multiply once:
         *
         *     h[i]*x[i] + h[i]*x[N-1-i]  ==  h[i] * (x[i] + x[N-1-i])
         *
         * That halves the multiplier count for free. The pre-adder is
         * literally free on a DSP48 -- the slice has a dedicated pre-adder
         * input for exactly this. Expect ~NTAPS/2 DSPs instead of NTAPS.
         *
         * This only works for a SYMMETRIC filter. Check your coefficients;
         * applying it to an asymmetric filter silently computes garbage. */
    MAC_FOLDED:
        for (int i = 0; i < NTAPS / 2; ++i) {
#pragma HLS UNROLL
            acc_t pre = (acc_t)shift[i] + (acc_t)shift[NTAPS - 1 - i];
            acc += pre * COEF[i];
        }
        if (NTAPS & 1) {
            acc += (acc_t)shift[NTAPS / 2] * COEF[NTAPS / 2];
        }
#else
        /* -- Direct form -------------------------------------------------
         * One multiply per tap. HLS builds an adder tree over the products,
         * which pipelines fine but costs NTAPS multipliers. */
    MAC_DIRECT:
        for (int i = 0; i < NTAPS; ++i) {
#pragma HLS UNROLL
            acc += (acc_t)shift[i] * COEF[i];
        }
#endif

        /* Saturate back into the sample type. acc_t is deliberately wider
         * than sample_t, so this single conversion is where rounding and
         * saturation happen -- the "quantise once" rule from docs/12. */
        sample_t y = (sample_t)acc;

        s16_axis_t out;
        out.data.range(15, 0) = y.range(15, 0);
        AXIS_SET_KEEP(out);
        out.user = (n == 0) ? 1 : 0;
        out.last = (n == n_samples - 1) ? 1 : 0;
        out.id = 0; out.dest = 0;
        dst.write(out);
    }
}
