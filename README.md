# Vitis / Vivado HLS notes

Working notes, scripts and runnable examples for writing HLS that synthesises
into hardware you'd be willing to ship.

Bias throughout: **`II=1` streaming designs, no deadlocks, explicit resource
decisions, and everything driven from Tcl rather than the GUI.**

Every claim in here is measured on the tool, not recalled. Examples are
verified end to end: **16 examples pass `make regress`** (csim + csynth, II=1
throughout), plus two SystemVerilog testbenches in xsim and a passing RTL
blackbox cosim.

Works with `vivado_hls` (≤2020.1) and `vitis_hls` (2020.1+);
[common/hls_compat.h](common/hls_compat.h) absorbs the differences. Measurements
are on `xczu7ev-ffvc1156-2-e` / Vitis HLS 2023.2 unless stated.

---

## Quick start

```bash
make list                        # show the examples
make csim  EX=14_fir_filter
make synth EX=15_matmul_tiled
make cosim EX=05_split_join_skew
make regress                     # csim + csynth everything, QoR table at the end
make sim-sv EX=13_ap_ctrl_latch  # SystemVerilog testbench in xsim
```

Different tool or target:

```bash
make synth EX=02_rgb_to_gray HLS=vivado_hls PART=xc7z020clg400-1 PERIOD=6.0
```

