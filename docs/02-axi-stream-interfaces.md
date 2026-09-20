# 2 — AXI4-Stream interfaces

## 2.1 The type

```c
#include <ap_axi_sdata.h>
#include <hls_stream.h>

typedef ap_axiu<24, 1, 1, 1> vid_axis_t;   // TDATA=24b, TUSER=1b, TID=1b, TDEST=1b
typedef hls::stream<vid_axis_t> vid_stream_t;
```

Template parameters are `<WDATA, WUSER, WID, WDEST>`. Members are `.data`,
`.keep`, `.strb`, `.user`, `.last`, `.id`, `.dest`.

- `ap_axiu` → `.data` is `ap_uint<W>`; `ap_axis` → `.data` is `ap_int<W>`.
- Width-0 side channels are illegal on older tools; use 1 and let the unused
  port be optimised away. It costs nothing.
- Vitis HLS ≥ 2020.1 prefers `hls::axis<T,U,TI,TD>`, which also accepts
  structs and `ap_fixed` as `T`. `ap_axiu` remains an alias.

[common/hls_compat.h](../common/hls_compat.h) wraps all of this so the examples
move between installs unedited.

## 2.2 Declaring the port

```c
void k(vid_stream_t &src, vid_stream_t &dst) {
#pragma HLS INTERFACE axis port=src
#pragma HLS INTERFACE axis port=dst
#pragma HLS INTERFACE ap_ctrl_none port=return
```

**Direction is inferred from use, not declared.** Read it → slave (`s_axis`).
Write it → master (`m_axis`). A stream you both read and write is an error.

### Register slices

```c
#pragma HLS INTERFACE axis register both port=src
```

`register off|forward|reverse|both`. `both` inserts a full skid buffer,
breaking the combinational TVALID→TREADY path in *both* directions. Use it when
timing closure fails on the stream handshake — which happens as soon as your
block feeds another block across a long route. Costs one beat of latency and a
handful of FFs.

## 2.3 Video side-channel conventions

Xilinx video IP (VDMA, Video Frame Buffer, Test Pattern Generator, Subset
Converter) all agree on:

| Signal | Meaning |
|---|---|
| `TUSER[0]` | **Start Of Frame** — asserted on the *first pixel of the frame only* |
| `TLAST` | **End Of Line** — asserted on the *last pixel of every line* |
| `TKEEP`/`TSTRB` | all ones (continuous aligned stream) |

Three failure modes, all of which look like "the IP is dead":

1. **TUSER not propagated.** The Frame Buffer Write never sees SOF, so it never
   starts a frame. Everything on the scope looks healthy. Copy the whole
   `ap_axiu` word (`dst.write(w)`), don't copy `.data` and `.last` by hand.
2. **TLAST on the last pixel of the frame instead of every line.** The consumer
   waits for an end-of-line that arrives once per frame, so line buffers
   overflow or the DMA descriptor never completes.
3. **TKEEP left at zero.** `ap_axiu` gives you TKEEP whether you want it or
   not. AXIS Subset Converters and DMA engines *drop bytes whose TKEEP bit is
   0*, so you get silent corruption, not an error. Always
   `w.keep = -1; w.strb = -1;`.

If the downstream IP has no `tkeep`/`tstrb` ports, either drive them anyway
(harmless) or strip them with an AXI4-Stream Subset Converter in the block
design.

## 2.4 Three ways to be free-running

### (a) `while(true)` + `ap_ctrl_none`

```c
#pragma HLS INTERFACE ap_ctrl_none port=return
while (true) {
#pragma HLS PIPELINE II=1
    dst.write(src.read());
}
```

Fully data-driven; no assumption about frame length. **Does not co-simulate**
(cosim waits for `ap_done`, which never arrives) and hangs csim unless you
guard the condition — see `HLS_FOREVER_ON` in
[common/hls_compat.h](../common/hls_compat.h).

Used by [example 01](../examples/01_axis_passthrough) and
[example 07](../examples/07_frame_resync).

### (b) Bounded loop + `ap_ctrl_none` ← **default choice**

```c
#pragma HLS INTERFACE ap_ctrl_none port=return
for (y = 0; y < rows; ++y)
  for (x = 0; x < cols; ++x) {
#pragma HLS PIPELINE II=1
    ...
  }
```

HLS wraps the body in an implicit forever-loop, so it is **still free-running
hardware** — but csim terminates, cosim works, and the latency report is
meaningful. The trade: it assumes every frame is exactly `rows*cols` beats.
[Example 07](../examples/07_frame_resync) shows how to keep that assumption
from becoming permanent desync.

Used by examples 02–06.

### (c) Bounded loop + `ap_ctrl_hs` (the default if you say nothing)

One frame per `ap_start` pulse. **This is the #1 "my HLS video IP produces one
frame then dies" bug** — and it is not a deadlock: the block is idle, waiting
to be told to go again. Correct only when software genuinely drives each
invocation, e.g. a frame writer that needs `ap_done` to know when a buffer is
safe to read ([example 08](../examples/08_axis_to_mem_dma)).

## 2.5 Control registers on a free-running block

You can have both:

```c
#pragma HLS INTERFACE s_axilite port=threshold bundle=ctrl
#pragma HLS INTERFACE ap_ctrl_none port=return
```

The AXI-Lite bank contains your fields but no `CTRL`/`ap_start` register.

**Sampling is unspecified.** With no function-call boundary, HLS reads the
register whenever the datapath needs it — possibly mid-frame. If a mid-frame
change would tear a frame, latch it yourself:

```c
if (w.user) threshold_latched = threshold;   // latch on Start-Of-Frame
```

Do not rely on software writing during blanking.

## 2.6 Width conversion

- **Widening (e.g. 1 pixel → 4 pixels/beat)** is how you get past ~150 MHz on
  4K. Pack N pixels into one `ap_uint<N*24>` beat and process N per cycle. Your
  line buffers narrow by N and your TLAST arrives N× less often.
- HLS will not do this for you. Either do it explicitly in your kernel or put
  an AXI4-Stream Data Width Converter in the block design.
- **Watch the odd remainder.** 1920 is divisible by 4 but not by 8 in pixel
  terms if you also need TKEEP granularity — handle the partial final beat, or
  constrain `cols` to a multiple of N and validate it in the driver.

## 2.7 Quick reference

| Want | Write |
|---|---|
| Stream in/out | `#pragma HLS INTERFACE axis port=s` |
| Break handshake timing | `#pragma HLS INTERFACE axis register both port=s` |
| Free-running, no CPU | `#pragma HLS INTERFACE ap_ctrl_none port=return` |
| Runtime config | `#pragma HLS INTERFACE s_axilite port=p bundle=ctrl` |
| Frame-at-a-time, SW-driven | `#pragma HLS INTERFACE s_axilite port=return bundle=ctrl` |
| DDR master | `#pragma HLS INTERFACE m_axi port=p offset=slave bundle=gmem` |
