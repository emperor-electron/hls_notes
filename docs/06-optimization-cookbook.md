# 6 — Optimisation cookbook

> For *writing* synthesisable C/C++ in the first place — types, loops,
> functions, what does and doesn't synthesise — see
> [docs/16](16-writing-good-hls-code.md). This document is about making code
> that already synthesises go faster.

## 6.1 Order of operations

Work in this order. Each step is cheap and each one invalidates conclusions
from the steps after it.

1. **Get II=1 on the inner loop.** Nothing else matters until this is done.
2. **Confirm loop flattening** on your nest. A flatten failure costs a pipeline
   flush per line (~1080 × depth cycles per frame at 1080p).
3. **Fix the clock estimate.** Then widen the data path if you still can't hit
   pixel rate.
4. **Then** worry about area.

Do not start by unrolling things.

## 6.2 Getting II=1

The synthesis log tells you exactly why it failed. Read it — it names the
resource or the variable.

| Log says | Cause | Fix |
|---|---|---|
| "unable to schedule load/store on array X" | Two accesses to a 2-port BRAM in one cycle | `ARRAY_PARTITION` (dim=1 on a `[2][N]` line buffer) |
| "carried dependency on X" | Loop-carried dependency HLS can't disprove | `#pragma HLS DEPENDENCE variable=X inter false` — **only if it is genuinely false** |
| "cannot flatten" | Imperfect loop nest | Move the statement between the two `for`s |
| II limited by an operator | Multi-cycle op (divide, float, sqrt) | Remove it (§6.4) |
| II limited by a stream | Two reads/writes to the same stream per iteration | Restructure to one each (§6.2.1) |

### 6.2.1 One access per stream port per iteration

This one is worth its own section because the failure is silent and the fix is
counter-intuitive.

```c
// Exceptional path: close the packet early
if (resync && have_pending) { pending.last = 1; dst.write(pending); }
...
// Normal path
if (have_pending) dst.write(pending);
pending = o;
```

Two `dst.write()` calls, on mutually exclusive paths, in one iteration. HLS
cannot schedule two writes to the same AXI-Stream port in a single II=1
iteration, so it settles for II=2 and halves your throughput:

```
WARNING: [HLS 200-880] The II Violation in module 'frame_resync_Pipeline_LOOP'
  (loop 'LOOP'): Unable to enforce a carried dependence constraint
  (II = 1, distance = 1, offset = 1) between axis write operation
  'dst_V_data_V_write_ln138' ... and axis write operation
  'dst_V_data_V_write_ln107' ...
```

Mutual exclusivity does not save you — the scheduler reasons about the port,
not the predicate.

> **Rule: at most one read and one write per stream port per iteration of a
> pipelined loop.**

**The fix is to fold the exceptional path into the normal one**, not to add a
second port. Here, the one-beat output lookahead already holds the beat, so
the resync path just *mutates* it and lets the single normal write emit it:

```c
if (resync && have_pending) pending.last = 1;   // no write
...
if (have_pending) dst.write(pending);            // the only write
pending = o;
```

Identical behaviour, `II=1`. Worked through in
[example 07](../examples/07_frame_resync).

`DEPENDENCE ... inter false` is the most dangerous directive in HLS. It tells
the scheduler to *assume* no dependency exists. If one does, you get RTL that
disagrees with csim and no warning at all. Prove it by hand before using it.

## 6.3 ARRAY_PARTITION

```c
static u8_t linebuf[2][MAX_COLS];
#pragma HLS ARRAY_PARTITION variable=linebuf complete dim=1   // 2 separate BRAMs
```

- `dim=1` on `[2][N]` → two independent memories. **This is the one that
  matters.** Without it the two line reads serialise and you get II=2.
- `dim=2` on `[2][1920]` → 1920 registers. Never do this.
- `dim=0` → partition **all** dimensions. Correct for a 3×3 window (9
  registers); catastrophic for a line buffer.
- `cyclic factor=N` → N banks, round-robin. Use when you access N consecutive
  elements per cycle (multi-pixel-per-beat).
- `block factor=N` → N banks, contiguous chunks. Use when N workers each own a
  contiguous region.

Rule of thumb: partition what you access **in parallel**, never what you access
**sequentially**.

## 6.4 Operator cost

| Operation | Cost | Replace with |
|---|---|---|
| `/` or `%` by a runtime value | ~30 cycles, kills II=1 | A carried counter; or reciprocal multiply |
| `/` or `%` by a power of 2 | Free (shift/mask) | — |
| `/` by a constant | Free (HLS folds to reciprocal multiply) | — |
| `float`/`double` | 3–10 cycles + DSPs | `ap_fixed` |
| `sqrt` | ~8 cycles + a core | `\|a\|+\|b\|` or alpha-max-beta-min |
| `sin`/`cos`/`exp`/`log` | Very expensive | A lookup table + linear interpolation |
| 8×8 multiply | 1 DSP, or LUTs | Fine |
| 25×18 multiply | 1 DSP48 | Fine — this is the native size |
| 32×32 multiply | 4 DSP48 + adders | Narrow your operands |

