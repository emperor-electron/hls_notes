# Vivado / Vitis HLS notes — video & image processing

Working notes, scripts and runnable examples for building image-processing
kernels in HLS for AXI4-Stream video pipelines.

Bias throughout: **free-running AXI4-Stream blocks, `II=1`, no deadlocks, and
everything driven from Tcl rather than the GUI.**

Works with both `vivado_hls` (≤ 2020.1) and `vitis_hls` (2020.1+);
[common/hls_compat.h](common/hls_compat.h) absorbs the differences.

---

## Quick start

```bash
make list                        # show the examples
make csim  EX=03_line_buffer_sobel
make synth EX=03_line_buffer_sobel
make cosim EX=05_split_join_skew
make regress                     # csim + csynth everything, QoR table at the end
```

Different tool or target:

```bash
make synth EX=02_rgb_to_gray HLS=vivado_hls PART=xc7z020clg400-1 PERIOD=6.0
```

Design-space exploration (one process per point — see
[docs/01 §1.6](docs/01-tooling-and-tcl-automation.md#16-design-space-exploration)
for why that matters):

```bash
./scripts/sweep.py --example 03_line_buffer_sobel --period 2.0,2.5,3.0,3.33,4.0
./scripts/sweep.py --example 05_split_join_skew --define SKEW_DEPTH=2,16,64,128 --stage cosim
```

---

## Notes

| | |
|---|---|
| [01 — Tooling and Tcl automation](docs/01-tooling-and-tcl-automation.md) | Scripted builds, report scraping, sweeps, CI |
| [02 — AXI4-Stream interfaces](docs/02-axi-stream-interfaces.md) | Types, side channels, the three ways to be free-running |
| [03 — Backpressure](docs/03-backpressure.md) | Why you don't write any, FIFO depth, rate budgeting |
| [04 — The deadlock playbook](docs/04-deadlock-playbook.md) | Four families, triage table, live debugging |
| [05 — Video pipeline patterns](docs/05-video-pipeline-patterns.md) | Filter skeleton, fixed point, frame state, multi-pixel |
| [06 — Optimisation cookbook](docs/06-optimization-cookbook.md) | II=1, partitioning, operator cost, timing, area |
| [07 — Verification](docs/07-verification.md) | What csim/cosim can and cannot prove; tests that find bugs |
| [08 — Troubleshooting index](docs/08-troubleshooting.md) | Symptom → cause → section |

---

## Examples

Each is self-contained: `src/`, `tb/`, `hls_config.tcl`, `README.md`.

| | Example | Teaches |
|---|---|---|
| 01 | [axis_passthrough](examples/01_axis_passthrough) | Minimum viable stream block; `ap_ctrl_none`; `s_axilite` on a free-running block |
| 02 | [rgb_to_gray](examples/02_rgb_to_gray) | Bounded-loop free-running pattern; fixed-point done right; runtime resolution |
| 03 | [line_buffer_sobel](examples/03_line_buffer_sobel) | **The neighbourhood-filter skeleton.** Line buffers, windows, borders, `II=1` |
| 04 | [dataflow_pipeline](examples/04_dataflow_pipeline) | `DATAFLOW`; the seven canonical-form rules; FIFO depth |
| 05 | [split_join_skew](examples/05_split_join_skew) | **Deadlock family A:** latency skew across a fan-out/join |
| 06 | [rate_change_deadlock](examples/06_rate_change_deadlock) | **Deadlock family B:** data-dependent token counts; three fixes |
| 07 | [frame_resync](examples/07_frame_resync) | Surviving a malformed upstream; the one-beat lookahead; status counters |
| 08 | [axis_to_mem_dma](examples/08_axis_to_mem_dma) | `m_axi` bursts; ping-pong via `DATAFLOW` in a loop; cycles through memory |

---

## The three things worth internalising

**1. You do not write backpressure logic.**
`hls::stream::read()`/`write()` are blocking. HLS builds the stall network from
that, and it propagates upstream automatically. So essentially every
"backpressure bug" is really a deadlock bug or a throughput bug.
→ [docs/03](docs/03-backpressure.md)

**2. A deadlock requires a *cycle* in the blocking graph.**
A 1-in-1-out block cannot deadlock by itself — it can only participate in a
cycle created elsewhere. When something hangs, stop staring at the stalled
block and go find the cycle.
→ [docs/04](docs/04-deadlock-playbook.md)

**3. csim and cosim fail differently, and that's diagnostic.**

| Observation | Diagnosis |
|---|---|
| csim fails `ERROR [HLS SIM]: an hls::stream is read while empty` | **Count** bug |
| csim passes, cosim hangs | **Depth or ordering** bug |
| Both pass, hardware emits one frame then stops | Not a deadlock — `ap_ctrl_hs` with nobody pulsing `ap_start` |
| Runs, but every frame is wrong after one glitch | Not a deadlock — permanent desync |

csim runs dataflow stages sequentially and ignores `depth=` entirely, so a
count mismatch surfaces and a depth problem cannot. Note that *over*-production
(leftover data) is only a warning and exits 0 — `scripts/build.tcl` greps the
csim log so that fails the build too.
→ [docs/07 §7.1](docs/07-verification.md#71-what-each-stage-can-and-cannot-prove)

---

## Layout

```
common/          hls_compat.h (portable AXIS types), video_tb_utils.h
docs/            the notes
examples/NN_*/   src/ tb/ hls_config.tcl README.md
scripts/
  build.tcl      one build script for every example, key=value tclargs
  hls_lib.tcl    logging, arg parsing, csynth XML scraping, timing gate
  sweep.py       DSE driver; records a cosim timeout as HANG, not a crash
Makefile
```

Build output goes to `examples/*/hls_proj/` and `build/`, both gitignored.

## Conventions

- Directives live **in the source as pragmas**, not in `directives.tcl`. They
  travel with the code and survive review.
- `MAX_ROWS`/`MAX_COLS` are compile-time; `rows`/`cols` are runtime `s_axilite`
  registers bounded by them.
- Every `hls::stream` is **named in its constructor** — cosim messages and
  waveform signals use the name.
- Every `STREAM depth=` has a comment naming the scenario that justifies it.
- Regression frames are small and awkward (`20×28`, not `16×16`).
