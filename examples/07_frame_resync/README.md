# 07 — Frame resynchronisation: surviving a malformed upstream

Examples 05 and 06 were about deadlocks *inside* your IP. This one is about the
failure mode that looks like a deadlock from outside but isn't: **permanent
desynchronisation.**

Every counted block (examples 02–06) assumes each frame is exactly `rows*cols`
beats. If an upstream source ever delivers a short frame — sensor glitch, mode
change, DMA underrun, cable reconnect — a counted block doesn't hang and
doesn't error. It silently consumes the first N lines of frame *K+1* as the
last N lines of frame *K*, and **every frame after that is wrong, forever.**

That is worse than a deadlock. A deadlock at least stops and tells you.

## Build

```bash
make csim EX=07_frame_resync
```

## The one-beat output lookahead

The trick that makes graceful recovery possible:

```c
if (have_pending) dst.write(pending);
pending = o;
have_pending = true;
```

Holding the newest output beat back by one cycle lets you go back and assert
`TLAST` on it **after the fact**, once you discover the frame ended early.
Without it, an early SOF leaves the downstream consumer parked mid-packet
waiting for a TLAST that never comes — a real deadlock, one hop outside your
module. This is why "my IP is fine, the DMA is broken" tickets exist.

Cost: one beat of latency, ~30 flip-flops.

## Why this block cannot deadlock

At most one blocking read and one blocking write per iteration, in that order,
with nothing else depending on it:

- upstream stalls → we stall on read → we assert backpressure. Fine.
- downstream stalls → we stall on write → we stop reading. Fine.

**A deadlock needs a *cycle* in the blocking graph.** A straight-line
1-in-1-out block cannot form one by itself; it can only participate in a cycle
created elsewhere by a fan-out that rejoins (example 05).

## Status counters tell you which bug you have

| `resyncs` | `eol_mismatch` | Diagnosis |
|---|---|---|
| 0 | 0 | Healthy |
| climbing | ~0 | Upstream is truncating frames — glitch, underrun, cable |
| ~0 | climbing | `cols` register disagrees with the actual line length — **config bug** |
| climbing | climbing | Resolution changed and nobody told the register bank |

Counters saturate rather than wrap. A wrapped counter reads identically to "no
errors", which is the worst possible failure mode for a diagnostic.

## What this block deliberately does *not* do

- **No timeout on a blocking read.** You cannot express it in HLS and you
  shouldn't want to — a stalled read is correct backpressure, not a fault. If
  you need a watchdog, put it in a separate always-running block or in PL
  logic, counting cycles since the last TVALID.
- **No line-length discovery.** Learning `cols` from the first TLAST makes
  output geometry depend on input data, so downstream line buffers (sized at
  compile time) could overflow. Take geometry from a register; use side
  channels only to align to it.
- **No dropping of partial frames.** Buffering a whole frame to decide whether
  to emit it costs a frame of latency and a frame buffer. Do that at the frame
  buffer (example 08), not here.
