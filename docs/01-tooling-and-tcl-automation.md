# 1 — Tooling and Tcl automation

> Goal: never open the GUI except to look at a schedule viewer or a waveform.

## 1.1 The two binaries

| | `vivado_hls` | `vitis_hls` |
|---|---|---|
| Releases | ≤ 2020.1 | 2020.1 → |
| Stream type | `ap_axis`/`ap_axiu` structs | `hls::axis<>`, with `ap_axiu` as an alias |
| Default dataflow channel | FIFO | PIPO (scalars/arrays), FIFO (streams) |
| `config_dataflow -strict_mode` | `warning` | `error` available and worth using |
| `RESOURCE` directive | canonical | deprecated → `BIND_STORAGE` / `BIND_OP` |
| Free-running `ap_ctrl_none` cosim | unsupported | still effectively unsupported for unbounded loops |

Both accept `-f script.tcl` and both return the script's exit code, so either
drops straight into a Makefile. This repo's [scripts/build.tcl](../scripts/build.tcl)
works with both and prints which one it detected.

## 1.2 Why scripts, not the GUI

The GUI stores directives in `<solution>/directives.tcl` and rewrites that file
on every interaction. It is a bad source of truth:

- machine-ordered, so diffs are noise
- silently drops directives whose label no longer exists in the source
- invisible in code review

**Put directives in the source as pragmas.** They travel with the code, they
are reviewable, and they cannot desynchronise from the function they apply to.
Use `set_directive_*` in Tcl only for things you genuinely want to *sweep*
without editing source.

## 1.3 The minimum viable build script

```tcl
open_project -reset proj
set_top      my_kernel
add_files    src/my_kernel.cpp -cflags "-I../common"
add_files -tb tb/tb_my_kernel.cpp -cflags "-I../common"

open_solution -reset solution1
set_part      xczu7ev-ffvc1156-2-e
create_clock  -period 3.33 -name default

csim_design  -clean
csynth_design
cosim_design -rtl verilog
export_design -format ip_catalog
exit
```

Four things people get wrong here:

1. **`-cflags` is per-`add_files`.** There is no project-wide include path.
2. **`csim_design -clean`.** Without it, a stale object file from before a
   header change links happily and lies to you.
3. **`exit` at the end.** Without it the tool drops to an interactive prompt
   and your CI job hangs until it times out.
4. **`add_files -tb` for testbench files.** Add a TB with plain `add_files` and
   HLS tries to synthesise `std::vector` and `<iostream>`, producing a wall of
   errors that look like tool bugs.

## 1.4 Passing arguments

```bash
vitis_hls -f build.tcl -tclargs example=03_line_buffer_sobel stage=csynth period=2.5
```

`$::argv` holds the list after `-tclargs`. Positional args become unreadable
fast; this repo parses `key=value` pairs — see `hls::parse_args` in
[scripts/hls_lib.tcl](../scripts/hls_lib.tcl).

## 1.5 Scraping results

The human-readable `.rpt` has no stable format. The XML beside it does:

```
<proj>/<solution>/syn/report/<top>_csynth.xml
```

Tags worth pulling: `TargetClockPeriod`, `EstimatedClockPeriod`,
`WorstCaseLatency`, `PipelineInitiationInterval`, `BRAM_18K`, `DSP`, `FF`,
`LUT`, `URAM`. (`DSP` was `DSP48E` before ~2020 — normalise both.)

**Per-loop II is NOT in that XML.** Vitis HLS hoists each pipelined loop into
`<top>_Pipeline_<LOOP_LABEL>` and the achieved II appears only in that
sub-function's `.rpt`; the top-level report just says `* Loop: N/A`. Scan every
`*_csynth.rpt` in the report directory for the loop table:

```
|- LOOP_STREAM  |  6| 2075526|   7|   1|   1| 1 ~ 2075521| yes|
   name          min   max    lat  ach tgt    count       pipelined
```

`hls::parse_loop_ii` / `hls::check_ii` do this and fail the build when a loop
misses its target. Without it a silent II=1 → II=2 regression halves your frame
rate and nothing reports it.

