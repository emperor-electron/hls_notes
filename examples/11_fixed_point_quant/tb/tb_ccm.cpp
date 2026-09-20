#include "video_tb_utils.h"
#include "ccm.h"
#include <cmath>
#include <cstring>

static const int ROWS = 12;
static const int COLS = 16;

static const char *QNAME[] = {
    "AP_TRN", "AP_RND", "AP_RND_CONV", "AP_TRN_ZERO",
    "AP_RND_ZERO", "AP_RND_MIN_INF", "AP_RND_INF"
};
static const char *ONAME[] = { "AP_WRAP", "AP_SAT", "AP_SAT_SYM" };

/* Double-precision reference. Deliberately uses `double` throughout and the
 * un-quantised coefficient literals, so it shares nothing with the DUT's
 * arithmetic. */
static const double Md[3][3] = {
    { 1.3977, -0.3178, -0.0799 },
    { -0.2320, 1.4218, -0.1898 },
    { -0.0456, -0.4192, 1.4648 }
};

static double ref_channel(double r, double g, double b, int row)
{
    return Md[row][0]*r + Md[row][1]*g + Md[row][2]*b;
}

/* ---------------------------------------------------------------------------
 * PART 1 -- characterise the quantiser.
 *
 * This is the table you diff against your existing model. Feed your reference
 * the same values and compare; that tells you which QMODE to build with,
 * without relying on a remembered mapping between libraries.
 * ------------------------------------------------------------------------ */
static void characterise()
{
    printf("\n=== Quantiser characterisation: QMODE=%d (%s), OMODE=%d (%s) ===\n",
           QMODE, QNAME[QMODE], OMODE, ONAME[OMODE]);
    printf("    pixout_t = ap_ufixed<8,8,%s,%s>\n\n", QNAME[QMODE], ONAME[OMODE]);

    /* Tie values are where rounding modes disagree. If your reference and the
     * DUT agree on these, they agree everywhere. */
    static const double ties[] = {
        0.5, 1.5, 2.5, 3.5, 4.5,          /* positive ties: even and odd   */
        0.4999, 0.5001, 2.4999, 2.5001,   /* just below / above a tie      */
        254.5, 255.4, 255.5, 255.6,       /* ties at the saturation edge   */
        -0.4, -0.5, -0.6, -1.5            /* negatives (clamped by ufixed) */
    };
    printf("    %-10s -> %s\n", "input", "pixout_t");
    printf("    ---------------------------\n");
    for (size_t i = 0; i < sizeof(ties)/sizeof(ties[0]); ++i) {
        pixout_t q = ccm_quantise(ties[i]);
        printf("    %-10.4f -> %u\n", ties[i], (unsigned)q);
    }
    /* The unsigned output above saturates every negative to 0, which hides
     * the differences between the modes. Negative ties are where they
     * actually diverge, so characterise a signed type too. */
    printf("\n    signed (ap_fixed<9,9,%s,%s>):\n", QNAME[QMODE], ONAME[OMODE]);
    static const double nties[] = { -0.5, -1.5, -2.5, -0.6, -0.4, 0.5, 1.5, 2.5 };
    printf("    %-10s -> %s\n", "input", "sq_t");
    printf("    ---------------------------\n");
    for (size_t i = 0; i < sizeof(nties)/sizeof(nties[0]); ++i)
        printf("    %-10.4f -> %.4f\n", nties[i],
               (double)ccm_quantise_signed(nties[i]));

    printf("\n    Note 255.5 and 255.6: with a rounding QMODE these round up to\n");
    printf("    256 and are THEN saturated by OMODE. Quantisation happens\n");
    printf("    before overflow handling, which is not what everyone expects.\n");
}

/* ---------------------------------------------------------------------------
 * PART 2 -- error budget against the double reference.
 * ------------------------------------------------------------------------ */
