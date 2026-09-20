# 03 — Line buffer + 3×3 window (Sobel)

The structural template for **every** neighbourhood filter. Convolution,
median, erosion/dilation, demosaic and bilateral all reuse this skeleton and
change only `sobel_mag()`.

## Build

```bash
make csim EX=03_line_buffer_sobel
```

## The timing skeleton

A window centred on `(y,x)` needs row `y+1`, so output lags input by
**one line + one pixel**. The loop therefore runs `rows*cols + cols + 1` times:

```
iteration        0 ................ rows*cols ....... rows*cols+cols+1
reads input      |################################|
writes output              |################################|
                           ^ lag = cols + 1
```

The trailing iterations read nothing but **must still advance the column
counter**, or the bottom-right corner is corrupted and nothing else is — the
signature bug of hand-rolled line buffers.

## Directives that are load-bearing

| Directive | Effect if you omit it |
|---|---|
| `ARRAY_PARTITION variable=linebuf complete dim=1` | Both line reads serialise onto one BRAM → **II=2 or worse** |
| `ARRAY_PARTITION variable=win complete dim=0` | The 9 window taps become a memory → II collapses |
| `BIND_STORAGE ... type=ram_2p` | Read+write at the same address may serialise |
| `PIPELINE II=1` | Sequential execution, ~10× slower |

Do **not** partition `linebuf` on `dim=2` — that turns 1920 pixels into 1920
registers.

## Things this example is deliberate about

- **Replicate (clamp-to-edge) borders**, not zero padding. Zero padding draws a
  bright rectangle around the whole image that people then chase as a kernel bug.
- **`|gx|+|gy|`**, not `sqrt(gx²+gy²)`. Within 12%, costs two adders instead of
  two multipliers and a sqrt core.
- **Saturate** the magnitude. Wrapping turns the strongest edges black.
- **No runtime divide/modulo.** `(i-lag)/cols` is ~30 cycles and instantly kills
  `II=1`. Carry counters instead.
- **`ox`/`oy` are not `static`.** Statics would survive a resolution change and
  desynchronise the output permanently.

## Test coverage worth copying

The testbench runs the same frame **twice in a row**. A line buffer that works
on frame 1 and corrupts the top two rows of frame 2 (stale data from the
previous frame leaking through the border logic) is the single most common
defect in this pattern, and a single-frame test never sees it.

## Library alternatives

`hls::LineBuffer<>`/`hls::Window<>` (`hls_video.h`) and `xf::cv` (Vitis Vision)
do this for you. Use them — but their shift semantics
(`shift_pixels_up` vs `shift_pixels_down`, `insert_bottom_row` vs
`insert_top_row`) are a well-known source of vertically flipped kernels, so
know what this code does before you delegate it.
