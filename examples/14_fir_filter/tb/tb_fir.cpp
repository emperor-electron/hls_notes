#include "video_tb_utils.h"
#include "fir.h"
#include <cmath>
#include <vector>

static const int NS = 512;

/* Golden model in double, using the SAME coefficient table as the DUT (it is
 * exported from fir.cpp). Sharing the coefficients is deliberate: we are
 * testing the filter structure and the fixed-point typing, not whether two
 * copies of a number list match. */
static double gold_fir(const std::vector<double> &x, int n)
{
    double acc = 0.0;
    for (int i = 0; i < NTAPS; ++i) {
        int k = n - i;
        if (k >= 0) acc += FIR_COEF_D[i] * x[k];
    }
    return acc;
}

static double q15(double v)   /* what sample_t can represent */
{
    double s = std::floor(v * 32768.0 + 0.5) / 32768.0;
    if (s >  0.99997) s =  0.99997;
    if (s < -1.0)     s = -1.0;
    return s;
}

int main()
{
    printf("=== NTAPS=%d SYMMETRIC=%d ===\n", NTAPS, SYMMETRIC);

    /* Verify the filter really is symmetric before trusting the folded path.
     * The folded form silently computes garbage on an asymmetric filter, so
     * this assertion is not paranoia -- it is the precondition. */
    int asym = 0;
    for (int i = 0; i < NTAPS / 2; ++i)
        if (std::fabs(FIR_COEF_D[i] - FIR_COEF_D[NTAPS - 1 - i]) > 1e-12) ++asym;
    if (asym) { printf("*** coefficients are NOT symmetric (%d)\n", asym); return 1; }
    printf("coefficients verified symmetric\n");

    /* Stimulus: two tones, one in the passband and one well into the stopband,
     * plus an impulse at the start. A filter that is subtly wrong usually
     * still passes a random-noise test; a stopband tone does not lie. */
    std::vector<double> x(NS);
    for (int n = 0; n < NS; ++n) {
        double pass = 0.4 * std::sin(2.0 * M_PI * 0.05 * n);   /* passband  */
        double stop = 0.4 * std::sin(2.0 * M_PI * 0.40 * n);   /* stopband  */
        x[n] = q15(pass + stop);
    }

    s16_stream_t src("src"), dst("dst");
    for (int n = 0; n < NS; ++n) {
        s16_axis_t w;
        short raw = (short)std::floor(x[n] * 32768.0 + 0.5);
        w.data = 0;
        w.data.range(15, 0) = (ap_uint<16>)(unsigned short)raw;
        AXIS_SET_KEEP(w);
        w.user = (n == 0); w.last = (n == NS - 1);
        w.id = 0; w.dest = 0;
        src.write(w);
    }

    fir(src, dst, NS);

    int errs = 0;
    double maxerr = 0.0;
    std::vector<double> y(NS);
    for (int n = 0; n < NS; ++n) {
        if (dst.empty()) { printf("starved at %d\n", n); return 1; }
        s16_axis_t w = dst.read();
        short raw = (short)(unsigned short)(unsigned)w.data.range(15, 0);
        y[n] = raw / 32768.0;

        double ref = gold_fir(x, n);
        if (ref >  0.99997) ref =  0.99997;
        if (ref < -1.0)     ref = -1.0;
        double e = std::fabs(y[n] - ref);
        if (e > maxerr) maxerr = e;

        int want_last = (n == NS - 1);
        if ((int)w.last != want_last) { printf("TLAST wrong at %d\n", n); ++errs; }
    }
    if (!dst.empty()) { printf("extra output beats\n"); ++errs; }

    printf("max |error| vs double reference: %.6f  (%.2f LSB of Q1.15)\n",
           maxerr, maxerr * 32768.0);
    /* Budget: coefficient quantisation to 18 bits plus one output rounding.
     * A few LSB is expected; tens of LSB means a structural bug. */
    if (maxerr * 32768.0 > 8.0) {
        printf("*** error exceeds the 8 LSB budget\n"); ++errs;
    }

    /* Stopband rejection: measure the residual energy at the stopband tone by
     * correlating the settled output against it. This is the check that
     * actually proves it is a filter and not just "close to some numbers". */
    double cs = 0, cc = 0;
    for (int n = NTAPS; n < NS; ++n) {
        cs += y[n] * std::sin(2.0 * M_PI * 0.40 * n);
        cc += y[n] * std::cos(2.0 * M_PI * 0.40 * n);
    }
    double stop_amp = 2.0 * std::sqrt(cs*cs + cc*cc) / (NS - NTAPS);
    double ps = 0, pc = 0;
    for (int n = NTAPS; n < NS; ++n) {
        ps += y[n] * std::sin(2.0 * M_PI * 0.05 * n);
        pc += y[n] * std::cos(2.0 * M_PI * 0.05 * n);
    }
    double pass_amp = 2.0 * std::sqrt(ps*ps + pc*pc) / (NS - NTAPS);

    printf("passband tone amplitude : %.4f (in 0.4)\n", pass_amp);
    printf("stopband tone amplitude : %.4f (in 0.4)\n", stop_amp);
    printf("rejection               : %.1f dB\n", 20.0 * std::log10(pass_amp / stop_amp));
    if (stop_amp > 0.05) { printf("*** stopband tone not rejected\n"); ++errs; }
    if (pass_amp < 0.20) { printf("*** passband tone lost\n"); ++errs; }

    if (errs) { printf("\n*** FAIL: %d errors\n", errs); return 1; }
    printf("\n*** PASS\n");
    return 0;
}
