# 10 — Line buffers and sliding windows

Worked examples: [03 (3×3 Sobel)](../examples/03_line_buffer_sobel) ·
[10 (N-tap, storage variants)](../examples/10_storage_binding)

## 10.1 Why a line buffer exists

A stream delivers pixels in raster order. A window centred on `(y,x)` needs
pixels from row `y+1`, which has not arrived yet. So you delay the stream by
enough rows to have the whole window in hand at once.

For an `N×N` window you need **`N-1` line delays**, not `N` — the current
pixel is the `N`th row, still in flight.

## 10.2 The universal skeleton

```
H    = (N-1)/2                     # half window
lag  = H*cols + H                  # output trails input by this many beats
loop i in [0, rows*cols + lag):
    if (i < rows*cols)  pix = src.read()
    read column ix from all N-1 line buffers
    push the column down (line k <- line k+1, last line <- pix)
    shift the window left, insert the new column on the right
    if (i >= lag)       dst.write(f(window))
    advance ix, iy      <-- on EVERY iteration
    advance ox, oy      <-- only when emitting
```

Four things that are always true and always bite:

1. **The trailing `lag` iterations read nothing but must still advance the
   column counter.** Forget it and the bottom-right corner is corrupted and
   nothing else — the signature bug of hand-rolled line buffers.
2. **Never derive `ox`/`oy` with `/` and `%` on a runtime `cols`.** ~30 cycles;
   `II=1` becomes impossible. Carry counters.
3. **`ox`/`oy` must not be `static`.** Statics survive a resolution change and
   desynchronise the output permanently.
4. **Test two frames back to back.** A line buffer that works on frame 1 and
   corrupts the top rows of frame 2 (stale data leaking through the border
   logic) is the most common defect here, and a single-frame test never sees it.

## 10.3 Getting `II=1`

The window needs `N-1` line reads **in the same cycle**. That means `N-1`
independent memories:

```c
static u8_t linebuf[NLINES][MAX_COLS];
#pragma HLS ARRAY_PARTITION variable=linebuf complete dim=1   // load-bearing
static u8_t win[N][N];
#pragma HLS ARRAY_PARTITION variable=win complete dim=0       // registers
```

| Omission | Result |
|---|---|
| `dim=1` partition on `linebuf` | Reads serialise onto one memory → `II = ceil(NLINES/2)` |
| `dim=0` partition on `win` | The window becomes a memory → II collapses |
| `type=ram_2p` binding | Read+write at the same address may serialise |

Never partition `dim=2` — that turns 1920 pixels into 1920 registers.

Keep the shift a **cascade** (`line k ← line k+1`, newest ← `pix`). Each memory
then does exactly 1 read + 1 write per cycle at the same address, which is
exactly what `ram_2p` provides. A pattern needing 2R+1W forces a true dual-port
or a replicated copy, doubling your memory count.

## 10.4 Border strategies

| Strategy | Cost | When |
|---|---|---|
| **Replicate** (clamp-to-edge) | `N²` muxes | **Default.** Right for almost everything |
| Zero pad | free | Almost never — see below |
| Mirror | `N²` muxes + index maths | Matching an OpenCV reference exactly |
| Crop | free | When the consumer tolerates an `(rows-2H)×(cols-2H)` frame |

Zero padding invents a hard black border and lights up a bright rectangle
around the entire image, which people then chase as a kernel bug. Replicate on
a 3×3 costs nine 8-bit muxes — nothing.

Implement replicate by clamping the *tap index*, not by writing padding into
the line buffer:

```c
int rr = r, cc = c;
if (oy == 0        && r == 0) rr = 1;
if (oy == rows - 1 && r == 2) rr = 1;
if (ox == 0        && c == 0) cc = 1;
if (ox == cols - 1 && c == 2) cc = 1;
w[r][c] = win[rr][cc];
```

Note this scales badly: at `N=15` a runtime-indexed tap read becomes 15 muxes
of 15 inputs, pure LUTs. Past about 7×7, prefer crop-and-pad-outside, or handle
the edge rows with a separate simpler path.

## 10.5 Cheaper windows

### Separate the kernel

A 15×15 Gaussian as a 2D convolution is 225 multipliers. Separated into 15×1
then 1×15 it is 30 — and if the coefficients are powers of two, zero.
**Check separability before writing the 2D form.** Gaussian, box, Sobel and
most smoothing kernels are separable; median and bilateral are not.

