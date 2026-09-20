# 16 — Writing good HLS C/C++

HLS is not "C that runs on an FPGA". It is a hardware description language that
happens to use C syntax. The code that synthesises well is code whose *structure*
maps onto hardware; the code that synthesises badly is often the most idiomatic
software.

This is the general-purpose companion to the domain docs. Everything here
applies whether you are writing a video filter, a DSP block or a compute kernel.

## 16.1 The mental model

| C construct | What it becomes |
|---|---|
| A function | A module (or inlined into its caller) |
| A loop | A state machine, or a pipeline, or unrolled logic |
| A local scalar | A wire or a register |
| A local array | A memory (BRAM/URAM/LUTRAM) or registers |
| A `static` local | A register/memory that survives across calls |
| An argument | A port |
| `if`/`else` | A mux — **both sides are built**, one is selected |
| A function call in a loop | One shared instance, unless inlined/unrolled |

Two consequences worth internalising:

- **Both branches of an `if` cost area.** There is no "not taken" in hardware.
  A deeply nested conditional tree is a deeply nested mux tree.
- **Everything is allocated statically.** There is no heap, no stack growth,
  and every array is sized at compile time.

## 16.2 What does not synthesise

| Not allowed | Instead |
|---|---|
| `malloc`/`new`/`free`/`delete` | Fixed-size arrays sized by a `#define` or template parameter |
| Recursion (unbounded) | A loop, or a template with a compile-time depth |
| Function pointers, virtual calls | Templates, or a `switch` |
| `std::vector`, `std::map`, `std::string` | C arrays, `hls::stream`, `ap_int` |
| `printf`/`iostream` in synthesised code | Guard with `#ifndef __SYNTHESIS__` |
| System calls, file I/O, time | Testbench only |
| Unbounded `while (cond)` where `cond` is not provably bounded | Bound it, or accept an unknown latency |
| Pointer arithmetic across different objects | Index into one array |
| Casting a pointer to an integer and back | Don't |

Anything in the "instead" column is available in the testbench — the TB is
ordinary C++ compiled with the host compiler. That asymmetry is the single most
useful thing to know: **`add_files -tb` code has no restrictions at all.**

## 16.3 Types

### Use sized types everywhere

```c
int         counter;      // 32 bits, always, whether you need them or not
ap_uint<12> counter;      // 12 bits
```

`int` is not wrong, it is *expensive by default*. HLS will often prune the
unused bits, but pruning is not guaranteed and it cannot prune what it cannot
prove. On one kernel it's noise; replicated across a window filter or an unrolled
MAC tree it is real LUTs and real DSPs.

Rules of thumb:

- Size an accumulator to `operand_bits + ceil(log2(n_terms))`, deliberately.
- Keep multiply operands inside the DSP's native width (27×18 on UltraScale+)
  or you pay several DSPs for one multiply — see
  [example 14](../examples/14_fir_filter).
- Prefer `ap_uint<N>` over `ap_int<N>` when the value cannot be negative; it
  removes sign-extension logic.
- Never use `float`/`double` in a datapath unless you genuinely need the range.
  Use `ap_fixed` — [docs/12](12-fixed-point.md).

### `.range()` is a bit-copy; `=` is a numeric conversion

```c
sample_t x;
x.range(15,0) = word.range(15,0);   // reinterpret raw bits
x = word;                           // convert the INTEGER value, then saturate
```

At a protocol edge you almost always want the first. Mixing them up produces
output that is *nearly* right, which is worse than obviously wrong.

### Beware C integer promotion

```c
ap_uint<8> a = 200, b = 100;
ap_uint<8> c = a + b;          // ap_uint arithmetic: grows to <9>, then truncates
int        d = a + b;          // promoted to int: 300
```

`ap_int`/`ap_uint` arithmetic grows the result to be lossless and loses
precision **on assignment**. Plain C types promote to `int`. Mixing them in one
expression gets you whichever rule applies to the widest operand, which is
rarely what you meant. Be explicit with casts at the boundaries.

## 16.4 Loops

**Bounds should be compile-time constants where possible.** A constant bound
lets HLS unroll, flatten and pipeline with known trip counts. Where the bound
must be runtime, add `LOOP_TRIPCOUNT` so the latency report is meaningful —
it changes no hardware.

**Keep nests perfect.** A statement between two `for` headers blocks flattening,
silently:

```c
for (y = 0; y < rows; ++y) {
    int base = y * cols;          // breaks flattening
    for (x = 0; x < cols; ++x) { ... }
}
```

**Pipeline the inner loop, not the outer.** `PIPELINE` on an outer loop
implicitly fully unrolls everything inside it.

**`break`/`continue` are allowed but cost you.** They make the trip count
data-dependent, which blocks flattening and can block pipelining. Prefer
predication:

```c
for (i = 0; i < n; ++i) { if (!done) { ... } }    // often better than break
```

**Loop-carried dependencies set the II.** A reduction into a single accumulator
serialises at the adder latency; split it into N partial sums and combine at the
end — see [example 15](../examples/15_matmul_tiled), where that single change
takes II from 8 to 1.

## 16.5 Functions

- **A function is a module.** HLS inlines small ones automatically; control it
  with `#pragma HLS INLINE` / `INLINE off`.
- **Inline aggressively inside a pipelined loop.** A non-inlined function
  becomes a shared instance that iterations must queue for.
- **Keep dataflow stages non-inlined** (`INLINE off`) — they must stay separate
  processes.
