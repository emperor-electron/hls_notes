# 11 — Making HLS use BRAM and URAM efficiently

Worked example: [examples/10_storage_binding](../examples/10_storage_binding)
All numbers below are measured on `xczu7ev-ffvc1156-2-e`, Vitis HLS 2023.2.

## 11.1 The primitives

| | Size | Native shapes | Notes |
|---|---|---|---|
| **LUTRAM** (distributed) | 64 bits / LUT | any | Tiny, fast, no latency. Costs LUTs |
| **BRAM18** | 18 Kb | 16K×1 … 1K×18 | The workhorse |
| **BRAM36** | 36 Kb | 32K×1 … 1K×36, 512×72 | Two BRAM18 fused |
| **URAM** | 288 Kb | **4096×72 only** | UltraScale+/Versal. Cannot be reshaped |

The one that surprises people: **URAM has exactly one shape.** A BRAM can be
configured narrow-and-deep or wide-and-shallow; a URAM is 4096×72, full stop.
Ask for anything else and the tool pads or stacks, and you pay for the whole
288 Kb either way.

## 11.2 Three decisions, in order

1. **Shape** — how the array is declared and partitioned. Decides how many
   *independent* memories you need.
2. **Binding** — which primitive each memory becomes.
3. **Ports** — 1R1W vs 2R1W, which decides whether you get `II=1`.

**Shape dominates.** A badly shaped array cannot be rescued by a binding
directive, and this is where nearly all real waste comes from.

## 11.3 Binding

```c
#pragma HLS BIND_STORAGE variable=buf type=ram_2p impl=bram    // Vitis HLS
#pragma HLS RESOURCE     variable=buf core=RAM_2P_BRAM         // Vivado HLS
```

`type`: `ram_1p`, `ram_2p` (1R+1W), `ram_t2p` (true dual port), `ram_s2p`,
`rom_*`, `fifo`.
`impl`: `bram`, `uram`, `lutram`, `srl`, `auto`.

**State it explicitly even when `auto` picks the same thing.** `auto` is a
heuristic that changes between releases, and a design that silently migrates
BRAM → LUTRAM on a version bump can blow your LUT budget or your timing with no
source change to point at.

Measured, 6 line buffers of 1920×8b (`NTAPS=7`):

| `impl` | BRAM18 | URAM | FF | LUT |
|---|---|---|---|---|
| `auto` | 6 | 0 | 1290 | 1454 |
| `bram` | 6 | 0 | 1290 | 1454 |
| `uram` | 0 | 6 | 1290 | 1454 |
| `lutram` | 0 | 0 | 1032 | **2766** |

LUTRAM cost here is ~1440 LUTs for 90 Kb — roughly 64 bits per LUT, as
expected. Viable on this device but it scales badly and hurts routing; keep
LUTRAM for small coefficient tables and windows, not line buffers.

## 11.4 The rounding loss nobody budgets for

`ARRAY_PARTITION complete dim=1` gives you `N` *independent* memories — which
is what a window filter needs, since it reads one pixel from every line every
cycle. But it also means **you pay for one whole primitive per line, however
little data it holds.**

Measured, `PACKED=0`:

| Taps | Lines | Bits total | BRAM18 | URAM |
|---|---|---|---|---|
| 3 | 2 | 30 Kb | 2 | 2 |
| 7 | 6 | 90 Kb | 6 | 6 |
| 15 | 14 | 210 Kb | 14 | 14 |

Utilisation per primitive at 1920×8b:

- **BRAM18**: 15,360 / 18,432 = **83%**. Fine.
- **URAM**: 15,360 / 294,912 = **5.2%**. Catastrophic.

Fourteen URAMs on a ZU7EV is 15% of the device's UltraRAM to store 210 Kb that
would fit in one. **This is why "just force URAM to save BRAM" is usually
wrong.**

## 11.5 Pack lines into width, not into count

The fix is to put all `N-1` line delays in the **width** of a single memory
rather than in separate memories:

```c
typedef ap_uint<NLINES * 8> lineword_t;
static lineword_t linebuf[MAX_COLS];      // ONE memory, MAX_COLS deep

lineword_t rd = linebuf[ix];              // one read returns every line's pixel
for (int k = 0; k < NLINES; ++k) {
#pragma HLS UNROLL
    col[k] = rd.range(8*k + 7, 8*k);
}
lineword_t wr;                            // shift in registers
for (int k = 0; k < NLINES - 1; ++k) {
#pragma HLS UNROLL
    wr.range(8*k + 7, 8*k) = col[k + 1];
}
wr.range(8*(NLINES-1) + 7, 8*(NLINES-1)) = pix;
linebuf[ix] = wr;
```

