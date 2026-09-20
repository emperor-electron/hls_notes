# =============================================================================
# build.tcl -- one build script for every example in this repo.
#
# Usage:
#   vitis_hls -f scripts/build.tcl -tclargs example=01_axis_passthrough stage=csynth
#   vivado_hls -f scripts/build.tcl -tclargs example=03_line_buffer_sobel stage=all
#
# Recognised key=value tclargs (all optional except `example`):
#   example=<dir>        directory under examples/          (required)
#   stage=<s>            csim|csynth|cosim|export|all       (default csynth)
#   part=<xcpart>        override target part
#   period=<ns>          override clock period
#   solution=<name>      solution name                      (default solution1)
#   reset=0|1            wipe the solution first            (default 1)
#   csv=<path>           append a QoR row to this CSV
#   define=<K=V>         extra -D passed to the C compiler (repeatable via
#                        define2=, define3=, ... because tclargs keys are unique)
#
# WHY A SCRIPT AND NOT THE GUI
# ----------------------------
# The GUI stores directives in solution1/directives.tcl, which it rewrites on
# every interaction. That file is not a good source of truth: it is machine
# ordered, it silently drops directives whose label no longer exists, and it
# makes code review impossible. Everything here is either a pragma in the
# source (preferred -- travels with the code) or an explicit line in the
# per-example hls_config.tcl (for directives you want to sweep).
# =============================================================================

set THIS_DIR  [file dirname [file normalize [info script]]]
set REPO_ROOT [file dirname $THIS_DIR]
source [file join $THIS_DIR hls_lib.tcl]

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
array set opt {
    example  ""
    stage    csynth
    part     xczu7ev-ffvc1156-2-e
    period   3.33
    solution solution1
    reset    1
    csv      ""
    flow     ""
}
hls::parse_args $::argv opt

if {$opt(example) eq ""} { hls::die "example=<dir> is required" }

set EX_DIR [file join $REPO_ROOT examples $opt(example)]
if {![file isdirectory $EX_DIR]} { hls::die "no such example: $EX_DIR" }

# ---------------------------------------------------------------------------
# Per-example configuration.
#
# hls_config.tcl must set, at minimum:
#   set CFG(top)  <top function name>
#   set CFG(srcs) {list of synthesisable sources, relative to the example dir}
# and may set:
#   CFG(tb)        testbench sources
#   CFG(cflags)    extra compiler flags for synthesis sources
#   CFG(tbflags)   extra compiler flags for TB sources
#   CFG(part)      part override
#   CFG(period)    clock period override
#   CFG(directives) a script evaluated after open_solution
#   CFG(cosim_args) extra args to cosim_design
# ---------------------------------------------------------------------------
array set CFG {
    top        ""
    srcs       {}
    tb         {}
    cflags     ""
    tbflags    ""
    part       ""
    period     ""
    directives ""
    cosim_args ""
    csim_args  ""
    expect_ii  0
}
set cfg_file [file join $EX_DIR hls_config.tcl]
if {![file exists $cfg_file]} { hls::die "missing $cfg_file" }
source $cfg_file

if {$CFG(top) eq ""} { hls::die "hls_config.tcl did not set CFG(top)" }

# scripts/sweep.py passes swept -D defines through the environment rather than
# through tclargs, because tclargs keys must be unique and a sweep may want
# several defines at once. They apply to BOTH the synthesis sources and the
# testbench -- if a define changes the DUT's behaviour the golden model in the
# TB has to see it too, or csim will "fail" for the wrong reason.
if {[info exists ::env(HLS_SWEEP_CFLAGS)] && $::env(HLS_SWEEP_CFLAGS) ne ""} {
    hls::info_ "sweep cflags: $::env(HLS_SWEEP_CFLAGS)"
    append CFG(cflags)  " $::env(HLS_SWEEP_CFLAGS)"
    append CFG(tbflags) " $::env(HLS_SWEEP_CFLAGS)"
}

# Precedence: command line  >  hls_config.tcl  >  defaults above.
proc given {key} {
    foreach a [hls::tclargs $::argv] {
        if {[string match "$key=*" $a]} { return 1 }
    }
    return 0
}
set PART   [expr {[given part]   ? $opt(part)   : ($CFG(part)   ne "" ? $CFG(part)   : $opt(part))}]
set PERIOD [expr {[given period] ? $opt(period) : ($CFG(period) ne "" ? $CFG(period) : $opt(period))}]

hls::info_ "example  : $opt(example)"
hls::info_ "top      : $CFG(top)"
hls::info_ "part     : $PART"
hls::info_ "period   : ${PERIOD}ns"
hls::info_ "stage    : $opt(stage)"
hls::info_ "tool     : [hls::flavour] [hls::version_year]"

# ---------------------------------------------------------------------------
# Project
#
# The project lives inside the example directory as `hls_proj` and is
# .gitignore'd. Keeping builds next to their sources means `rm -rf` cleanup is
# obvious and two examples can never stomp on each other's solution names.
# ---------------------------------------------------------------------------
set PROJ [file join $EX_DIR hls_proj]

open_project -reset $PROJ
set_top $CFG(top)

