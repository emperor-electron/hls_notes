#include "video_tb_utils.h"
#include "normalize.h"

static const int ROWS = 8;
static const int COLS = 12;

/* Reference for the blackbox itself. Written independently of the C model so
 * a bug in the C model does not validate itself. */
static unsigned long ref_div(unsigned num, unsigned den)
{
    if (den == 0) return 0xFFFFFFFFul;
    return ((unsigned long long)num << FX_FRAC) / den;
}

static int test_divider()
{
    printf("=== blackbox fx_divide ===\n");
    int errs = 0;
    /* Exercise the edges the RTL and the C model must agree on. If these two
     * ever disagree, csim passes and cosim fails -- so test them here, where
     * the failure is cheap and legible. */
    static const unsigned cases[][2] = {
        {255, 1}, {255, 255}, {255, 128}, {255, 3}, {255, 7},
        {1, 255}, {0, 255}, {255, 0}, {0, 0}, {1, 1},
        {65535, 255}, {255, 2}, {100, 7}
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        ap_uint<32> q = 0;
        fx_divide(cases[i][0], cases[i][1], q);
        unsigned long e = ref_div(cases[i][0], cases[i][1]) & 0xFFFFFFFFul;
        if ((unsigned long)q != e) {
            printf("  fx_divide(%u, %u) = %lu, expected %lu\n",
                   cases[i][0], cases[i][1], (unsigned long)q, e);
            ++errs;
        }
    }
    printf("  %s (%zu cases)\n", errs ? "FAIL" : "ok",
           sizeof(cases)/sizeof(cases[0]));
    return errs;
}

static int test_normalize()
{
    printf("=== normalize (frame-to-frame feedback) ===\n");
    int errs = 0;

    /* A frame whose maximum is 128. The FIRST frame passes through unchanged
     * (scale initialises to 1.0); the SECOND is scaled by 255/128. */
    Frame in(ROWS, COLS);
    for (int y = 0; y < ROWS; ++y)
        for (int x = 0; x < COLS; ++x)
            in.at(y, x) = (unsigned)((x * 128) / (COLS - 1));

    {
        gray_stream_t src("src"), dst("dst");
        drive_frame<gray_axis_t>(src, in);
        normalize(src, dst, ROWS, COLS, 255);
        Frame got(ROWS, COLS);
        errs += capture_frame<gray_axis_t>(dst, got, "frame1");
        errs += compare_frames(got, in, 0, "frame1_passthrough");
        printf("  frame 1 passes through unchanged: %s\n", errs ? "FAIL" : "ok");
    }

    {
        gray_stream_t src("src"), dst("dst");
        drive_frame<gray_axis_t>(src, in);
        normalize(src, dst, ROWS, COLS, 255);
        Frame got(ROWS, COLS);
        errs += capture_frame<gray_axis_t>(dst, got, "frame2");

        unsigned long scale = ref_div(255, 128);
        Frame exp(ROWS, COLS);
        for (int y = 0; y < ROWS; ++y)
            for (int x = 0; x < COLS; ++x) {
                unsigned long v = ((unsigned long)in.at(y, x) * scale) >> FX_FRAC;
                exp.at(y, x) = (unsigned)(v > 255 ? 255 : v);
            }
        errs += compare_frames(got, exp, 0, "frame2_scaled");
        printf("  frame 2 scaled by 255/128 = %.4f: %s\n",
               (double)scale / (1 << FX_FRAC), errs ? "FAIL" : "ok");
        printf("  peak now %u (was 128)\n", got.at(0, COLS - 1));
    }
    return errs;
}

int main()
{
    int errs = test_divider() + test_normalize();
    printf("\nNOTE: csim ran the C MODEL of fx_divide, not the SystemVerilog.\n");
    printf("      Only cosim exercises rtl/fx_divide.sv. A C-model/RTL\n");
    printf("      mismatch is invisible until then:\n");
    printf("        make cosim EX=12_rtl_blackbox\n");
    if (errs) { printf("\n*** FAIL: %d errors\n", errs); return 1; }
    printf("\n*** PASS\n");
    return 0;
}
