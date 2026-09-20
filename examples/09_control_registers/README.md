# 09 — Configuration registers from the PS

How a value gets from an ARM core into a free-running HLS datapath — and the
part that is actually hard, which is not *reading* the register but deciding
*when*.

## Build

```bash
make csim  EX=09_control_registers
make synth EX=09_control_registers
```

## The sampling problem

With `ap_ctrl_none` there is no function-call boundary, so there is no defined
moment at which arguments are "passed". HLS reads each `s_axilite` register
whenever the datapath needs it — possibly mid-frame, and at a different time
for each register.

- **Tearing in time** — three gain writes are three bus transactions; the
  datapath can sample between them and use the new red with the old green.
- **Tearing in space** — a register sampled mid-frame gives you the top of the
  frame processed one way and the bottom the other.

## The fix: a generation counter

```
Software:  write r_gain, g_gain, b_gain  (any order)
           write cfg_seq = cfg_seq + 1    <- the commit
```

```c
if (sof && (!seq_init || cfg_seq != seq_latched)) {
    act_r = r_gain;  act_g = g_gain;  act_b = b_gain;   // atomic
    seq_latched = cfg_seq;  seq_init = true;
}
```

Atomic *and* frame-synchronous, with one mechanism and no extra ports.

**Why a counter and not an "apply" bit?** Clearing an apply bit needs the PL to
write back to an `s_axilite` input register — and those are **read-only from
the hardware side**. A monotonically increasing counter needs no write-back and
no polling for an ack.

## What the testbench pins down

| Test | Catches |
|---|---|
| `first` | Config latching on frame 1 — without `seq_init`, boot-time registers read 0 and the design runs on hard-coded defaults until software first *changes* something |
| `uncommitted` | Shadow registers leaking into the datapath — a gain write with no commit must have **no** effect |
| `committed` | The commit actually applying |
| `bypass` | The deliberately *unlatched* register taking effect immediately |
| `wrap` | `cfg_seq` 0xFFFFFFFF → 0 still committing. `if (cfg_seq > seq_latched)` would wedge config permanently — after ~2.3 years of uptime at 60 Hz |

## Not everything should be frame-synchronous

`bypass` is read directly and takes effect at once — an emergency passthrough
should happen *now*, not next frame. The point is that it's **a choice**. Make
it per register and document it, because your software team cannot infer it
from the register map.

## The generated register map is an ABI

```
<proj>/<solution>/impl/misc/drivers/<top>_v1_0/src/x<top>_hw.h
```

Real output from this example, abridged:

```
// 0x00..0x0c : reserved      <- CTRL/GIE/IER/ISR under ap_ctrl_hs;
//                               with ap_ctrl_none there is no ap_start
// 0x10 : rows      bit 15~0   (Read/Write)
// 0x18 : cols      bit 15~0   (Read/Write)
// 0x20 : r_gain    bit 15~0   (Read/Write)
// 0x38 : cfg_seq   bit 31~0   (Read/Write)
// 0x40 : bypass    bit 0      (Read/Write)
// 0x50 : status    bit 31~0   (Read)       <- struct flattened,
// 0x54 : status    bit 63~32  (Read)          declaration order
// 0x58 : status    bit 95~64  (Read)
```

- Every scalar gets an **8-byte stride** regardless of width — packing flags by
  hand saves nothing.
- A struct is flattened **in declaration order**. Insert a field in the middle
  and every later offset moves, silently breaking a driver you didn't rebuild.
  **Append only.**
- Use the generated driver/header. A hand-written `#define 0x10` addresses the
  wrong field the moment someone adds an argument.

Full treatment, including `ap_none`/`ap_stable`, DDR-resident config and
interrupts: [docs/09](../../docs/09-ps-control-registers.md).
