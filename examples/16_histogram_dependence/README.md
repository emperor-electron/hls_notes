# 16 — Read-modify-write dependence: `hist[v]++`

Three lines of C, and the most instructive II/timing problem in HLS. The same
shape appears in scatter-adds, sparse accumulation, binning, reference
counting and graph algorithms — anything that updates a table at a
**data-dependent index**.

## Build and sweep

```bash
make csim EX=16_histogram_dependence
./scripts/sweep.py --example 16_histogram_dependence \
    --define METHOD=0,1,2,3 --define STORAGE=0,1 --stage csynth
```

## The folklore is out of date — measure before you "fix"

The classic advice is *"histograms are II=2, add `#pragma HLS DEPENDENCE`."*
On Vitis HLS 2023.2 (xczu7ev) that **does not reproduce**. All four strategies
reach **II=1**, including the completely naive one, and including when the
table is forced into BRAM. The tool inserts the accumulator forwarding itself.

What the dependence costs now is **timing**, not throughput:

| METHOD | II | Est. period | Depth | BRAM | LUT | Correct? |
|---|---|---|---|---|---|---|
| 0 naive | 1 | **3.680 ns** | 2 | 3 | 692 | yes |
| 1 manual forward | 1 | **3.680 ns** | 2 | 3 | 761 | yes |
| 2 banked | 1 | **2.443 ns** | 4 | 6 | 1010 | yes |
| 3 unsafe `DEPENDENCE` | 1 | **2.443 ns** | 4 | 3 | 611 | **no** |

Target is 3.33 ns, so 0 and 1 **miss timing** and 2 and 3 meet it.

The reported critical path says exactly why. METHOD=0:

```
icmp (loop counter)        1.016 ns
axis read                  1.000 ns
load from array 'hist'     1.237 ns   <- the RMW, closed in one cycle
                           --------
                           3.680 ns
```

For METHOD=2 the array load is **not on the critical path at all** — banking
let the scheduler push it into a later stage (depth 4 instead of 2), leaving
just the counter compare and the stream read.

So the useful question is not *"how do I get II=1"* — you already have it — but
*"how do I stop the read-modify-write sitting in a single cycle."* Adding
pipeline stages does that correctly; asserting the dependency away does it
incorrectly.

## The four strategies

**0 — naive.** `hist[v] = hist[v] + 1;` If your clock has the slack, **ship
this.** It is the smallest and clearest, and every alternative is a response to
a timing problem you may not have.

**1 — manual forwarding.** Keep the previous bin's value in a register and
bypass the memory when the bin repeats. On 2023.2 this is exactly what HLS
already does, so it costs ~50 FF and ~70 LUT and buys nothing. It's here
because it's the right technique on older tools, and because the *pattern* —
detect the collision and bypass — is what to reach for when the tool can't.

**2 — banked.** `NBANKS` copies, round-robined by iteration, summed at the end.
The one that actually fixes the timing. Costs 2× the memory, ~2× the LUTs, and
a merge pass per block.

**3 — `#pragma HLS DEPENDENCE ... inter false`.** Tells the scheduler to
*assume* no dependency. One demonstrably exists, so this produces RTL that
disagrees with csim and **undercounts repeated values**.

> `DEPENDENCE` is legitimate when you can *prove* independence — e.g. indices
> distinct by construction. **"The tool complained and this made it stop" is
> not a proof.**

## csim cannot catch METHOD=3

```
=== METHOD=3 ... ***  UNEXPECTED PASS
```

The `DEPENDENCE` pragma has no effect in C — csim is sequential, so the hazard
does not exist there. Only cosim or hardware shows the lost counts. The
testbench says so explicitly rather than reporting a misleading pass.

## Test with runs, not random data

The testbench includes a **run of 1000 identical samples**. Adjacent-sample
collisions are rare in random data, so a random-only test passes with broken
dependence handling — which is exactly why it is not sufficient. Also covered:
alternating pairs (collision at distance 2), short runs, and edge bins.
