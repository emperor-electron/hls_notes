#include "video_tb_utils.h"
#include "sobel.h"

static const int ROWS = 20;
static const int COLS = 28;
static const int THR  = 0;

/* Golden model in plain C with explicit clamp-to-edge indexing. Deliberately
 * written the "obvious slow way" -- random access into a 2D array -- because
 * the whole point is to be structurally different from the line-buffer DUT.
 * If the reference also used a line buffer, a shift-direction bug would be
 * present in both and the test would pass. */
static unsigned char gold_sobel(const Frame &f, int y, int x, int thr)
{
    int gx = 0, gy = 0;
    static const int KX[3][3] = {{-1,0,1},{-2,0,2},{-1,0,1}};
    static const int KY[3][3] = {{-1,-2,-1},{0,0,0},{1,2,1}};
    for (int r = -1; r <= 1; ++r) {
        for (int c = -1; c <= 1; ++c) {
            int yy = y + r, xx = x + c;
            if (yy < 0) yy = 0;
            if (yy >= f.rows) yy = f.rows - 1;
            if (xx < 0) xx = 0;
            if (xx >= f.cols) xx = f.cols - 1;
            int v = (int)(f.at(yy, xx) & 0xFF);
            gx += KX[r + 1][c + 1] * v;
            gy += KY[r + 1][c + 1] * v;
        }
    }
    int mag = abs(gx) + abs(gy);
    if (mag > 255) mag = 255;
    return (unsigned char)((mag >= thr) ? mag : 0);
}

static int run_case(int rows, int cols, int thr, const char *name)
{
    gray_stream_t src("src"), dst("dst");

    /* Grayscale frame: only the low byte carries data for an 8-bit stream. */
    Frame in = make_square_frame(rows, cols);
    for (size_t i = 0; i < in.px.size(); ++i) in.px[i] &= 0xFF;

    drive_frame<gray_axis_t>(src, in);
    sobel_3x3(src, dst, rows, cols, thr);

    Frame got(rows, cols);
    int errs = capture_frame<gray_axis_t>(dst, got, name);

    Frame exp(rows, cols);
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x)
            exp.at(y, x) = gold_sobel(in, y, x, thr);

    errs += compare_frames(got, exp, 0, name);
    return errs;
}

int main()
{
    int errs = 0;

    /* Nominal case. */
    errs += run_case(ROWS, COLS, THR, "sobel");

    /* Thresholded. */
    errs += run_case(ROWS, COLS, 64, "sobel_thr64");

    /* Back-to-back frames through the SAME static line buffer.
     * This is the test that catches the most real bugs. A hand-rolled line
     * buffer that works on frame 1 and corrupts the first two rows of frame 2
     * is extremely common: the static linebuf still holds the previous
     * frame's bottom rows, and if your border logic reads them instead of
     * replicating, frame 2's top edge is wrong. Run the same case twice and
     * require both to pass. */
    errs += run_case(ROWS, COLS, THR, "sobel_frame2");

    /* Resolution change at runtime. Exercises the non-static ox/oy reset and
     * the line buffer being re-addressed with a different stride. */
    errs += run_case(12, 40, THR, "sobel_wide");
    errs += run_case(40, 12, THR, "sobel_tall");

    /* Degenerate small frame: rows and cols both smaller than the window.
     * Every index gets clamped to the single valid row/column, so the answer
     * is all-zero gradient. Kernels that assume rows>=3 hang or emit garbage
     * here, and a 2-pixel-tall frame is exactly what a misconfigured VDMA
     * produces during bring-up. */
    errs += run_case(2, 3, THR, "sobel_tiny");

    if (errs) { printf("*** FAIL: %d errors\n", errs); return 1; }
    printf("*** PASS\n");
    return 0;
}