You still get all `N-1` taps in one cycle — the thing complete partitioning was
for — but from **one** primitive. Still 1R + 1W at the same address, so still
`ram_2p`, so still `II=1`.

Measured, `NTAPS=15` (14 lines, 210 Kb):

| Layout | `impl` | BRAM18 | URAM | FF | LUT |
|---|---|---|---|---|---|
| Partitioned | bram | 14 | 0 | 1944 | 2143 |
| Partitioned | uram | 0 | **14** | 1944 | 2143 |
| **Packed** | bram | 13 | 0 | **876** | **1227** |
| **Packed** | uram | 0 | **2** | **876** | **1227** |

- **URAM: 14 → 2.** A 7× reduction, and utilisation goes 5% → 36%.
- **BRAM: 14 → 13.** Barely moves — BRAM was already 83% utilised, so there was
  nothing to reclaim.
- **FF and LUT roughly halve**, because the shift cascade is now bit-slicing one
  register instead of driving `N-1` separate memory port sets.

Why 2 URAMs and not 1? The packed word is `14×8 = 112` bits, and URAM is 72 bits
wide, so the tool places two side by side. **Past 72 bits you start paying per
line again** — so packing has a sweet spot. For 8-bit video that is up to 9
lines per URAM; beyond that, pack in groups.

Run it yourself:

```bash
./scripts/sweep.py --example 10_storage_binding \
    --define PACKED=0,1 --define STORAGE=1,2 --define NTAPS=15 --stage csynth
```

## 11.6 When URAM is actually the right answer

URAM wins when the memory is **deep**, not when it is merely large-in-total:

| Use | Verdict |
|---|---|
| Frame buffer / large ROI store (hundreds of Kb, one access per cycle) | ✅ ideal |
| Deep FIFO (skew buffer at 4K, ≥ 4096 entries) | ✅ |
| Big LUT / histogram / statistics array | ✅ |
| Line buffers, unpacked | ❌ 5% utilised |
| Line buffers, packed | ✅ if ≥ ~4 lines |
| Anything needing more than 2 ports | ❌ URAM is 2-port, period |

On MPSoC, URAM is frequently idle while BRAM is the binding constraint, so
moving one large deep structure to URAM can unblock a whole design. Check what
is actually scarce before optimising.

## 11.7 Other common waste

**`MAX_COLS` larger than you support.** The single biggest avoidable cost.
4096 on a 1080p design doubles every line buffer. Set it to the real maximum.

**AXI side channels on internal FIFOs.** Inside a `DATAFLOW` region you are on
an HLS FIFO (data/valid/ready), so `TKEEP`/`TSTRB`/`TID`/`TDEST` are pure waste.
Strip at ingress, regenerate at egress — at depth 2048 that is 1 BRAM instead
of 4. See [docs/05 §5.1](05-video-pipeline-patterns.md#51-the-shape-of-a-pipeline).

**FIFOs sized by fear.** Every `STREAM depth=` should name the scenario that
justifies it. Small depths (≤ 32) map to SRL/LUTRAM and are free; the moment
you cross into BRAM territory, justify it.
[docs/03 §3.4](03-backpressure.md#34-fifo-depth-the-only-knob-you-actually-have)
has the sizing rules.

**Replicated kernels.** Four instances each with its own line buffer is 4× the
BRAM. One instance at 4 px/beat is 1× — same throughput, same total bits,
quarter the primitives.

**Arrays that became memories by accident.** A surprise BRAM in the report is
usually a local array you meant to be registers. Check
`<top>_csynth.rpt` → Utilization Estimates → `Memory` row.

**PIPO ping-pong buffers.** `DATAFLOW` inside a loop doubles any array declared
in the region (that is the point — see
[example 08](../examples/08_axis_to_mem_dma)). Budget for 2×, and do not try to
"save" it by hoisting the array out of the loop: that breaks the ping-pong,
serialises the stages, and costs you half your throughput.

## 11.8 Checklist

- [ ] `MAX_COLS`/`MAX_ROWS` set to the real maximum, not a round number
- [ ] Every large array has an explicit `BIND_STORAGE`, not `auto`
- [ ] Line buffers partitioned `complete dim=1` only (never `dim=2`)
- [ ] Windows/tap files partitioned `complete dim=0`
- [ ] `type=ram_2p`, and the access pattern really is 1R+1W per cycle
- [ ] ≥ 4 line buffers → measure the packed layout (§11.5)
- [ ] URAM only for deep structures, or packed line buffers
- [ ] Internal dataflow channels carry bare pixels, not `ap_axiu`
- [ ] Every `STREAM depth=` justified by a named scenario
- [ ] `Memory` row in the utilisation report contains nothing you did not intend
