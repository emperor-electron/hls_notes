#include "video_tb_utils.h"
#include "feature_scan.h"
#include <vector>

static const int ROWS = 16;
static const int COLS = 32;

static int run_case(const Frame &in, int thr, const char *name,
                    bool expect_hang_on_variant0)
{
    gray_stream_t  src("src");
    coord_stream_t dst("dst");

    drive_frame<gray_axis_t>(src, in);

    /* Golden: list of coordinates at or above threshold, raster order. */
    std::vector<unsigned> exp;
    for (int y = 0; y < in.rows; ++y)
        for (int x = 0; x < in.cols; ++x)
            if ((int)(in.at(y, x) & 0xFF) >= thr)
                exp.push_back(((unsigned)y << 16) | (unsigned)x);

    printf("[%s] threshold=%d, expecting %u features (of %d pixels)\n",
           name, thr, (unsigned)exp.size(), in.rows * in.cols);

#if VARIANT == 0
    if (expect_hang_on_variant0 && exp.size() < (size_t)(in.rows * in.cols)) {
        printf("[%s] VARIANT=0 is the BROKEN build. stage_emit will read\n", name);
        printf("[%s]   %d tokens but stage_scan only wrote %u.\n",
               name, in.rows * in.cols, (unsigned)exp.size());
        printf("[%s] Expect: ERROR [HLS SIM]: an hls::stream is read while\n", name);
        printf("[%s]   empty ... then abort(). It takes ~1s to fire -- it is a\n", name);
        printf("[%s]   detector thread, not an inline check.\n", name);
        printf("[%s] That failure IS the lesson. Build VARIANT=1 or 2 to pass.\n",
               name);
    }
#else
    (void)expect_hang_on_variant0;
#endif

    ap_uint<32> n_found = 0;
    feature_scan(src, dst, in.rows, in.cols, thr, &n_found);

    int errs = 0;
    if ((unsigned)n_found != exp.size()) {
        printf("[%s] count mismatch: got %u expected %u\n",
               name, (unsigned)n_found, (unsigned)exp.size());
        ++errs;
    }

    for (size_t i = 0; i < exp.size(); ++i) {
        if (dst.empty()) { printf("[%s] starved at feature %u\n", name, (unsigned)i); return errs + 1; }
        coord_axis_t w = dst.read();
        if ((unsigned)w.data != exp[i]) {
            if (errs < 8)
                printf("[%s] feature %u: got (%u,%u) expected (%u,%u)\n", name,
                       (unsigned)i, (unsigned)w.data >> 16, (unsigned)w.data & 0xFFFF,
                       exp[i] >> 16, exp[i] & 0xFFFF);
            ++errs;
        }
        int want_last = (i == exp.size() - 1) ? 1 : 0;
        if ((int)w.last != want_last) {
            printf("[%s] TLAST wrong at feature %u: got %d want %d\n",
                   name, (unsigned)i, (int)w.last, want_last);
            ++errs;
        }
        int want_user = (i == 0) ? 1 : 0;
        if ((int)w.user != want_user) {
            printf("[%s] TUSER wrong at feature %u\n", name, (unsigned)i);
            ++errs;
        }
    }
    if (!dst.empty()) { printf("[%s] extra output beats\n", name); ++errs; }
    return errs;
}

int main()
{
    printf("=== VARIANT = %d ===\n", VARIANT);
    int errs = 0;

    Frame f = make_square_frame(ROWS, COLS);
    for (size_t i = 0; i < f.px.size(); ++i) f.px[i] &= 0xFF;

    /* Sparse: only the bright square qualifies. This is where VARIANT=0 dies. */
    errs += run_case(f, 0x80, "sparse", true);

    /* Everything qualifies: token counts coincidentally match, so even the
     * BROKEN variant passes. This is exactly how a count bug survives code
     * review and bring-up and then fails in the field on the first dark
     * frame -- your test vector was dense. Always test the sparse case, and
     * always test the EMPTY case below. */
    errs += run_case(f, 0x00, "dense", false);

#if VARIANT != 0
    /* Zero features. The degenerate case that breaks naive sentinel
     * implementations (nothing to flush, TLAST has nowhere to go) and that
     * every real pipeline eventually sees -- a lens cap, a black frame
     * between scenes, a sensor that has not started. */
    Frame black(ROWS, COLS);
    errs += run_case(black, 0x01, "empty", false);
#endif

    if (errs) { printf("*** FAIL: %d errors\n", errs); return 1; }
    printf("*** PASS\n");
    return 0;
}
