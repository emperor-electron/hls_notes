#!/usr/bin/env python3
"""
sweep.py -- design-space exploration driver for the examples in this repo.

The HLS Tcl API has no loop-over-solutions primitive, and doing it inside one
`vitis_hls -f` invocation is a trap: the tool caches C parsing per project, so
a sweep that varies a -D define will silently reuse the first parse. Driving it
from outside with one process per point is slower but correct.

Examples
--------
  # Sweep the output FIFO depth of the split/join example and find the
  # smallest depth that does not deadlock in cosim.
  ./scripts/sweep.py --example 05_split_join_skew \\
      --define DEPTH=2,4,8,16,32,64,128 --stage cosim

  # Sweep clock period to find the fastest the Sobel kernel closes at.
  ./scripts/sweep.py --example 03_line_buffer_sobel \\
      --period 2.0,2.5,3.0,3.33,4.0 --stage csynth

  # Cross product of two knobs.
  ./scripts/sweep.py --example 04_dataflow_pipeline \\
      --define UNROLL=1,2,4 --period 3.33,5.0

Output is a CSV plus a printed table sorted by the metric you care about.
"""

import argparse
import csv
import itertools
import os
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Numbers we scrape out of <top>_csynth.xml. The XML schema is stable enough
# across releases that tag names work, but tags do come and go -- anything
# missing just lands as "" rather than crashing the sweep.
# Candidate XML tags per metric, in priority order. Tag names moved between
# releases: latency tags are hyphenated in Vitis HLS ("Worst-caseLatency") and
# were not in older Vivado HLS; DSP was DSP48E before ~2020.
METRICS = [
    ("clk_target", ["TargetClockPeriod"]),
    ("clk_est",    ["EstimatedClockPeriod"]),
    ("lat_worst",  ["Worst-caseLatency", "WorstCaseLatency"]),
    ("lat_best",   ["Best-caseLatency", "BestCaseLatency"]),
    ("interval",   ["Interval-min", "IntervalMin"]),
    ("bram",       ["BRAM_18K"]),
    ("uram",       ["URAM"]),
    ("dsp",        ["DSP", "DSP48E"]),
    ("ff",         ["FF"]),
    ("lut",        ["LUT"]),
]

# Loop table row in *_csynth.rpt (2023.2):
#   |- LOOP_STREAM  |  6| 2075526|  7|  1|  1| 1 ~ 2075521| yes|
#      name          min   max    lat ach tgt   count      pipelined
LOOP_ROW = re.compile(r"^\s*\|[-+ ]+([A-Za-z_][A-Za-z0-9_.]*)\s*\|(.*)$")


def parse_loop_ii(report_dir):
    """Worst achieved II across every pipelined loop.

    NOT available in <top>_csynth.xml. Vitis HLS hoists each pipelined loop
    into a <top>_Pipeline_<LABEL> sub-function and the achieved II appears
    only in that sub-function's .rpt. A scraper that reads only the top-level
    report reports nothing and an II gate built on it passes a design that
    silently regressed to II=4.
    """
    worst = ""
    if not report_dir.is_dir():
        return worst
    for rpt in report_dir.glob("*_csynth.rpt"):
        for line in rpt.read_text(errors="ignore").splitlines():
            m = LOOP_ROW.match(line)
            if not m:
                continue
            cells = [c.strip() for c in m.group(2).split("|") if c.strip()]
            if len(cells) < 7:
                continue
            try:
                ach = int(cells[3])
            except ValueError:
                continue
            if worst == "" or ach > worst:
                worst = ach
    return worst


def parse_csynth(xml_path: Path) -> dict:
    """Pull QoR numbers out of the csynth XML report."""
    out = {k: "" for k, _ in METRICS}
    if not xml_path.exists():
        return out
    try:
        root = ET.parse(xml_path).getroot()
    except ET.ParseError:
        return out
    for key, tags in METRICS:
        # The same tag name can appear under both AreaEstimates/Resources and
        # AvailableResources; the USED figure comes first in document order, so
        # take the first non-empty hit.
        for tag in tags:
            found = False
            for node in root.iter(tag):
                if node.text and node.text.strip():
                    out[key] = node.text.strip()
                    found = True
                    break
            if found:
                break
    return out


def parse_cosim(rpt_dir: Path, top: str) -> str:
    """Return 'pass' / 'fail' / 'hang' / '' from the cosim report."""
    for name in (f"{top}_cosim.rpt", "cosim.rpt"):
        p = rpt_dir / name
        if not p.exists():
            continue
        txt = p.read_text(errors="ignore")
        if re.search(r"\bPass\b", txt):
            return "pass"
        if re.search(r"\bFail\b", txt):
            return "fail"
    return ""


def read_top(example: str) -> str:
    """Scrape CFG(top) out of the example's hls_config.tcl."""
    cfg = REPO / "examples" / example / "hls_config.tcl"
    m = re.search(r"set\s+CFG\(top\)\s+(\S+)", cfg.read_text())
    if not m:
        sys.exit(f"could not find CFG(top) in {cfg}")
    return m.group(1).strip('"{}')


