#ifndef VIDEO_TB_UTILS_H
#define VIDEO_TB_UTILS_H

/*
 * Test-bench-only helpers. Do NOT add this file with `add_files` -- add it
 * with `add_files -tb`, otherwise HLS will try to synthesise std::vector and
 * iostream and you will get a wall of errors that look like tool bugs.
 */

#include <cstdio>
#include <cstdlib>
#include <vector>
#include "hls_compat.h"

/* ---------------------------------------------------------------------------
 * Frame generation
 * ------------------------------------------------------------------------ */

struct Frame {
    int rows, cols;
    std::vector<unsigned int> px;   /* 0x00RRGGBB */

    Frame(int r, int c) : rows(r), cols(c), px((size_t)r * c, 0u) {}

    unsigned int &at(int y, int x)             { return px[(size_t)y * cols + x]; }
    const unsigned int &at(int y, int x) const { return px[(size_t)y * cols + x]; }
};

/* Deterministic pseudo-random frame -- same content every run, so a csim
 * regression that changes is a real change and not a reseeded RNG. */
inline Frame make_test_frame(int rows, int cols, unsigned seed = 1) {
    Frame f(rows, cols);
    unsigned s = seed * 2654435761u + 1u;
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
            s = s * 1103515245u + 12345u;
            unsigned r = (s >> 16) & 0xFF;
            unsigned g = ((s >> 8) ^ (unsigned)x) & 0xFF;
            unsigned b = ((s >> 24) ^ (unsigned)y) & 0xFF;
            f.at(y, x) = (r << 16) | (g << 8) | b;
        }
    }
    return f;
}

/* A frame with a bright square in the middle -- useful for eyeballing
 * filter kernels (edges should light up on the square's border). */
inline Frame make_square_frame(int rows, int cols) {
    Frame f(rows, cols);
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x) {
            bool in = (y > rows / 4) && (y < 3 * rows / 4) &&
                      (x > cols / 4) && (x < 3 * cols / 4);
            unsigned v = in ? 0xC0 : 0x20;
            f.at(y, x) = (v << 16) | (v << 8) | v;
        }
    return f;
}

/* ---------------------------------------------------------------------------
 * Stream drive / capture, with correct video side-channel semantics:
 *   TUSER = 1 on the very first pixel of the frame (Start Of Frame)
 *   TLAST = 1 on the last pixel of every line   (End Of Line)
 * ------------------------------------------------------------------------ */

template <typename AXI_T, typename STREAM_T>
inline void drive_frame(STREAM_T &s, const Frame &f) {
    for (int y = 0; y < f.rows; ++y) {
        for (int x = 0; x < f.cols; ++x) {
            AXI_T w;
            w.data = f.at(y, x);
            AXIS_SET_KEEP(w);
            w.user = (y == 0 && x == 0) ? 1 : 0;
            w.last = (x == f.cols - 1) ? 1 : 0;
            w.id   = 0;
            w.dest = 0;
            s.write(w);
        }
    }
}

/* Reads exactly rows*cols beats. Returns non-zero on a side-channel error.
 * Crucially this does NOT loop `while (!s.empty())` -- see docs/06 for why a
 * TB that drains opportunistically hides deadlocks instead of exposing them. */
template <typename AXI_T, typename STREAM_T>
inline int capture_frame(STREAM_T &s, Frame &f, const char *name = "out") {
    int errs = 0;
    for (int y = 0; y < f.rows; ++y) {
        for (int x = 0; x < f.cols; ++x) {
            if (s.empty()) {
                printf("[%s] STARVED at (y=%d,x=%d): DUT produced too few beats\n",
                       name, y, x);
                return errs + 1;
            }
            AXI_T w = s.read();
            f.at(y, x) = (unsigned int)w.data;

            int want_user = (y == 0 && x == 0) ? 1 : 0;
            int want_last = (x == f.cols - 1) ? 1 : 0;
            if ((int)w.user != want_user) {
                printf("[%s] TUSER mismatch at (%d,%d): got %d want %d\n",
                       name, y, x, (int)w.user, want_user);
                if (++errs > 8) return errs;
            }
            if ((int)w.last != want_last) {
                printf("[%s] TLAST mismatch at (%d,%d): got %d want %d\n",
                       name, y, x, (int)w.last, want_last);
                if (++errs > 8) return errs;
            }
        }
    }
    if (!s.empty()) {
        printf("[%s] OVERRUN: %u extra beats left in stream\n",
               name, (unsigned)s.size());
        ++errs;
    }
    return errs;
}

/* ---------------------------------------------------------------------------
 * Comparison
 * ------------------------------------------------------------------------ */

inline int compare_frames(const Frame &got, const Frame &exp,
                          int tol = 0, const char *name = "frame") {
    int errs = 0;
    for (int y = 0; y < exp.rows; ++y) {
        for (int x = 0; x < exp.cols; ++x) {
            unsigned g = got.at(y, x), e = exp.at(y, x);
            int dr = (int)((g >> 16) & 0xFF) - (int)((e >> 16) & 0xFF);
            int dg = (int)((g >>  8) & 0xFF) - (int)((e >>  8) & 0xFF);
            int db = (int)((g      ) & 0xFF) - (int)((e      ) & 0xFF);
            if (abs(dr) > tol || abs(dg) > tol || abs(db) > tol) {
                if (errs < 8)
                    printf("[%s] mismatch (%d,%d): got %06X exp %06X\n",
                           name, y, x, g, e);
                ++errs;
            }
        }
    }
    if (errs) printf("[%s] %d mismatching pixels\n", name, errs);
    return errs;
}

#endif /* VIDEO_TB_UTILS_H */
