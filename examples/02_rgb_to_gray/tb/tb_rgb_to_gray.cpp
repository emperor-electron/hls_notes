#include "video_tb_utils.h"
#include "rgb_to_gray.h"

static const int ROWS = 12;
static const int COLS = 20;

/* Golden model. Written in double on purpose: the point of the test is to
 * prove the Q16 fixed-point kernel matches the real-valued definition to
 * within 1 LSB, so the reference must NOT share the DUT's arithmetic. A
 * golden model that duplicates the DUT's rounding tests nothing. */
static unsigned gold_luma(unsigned px)
{
    double r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
    double y = 0.299 * r + 0.587 * g + 0.114 * b;
    int yi = (int)(y + 0.5);
    if (yi < 0) yi = 0;
    if (yi > 255) yi = 255;
    return (unsigned)yi;
}

int main()
{
    vid_stream_t  src("src");
    gray_stream_t dst("dst");

    Frame in = make_test_frame(ROWS, COLS, 3);
    drive_frame<vid_axis_t>(src, in);

    rgb_to_gray(src, dst, ROWS, COLS);

    Frame got(ROWS, COLS);
    int errs = capture_frame<gray_axis_t>(dst, got, "gray");

    Frame exp(ROWS, COLS);
    for (int y = 0; y < ROWS; ++y)
        for (int x = 0; x < COLS; ++x)
            exp.at(y, x) = gold_luma(in.at(y, x));

    /* tol=1: Q16 rounding may differ from double rounding by one LSB at exact
     * .5 boundaries. Tolerating 1 here is correct; tolerating 2 would be
     * hiding a coefficient bug. */
    errs += compare_frames(got, exp, 1, "gray");

    /* Endpoint checks that a random frame will not hit. Black must be 0 and
     * white must be exactly 255 -- if the coefficients do not sum to 65536,
     * white comes out 254 and every image is imperceptibly dark forever. */
    {
        vid_stream_t  s2("s2");
        gray_stream_t d2("d2");
        Frame extremes(1, 2);
        extremes.at(0, 0) = 0x000000;
        extremes.at(0, 1) = 0xFFFFFF;
        drive_frame<vid_axis_t>(s2, extremes);
        rgb_to_gray(s2, d2, 1, 2);
        Frame g2(1, 2);
        errs += capture_frame<gray_axis_t>(d2, g2, "extremes");
        if (g2.at(0, 0) != 0)   { printf("black -> %u, expected 0\n",   g2.at(0,0)); ++errs; }
        if (g2.at(0, 1) != 255) { printf("white -> %u, expected 255\n", g2.at(0,1)); ++errs; }
    }

    if (errs) { printf("*** FAIL: %d errors\n", errs); return 1; }
    printf("*** PASS\n");
    return 0;
}
