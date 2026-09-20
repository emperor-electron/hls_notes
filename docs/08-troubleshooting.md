# 8 — Troubleshooting index

Symptom → cause → where to read.

## 8.1 No video at all

| Symptom | Likely cause | See |
|---|---|---|
| One frame appears, then nothing | `ap_ctrl_hs` (the default) with nobody pulsing `ap_start`. **Not a deadlock** — the block is idle | [docs/02 §2.4](02-axi-stream-interfaces.md#24-three-ways-to-be-free-running) |
| Nothing at all, everything looks healthy on a scope | TUSER not propagated — the frame buffer never sees SOF | [docs/02 §2.3](02-axi-stream-interfaces.md#23-video-side-channel-conventions) |
| Downstream DMA never completes a descriptor | TLAST on the last pixel of the *frame* instead of every *line* | [docs/02 §2.3](02-axi-stream-interfaces.md#23-video-side-channel-conventions) |
| Variable-length output stream never completes | No TLAST on the final beat. Deadlock one hop outside your IP | [ex. 07](../examples/07_frame_resync) |
| Pixels silently missing / corrupt | TKEEP left at 0 — subset converters and DMAs drop those bytes | [docs/02 §2.3](02-axi-stream-interfaces.md#23-video-side-channel-conventions) |

## 8.2 Hangs

| Symptom | Cause | See |
|---|---|---|
| csim fails `ERROR [HLS SIM]: an hls::stream is read while empty` | Token-count mismatch | [docs/04 §4.4](04-deadlock-playbook.md#44-family-b--data-dependent-token-counts) |
| csim passes, cosim hangs | FIFO depth or ordering | [docs/04 §4.3](04-deadlock-playbook.md#43-family-a--latency-skew-across-a-fan-outjoin) |
| Works at 640×480, hangs at 1080p | Skew FIFO sized for the test resolution, not `MAX_COLS` | [docs/04 §4.3](04-deadlock-playbook.md#43-family-a--latency-skew-across-a-fan-outjoin) |
| Hangs only under load | Depth bug cosim's ideal sink can't trigger. Need randomised TREADY | [docs/07 §7.5](07-verification.md#75-what-only-an-rtl-testbench-gives-you) |
| `awvalid` high, `awready` low forever | Read+write on one `m_axi` bundle, or a saturated interconnect | [docs/04 §4.6](04-deadlock-playbook.md#46-family-d--cycles-through-memory-or-software) |
| Hangs after adding a second output | Fan-out with no skew FIFO | [ex. 05](../examples/05_split_join_skew) |

## 8.3 Wrong picture

| Symptom | Cause | See |
|---|---|---|
| Every frame wrong after one glitch; power cycle fixes it | Permanent desync in a counted block | [docs/04 §4.7](04-deadlock-playbook.md#47-the-thing-that-looks-like-a-deadlock-and-isnt) |
| Bottom-right corner corrupt, rest fine | Trailing flush iterations don't advance the column counter | [ex. 03](../examples/03_line_buffer_sobel) |
| Top two rows of frame 2+ wrong, frame 1 fine | Stale `static` line buffer leaking through border logic | [docs/07 §7.2](07-verification.md#the-test-cases-that-actually-find-things) |
| Bright rectangle around the whole image | Zero padding instead of replicate | [docs/05 §5.2](05-video-pipeline-patterns.md#border-handling) |
| Strong edges come out black | Magnitude wrapping instead of saturating | [docs/05 §5.3](05-video-pipeline-patterns.md#53-fixed-point-arithmetic) |
| Image imperceptibly dark; white is 254 | Coefficients don't sum to the scale factor | [docs/05 §5.3](05-video-pipeline-patterns.md#53-fixed-point-arithmetic) |
| Gradual darkening through a multi-stage pipeline | Truncation bias; no half-LSB rounding | [docs/05 §5.3](05-video-pipeline-patterns.md#53-fixed-point-arithmetic) |
| Kernel vertically flipped | `hls::LineBuffer` shift direction | [docs/05 §5.8](05-video-pipeline-patterns.md#58-useful-libraries) |
| Every line after the first is garbage in DDR | Writer assumes `stride == cols` | [ex. 08](../examples/08_axis_to_mem_dma) |
| Frame tears | Software races the writer; needs `ap_done` or double-buffering | [ex. 08](../examples/08_axis_to_mem_dma) |
| Half-old / half-new frame after a register write | Config not latched on SOF | [docs/05 §5.5](05-video-pipeline-patterns.md#55-frame-level-state) |

## 8.4 Too slow

| Symptom | Cause | See |
|---|---|---|
| II=2 on a line-buffer kernel | Missing `ARRAY_PARTITION ... dim=1` | [docs/06 §6.3](06-optimization-cookbook.md#63-array_partition) |
| II won't go below N; log names an operator | Runtime divide/modulo, float, or sqrt | [docs/06 §6.4](06-optimization-cookbook.md#64-operator-cost) |
| Latency ~2× expected | Loop nest didn't flatten | [docs/06 §6.6](06-optimization-cookbook.md#66-loop-flattening) |
| Dataflow design is 4× too slow, no error | Non-canonical region, or `HLS 214-107/110/113` on a `DATAFLOW`-in-a-loop counter | [docs/04 §4.5](04-deadlock-playbook.md#45-family-c--canonical-form-violations) |
| DDR bandwidth ~1/16 of expected | Burst inference failed. `grep -E "214-115\|burst .* inferred"` — silence is the failure | [ex. 08](../examples/08_axis_to_mem_dma) |
| Stalls once per line at the memory boundary | `num_write_outstanding=1` — serialising on DDR round-trip | [docs/03 §3.7](03-backpressure.md#37-backpressure-at-the-memory-boundary-is-different) |
| Can't reach 4Kp60 at II=1 | 594 MHz isn't achievable. Widen to 2 or 4 px/beat | [docs/05 §5.6](05-video-pipeline-patterns.md#56-multi-pixel-per-beat) |
| Area explosion after adding `PIPELINE` | Pipelined an *outer* loop → inner loop fully unrolled | [docs/06 §6.5](06-optimization-cookbook.md#65-pipeline-vs-unroll-vs-dataflow) |

## 8.5 Timing

| Symptom | Cause | See |
|---|---|---|
| csynth estimate misses target | Over-constrain; HLS estimates are optimistic | [docs/06 §6.8](06-optimization-cookbook.md#68-timing-closure) |
| csynth fine, Vivado WNS fails on `*_TREADY` | Combinational handshake path across a long route | `register both` — [docs/02 §2.2](02-axi-stream-interfaces.md#register-slices) |
| 4 DSPs where you expected 1 | Operand exceeded 25×18 | [docs/06 §6.4](06-optimization-cookbook.md#64-operator-cost) |
| 2× the expected BRAM | `MAX_COLS` set higher than you actually support | [docs/06 §6.9](06-optimization-cookbook.md#69-area) |

## 8.6 Tooling

| Symptom | Cause | See |
|---|---|---|
| Every sweep point returns identical numbers | Looped solutions in one tool invocation; C parse cached | [docs/01 §1.6](01-tooling-and-tcl-automation.md#16-design-space-exploration) |
| CI job times out with no output | Missing `exit` at the end of the Tcl script | [docs/01 §1.3](01-tooling-and-tcl-automation.md#13-the-minimum-viable-build-script) |
| Wall of STL/iostream errors from csynth | TB added with `add_files` instead of `add_files -tb` | [docs/01 §1.3](01-tooling-and-tcl-automation.md#13-the-minimum-viable-build-script) |
| csim passes, csynth fails on the same file | `__SYNTHESIS__` is defined only for csynth — check your `#if`s | [docs/01 §1.10](01-tooling-and-tcl-automation.md#110-things-that-will-waste-your-afternoon) |
| GUI and script give different results | Leftover directives in the GUI's solution — always `open_solution -reset` | [docs/01 §1.2](01-tooling-and-tcl-automation.md#12-why-scripts-not-the-gui) |
| Directives "don't apply" | Label in `directives.tcl` no longer matches a source loop label | [docs/01 §1.2](01-tooling-and-tcl-automation.md#12-why-scripts-not-the-gui) |
| cosim out-of-bounds abort on an `m_axi` design | `depth=` doesn't cover the TB's whole buffer. Not a design bug | [docs/07 §7.4](07-verification.md#74-cosim) |
| `RESOURCE`/`BIND_STORAGE` unknown pragma | Tool-version split at ~2020.1 | [docs/01 §1.1](01-tooling-and-tcl-automation.md#11-the-two-binaries) |

## 8.7 The four questions to ask first

1. **Does csim fail with `read while empty`?** That is a count bug, not a
   depth bug. Over-production only *warns*, so read the log too.
2. **Is it actually hanging, or is it idle?** `ap_idle` high means nobody is
   pulsing `ap_start` — a different problem entirely.
3. **Which direction does the stall point?** `tvalid=1, tready=0` → downstream.
   `tvalid=0, tready=1` → upstream. Walk until it loops.
4. **Does it depend on resolution?** If it works small and hangs large, it's a
   compile-time depth against a runtime skew.
