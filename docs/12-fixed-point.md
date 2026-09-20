# 12 — Fixed-point arithmetic and matching a reference model

Worked example: [examples/11_fixed_point_quant](../examples/11_fixed_point_quant)

## 12.1 The type

```c
ap_fixed<W, I, Q, O, N>
//       │  │  │  │  └─ N: saturation bits (almost always 0)
//       │  │  │  └──── O: overflow mode
//       │  │  └─────── Q: quantisation mode
//       │  └────────── I: integer bits, INCLUDING the sign bit
//       └───────────── W: total bits
```

Fractional bits = `W - I`. A negative `I` is legal: `ap_fixed<8,-2>` covers
0 … 2⁻² in steps of 2⁻¹⁰.

**The defaults are `AP_TRN, AP_WRAP`** — truncate and *wrap*. Wrapping means
256 becomes 0, so a bright pixel turns black. For anything that can overflow,
state `AP_SAT` explicitly.

## 12.2 The one rule

> **Quantise once, at the end. Never in the middle.**

Every intermediate you round is a rounding you must also model in your
reference, and the errors compound in a way that depends on scheduling. Carry
full precision through the accumulation and let a single final conversion apply
rounding and saturation. It is also cheaper — each rounding point costs an adder.

## 12.3 Bit growth, and where precision is actually lost

`ap_fixed` arithmetic **grows the result** to be lossless:

```c
ap_fixed<8,4> a, b;
a + b   // -> ap_fixed<9,5>    (one carry bit)
a * b   // -> ap_fixed<16,8>   (widths add)
```

Loss happens on **assignment**, not in the expression:

```c
ap_fixed<8,4> c = a * b;   // computes in <16,8>, then truncates into c
                           // using C's Q and O modes
```

So **the rounding mode that applies is the one on the destination type.**
Putting `AP_RND` on the operands does nothing to that statement.

Corollary: *a narrow named intermediate is a quantisation point you probably
did not intend.* If hardware disagrees with your reference by fractions of an
LSB in unpredictable places, go looking for one.

### The related trap: a too-narrow operand

```c
typedef ap_fixed<16,3> coef_t;   // coefficients live in [-1, 2]
acc += M[i] * (coef_t)pixel;     // WRONG: coef_t saturates at ~3.9999
```

Casting an 0–255 pixel into a 3-integer-bit type collapses everything above 3
to the same value. The output still looks like an image — washed out and wrong —
which is why it survives an eyeball check. Give each operand a type wide enough
for *its own* range; the coefficient's type is for the coefficient. (This bug
is in the example's history, and the testbench now catches it: worst-case error
went 251 LSB → 0.)

## 12.4 The quantisation modes, measured

Measured with `ap_fixed<9,9>` (LSB = 1, so ties fall at `x.5`) on Vitis HLS
2023.2 — regenerate with
`./scripts/sweep.py --example 11_fixed_point_quant --define QMODE=0,1,2,3,4,5,6 --stage csim`:

| Mode | −2.5 | −1.5 | −0.5 | 0.5 | 1.5 | 2.5 | Tie rule |
|---|---|---|---|---|---|---|---|
| `AP_TRN` *(default)* | −3 | −2 | −1 | 0 | 1 | 2 | floor (toward −∞) |
| `AP_TRN_ZERO` | −2 | −1 | 0 | 0 | 1 | 2 | toward zero |
| `AP_RND` | −2 | −1 | 0 | 1 | 2 | 3 | toward +∞ |
| `AP_RND_ZERO` | −2 | −1 | 0 | 0 | 1 | 2 | toward zero |
| `AP_RND_MIN_INF` | −3 | −2 | −1 | 0 | 1 | 2 | toward −∞ |
| `AP_RND_INF` | −3 | −2 | −1 | 1 | 2 | 3 | away from zero |
| `AP_RND_CONV` | −2 | −2 | 0 | 0 | 2 | 2 | **to even** (banker's) |

`AP_TRN` and `AP_RND_MIN_INF` agree on ties but differ elsewhere: `AP_TRN`
always floors, `AP_RND_MIN_INF` rounds to nearest. Same for `AP_TRN_ZERO` vs
`AP_RND_ZERO`.

### Quantisation happens *before* overflow handling

```
ap_ufixed<8,8,AP_RND,AP_SAT>:  255.6 -> rounds to 256 -> saturates to 255
ap_ufixed<8,8,AP_TRN,AP_SAT>:  255.6 -> truncates to 255, never overflows
```

Not everyone expects that ordering.

### Cost

| Mode | Hardware |
|---|---|
| `AP_TRN`, `AP_WRAP` | free (drop bits) |
| `AP_TRN_ZERO` | ~1 adder |
| `AP_RND*` | ~1 adder |
| `AP_RND_CONV` | ~1 adder + tie logic |
| `AP_SAT` | ~1 comparator + mux per bound |

Cheap at one quantisation point. Expensive when you have accidentally created
twenty by declaring narrow intermediates.

## 12.5 Matching an existing model

Which strategy applies depends on what the reference actually is.

### Strategy 1 — reference is float/double (numpy, MATLAB double, a C model)

**You cannot match it bit-exactly, and trying is wasted effort.** It has ~52
mantissa bits; you have 13 fractional bits. Define an error budget instead:

```
max |hw − ref| ≤ 1 LSB of the output
```

and verify it statistically over a wide input set including the extremes. The
example reports the full distribution:

```
samples          12288
bit-exact        4255 (34.6%)
max |error|      0.4992 LSB
RMS error        0.2339 LSB
```

Budget: ≤0.5 LSB for a rounding mode, ≤1 LSB for `AP_TRN`, plus coefficient
quantisation error. Worse than that is a bug, not a precision choice.

### Strategy 2 — reference is itself fixed-point (a C model with ints, a spec like BT.601, another team's RTL)

Bit-exactness is achievable **and required**. Do not paraphrase it with
`ap_fixed` implicit conversions — mirror the reference operation for operation
with `ap_int`/`ap_uint` and explicit shifts:

```c
// the spec, in plain C
int spec = (19595*r + 38470*g + 7471*b + 32768) >> 16;

// HLS, operation for operation
ap_uint<27> acc = (ap_uint<27>)(19595*r) + 38470*g + 7471*b;
ap_uint<8>  hw  = (ap_uint<8>)((acc + 32768) >> 16);
```

**Explicit beats implicit whenever "bit-exact" is in the requirement.**
[Example 02](../examples/02_rgb_to_gray) is written this way for exactly this
reason.

### Strategy 3 — reference is a fixed-point library (MATLAB `fi`, Python `fxpmath`)

Bit-exactness is achievable if the rounding and overflow modes match — but
**do not trust a remembered mapping table between libraries.** Determine it
empirically:

1. Feed your reference the tie values: `±0.5, ±1.5, ±2.5`, values just either
   side of a tie, and the overflow boundary.
2. Feed `ap_fixed` the same values (the example prints exactly this table).
3. Diff. The row that matches is your `QMODE`.

That takes ten minutes and is correct; a remembered table is neither.

## 12.6 Verification checklist

- [ ] Golden model written in `double`, structurally independent of the DUT
- [ ] Error budget stated as a number, and enforced by the testbench
- [ ] Tie values tested explicitly, not just random inputs
- [ ] Saturation boundary tested from both sides
- [ ] Zero and full-scale tested (white must map to white — see
      [docs/05 §5.3](05-video-pipeline-patterns.md#53-fixed-point-arithmetic))
- [ ] Every intermediate type's range checked against the values it must hold
- [ ] `AP_SAT` on anything that can overflow