Separation costs one extra pass, so you either run two kernels back to back in
a `DATAFLOW` region (two sets of line buffers — the vertical pass needs them,
the horizontal pass needs none) or fold both into one loop.

### Running sums for box filters

An `N×N` box filter is `O(1)` per pixel regardless of `N`:

```
colsum[x] += new_pixel - pixel_leaving_the_window   # one line buffer of sums
running  += colsum[x+H] - colsum[x-H-1]             # one register
```

You keep **one** line buffer of column sums (wider — `8+log2(N)` bits) instead
of `N-1` line buffers of pixels, and the arithmetic is two adds. For a 15×15
box that is 1 memory and 2 adders instead of 14 memories and a 225-input adder
tree.

The catch: it only works for kernels where terms can be removed as cheaply as
added — box/mean, and (with more care) integral-image tricks. Not Gaussian, not
median, not max.

### Morphology: running min/max

Erosion and dilation over an `N`-wide window are `O(1)` per pixel with the
van Herk–Gil–Werman algorithm (two passes of prefix/suffix extrema). Worth it
above about `N=7`; below that a straight comparator tree is smaller.

### Dilated (atrous) kernels

A dilated `3×3` with rate `R` samples rows `y-R, y, y+R`, so it needs `2R` line
buffers but only 9 taps. The BRAM cost scales with the *dilation*, not the tap
count — a rate-8 dilated 3×3 costs the same memory as a 17×17 window.

## 10.6 Multi-pixel-per-beat windows

Required above ~1080p60 (see
[docs/03 §3.3](03-backpressure.md#33-budgeting-can-the-pipeline-absorb-the-source)).
Packing `P` pixels per beat:

- line buffers become `MAX_COLS/P` deep and `P×8` bits wide — **same total
  bits, same BRAM**
- the window needs `P+2` columns (for a 3-wide kernel) to produce `P` outputs
  per beat
- horizontal neighbours *inside* a beat are free; only the pixels at the beat
  boundary need the previous beat held in a register
- `cols` must be a multiple of `P`, or you handle a partial final beat
- TLAST arrives `P×` less often

This changes the window logic, so decide it before writing the kernel, not after.

## 10.7 Library options

`hls::LineBuffer<>` / `hls::Window<>` (`hls_video.h`) and `xf::cv::Mat`
(Vitis Vision) implement all of this. They are fine to use — and their shift
semantics (`shift_pixels_up` vs `shift_pixels_down`, `insert_bottom_row` vs
`insert_top_row`) are a well-known source of vertically flipped kernels. Know
what [example 03](../examples/03_line_buffer_sobel) does before delegating it.

`xf::cv::Mat` is a stream wrapper, so every dataflow rule in
[docs/04](04-deadlock-playbook.md) applies to Vitis Vision pipelines unchanged.

## 10.8 Sizing

At `MAX_COLS=1920` and 8 bits, one line is 15,360 bits and lands in exactly one
BRAM18 (83% utilised), so **BRAM18 count == line-buffer count**:

| Window | Line buffers | BRAM18 | |
|---|---|---|---|
| 3×3 | 2 | 2 | measured |
| 5×5 | 4 | 4 | derived |
| 7×7 | 6 | 6 | measured |
| 15×15 | 14 | 14 | measured |
| 3×3 dilated R=8 | 16 | 16 | derived — span is `2R+1` rows |

Measured rows are from [example 10](../examples/10_storage_binding) at
`PACKED=0`, `STORAGE=1`, on `xczu7ev-ffvc1156-2-e` / Vitis HLS 2023.2
(`NTAPS=3,7,15`). The derived rows apply the same one-BRAM18-per-line rule,
which held exactly across all three measured points.

A dilated 3×3 at rate `R` samples rows `y-R, y, y+R`, spanning `2R+1` rows — so
its memory cost follows the **dilation**, not the tap count. At `R=8` it costs
the same as a 17×17 window while doing 9 multiply-accumulates.

**Set `MAX_COLS` to what you actually support.** Leaving it at 4096 on a 1080p
design doubles every line buffer for nothing. And see
[docs/11 §11.5](11-memory-resources.md#115-pack-lines-into-width-not-into-count)
— packing those `N-1` buffers into the *width* of one memory cut URAM from 14
to 2 and halved the LUT/FF cost, measured.
