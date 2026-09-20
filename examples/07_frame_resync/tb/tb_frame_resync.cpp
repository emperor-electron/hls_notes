#include "video_tb_utils.h"
#include "frame_resync.h"
#include <vector>

static const int ROWS = 8;
static const int COLS = 12;

/* Raw beat, so the TB can construct deliberately malformed input. */
struct beat { unsigned data; int user, last; };

static void push(vid_stream_t &s, const std::vector<beat> &bs)
{
    for (size_t i = 0; i < bs.size(); ++i) {
        vid_axis_t w;
        w.data = bs[i].data;
        AXIS_SET_KEEP(w);
        w.user = bs[i].user;
        w.last = bs[i].last;
        w.id = 0; w.dest = 0;
        s.write(w);
    }
}

/* A well-formed frame of `rows` x `cols`, pixel value = linear index + base. */
static std::vector<beat> good_frame(int rows, int cols, unsigned base)
{
    std::vector<beat> v;
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x) {
            beat b;
            b.data = base + (unsigned)(y * cols + x);
            b.user = (y == 0 && x == 0);
            b.last = (x == cols - 1);
            v.push_back(b);
        }
    return v;
}

/* Drain the DUT output, returning every beat. */
static std::vector<beat> drain(vid_stream_t &s)
{
    std::vector<beat> v;
    while (!s.empty()) {
        vid_axis_t w = s.read();
        beat b; b.data = (unsigned)w.data; b.user = (int)w.user; b.last = (int)w.last;
        v.push_back(b);
    }
    return v;
}

/* Check that every emitted beat obeys the output contract:
 * exactly one TUSER per rows*cols block, TLAST every cols beats. */
/* `require_terminated`: whether the output is expected to end on a packet
 * boundary.
 *
 * It is NOT always expected. This block is free-running: it cannot know the
 * stream has ended, so if the input simply stops part-way through a line, the
 * output legitimately stops part-way through a packet. In hardware that is a
 * non-event -- the held beat goes out as soon as the next pixel arrives.
 *
 * So require it only when the input contained a whole number of output lines.
 * Asserting it unconditionally makes the testbench fail on correct behaviour,
 * which is worse than not testing it at all. */
static int check_geometry(const std::vector<beat> &out, int cols,
                          const char *name, bool require_terminated = true)
{
    int errs = 0, col = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        int want_last = (col == cols - 1);
        if (out[i].last != want_last) {
            printf("[%s] beat %u: TLAST=%d want %d (col %d)\n",
                   name, (unsigned)i, out[i].last, want_last, col);
            if (++errs > 6) return errs;
        }
        col = (col == cols - 1) ? 0 : col + 1;
    }
    if (require_terminated && !out.empty() && out.back().last != 1) {
        printf("[%s] stream ends MID-PACKET -- downstream DMA would hang\n", name);
        ++errs;
    }
    return errs;
}

static int test_clean()
{
    vid_stream_t src("src"), dst("dst");
    resync_status_t st = {0, 0, 0, 0};

    push(src, good_frame(ROWS, COLS, 0));
    frame_resync(src, dst, ROWS, COLS, &st);

    std::vector<beat> out = drain(dst);
    int errs = 0;
    if ((int)out.size() != ROWS * COLS) {
        printf("[clean] got %u beats, want %d\n", (unsigned)out.size(), ROWS * COLS);
        ++errs;
    }
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i].data != (unsigned)i) { printf("[clean] data corrupt at %u\n", (unsigned)i); ++errs; break; }
    errs += check_geometry(out, COLS, "clean");
    if (st.resyncs != 0 || st.eol_mismatch != 0 || st.sof_dropped != 0) {
        printf("[clean] spurious status: resync=%u eol=%u drop=%u\n",
               (unsigned)st.resyncs, (unsigned)st.eol_mismatch, (unsigned)st.sof_dropped);
        ++errs;
    }
    printf("[clean] %s\n", errs ? "FAIL" : "ok");
    return errs;
}

