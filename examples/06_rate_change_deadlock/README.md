# 06 — Data-dependent token counts: the second deadlock family

Example 05 deadlocked on FIFO **depth** — every count matched, the ordering was
wrong. This one deadlocks on FIFO **counts**: a producer whose number of writes
depends on pixel values, feeding a consumer whose number of reads is fixed at
`rows*cols`.

> **The rule:** over one execution of a dataflow region, for every channel,
> `#writes == #reads` — and both must be computable from values that **both
> stages can see**.

"Both stages can see" is the part people miss. Even if the counts happen to
match, if the consumer cannot know the count without inspecting the data, you
have not satisfied the rule — you got lucky on one test vector.

## Three builds

```bash
./scripts/sweep.py --example 06_rate_change_deadlock --define VARIANT=0,1,2 --stage csim
```

| VARIANT | Scheme | Verdict |
|---|---|---|
| 0 | Conditional write, fixed-count read | **Broken.** Fails csim on any sparse frame |
| 1 | Always write, carry a `valid` bit | **Recommended** for video |
| 2 | Variable count + end-of-stream sentinel | Correct; use when compaction ratio matters |

VARIANT 1 costs one bit of FIFO width and fixes throughput at one pixel per
beat. For a video pipeline that is exactly right — the pipeline is paced by the
pixel clock anyway, and a data-dependent rate just creates backpressure jitter
upstream.

## What catches what

| | Depth/ordering deadlock (ex. 05) | Count mismatch (ex. 06) |
|---|---|---|
| **csim** | ❌ misses it — streams are unbounded | ✅ `ERROR [HLS SIM]: an hls::stream is read while empty` |
| **cosim** | ✅ but reports it as a hang | ✅ but reports it as a hang |

**The most useful debugging fact here:** a csim `read while empty` failure
means a **count** bug; a cosim **hang with clean csim** means a **depth or
ordering** bug.

Verified on Vitis HLS 2023.2 — `VARIANT=0` produces:

```
ERROR [HLS SIM]: an hls::stream is read while empty, which may result in
RTL simulation hanging.
ERROR: [SIM 211-100] 'csim_design' failed: nonzero return value.
```

It's a detector thread, not an inline check — `read()` blocks and the detector
`abort()`s about a second later, once more streams are blocked than there are
tasks running.

Two gaps to know about:

| Situation | Result |
|---|---|
| Read from an empty stream | **abort, exit 1** |
| Data **left over** in a stream | `WARNING ... contains leftover data`, **exit 0** |
| Built `-DALLOW_EMPTY_HLS_STREAM_READS` | Empty read becomes a warning returning a default value |

So over-*production* only warns — and it deadlocks a downstream consumer just
as effectively. `hls::check_csim_log` greps for all three and fails the build;
`build.tcl` calls it after every `csim_design`.

## Test the sparse case, always

The testbench runs a `dense` case where every pixel qualifies — and **VARIANT=0
passes it**. Counts coincidentally match. That is exactly how a count bug
survives code review and bring-up and then fails in the field on the first dark
frame. Test sparse, and test empty (lens cap, black frame between scenes, a
sensor that hasn't started).

## Don't "fix" it with non-blocking access

```c
if (!out.full()) out.write_nb(t);   // DON'T
if (!in.empty()) v = in.read_nb();  // DON'T
```

This converts a deadlock into **silent data loss**, which is strictly worse:
nothing hangs, so nothing alerts you, and the drop rate depends on downstream
timing so it isn't reproducible.

Legitimate uses are narrow — draining a stream during error recovery
([example 07](../07_frame_resync)), or a genuinely optional statistics side
channel where dropping is the specified, counted behaviour. **If you cannot say
in one sentence what the system does when the non-blocking access fails, you
want a blocking access.**
