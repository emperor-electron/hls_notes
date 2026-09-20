# 11 — Fixed-point arithmetic and matching a reference model

A 3×3 colour-correction matrix in `ap_fixed`, with a testbench that
**characterises the quantiser** so you can diff it against an existing model.

## Build

```bash
make csim EX=11_fixed_point_quant
./scripts/sweep.py --example 11_fixed_point_quant --define QMODE=0,1,2,3,4,5,6 --stage csim
```

## The one rule

> Quantise **once**, at the end. Never in the middle.

Carry full precision through the accumulation (`acc_t` keeps 13 fractional
bits, same as the coefficients, so every product is exact) and let the single
conversion to `pixout_t` apply rounding and saturation. Each extra rounding
point is an adder *and* a thing your reference model has to replicate.

## Rounding modes, measured

`ap_fixed<9,9>` (LSB = 1, ties at `x.5`):

| Mode | −2.5 | −1.5 | −0.5 | 0.5 | 1.5 | 2.5 |
|---|---|---|---|---|---|---|
| `AP_TRN` *(default)* | −3 | −2 | −1 | 0 | 1 | 2 |
| `AP_TRN_ZERO` | −2 | −1 | 0 | 0 | 1 | 2 |
| `AP_RND` | −2 | −1 | 0 | 1 | 2 | 3 |
| `AP_RND_ZERO` | −2 | −1 | 0 | 0 | 1 | 2 |
| `AP_RND_MIN_INF` | −3 | −2 | −1 | 0 | 1 | 2 |
| `AP_RND_INF` | −3 | −2 | −1 | 1 | 2 | 3 |
| `AP_RND_CONV` | −2 | −2 | 0 | 0 | 2 | 2 |

**Use this table by diffing it against your model**, not by memorising a
mapping between libraries. Feed your reference the same tie values; the row
that matches is your `QMODE`.

Note also that **quantisation happens before overflow handling**:
`255.6` with `AP_RND` rounds to 256 and *then* saturates to 255.

## Two traps this example is built around

**The destination's mode is the one that applies.**

```c
ap_fixed<8,4> c = a * b;   // computes in <16,8>, truncates into c using C's mode
```
Putting `AP_RND` on `a` and `b` does nothing to that statement. A narrow named
intermediate is a quantisation point you probably did not intend.

**A too-narrow operand silently destroys data.** This bug was in the example's
first draft:

```c
typedef ap_fixed<16,3> coef_t;   // coefficients live in [-1, 2]
acc += M[i] * (coef_t)pixel;     // coef_t saturates at ~3.9999!
```

Every pixel above 3 collapses to the same value. Worst-case error was **251
LSB** and the output still looked like an image — washed out and wrong, which
is why it survives an eyeball check. Fixed by giving the pixel its own type
(`pixin_t = ap_ufixed<8,8>`); error went to **0 LSB**.

## What the testbench does

1. **Characterises** the quantiser on tie values and the saturation boundary.
2. **Error budget** vs a `double` reference over the RGB cube — reports
   bit-exact %, max and RMS error, and enforces ≤0.5 LSB for rounding modes.
3. **Full kernel** end to end.
4. **Bit-exact integer path** — the strategy for when matching a fixed-point
   spec is a hard requirement (explicit `ap_int` + shifts, not `ap_fixed`).

Current result: max error 0.4992 LSB, RMS 0.2339 LSB, 34.6% bit-exact,
worst kernel channel error 0 LSB.

Full treatment: [docs/12](../../docs/12-fixed-point.md).
