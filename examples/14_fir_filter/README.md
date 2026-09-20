# 14 — Streaming FIR filter (DSP)

The "hello world" of DSP in HLS, and a vehicle for four techniques that apply
well beyond filters.

## Build

```bash
make csim EX=14_fir_filter
./scripts/sweep.py --example 14_fir_filter \
    --define NTAPS=15,31 --define SYMMETRIC=0,1 --stage csynth --sort dsp
```

## Technique 1 — exploit algebraic structure

A linear-phase FIR has `h[i] == h[N-1-i]`, so the two samples sharing a
coefficient can be **added before** the multiply:

```
h[i]*x[i] + h[i]*x[N-1-i]  ==  h[i] * (x[i] + x[N-1-i])
```

The pre-adder is free — a DSP48 has a dedicated pre-adder input for exactly
this. Measured on ZU7EV:

| Taps | Form | DSP | FF | LUT |
|---|---|---|---|---|
| 15 | direct | 26 | 5264 | 3179 |
| 15 | **folded** | **14** | 3489 | 2209 |
| 31 | direct | 58 | 11143 | 5814 |
| 31 | **folded** | **30** | 7305 | 3886 |

Roughly half the DSPs *and* half the FF/LUT, for one algebraic identity.

It is also a correctness trap: **the folded form silently computes garbage on
an asymmetric filter.** The testbench asserts symmetry before trusting it —
that check is the precondition, not paranoia. (It earned its place immediately:
the first hand-typed coefficient table wasn't symmetric, and this caught it.)

## Technique 2 — size types to the DSP slice

A DSP48E2 multiplier is 27×18. Keep one operand ≤18 bits and the other ≤27 and
a multiply stays in one slice; exceed it and the tool builds the multiply from
several DSPs plus adders.

```c
typedef ap_fixed<16, 1, AP_RND_CONV, AP_SAT> sample_t;  // Q1.15
typedef ap_fixed<18, 1, AP_RND_CONV, AP_SAT> coef_t;    // 18 = free width
typedef ap_fixed<48, 8>                      acc_t;     // = DSP accumulator
```

`acc_t` at 48 bits is the DSP48's own accumulator width, so it costs nothing
and covers ~16000 taps without overflow.

## Technique 3 — constants are free, reloadable ones are not

```c
static const coef_t COEF[NTAPS] = { ... };
```

`static const` means HLS wires the values straight into the multipliers — no
memory, no partitioning directive, no cost. The `double` literals are converted
at compile time; **there is no floating point anywhere in the result.**

Make that array non-`const` and writable at runtime and you get real registers
*and* real multipliers. That is the single biggest cost difference between a
fixed and a programmable FIR — worth knowing before you promise "programmable".

## Technique 4 — `.range()` is a bit-copy, `=` is a conversion

```c
sample_t x;
x.range(15, 0) = in.data.range(15, 0);   // reinterpret raw bits as Q1.15
// x = in.data;                          // WRONG: converts integer 12345 -> 12345.0, saturates
```

At a protocol edge the wire carries raw two's complement. This confusion is the
most common fixed-point-on-a-bus bug, and it produces output that is *nearly*
right — saturated, not random — so it survives a casual look.

## What the testbench checks

Two tones, one in the passband (0.05 cyc/sample) and one deep in the stopband
(0.40), plus symmetry and side-channel checks. A subtly wrong filter usually
still passes a random-noise test; **a stopband tone does not lie.**

```
coefficients verified symmetric
max |error| vs double reference: 0.000015  (0.49 LSB of Q1.15)
passband tone amplitude : 0.4000 (in 0.4)
stopband tone amplitude : 0.0013 (in 0.4)
rejection               : 49.7 dB
```

## The shift register is the same pattern as a line buffer

`shift[NTAPS]` with `ARRAY_PARTITION complete` is structurally identical to the
video line buffer in [example 03](../03_line_buffer_sobel) — a "line" one
sample deep. Neighbourhood access over a stream is one pattern, whatever the
data means. Omit the partition and the MAC serialises onto a BRAM port: NTAPS
cycles per sample instead of one.
