# =============================================================================
# hls_lib.tcl -- helpers shared by every build script in this repo.
#
# Sourced by scripts/build.tcl. Nothing in here opens a project; these are
# pure utilities so they can also be sourced from an interactive
# `vitis_hls -i` session while you poke at a solution.
# =============================================================================

namespace eval hls {

    # -------------------------------------------------------------------------
    # Logging. The HLS Tcl console interleaves stdout from the C compiler,
    # the scheduler and Tcl itself, so tag our own lines to make them greppable
    # in a CI log.
    # -------------------------------------------------------------------------
    proc log {level msg} {
        puts "\[hls-$level\] $msg"
        flush stdout
    }
    proc info_  {msg} { log INFO  $msg }
    proc warn   {msg} { log WARN  $msg }
    proc error_ {msg} { log ERROR $msg }

    proc die {msg} {
        error_ $msg
        # exit 1 from inside vitis_hls returns 1 to the shell, which is what a
        # Makefile / CI job needs. `error` alone prints a Tcl stack trace and
        # STILL exits 0 on some releases -- always exit explicitly.
        exit 1
    }

    # -------------------------------------------------------------------------
    # Which tool are we running under? The two binaries share ~90% of the Tcl
    # API but differ on config_* options and on default behaviours (e.g. Vitis
    # HLS defaults dataflow channels to PIPO, Vivado HLS to FIFO for streams).
    # -------------------------------------------------------------------------
    proc flavour {} {
        # ::version is set by both tools, e.g. "2019.2" / "2023.2"
        if {[info exists ::env(XILINX_VITIS_HLS)] || \
            [string match "*vitis_hls*" [info nameofexecutable]]} {
            return vitis
        }
        return vivado
    }

    proc version_year {} {
        if {[catch {set v [version -short]} err]} { return 0 }
        if {[regexp {^(\d{4})} $v -> y]} { return $y }
        return 0
    }

    # -------------------------------------------------------------------------
    # Parse "key=value key=value" style tclargs into an array. Lets you do
    #   vitis_hls -f build.tcl -tclargs example=03_line_buffer_sobel stage=csynth
    # which is far easier to read in a Makefile than positional arguments.
    # -------------------------------------------------------------------------
    #
    # NOTE: $::argv is NOT just what follows -tclargs. Depending on the
    # release it may contain the whole command line, including
    # "-f scripts/build.tcl". Vitis HLS 2023.2 does exactly this. So strip
    # everything up to and including a literal "-tclargs" if one is present,
    # and otherwise ignore anything that is not a key=value pair.
    proc tclargs {argv_list} {
        set i [lsearch -exact $argv_list "-tclargs"]
        if {$i >= 0} {
            return [lrange $argv_list [expr {$i + 1}] end]
        }
        set out {}
        foreach a $argv_list {
            if {[regexp {^[A-Za-z_][A-Za-z0-9_]*=} $a]} { lappend out $a }
        }
        return $out
    }

    proc parse_args {argv_list defaults_name} {
        upvar 1 $defaults_name opt
        foreach a [tclargs $argv_list] {
            if {![regexp {^([A-Za-z_][A-Za-z0-9_]*)=(.*)$} $a -> k v]} {
                die "bad tclarg '$a' -- expected key=value"
            }
            set opt($k) $v
        }
    }

    # -------------------------------------------------------------------------
    # Report scraping.
    #
    # The human-readable .rpt is not machine-parseable in a stable way, but the
    # XML beside it is. Path:
    #   <proj>/<solution>/syn/report/<top>_csynth.xml
    # We pull the numbers that actually matter for a video pipeline: II,
    # latency, and whether we blew a BRAM/DSP budget.
    # -------------------------------------------------------------------------
    proc parse_csynth_xml {xml_path} {
        if {![file exists $xml_path]} { return {} }
        set fh [open $xml_path r]
        set x [read $fh]
        close $fh

        # Tag names differ between releases. Notably the latency tags are
        # hyphenated in Vitis HLS ("Worst-caseLatency") and were not in older
        # Vivado HLS ("WorstCaseLatency"), and DSP was DSP48E before ~2020.
        # Each key lists candidate tags in priority order; first hit wins.
        set r [dict create]
        foreach {key tags} {
            clk_target    {TargetClockPeriod}
            clk_estimate  {EstimatedClockPeriod}
            latency_best  {Best-caseLatency BestCaseLatency}
            latency_avg   {Average-caseLatency AverageCaseLatency}
            latency_worst {Worst-caseLatency WorstCaseLatency}
            interval_min  {Interval-min IntervalMin}
            interval_max  {Interval-max IntervalMax}
            bram          {BRAM_18K}
            dsp           {DSP DSP48E}
            ff            {FF}
            lut           {LUT}
            uram          {URAM}
        } {
            foreach tag $tags {
                if {[regexp "<$tag>(\[^<\]*)</$tag>" $x -> val]} {
                    dict set r $key [string trim $val]
                    break
                }
            }
        }
        return $r
    }

