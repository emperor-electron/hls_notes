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

## 8.3b Configuration

| Symptom | Cause | See |
|---|---|---|
| One frame has a colour cast after a config change | Multi-word config torn across bus transactions | [docs/09 §9.4](09-ps-control-registers.md#94-the-sampling-problem-and-the-fix) |
| Top of frame uses old setting, bottom uses new | Register sampled mid-frame; latch on SOF | [docs/09 §9.4](09-ps-control-registers.md#94-the-sampling-problem-and-the-fix) |
| First frame(s) after boot use wrong settings | No `seq_init`; counter starts equal to the reset value | [ex. 09](../examples/09_control_registers) |
| Config stops applying after very long uptime | `if (seq > latched)` instead of `!=`; counter wrapped | [ex. 09](../examples/09_control_registers) |
| Driver writes the wrong field after a code change | Hand-written offsets; struct field inserted mid-list | [docs/09 §9.3](09-ps-control-registers.md#93-the-generated-register-map-is-the-abi) |
| No interrupt on a free-running block | `ap_ctrl_none` has no `ap_done`; poll a status register | [docs/09 §9.8](09-ps-control-registers.md#98-interrupts) |

## 8.3c Custom RTL, fixed point and ap_* control

| Symptom | Cause | See |
|---|---|---|
| `Cannot find blackbox RTL port 'logic'` (once per port) | Blackbox port list uses SystemVerilog types; HLS parses V2001 | [docs/13 §13.3](13-custom-rtl-integration.md#133-the-rtl-blackbox-concretely) |
| `No 'module_clock_enable' in 'rtl_common_signal'` | Blackbox RTL must expose an `ap_ce` port | [docs/13 §13.3](13-custom-rtl-integration.md#133-the-rtl-blackbox-concretely) |
| `Cannot find blackbox RTL port 'scl_o'` | A blackbox cannot own top-level I/O — use a sibling IP | [docs/13 §13.5](13-custom-rtl-integration.md#135-the-sibling-ip-pattern-what-to-use-for-ic) |
| `[HLS 200-646] RTL file ... does not exist` | JSON paths resolve against the CWD, not the JSON | [docs/13 §13.3](13-custom-rtl-integration.md#133-the-rtl-blackbox-concretely) |
| Undefined reference to the blackbox function at csim link | JSON's `c_file` path did not resolve; `cflag` had no includes | [docs/13 §13.3](13-custom-rtl-integration.md#133-the-rtl-blackbox-concretely) |
| `[XSIM 43-4099] ... doesn't have a timescale` | Custom RTL is missing `` `timescale `` | [docs/13 §13.3](13-custom-rtl-integration.md#133-the-rtl-blackbox-concretely) |
| Blackbox cosim hangs at `0 / N transactions` | `"II":"1"` declared on a non-pipelined blackbox (II must equal latency), or a constant argument was folded away and shifted the port map | [docs/13 §13.3](13-custom-rtl-integration.md#rtl_performance-is-a-contract-not-a-comment) |
| Blackbox cosim reports pixel mismatches | The C model and the RTL disagree — write a standalone RTL unit test | [docs/13 §13.4](13-custom-rtl-integration.md#write-a-standalone-rtl-unit-test-first) |
| `[COSIM 212-345] Cosim only supports ... 'ap_ctrl_none'` | Sequential tail on a free-running block; use `ap_ctrl_hs` | [docs/13 §13.3](13-custom-rtl-integration.md#133-the-rtl-blackbox-concretely) |
| Block completes once and never restarts | Controller missed coincident `ap_ready`/`ap_done` | [docs/14 §14.2](14-ap-control-protocols.md#142-the-ap_ctrl_hs-handshake) |
| Block "sometimes doesn't start" | `ap_start` pulsed instead of held until `ap_ready` | [docs/14 §14.2](14-ap-control-protocols.md#142-the-ap_ctrl_hs-handshake) |
| `ap_done` stuck high | `ap_ctrl_chain` with `ap_continue` undriven | [docs/14 §14.3](14-ap-control-protocols.md#143-ap_ctrl_chain-and-ap_continue) |
| Every other trigger ignored | `go` pulse dropped while busy | [ex. 13](../examples/13_ap_ctrl_latch) |
| Occasional wrong result, not reproducible | `ap_stable` contract violated — config changed mid-run | [docs/14 §14.4](14-ap-control-protocols.md#144-latching-ps-registers-into-ap_stable-inputs) |
| Image washed out but plausible | A pixel cast into a too-narrow `ap_fixed` coefficient type | [docs/12 §12.3](12-fixed-point.md#123-bit-growth-and-where-precision-is-actually-lost) |
| Bright pixels turn black | `AP_WRAP` (the default) instead of `AP_SAT` | [docs/12 §12.1](12-fixed-point.md#121-the-type) |
| Hardware differs from the model by fractions of an LSB | A narrow named intermediate is an unintended quantisation point | [docs/12 §12.3](12-fixed-point.md#123-bit-growth-and-where-precision-is-actually-lost) |
| Debug signal "optimised away" | It is not an output, so HLS deleted it | [docs/15 §15.3](15-design-for-test.md#153-tier-2--debug-wires-and-an-ila) |

## 8.3d General C/C++ and compute kernels

| Symptom | Cause | See |
|---|---|---|
| `malloc`/`new`/STL/recursion won't synthesise | Not synthesisable — but fine in the **testbench** | [docs/16 §16.2](16-writing-good-hls-code.md#162-what-does-not-synthesise) |
| Far more LUTs/DSPs than expected | `int` everywhere instead of sized types | [docs/16 §16.3](16-writing-good-hls-code.md#163-types) |
| One multiply costs 4 DSPs | Operand exceeded the DSP's native 27×18 | [ex. 14](../examples/14_fir_filter) |
| Reduction loop stuck at II=N | Single accumulator; split into partial sums | [ex. 15](../examples/15_matmul_tiled) |
| Tiled kernel won't hit target II | A and B partitioned on the same dimension — they need opposite ones | [ex. 15](../examples/15_matmul_tiled) |
| Memory-bound compute kernel | Strided access can't burst; tile it | [ex. 15](../examples/15_matmul_tiled) |
| Filter output is saturated but "nearly right" | `=` used where `.range()` was meant at a bus edge | [ex. 14](../examples/14_fir_filter) |
| Filter produces silence | Unsupported config fell through to an all-zero table | [docs/16 §16.9](16-writing-good-hls-code.md#169-style-that-pays-off) |
| `hist[v]++` blamed for II | On 2023.2 it is II=1 already; check **timing**, not II | [ex. 16](../examples/16_histogram_dependence) |
| Counts lost at runtime but csim passes | `DEPENDENCE inter false` asserted over a real dependency | [ex. 16](../examples/16_histogram_dependence) |
| csim and csynth disagree, source looks identical | Logic hidden inside `#ifndef __SYNTHESIS__` | [docs/16 §16.8](16-writing-good-hls-code.md#168-the-__synthesis__-asymmetry) |
| Loop shows as `VITIS_LOOP_144_5` in reports | Loop not labelled | [docs/16 §16.9](16-writing-good-hls-code.md#169-style-that-pays-off) |

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
| 2× the expected BRAM | `MAX_COLS` set higher than you actually support | [docs/11 §11.7](11-memory-resources.md#117-other-common-waste) |
| URAM count == number of line buffers, each ~5% full | Unpacked line buffers bound to URAM | [docs/11 §11.4](11-memory-resources.md#114-the-rounding-loss-nobody-budgets-for) |
| Surprise BRAM in the report | A local array became a memory — check the `Memory` row | [docs/11 §11.7](11-memory-resources.md#117-other-common-waste) |
| Storage type changed after a tool upgrade | Relying on `impl=auto` | [docs/11 §11.3](11-memory-resources.md#113-binding) |

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
