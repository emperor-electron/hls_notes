#include "matmul.h"

/* ===========================================================================
 * WHY TILING, CONCRETELY
 *
 * The naive form:
 *
 *     for (i) for (j) { acc = 0; for (k) acc += A[i*K+k] * B[k*N+j]; C[i*N+j] = acc; }
 *
 * has three separate problems, and only the third is obvious:
 *
 *   1. B[k*N+j] strides by N between iterations. A strided access cannot be
 *      turned into an AXI burst, so every element costs a full memory
 *      round trip -- see docs/08 section 8.4 on burst inference failing
 *      silently.
 *   2. Every element of A and B is re-read M or N times from DDR. The
 *      arithmetic intensity is ~1 op per element loaded, so you are
 *      bandwidth-bound at a few percent of the fabric's capability.
 *   3. The k loop has a loop-carried dependency on `acc`, so its II is set by
 *      the adder latency.
 *
 * Tiling fixes 1 and 2: load TILE x TILE blocks with sequential (burstable)
 * reads, then reuse each loaded element TILE times from BRAM. Problem 3 is
 * handled by partitioning and unrolling the inner product (section below).
 * ======================================================================== */

void matmul(const elem_t *A, const elem_t *B, elem_t *C,
            int M, int N, int K)
{
    /* -- Interfaces -------------------------------------------------------
     * Three SEPARATE m_axi bundles, not one.
     *
     * Sharing a bundle serialises A, B and C onto one AXI port: the reads of
     * the next tile cannot overlap the writeback of the last, and you lose
     * most of the benefit of tiling. It also risks the read-after-write
     * deadlock in docs/04 section 4.6.
     *
     * Three ports cost three interconnect slaves. On any real SoC that is a
     * much better trade than one-third of the bandwidth. */
#pragma HLS INTERFACE m_axi port=A offset=slave bundle=gmem_a \
        depth=(MAX_DIM*MAX_DIM) max_read_burst_length=256 num_read_outstanding=8
#pragma HLS INTERFACE m_axi port=B offset=slave bundle=gmem_b \
        depth=(MAX_DIM*MAX_DIM) max_read_burst_length=256 num_read_outstanding=8
#pragma HLS INTERFACE m_axi port=C offset=slave bundle=gmem_c \
        depth=(MAX_DIM*MAX_DIM) max_write_burst_length=256 num_write_outstanding=8

#pragma HLS INTERFACE s_axilite port=A bundle=ctrl
#pragma HLS INTERFACE s_axilite port=B bundle=ctrl
#pragma HLS INTERFACE s_axilite port=C bundle=ctrl
#pragma HLS INTERFACE s_axilite port=M bundle=ctrl
#pragma HLS INTERFACE s_axilite port=N bundle=ctrl
#pragma HLS INTERFACE s_axilite port=K bundle=ctrl
#pragma HLS INTERFACE s_axilite port=return bundle=ctrl

    /* -- On-chip tiles ----------------------------------------------------
     * PARTITIONING IS THE WHOLE DESIGN.
     *
     * The inner product reads A_tile[ti][k] and B_tile[k][tj] for UNROLL_K
     * consecutive k in the same cycle. So both tiles must be partitioned
     * along their k dimension by at least UNROLL_K:
     *
     *   A_tile is [TILE][TILE] indexed [row][k]  -> partition dim=2
     *   B_tile is [TILE][TILE] indexed [k][col]  -> partition dim=1
     *
     * They are partitioned on OPPOSITE dimensions, because k is the second
     * index of one and the first index of the other. Getting this backwards
     * is the single most common reason a tiled matmul refuses to hit its
     * target II -- the numbers look right, the directive looks present, and
     * the tool still serialises.
     *
     * `cyclic factor=UNROLL_K` (not `complete`) keeps the tiles in BRAM and
     * only creates UNROLL_K banks. `complete` would turn TILE*TILE elements
     * into registers -- at TILE=32 that is 1024 registers per tile. */
    static elem_t A_tile[TILE][TILE];
    static elem_t B_tile[TILE][TILE];
    static acc_t  C_tile[TILE][TILE];
#pragma HLS ARRAY_PARTITION variable=A_tile cyclic factor=UNROLL_K dim=2
#pragma HLS ARRAY_PARTITION variable=B_tile cyclic factor=UNROLL_K dim=1
#pragma HLS ARRAY_PARTITION variable=C_tile cyclic factor=UNROLL_K dim=2

ROW_TILE:
    for (int i0 = 0; i0 < M; i0 += TILE) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=(MAX_DIM/TILE)
    COL_TILE:
        for (int j0 = 0; j0 < N; j0 += TILE) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=(MAX_DIM/TILE)

            /* Clear the output tile accumulators. */
        CLEAR:
            for (int ti = 0; ti < TILE; ++ti)
                for (int tj = 0; tj < TILE; ++tj) {
#pragma HLS PIPELINE II=1
                    C_tile[ti][tj] = 0;
                }

        K_TILE:
            for (int k0 = 0; k0 < K; k0 += TILE) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=(MAX_DIM/TILE)

                /* ---- Load A tile: rows are contiguous, so each row is a
                 * burst of TILE elements. */
            LOAD_A:
                for (int ti = 0; ti < TILE; ++ti)
                    for (int tk = 0; tk < TILE; ++tk) {
#pragma HLS PIPELINE II=1
                        int r = i0 + ti, c = k0 + tk;
                        A_tile[ti][tk] = (r < M && c < K) ? A[r * K + c] : (elem_t)0;
                    }

                /* ---- Load B tile: also row-contiguous in memory. We index
                 * it as [k][col], which is exactly B's natural layout, so
                 * this too is a sequential burst. THIS is the access pattern
                 * the naive loop got wrong. */
            LOAD_B:
                for (int tk = 0; tk < TILE; ++tk)
                    for (int tj = 0; tj < TILE; ++tj) {
#pragma HLS PIPELINE II=1
                        int r = k0 + tk, c = j0 + tj;
                        B_tile[tk][tj] = (r < K && c < N) ? B[r * N + c] : (elem_t)0;
                    }

                /* ---- Compute: TILE x TILE outputs, UNROLL_K MACs each cycle.
                 *
                 * The accumulation into C_tile[ti][tj] is a loop-carried
                 * dependency across tk. We break it by splitting into
                 * UNROLL_K independent partial sums and adding them at the
                 * end -- the standard trick for any reduction. Accumulating
                 * straight into one variable would set II to the adder
                 * latency instead of 1. */
            COMPUTE:
                for (int ti = 0; ti < TILE; ++ti) {
                    for (int tj = 0; tj < TILE; ++tj) {
#pragma HLS PIPELINE II=1
                        acc_t part[UNROLL_K];
#pragma HLS ARRAY_PARTITION variable=part complete dim=1
                    INIT:
                        for (int u = 0; u < UNROLL_K; ++u) {
#pragma HLS UNROLL
                            part[u] = 0;
                        }
                    MAC:
                        for (int tk = 0; tk < TILE; tk += UNROLL_K) {
#pragma HLS UNROLL factor=1
                            for (int u = 0; u < UNROLL_K; ++u) {
#pragma HLS UNROLL
                                part[u] += (acc_t)A_tile[ti][tk + u] *
                                           (acc_t)B_tile[tk + u][tj];
                            }
                        }
                        acc_t sum = 0;
                    REDUCE:
                        for (int u = 0; u < UNROLL_K; ++u) {
#pragma HLS UNROLL
                            sum += part[u];
                        }
                        C_tile[ti][tj] += sum;
                    }
                }
            }

            /* ---- Store the finished output tile. Row-contiguous, bursts. */
        STORE_C:
            for (int ti = 0; ti < TILE; ++ti)
                for (int tj = 0; tj < TILE; ++tj) {
#pragma HLS PIPELINE II=1
                    int r = i0 + ti, c = j0 + tj;
                    if (r < M && c < N) C[r * N + c] = (elem_t)C_tile[ti][tj];
                }
        }
    }
}

/* ===========================================================================
 * THE EDGE CASE THAT IS ALWAYS WRONG FIRST
 *
 * Matrices whose dimensions are not multiples of TILE. The loads above pad
 * with zeros and the store is predicated, which is the cheap correct answer:
 * zero-padded products contribute nothing to the sum, and out-of-range
 * outputs are simply not written.
 *
 * The tempting alternative -- clamping the index instead of predicating --
 * reads real data from the wrong row and silently corrupts the edge tiles.
 * The testbench therefore uses deliberately awkward dimensions (M, N, K not
 * multiples of TILE and not equal to each other), because a square
 * power-of-two test matrix passes with the bug present.
 * ======================================================================== */