    # -------------------------------------------------------------------------
    # Per-loop initiation interval.
    #
    # THIS IS NOT IN THE TOP-LEVEL REPORT. Vitis HLS hoists each pipelined loop
    # into its own sub-function named <top>_Pipeline_<LOOP_LABEL>, and the
    # achieved II only appears in that sub-function's report. The top-level
    # report shows "* Loop: N/A" and the XML has no II tag at all, which is why
    # a scraper that only reads <top>_csynth.xml silently reports nothing and
    # your CI II-gate passes on a design that regressed to II=4.
    #
    # So: scan every *_csynth.rpt in the report directory and pull the loop
    # table rows. Format (2023.2):
    #   |- LOOP_STREAM  |  6| 2075526|  7|  1|  1| 1 ~ 2075521| yes|
    #     name            min   max    lat  ach tgt   count    pipelined
    #
    # Returns a list of {loop_name achieved_ii target_ii pipelined}.
    # -------------------------------------------------------------------------
    proc parse_loop_ii {proj solution} {
        set dir [file join $proj $solution syn report]
        set out {}
        foreach f [glob -nocomplain [file join $dir *_csynth.rpt]] {
            set fh [open $f r]
            set txt [read $fh]
            close $fh
            foreach line [split $txt \n] {
                # Loop rows start with "|- " or "| + " for nested loops.
                if {![regexp {^\s*\|[-+ ]+([A-Za-z_][A-Za-z0-9_.]*)\s*\|(.*)$} \
                        $line -> name rest]} { continue }
                set cells {}
                foreach c [split $rest |] {
                    set c [string trim $c]
                    if {$c ne ""} { lappend cells $c }
                }
                # min max latency achieved target count pipelined
                if {[llength $cells] < 7} { continue }
                set ach  [lindex $cells 3]
                set tgt  [lindex $cells 4]
                set pipe [lindex $cells end]
                if {![string is integer -strict $ach]} { continue }
                lappend out [list $name $ach $tgt $pipe]
            }
        }
        return $out
    }

    # Fail the build if any pipelined loop missed its II target.
    proc check_ii {proj solution {expect 0}} {
        set bad 0
        foreach row [parse_loop_ii $proj $solution] {
            lassign $row name ach tgt pipe
            info_ [format "  loop %-24s II=%-4s target=%-4s pipelined=%s" \
                     $name $ach $tgt $pipe]
            if {[string is integer -strict $tgt] && $ach > $tgt} {
                warn "  loop '$name' MISSED its II target ($ach > $tgt)"
                set bad 1
            }
            if {$expect > 0 && $ach > $expect} {
                warn "  loop '$name' II=$ach exceeds expected $expect"
                set bad 1
            }
        }
        return $bad
    }

    proc print_summary {proj solution top} {
        set xml [file join $proj $solution syn report ${top}_csynth.xml]
        set d [parse_csynth_xml $xml]
        if {[dict size $d] == 0} {
            warn "no csynth XML at $xml"
            return
        }
        info_ "---------------- synthesis summary: $top ----------------"
        foreach k {clk_target clk_estimate latency_best latency_worst \
                   interval_min interval_max bram uram dsp ff lut} {
            if {[dict exists $d $k]} {
                info_ [format "  %-14s %s" $k [dict get $d $k]]
            }
        }
        info_ "--------------------------------------------------------"

        # Timing guard: HLS happily reports a solution that misses timing and
        # only warns. In CI you want that to be a hard failure.
        if {[dict exists $d clk_estimate] && [dict exists $d clk_target]} {
            set est [dict get $d clk_estimate]
            set tgt [dict get $d clk_target]
            if {[string is double -strict $est] && [string is double -strict $tgt]} {
                if {$est > $tgt} {
                    warn "ESTIMATED PERIOD ${est}ns EXCEEDS TARGET ${tgt}ns"
                    warn "  (HLS estimates are optimistic -- if csynth misses,"
                    warn "   place-and-route will miss by more)"
                    return 1
                }
            }
        }
        return 0
    }

