# 12 — Using your own SystemVerilog inside HLS (RTL blackbox)

A hand-written iterative divider in SystemVerilog, called from HLS as an
ordinary function. HLS owns the sequencing; the RTL owns the one operation HLS
would implement badly.

## Build

```bash
make csim  EX=12_rtl_blackbox   # runs the C MODEL
make synth EX=12_rtl_blackbox   # instantiates the .sv
make cosim EX=12_rtl_blackbox   # runs the REAL SystemVerilog
```

## What it does

Per-frame auto-normalisation: accumulate the frame maximum, then scale the next
frame by `255/max`. The divide happens **once per frame**, so a 32-cycle
iterative divider is free — whereas asking HLS for a pipelined divider costs
DSPs and LUTs permanently for something used 60 times a second.

## ⚠️ A blackbox cannot own external pins

AMD's docs say it **"cannot connect to top-level interface I/O signals"**, and
the tool enforces it — any RTL port not mapped to a C parameter is an error:

```
ERROR: [HLS 200-654] Cannot find blackbox RTL port 'scl_o' in the json file
```

**So an I²C/SPI/UART core cannot be a blackbox.** Use the sibling-IP pattern in
[docs/13 §13.5](../../docs/13-custom-rtl-integration.md#135-the-sibling-ip-pattern-what-to-use-for-ic).

## Four things that cost real time

| # | Rule | If you break it |
|---|---|---|
| 1 | `ap_ce` is **mandatory** | `ERROR: [HLS 214-145] No 'module_clock_enable'` — and an empty string gives `[HLS 200-653] Cannot find blackbox json port ''` |
| 2 | Port list must be **Verilog-2001**, not SV | `ERROR: [HLS 200-654] Cannot find blackbox RTL port 'logic'`, once per port |
| 3 | Every RTL port must be in the JSON | `[HLS 200-654]` — no pass-through to the top level |
| 4 | Needs a `` `timescale `` | `ERROR: [XSIM 43-4099] Module fx_divide_default doesn't have a timescale` |

On (2): only the module **header** is parsed by HLS. The **body** can be full
SystemVerilog — `typedef enum`, `always_ff`, `unique case`, `logic` — and this
example uses all of them. Keep the `.sv` extension.

## Paths inside the JSON are relative to the *working directory*

Not to the JSON. Build from a repo root and you get
`ERROR: [HLS 200-646] RTL file '...' does not exist` — or, worse, the **C model
path silently fails to resolve at csim time** and you get an undefined
reference to the blackbox function with nothing else to go on. `"cflag"` also
defaults to empty, so the C model compiles with no include paths.

`scripts/build.tcl` rewrites the JSON with absolute paths and real flags before
adding it (`hls::resolve_blackbox_json`). Worth stealing.

## ⚠️ `rtl_performance` is a contract

```json
"rtl_performance" : { "latency" : "49", "II" : "49" }
```

`II` is the one that bites. The AMD example ships `"II":"1"` because its
blackbox is a *pipelined adder*. This divider is a **state machine that runs 49
cycles per call**, so its II equals its latency. Declaring `"II":"1"` told HLS
it could issue a new transaction every cycle — the handshake desynchronised and
cosim hung at `0 / 2 transactions` forever.

This design took four fixes to get to a passing cosim:

| Attempt | Symptom |
|---|---|
| Constant argument `fx_divide(255, …)` | Ports shifted, `quot` unconnected, hang |
| Broken restoring-division loop | "88 mismatching pixels" |
| `"latency":"50", "II":"1"` | hang at `0 / 2` |
| `"latency":"49", "II":"49"` | **PASS** |

Every one of them passed csim.

## ⚠️ Never pass a compile-time constant

`fx_divide(255, prev_max, q)` made HLS fold the literal away and **shift the
port mapping**: `den`'s wire landed on `.num`, `den` was tied to `32'd0`, and
`quot` was **not connected at all**. csim passed, csynth warned about nothing,
cosim hung at `0 / 2 transactions`.

Use a runtime value (here `target`, an `s_axilite` register) and check the
instantiation once:

```bash
sed -n '/fx_divide grp_/,/);/p' hls_proj/solution1/syn/verilog/normalize.v
```

## Develop the RTL against a standalone testbench, not cosim

```bash
make sim-sv EX=12_rtl_blackbox TB_TOP=tb_fx_divide   # ~1 second
```

The divider's first version had a broken restoring-division loop (it compared
the whole 2W-bit remainder against the divisor, so the subtraction never
reduced anything). csim passed — it runs the C model. cosim reported
*"88 mismatching pixels"*, three layers from the cause. The standalone
testbench names the failing operand pair immediately.

## The C model must match the RTL bit-for-bit

csim runs `src/fx_divide_model.cpp`. csynth and cosim run `rtl/fx_divide.sv`.
**If they disagree, csim passes and cosim fails**, and nothing in the C source
hints at why. The testbench therefore hammers the edges both must agree on —
divide by zero, `num=0`, `den=1`, overflow — where the failure is cheap and
legible.

Confirm the substitution actually happened:

```bash
grep -n "fx_divide " examples/12_rtl_blackbox/hls_proj/solution1/syn/verilog/normalize.v
# -> fx_divide grp_fx_divide_fu_141(
```

## Why `ap_ctrl_hs`

A blackbox call after the pixel loop is a sequential tail, and cosim refuses
`ap_ctrl_none` for that:

```
ERROR: [COSIM 212-345] Cosim only supports the following 'ap_ctrl_none'
designs: (1) combinational; (2) pipelined with II of 1; (3) stream-ported.
```

For a per-frame operation `ap_ctrl_hs` is the right protocol anyway —
see [docs/14](../../docs/14-ap-control-protocols.md).

## Verified

```
make csim   EX=12_rtl_blackbox                    # C model      -> PASS
make sim-sv EX=12_rtl_blackbox TB_TOP=tb_fx_divide # RTL unit test -> PASS
make cosim  EX=12_rtl_blackbox                    # C vs RTL      -> PASS
```

```
// RTL Simulation : 2 / 2 [100.00%]
INFO: [COSIM 212-1000] *** C/RTL co-simulation finished: PASS ***
```

Full treatment: [docs/13](../../docs/13-custom-rtl-integration.md).
