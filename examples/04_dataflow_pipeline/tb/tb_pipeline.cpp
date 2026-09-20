#include "video_tb_utils.h"
#include "pipeline.h"

static const int ROWS = 16;
static const int COLS = 24;
static const int THR  = 100;

static unsigned char gold(const Frame &in, int y, int x, int thr)
{
    /* luma -> 3x3 box blur (clamp borders) -> threshold */
    int sum = 0;
    for (int r = -1; r <= 1; ++r)
        for (int c = -1; c <= 1; ++c) {
            int yy = y + r, xx = x + c;
            if (yy < 0) yy = 0; if (yy >= in.rows) yy = in.rows - 1;
            if (xx < 0) xx = 0; if (xx >= in.cols) xx = in.cols - 1;
            unsigned p = in.at(yy, xx);
            double l = 0.299 * ((p >> 16) & 0xFF)
                     + 0.587 * ((p >> 8) & 0xFF)
                     + 0.114 * (p & 0xFF);
            int li = (int)(l + 0.5);
            if (li > 255) li = 255;
            sum += li;
        }
    int blur = (int)((sum / 9.0) + 0.5);
    if (blur > 255) blur = 255;
    return (unsigned char)((blur >= thr) ? 255 : 0);
}

int main()
{
    int errs = 0;

    for (int trial = 0; trial < 2; ++trial) {
        vid_stream_t  src("src");
        gray_stream_t dst("dst");

        Frame in = make_square_frame(ROWS, COLS);
        drive_frame<vid_axis_t>(src, in);

        video_pipeline(src, dst, ROWS, COLS, THR);

        Frame got(ROWS, COLS);
        char nm[32]; snprintf(nm, sizeof nm, "pipe%d", trial);
        errs += capture_frame<gray_axis_t>(dst, got, nm);

        Frame exp(ROWS, COLS);
        for (int y = 0; y < ROWS; ++y)
            for (int x = 0; x < COLS; ++x)
                exp.at(y, x) = gold(in, y, x, THR);

        /* tol=0 on a binary output: a thresholded pixel is right or wrong.
         * If the luma and blur rounding differ from the golden model by 1 LSB
         * at a pixel sitting exactly on the threshold, this WILL flag it. That
         * is intentional -- pick a threshold (100) that the test frame does not
         * land on, rather than loosening the tolerance and losing the check. */
        errs += compare_frames(got, exp, 0, nm);
    }

    /* Important DATAFLOW-specific check: in csim the stages execute
     * SEQUENTIALLY (csim is plain C++; there is no concurrency). So csim
     * passing tells you the arithmetic is right and tells you NOTHING about
     * whether the dataflow region deadlocks. Only cosim does that:
     *
     *     make cosim EX=04_dataflow_pipeline
     *
     * A csim-green / cosim-hanging design is the normal failure mode here, not
     * an exotic one. */

    if (errs) { printf("*** FAIL: %d errors\n", errs); return 1; }
    printf("*** PASS (csim only proves arithmetic -- run cosim for deadlock)\n");
    return 0;
}