**Gate the csim log too.** `csim_design` *does* fail on a read from an empty
stream, but data **left over** in a stream is only a warning and still exits 0
— and over-production deadlocks a downstream consumer just as effectively. See
[docs/04 §4.2](04-deadlock-playbook.md#what-csim-actually-does-on-an-empty-read).
`hls::check_csim_log` greps `<proj>/<sol>/csim/report/<top>_csim.log`.

**Make a missed clock estimate a hard failure.** HLS only *warns* when the
estimate exceeds the target, and an HLS estimate is optimistic — if csynth
misses, place-and-route misses by more. `hls::print_summary` returns non-zero
in that case and `build.tcl` propagates it to the shell.

## 1.6 Design-space exploration

**Do not loop over solutions inside one `vitis_hls -f` invocation.** The tool
caches the C parse per project, so a sweep that varies a `-D` define silently
reuses the first parse and every point returns identical numbers. One process
per point is slower and correct.

[scripts/sweep.py](../scripts/sweep.py) does this:

```bash
./scripts/sweep.py --example 03_line_buffer_sobel --period 2.0,2.5,3.0,3.33,4.0
./scripts/sweep.py --example 05_split_join_skew --define SKEW_DEPTH=2,64,512 --stage cosim
./scripts/sweep.py --example 04_dataflow_pipeline --define UNROLL=1,2,4 --period 3.33,5.0
```

It writes a CSV, prints a sorted table, and — importantly for the deadlock
examples — **records a per-point timeout as `HANG` instead of aborting the
sweep**. Finding the FIFO depth at which a design starts to deadlock is a
binary search over timeouts, so the driver has to treat a timeout as data.

## 1.7 Global config worth setting on every video project

```tcl
config_dataflow -strict_mode error       ;# canonical-form violations = build failure
config_dataflow -default_channel fifo    ;# streams, not ping-pong buffers
config_interface -m_axi_addr64=false     ;# 32-bit addresses on Zynq-7000
```

`-strict_mode error` is the highest-value line in the list. Without it, HLS
**silently declines** to apply DATAFLOW when your region is non-canonical: you
get a correct design that is 4× too slow and no error message.

Wrap each in `catch {}` — option names have moved between releases and a
missing option should not fail the build.

## 1.8 Packaging IP reproducibly

```tcl
export_design -format ip_catalog \
    -vendor "myco" -library "video" -version "1.0" -display_name $top
```

Set a stable VLNV. If you let it default, every rebuild produces a new
identity and your Vivado block design loses the connection.

## 1.9 CI shape

```bash
make regress          # csim + csynth every example, keep going on failure
```

The pattern that matters: **keep going after a failure, then fail at the end.**
A regression that stops at the first error hides the other five.

Gate on, in order of value:
1. csim exit code
2. csynth success
3. estimated clock ≤ target
4. achieved II == expected II (scrape it; a silent II=2 is the most common
   performance regression)
5. resource deltas against a checked-in baseline CSV

## 1.10 Things that will waste your afternoon

| Symptom | Cause |
|---|---|
| Sweep points all identical | Looped solutions inside one tool invocation; C parse was cached |
| CI job times out, no output | Missing `exit` at the end of the Tcl script |
| Wall of STL errors | Testbench added with `add_files` instead of `add_files -tb` |
| Directives "don't apply" | Label in `directives.tcl` no longer matches a loop label in source |
| Different results GUI vs. script | GUI solution has leftover directives your script doesn't set — always `open_solution -reset` |
| `csim` passes, `csynth` fails on the same file | `__SYNTHESIS__` is defined only for csynth; check your `#if`s |
| `csynth` works but `csim` cannot link (`cannot find crt1.o`, `unknown type [0x13] section '.relr.dyn'`) | Vitis ≤ 2023.2 bundles binutils 2.37, which cannot read glibc ≥ 2.38 shared objects. See below |

### The csim linker failure on Ubuntu 24.04+

```
ld: /lib/x86_64-linux-gnu/libm.so.6: unknown type [0x13] section `.relr.dyn'
ld: skipping incompatible /lib/x86_64-linux-gnu/libm.so.6 ...
ld: cannot find crt1.o: No such file or directory
```

Vitis HLS ≤ 2023.2 links csim with the binutils 2.37 in
`tps/lnx64/binutils-2.37`, which predates glibc 2.38's `.relr.dyn` relocation
format. **csynth is unaffected** — it never links against the system libc — so
the symptom is "synthesis works, csim cannot build", which reads like a
testbench problem and is not.

Fix without touching the Xilinx install:

```bash
export COMPILER_PATH=/usr/bin                  # use the system ld
export LIBRARY_PATH=/usr/lib/x86_64-linux-gnu  # let it find crt1.o/crti.o
```

`COMPILER_PATH` is how gcc locates its auxiliary tools, so this redirects
`collect2` to the system linker while leaving everything else alone. The
[Makefile](../Makefile) sets both by default; `make HLS_LD_FIX=0 ...` disables
it on a machine that does not need it. Verified on Vitis HLS 2023.2 /
Ubuntu 24.04 / glibc 2.39.
