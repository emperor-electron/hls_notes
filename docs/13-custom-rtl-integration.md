# 13 — Using your own SystemVerilog inside / alongside HLS

Worked example: [examples/12_rtl_blackbox](../examples/12_rtl_blackbox)
(verified: csim against the C model, cosim against the real `.sv`)

## 13.1 Short answer for the I²C case

> **Q:** "Can I write the protocol-level SystemVerilog for something like I²C
> and have HLS handle the control logic around it?"

**Yes — but not with the RTL blackbox.** Use the *sibling IP* pattern (§13.5).

The blackbox mechanism is real and works, but AMD's own documentation states
it **"cannot connect to top-level interface I/O signals"**, and the tool
enforces it: any RTL port not mapped to a C function parameter is an error.

```
ERROR: [HLS 200-654] Cannot find blackbox RTL port 'scl_o' in the json file
```

*(verified empirically — adding unmapped `scl_o`/`sda_i` ports to a working
blackbox fails exactly like this.)*

Since an I²C core must own `scl`/`sda` pads, it cannot be a blackbox. It has to
sit **beside** the HLS block in the block design, with HLS driving it over a
handshake.

That is also the better architecture. I²C runs at 100–400 kHz; a byte takes
~80 µs. Embedding that inside an HLS call means the HLS block is blocked for
tens of thousands of cycles, and anything sharing a dataflow region with it
stalls too.

## 13.2 Decision table

| You want to… | Use | Why |
|---|---|---|
| A custom compute helper (divider, CORDIC, CRC, hand-tuned datapath) | **Blackbox** (§13.3) | HLS schedules around it; it is just a function call |
| Anything touching external pins (I²C, SPI, UART, MDIO, LVDS) | **Sibling IP** (§13.5) | Blackbox cannot own top-level I/O |
| Anything slow relative to the data path | **Sibling IP** | Don't stall the pipeline on a 80 µs transaction |
| An existing verified IP with an AXI interface | **Sibling IP** | Connect in IPI; no HLS involvement needed |
| Replace one function in a mostly-HLS design | **Blackbox** | Smallest change |

## 13.3 The RTL blackbox, concretely

```tcl
add_files -blackbox my_core.json
```

### The RTL port contract — all mandatory

```verilog
module fx_divide #(parameter W = 32) (
    input  wire          ap_clk,
    input  wire          ap_rst,      // ACTIVE HIGH
    input  wire          ap_ce,       // NOT optional
    input  wire          ap_start,
    output wire          ap_done,
    output wire          ap_idle,
    output wire          ap_ready,
    input  wire          ap_continue,
    input  wire [W-1:0]  num,         // one per C parameter
    input  wire [W-1:0]  den,
    output wire [W-1:0]  quot,
    output reg           quot_vld     // data_write_valid for outputs
);
```

Four things that cost real time if you don't know them:

1. **`ap_ce` is required.** Omit `module_clock_enable` from the JSON and you
   get `ERROR: [HLS 214-145] No 'module_clock_enable' in 'rtl_common_signal'`.
   Give it an empty string and you get
   `ERROR: [HLS 200-653] Cannot find blackbox json port ''`.
2. **The port list must be Verilog-2001, not SystemVerilog.** HLS parses the
   module header with a V2001 parser. Given `input logic [31:0] x` it reads the
   *keyword* `logic` as a port name:
   ```
   ERROR: [HLS 200-654] Cannot find blackbox RTL port 'logic' in the json file
   ```
   …once per port. Use `wire`/`reg` and untyped `parameter` **in the port
   list**. The module **body** can be full SystemVerilog (`typedef enum`,
   `always_ff`, `unique case`, `logic`) — only the header is parsed by HLS.
   Keep the `.sv` extension.
3. **Every RTL port must appear in the JSON.** No pass-through to the top
   level, hence §13.1.
