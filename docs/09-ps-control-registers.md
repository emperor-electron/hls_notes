# 9 — Receiving configuration from the PS

Worked example: [examples/09_control_registers](../examples/09_control_registers)

## 9.1 Three ways a value reaches the datapath

| Mechanism | Source | Use for |
|---|---|---|
| `s_axilite` | PS writes a memory-mapped register | ~95% of configuration |
| `ap_none` / `ap_stable` | A raw wire from PL | Values produced by other IP, GPIO, pins |
| `m_axi` | PS puts data in DDR, you fetch it | Config too big for registers (LUTs, matrices) |

## 9.2 The basic `s_axilite` port

```c
void kernel(vid_stream_t &src, vid_stream_t &dst, ap_uint<16> threshold) {
#pragma HLS INTERFACE s_axilite port=threshold bundle=ctrl
#pragma HLS INTERFACE ap_ctrl_none port=return
```

**Always name the bundle.** Without `bundle=ctrl` each scalar can land in its
own AXI4-Lite slave and your block design sprouts a forest of AXI ports.

`s_axilite` scalars are **read-only from the hardware side**. The PL cannot
write back to one. This single fact drives the design of §9.4.

## 9.3 The generated register map is the ABI

You do not choose the offsets. After `csynth_design` they are in:

```
<proj>/<solution>/impl/misc/drivers/<top>_v1_0/src/x<top>_hw.h
```

with a matching C driver (`x<top>.h`, `XKernel_Set_threshold()`, …) beside it.
Real output from [example 09](../examples/09_control_registers), abridged
(the tool puts the bit description on its own line):

```
// ctrl
// 0x00 : reserved          <- CTRL/GIE/IER/ISR live here under ap_ctrl_hs;
// 0x04 : reserved             with ap_ctrl_none there is no ap_start
// 0x08 : reserved
// 0x0c : reserved
// 0x10 : Data signal of rows       bit 15~0 - rows[15:0]   (Read/Write)
// 0x18 : Data signal of cols       bit 15~0 - cols[15:0]   (Read/Write)
// 0x20 : Data signal of r_gain     bit 15~0 - r_gain[15:0] (Read/Write)
// 0x28 : Data signal of g_gain
// 0x30 : Data signal of b_gain
// 0x38 : Data signal of cfg_seq    bit 31~0 - cfg_seq[31:0] (Read/Write)
// 0x40 : Data signal of bypass     bit 0    - bypass[0]    (Read/Write)
// 0x50 : Data signal of status     bit 31~0 - status[31:0] (Read)
// 0x54 : Data signal of status     bit 31~0 - status[63:32] (Read)
// 0x58 : Data signal of status     bit 31~0 - status[95:64] (Read)
```

Things to read off that map:

- **Every scalar gets an 8-byte stride** regardless of width. An `ap_uint<1>`
  costs the same address space as an `ap_uint<32>`. Do not pack flags by hand
  to "save registers" — you save nothing and lose readability.
- **A narrow type occupies the low bits** of a 32-bit register; the rest is
  reserved. Read-modify-write is unnecessary and harmful.
- **A struct is flattened into consecutive registers in declaration order.**
  That ordering is an ABI: insert a field in the middle and every offset after
  it moves, silently breaking a driver you did not rebuild. **Append only.**
- **Status is marked `(Read)`.** Pointer-to-struct outputs become read-only
  register banks.

> Use the generated header or driver. A hand-written `#define THRESHOLD 0x10`
> addresses the wrong field the moment somebody adds an argument.

## 9.4 The sampling problem, and the fix

With `ap_ctrl_none` there is no function-call boundary, so there is no defined
moment at which arguments are "passed". HLS reads each register whenever the
datapath needs it — possibly mid-frame, and at a different time for each
register. Two consequences:

**Tearing in time.** Software writes `r_gain`, `g_gain`, `b_gain` as three
separate bus transactions. The datapath can sample between them and use the new
red with the old green. One frame comes out with a colour cast.

**Tearing in space.** A register sampled mid-frame processes the top of the
frame with the old value and the bottom with the new.

Neither is a tool bug. Both are what "no call boundary" means.

### The generation-counter handshake

```
Software:   write r_gain, g_gain, b_gain   (any order)
            write cfg_seq = cfg_seq + 1     <- the commit
```

```c
bool sof = (ox == 0 && oy == 0);
if (sof && (!seq_init || cfg_seq != seq_latched)) {
    act_r = r_gain;  act_g = g_gain;  act_b = b_gain;   // atomic
    seq_latched = cfg_seq;
    seq_init = true;
}
```

One mechanism, both problems solved: **atomic**, because the shadow registers
are only ever read on the cycle the counter changed; **frame-synchronous**,
because that cycle is always the first pixel of a frame.