**Size your operands.** A DSP48E is 25×18. Two 18-bit operands fit one slice;
one 19-bit operand costs four. Declaring `ap_int<18>` instead of `int` is
frequently a 4× DSP saving.

## 6.5 PIPELINE vs UNROLL vs DATAFLOW

| | Applies to | Effect | Use when |
|---|---|---|---|
| `PIPELINE II=N` | a loop | Overlaps iterations | Always, on the innermost loop |
| `UNROLL factor=N` | a loop | N copies of the body | Small constant-bound loops (window taps) |
| `DATAFLOW` | a region | Stages run concurrently | Multi-stage pipelines |

- `PIPELINE` on an outer loop **implicitly fully unrolls** every inner loop.
  On a `for(y) for(x)` nest that means 1920 copies of your body. Pipeline the
  **inner** loop and let HLS flatten.
- `UNROLL` without a factor fully unrolls. On a runtime-bounded loop that's an
  error; on a large constant-bounded one it's an area explosion.
- `DATAFLOW` requires canonical form — see
  [docs/04 §4.5](04-deadlock-playbook.md#45-family-c--canonical-form-violations).

## 6.6 Loop flattening

HLS flattens a **perfect** or **semi-perfect** nest automatically. A single
statement between the two `for` headers makes it imperfect and flattening
fails — silently, apart from a log line.

```c
for (y = 0; y < rows; ++y) {
    ap_uint<16> row_base = y * cols;   // ← breaks flattening
    for (x = 0; x < cols; ++x) { ... }
}
```

Move it inside, or derive it with a counter. Always grep the log:

```bash
grep -i "flatten" hls_proj/solution1/syn/report/*.rpt
```

## 6.7 LOOP_TRIPCOUNT

```c
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_ROWS avg=MAX_ROWS
```

**Changes no hardware.** It only makes the latency report honest for a
runtime-variable bound. Without it, latency reads `?` and you cannot tell
whether you meet frame rate. Put one on every runtime-bounded loop.

## 6.8 Timing closure

HLS's estimated period is **optimistic** — it doesn't know your routing. If
csynth misses, P&R misses by more. Make it a hard build failure
([scripts/hls_lib.tcl](../scripts/hls_lib.tcl) does).

**Check your diagnostics first.** A real case from
[example 07](../examples/07_frame_resync): writing four 32-bit AXI-Lite status
registers *on every beat* pushed the estimate to 3.501 ns against a 3.33 ns
target. Publishing them once per frame instead brought it to 2.697 ns and cost
nothing — nobody polls diagnostic counters at 148.5 MHz. A frame-boundary
update is also a *consistent snapshot*: update per-pixel and a reader can catch
`frames_out` from after an increment and `resyncs` from before it, and conclude
a frame was clean when it wasn't.

When it still misses:

1. **Over-constrain.** Target 3.0 ns for a 3.33 ns clock. HLS inserts pipeline
   registers to meet the target you give it, so giving it slack you don't have
   is free margin.
2. **`register both` on AXIS ports.** Breaks the combinational TVALID→TREADY
   path. Usually the actual failing path once blocks are connected.
3. **Break wide combinational chains.** A 9-input adder tree at 300 MHz needs a
   register in the middle; add a manual pipeline stage in the C.
4. **Narrow the operands** (§6.4).
5. `config_compile -pipeline_loops` / `config_schedule -effort high` — try
   these last; they're a lottery.

## 6.9 Area

- **Strip AXI side channels** on internal dataflow channels. 4× on wide FIFOs.
- **Set `MAX_COLS` to what you actually support.** Leaving it at 4096 on a
  1080p design buys you 2× the line-buffer BRAM for nothing.
- **Check `BIND_STORAGE ... impl=uram`** on large buffers. UltraRAM is 288 Kb
  vs BRAM36's 36 Kb; on MPSoC it's often idle while BRAM is the constraint.
- **Share, don't replicate.** Four instances of a kernel with its own line
  buffer is 4× the BRAM. One instance at 4 px/beat is 1×.

## 6.10 Reading the QoR report

```bash
./scripts/sweep.py --example 03_line_buffer_sobel --period 2.0,2.5,3.0,3.33,4.0
```

> **Where II actually lives.** Vitis HLS hoists each pipelined loop into a
> sub-function named `<top>_Pipeline_<LOOP_LABEL>`, and the achieved II appears
> **only in that sub-function's `.rpt`**. The top-level report shows
> `* Loop: N/A` and `<top>_csynth.xml` contains no II tag at all. A scraper
> that reads only the top-level XML finds nothing, reports nothing, and your
> CI II-gate passes a design that regressed to II=4.
> `hls::parse_loop_ii` in [scripts/hls_lib.tcl](../scripts/hls_lib.tcl) scans
> every `*_csynth.rpt` for the loop table instead.

Look at, in order:

1. `ii` — must be 1. Anything else, stop and fix it.
2. `clk_est` vs `clk_target` — must be under.
3. `lat_worst` — sanity-check against `rows*cols + lag`. A latency 2× what you
   expect means a loop didn't flatten.
4. `bram` — compare against `2 × MAX_COLS × bits / 18432` per line buffer. A
   surprise means an array you didn't intend to be a memory.
5. `dsp` — a surprise means an operand widened somewhere.
