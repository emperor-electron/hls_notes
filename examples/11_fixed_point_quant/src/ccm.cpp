#include "ccm.h"

/* ===========================================================================
 * FIXED-POINT ARITHMETIC AND MATCHING A REFERENCE MODEL
 *
 * A 3x3 colour-correction matrix: out = clamp(M * in), a real video operation
 * with exactly the properties that make fixed-point interesting -- signed
 * coefficients, accumulation, and a saturating 8-bit output.
 *
 * ---------------------------------------------------------------------------
 * THE ONE RULE
 *
 *   Quantise ONCE, at the end. Never in the middle.
 *
 * Every intermediate you round is a rounding you have to model in your
 * reference, and the errors accumulate in a way that depends on scheduling.
 * Carry full precision through the accumulation (acc_t has 13 fractional bits,
 * same as the coefficients, so the products are exact) and let the single
 * conversion to pixout_t apply the rounding and saturation.
 *
 * This is also cheaper: rounding costs an adder per rounding point.
 * ======================================================================== */

/* Coefficients. A plausible sRGB-ish correction matrix; rows sum to ~1.0 so
 * neutral grey stays neutral.
 *
 * These are `static const coef_t` initialised from double literals. The
 * conversion happens at COMPILE TIME -- no hardware, no runtime cost. The Q/O
 * modes on coef_t therefore only affect how accurately the literal is
 * captured, never the datapath. */
static const coef_t M[3][3] = {
    { 1.3977, -0.3178, -0.0799 },
    { -0.2320, 1.4218, -0.1898 },
    { -0.0456, -0.4192, 1.4648 }
};

/* Exposed so the testbench can characterise the quantiser in isolation. */
pixout_t ccm_quantise(double v)
{
    /* Assignment from double to an ap_ufixed applies CCM_Q then CCM_O.
     * Note the ORDER: quantise first, then handle overflow. A value of 255.6
     * with AP_RND rounds to 256, which then saturates to 255. With AP_TRN it
     * truncates to 255 and never overflows. That interaction surprises people
     * who assume saturation happens on the pre-rounded value. */
    pixout_t p = v;
    return p;
}

sq_t ccm_quantise_signed(double v)
{
    sq_t q = v;
    return q;
}

static pixout_t ccm_pixel(u8_t r, u8_t g, u8_t b, int row)
{
#pragma HLS INLINE
    /* Products are exact: 8 integer bits x 13 fractional bits fits acc_t's
     * 12 integer + 13 fractional with room to spare. HLS will NOT insert any
     * rounding here because no precision is lost. */
    /* NOTE pixin_t, not coef_t, for the pixel operands.
     *
     * Writing `(coef_t)r` here is a real and easy mistake: coef_t is
     * ap_fixed<16,3>, so it saturates at ~3.9999 and every pixel above 3
     * collapses to the same value. The output still looks like an image --
     * washed out and wrong -- which is why this survives a casual eyeball
     * check. Always give the operands a type wide enough for their own range;
     * the coefficient's type is for the coefficient.
     *
     * Widths: coef_t<16,3> * pixin_t<8,8> -> ap_fixed<24,11>, with 13
     * fractional bits. acc_t is <25,12> with 13 fractional bits, so every
     * product is held EXACTLY and no rounding occurs during accumulation. */
    acc_t acc = (acc_t)0;
    acc += (acc_t)(M[row][0] * (pixin_t)r);
    acc += (acc_t)(M[row][1] * (pixin_t)g);
    acc += (acc_t)(M[row][2] * (pixin_t)b);

    /* THE single quantisation point. acc_t -> pixout_t applies CCM_Q
     * (rounding) and CCM_O (saturation / wrap). */
    return (pixout_t)acc;
}

void ccm(vid_stream_t &src, vid_stream_t &dst,
         ap_uint<16> rows, ap_uint<16> cols)
{
#pragma HLS INTERFACE axis port=src
#pragma HLS INTERFACE axis port=dst
#pragma HLS INTERFACE s_axilite port=rows bundle=ctrl
#pragma HLS INTERFACE s_axilite port=cols bundle=ctrl
#pragma HLS INTERFACE ap_ctrl_none port=return

LOOP_ROWS:
    for (ap_uint<16> y = 0; y < rows; ++y) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_ROWS
    LOOP_COLS:
        for (ap_uint<16> x = 0; x < cols; ++x) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_COLS
#pragma HLS PIPELINE II=1
            vid_axis_t w = src.read();
            rgb_t p = w.data;
            u8_t r = rgb_r(p), g = rgb_g(p), b = rgb_b(p);

            vid_axis_t o;
            o.data = rgb_pack((u8_t)ccm_pixel(r, g, b, 0),
                              (u8_t)ccm_pixel(r, g, b, 1),
                              (u8_t)ccm_pixel(r, g, b, 2));
            AXIS_SET_KEEP(o);
            o.user = (y == 0 && x == 0) ? 1 : 0;
            o.last = (x == cols - 1)    ? 1 : 0;
            o.id = 0; o.dest = 0;
            dst.write(o);
        }
    }
}

