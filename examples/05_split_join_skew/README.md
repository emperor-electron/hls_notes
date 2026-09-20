# 05 — Split/join latency skew: the classic HLS video deadlock

```
                 ┌──────────────────────────────┐
                 │     sharp  (lag 0)           │  depth = SKEW_DEPTH
 src ──[fanout]──┤                              ├──[combine]── dst
                 │ sblur ─[blur3x3]─ sblurred   │  lag = cols+1
                 └──────────────────────────────┘
```

## The deadlock, step by step

`stage_blur` cannot emit its first output until it has consumed `cols+1`
inputs — a 3×3 window centred on row *y* needs row *y+1*. Meanwhile:

1. `combine` blocks waiting on `sblurred`
2. → it never drains `sharp`
3. → `sharp` fills to its depth
4. → `fanout` blocks writing `sharp`
5. → `fanout` never writes `sblur`
6. → `blur` starves and never produces its first output
7. → **circular wait**

## The fix

```
depth(sharp) ≥ latency_skew + 1 = (cols + 1) + 1
```

Round up, add margin for the blur stage's own pipeline depth.

**The part that bites:** FIFO depth is a *compile-time* constant; the skew is
`cols + 1`, a *runtime* value. Size for `MAX_COLS`, not for the resolution you
are testing at. A design that works at 640×480 and deadlocks the instant
someone selects 1080p is this bug, every time.

## Swapping the read order does not help

```c
pix_t s = sharp.read();
pix_t b = blurred.read();   // swapping these changes nothing
```

Both reads must complete before the iteration retires. Read order only matters
when the two producers are themselves coupled — that's [example 06](../06_rate_change_deadlock).

## Reproduce it

```bash
make csim EX=05_split_join_skew     # passes. proves nothing.
make cosim EX=05_split_join_skew    # passes at the default depth
```

Now find the cliff:

```bash
./scripts/sweep.py --example 05_split_join_skew \
    --define SKEW_DEPTH=2,16,32,64,66,128 --stage cosim --timeout 900
```

Everything at or below `cols+1` reports `HANG`. `sweep.py` records a timeout as
`HANG` rather than killing the run, precisely so this sweep is usable.

## Cost check before you panic about BRAM

| Branch data | Max cols | Depth | Bits | BRAM18 |
|---|---|---|---|---|
| 8-bit gray | 1920 | 1928 | 15 Kb | 1 |
| 24-bit RGB | 1920 | 1928 | 46 Kb | 3 |
| 24-bit RGB | 4096 | 4104 | 98 Kb | 6 |

Skew FIFOs on 8-bit video are cheap. When they stop being cheap, move the
fan-out point *later* so less data is in flight — do not shave the depth and
hope.
