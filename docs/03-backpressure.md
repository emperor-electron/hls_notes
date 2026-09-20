# 3 — Backpressure

## 3.1 You do not write backpressure logic

```c
pix_t p = src.read();   // RTL: assert TREADY, wait for TVALID
dst.write(q);           // RTL: assert TVALID, wait for TREADY
```

`hls::stream::read()` and `write()` are **blocking**. HLS generates the stall
network from those semantics: when any access in a pipelined loop cannot
proceed, the entire pipeline stage freezes, and that freeze propagates to every
other access in the same loop. Downstream stall → your writes block → your
reads stop → TREADY deasserts → upstream stalls. The chain is automatic.

This is the whole reason `hls::stream` is safe to use in a video pipeline, and
it means **almost every backpressure bug is actually a deadlock bug or a
throughput bug**, not a "handling backpressure" bug.

## 3.2 What stalling costs

A stall is not free even though it is correct. In a pipelined loop with II=1
and depth D, a single-cycle downstream stall costs one cycle. But:

- **Stalls inside a DATAFLOW region are per-stage.** Stage 3 stalling does not
  stop stage 1 until the FIFO between them fills. That decoupling is the point
  of the FIFOs, and it is why depth matters (§3.4).
- **Stalls do not compose across a non-pipelined loop.** If your inner loop
  isn't pipelined, a one-cycle stall can cost you the whole loop-body latency.
- **A stall while `II > 1` wastes `II` cycles, not one.**

## 3.3 Budgeting: can the pipeline absorb the source?

For a free-running video block, the requirement is simply:

```
f_clk / II  ≥  pixel_rate
```

| Format | Pixel rate (incl. blanking) | Min f_clk at II=1 |
|---|---|---|
| 720p60 | 74.25 MP/s | 74.25 MHz |
| 1080p60 | 148.5 MP/s | 148.5 MHz |
| 4Kp30 | 297 MP/s | 297 MHz |
| 4Kp60 | 594 MP/s | 594 MHz — **not achievable**, widen to 2 or 4 px/beat |

At 4Kp60 you need 2 px/beat at 297 MHz or 4 px/beat at 148.5 MHz. This is a
design-time decision: it changes your line buffer widths, your window logic and
your TLAST cadence. Do not discover it after you have written the kernel.

**Blanking is your friend.** Real video has ~10–25% blanking, during which
TVALID is low and your pipeline idles. That idle time absorbs short stalls for
free. A design that is exactly at 100% utilisation on active pixels has no
margin for a DDR refresh burst; one that is at 80% has a whole blanking
interval of slack.

## 3.4 FIFO depth: the only knob you actually have

```c
#pragma HLS STREAM variable=s depth=N
```

Depth decouples a producer from a consumer for up to N tokens of misalignment.
Pick N from what actually backs up:

| Situation | Depth |
|---|---|
| Rate-matched 1-in-1-out chain | 2 (default) |
| Consumer has a startup latency of L before its first output | ≥ L + 2 |
| Bursty producer (writes a line then stalls) | ≥ burst length |
| Fan-out rejoining after different latencies | ≥ **latency skew** + 2 |
| Absorbing DDR/interconnect jitter at a memory boundary | ≥ worst-case outstanding latency × rate |

**The skew case is the one that deadlocks** — see
[example 05](../examples/05_split_join_skew) and
[§4.3](04-deadlock-playbook.md#43-family-a--latency-skew-across-a-fan-outjoin).

### Depth is compile-time; skew is often runtime

`cols + 1` is a runtime value. `depth=` is a constant. **Size for `MAX_COLS`,
not for the resolution you are testing at.** A design that works at 640×480 and
deadlocks the instant someone selects 1080p is this, every time.

### Cost

| Payload | Depth | Bits | Implementation |
|---|---|---|---|
| 8 b | ≤ 32 | ≤ 256 | SRL / LUTRAM — free |
| 8 b | 1928 | 15 Kb | 1 BRAM18 |
| 24 b | 1928 | 46 Kb | 3 BRAM18 |
| 24 b | 4104 | 98 Kb | 6 BRAM18 |

Skew FIFOs on 8-bit video are cheap. When they stop being cheap (24-bit at 4K),
the right move is to **move the fan-out point later** so less data is in
flight — not to shave the depth and hope.

## 3.5 Registering for timing, not for buffering

A depth-2 FIFO is not a pipeline register. If timing fails on the stream
handshake:

```c
#pragma HLS INTERFACE axis register both port=src
```

This is a **skid buffer**: it breaks the combinational TVALID→TREADY path in
both directions at the cost of one beat of latency. You need it as soon as your
block feeds another block across a long route, and the symptom is a WNS failure
on a `*_TREADY` path in Vivado, not anything visible in HLS.

## 3.6 Non-blocking access: when (almost never)

```c
if (!out.full())  out.write_nb(t);     // usually wrong
if (!in.empty())  v = in.read_nb();    // usually wrong
```

These convert a deadlock into **silent data loss**, which is strictly worse:
nothing hangs, so nothing alerts you, and the drop rate depends on downstream
timing so it is not reproducible.

Legitimate uses:

- draining a stream during error recovery at a frame boundary
- a genuinely optional statistics/telemetry side channel where dropping is the
  **specified, counted** behaviour

> **The test:** if you cannot state in one sentence what the system does when
> the non-blocking access fails, you want a blocking access.

Also note `empty()`/`full()` are non-blocking *peeks*. Using them as loop
conditions in synthesisable code creates a race between the peek and the
access. Confine them to `#ifndef __SYNTHESIS__`.

## 3.7 Backpressure at the memory boundary is different

An `m_axi` port can stall you for reasons that depend on other masters, DDR
refresh and arbitration — none of which you control or can bound. That alone is
slowness, not deadlock. It becomes a deadlock when you create a cycle *through*
memory; see [§4.6](04-deadlock-playbook.md#46-family-d--cycles-through-memory-or-software)
and [example 08](../examples/08_axis_to_mem_dma).

Mitigations, in order:

1. `num_write_outstanding` / `num_read_outstanding` ≥ 4 so you don't serialise
   on round-trip latency once per line.
2. `max_write_burst_length=256` (AXI4 max) to amortise address phases.
3. A deep input FIFO ahead of the memory stage to absorb jitter — this is the
   one place where a multi-BRAM FIFO is unambiguously worth it.
4. Separate read and write bundles.

## 3.8 Checklist

- [ ] `f_clk / II ≥ pixel_rate` for the maximum supported format
- [ ] Every `STREAM depth=` justified by a named scenario, not by "felt safe"
- [ ] Skew depths sized for `MAX_COLS`, not the test resolution
- [ ] No `read_nb`/`write_nb` without a one-sentence failure policy
- [ ] `register both` on stream ports that cross a long route
- [ ] `num_*_outstanding` raised on every `m_axi` port
