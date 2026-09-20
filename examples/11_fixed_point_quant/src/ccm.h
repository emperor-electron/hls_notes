#ifndef CCM_H
#define CCM_H

#include "hls_compat.h"
#include <ap_fixed.h>

#ifndef MAX_COLS
#define MAX_COLS 1920
#endif
#ifndef MAX_ROWS
#define MAX_ROWS 1080
#endif

/* ---------------------------------------------------------------------------
 * QUANTISATION AND OVERFLOW MODES -- the whole point of this example.
 *
 * ap_fixed<W, I, Q, O, N>
 *          │  │  │  │  └─ N: number of saturation bits (almost always 0)
 *          │  │  │  └──── O: overflow mode   AP_WRAP | AP_SAT | AP_SAT_SYM | ...
 *          │  │  └─────── Q: quantisation    AP_TRN | AP_RND | AP_RND_CONV | ...
 *          │  └────────── I: integer bits INCLUDING the sign bit
 *          └───────────── W: total bits
 *
 * Fractional bits = W - I. A negative I is legal and means "all fractional,
 * with leading zeros" -- ap_fixed<8,-2> represents 0 .. 2^-2 in steps of 2^-10.
 *
 * Sweep these:
 *   ./scripts/sweep.py --example 11_fixed_point_quant \
 *       --define QMODE=0,1,2,3,4,5,6 --stage csim
 * ------------------------------------------------------------------------ */
#ifndef QMODE
#define QMODE 2          /* default: AP_RND_CONV, see table below */
#endif
#ifndef OMODE
#define OMODE 1          /* default: AP_SAT */
#endif

#if   QMODE == 0
  #define CCM_Q AP_TRN           /* truncate toward -inf (the DEFAULT)      */
#elif QMODE == 1
  #define CCM_Q AP_RND           /* round to nearest, ties toward +inf      */
#elif QMODE == 2
  #define CCM_Q AP_RND_CONV      /* round to nearest, ties to EVEN (banker) */
#elif QMODE == 3
  #define CCM_Q AP_TRN_ZERO      /* truncate toward zero                    */
#elif QMODE == 4
  #define CCM_Q AP_RND_ZERO      /* round to nearest, ties toward zero      */
#elif QMODE == 5
  #define CCM_Q AP_RND_MIN_INF   /* round to nearest, ties toward -inf      */
#elif QMODE == 6
  #define CCM_Q AP_RND_INF       /* round to nearest, ties away from zero   */
#endif

#if   OMODE == 0
  #define CCM_O AP_WRAP          /* wrap around (the DEFAULT) -- dangerous  */
#elif OMODE == 1
  #define CCM_O AP_SAT           /* saturate to min/max                     */
#elif OMODE == 2
  #define CCM_O AP_SAT_SYM       /* saturate symmetrically (min = -max)     */
#endif

/* Coefficient type. A 3x3 colour-correction matrix has entries in roughly
 * [-1, +2], so 3 integer bits (sign + 2) is enough; 13 fractional bits gives
 * a coefficient step of 1/8192, far finer than 8-bit output can resolve.
 *
 * Coefficients are CONSTANTS, so their quantisation happens once at compile
 * time -- the Q and O modes on this type affect only how the literal doubles
 * below are converted, never the hardware. */
typedef ap_fixed<16, 3, AP_RND_CONV, AP_SAT> coef_t;

/* Pixel INPUT type: 8 integer bits, no fraction. This exists solely so the
 * 0..255 pixel can enter the arithmetic without being squeezed through a
 * narrow type. Casting a pixel to coef_t instead -- which has 3 integer bits
 * and therefore saturates at ~4.0 -- silently destroys every value above 3
 * and is a mistake that is very easy to make and very hard to see, because
 * the result still looks like a plausible image, just wrong. */
typedef ap_ufixed<8, 8> pixin_t;

/* Accumulator. Three products of (8-bit pixel) x (16-bit coefficient), summed.
 * Worst case |sum| < 3 * 255 * 2 = 1530, so 12 integer bits (incl. sign) is
 * ample. Keeping 13 fractional bits means NO rounding happens inside the
 * accumulation -- all the quantisation is deferred to the single final
 * conversion, which is what you want and is explained in ccm.cpp. */
typedef ap_fixed<25, 12, AP_TRN, AP_WRAP> acc_t;

/* Output type: an 8-bit unsigned pixel, expressed as fixed point so the
 * conversion from acc_t applies CCM_Q and CCM_O. ap_ufixed<8,8> is just an
 * 8-bit unsigned integer with the rounding/saturation machinery attached. */
typedef ap_ufixed<8, 8, CCM_Q, CCM_O> pixout_t;

/* A SIGNED quantiser with the same modes. Exposed purely so the testbench can
 * characterise negative-side rounding -- which is where the seven modes
 * actually differ, and which an unsigned output type hides completely by
 * saturating everything negative to 0. */
typedef ap_fixed<9, 9, CCM_Q, CCM_O> sq_t;   /* 0 fractional bits: ties land at x.5 */
sq_t ccm_quantise_signed(double v);

void ccm(vid_stream_t &src, vid_stream_t &dst,
         ap_uint<16> rows, ap_uint<16> cols);

/* Exposed for the testbench so it can characterise the modes directly. */
pixout_t ccm_quantise(double v);

#endif
