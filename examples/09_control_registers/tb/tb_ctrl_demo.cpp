#include "video_tb_utils.h"
#include "ctrl_demo.h"

static const int ROWS = 6;
static const int COLS = 8;

static unsigned apply_gain(unsigned px, int gr, int gg, int gb)
{
    unsigned r = (((px >> 16) & 0xFF) * gr + 128) >> 8;
    unsigned g = (((px >>  8) & 0xFF) * gg + 128) >> 8;
    unsigned b = (((px      ) & 0xFF) * gb + 128) >> 8;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (r << 16) | (g << 8) | b;
}

/* Drive one frame through the DUT and return what came out. */
static Frame run_frame(const Frame &in,
                       int gr, int gg, int gb, unsigned seq, int bypass,
                       ctrl_status_t &st)
{
    vid_stream_t src("src"), dst("dst");
    drive_frame<vid_axis_t>(src, in);
    ctrl_demo(src, dst, in.rows, in.cols, gr, gg, gb, seq, bypass, &st);
    Frame got(in.rows, in.cols);
    capture_frame<vid_axis_t>(dst, got, "ctrl");
    return got;
}

int main()
{
    int errs = 0;
    ctrl_status_t st = {0, 0, 0};
    Frame in = make_test_frame(ROWS, COLS, 11);

    /* ---- 1. First frame: config must latch even though seq is 0 ---------
     * The `seq_init` flag exists for exactly this. Without it, a design whose
     * registers happen to be 0 at reset would run its first frames at the
     * hard-coded default (unity gain) and only pick up software's settings
     * when the counter first changed -- a "the first frame after boot is
     * wrong" bug that is maddening to reproduce. */
    Frame f1 = run_frame(in, 512, 256, 128, 0, 0, st);   /* 2.0, 1.0, 0.5 */
    for (int y = 0; y < ROWS; ++y)
        for (int x = 0; x < COLS; ++x) {
            unsigned e = apply_gain(in.at(y, x), 512, 256, 128);
            if (f1.at(y, x) != e) {
                printf("[first] (%d,%d) got %06X exp %06X\n", y, x, f1.at(y,x), e);
                if (++errs > 4) goto done;
            }
        }
    if (st.cfg_applied != 1) {
        printf("[first] cfg_applied=%u, want 1\n", (unsigned)st.cfg_applied); ++errs;
    }
    printf("[first]      ok (gains latched on frame 1, cfg_applied=%u)\n",
           (unsigned)st.cfg_applied);

    /* ---- 2. Change the gains but DO NOT bump cfg_seq ---------------------
     * This is the core of the contract: a shadow-register write with no
     * commit must have NO effect. If this test fails, the datapath is reading
     * the shadow registers directly and you have the tearing bug the whole
     * pattern exists to prevent. */
    {
        Frame f2 = run_frame(in, 64, 64, 64, 0, 0, st);   /* new gains, same seq */
        for (int y = 0; y < ROWS; ++y)
            for (int x = 0; x < COLS; ++x) {
                unsigned e = apply_gain(in.at(y, x), 512, 256, 128);  /* OLD gains */
                if (f2.at(y, x) != e) {
                    printf("[uncommitted] (%d,%d) got %06X exp %06X "
                           "-- shadow regs leaked into the datapath\n",
                           y, x, f2.at(y,x), e);
                    if (++errs > 4) goto done;
                }
            }
        if (st.cfg_applied != 1) {
            printf("[uncommitted] cfg_applied=%u, want still 1\n",
                   (unsigned)st.cfg_applied); ++errs;
        }
        printf("[uncommitted] ok (gain write without commit had no effect)\n");
    }

    /* ---- 3. Bump cfg_seq: now it takes effect ---------------------------- */
    {
        Frame f3 = run_frame(in, 64, 64, 64, 1, 0, st);
        for (int y = 0; y < ROWS; ++y)
            for (int x = 0; x < COLS; ++x) {
                unsigned e = apply_gain(in.at(y, x), 64, 64, 64);
                if (f3.at(y, x) != e) {
                    printf("[committed] (%d,%d) got %06X exp %06X\n",
                           y, x, f3.at(y,x), e);
                    if (++errs > 4) goto done;
                }
            }
        if (st.cfg_applied != 2 || st.cfg_seq_seen != 1) {
            printf("[committed] cfg_applied=%u seq_seen=%u, want 2 / 1\n",
                   (unsigned)st.cfg_applied, (unsigned)st.cfg_seq_seen); ++errs;
        }
        printf("[committed]  ok (commit applied, cfg_applied=%u seq_seen=%u)\n",
               (unsigned)st.cfg_applied, (unsigned)st.cfg_seq_seen);
    }

    /* ---- 4. bypass is immediate, not frame-synchronous -------------------
     * Documents the deliberate asymmetry. */
    {
        Frame f4 = run_frame(in, 64, 64, 64, 1, 1, st);
        errs += compare_frames(f4, in, 0, "bypass");
        printf("[bypass]     ok (unlatched register takes effect at once)\n");
    }

    /* ---- 5. A wrapped sequence counter must still commit -----------------
     * cfg_seq is 32 bits; at one commit per frame at 60 Hz it wraps after
     * ~2.3 years of continuous operation. Comparing for INEQUALITY rather
     * than greater-than means a wrap is a non-event. Writing
     * `if (cfg_seq > seq_latched)` would wedge configuration permanently
     * after the wrap -- on a product that had been running for two years. */
    {
        st.cfg_applied = 0;
        Frame f5 = run_frame(in, 300, 300, 300, 0xFFFFFFFFu, 0, st);
        for (int y = 0; y < ROWS; ++y)
            for (int x = 0; x < COLS; ++x) {
                unsigned e = apply_gain(in.at(y, x), 300, 300, 300);
                if (f5.at(y, x) != e) { printf("[wrap] mismatch\n"); ++errs; goto done; }
            }
        Frame f6 = run_frame(in, 200, 200, 200, 0, 0, st);   /* wrapped to 0 */
        for (int y = 0; y < ROWS; ++y)
            for (int x = 0; x < COLS; ++x) {
                unsigned e = apply_gain(in.at(y, x), 200, 200, 200);
                if (f6.at(y, x) != e) {
                    printf("[wrap] (%d,%d) got %06X exp %06X "
                           "-- config wedged after counter wrap\n",
                           y, x, f6.at(y,x), e);
                    ++errs; goto done;
                }
            }
        printf("[wrap]       ok (seq 0xFFFFFFFF -> 0 still commits)\n");
    }

done:
    if (errs) { printf("*** FAIL: %d errors\n", errs); return 1; }
    printf("*** PASS\n");
    return 0;
}