Four details that matter, each covered by a test in
[the testbench](../examples/09_control_registers/tb/tb_ctrl_demo.cpp):

1. **Why a counter, not an "apply" bit.** Clearing an apply bit needs the PL to
   write back to an `s_axilite` input, which it cannot do (§9.2). A
   monotonically increasing counter needs no write-back and no polling for an
   ack — hardware just remembers the last value it saw.
2. **Compare for inequality, not `>`.** `cfg_seq` wraps after ~2.3 years at one
   commit per frame at 60 Hz. `if (cfg_seq > seq_latched)` wedges configuration
   permanently after the wrap — on a product that had been running for two
   years. `!=` makes a wrap a non-event.
3. **Handle the first frame.** Registers read 0 at reset and `seq_latched` is
   0, so without a `seq_init` flag the design runs its first frames on
   hard-coded defaults and only picks up software's settings when the counter
   first changes. That is a "the first frame after boot is wrong" bug, and it
   is maddening to reproduce.
4. **Software must write the fields before bumping the counter.** On a
   Cortex-A with the usual `ioremap` (Device-nGnRnE), writes to device memory
   are not reordered, so this is free — but say so in the driver, because the
   next person will use a cached mapping for something.

### Deciding per register

Not everything should be frame-synchronous. In example 09, `bypass` is read
directly and takes effect immediately — an emergency passthrough should happen
*now*, not next frame.

The point is that it is **a choice**. Make it deliberately per register and
write it down, because your software team cannot infer it from the register map.

## 9.5 `ap_none`, `ap_stable` and friends

```c
#pragma HLS INTERFACE ap_none   port=mode    // bare input wire from PL
#pragma HLS INTERFACE ap_stable port=cols    // ...and I promise it is constant
```

`ap_stable` produces the same RTL port as `ap_none` but tells the **scheduler**
the value never changes while the block runs, so it may read it once and reuse
it instead of re-reading every cycle. That can relieve a fanout bottleneck on a
value used in many places, such as a resolution feeding every loop bound.

It is a **promise, not an enforcement**. Break it and behaviour is undefined —
part of the design sees the old value, part the new, with no reproducible
pattern. Only use it for values software writes once at configuration time and
then leaves alone, and only if your driver actually guarantees that (typically:
stop the pipeline, write, restart).

`ap_vld`/`ap_ack`/`ap_hs` give a scalar its own handshake. Rarely what you want
for configuration: on a free-running block you still have to decide what to do
when a new value arrives mid-frame, so you are back to §9.4 with extra ports.

## 9.6 Configuration too big for registers

A 256-entry gamma LUT is 1 KB. Do not make that 256 AXI-Lite registers. Put it
in DDR, give the PS a pointer register, and burst it into BRAM.

**Double-buffer it.** Load into the inactive copy and swap at SOF, so a slow
DDR read can never stall the pixel stream:

```c
static u8_t lut[2][256];
static ap_uint<1> active = 0;

if (sof && cfg_seq != seq_latched) {
    memcpy(lut[!active], gamma_ptr, 256);   // burst into the spare copy
    active = !active;
    seq_latched = cfg_seq;
}
... out = lut[active][in];
```

This reintroduces every hazard in [example 08](../examples/08_axis_to_mem_dma) —
you are blocking on the interconnect from inside a data path — which is exactly
why the load targets the *inactive* copy and the swap is a single register
write.

## 9.7 Status readback

- Publish **once per frame, not per pixel.** Four AXI-Lite writes per beat is a
  documented way to fail timing for nothing —
  [docs/06 §6.8](06-optimization-cookbook.md#68-timing-closure) has the measured
  case (3.501 ns → 2.697 ns).
- A frame-boundary update also gives the PS a **consistent snapshot** rather
  than a mix of pre- and post-increment values.
- **Saturate counters, never wrap.** A wrapped counter reads identically to "no
  errors", which is the worst possible failure mode for a diagnostic.
- Design the counters so their *signature* identifies the fault — see the table
  in [docs/04 §4.7](04-deadlock-playbook.md#47-the-thing-that-looks-like-a-deadlock-and-isnt).

## 9.8 Interrupts

With `ap_ctrl_hs`, offsets `0x04`–`0x0c` are `GIE`/`IER`/`ISR` and you get an
`interrupt` output that fires on `ap_done`. That is the natural completion
signal for a frame writer ([example 08](../examples/08_axis_to_mem_dma)).

With `ap_ctrl_none` there is no `ap_done`, so **there is no interrupt**. If
software needs to be woken per frame, either use the frame buffer IP's
interrupt, or have the PS wait on a separate always-running block, or poll
`frames_out`. Polling a status register at 60 Hz is cheap and is usually the
right answer.
