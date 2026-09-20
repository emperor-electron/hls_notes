# 13 — `ap_ctrl_hs`, `ap_stable`, and latching PS registers in RTL

An HLS measurement block driven by a hand-written SystemVerilog controller that
**holds its `ap_stable` inputs stable for the duration of each call** — verified
in xsim against the real generated RTL.

## Build and run

```bash
make csim   EX=13_ap_ctrl_latch    # arithmetic only
make sim-sv EX=13_ap_ctrl_latch    # the RTL testbench that actually proves it
```

## The structure

```
PS --AXI-Lite--> [shadow regs] --+
                                 |  rtl/hls_cfg_latch.sv
                     +-----------+------------+
                     | sample ALL config on   |
                     | the cycle ap_start is  |
                     | asserted               |
                     +-----------+------------+
                                 |  cfg_* wires (held constant)
                                 v
                        +------------------+
              ap_start->|  HLS roi_stats   |->ap_done
                        +------------------+
```

`ap_stable` produces a plain input wire and **promises the scheduler the value
will not change while the block runs**, so it may be read once and fanned out.
Violate that — let the PS rewrite it mid-run — and part of the datapath sees the
old value and part the new. No error, no warning, no reproducible pattern.

The controller makes the promise true: the PS writes shadow registers whenever
it likes, and the controller copies them to the kernel's wires **exactly once
per call, on the cycle it launches the call**.

## What the RTL testbench proves

csim structurally cannot test this — in C there is no such thing as an argument
changing during a call.

```
=== TEST 1: baseline run ===
  [baseline]      ok   sum=6696 count=48 min=76 max=203
=== TEST 2: PS rewrites shadow regs MID-RUN ===
    (PS rewrote ROI to full-frame at t=8295000)
    (PS rewrote ROI to 1x1 at t=8595000)
  [held-stable]   ok   sum=6696 count=48 min=76 max=203   <-- unchanged
=== TEST 3: next run picks up the new config ===
  [new-config]    ok   sum=40768 count=384 min=0 max=255
=== TEST 4: go pulse while busy is remembered ===
  [run-B-pending] ok   sum=1400 count=16 min=50 max=125
*** PASS
```

Test 2 is the point: a hostile PS scribbles over the shadow registers twice,
mid-frame, and the result is bit-identical to the baseline.

## The bug this found

Writing the controller, I used the obvious FSM: assert `ap_start`, wait for
`ap_ready`, then wait for `ap_done`. **It deadlocked.**

On a **non-pipelined** `ap_ctrl_hs` block, `ap_ready` and `ap_done` assert on
the *same cycle* — the block consumes its inputs and finishes together. So
`S_LAUNCH` consumed `ap_ready` and `S_RUN` then waited forever for an `ap_done`
that had already gone past. The first run completed correctly and the block
never started again, which reads like a software bug and is not.

```systemverilog
S_LAUNCH: if (ap_ready) begin
    ap_start <= 1'b0;
    if (ap_done) begin run_count <= run_count + 1; state <= S_IDLE; end
    else                                            state <= S_RUN;   // pipelined only
end
```

Only a *pipelined* block has `ap_ready` strictly before `ap_done`.

## Other handshake rules the controller obeys

- **`ap_start` is a LEVEL, not a pulse** — hold it until `ap_ready`. A one-cycle
  pulse only works if `ap_ready` happened to be high that cycle. Symptom:
  "the block sometimes doesn't start."
- **Launch only from `ap_idle`** — not from "I think it finished".
- **A `go` pulse arriving while busy is remembered**, not dropped. Dropping it
  is how you get "every other trigger is ignored", which gets blamed on software.

## Why the results come out as wires

```c
#pragma HLS INTERFACE ap_vld     port=out       // flat bus + valid strobe
#pragma HLS INTERFACE ap_stable  port=roi_x     // plain input wires
#pragma HLS INTERFACE ap_ctrl_hs port=return    // ap_* as top-level ports
```

A deliberate choice: **the SystemVerilog wrapper owns the register map**, and
HLS is pure datapath. Common when you already have AXI-Lite infrastructure and
do not want a second, differently shaped register bank inside an HLS IP — and
it makes the block trivially testable in an RTL testbench, with no AXI
transactions needed to configure it or read the answer.

The struct flattens in **declaration order, little end first**:
`{max_val[79:72], min_val[71:64], count[63:32], sum[31:0]}`. Reordering the
struct silently reshuffles the bus — **append only**.

Full treatment: [docs/14](../../docs/14-ap-control-protocols.md) ·
[docs/15](../../docs/15-design-for-test.md).
