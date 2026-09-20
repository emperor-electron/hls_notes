#include "video_tb_utils.h"
#include "axis_to_mem.h"
#include <vector>

static const int ROWS   = 8;
static const int COLS   = 16;
static const int STRIDE = 24;    /* > COLS on purpose: real frame buffers are
                                  * padded for alignment, and a writer that
                                  * assumes stride == cols corrupts every line
                                  * after the first. */

int main()
{
    int errs = 0;

    /* Allocate with the guard region the DUT must NOT touch. Checking the
     * padding between lines is the whole point of a stride test -- a
     * stride-blind writer passes a pixel-value check and still destroys the
     * frame buffer. */
    const unsigned FILL = 0xA5A5A5A5u;
    std::vector<memword_t> mem((size_t)ROWS * STRIDE + 64, FILL);

    vid_stream_t src("src");
    Frame in = make_test_frame(ROWS, COLS, 5);
    drive_frame<vid_axis_t>(src, in);

    axis_to_mem(src, mem.data(), ROWS, COLS, STRIDE);

    /* 1. Pixels landed at the right addresses. */
    for (int y = 0; y < ROWS && errs < 10; ++y)
        for (int x = 0; x < COLS && errs < 10; ++x) {
            unsigned got = (unsigned)mem[(size_t)y * STRIDE + x];
            unsigned exp = in.at(y, x);
            if (got != exp) {
                printf("mem[%d][%d] = %08X, expected %08X\n", y, x, got, exp);
                ++errs;
            }
        }

    /* 2. The inter-line padding is untouched. */
    for (int y = 0; y < ROWS && errs < 20; ++y)
        for (int x = COLS; x < STRIDE; ++x) {
            unsigned got = (unsigned)mem[(size_t)y * STRIDE + x];
            if (got != FILL) {
                printf("stride padding clobbered at [%d][%d]: %08X\n", y, x, got);
                ++errs;
            }
        }

    /* 3. Nothing past the end of the frame. */
    for (size_t i = (size_t)ROWS * STRIDE; i < mem.size(); ++i)
        if ((unsigned)mem[i] != FILL) {
            printf("wrote past end of frame at word %u\n", (unsigned)i);
            ++errs;
            break;
        }

    printf("\n");
    printf("Reminder: csim does not model AXI at all -- `mem` is just a\n");
    printf("pointer here. csim proves addressing and stride handling only.\n");
    printf("For burst behaviour, read the csynth log and grep for:\n");
    printf("    'HLS 214-115' / 'burst .* inferred on bundle'\n");
    printf("If that line is ABSENT, burst inference failed and you have a\n");
    printf("design ~16x slower than it looks. No warning is issued.\n");

    if (errs) { printf("*** FAIL: %d errors\n", errs); return 1; }
    printf("*** PASS\n");
    return 0;
}
