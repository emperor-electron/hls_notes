# 10 — Storage binding: making HLS use BRAM and URAM well

An N-tap vertical filter whose arithmetic is deliberately trivial, so that
essentially all the reported BRAM/URAM/LUT comes from the line buffer and you
can read the cost directly.

## Build and sweep

```bash
make csim EX=10_storage_binding

./scripts/sweep.py --example 10_storage_binding --define STORAGE=0,1,2,3
./scripts/sweep.py --example 10_storage_binding \
    --define PACKED=0,1 --define STORAGE=1,2 --define NTAPS=15
```

| Knob | Values |
|---|---|
| `STORAGE` | 0 auto · 1 bram · 2 uram · 3 lutram |
| `NTAPS` | vertical taps (odd); uses `NTAPS-1` line buffers |
| `PACKED` | 0 array-of-lines · 1 one wide memory |

## Result 1 — binding choice

6 line buffers of 1920×8b (`NTAPS=7`), measured on ZU7EV:

| `impl` | BRAM18 | URAM | FF | LUT |
|---|---|---|---|---|
| auto | 6 | 0 | 1290 | 1454 |
| bram | 6 | 0 | 1290 | 1454 |
| uram | 0 | 6 | 1290 | 1454 |
| lutram | 0 | 0 | 1032 | **2766** |

## Result 2 — the rounding loss

`ARRAY_PARTITION complete dim=1` gives independent memories (which the filter
needs — one pixel from every line, every cycle) but you pay **one whole
primitive per line**, however little it holds:

- BRAM18: 15,360 / 18,432 = **83%** utilised. Fine.
- URAM: 15,360 / 294,912 = **5.2%** utilised. Catastrophic.

Fourteen URAMs to store 210 Kb that fits in one. This is why "force URAM to
save BRAM" is usually wrong.

## Result 3 — pack into width, not count

Put all `N-1` delays in the **width** of one memory. One read still returns
every line's pixel, so you keep `II=1`:

```c
typedef ap_uint<NLINES*8> lineword_t;
static lineword_t linebuf[MAX_COLS];
```

`NTAPS=15` (14 lines, 210 Kb):

| Layout | impl | BRAM18 | URAM | FF | LUT |
|---|---|---|---|---|---|
| Partitioned | bram | 14 | 0 | 1944 | 2143 |
| Partitioned | uram | 0 | **14** | 1944 | 2143 |
| **Packed** | bram | 13 | 0 | **876** | **1227** |
| **Packed** | uram | 0 | **2** | **876** | **1227** |

- **URAM 14 → 2** (7×), utilisation 5% → 36%
- **BRAM 14 → 13** — barely moves; it was already 83% utilised
- **FF/LUT roughly halve** — the shift is now bit-slicing one register instead
  of driving 14 separate memory port sets

Two URAMs rather than one because the packed word is `14×8 = 112` bits and URAM
is 72 wide, so the tool places two side by side. **Past 72 bits you pay per line
again** — for 8-bit video the sweet spot is ≤ 9 lines per URAM.

## A note on the timing numbers

Every variant reports `clk_estimate = 3.330` against a 3.33 ns target — exactly
at the limit, not comfortably under. That is normal: HLS schedules to *meet*
the constraint you give it and then stops, so "exactly at target" is what a
non-trivial design looks like. It is also why
[docs/06 §6.8](../../docs/06-optimization-cookbook.md#68-timing-closure) tells
you to over-constrain — ask for 3.0 ns if you need 3.33, because the HLS
estimate is optimistic about routing and place-and-route will only do worse.

The comparison between variants is still valid: they all had the same target,
so the differences in BRAM/URAM/FF/LUT are like-for-like.

## Scope note

This kernel does **not** do border replication — clamping the vertical taps
needs a runtime-indexed read of the tap file, which at `NTAPS=15` is 15 muxes of
15 inputs and would swamp the numbers the example exists to show. Borders are
covered in [example 03](../03_line_buffer_sobel); the testbench here checks
interior rows only, across two consecutive frames.

Full treatment: [docs/11](../../docs/11-memory-resources.md).
