# 02 — RGB → grayscale, and the bounded-loop free-running pattern

## Build

```bash
make csim  EX=02_rgb_to_gray
make cosim EX=02_rgb_to_gray
```

## The pattern that matters

```c
#pragma HLS INTERFACE ap_ctrl_none port=return
for (y = 0; y < rows; ++y)
  for (x = 0; x < cols; ++x) {
#pragma HLS PIPELINE II=1
    ...
  }
```

Under `ap_ctrl_none` HLS wraps the body in an implicit infinite loop, so this
is **still free-running hardware** — but it terminates in csim and it
co-simulates. Prefer it over `while(true)` unless you specifically need
data-driven frame lengths.

## Fixed-point checklist

- Scale to a power of two (Q16 here) so the divide is a shift.
- Make the coefficients **sum exactly to the scale factor**, or white will not
  map to white.
- Add half-LSB before the shift; truncation bias compounds through a pipeline.
- Size the accumulator explicitly (`ap_uint<27>`), don't inherit `int`.

## Regenerate vs. copy side channels

This block *regenerates* TUSER/TLAST from the loop counters instead of copying
them from the input. That makes it robust to a malformed upstream (a bad frame
becomes a visible artefact, not a downstream hang) at the cost of hard-coding
"every frame is exactly rows×cols". Example 07 shows the other trade.

## Sweep it

```bash
./scripts/sweep.py --example 02_rgb_to_gray --period 2.0,2.5,3.0,3.33,4.0
```
