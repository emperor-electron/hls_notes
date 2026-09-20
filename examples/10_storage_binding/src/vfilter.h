#ifndef VFILTER_H
#define VFILTER_H

#include "hls_compat.h"

/* ---------------------------------------------------------------------------
 * Knobs. Every one of these changes which physical memory primitive HLS picks
 * and how many of them it needs. Sweep them:
 *
 *   ./scripts/sweep.py --example 10_storage_binding \
 *       --define STORAGE=0,1,2,3 --define NTAPS=3,7,15
 *
 * STORAGE:  0 = let HLS choose        (auto)
 *           1 = force BRAM
 *           2 = force URAM
 *           3 = force LUTRAM (distributed)
 * ------------------------------------------------------------------------ */
#ifndef STORAGE
#define STORAGE 0
#endif

/* Number of vertical taps. NTAPS-1 line buffers are required. */
#ifndef NTAPS
#define NTAPS 7
#endif

/* PACKED: how the NLINES line delays are laid out in memory.
 *   0 = array of lines, [NLINES][MAX_COLS], partitioned on dim=1
 *       -> NLINES independent memories, one primitive each
 *   1 = one wide memory, ap_uint<NLINES*8>[MAX_COLS]
 *       -> ONE memory; a single read returns all NLINES pixels
 *
 * Sweep it against STORAGE to see why this matters for URAM:
 *   ./scripts/sweep.py --example 10_storage_binding \
 *       --define PACKED=0,1 --define STORAGE=1,2 --define NTAPS=15 */
#ifndef PACKED
#define PACKED 0
#endif

#ifndef MAX_COLS
#define MAX_COLS 1920
#endif
#ifndef MAX_ROWS
#define MAX_ROWS 1080
#endif

#define NLINES (NTAPS - 1)

void vfilter(gray_stream_t &src, gray_stream_t &dst,
             ap_uint<16> rows, ap_uint<16> cols);

#endif
