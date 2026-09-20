#include "video_tb_utils.h"
#include "axis_passthrough.h"

/* Small frame: csim cost is O(rows*cols) and cosim is ~1000x slower, so keep
 * regression frames tiny. Verify at 1920x1080 once, manually, not in CI. */
static const int ROWS = 16;
static const int COLS = 24;

static int test_passthrough()
{
    vid_stream_t src("src"), dst("dst");
    /* Naming streams is not cosmetic: HLS uses the name in deadlock and
     * FIFO-overflow messages during cosim. An unnamed stream shows up as
     * something like "hls::stream<...>.0" and you will not know which one. */

    Frame in = make_test_frame(ROWS, COLS);
    drive_frame<vid_axis_t>(src, in);

    axis_passthrough(src, dst);

    Frame got(ROWS, COLS);
    int errs = capture_frame<vid_axis_t>(dst, got, "passthrough");

    /* Golden model: identity. axis_passthrough copies the whole AXI word and
     * changes nothing -- inversion belongs to axis_passthrough_ctrl, below. */
    errs += compare_frames(got, in, 0, "passthrough");
    return errs;
}

static int test_ctrl(int bypass)
{
    vid_stream_t src("src"), dst("dst");
    Frame in = make_test_frame(ROWS, COLS, 7);
    drive_frame<vid_axis_t>(src, in);

    axis_passthrough_ctrl(src, dst, bypass);

    Frame got(ROWS, COLS);
    int errs = capture_frame<vid_axis_t>(dst, got, bypass ? "bypass" : "invert");

    Frame exp(ROWS, COLS);
    for (int y = 0; y < ROWS; ++y)
        for (int x = 0; x < COLS; ++x)
            exp.at(y, x) = bypass ? in.at(y, x) : ((~in.at(y, x)) & 0xFFFFFFu);

    errs += compare_frames(got, exp, 0, bypass ? "bypass" : "invert");
    return errs;
}

int main()
{
    int errs = 0;
    errs += test_passthrough();
    errs += test_ctrl(0);

    /* NOTE: the bypass=1 case relies on the static `bypass_latched` register.
     * Statics persist across calls in csim exactly as they do in hardware,
     * which is what makes this test meaningful -- but it also means test order
     * matters. If you ever see a csim pass standalone and fail in the suite,
     * a static in the DUT is carrying state between your test cases. That is
     * not a TB bug; it is hardware behaviour you are now observing. */
    errs += test_ctrl(1);

    if (errs) {
        printf("*** FAIL: %d errors\n", errs);
        return 1;
    }
    printf("*** PASS\n");
    return 0;
}
