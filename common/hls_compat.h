#ifndef HLS_COMPAT_H
#define HLS_COMPAT_H

/*
 * Version-portable AXI4-Stream typedefs, plus small helpers shared by every
 * example in this repo (video and non-video alike).
 *
 * The name and layout of the AXI-Stream side-channel struct changed across
 * tool releases, which is the single most common reason a set of notes /
 * examples fails to compile on someone else's install:
 *
 *   Vivado HLS <= 2018.3   ap_axis<D,U,TI,TD>   ->  .data is ap_int<D>
 *                          ap_axiu<D,U,TI,TD>   ->  .data is ap_uint<D>
 *                          (from ap_axi_sdata.h; fields are plain members)
 *
 *   Vivado HLS 2019.x      same as above, but ap_axi_sdata.h started
 *                          pulling in ap_int.h / hls_stream.h itself.
 *
 *   Vitis HLS >= 2020.1    hls::axis<T,U,TI,TD> is the blessed template and
 *                          ap_axiu/ap_axis become aliases of it. Members are
 *                          still .data/.keep/.strb/.user/.last/.id/.dest, but
 *                          hls::axis<> also accepts non-integer T (structs,
 *                          ap_fixed) and has a `.keep_all()` helper.
 *
 * Everything in this repo goes through the typedefs below so the examples move
 * between installs without edits. If you only ever target one tool version,
 * feel free to use ap_axiu<> directly -- but know why it works.
 */

#include <ap_int.h>
#include <ap_axi_sdata.h>
#include <hls_stream.h>

/* __VITIS_HLS__ is defined by vitis_hls 2020.1 and later. */
#if defined(__VITIS_HLS__)
  #define HLS_COMPAT_VITIS 1
#else
  #define HLS_COMPAT_VITIS 0
#endif

/* ---------------------------------------------------------------------------
 * GENERIC stream words.
 *
 * AXI4-Stream is not a video thing -- it is the default point-to-point
 * interface for any streaming HLS kernel: DSP, packet processing, compute
 * offload. These typedefs carry no video semantics; TLAST simply marks the
 * end of a packet/block and TUSER is free for you to define.
 *
 * Use hls::stream<T> with a bare T for INTERNAL dataflow channels, and an
 * ap_axiu<> only where you cross a real AXI boundary -- see
 * docs/05 section 5.1 for why carrying side channels internally wastes FIFO.
 * ------------------------------------------------------------------------ */
typedef ap_axiu<16, 1, 1, 1>  s16_axis_t;   /* 16-bit signed sample stream  */
typedef hls::stream<s16_axis_t> s16_stream_t;

typedef ap_axiu<32, 1, 1, 1>  w32_axis_t;   /* 32-bit word stream           */
typedef hls::stream<w32_axis_t> w32_stream_t;

typedef ap_axiu<8, 1, 1, 1>   byte_axis_t;  /* byte stream (packets, RLE)   */
typedef hls::stream<byte_axis_t> byte_stream_t;

/*
 * Canonical video stream word.
 *
 *   WDATA = 24  -> one RGB888 pixel per beat
 *   WUSER = 1   -> TUSER carries Start-Of-Frame on the first pixel
 *   WID   = 1   -> unused, but must be >= 1 on older tools (a 0-width
 *                  ap_uint is illegal). Width-0 side channels are simply
 *                  not emitted into the RTL, so this costs nothing.
 *   WDEST = 1   -> unused, same reasoning.
 *
 * NOTE: TKEEP/TSTRB exist on ap_axiu whether you want them or not. If the
 * generated RTL has tkeep/tstrb ports and the IP you are connecting to does
 * not, either drive them to all-ones (see AXIS_SET_KEEP below) or strip them
 * with an AXI4-Stream Subset Converter in the block design.
 */
static const int VID_WDATA = 24;
static const int VID_WUSER = 1;
static const int VID_WID   = 1;
static const int VID_WDEST = 1;

typedef ap_axiu<VID_WDATA, VID_WUSER, VID_WID, VID_WDEST> vid_axis_t;
typedef hls::stream<vid_axis_t>                           vid_stream_t;

/* 8-bit single-channel (grayscale / mask) variant. */
typedef ap_axiu<8, VID_WUSER, VID_WID, VID_WDEST>         gray_axis_t;
typedef hls::stream<gray_axis_t>                          gray_stream_t;

/*
 * TKEEP/TSTRB must be all-ones for a "continuous aligned stream", which is
 * what every Xilinx video IP expects. Forgetting this is a classic
 * silent-corruption bug: AXIS Subset Converters and DMA engines will drop
 * bytes whose tkeep bit is 0.
 */
#define AXIS_SET_KEEP(w)   do { (w).keep = -1; (w).strb = -1; } while (0)

/*
 * Pixel helpers. RGB888 packed as {R[23:16], G[15:8], B[7:0]}.
 * This matches the Xilinx Video Frame Buffer / VDMA convention for RGB888.
 */
typedef ap_uint<8>  u8_t;
typedef ap_uint<24> rgb_t;

static inline u8_t rgb_r(rgb_t p) { return p.range(23, 16); }
static inline u8_t rgb_g(rgb_t p) { return p.range(15,  8); }
static inline u8_t rgb_b(rgb_t p) { return p.range( 7,  0); }

static inline rgb_t rgb_pack(u8_t r, u8_t g, u8_t b) {
    rgb_t p;
    p.range(23, 16) = r;
    p.range(15,  8) = g;
    p.range( 7,  0) = b;
    return p;
}

/* ---------------------------------------------------------------------------
 * Free-running loop termination for C simulation.
 *
 * A free-running (ap_ctrl_none) block is an infinite loop in hardware, but an
 * infinite loop in csim just hangs your terminal. __SYNTHESIS__ is defined by
 * the HLS front end and NOT by the csim compiler, so this macro gives you a
 * genuine while(true) in RTL and a drain-the-input loop in csim.
 *
 * CAVEATS -- read these before using it:
 *   - The csim form requires the testbench to have fully populated the input
 *     stream BEFORE calling the DUT. That is fine for a file-driven TB and
 *     wrong for a TB that wants to interleave producer and consumer.
 *   - `.empty()` on a real hls::stream in RTL is a non-blocking peek; using it
 *     as a loop condition in synthesisable code creates a race. Confining it
 *     to the !__SYNTHESIS__ branch keeps it out of the hardware entirely.
 *   - Prefer a BOUNDED loop plus ap_ctrl_none where you can (see
 *     docs/02-axi-stream-interfaces.md, "Three ways to be free-running").
 *     HLS wraps the whole function body in an implicit forever-loop under
 *     ap_ctrl_none, so a bounded loop is still free-running in hardware while
 *     terminating naturally in csim -- and, unlike this macro, it co-simulates.
 * ------------------------------------------------------------------------ */
#ifdef __SYNTHESIS__
  #define HLS_FOREVER_ON(s)  while (true)
#else
  #define HLS_FOREVER_ON(s)  while (!(s).empty())
#endif

#endif /* HLS_COMPAT_H */
