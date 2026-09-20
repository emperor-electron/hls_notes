#include "video_tb_utils.h"
#include "unsharp.h"

/* Frame must be small enough for cosim to finish in minutes, but COLS must be
 * large enough that the skew is visible: with COLS=64 the required FIFO depth
 * is 65, so a sweep down to depth=2/32 shows a clean pass/hang boundary. */
static const int ROWS = 24;
static const int COLS = 64;
static const int AMT  = 128;

static unsigned char gold(const Frame &in, int y, int x, int amount)
{
    int sum = 0;
    for (int r = -1; r <= 1; ++r)
        for (int c = -1; c <= 1; ++c) {
            int yy = y + r, xx = x + c;
            if (yy < 0) yy = 0; if (yy >= in.rows) yy = in.rows - 1;
            if (xx < 0) xx = 0; if (xx >= in.cols) xx = in.cols - 1;
            sum += (int)(in.at(yy, xx) & 0xFF);
        }
    int blur = (int)((sum * 7282 + 32768) >> 16);
    int s    = (int)(in.at(y, x) & 0xFF);
    int adj  = s + (((s - blur) * amount) >> 8);
    if (adj < 0) adj = 0;
    if (adj > 255) adj = 255;
    return (unsigned char)adj;
}

int main()
{
    gray_stream_t src("src"), dst("dst");

    Frame in = make_square_frame(ROWS, COLS);
    for (size_t i = 0; i < in.px.size(); ++i) in.px[i] &= 0xFF;

    drive_frame<gray_axis_t>(src, in);
    unsharp_mask(src, dst, ROWS, COLS, AMT);

    Frame got(ROWS, COLS);
    int errs = capture_frame<gray_axis_t>(dst, got, "unsharp");

    Frame exp(ROWS, COLS);
    for (int y = 0; y < ROWS; ++y)
        for (int x = 0; x < COLS; ++x)
            exp.at(y, x) = gold(in, y, x, AMT);

    errs += compare_frames(got, exp, 0, "unsharp");

    printf("\n");
    printf("-------------------------------------------------------------\n");
    printf(" This csim result is NOT evidence that the design works.\n");
    printf("\n");
    printf(" csim runs the dataflow stages sequentially and treats every\n");
    printf(" hls::stream as an unbounded queue. The `sharp` FIFO grew to\n");
    printf(" %d entries here; in hardware it is SKEW_DEPTH=%d deep.\n",
           ROWS * COLS, (int)(SKEW_DEPTH));
    printf("\n");
    printf(" Required depth is cols+2 = %d at this resolution, and\n", COLS + 2);
    printf(" MAX_COLS+2 = %d at the maximum this build supports.\n", MAX_COLS + 2);
    printf("\n");
    printf(" Prove it with cosim, and find the cliff with:\n");
    printf("   ./scripts/sweep.py --example 05_split_join_skew \\\n");
    printf("       --define SKEW_DEPTH=2,16,32,64,66,128 --stage cosim\n");
    printf("-------------------------------------------------------------\n");

    if (errs) { printf("*** FAIL: %d errors\n", errs); return 1; }
    printf("*** PASS (arithmetic only)\n");
    return 0;
}