static int test_no_leading_sof()
{
    /* Stream starts mid-frame: no TUSER until partway in. A counted block
     * would lock onto the wrong phase permanently. */
    vid_stream_t src("src"), dst("dst");
    resync_status_t st = {0, 0, 0, 0};

    std::vector<beat> v;
    for (int i = 0; i < 17; ++i) { beat b = {0xDEAD0000u + i, 0, 0}; v.push_back(b); }
    std::vector<beat> f = good_frame(ROWS, COLS, 0);
    v.insert(v.end(), f.begin(), f.end());
    push(src, v);

    frame_resync(src, dst, ROWS, COLS, &st);
    std::vector<beat> out = drain(dst);

    int errs = 0;
    if ((int)out.size() != ROWS * COLS) {
        printf("[no_sof] got %u beats want %d\n", (unsigned)out.size(), ROWS * COLS); ++errs;
    }
    if (st.sof_dropped != 17) {
        printf("[no_sof] dropped %u, want 17\n", (unsigned)st.sof_dropped); ++errs;
    }
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i].data != (unsigned)i) { printf("[no_sof] locked to wrong phase at %u\n", (unsigned)i); ++errs; break; }
    errs += check_geometry(out, COLS, "no_sof");
    printf("[no_sof] %s (dropped %u pre-SOF beats)\n", errs ? "FAIL" : "ok",
           (unsigned)st.sof_dropped);
    return errs;
}

static int test_short_frame()
{
    /* THE case this block exists for. A truncated frame, immediately followed
     * by a good one. Requirement: the good frame comes out correct and
     * correctly framed. A counted block fails every frame from here on. */
    vid_stream_t src("src"), dst("dst");
    resync_status_t st = {0, 0, 0, 0};

    std::vector<beat> truncated = good_frame(ROWS, COLS, 0x1000);
    truncated.resize(ROWS * COLS - 7);            /* chop the tail */
    std::vector<beat> v = truncated;
    std::vector<beat> f = good_frame(ROWS, COLS, 0);
    v.insert(v.end(), f.begin(), f.end());
    push(src, v);

    frame_resync(src, dst, ROWS, COLS, &st);
    std::vector<beat> out = drain(dst);

    int errs = 0;
    if (st.resyncs != 1) {
        printf("[short] resyncs=%u want 1\n", (unsigned)st.resyncs); ++errs;
    }
    /* The second frame must be intact, whatever happened to the first. */
    if ((int)out.size() < ROWS * COLS) {
        printf("[short] only %u beats total\n", (unsigned)out.size()); ++errs;
    } else {
        const size_t off = out.size() - (size_t)(ROWS * COLS);
        for (size_t i = 0; i < (size_t)(ROWS * COLS); ++i) {
            if (out[off + i].data != (unsigned)i) {
                printf("[short] recovered frame corrupt at %u: %08X\n",
                       (unsigned)i, out[off + i].data);
                ++errs; break;
            }
        }
        if (out[off].user != 1) { printf("[short] recovered frame has no SOF\n"); ++errs; }
    }
    /* And the truncated packet must have been properly closed. */
    if (!out.empty() && out.back().last != 1) {
        printf("[short] stream ends mid-packet\n"); ++errs;
    }
    printf("[short] %s (resyncs=%u)\n", errs ? "FAIL" : "ok", (unsigned)st.resyncs);
    return errs;
}

static int test_wrong_line_length()
{
    /* Upstream emits TLAST every COLS+1 beats -- a `cols` misconfiguration,
     * not a glitch. Expect eol_mismatch to climb while resyncs stays low:
     * that signature is how you tell a config bug from a cable glitch. */
    vid_stream_t src("src"), dst("dst");
    resync_status_t st = {0, 0, 0, 0};

    std::vector<beat> v;
    const int bad_cols = COLS + 1;
    for (int y = 0; y < ROWS; ++y)
        for (int x = 0; x < bad_cols; ++x) {
            beat b;
            b.data = (unsigned)(y * bad_cols + x);
            b.user = (y == 0 && x == 0);
            b.last = (x == bad_cols - 1);
            v.push_back(b);
        }
    push(src, v);

    frame_resync(src, dst, ROWS, COLS, &st);
    std::vector<beat> out = drain(dst);

    /* 8 x 13 = 104 input beats against cols=12: 104 % 12 = 8, so the output
     * cannot end on a line boundary. That is correct behaviour, not a bug --
     * see check_geometry(). */
    int errs = check_geometry(out, COLS, "badlen", /*require_terminated=*/false);
    if (st.eol_mismatch == 0) {
        printf("[badlen] eol_mismatch should be non-zero\n"); ++errs;
    }
    printf("[badlen] %s (eol_mismatch=%u, resyncs=%u -> config bug signature)\n",
           errs ? "FAIL" : "ok", (unsigned)st.eol_mismatch, (unsigned)st.resyncs);
    return errs;
}

int main()
{
    int errs = 0;
    errs += test_clean();
    errs += test_no_leading_sof();
    errs += test_short_frame();
    errs += test_wrong_line_length();

    if (errs) { printf("*** FAIL: %d errors\n", errs); return 1; }
    printf("*** PASS\n");
    return 0;
}
