#include "video_tb_utils.h"
#include "vfilter.h"

static const int ROWS = 24;
static const int COLS = 32;
static const int H    = (NTAPS - 1) / 2;

int main()
{
    printf("=== NTAPS=%d  NLINES=%d  STORAGE=%d  PACKED=%d  MAX_COLS=%d ===\n",
           NTAPS, NLINES, STORAGE, PACKED, MAX_COLS);
    int errs = 0;

    /* Two frames back to back through the same static line buffer. The
     * interior rows must be identical both times -- if frame 2's interior
     * differs, the line-buffer shift is wrong, not just the borders. */
    for (int trial = 0; trial < 2; ++trial) {
        gray_stream_t src("src"), dst("dst");

        Frame in = make_test_frame(ROWS, COLS, 21);
        for (size_t i = 0; i < in.px.size(); ++i) in.px[i] &= 0xFF;

        drive_frame<gray_axis_t>(src, in);
        vfilter(src, dst, ROWS, COLS);

        Frame got(ROWS, COLS);
        char nm[24]; snprintf(nm, sizeof nm, "vf%d", trial);
        errs += capture_frame<gray_axis_t>(dst, got, nm);

        /* Interior rows only -- see the SCOPE NOTE in src/vfilter.cpp. */
        int checked = 0;
        for (int y = H; y < ROWS - H; ++y)
            for (int x = 0; x < COLS; ++x) {
                int sum = 0;
                for (int k = -H; k <= H; ++k)
                    sum += (int)(in.at(y + k, x) & 0xFF);
                unsigned e = (unsigned)(sum / NTAPS);
                if (got.at(y, x) != e) {
                    if (errs < 6)
                        printf("[%s] (%d,%d) got %u exp %u\n",
                               nm, y, x, got.at(y, x), e);
                    ++errs;
                }
                ++checked;
            }
        printf("[%s] checked %d interior pixels (rows %d..%d)\n",
               nm, checked, H, ROWS - H - 1);
    }

    if (errs) { printf("*** FAIL: %d errors\n", errs); return 1; }
    printf("*** PASS\n");
    return 0;
}