# -I for the shared headers. Note -I lives in the *cflags* of add_files, NOT in
# a global; there is no project-wide include path in the HLS Tcl API.
set INC "-I[file join $REPO_ROOT common] -I[file join $EX_DIR src]"

foreach f $CFG(srcs) {
    set p [file join $EX_DIR $f]
    if {![file exists $p]} { hls::die "source not found: $p" }
    add_files $p -cflags "$INC $CFG(cflags)"
}

foreach f $CFG(tb) {
    set p [file join $EX_DIR $f]
    if {![file exists $p]} { hls::die "testbench not found: $p" }
    # TB files get their own flags. -Wno-unknown-pragmas keeps g++ quiet about
    # the HLS pragmas it sees through included headers during csim.
    add_files -tb $p -cflags "$INC $CFG(tbflags) -Wno-unknown-pragmas"
}

if {$opt(reset)} {
    open_solution -reset $opt(solution)
} else {
    open_solution $opt(solution)
}

set_part $PART
create_clock -period $PERIOD -name default

# ---------------------------------------------------------------------------
# Global config knobs worth setting for every video design.
# ---------------------------------------------------------------------------

# Do not let HLS infer an ap_ctrl_hs block-level handshake when the design is
# meant to be free-running -- but only where the example asked for it. The
# per-example pragmas own this, so nothing global here. See docs/02.

# config_interface: on 2020.2+ this controls the default m_axi behaviour.
# `-m_axi_addr64` off keeps 32-bit addresses on Zynq-7000; on MPSoC you want it
# on. Guarded because the option name moved around.
if {[hls::version_year] >= 2020} {
    catch { config_interface -m_axi_addr64=false }
}

# config_dataflow: `-strict_mode warning` (or `error` on newer tools) makes HLS
# complain loudly about dataflow canonical-form violations instead of silently
# serialising your pipeline. Turning this on is the single highest-value
# global setting in this file.
catch { config_dataflow -default_channel fifo -fifo_depth 2 }
catch { config_dataflow -strict_mode warning }

# Per-example directives (FIFO depth sweeps, array partitioning experiments,
# anything you want to vary without editing the source).
if {$CFG(directives) ne ""} {
    hls::info_ "applying per-example directives"
    eval $CFG(directives)
}

# ---------------------------------------------------------------------------
# Stages
# ---------------------------------------------------------------------------
proc want {s} {
    global opt
    return [expr {$opt(stage) eq "all" || $opt(stage) eq $s}]
}

set rc 0

if {[want csim]} {
    hls::info_ "==== C SIMULATION ===="
    # -clean forces a rebuild of the TB; without it a stale object file from a
    # previous header change will happily link and lie to you.
    if {[catch { eval csim_design -clean $CFG(csim_args) } err]} {
        hls::die "csim failed: $err"
    }
    # csim_design fails on an empty read, but exits 0 on leftover data. Gate
    # on the log ourselves -- see hls::check_csim_log for the full story.
    if {[hls::check_csim_log $PROJ $opt(solution) $CFG(top)]} {
        set rc 1
    }
}

if {[want csynth] || [want cosim] || [want export] || $opt(stage) eq "all"} {
    hls::info_ "==== C SYNTHESIS ===="
    if {[catch { csynth_design } err]} {
        hls::die "csynth failed: $err"
    }
    if {[hls::print_summary $PROJ $opt(solution) $CFG(top)]} {
        set rc 1
    }
    # Per-loop II lives in the <top>_Pipeline_<LABEL> sub-reports, not the
    # top-level one. A silent regression from II=1 to II=2 halves your frame
    # rate and nothing else catches it.
    if {[hls::check_ii $PROJ $opt(solution) $CFG(expect_ii)]} {
        set rc 1
    }
    if {$opt(csv) ne ""} {
        hls::append_csv $opt(csv) $opt(example) $PROJ $opt(solution) $CFG(top)
        hls::info_ "QoR appended to $opt(csv)"
    }
}

if {[want cosim]} {
    hls::info_ "==== C/RTL CO-SIMULATION ===="
    # -trace_level all writes a waveform; it is slow and huge, so it is opt-in
    # per example via CFG(cosim_args). For streaming designs the waveform is
    # usually the only way to see a deadlock, so most of the deadlock examples
    # do enable it.
    if {[catch { eval cosim_design -rtl verilog $CFG(cosim_args) } err]} {
        hls::error_ "cosim failed: $err"
        hls::error_ "  a cosim HANG (rather than a mismatch) almost always means"
        hls::error_ "  a stream deadlock -- see docs/06-deadlock-playbook.md"
        set rc 1
    }
}

if {[want export]} {
    hls::info_ "==== EXPORT IP ===="
    # -format ip_catalog produces a packaged IP for Vivado IPI.
    # Set a stable VLNV so block designs do not break when you rebuild.
    if {[catch {
        export_design -format ip_catalog \
                      -vendor    "hls_notes" \
                      -library   "video" \
                      -version   "1.0" \
                      -display_name $CFG(top)
    } err]} {
        hls::die "export_design failed: $err"
    }
}

hls::info_ "done (rc=$rc)"
exit $rc