- **Pass arrays by reference/pointer, not by value.** Passing by value copies
  a whole memory.
- **Templates are free and excellent.** They give you compile-time
  parameterisation with no runtime cost:

```c
template<int N, typename T>
T dot(const T a[N], const T b[N]) {
#pragma HLS INLINE
    T acc = 0;
    for (int i = 0; i < N; ++i) {
#pragma HLS UNROLL
        acc += a[i] * b[i];
    }
    return acc;
}
```

Prefer a template parameter over a `#define` when the value is a property of a
*call site* rather than of the build.

## 16.6 Arrays and memory

- **An array is a memory with 1–2 ports.** Two reads in one cycle need
  partitioning, replication or a true dual-port binding.
- **Partition what you access in parallel; never what you access sequentially.**
- **Initialise `static` arrays explicitly** if you depend on their value.
  Statics reset only on reset, not per call.
- **Local (non-static) arrays are re-created per call** — for a large array that
  means real initialisation cost.
- **Avoid a runtime-variable index into a fully-partitioned array.** It becomes
  an N-input mux; at N=64 that is expensive. Restructure so the index is a
  compile-time constant inside an unrolled loop.

Full treatment in [docs/11](11-memory-resources.md).

## 16.7 Control flow and structs

**Structs are flattened.** A `struct` passed over an interface becomes a
concatenated bus in declaration order. That ordering is an ABI — **append
only**, or you silently reshuffle every downstream field
([docs/09 §9.3](09-ps-control-registers.md#93-the-generated-register-map-is-the-abi)).

**Prefer a `switch` over a chain of `if`s** when the cases are mutually
exclusive and you want a single mux rather than a priority chain.

**Hoist loop-invariant conditionals out of loops.** HLS often does this, but
when it cannot (because the condition touches a volatile or an interface) you
pay the mux every iteration.

## 16.8 The `__SYNTHESIS__` asymmetry

```c
#ifndef __SYNTHESIS__
    printf("debug: %d\n", (int)v);   // csim only
#endif
```

`__SYNTHESIS__` is defined by the HLS front end and **not** by the csim
compiler. Use it for debug output, assertions, and to bound otherwise-infinite
loops ([docs/02 §2.4](02-axi-stream-interfaces.md#24-three-ways-to-be-free-running)).

Two traps:

1. Code inside `#ifndef __SYNTHESIS__` is **never verified against hardware**.
   If it computes something the rest of the code uses, csim and csynth diverge
   and the C source gives no hint.
2. Never define `__SYNTHESIS__` yourself to "test the synthesis path" — it
   changes library behaviour in ways real csim does not use. (`hls_stream.h`
   will silently return default values on empty reads — see
   [docs/04 §4.2](04-deadlock-playbook.md#what-csim-actually-does-on-an-empty-read).)

## 16.9 Style that pays off

- **Name every loop.** Labels appear in reports, directives and error messages;
  an unnamed loop shows up as `VITIS_LOOP_144_5`.
- **Name every `hls::stream`** in its constructor — cosim messages and waveform
  signals use it.
- **Put directives in the source as pragmas**, not in `directives.tcl`. They
  travel with the code and survive review
  ([docs/01 §1.2](01-tooling-and-tcl-automation.md#12-why-scripts-not-the-gui)).
- **Write the comment that says *why the pragma is there*.** `#pragma HLS
  ARRAY_PARTITION` with no note is indistinguishable from cargo cult six months
  later. Say what breaks without it.
- **Keep one compile-time knob per design decision** (`TILE`, `UNROLL_K`,
  `NTAPS`, `METHOD`) and sweep them. A design with named knobs is a design you
  can optimise; a design with magic numbers is one you can only rewrite.
- **Guard unsupported configurations with `#error`.** A silent fallback to a
  degenerate case ("all-zero coefficients") synthesises, runs and produces
  plausible garbage.

## 16.10 A checklist before you synthesise

- [ ] No `malloc`, recursion, virtual calls, or STL containers in synthesised code
- [ ] Every array bound and loop bound is compile-time, or has `LOOP_TRIPCOUNT`
- [ ] Every accumulator width derived, not inherited from `int`
- [ ] Multiply operands fit the DSP's native width
- [ ] Inner loops pipelined; nests perfect enough to flatten
- [ ] Reductions split into enough partial sums to hit the target II
- [ ] Arrays partitioned along the dimension accessed in parallel
- [ ] No runtime divide/modulo in a pipelined loop
- [ ] `AP_SAT` on anything that can overflow
- [ ] Every loop and stream named
- [ ] Every pragma has a comment saying what breaks without it

## 16.11 And then: measure, don't assume

The repo's own measurements contradicted received wisdom more than once:

| Folklore | Measured on Vitis HLS 2023.2 |
|---|---|
| "`hist[v]++` is II=2, add `DEPENDENCE`" | II=1 with no help; the real cost is **timing** ([ex. 16](../examples/16_histogram_dependence)) |
| "`impl=uram` saves BRAM" | Unpacked line buffers are **5% utilised** — 14 URAM for 210 Kb ([docs/11 §11.4](11-memory-resources.md#114-the-rounding-loss-nobody-budgets-for)) |
| "A blackbox's `II` field is advisory" | `"II":"1"` on a sequential block **hangs cosim** ([docs/13](13-custom-rtl-integration.md)) |

Sweep the variants on your tool version, read the critical path, and only then
complicate working code.
