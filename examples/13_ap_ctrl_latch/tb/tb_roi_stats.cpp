#include "video_tb_utils.h"
#include "roi_stats.h"

static const int ROWS = 16;
static const int COLS = 24;

static int run_case(int rx, int ry, int rw, int rh, const char *name)
{
    gray_stream_t src("src");
    Frame in = make_test_frame(ROWS, COLS, 41);
    for (size_t i = 0; i < in.px.size(); ++i) in.px[i] &= 0xFF;
    drive_frame<gray_axis_t>(src, in);

    roi_result_t out = {0, 0, 0, 0};
    roi_stats(src, ROWS, COLS, rx, ry, rw, rh, &out);

    /* Golden model */
    unsigned long sum = 0, cnt = 0;
    unsigned mn = 255, mx = 0;
    for (int y = 0; y < ROWS; ++y)
        for (int x = 0; x < COLS; ++x)
            if (x >= rx && x < rx + rw && y >= ry && y < ry + rh) {
                unsigned p = in.at(y, x) & 0xFF;
                sum += p; ++cnt;
                if (p < mn) mn = p;
                if (p > mx) mx = p;
            }
    if (cnt == 0) mn = 0;

    int errs = 0;
    if ((unsigned long)out.sum != sum)   { printf("[%s] sum %lu != %lu\n", name, (unsigned long)out.sum, sum); ++errs; }
    if ((unsigned long)out.count != cnt) { printf("[%s] count %lu != %lu\n", name, (unsigned long)out.count, cnt); ++errs; }
    if ((unsigned)out.min_val != mn)     { printf("[%s] min %u != %u\n", name, (unsigned)out.min_val, mn); ++errs; }
    if ((unsigned)out.max_val != mx)     { printf("[%s] max %u != %u\n", name, (unsigned)out.max_val, mx); ++errs; }
    printf("[%-10s] roi=(%d,%d,%dx%d) sum=%lu count=%lu min=%u max=%u  %s\n",
           name, rx, ry, rw, rh, sum, cnt, mn, mx, errs ? "FAIL" : "ok");
    return errs;
}

int main()
{
    int errs = 0;
    errs += run_case(4, 3, 8, 6, "interior");
    errs += run_case(0, 0, COLS, ROWS, "fullframe");
    errs += run_case(0, 0, 1, 1, "single");
    /* Empty ROI: count==0. min must not report the sentinel 255. */
    errs += run_case(5, 5, 0, 0, "empty");
    /* ROI running off the right/bottom edge -- the kernel must clip, not
     * wrap or read past the frame. */
    errs += run_case(COLS - 3, ROWS - 2, 10, 10, "clipped");

    printf("\nNOTE: csim proves the arithmetic. It does NOT prove the\n");
    printf("      ap_stable contract, because in C there is no such thing as\n");
    printf("      a value changing mid-call. That is what the SystemVerilog\n");
    printf("      testbench in tb/tb_cfg_latch.sv exists for:\n");
    printf("        make -C ../.. sim-sv EX=13_ap_ctrl_latch\n");

    if (errs) { printf("\n*** FAIL: %d errors\n", errs); return 1; }
    printf("\n*** PASS\n");
    return 0;
}
