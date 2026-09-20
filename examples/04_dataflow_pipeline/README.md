# 04 — DATAFLOW: concurrent multi-stage pipeline

```
src ─[ingest]─ s0 ─[blur3x3]─ s1 ─[threshold]─ s2 ─[egress]─ dst
```

Without `#pragma HLS DATAFLOW` these four stages run **sequentially**: stage 2
does not start until stage 1 has finished the whole frame, so you need a frame
buffer between every stage and your latency is measured in frames. With it they
run concurrently, connected by FIFOs, and latency is a few lines.

## Build

```bash
make csim  EX=04_dataflow_pipeline
make cosim EX=04_dataflow_pipeline   # the one that can actually find a deadlock
```

## Canonical form — the seven rules

Written out in full at the bottom of [src/pipeline.cpp](src/pipeline.cpp).
Summary:

1. **Single producer, single consumer** per channel.
2. **No bypass** — no channel that skips a stage.
3. **No feedback** — close loops outside the region.
4. **Stage calls only** between the channel declarations and the end.
5. **Every stage runs every time** — no `if (cond) stage_b(...)`.
6. **Token counts must match** — writes to a channel == reads from it.
7. **No static shared between stages.**

Turn violations into build failures:

```tcl
config_dataflow -strict_mode error
```

Without this, HLS *silently declines* to apply DATAFLOW when the form is
non-canonical. You get a correct design that is 4× too slow and no error.

## Strip the AXI side channels at the edges

Internal channels carry a bare `ap_uint<8>`, not an `ap_axiu`. Inside a
dataflow region you are on an HLS FIFO (data/valid/ready), so TKEEP/TSTRB/TID/
TDEST are pure waste. At depth 2048 that is 1 BRAM instead of 4.

## FIFO depth

Default is 2, which is correct for a chain of rate-matched stages. Go deeper
only when you know what is backing up:

| Situation | Depth needed |
|---|---|
| 1-in-1-out chain | 2 (default) |
| Bursty producer | ≥ burst length |
| Fan-out rejoining after different latency | ≥ latency **skew** — see [example 05](../05_split_join_skew) |

Sizing FIFOs by "what *could* back up" rather than "what *does*" is how you
spend all your BRAM on nothing.

## csim cannot find dataflow deadlocks

csim is plain C++ executed sequentially — there is no concurrency, so every
channel behaves like an unbounded queue. **A design that deadlocks in hardware
passes csim.** Only `cosim_design` models the real FIFO depths and the real
concurrency. Green csim + hanging cosim is the normal failure mode here.