def build_point(args, point, solution):
    """Run one HLS build. Returns (rc, elapsed_seconds, timed_out)."""
    # NOTE: `args.part` is the sweep AXIS (a list, or None when not swept), not
    # a usable part name. The per-point fallback must come from
    # --default-part. Conflating the two passes the literal string "None" to
    # set_part and every point dies with "Part 'None' is not installed".
    tclargs = [
        f"example={args.example}",
        f"stage={args.stage}",
        f"solution={solution}",
        f"part={point.get('part', args.default_part)}",
        f"period={point.get('period', args.default_period)}",
    ]
    # Defines are passed through as a single cflags blob; build.tcl appends
    # CFG(cflags) so we go through an env var the config file can pick up.
    env = dict(os.environ)
    defines = point.get("defines", {})
    if defines:
        env["HLS_SWEEP_CFLAGS"] = " ".join(f"-D{k}={v}" for k, v in defines.items())

    cmd = [args.hls, "-f", str(REPO / "scripts" / "build.tcl"), "-tclargs", *tclargs]
    t0 = time.time()
    try:
        proc = subprocess.run(
            cmd, cwd=REPO, env=env, timeout=args.timeout,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        rc, timed_out, log = proc.returncode, False, proc.stdout
    except subprocess.TimeoutExpired as e:
        # A cosim that never terminates IS the result we are looking for in the
        # deadlock examples -- record it rather than treating it as tooling
        # failure.
        rc, timed_out, log = -1, True, (e.stdout or "")
        if isinstance(log, bytes):
            log = log.decode(errors="ignore")

    logdir = REPO / "build" / "sweep_logs"
    logdir.mkdir(parents=True, exist_ok=True)
    (logdir / f"{args.example}_{solution}.log").write_text(log)
    return rc, time.time() - t0, timed_out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--example", required=True)
    ap.add_argument("--stage", default="csynth",
                    choices=["csim", "csynth", "cosim", "all"])
    ap.add_argument("--define", action="append", default=[],
                    metavar="NAME=v1,v2,v3",
                    help="sweep a preprocessor define; repeatable")
    ap.add_argument("--period", default=None, metavar="p1,p2,...",
                    help="sweep the clock period in ns")
    ap.add_argument("--part", action="append", default=None, metavar="part1,part2",
                    help="sweep the target part")
    ap.add_argument("--default-period", default="3.33")
    ap.add_argument("--default-part", default="xczu7ev-ffvc1156-2-e",
                    help="part used for points that do not sweep --part")
    ap.add_argument("--hls", default=os.environ.get("HLS", "vitis_hls"))
    ap.add_argument("--timeout", type=int, default=3600,
                    help="per-point timeout in seconds; a cosim hang is "
                         "reported as TIMEOUT rather than killing the sweep")
    ap.add_argument("--csv", default=None)
    ap.add_argument("--sort", default="ii",
                    help="metric column to sort the printed table by")
    args = ap.parse_args()

    # Build the cross product of every swept axis.
    axes = []          # list of (kind, name, [values])
    for d in args.define:
        name, _, vals = d.partition("=")
        axes.append(("define", name, vals.split(",")))
    if args.period:
        axes.append(("period", "period", args.period.split(",")))
    if args.part:
        flat = [v for a in args.part for v in a.split(",")]
        axes.append(("part", "part", flat))

    combos = list(itertools.product(*[a[2] for a in axes])) if axes else [()]

    top = read_top(args.example)
    csv_path = Path(args.csv) if args.csv else \
        REPO / "build" / f"sweep_{args.example}.csv"
    csv_path.parent.mkdir(parents=True, exist_ok=True)

    rows = []
    for i, combo in enumerate(combos):
        point = {"defines": {}}
        label_bits = []
        for (kind, name, _), val in zip(axes, combo):
            if kind == "define":
                point["defines"][name] = val
            else:
                point[name] = val
            label_bits.append(f"{name}={val}")
        label = ",".join(label_bits) or "default"
        solution = f"sweep_{i:03d}"

        print(f"[{i+1}/{len(combos)}] {label} ... ", end="", flush=True)
        rc, secs, timed_out = build_point(args, point, solution)

        proj = REPO / "examples" / args.example / "hls_proj" / solution
        row = {"label": label, "solution": solution,
               "status": "TIMEOUT" if timed_out else ("ok" if rc == 0 else f"rc={rc}"),
               "build_s": f"{secs:.0f}"}
        row.update(parse_csynth(proj / "syn" / "report" / f"{top}_csynth.xml"))
        row["ii"] = parse_loop_ii(proj / "syn" / "report")
        if args.stage in ("cosim", "all"):
            row["cosim"] = "HANG" if timed_out else \
                parse_cosim(proj / "sim" / "report", top)
        rows.append(row)
        print(f"{row['status']}  ({secs:.0f}s)")

    if not rows:
        return
    cols = list(rows[0].keys())
    with csv_path.open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader()
        w.writerows(rows)

    def sort_key(r):
        try:
            return (0, float(r.get(args.sort) or "inf"))
        except ValueError:
            return (1, 0.0)
    rows.sort(key=sort_key)

    widths = {c: max(len(c), *(len(str(r.get(c, ""))) for r in rows)) for c in cols}
    print()
    print("  ".join(c.ljust(widths[c]) for c in cols))
    print("  ".join("-" * widths[c] for c in cols))
    for r in rows:
        print("  ".join(str(r.get(c, "")).ljust(widths[c]) for c in cols))
    print(f"\nwrote {csv_path}")


if __name__ == "__main__":
    main()