/* ===========================================================================
 * BIT GROWTH: what ap_fixed does that plain C does not
 *
 * ap_fixed arithmetic GROWS the result type to be lossless:
 *
 *   ap_fixed<8,4> a, b;
 *   a + b   -> ap_fixed<9,5>     (one extra integer bit for the carry)
 *   a * b   -> ap_fixed<16,8>    (widths add)
 *   a / b   -> ap_fixed<...>     (grows a lot; avoid -- see docs/06)
 *
 * The loss happens on ASSIGNMENT, not in the expression. This is the single
 * most common fixed-point bug in HLS:
 *
 *   ap_fixed<8,4> c = a * b;     // computes in <16,8>, then truncates to <8,4>
 *                                // using c's Q and O modes
 *
 * ...which means the rounding mode that applies is the one on the
 * DESTINATION type, not on the operands. If you want AP_RND you must put it
 * on `c`. Putting it on `a` and `b` does nothing to this statement.
 *
 * Corollary: an intermediate written to a narrow named variable is a
 * quantisation point you probably did not intend. If you find your hardware
 * disagrees with your reference by a fraction of an LSB in unpredictable
 * places, look for a narrow intermediate.
 *
 * ---------------------------------------------------------------------------
 * MATCHING AN EXISTING MODEL
 *
 * Which strategy applies depends on what your reference actually is:
 *
 * 1. REFERENCE IS FLOAT/DOUBLE (numpy, MATLAB double, a C model).
 *    You CANNOT match it bit-exactly, and chasing that is wasted effort. The
 *    reference has ~52 bits of mantissa; you have 13 fractional bits. Define
 *    an error budget instead:
 *        max |hw - ref| <= 1 LSB of the output
 *    and verify it statistically over a large input set, including the
 *    extremes. The testbench here does exactly that and reports the
 *    distribution.
 *
 * 2. REFERENCE IS ITSELF FIXED-POINT (a C model with int arithmetic, a
 *    standard like ITU-R BT.601, another team's RTL).
 *    Now bit-exactness is achievable AND required. Do not use ap_fixed's
 *    implicit conversions for this -- use ap_int/ap_uint with EXPLICIT shifts
 *    and explicit rounding, mirroring the reference operation for operation:
 *        ap_int<27> acc = k0*r + k1*g + k2*b;   // exactly the model's ints
 *        ap_uint<8> out = (acc + (1 << 12)) >> 13;   // exactly its rounding
 *    Explicit beats implicit whenever "bit-exact" is in the requirement.
 *    Example 02 (rgb_to_gray) is written this way for precisely this reason.
 *
 * 3. REFERENCE IS A FIXED-POINT LIBRARY (MATLAB fi, Python fxpmath).
 *    Bit-exactness is achievable if you match the rounding and overflow
 *    modes -- but do NOT trust a remembered mapping table between libraries.
 *    Determine it empirically: feed both your reference and ap_fixed the tie
 *    values (x.5 for positive and negative x, and the overflow boundary) and
 *    compare. The testbench below prints ap_fixed's behaviour on exactly
 *    those values so you can diff it against your model in one pass.
 *
 * ---------------------------------------------------------------------------
 * HARDWARE COST OF THE MODES
 *
 *   AP_TRN      free (drop bits)
 *   AP_TRN_ZERO ~1 adder (conditional increment for negatives)
 *   AP_RND*     ~1 adder
 *   AP_RND_CONV ~1 adder + a little logic for the tie case
 *   AP_WRAP     free (drop bits)
 *   AP_SAT      ~1 comparator + mux per bound
 *
 * None of this is expensive at one quantisation point. All of it is expensive
 * when you have accidentally created twenty of them by declaring narrow
 * intermediates.
 *
 * NOTE THE DEFAULTS: ap_fixed<W,I> is ap_fixed<W,I,AP_TRN,AP_WRAP>.
 * Truncate and WRAP. Wrapping means 256 becomes 0 -- a bright pixel turns
 * black. For anything that can overflow, say AP_SAT explicitly.
 * ======================================================================== */
