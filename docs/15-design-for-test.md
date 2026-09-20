# 15 — Designing HLS modules for on-FPGA testability

## 15.1 The fundamental problem

**HLS deletes anything that does not affect an output.** A "debug" variable you
compute and never use does not become a signal you can probe — it becomes
nothing at all. And what survives is renamed and rescheduled: your C has no
one-to-one correspondence with the RTL.

So on-chip observability in HLS is not "find the signal and probe it". It is a
**design decision you make up front**: decide what you need to observe, and
make it an output.

Three tiers, cheapest first:

| Tier | Mechanism | Cost | Gets you |
|---|---|---|---|
| 1 | Status counters over AXI-Lite | ~nothing | 90% of bring-up |
| 2 | Debug wires / a debug bus + ILA | a few LUTs | Cycle-accurate behaviour |
| 3 | A debug AXI-Stream tap | a FIFO | Full data capture |

Use tier 1 until it genuinely is not enough.

## 15.2 Tier 1 — counters and status registers

This is by far the highest value per LUT, and it is covered in detail in
[docs/09 §9.7](09-ps-control-registers.md#97-status-readback). The short version:

- Count **events**, not states: frames completed, resyncs, protocol errors,
  stall cycles, min/max/sum of something.
- **Saturate, never wrap.** A wrapped counter reads identically to "no errors".
- **Publish once per frame**, not per pixel — per-pixel AXI-Lite writes cost
  real timing ([docs/06 §6.8](06-optimization-cookbook.md#68-timing-closure)).
- Design the counters so their **signature** identifies the fault. The table in
  [docs/04 §4.7](04-deadlock-playbook.md#47-the-thing-that-looks-like-a-deadlock-and-isnt)
  distinguishes "upstream glitch" from "register misconfigured" using two
  counters and no ILA.

### The frame checksum

The single most useful diagnostic in a video pipeline:

```c
static ap_uint<32> crc;                 // or a simple sum/XOR accumulator
if (sof) crc = 0xFFFFFFFF;
crc = crc_update(crc, pixel);
if (eof) status->frame_crc = crc;       // published once per frame
```

Compute the same checksum in your C testbench. Now you can answer
*"is the hardware producing the same pixels as the simulation?"* — bit-exactly,
over a whole frame, with one register read and no ILA, no DMA and no capture
buffer. Put one at the input and one at the output of each block and you can
bisect a pipeline in minutes.

## 15.3 Tier 2 — debug wires and an ILA

### Make it an output, or it will not exist

```c
void kernel(..., ap_uint<32> *dbg) {
#pragma HLS INTERFACE ap_vld port=dbg     // or ap_none, or s_axilite
```

An output port is by definition observable, so HLS must keep whatever feeds it.
This is the *only* reliable way to guarantee an internal value survives.

`volatile` on a local is sometimes suggested for this. It does discourage some
optimisations, but it does not create a port and it is not a contract — if
nothing reads the value, nothing has to exist. Use a port.

### The debug bus pattern

Rather than one port per signal, expose one wide bus and a selector:

```c
void kernel(..., ap_uint<8> dbg_sel, ap_uint<32> *dbg_bus) {
#pragma HLS INTERFACE s_axilite port=dbg_sel bundle=ctrl
#pragma HLS INTERFACE ap_none   port=dbg_bus
    ...
    switch (dbg_sel) {                       // one mux, N observables
        case 0:  *dbg_bus = (ox, oy);          break;
        case 1:  *dbg_bus = frame_count;       break;
        case 2:  *dbg_bus = (pix, win[1][1]);  break;
        default: *dbg_bus = 0xDEADBEEF;        break;
    }
}
```

One ILA probe, N things to look at, selected at runtime from software. The cost
is a mux; the benefit is that you do not re-synthesise to change what you watch.

### Probe at the boundaries, not inside

The highest-information probe in a streaming design is the handshake pair on
each block boundary:

| `tvalid` | `tready` | Meaning |
|---|---|---|
| 1 | 0 | Consumer blocked — problem is **downstream** |
| 0 | 1 | Producer starved — problem is **upstream** |
| 1 | 1 | Data moving |
| 0 | 0 | Both stalled — you are inside a cycle |

Walk the direction the stall points until it loops back on itself; that is your
deadlock ([docs/04 §4.8](04-deadlock-playbook.md#48-debugging-a-live-hang)).
Two bits per boundary tells you more than fifty bits of internal state.

### Signal names in the generated RTL

Measured on Vitis HLS 2023.2:

- **Top-level ports keep their C names exactly** — `rows`, `cols`, `threshold`,
  `src_TDATA`. These are always safe to reference.
- **Scalar reads appear as `<name>_read_reg_<N>`** — e.g. `threshold_read_reg_341`.
  In my testing those numbers stayed put across a clock-period change and an
  unrelated source edit, but they are **LLVM IR value numbers, not a contract**.
  A change that reorders those operations will move them.
- **Arrays and internal state are mangled** into memory instances and pipeline
  registers with no recoverable relationship to your C.

Practical consequence: **do not hand-maintain an ILA probe file full of
`*_read_reg_NNN` names.** Either probe top-level ports and your own debug bus
(stable by construction), or regenerate the probe list after each build.

### Marking signals for debug

HLS does not emit `(* mark_debug *)`. Apply it in Vivado, on the **IP's ports**
(which are stable) rather than on internals:

```tcl
set_property MARK_DEBUG true [get_nets -hier -filter {NAME =~ *dbg_bus*}]
```

…or just instantiate an ILA in the block design and wire your debug ports to it,
which survives re-synthesis far better than net-name matching.

## 15.4 Tier 3 — a debug stream tap

When you need the actual pixels:

```c
void kernel(vid_stream_t &src, vid_stream_t &dst,
            vid_stream_t &dbg, ap_uint<1> dbg_en) {
    ...
    dst.write(o);
    if (dbg_en) dbg.write(o);      // tap
}
```

Two warnings, both from [docs/04](04-deadlock-playbook.md):

1. **A conditional write with a counted reader is a deadlock** (family B). If
   `dbg_en` is 0 the consumer gets nothing and blocks forever. Either make the
   consumer data-driven (a DMA reading until TLAST is fine), or always write and
   tag.
2. **A tap is a fan-out.** If anything ever rejoins the two paths, you now have
   a skew FIFO to size (family A). Keep the tap terminal — straight into a DMA
   — and never merge it back.

Route the tap to an AXI-Stream FIFO or a Video Frame Buffer Write and you can
pull whole frames into DDR from software.

## 15.5 Make the block testable without the rest of the system

Bring-up fails fastest when you cannot isolate anything. Two cheap features pay
for themselves immediately:

**A bypass path.** Covered in
[example 01](../examples/01_axis_passthrough) — one register that makes the
block a passthrough. Lets you prove the *plumbing* works before you debug the
*algorithm*, and those are otherwise indistinguishable.

**An internal pattern generator.** A register that makes the block ignore its
input and emit a known ramp or checkerboard. Now you can test everything
downstream with no camera, no HDMI source and no capture card. This is the
difference between "the pipeline is broken somewhere" and "the pipeline is fine,
the sensor is not configured."

```c
ap_uint<8> p = tpg_en ? (ap_uint<8>)((y ^ x) & 0xFF) : src.read().data;
```

Note the hazard: with `tpg_en` set, this stops reading `src`, so upstream will
back up. That is usually what you want (the source stalls harmlessly), but if
`src` feeds anything else through a fan-out, you have created family A. Drain
rather than skip if in doubt:

```c
ap_uint<8> in = src.read().data;          // ALWAYS consume
ap_uint<8> p  = tpg_en ? pattern(y, x) : in;
```

## 15.6 Expose wire I/O when RTL owns the register map

If you already have AXI-Lite infrastructure, interrupt logic and test registers
in RTL, do not grow a second differently-shaped register bank inside an HLS IP.
Give the HLS block plain wires and let your wrapper own the map:

```c
#pragma HLS INTERFACE ap_vld    port=out       // flat bus + valid strobe
#pragma HLS INTERFACE ap_stable port=roi_x     // plain input wires
#pragma HLS INTERFACE ap_ctrl_hs port=return   // ap_* as top-level ports
```

This is what [example 13](../examples/13_ap_ctrl_latch) does, and it makes the
block **trivially testable in an RTL testbench** — no AXI transactions needed to
configure it or read the result. That test found a real handshake bug that csim
structurally could not ([docs/14 §14.2](14-ap-control-protocols.md#142-the-ap_ctrl_hs-handshake),
rule 2).

Note the struct-to-bus layout is declaration order, little end first:
`{max_val[79:72], min_val[71:64], count[63:32], sum[31:0]}`. Reordering the
struct silently reshuffles the bus — **append only**.

## 15.7 Reset

```tcl
config_rtl -reset control -reset_level low
```

- `control` (default) resets control registers only; `state` also resets
  data-path registers; `all` resets everything including memories.
- Get `-reset_level` wrong and the block never leaves reset, which looks
  exactly like "nothing is driving `ap_start`". Note HLS emits **`ap_rst_n`
  (active low)** for AXI-Stream designs but **`ap_rst` (active high)** for an
  RTL blackbox's expected interface — mixing them up is a genuine and common
  bring-up failure.

## 15.8 Bring-up order

1. **Passthrough first.** Prove video flows through the slot before enabling
   your kernel. Separates "kernel wrong" from "block design wrong".
2. **Check TUSER reaches the frame buffer.** The most common "no video, no
   error" cause ([docs/02 §2.3](02-axi-stream-interfaces.md#23-video-side-channel-conventions)).
3. **Read the counters.** Frames in, frames out, resyncs, errors.
4. **Compare frame checksums** against simulation.
5. **Only then** reach for the ILA.

## 15.9 Checklist

- [ ] Status counters for frames, errors and resyncs; saturating; per-frame
- [ ] A frame checksum on input and output of each block
- [ ] A bypass register
- [ ] A test-pattern register that still consumes its input
- [ ] A debug bus with a runtime selector
- [ ] ILA on stream handshakes at every block boundary
- [ ] Struct outputs documented as append-only
- [ ] Reset polarity confirmed against the generated port name