    # -------------------------------------------------------------------------
    # csim log gate.
    #
    # csim_design DOES abort on a read from an empty stream:
    #   ERROR [HLS SIM]: an hls::stream is read while empty ...
    # (verified on Vitis HLS 2023.2 -- a detector thread, so it fires about a
    # second in rather than immediately).
    #
    # But it does NOT fail on two related conditions:
    #   - data LEFT OVER in a stream at the end, which is only
    #       WARNING [HLS SIM]: hls::stream 'x' contains leftover data
    #     and still exits 0. That is over-production, and it deadlocks a
    #     downstream consumer just as effectively as under-production.
    #   - anything at all, if the testbench was built with
    #     -DALLOW_EMPTY_HLS_STREAM_READS, which downgrades the empty read to a
    #     warning that returns a default value.
    #
    # So grep the log as well as checking the exit status.
    proc check_csim_log {proj solution top} {
        set candidates [list \
            [file join $proj $solution csim report ${top}_csim.log] \
            [file join $proj $solution csim report csim.log]]
        set log ""
        foreach c $candidates {
            if {[file exists $c]} { set log $c; break }
        }
        if {$log eq ""} {
            warn "no csim log found -- cannot check for stream protocol errors"
            return 0
        }
        set fh [open $log r]
        set txt [read $fh]
        close $fh

        set bad 0
        foreach {pat what} {
            "is read while empty"      "read from an EMPTY stream (token-count mismatch)"
            "contains leftover data"   "LEFTOVER data in a stream (over-production)"
            "deadlock detected"        "DEADLOCK detected"
        } {
            set n 0
            foreach line [split $txt \n] {
                if {[string first $pat $line] >= 0} { incr n }
            }
            if {$n > 0} {
                error_ "csim: $what  (${n} occurrence(s))"
                set bad 1
            }
        }
        if {$bad} {
            error_ "csim reported stream protocol errors but STILL EXITED 0."
            error_ "  See docs/04-deadlock-playbook.md section 4.4."
        }
        return $bad
    }

    # Emit a one-line CSV record so a sweep can be pasted into a spreadsheet.
    proc append_csv {csv_path label proj solution top} {
        set d [parse_csynth_xml \
                 [file join $proj $solution syn report ${top}_csynth.xml]]

        # Create the parent directory. Without this, running build.tcl
        # directly (rather than through the Makefile, which mkdir -p's) dies
        # with "couldn't open ...: no such file or directory" AFTER a
        # successful synthesis -- which reads like a synthesis failure.
        set dir [file dirname $csv_path]
        if {$dir ne "" && ![file isdirectory $dir]} {
            file mkdir $dir
        }

        # Worst achieved II across all pipelined loops -- see parse_loop_ii
        # for why this does not come from the XML.
        set worst_ii ""
        foreach row [parse_loop_ii $proj $solution] {
            set ach [lindex $row 1]
            if {$worst_ii eq "" || $ach > $worst_ii} { set worst_ii $ach }
        }

        set new [expr {![file exists $csv_path]}]
        set fh [open $csv_path a]
        set cols {clk_target clk_estimate latency_worst interval_min \
                  bram uram dsp ff lut}
        if {$new} { puts $fh "label,top,ii,[join $cols ,]" }
        set row [list $label $top $worst_ii]
        foreach c $cols {
            # Deliberately NOT `expr {... ? ... : ...}`. Tcl's expr coerces a
            # numeric-looking string to a double, so the XML's "3.33" comes out
            # as "3.3300000000000001" and your QoR diffs are full of noise.
            # Keep report values as the strings the tool actually wrote.
            if {[dict exists $d $c]} {
                lappend row [dict get $d $c]
            } else {
                lappend row ""
            }
        }
        puts $fh [join $row ,]
        close $fh
    }
}
