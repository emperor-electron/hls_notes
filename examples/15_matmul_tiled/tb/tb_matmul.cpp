#include <cstdio>
#include <cstdlib>
#include <vector>
#include "matmul.h"

/* Deliberately awkward dimensions: not multiples of TILE, not equal, not
 * powers of two. A square power-of-two test matrix passes even when the
 * tile-edge handling is wrong, which is exactly why it is the wrong test. */
static int run_case(int M, int N, int K, const char *name)
{
    std::vector<elem_t> A((size_t)M * K), B((size_t)K * N), C((size_t)M * N, 0);
    std::vector<int>    Cref((size_t)M * N, 0);

    unsigned s = 12345u;
    for (size_t i = 0; i < A.size(); ++i) { s = s*1103515245u+12345u; A[i] = (short)((s >> 20) % 41) - 20; }
    for (size_t i = 0; i < B.size(); ++i) { s = s*1103515245u+12345u; B[i] = (short)((s >> 20) % 41) - 20; }

    /* Golden model: the naive triple loop, in plain int. Structurally
     * different from the tiled DUT, which is the point. */
    for (int i = 0; i < M; ++i)
        for (int j = 0; j < N; ++j) {
            int acc = 0;
            for (int k = 0; k < K; ++k) acc += (int)A[(size_t)i*K+k] * (int)B[(size_t)k*N+j];
            Cref[(size_t)i*N+j] = acc;
        }

    /* Guard word past the end -- catches a store that runs off the tile. */
    C.push_back((elem_t)0x5A5A);

    matmul(A.data(), B.data(), C.data(), M, N, K);

    int errs = 0;
    for (int i = 0; i < M && errs < 6; ++i)
        for (int j = 0; j < N && errs < 6; ++j) {
            int got = (int)C[(size_t)i*N+j];
            int exp = (int)(short)Cref[(size_t)i*N+j];   /* DUT truncates to elem_t */
            if (got != exp) {
                printf("  [%s] C[%d][%d] = %d, expected %d\n", name, i, j, got, exp);
                ++errs;
            }
        }
    if ((int)(short)C[(size_t)M*N] != (short)0x5A5A) {
        printf("  [%s] wrote past the end of C\n", name); ++errs;
    }
    printf("  [%-12s] %dx%d x %dx%d  TILE=%d UNROLL_K=%d  %s\n",
           name, M, K, K, N, TILE, UNROLL_K, errs ? "FAIL" : "ok");
    return errs;
}

int main()
{
    printf("=== TILE=%d UNROLL_K=%d ===\n", TILE, UNROLL_K);
    int errs = 0;
    errs += run_case(TILE, TILE, TILE, "exact-tile");
    errs += run_case(2*TILE, 2*TILE, 2*TILE, "multi-tile");
    errs += run_case(TILE+3, TILE+5, TILE+1, "ragged");      /* the real test */
    errs += run_case(1, 1, 1, "1x1");
    errs += run_case(1, TILE+7, 3, "thin-row");
    errs += run_case(TILE+7, 1, 3, "thin-col");

    if (errs) { printf("\n*** FAIL: %d errors\n", errs); return 1; }
    printf("\n*** PASS\n");
    return 0;
}