Design-space exploration — one process per point, which
[matters](docs/01-tooling-and-tcl-automation.md#16-design-space-exploration):

```bash
./scripts/sweep.py --example 14_fir_filter --define NTAPS=15,31 --define SYMMETRIC=0,1
./scripts/sweep.py --example 15_matmul_tiled --define UNROLL_K=1,2,4,8
./scripts/sweep.py --example 05_split_join_skew --define SKEW_DEPTH=2,16,64 --stage cosim
```

---

## Notes

### Core — applies to any HLS design

| | |
|---|---|
| [01 — Tooling and Tcl automation](docs/01-tooling-and-tcl-automation.md) | Scripted builds, report scraping, sweeps, CI |
| [16 — Writing good HLS C/C++](docs/16-writing-good-hls-code.md) | **Start here if you're new.** What synthesises, types, loops, functions, style |
| [02 — AXI4-Stream interfaces](docs/02-axi-stream-interfaces.md) | Types, side channels, the three ways to be free-running |
| [03 — Backpressure](docs/03-backpressure.md) | Why you don't write any, FIFO depth, rate budgeting |
| [04 — The deadlock playbook](docs/04-deadlock-playbook.md) | Four families, triage table, live debugging |
| [06 — Optimisation cookbook](docs/06-optimization-cookbook.md) | II=1, partitioning, operator cost, timing, area |
| [07 — Verification](docs/07-verification.md) | What csim/cosim can and cannot prove; tests that find bugs |
| [11 — BRAM and URAM efficiency](docs/11-memory-resources.md) | Shape vs binding vs ports; why URAM line buffers waste 95% |
| [12 — Fixed point & quantisation](docs/12-fixed-point.md) | `ap_fixed`, rounding-mode truth table, matching a reference model |
| [08 — Troubleshooting index](docs/08-troubleshooting.md) | Symptom → cause → section |

### Integration — talking to the PS, to RTL, and to a debugger

| | |
|---|---|
| [09 — PS control registers](docs/09-ps-control-registers.md) | `s_axilite`, the register-map ABI, atomic frame-synchronous config |
| [13 — Custom SystemVerilog](docs/13-custom-rtl-integration.md) | RTL blackbox vs sibling IP; why an I²C core cannot be a blackbox |
| [14 — `ap_*` control protocols](docs/14-ap-control-protocols.md) | `ap_ctrl_hs`/`chain`, latching PS registers into `ap_stable` |
| [15 — Design for test](docs/15-design-for-test.md) | Counters, frame checksums, debug buses, ILA strategy |

### Domain — video and image processing

| | |
|---|---|
| [05 — Video pipeline patterns](docs/05-video-pipeline-patterns.md) | Pipeline shape, fixed point, frame state, multi-pixel |
| [10 — Line buffers & sliding windows](docs/10-line-buffers-and-sliding-windows.md) | The universal skeleton, borders, separable/running-sum kernels |

---

## Examples

Each is self-contained: `src/`, `tb/`, `hls_config.tcl`, `README.md`.

### General HLS techniques

| | Example | Teaches |
|---|---|---|
| 14 | [fir_filter](examples/14_fir_filter) | DSP sizing, symmetry folding (**26→14 DSP**), constant ROMs, `.range()` vs `=` |
| 15 | [matmul_tiled](examples/15_matmul_tiled) | Loop tiling, opposite-dimension partitioning, reductions (**II 8→1**), multi-bundle `m_axi` |
| 16 | [histogram_dependence](examples/16_histogram_dependence) | Read-modify-write dependence; **the folklore is out of date** — measure first |
| 11 | [fixed_point_quant](examples/11_fixed_point_quant) | `ap_fixed` quantiser characterisation; bit-exact vs error-budget |
| 12 | [rtl_blackbox](examples/12_rtl_blackbox) | Hand-written SystemVerilog called from HLS, verified in cosim |
| 13 | [ap_ctrl_latch](examples/13_ap_ctrl_latch) | SV controller latching PS regs into `ap_stable`; verified in xsim |
| 10 | [storage_binding](examples/10_storage_binding) | **Sweepable** BRAM/URAM/LUTRAM; packing cuts URAM 14→2 |
| 09 | [control_registers](examples/09_control_registers) | Atomic config via a generation counter; register-map ABI |

### Streaming, dataflow and deadlocks

| | Example | Teaches |
|---|---|---|
| 01 | [axis_passthrough](examples/01_axis_passthrough) | Minimum viable stream block; `ap_ctrl_none`; `s_axilite` on a free-running block |
| 04 | [dataflow_pipeline](examples/04_dataflow_pipeline) | `DATAFLOW`; the seven canonical-form rules; FIFO depth |
| 05 | [split_join_skew](examples/05_split_join_skew) | **Deadlock family A:** latency skew across a fan-out/join |
| 06 | [rate_change_deadlock](examples/06_rate_change_deadlock) | **Deadlock family B:** data-dependent token counts; three fixes |
| 08 | [axis_to_mem_dma](examples/08_axis_to_mem_dma) | `m_axi` bursts; ping-pong via `DATAFLOW` in a loop |

### Video and image processing

| | Example | Teaches |
|---|---|---|
| 02 | [rgb_to_gray](examples/02_rgb_to_gray) | Bounded-loop free-running pattern; fixed point; runtime resolution |
| 03 | [line_buffer_sobel](examples/03_line_buffer_sobel) | **The neighbourhood-filter skeleton.** Line buffers, windows, borders |
| 07 | [frame_resync](examples/07_frame_resync) | Surviving a malformed upstream; one-beat lookahead; status counters |

---

## Measured results that contradict common advice

| Folklore | Measured on Vitis HLS 2023.2 |
|---|---|
| "`hist[v]++` is II=2 — add `DEPENDENCE`" | II=1 with no help; the real cost is **timing** ([ex. 16](examples/16_histogram_dependence)) |
| "Force URAM to save BRAM" | Unpacked line buffers are **5% utilised** — 14 URAM for 210 Kb ([docs/11 §11.4](docs/11-memory-resources.md#114-the-rounding-loss-nobody-budgets-for)) |
| "A blackbox's `II` field is advisory" | `"II":"1"` on a sequential block **hangs cosim forever** ([docs/13](docs/13-custom-rtl-integration.md)) |
| "Status registers are free" | Publishing per-pixel cost **3.501 ns vs 2.697 ns** ([docs/06 §6.8](docs/06-optimization-cookbook.md#68-timing-closure)) |
| "Unroll more for speed" | Packing line buffers into *width* cut URAM **14→2** *and* halved FF/LUT ([docs/11 §11.5](docs/11-memory-resources.md#115-pack-lines-into-width-not-into-count)) |

---

## The three things worth internalising

**1. You do not write backpressure logic.**
`hls::stream::read()`/`write()` are blocking, and HLS builds the stall network
from that. So essentially every "backpressure bug" is really a deadlock bug or
a throughput bug. → [docs/03](docs/03-backpressure.md)

**2. A deadlock requires a *cycle* in the blocking graph.**
A 1-in-1-out block cannot deadlock by itself — it can only participate in a
cycle created elsewhere. When something hangs, stop staring at the stalled
block and go find the cycle. → [docs/04](docs/04-deadlock-playbook.md)

**3. csim and cosim fail differently, and that's diagnostic.**

| Observation | Diagnosis |
|---|---|
| csim fails `ERROR [HLS SIM]: an hls::stream is read while empty` | **Count** bug |
| csim passes, cosim hangs | **Depth or ordering** bug |
| Both pass, hardware emits one frame then stops | Not a deadlock — `ap_ctrl_hs` with nobody pulsing `ap_start` |
| Runs, but every frame wrong after one glitch | Not a deadlock — permanent desync |

csim runs dataflow stages sequentially and ignores `depth=` entirely. Note
*over*-production (leftover data) is only a warning and exits 0 —
`scripts/build.tcl` greps the csim log so that fails the build too.
→ [docs/07 §7.1](docs/07-verification.md#71-what-each-stage-can-and-cannot-prove)

---

## Layout

```
common/          hls_compat.h (portable AXIS types), video_tb_utils.h
docs/            the notes
examples/NN_*/   src/ tb/ rtl/ hls_config.tcl README.md
scripts/
  build.tcl      one build script for every example, key=value tclargs
  hls_lib.tcl    logging, arg parsing, report scraping, II + timing + csim gates
  sweep.py       DSE driver; records a cosim timeout as HANG, not a crash
Makefile
```

Build output goes to `examples/*/hls_proj/`, `examples/*/xsim_run/` and
`build/`, all gitignored.

## Conventions

- Directives live **in the source as pragmas**, with a comment saying what
  breaks without them.
- `MAX_*` are compile-time; runtime sizes are `s_axilite` registers bounded by
  them.
- Every `hls::stream` is **named in its constructor** — cosim and waveforms use it.
- Every design decision gets **one named compile-time knob** so it can be swept.
- Unsupported configurations are an `#error`, never a silent degenerate case.
- Regression inputs are small and awkward (`19×17`, not `16×16`).
