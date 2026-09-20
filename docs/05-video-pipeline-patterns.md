# 5 — Video pipeline patterns

## 5.1 The shape of a pipeline

```
sensor/HDMI ─► [resync] ─► [csc] ─► [filter] ─► [csc] ─► [frame buffer] ─► display
                  ▲                     ▲                       ▲
              ex. 07              ex. 03/04/05              ex. 08
```

Design rules that fall out of this:

- **Resync at the ingress**, once. Every downstream block can then be counted.
- **Convert to your working format once**, at the front, and back once at the
  end. Not per-stage.
- **Keep the internal format narrow.** Inside a dataflow region you're on an
  HLS FIFO, not an AXI bus — carrying TKEEP/TSTRB/TID/TDEST between stages is
  pure waste. Strip at ingress, regenerate at egress. At depth 2048 that's
  1 BRAM instead of 4.
- **One frame buffer, at the end.** Every frame buffer is ~2 ms of latency at
  1080p60 and hundreds of MB/s of DDR bandwidth.

## 5.2 The neighbourhood-filter skeleton

Every `N×N` filter has this shape. Only the arithmetic changes.

```
lag = (N/2)·cols + (N/2)
loop i in [0, rows·cols + lag):
    if (i < rows·cols)  pix = src.read()
    shift line buffers at column ix
    shift window left, insert new column
    if (i >= lag)       dst.write(f(window))
    advance ix, iy                  ← on EVERY iteration
    advance ox, oy                  ← only when emitting
```

**The trailing iterations read nothing but must still advance the column
counter.** Forgetting that corrupts the bottom-right corner and nothing else —
the signature bug of hand-rolled line buffers.

Worked out in full in [example 03](../examples/03_line_buffer_sobel).

### Costs

| Window | Line buffers | BRAM18 @ 1920×8b | DSP (separable) |
|---|---|---|---|
| 3×3 | 2 | 2 | 0 (shifts) |
| 5×5 | 4 | 4 | 2 |
| 7×7 | 6 | 6 | 2 |
| 15×15 | 14 | 14 | 2 |

**Separate your kernels.** A 15×15 Gaussian as a 2D convolution is 225
multipliers. Separated into 15×1 then 1×15 it's 30, and if the coefficients are
powers of two it's zero. Check separability before you write the 2D form.

### Border handling

Use **replicate** (clamp-to-edge) by default. Zero padding invents a hard black
border and lights up a bright rectangle around the entire image, which people
then chase as a kernel bug. Replicate costs nine 8-bit muxes on a 3×3 — nothing.

## 5.3 Fixed-point arithmetic

- Scale to a **power of two** so the divide is a shift. Q16 gives ~5 decimal
  digits, far beyond what 8-bit output resolves.
- Make coefficients **sum exactly to the scale factor**. Round each
  independently and white maps to 254 instead of 255 — every image is
  imperceptibly dark, forever.
- **Add half-LSB before the shift.** Truncation bias is invisible in one stage
  and compounds visibly through a pipeline of several.
- **Size accumulators explicitly** (`ap_uint<27>`), don't inherit `int`. On one
  kernel it's noise; replicated nine times in a window filter it's real LUTs.
- **Saturate, don't wrap.** A wrapping magnitude turns the strongest edges
  black — the filter looks broken exactly where it's working hardest.
- Prefer `|gx|+|gy|` (12% error, two adders) or alpha-max-beta-min
  (`0.96·max + 0.40·min`, 4% error, shifts and adds) over `sqrt(gx²+gy²)`
  (two multipliers + a sqrt core + ~8 cycles).

## 5.4 Never divide or modulo by a runtime value

```c
oy = (i - lag) / cols;     // ~30 cycles. II=1 is now impossible.
ox = (i - lag) % cols;
```

Carry counters instead. This applies to `/9` in a box blur too — write it as a
reciprocal multiply (`sum * 7282 >> 16`) so you can see the rounding, even
though HLS will constant-fold a literal `/9` into the same thing.

## 5.5 Frame-level state

Feedback (auto-exposure, auto-white-balance, histogram-driven contrast) cannot
close inside a dataflow region — rule 3. Close it **across frames**:

```c
static stats_t prev_frame_stats;      // survives the implicit restart

void kernel(...) {
#pragma HLS INTERFACE ap_ctrl_none port=return
    gain_t g = compute_gain(prev_frame_stats);   // uses frame N-1
    stats_t s = {0};
    for (...) { apply(g); accumulate(s); }
    prev_frame_stats = s;                        // for frame N+1
}
```

One frame of loop delay. That is fine — the control loop bandwidth you need for
exposure is single-digit Hz, and a frame is 16 ms.

**Latch config on SOF.** With `ap_ctrl_none` there's no call boundary, so HLS
reads an `s_axilite` register whenever the datapath wants it — possibly
mid-frame:

```c
if (w.user) threshold_latched = threshold;
```

## 5.6 Multi-pixel-per-beat

Required above ~1080p60 (see [docs/03 §3.3](03-backpressure.md#33-budgeting-can-the-pipeline-absorb-the-source)).
Pack N pixels into one beat:

- line buffers become `MAX_COLS/N` deep and `N×8` bits wide — **same BRAM**
- the window needs `N + 2` columns, not 3, to produce N outputs per beat
- horizontal neighbours within a beat are free; only the beat-boundary pixels
  need the previous beat held in a register
- TLAST arrives `N×` less often; `cols` must be a multiple of N, or you handle
  a partial final beat

This is a design-time decision. It changes the window logic, so do not discover
it after writing the kernel.

## 5.7 Format notes

| Format | Note |
|---|---|
| RGB888 | 24 b packed `{R[23:16],G[15:8],B[7:0]}` — matches VDMA/Frame Buffer |
| xRGB8888 | 32 b in **memory**. Power-of-two element size keeps bursts aligned; one wasted byte is 8 MB/s at 1080p60, nothing next to the ~500 MB/s the frame costs |
| YUV422 (UYVY) | 16 b/pixel on the wire. Halves your line buffers. Chroma is subsampled — a 3×3 on chroma is 6×3 in luma terms |
| YUV420 | Do not stream this through a line-buffer filter. Convert to 422 or 444 first |
| Bayer | Demosaic is a neighbourhood filter with a 2×2 phase. Track `(y&1, x&1)` |

## 5.8 Useful libraries

- **`hls_video.h`** — `hls::LineBuffer<>`, `hls::Window<>`. Fine to use. Their
  shift semantics (`shift_pixels_up` vs `_down`, `insert_bottom_row` vs
  `_top_row`) are a well-known source of vertically flipped kernels, so know
  what [example 03](../examples/03_line_buffer_sobel) does before delegating.
- **Vitis Vision (`xf::cv`)** — OpenCV-shaped HLS kernels. Good coverage,
  well-tested. Pays off for anything non-trivial (warp, stereo, optical flow).
  Its `xf::cv::Mat` is a stream wrapper, so the dataflow rules in
  [docs/04](04-deadlock-playbook.md) still apply unchanged.
- **Xilinx Video Frame Buffer Read/Write** — use instead of writing your own
  DDR frame buffer unless you need a layout it doesn't support. Already
  verified against every DDR controller Xilinx ships.
