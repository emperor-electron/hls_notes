# 01 — AXI4-Stream passthrough

The minimum viable streaming block, plus the three interface decisions that
cause the most field failures.

## Build

```bash
make csim  EX=01_axis_passthrough
make synth EX=01_axis_passthrough
```

## What to look at

| Thing | Where | Why it matters |
|---|---|---|
| `#pragma HLS INTERFACE axis` | [src/axis_passthrough.cpp](src/axis_passthrough.cpp) | Direction is *inferred* from use, not declared |
| `ap_ctrl_none port=return` | same | Without it your IP emits one frame and stops |
| Whole-word copy `dst.write(w)` | same | Propagates TLAST/TUSER; dropping TUSER breaks SOF downstream |
| `s_axilite` + `ap_ctrl_none` together | `axis_passthrough_ctrl` | You can have control registers on a free-running block |
| Latching config on TUSER | same | Prevents half-old/half-new frames |

## Expected synthesis result

`II=1`, latency 1–2, essentially zero LUT/FF. If you see `II=2`, you have
accidentally split the read and write across a control step — usually by
putting a `break` or a nested conditional in the loop body.

## The ap_ctrl_none / cosim tension

`cosim_design` cannot run against an unbounded free-running block: co-sim waits
for `ap_done`, which never arrives. Options, in order of preference:

1. Use a **bounded loop** with `ap_ctrl_none` (examples 04+). HLS wraps the
   body in an implicit forever-loop, so it is still free-running in hardware
   but terminates in simulation.
2. Verify with csim only, and check the RTL in a Vivado testbench instead.
3. Temporarily switch to `ap_ctrl_hs` for a cosim run. This changes the
   generated RTL, so it verifies a *different design* than you ship — note it
   in your review.
