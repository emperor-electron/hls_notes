# 7 — Verification

## 7.1 What each stage can and cannot prove

| | csim | cosim | RTL sim (Vivado) | Hardware |
|---|---|---|---|---|
| Arithmetic correctness | ✅ | ✅ | ✅ | ✅ |
| Side-channel (TUSER/TLAST) logic | ✅ | ✅ | ✅ | ✅ |
| Token-count mismatch (under-production) | ✅ aborts | ✅ | ✅ | ✅ |
| Over-production (leftover data) | ⚠️ warns, exits 0 | ✅ | ✅ | ✅ |
| **FIFO-depth deadlock** | ❌ | ✅ | ✅ | ✅ |
| **Dataflow concurrency** | ❌ | ✅ | ✅ | ✅ |
| II / throughput | ❌ | ✅ | ✅ | ✅ |
| Free-running `ap_ctrl_none` blocks | ✅ | ❌ | ✅ | ✅ |
| Real backpressure patterns | ❌ | ⚠️ ideal only | ✅ | ✅ |
| Interconnect / DDR interaction | ❌ | ❌ | ⚠️ | ✅ |
| Speed | seconds | minutes–hours | hours | — |

**csim runs dataflow stages sequentially** and treats every `hls::stream` as an
unbounded `std::deque`. The `depth=` pragma is ignored entirely. This is why a
design that deadlocks in hardware passes csim, and why a green csim on a
dataflow design proves only that the arithmetic is right.

