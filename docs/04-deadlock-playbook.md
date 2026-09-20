# 4 — The deadlock playbook

## 4.1 The one rule

> **A deadlock requires a cycle in the blocking graph.**

Draw the graph: a node per stage, an edge A→B meaning "A can block waiting for
B". A single 1-in-1-out block cannot form a cycle by itself — it can only
*participate* in one created elsewhere. So when something hangs, stop staring
at the stalled block and go find the cycle.

The four families below are every cycle you will meet in a video pipeline.

## 4.2 Triage: which bug do I have?

Run csim first. It is seconds, not minutes, and it partitions the problem:

| Observation | Diagnosis | Go to |
|---|---|---|
| csim fails: `ERROR [HLS SIM]: an hls::stream is read while empty` | **Token count mismatch** | [§4.4](#44-family-b--data-dependent-token-counts) |
| csim passes, cosim hangs | **Depth / ordering** — counts match, timing doesn't | [§4.3](#43-family-a--latency-skew-across-a-fan-outjoin) |
| csim passes, cosim passes, hardware emits one frame then stops | **Not a deadlock** — `ap_ctrl_hs` with nobody pulsing `ap_start` | [docs/02 §2.4](02-axi-stream-interfaces.md#24-three-ways-to-be-free-running) |
| Hardware runs but every frame is wrong after one glitch | **Not a deadlock** — permanent desync | [§4.7](#47-the-thing-that-looks-like-a-deadlock-and-isnt) |
| Hangs only at high resolution | Depth sized for the test resolution, not `MAX_COLS` | [§4.3](#43-family-a--latency-skew-across-a-fan-outjoin) |
| `awvalid` stuck high, `awready` low | Cycle through memory or a saturated interconnect | [§4.6](#46-family-d--cycles-through-memory-or-software) |

**Why csim sees one and not the other:** csim runs dataflow stages
sequentially and treats every `hls::stream` as an unbounded `std::deque`. The
`depth=` pragma is ignored entirely. So a *global count* mismatch shows up; a
*depth* or *ordering* problem cannot.

### What csim actually does on an empty read

Verified against `csim_design` on Vitis HLS 2023.2 (example 06, `VARIANT=0`):

```
ERROR [HLS SIM]: an hls::stream is read while empty, which may result in
RTL simulation hanging.
...
ERROR: [SIM 211-100] 'csim_design' failed: nonzero return value.
```

**csim fails the build**, exit code 1. The mechanism is not an inline check: the
read blocks on a condition variable and a detector thread notices after ~1 s
that more streams are blocked than there are running tasks, then `abort()`s.
That is why the failure takes a second to appear and why the message says
"*may* result in RTL simulation hanging".

Two caveats worth knowing:

| Situation | Result |
|---|---|
| Read from an empty stream | `ERROR [HLS SIM]` → **abort, exit 1** |
| Data **left over** in a stream at the end | `WARNING [HLS SIM]: ... contains leftover data` → **no abort, exit 0** |
| Built with `-DALLOW_EMPTY_HLS_STREAM_READS` | Empty read downgraded to a warning, returns a default value, continues |

So over-*production* is only a warning. `hls::check_csim_log` in
[scripts/hls_lib.tcl](../scripts/hls_lib.tcl) greps the csim log for
`is read while empty`, `contains leftover data` and `deadlock detected` and
fails the build on any of them; `build.tcl` calls it after every `csim_design`.

> Do **not** define `__VITIS_HLS__` yourself when compiling a testbench with
> plain `g++`. It is a synthesis-only macro, and defining it puts
> `hls_stream.h` into a "bcsim" mode that real csim does not use — empty reads
> silently return default values and your test passes when it should not.

---

## 4.3 Family A — latency skew across a fan-out/join

**Worked example: [05_split_join_skew](../examples/05_split_join_skew)**

```
                 ┌────────────────────────────┐
                 │   sharp  (lag 0)           │  depth = D
 src ──[fanout]──┤                            ├──[combine]── dst
                 │ sblur ─[blur]─ sblurred    │  lag = L
                 └────────────────────────────┘
```

`combine` reads one token from each branch per iteration, but `blur` cannot
produce its first output until it has consumed `L` inputs.

1. `combine` blocks on `sblurred`
2. → never drains `sharp`
3. → `sharp` fills to depth `D`
4. → `fanout` blocks writing `sharp`
5. → never writes `sblur`
6. → `blur` starves
7. → **cycle**

**Fix:** `D ≥ L + 2`. Nothing else works.

Notably, **swapping the read order in `combine` does not help.** Both reads
must complete before the iteration retires, so whichever is first in C, the
iteration blocks until both tokens exist.

**Finding L:** it is the consumer's startup latency — the number of input
tokens it must absorb before its first output. For an `N×N` window filter,
`L = (N/2)·cols + N/2`. For a full-frame transpose, `L = rows·cols` and you
need a frame buffer, not a FIFO.

**Finding D empirically:**

```bash
./scripts/sweep.py --example 05_split_join_skew \
    --define SKEW_DEPTH=2,16,32,64,66,128 --stage cosim --timeout 900
```

`sweep.py` records a timeout as `HANG` rather than killing the run, precisely
so this sweep is usable.

---

## 4.4 Family B — data-dependent token counts

**Worked example: [06_rate_change_deadlock](../examples/06_rate_change_deadlock)**

```c
// producer
if (pixel >= threshold) out.write(t);        // writes a data-dependent count

// consumer
for (i = 0; i < rows*cols; ++i) in.read();   // reads a fixed count
```

On any frame that isn't fully saturated, the consumer blocks forever.

> **The rule:** over one execution of a dataflow region, for every channel,
> `#writes == #reads` — and both must be computable from values **both stages
> can see**.

"Both stages can see" is the part people miss. Counts that happen to match on
your test vector are luck, not correctness.

**Three fixes:**

| | Scheme | Use when |
|---|---|---|
| 1 | **Tagged**: always write one token, carry a `valid` bit | Video pipelines. Default choice. |
| 2 | **Sentinel**: variable count + explicit end-of-stream token | Compaction ratio genuinely matters |
| 3 | **Side-channel count**: producer writes the count on a second channel first | Consumer needs the count *before* the data |

Tagged costs one bit of FIFO width and fixes throughput at one token per beat.
For video that is exactly right — the pipeline is paced by the pixel clock
anyway, and a data-dependent rate just creates backpressure jitter upstream.

For sentinel, two things must hold: the sentinel is written on **every** path
out of the producer (including early exits and error paths), and the consumer
does **not** also have a fixed trip count.

**Test the sparse case and the empty case.** A dense test vector (everything
qualifies) makes the broken version pass. That is exactly how this bug survives
review and bring-up and then fails on the first black frame.

---

## 4.5 Family C — canonical-form violations

DATAFLOW has seven rules. Break one and HLS either silently declines to apply
DATAFLOW (a 4×-too-slow design, no error) or applies it and produces something
that deadlocks.

1. **Single producer, single consumer** per channel. Need data in two places?
   Add an explicit fan-out stage.
2. **No bypass.** A channel from stage 1 to stage 3 skipping stage 2 breaks the
   form. Pass it *through* stage 2 even if stage 2 ignores it. Yes, that costs
   a FIFO. Pay it.
3. **No feedback.** A channel from a later stage back to an earlier one is
   illegal. Genuine feedback (auto-exposure, AGC) closes **outside** the
   region, across frames, through a static.
4. **Stage calls only.** Between the channel declarations and the end of the
   function: nothing but stage calls. No stray assignment, no conditional
   around a call, no loop over calls. (Scalars passed as arguments are fine —
   that's why `rows`/`cols` can go to every stage.)
5. **Every stage runs every time.** `if (cond) stage_b(...)` is illegal. Push
   the condition *inside* the stage and have it pass data through unchanged
   when disabled — still consuming and producing the same token counts.
6. **Token counts match** (that's Family B).
7. **No static shared between stages.** A static inside one stage is fine (a
   line buffer). A file-scope static touched by two stages is an invisible
   dependency DATAFLOW does not model, and the stages race.

**Make this loud:**

```tcl
config_dataflow -strict_mode error
```

---

## 4.6 Family D — cycles through memory or software

An `m_axi` port is a blocking edge to something you do not control.

**D1 — read-after-write on the same bundle, in one loop:**

```c
#pragma HLS INTERFACE m_axi port=mem bundle=gmem
for (...) { mem[i] = f(mem[j]); }
```

The read and write channels of one bundle can share a buffer. If the write
backs up while the read waits, neither completes.
**Fix: separate bundles** (`gmem_rd` / `gmem_wr`).

**D2 — backpressure looping through the PS:**

Your writer stalls on DDR → DDR is slow because the PS is in an interrupt
handler → the handler is blocked waiting on your `ap_done`. A real system-level
deadlock that no HLS pragma fixes.

**Fix is architectural:** never let software's forward progress depend on a
frame that software itself is throttling. Double-buffer so software is never
waiting on the frame currently being written.

**D3 — an external IP that never issues TREADY** until it sees a TLAST you
never send. Most commonly: a coordinate/statistics list with no TLAST on the
final element. The DMA waits for a packet boundary forever. This is a deadlock
*one hop outside your IP* and is why "my IP is fine, the DMA is broken" tickets
exist. See the one-beat lookahead in
[example 07](../examples/07_frame_resync).

---

## 4.7 The thing that looks like a deadlock and isn't

**Permanent desynchronisation.** A counted block assumes every frame is exactly
`rows*cols` beats. One short frame — sensor glitch, mode change, DMA underrun,
cable reconnect — and it silently consumes the first N lines of frame *K+1* as
the last N lines of frame *K*. It does not hang. It does not error. **Every
frame from then on is wrong, and only a power cycle fixes it.**

That is worse than a deadlock, because a deadlock at least stops and tells you.

**Fix:** resynchronise on the input's TUSER.
[Example 07](../examples/07_frame_resync) does this, and adds status counters
whose *signature* identifies the upstream fault:

| `resyncs` | `eol_mismatch` | Diagnosis |
|---|---|---|
| climbing | ~0 | Upstream truncating frames — glitch, underrun, cable |
| ~0 | climbing | `cols` register disagrees with actual line length — **config bug** |
| climbing | climbing | Resolution changed, register bank not updated |

---

## 4.8 Debugging a live hang

**In cosim.** Enable the waveform (`cosim_design -trace_level all`), open it,
and look at the FIFO status signals between stages — HLS names them after your
stream variables, which is why you should always name your streams:

```c
hls::stream<pix_t> sharp("sharp");   // not: hls::stream<pix_t> sharp;
```

An unnamed stream appears as `hls::stream<...>.0` and you will not know which
one it is.

Then read the pattern:

| Waveform | Meaning |
|---|---|
| FIFO `full` high, its consumer's `ap_idle` low | Consumer is blocked elsewhere — follow *its* inputs |
| FIFO `empty` high, its producer blocked on a *different* FIFO's `full` | You have found the cycle |
| All FIFOs empty, everything stalled on the input port | Not a deadlock — the testbench stopped driving |

**In hardware.** Add an ILA on the stream handshakes. The signature is
diagnostic:

| `tvalid` | `tready` | Meaning |
|---|---|---|
| 1 | 0 | Consumer is blocked. Problem is **downstream**. |
| 0 | 1 | Producer is starved. Problem is **upstream**. |
| 0 | 0 | Both stalled — you are mid-cycle. Walk the graph. |

Walk in the direction the stall points until it loops back on itself. Where it
loops is the cycle.

## 4.9 Prevention checklist

- [ ] `config_dataflow -strict_mode error` set
- [ ] Every stream named in its constructor
- [ ] Every fan-out/join has its skew computed and the FIFO sized `≥ skew + 2`
- [ ] Every skew depth sized for `MAX_COLS`, not the test resolution
- [ ] No conditional `write()` feeding a fixed-count reader
- [ ] Every channel's write count and read count derivable from arguments
- [ ] Reads and writes on separate `m_axi` bundles
- [ ] TLAST asserted on the final beat of every variable-length output
- [ ] Testbench covers: sparse frame, empty frame, truncated frame, two
      consecutive frames, and the maximum resolution
