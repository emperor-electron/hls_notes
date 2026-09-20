# 14 — The `ap_*` block-level protocols

Worked example: [examples/13_ap_ctrl_latch](../examples/13_ap_ctrl_latch)
(verified in xsim against the real HLS-generated RTL)

## 14.1 The four protocols

| Protocol | Ports added to `return` | Use when |
|---|---|---|
| `ap_ctrl_none` | none | Free-running video block ([docs/02 §2.4](02-axi-stream-interfaces.md#24-three-ways-to-be-free-running)) |
| `ap_ctrl_hs` *(default)* | `ap_start, ap_done, ap_idle, ap_ready` | One call per frame/transaction |
| `ap_ctrl_chain` | the above **+ `ap_continue`** | Chained blocks; back-pressure the *completion* |
| `s_axilite` on `return` | the above as **register bits** + interrupt | Software drives it |

`#pragma HLS INTERFACE ap_ctrl_hs port=return` with **no** `s_axilite` exposes
`ap_start`/`ap_done`/`ap_idle`/`ap_ready` as **top-level wires**, which is what
lets RTL drive the block directly with no processor in the loop.

## 14.2 The `ap_ctrl_hs` handshake

```
        ┌───────────────────────────────────────────┐
ap_idle ────┐                                   ┌───────
            └───────────────────────────────────┘
ap_start ───┌───────────────────────────┐
        ────┘                           └───────────────
                                        ↑ deassert once ap_ready seen
ap_ready ───────────────────────────────┌─┐
        ────────────────────────────────┘ └─────────────
ap_done  ───────────────────────────────┌─┐
        ────────────────────────────────┘ └─────────────
```

Rules, in order of how often they are broken:

1. **`ap_start` is a LEVEL, not a pulse.** Hold it until you see `ap_ready`.
   A one-cycle pulse only works if `ap_ready` happened to be high that cycle.
   Symptom of getting it wrong: "the block sometimes doesn't start."
2. **`ap_ready` and `ap_done` assert on the SAME cycle for a non-pipelined
   block.** This one cost me a debug cycle while writing the example: a
   two-state controller that consumes `ap_ready` in one state and then waits
   for `ap_done` in the next **deadlocks** — the `ap_done` it is waiting for
   already went past. The first run completes correctly and the block never
   starts again, which reads like a software bug and is not.
   Only a *pipelined* block has `ap_ready` strictly before `ap_done`.
3. **`ap_idle` is the safe moment to change anything.** Launch from `ap_idle`,
   not from "I think it finished".
4. **`ap_done` is one cycle** (unless you use `ap_ctrl_chain`). If software
   polls, poll the `s_axilite` `CTRL` register's done bit, which is sticky and
   clear-on-read — not the wire.

### Auto-restart

With `s_axilite` on `return`, bit 7 of `CTRL` (`0x00`) is auto-restart: the
block relaunches itself on completion without software re-pulsing `ap_start`.
Convenient, and it means **your configuration registers are re-sampled at a
moment you do not control** — see §14.4.

## 14.3 `ap_ctrl_chain` and `ap_continue`

`ap_ctrl_chain` adds `ap_continue`: the block holds `ap_done` until the
consumer says it has taken the result. That is what lets HLS chain blocks
without a FIFO between them, and it is the protocol an
[RTL blackbox](13-custom-rtl-integration.md#133-the-rtl-blackbox-concretely)
must implement.

If you tie `ap_continue` high you get `ap_ctrl_hs` behaviour. If you forget to
drive it at all, the block completes once and hangs — with `ap_done` stuck
high, which is a distinctive and easy-to-spot signature on an ILA.

## 14.4 Latching PS registers into `ap_stable` inputs

This is the pattern the example exists for.

### The problem

`ap_stable` produces a plain input wire and **promises the scheduler the value
will not change while the block runs**, so it may be read once and fanned out
rather than re-read at each point of use. Violate the promise — let the PS
rewrite it mid-run — and part of the datapath sees the old value and part the
new. No error, no warning, no reproducible pattern.

### The structure

```
PS --AXI-Lite--> [shadow regs] --+
                                 |  hls_cfg_latch.sv
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

The PS writes shadow registers whenever it likes. The controller copies them to
the kernel's wires **exactly once per call, on the cycle it launches the call.**

### The controller

Full source: [rtl/hls_cfg_latch.sv](../examples/13_ap_ctrl_latch/rtl/hls_cfg_latch.sv).
The parts that matter:

```systemverilog
S_IDLE: begin
    ap_start <= 1'b0;
    // Launch only when the kernel is genuinely idle.
    if ((go_pending || auto_run) && ap_idle) begin
        // THE LATCH: every field sampled on the same edge that raises ap_start
        cfg_rows  <= s_rows;   cfg_cols  <= s_cols;
        cfg_roi_x <= s_roi_x;  cfg_roi_y <= s_roi_y;
        cfg_roi_w <= s_roi_w;  cfg_roi_h <= s_roi_h;
        ap_start   <= 1'b1;
        go_pending <= 1'b0;
        state      <= S_LAUNCH;
    end
end

S_LAUNCH: begin
    if (ap_ready) begin
        ap_start <= 1'b0;
        // ap_ready and ap_done coincide on a non-pipelined block (rule 2)
        if (ap_done) begin run_count <= run_count + 1; state <= S_IDLE; end
        else                                            state <= S_RUN;
    end
end
```

Plus `go_pending`: a `go` pulse arriving while busy is **remembered**, not
dropped. Dropping it silently is how you get "every other trigger is ignored"
behaviour that gets blamed on software.

### What the RTL testbench proves

csim cannot test any of this — in C there is no such thing as an argument
changing during a call. [tb/tb_cfg_latch.sv](../examples/13_ap_ctrl_latch/tb/tb_cfg_latch.sv)
runs against the generated RTL in xsim:

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

Test 2 is the whole point: a hostile PS scribbles over the shadow registers
twice, mid-frame, and the result is bit-identical to the baseline.

```bash
make sim-sv EX=13_ap_ctrl_latch
```

## 14.5 Choosing between the two config strategies

| | `ap_ctrl_none` + latch-on-SOF | `ap_ctrl_hs` + `ap_stable` + latch module |
|---|---|---|
| External logic | none | a small FSM |
| Config storage | a register per field, inside HLS | a register per field, in RTL |
| Completion signal | none (poll a status register) | `ap_done`, hence an interrupt |
| Who owns the register map | HLS | your RTL |
| Best for | continuous video | per-frame measurements software consumes |

Both are correct. Pick per block, and **write down which registers are
frame-synchronous and which take effect immediately** — your software team
cannot infer it from the register map. See
[docs/09 §9.4](09-ps-control-registers.md#94-the-sampling-problem-and-the-fix)
for the free-running variant.

## 14.6 Debug signatures

| Observation | Meaning |
|---|---|
| `ap_idle` high, nothing happens | Nobody is asserting `ap_start` — not a deadlock |
| `ap_start` high, `ap_idle` high, forever | Block never launched; check reset polarity |
| Completes once, never again | Controller missed coincident `ap_ready`/`ap_done` (rule 2) |
| `ap_done` stuck high | `ap_ctrl_chain` with `ap_continue` undriven |
| Runs, but occasionally wrong | `ap_stable` contract violated — no latch |
| Every other trigger ignored | `go` pulse dropped while busy |
