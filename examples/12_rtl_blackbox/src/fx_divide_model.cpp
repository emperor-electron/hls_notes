#include "normalize.h"

/* ===========================================================================
 * C MODEL OF THE BLACKBOX.
 *
 * This function is what csim executes. The RTL replaces it at csynth. They
 * MUST agree bit for bit, including on the edge cases nobody tests -- if they
 * disagree, csim passes and cosim fails, and the difference is invisible in
 * the C source.
 *
 * `#pragma HLS inline off` is required: the blackbox substitution happens at
 * the function boundary, so the function has to still exist.
 * ======================================================================== */
void fx_divide(ap_uint<32> num, ap_uint<32> den, ap_uint<32> &quot)
{
#pragma HLS inline off

    if (den == 0) {
        /* Must match the RTL's S_RUN divide-by-zero branch exactly. */
        quot = (ap_uint<32>)0xFFFFFFFFu;
        return;
    }

    /* (num << FRAC) / den, computed in 64 bits so the pre-scale cannot
     * overflow -- exactly what the RTL's 2W-wide remainder does. */
    ap_uint<64> n = ((ap_uint<64>)num) << FX_FRAC;
    ap_uint<64> q = n / (ap_uint<64>)den;

    /* The RTL produces W quotient bits, so anything above 2^32-1 is whatever
     * the shift register happened to contain. Rather than leave that
     * undefined, the RTL's shift naturally truncates -- mirror it. */
    quot = (ap_uint<32>)(q & 0xFFFFFFFFull);
}