4. **You need a `` `timescale ``.** HLS emits `1ns/1ps` in its own modules and
   xsim refuses a mixed design:
   ```
   ERROR: [XSIM 43-4099] Module fx_divide_default doesn't have a timescale
   but at least one module in design has a timescale.
   ```
   Note it names `fx_divide_default` — the parameterised variant, not a module
   you wrote — so the message is harder to act on than it looks.

### The JSON

```json
{
  "c_function_name"     : "fx_divide",
  "rtl_top_module_name" : "fx_divide",
  "c_files"  : [ { "c_file" : "fx_divide_model.cpp", "cflag" : "" } ],
  "rtl_files": [ "fx_divide.sv" ],
  "c_parameters" : [
    { "c_name":"num",  "c_port_direction":"in",
      "rtl_ports":{ "data_read_in":"num" } },
    { "c_name":"quot", "c_port_direction":"out",
      "rtl_ports":{ "data_write_out":"quot", "data_write_valid":"quot_vld" } }
  ],
  "rtl_common_signal" : {
    "module_clock":"ap_clk", "module_reset":"ap_rst",
    "module_clock_enable":"ap_ce",
    "ap_ctrl_chain_protocol_idle":"ap_idle",
    "ap_ctrl_chain_protocol_start":"ap_start",
    "ap_ctrl_chain_protocol_ready":"ap_ready",
    "ap_ctrl_chain_protocol_done":"ap_done",
    "ap_ctrl_chain_protocol_continue":"ap_continue"
  },
  "rtl_performance"    : { "latency":"33", "II":"1" },
  "rtl_resource_usage" : { "FF":"140","LUT":"220","BRAM":"0","URAM":"0","DSP":"0" }
}
```

Outputs are **by reference** in C (`void f(..., ap_uint<32> &quot)`), not a
return value. A return value needs the separate top-level `c_return` key.

### Paths inside the JSON are relative to the *working directory*

Not to the JSON. The AMD example only works because its script runs from the
directory holding the files. Build from a repo root and you get:

```
ERROR: [HLS 200-646] RTL file '...' does not exist
```

…or worse, the **C model path silently fails to resolve at csim time** and you
get an undefined reference to the blackbox function with nothing else to go on.
And `"cflag"` defaults to empty, so the C model compiles with **no include
paths**.

`scripts/build.tcl` fixes both by rewriting the JSON with absolute paths and
real flags before adding it (`hls::resolve_blackbox_json`). Worth stealing.

### `rtl_performance` is a contract, not a comment

```json
"rtl_performance" : { "latency" : "49", "II" : "49" }
```

HLS **believes** these numbers and schedules around them.

`II` is the killer. The AMD example ships `"II":"1"` because its blackbox is a
pipelined adder. A **non-pipelined** block — anything with a state machine that
runs for N cycles per call — has `II == latency`, because it cannot accept a
new transaction until the current one finishes. Declaring `"II":"1"` on it
tells HLS it may issue a new transaction every cycle; the handshake
desynchronises and cosim hangs at `0 / N transactions` forever.

That was the last bug in example 12, and it survived three earlier fixes:

| Attempt | Symptom |
|---|---|
| Constant argument `fx_divide(255, …)` | Ports shifted, `quot` unconnected, cosim hung |
| Broken restoring-division loop | cosim: "88 mismatching pixels" |
| `"latency":"50", "II":"1"` | cosim hung at `0 / 2` |
| `"latency":"49", "II":"49"` | **`*** C/RTL co-simulation finished: PASS ***`** |

Count the latency from `ap_start` to `ap_done` inclusive, and set `II` equal to
it unless your block is genuinely pipelined.

### Never pass a compile-time constant to a blackbox

This one produced **silently wrong RTL** on Vitis HLS 2023.2. Writing

```c
fx_divide(255, prev_max, q);     // 255 is a literal
```

made HLS constant-fold the literal away, and the remaining ports **shifted**:

```verilog
fx_divide grp_fx_divide_fu_141(
    ... .num(grp_fx_divide_fu_141_den),   // den's wire landed on num
        .den(32'd0)                       // tied to zero
);                                        // quot not connected AT ALL
```

csim passed (it runs the C model, which never sees the instantiation). csynth
issued **no warning**. Cosim hung forever at `0 / 2 transactions`.

Make every blackbox argument a genuine runtime value — an `s_axilite` register,
a loop variable, anything HLS cannot fold. Then check the instantiation:

```bash
sed -n '/fx_divide grp_/,/);/p' <proj>/<sol>/syn/verilog/<top>.v
```

Every data port should be connected to a real wire. **Read that instantiation
once per design**; it takes ten seconds and catches an entire class of silent
failure.

### Other limits

- Protocols: **`ap_ctrl_chain` or `ap_ctrl_none` only.**
- In a `DATAFLOW` region, only `hls::stream` and array arguments are supported —
  scalars and pointers are not. Keeping the call at the top level avoids this.
- The C model and the RTL **must agree bit-for-bit, including on edge cases**
  (divide-by-zero, saturation). If they disagree, csim passes and cosim fails,
  and nothing in the C source hints at why. Test the edges in csim explicitly.

### `ap_ctrl_none` + cosim

A design with a sequential tail (like a blackbox call after the pixel loop)
cannot be `ap_ctrl_none` if you want to cosim:

```
ERROR: [COSIM 212-345] Cosim only supports the following 'ap_ctrl_none'
designs: (1) combinational designs; (2) pipelined design with II of 1;
(3) designs with array streaming or hls_stream or AXI4 stream ports.
```

Use `ap_ctrl_hs` — which for a per-frame operation is the right protocol anyway
([docs/14](14-ap-control-protocols.md)).

## 13.4 Verifying a blackbox

| Stage | What runs | Proves |
|---|---|---|
| `csim` | the **C model** | your control logic and the model's arithmetic |
| `csynth` | — | the `.sv` is copied into `syn/verilog/` and instantiated |
| `cosim` | the **real `.sv`** | the C model and the RTL agree |
| a standalone SV testbench | the **real `.sv`**, alone | the RTL is correct, in ~1 s |

**csim never touches your SystemVerilog.** A C-model/RTL mismatch is invisible
until cosim. Confirm the instantiation happened and every port is wired:

```bash
sed -n '/fx_divide grp_/,/);/p' examples/12_rtl_blackbox/hls_proj/solution1/syn/verilog/normalize.v
```

### Write a standalone RTL unit test first

Cosim is the integration check, not the development loop. It takes ~40 s per
run, rebuilds the whole design, and reports a divergence as something like
*"88 mismatching pixels"* — three layers away from the cause.

The divider in example 12 got this wrong on the first attempt: its restoring
division compared the entire 2W-bit remainder against the divisor, so the
subtraction never reduced anything. csim passed, csynth passed, and cosim
reported 88 bad pixels with no pointer to the divider at all.

A 60-line standalone testbench
([tb/tb_fx_divide.sv](../examples/12_rtl_blackbox/tb/tb_fx_divide.sv)) compares
the RTL directly against the same expression the C model uses, runs in about a
second, and names the failing operand pair:

```bash
make sim-sv EX=12_rtl_blackbox TB_TOP=tb_fx_divide
#   ok   255/128 = 130560 (1.9922)
#   ok   255/0 = 4294967295 (65536.0000)
#   *** PASS
```

**Develop the RTL against that; use cosim as the integration check.** And make
sure the unit test covers the edges the C model also has to define — divide by
zero, zero numerator, saturation — because those are exactly where the two
implementations drift apart unnoticed.

## 13.5 The sibling-IP pattern (what to use for I²C)

```
            ┌──────────────────┐         ┌─────────────────┐
  AXI-Lite  │  HLS sequencer   │  cmd →  │  your SV I²C    │  scl/sda
  ─────────►│  (ap_ctrl_hs or  │◄─ rsp   │  byte master    │◄════════►
            │   free-running)  │         │  (owns the pads)│
            └──────────────────┘         └─────────────────┘
```

HLS exposes a command stream and a response stream; your core consumes commands
and drives the pins. Connect them in IP Integrator.

**HLS side** — a command/response pair of streams:

```c
struct i2c_cmd_t { ap_uint<8> op; ap_uint<8> data; };   // START/WRITE/READ/STOP
struct i2c_rsp_t { ap_uint<8> data; ap_uint<1> nack; };

void sensor_init(hls::stream<i2c_cmd_t> &cmd,
                 hls::stream<i2c_rsp_t> &rsp, ...) {
#pragma HLS INTERFACE axis port=cmd
#pragma HLS INTERFACE axis port=rsp
    // ... sequence the register writes, check NACKs, retry, report status
}
```

Why this is the right shape:

- **The pads stay in RTL**, where they belong (`IOBUF`, open-drain, clock
  stretching, glitch filtering are all things you want in hand-written RTL).
- **HLS blocks on a stream read**, which is exactly the right behaviour: it
  stalls until the byte completes and costs nothing while waiting.
- **The slow protocol cannot stall anything else**, because the HLS sequencer
  is its own IP, not part of your video dataflow region.
- **You can verify the core standalone** with an SV testbench and an I²C slave
  model, and verify the sequencer standalone with csim. That is a much better
  test story than one monolith.

Remember the deadlock rules still apply across the boundary: if the sequencer
writes `N` commands it must read exactly the responses the core produces, or it
blocks forever ([docs/04 §4.4](04-deadlock-playbook.md#44-family-b--data-dependent-token-counts)).
Decide up front whether every command produces a response, or only reads do —
and make the core and the sequencer agree.

### Variant: no streams, just a register interface

If the sequencing is simple, skip HLS entirely for the transport: have HLS
write a descriptor to a BRAM or DDR buffer and let the SV core walk it. HLS is
good at deciding *what* to send; it is not adding value in the byte-by-byte
handshake.

## 13.6 Which to reach for

- **Compute helper, no pins, fits a function call** → blackbox.
- **Pins, slow protocols, existing verified IP, anything you want to test
  standalone** → sibling IP.
- **When in doubt** → sibling IP. It is less coupled, independently testable,
  and does not constrain your HLS block's control protocol.
