#ifndef MATMUL_H
#define MATMUL_H

#include "hls_compat.h"

/* ---------------------------------------------------------------------------
 * Blocked (tiled) matrix multiply: C = A * B, from DDR, via on-chip tiles.
 *
 * The canonical "HLS for compute" problem. Everything interesting about it is
 * the MEMORY strategy, not the arithmetic:
 *
 *   - a naive triple loop reads B column-wise, which cannot burst and cannot
 *     be cached, so it runs at a few percent of peak
 *   - tiling turns that into sequential block reads that DO burst, and lets
 *     each element be reused TILE times from on-chip memory
 *   - the inner product is then unrolled and partitioned to get parallel MACs
 *
 * Sweep it:
 *   ./scripts/sweep.py --example 15_matmul_tiled \
 *       --define TILE=8,16,32 --define UNROLL_K=1,2,4 --stage csynth
 * ------------------------------------------------------------------------ */

#ifndef TILE
#define TILE 16          /* tile edge; on-chip buffers are TILE x TILE */
#endif

/* UNROLL_K is the number of INDEPENDENT partial accumulators in the inner
 * product, which is what actually determines whether the reduction can reach
 * II=1. Too few and the adder chain is longer than one cycle. Measured on
 * ZU7EV with TILE=16 -- see the README:
 *
 *   UNROLL_K   1     2     4     8
 *   achieved   II=8  II=4  II=2  II=1
 *
 * The default is the smallest value that meets II=1 for TILE=16. If you change
 * TILE you should re-sweep this. */
#ifndef UNROLL_K
#define UNROLL_K 8
#endif

#ifndef MAX_DIM
#define MAX_DIM 256      /* largest supported matrix edge */
#endif

typedef ap_int<16>  elem_t;    /* matrix element */
typedef ap_int<48>  acc_t;     /* 16x16 products summed over <=MAX_DIM terms */

void matmul(const elem_t *A, const elem_t *B, elem_t *C,
            int M, int N, int K);

#endif