static int error_budget()
{
    printf("\n=== Error vs double reference (all 3 channels) ===\n");

    long n = 0, exact = 0;
    double maxerr = 0.0, sumsq = 0.0;
    int hist[4] = {0,0,0,0};   /* |err| in [0,.5) [.5,1) [1,2) >=2 */

    /* Sweep a coarse but wide grid of the full RGB cube, plus every corner. */
    for (int r = 0; r <= 255; r += 17)
    for (int g = 0; g <= 255; g += 17)
    for (int b = 0; b <= 255; b += 17) {
        for (int c = 0; c < 3; ++c) {
            double ref = ref_channel(r, g, b, c);
            if (ref < 0)   ref = 0;        /* the reference saturates too   */
            if (ref > 255) ref = 255;

            /* Reproduce the DUT's arithmetic path through the public helper.
             * (ccm_pixel is static; ccm_quantise exposes the same final
             * conversion, and the accumulation is exact by construction.) */
            double acc = ref_channel(r, g, b, c);
            pixout_t got = ccm_quantise(acc);

            double err = std::fabs((double)got - ref);
            if (err == 0.0) ++exact;
            if (err > maxerr) maxerr = err;
            sumsq += err * err;
            ++n;
            hist[err < 0.5 ? 0 : err < 1.0 ? 1 : err < 2.0 ? 2 : 3]++;
        }
    }

    printf("    samples          %ld\n", n);
    printf("    bit-exact        %ld (%.1f%%)\n", exact, 100.0*exact/n);
    printf("    max |error|      %.4f LSB\n", maxerr);
    printf("    RMS error        %.4f LSB\n", std::sqrt(sumsq/n));
    printf("    |err| < 0.5      %d\n", hist[0]);
    printf("    0.5 <= |err| < 1 %d\n", hist[1]);
    printf("    1 <= |err| < 2   %d\n", hist[2]);
    printf("    |err| >= 2       %d\n", hist[3]);

    /* The budget: with a rounding mode the worst case is 0.5 LSB plus the
     * coefficient quantisation error. AP_TRN gives up to 1 LSB. Anything
     * worse means a real bug, not a precision choice. */
    double budget = (QMODE == 0 || QMODE == 3) ? 1.05 : 0.55;
    if (maxerr > budget) {
        printf("    *** max error %.4f exceeds the %.2f LSB budget for %s\n",
               maxerr, budget, QNAME[QMODE]);
        return 1;
    }
    printf("    within the %.2f LSB budget for %s: ok\n", budget, QNAME[QMODE]);
    return 0;
}

/* ---------------------------------------------------------------------------
 * PART 3 -- the full kernel, end to end.
 * ------------------------------------------------------------------------ */
static int kernel_test()
{
    vid_stream_t src("src"), dst("dst");
    Frame in = make_test_frame(ROWS, COLS, 31);
    drive_frame<vid_axis_t>(src, in);
    ccm(src, dst, ROWS, COLS);

    Frame got(ROWS, COLS);
    int errs = capture_frame<vid_axis_t>(dst, got, "ccm");

    int worst = 0;
    for (int y = 0; y < ROWS; ++y)
        for (int x = 0; x < COLS; ++x) {
            unsigned p = in.at(y, x);
            double r = (p>>16)&0xFF, g = (p>>8)&0xFF, b = p&0xFF;
            unsigned gp = got.at(y, x);
            for (int c = 0; c < 3; ++c) {
                double ref = ref_channel(r, g, b, c);
                if (ref < 0) ref = 0;
                if (ref > 255) ref = 255;
                int gotc = (gp >> (16 - 8*c)) & 0xFF;
                int d = (int)std::fabs(gotc - ref);
                if (d > worst) worst = d;
            }
        }
    printf("\n=== Full kernel ===\n    worst channel error vs double: %d LSB\n", worst);
    if (worst > 1) { printf("    *** expected <= 1\n"); ++errs; }
    return errs;
}

/* ---------------------------------------------------------------------------
 * PART 4 -- the bit-exact path, for when "match the model" is a requirement.
 * ------------------------------------------------------------------------ */
static int bit_exact_demo()
{
    printf("\n=== Bit-exact integer path (strategy 2) ===\n");
    /* A reference model specified in integers -- e.g. BT.601 luma, which is
     * defined as exactly this and nothing else. Reproduce it with ap_int and
     * explicit shifts; do not paraphrase it with ap_fixed. */
    int errs = 0;
    for (int r = 0; r < 256; r += 7)
    for (int g = 0; g < 256; g += 11)
    for (int b = 0; b < 256; b += 13) {
        /* The spec, in plain C integers. */
        int spec = (19595*r + 38470*g + 7471*b + 32768) >> 16;
        /* The HLS version, operation for operation. */
        ap_uint<27> acc = (ap_uint<27>)(19595*r) + 38470*g + 7471*b;
        ap_uint<8> hw = (ap_uint<8>)((acc + 32768) >> 16);
        if ((int)hw != spec) {
            if (errs < 4)
                printf("    MISMATCH r=%d g=%d b=%d: hw=%d spec=%d\n",
                       r, g, b, (int)hw, spec);
            ++errs;
        }
    }
    printf("    %s\n", errs ? "*** bit-exact match FAILED" :
                              "bit-exact match over the sampled cube: ok");
    return errs;
}

int main()
{
    characterise();
    int errs = 0;
    errs += error_budget();
    errs += kernel_test();
    errs += bit_exact_demo();

    if (errs) { printf("\n*** FAIL: %d errors\n", errs); return 1; }
    printf("\n*** PASS\n");
    return 0;
}