**csim does fail on an empty read** — `ERROR [HLS SIM]: an hls::stream is read
while empty`, exit 1 (verified on 2023.2). But **leftover data is only a
warning** and exits 0, so grep the log as well:
[scripts/hls_lib.tcl](../scripts/hls_lib.tcl) does, and `build.tcl` calls it
after every csim. Full behaviour table in
[docs/04 §4.2](04-deadlock-playbook.md#what-csim-actually-does-on-an-empty-read).

**cosim drives an ideal source and sink** — it never applies realistic
backpressure. It will find a deadlock that exists unconditionally; it will not
find one that only triggers when the downstream stalls for 200 cycles. For
that you need an RTL testbench with a randomised TREADY.

## 7.2 Writing a testbench that finds bugs

### Make the golden model structurally different

```c
// DUT: line buffer + shifting window
// Golden: random access into a 2D array with explicit clamping
```

A reference that duplicates the DUT's structure shares its bugs. A
shift-direction error is present in both and the test passes.

Likewise, write the golden model for fixed-point code **in `double`**. The
point is to prove the Q16 kernel matches the real-valued definition to within
1 LSB, so the reference must not share the DUT's rounding.

### Check the side channels, not just the data

`capture_frame()` in [common/video_tb_utils.h](../common/video_tb_utils.h)
verifies TUSER and TLAST land on the right beats and that the stream isn't
over- or under-run. A frame that's pixel-perfect with wrong TUSER produces
nothing downstream.

### Do not drain opportunistically

```c
while (!dst.empty()) { ... }     // hides starvation
for (i = 0; i < rows*cols; ++i)  // exposes it
```

A TB that reads until empty turns "the DUT produced 3 beats" into a silent
pass.

### The test cases that actually find things

| Case | Finds |
|---|---|
| **Two identical frames back to back** | Stale state in `static` line buffers — the #1 line-buffer bug, invisible to a single-frame test |
| **Sparse frame** (few features) | Data-dependent token-count deadlocks |
| **Empty frame** (zero features) | Sentinel/flush logic with nothing to flush |
| **Dense frame** (everything qualifies) | Nothing — but note it makes broken code *pass*, so never test only this |
| **Truncated frame followed by a good one** | Permanent desync |
| **Frame with no leading TUSER** | Phase-lock-to-garbage |
| **Wrong line length vs `cols` register** | Config-vs-glitch discrimination |
| **`rows`/`cols` smaller than the window** | Kernels that assume `rows ≥ 3` — exactly what a misconfigured VDMA produces during bring-up |
| **Resolution change between calls** | Statics that should have been locals |

[example 07's testbench](../examples/07_frame_resync/tb/tb_frame_resync.cpp)
implements most of these.

### Statics persist across calls in csim

Exactly as they do in hardware. That's what makes multi-frame tests
meaningful — and it means **test order matters**. A case that passes standalone
and fails in the suite is a static in the DUT carrying state. That's not a TB
bug; it's hardware behaviour you're now observing.

## 7.3 Keep frames tiny

csim is O(rows·cols); cosim is ~1000× slower. Regression frames should be
16×24, not 1920×1080. Verify at full resolution once, manually, and record the
QoR — don't put it in CI.

Pick dimensions that are *awkward*: not powers of two, not equal, larger than
the window. `20×28` finds things `16×16` doesn't.

## 7.4 cosim

```tcl
cosim_design -rtl verilog -trace_level all
```

- `-trace_level all` writes a waveform. Slow and huge — but for a streaming
  design the waveform is the only way to see a deadlock, so enable it on
  dataflow designs and leave it off elsewhere.
- **A cosim *hang* (not a mismatch) is almost always a stream deadlock.** Go to
  [docs/04 §4.8](04-deadlock-playbook.md#48-debugging-a-live-hang).
- **Name every stream in its constructor.** Cosim messages and waveform signals
  use the name; an unnamed stream appears as `hls::stream<...>.0`.
- `-trace_level all` on an `m_axi` design needs `depth=` on the INTERFACE
  pragma to cover the *whole* buffer the TB allocates, padding included, or you
  get an out-of-bounds abort that looks like a design bug and isn't.

### cosim can't do free-running blocks

An unbounded `while(true)` under `ap_ctrl_none` never asserts `ap_done`, so
cosim waits forever. Options in order of preference:

1. Use the **bounded-loop** form ([docs/02 §2.4b](02-axi-stream-interfaces.md#24-three-ways-to-be-free-running)) — still free-running in hardware, terminates in simulation.
2. Verify with csim + a Vivado RTL testbench.
3. Temporarily switch to `ap_ctrl_hs` for a cosim run — but note in review that
   you verified a *different design* than you ship.

## 7.5 What only an RTL testbench gives you

Write one in SystemVerilog against the exported IP when you need:

- **Randomised TREADY.** Deassert TREADY for random 1–200 cycle intervals. This
  is the only way to find depth bugs that cosim's ideal sink can't trigger.
- **Randomised TVALID gaps** on the input, to model a real sensor's blanking.
- **Protocol assertions.** TVALID must not deassert before TREADY (AXI4-Stream
  requires the producer to hold the beat). HLS gets this right; the IP you
  connect to might not.
- **Back-to-back frames with varying geometry.**

## 7.6 CI shape

```bash
make regress
```

Keeps going after a failure, then fails at the end. A regression that stops at
the first error hides the other five.

Gate on, in order of value:

1. csim exit code
2. **csim log clean** of `contains leftover data` — that one is a warning and
   exits 0
3. csynth success
4. estimated clock ≤ target
5. **achieved II == expected II** — and note this lives in the
   `<top>_Pipeline_<LABEL>` sub-report, *not* the top-level one, which shows
   `* Loop: N/A`. A scraper that reads only `<top>_csynth.xml` finds no II at
   all and your gate silently passes everything
6. resource deltas against a checked-in baseline CSV

## 7.7 Bring-up order in hardware

1. **AXIS passthrough first.** Put [example 01](../examples/01_axis_passthrough)
   in the pipeline slot and confirm video flows through. This separates "my
   kernel is wrong" from "my block design is wrong", which are otherwise
   indistinguishable.
2. **ILA on the stream handshakes** at every block boundary. The
   `tvalid`/`tready` pattern localises a stall in seconds — see
   [docs/04 §4.8](04-deadlock-playbook.md#48-debugging-a-live-hang).
3. **Check TUSER reaches the frame buffer.** The most common "no video, no
   error" cause.
4. **Then** enable your kernel.
