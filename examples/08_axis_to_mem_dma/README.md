# 08 — AXI4-Stream → DDR: the memory boundary

Where a video pipeline stops being a pipeline and starts being a memory system.
The failure modes change completely:

- backpressure now comes from the interconnect and the DDR controller — bursty
  and unpredictable, not a neighbouring block with a known FIFO depth
- latency to absorb is tens-to-hundreds of cycles, not one line
- **you can now deadlock against something you don't control**

> If the Xilinx Video Frame Buffer Write IP does what you need, use it. It's
> already verified against every DDR controller Xilinx ships. Write your own
> when you need a layout or a side effect it doesn't support.

## Build

```bash
make csim  EX=08_axis_to_mem_dma
make synth EX=08_axis_to_mem_dma
```

## Burst inference is silent when it fails

```c
memcpy(dst, line, cols * sizeof(memword_t));   // most reliable burst
```

A hand-written loop *can* infer a burst, but any of these quietly breaks the
proof: a conditional inside the loop, a non-unit stride, an access pattern HLS
can't linearise (`mem[y*stride + x]` with both varying), or reading and writing
the same bundle in one loop.

**When burst inference fails you get no error** — just single-beat AXI
transactions at ~1/16 the bandwidth. Always check the synthesis log for the
confirmation message:

```bash
grep -E "214-115|burst .* inferred" vitis_hls.log
```

On a working build (2023.2) that reads:

```
INFO: [HLS 214-115] Multiple burst writes of variable length and bit width 32
  in loop 'anonymous'() has been inferred on bundle 'gmem'.
```

**If that line is absent, inference failed.** There is no warning — silence is
the failure mode.

## m_axi pragma options that actually matter

| Option | Default | Why change it |
|---|---|---|
| `max_write_burst_length` | 16 | AXI4 max is 256; 16 costs ~16× the address-phase overhead |
| `num_write_outstanding` | 1 | Without it you serialise on DDR round-trip (~100 cy) once per line |
| `offset=slave` | — | Base address in an AXI-Lite register so software can page-flip |
| `depth=` | — | **Simulation only.** No hardware effect. Wrong value = cosim OOB abort that looks like a design bug |

`num_write_outstanding=8` with `max_write_burst_length=256` costs
8 × 256 × 32 bits = 64 Kb of BRAM. That's the real trade: **BRAM for bandwidth.**

## DATAFLOW inside a loop → ping-pong

```c
for (y = 0; y < rows; ++y) {
#pragma HLS DATAFLOW
    memword_t line[MAX_COLS];      // becomes a PIPO (2 copies, swapped)
    gather_line(src, line, cols);
    burst_line(line, mem + y*stride, cols);
}
```

This overlaps gathering line *N+1* with bursting line *N*. Declaring `line`
**outside** the loop to "save BRAM" breaks the ping-pong — it becomes a single
shared buffer with a loop-carried dependency, HLS serialises the stages, and
you get the BRAM saving you asked for plus half the performance you didn't.

## Two warnings this design cannot avoid

```
WARNING: [HLS 200-1449] Process dataflow_in_loop_ROWS.1 has both a predecessor
         and reads an input from its caller ... may lead to lower throughput.
WARNING: [HLS 200-1614] Cosimulation may deadlock if process
         dataflow_in_loop_ROWS.1 has a streamed top-level array input and has
         predecessor processes.
```

These are inherent to "a dataflow stage that talks to an `m_axi` port belonging
to the parent". `burst_line` has `gather_line` as a predecessor *and* reads
`mem` from the caller. You cannot restructure this away — the memory port is a
top-level interface by definition.

They are worth knowing rather than suppressing: 200-1614 in particular tells
you that if this design ever hangs in cosim, the AXI port is the first place to
look, not your FIFO depths.

## Loop-counter rules for `DATAFLOW` inside a loop

Two constraints that produce *warnings* and a silent fall back to sequential
execution — i.e. you lose the 2× the DATAFLOW was there to buy, and only a log
line tells you:

| Rule | Violation |
|---|---|
| Counter must be a plain `int`, declared in the header, initialised to `0` | `HLS 214-107` |
| Loop bound must be a constant or a **function argument** | `HLS 214-110` |
| No expressions between stage calls (do address maths *inside* the stage) | `HLS 214-113` |

This is why `axis_to_mem` takes `int rows, int cols, int stride` where the rest
of the repo uses `ap_uint<16>`. It costs nothing — the AXI-Lite registers are
32-bit either way.

## `ap_ctrl_hs` is correct here

The only place in this repo where the block-level handshake is right. A frame
writer is a genuine per-frame call: software sets `mem`, pulses `ap_start`,
waits `ap_done`, flips buffers. With `ap_ctrl_none` software has no way to know
when a frame is complete, so it races the writer and tears.

## Deadlocking against the interconnect

1. **Read-after-write on the same bundle in one loop.** The read and write
   channels of one bundle can share a buffer; if the write backs up while the
   read waits, neither completes. **Fix: separate bundles**
   (`bundle=gmem_rd` / `bundle=gmem_wr`).
2. **Backpressure looping through the PS.** Your writer stalls on DDR; DDR is
   slow because the PS is in an interrupt handler; the handler is blocked
   waiting on your `ap_done`. No pragma fixes this — never let software's
   forward progress depend on a frame software is itself throttling.
   Double-buffer.

**Diagnosing:** if `awvalid` is stuck high and `awready` low, the problem is
downstream of you and no HLS change will help.
