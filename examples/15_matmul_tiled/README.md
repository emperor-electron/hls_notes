# 15 — Tiled matrix multiply (compute + memory)

The canonical "HLS for compute" problem. Everything interesting about it is the
**memory strategy**, not the arithmetic.

## Build

```bash
make csim EX=15_matmul_tiled
./scripts/sweep.py --example 15_matmul_tiled --define UNROLL_K=1,2,4,8 --stage csynth
```

## Why the naive triple loop is slow

```c
for (i) for (j) { acc = 0; for (k) acc += A[i*K+k] * B[k*N+j]; C[i*N+j] = acc; }
```

Three problems, only the last of which is obvious:

1. **`B[k*N+j]` strides by `N`.** A strided access cannot become an AXI burst,
   so every element costs a memory round trip — and burst inference fails
   *silently* ([docs/08 §8.4](../../docs/08-troubleshooting.md)).
2. **Every element is re-read `M` or `N` times.** ~1 op per element loaded, so
   you are bandwidth-bound at a few percent of the fabric's capability.
3. **`acc` is a loop-carried dependency**, so II is set by the adder latency.

Tiling fixes 1 and 2; partitioning + partial sums fix 3.

## Partition A and B on *opposite* dimensions

The inner product reads `A_tile[ti][k]` and `B_tile[k][tj]` for `UNROLL_K`
consecutive `k` in the same cycle — and `k` is the **second** index of one and
the **first** index of the other:

```c
#pragma HLS ARRAY_PARTITION variable=A_tile cyclic factor=UNROLL_K dim=2
#pragma HLS ARRAY_PARTITION variable=B_tile cyclic factor=UNROLL_K dim=1
```

Getting this backwards is the most common reason a tiled matmul won't hit its
target II: the numbers look right, the directive is present, and the tool still
serialises.

Use `cyclic factor=`, not `complete` — `complete` on a 32×32 tile makes 1024
registers.

## The reduction needs enough partial sums

`C_tile[ti][tj] += ...` across `tk` is a loop-carried dependency. Split it into
`UNROLL_K` independent partial accumulators and add them at the end. **The
number of partial sums must cover the adder latency**, or II can't reach 1.

Measured on ZU7EV, `TILE=16`:

| `UNROLL_K` | II | Latency | DSP | LUT |
|---|---|---|---|---|
| 1 | **8** | 1,361,489 | 22 | 6131 |
| 2 | **4** | 804,433 | 22 | 6347 |
| 4 | **2** | 525,905 | 25 | 6758 |
| 8 | **1** | 308,803 | 26 | 7988 |

4.4× the throughput for 4 extra DSPs. Note DSP count barely moves — the win is
scheduling, not arithmetic. `UNROLL_K=8` is the default because it's the
smallest value that reaches II=1 at `TILE=16`; **re-sweep if you change TILE**.

## Three `m_axi` bundles, not one

```c
#pragma HLS INTERFACE m_axi port=A offset=slave bundle=gmem_a ...
#pragma HLS INTERFACE m_axi port=B offset=slave bundle=gmem_b ...
#pragma HLS INTERFACE m_axi port=C offset=slave bundle=gmem_c ...
```

Sharing a bundle serialises A, B and C onto one port — the next tile's reads
can't overlap the last tile's writeback — and risks the read-after-write
deadlock in [docs/04 §4.6](../../docs/04-deadlock-playbook.md#46-family-d--cycles-through-memory-or-software).
Three ports cost three interconnect slaves; that beats one-third the bandwidth.

## Test with ragged dimensions

The tile loads zero-pad and the store is predicated. The tempting alternative —
*clamping* the index instead of predicating — reads real data from the wrong row
and silently corrupts edge tiles.

**A square power-of-two test matrix passes with that bug present.** The
testbench therefore uses `19×17 × 17×21`, plus `1×1`, thin rows and thin
columns, and a guard word past the end of `C`.

```
[exact-tile  ] 16x16 x 16x16   ok
[multi-tile  ] 32x32 x 32x32   ok
[ragged      ] 19x17 x 17x21   ok      <- the one that matters
[1x1         ] 1x1 x 1x1       ok
[thin-row    ] 1x3 x 3x23      ok
[thin-col    ] 23x3 x 3x1      ok
```
