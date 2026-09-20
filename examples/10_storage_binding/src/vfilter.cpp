#include "vfilter.h"

/* ===========================================================================
 * MAKING HLS USE THE MEMORY YOU MEANT
 *
 * An NTAPS-tall vertical filter needs NTAPS-1 line delays. At 1920 columns and
 * 8 bits that is NLINES x 15,360 bits -- the single largest memory in most
 * video kernels, and the one that decides whether your design fits.
 *
 * This example exists to be swept. The arithmetic is deliberately trivial (a
 * vertical box filter) so that essentially all of the reported BRAM/URAM/LUT
 * comes from the line buffer and you can read the cost directly.
 *
 * ---------------------------------------------------------------------------
 * THE THREE DECISIONS
 *
 *   1. SHAPE   -- how the array is declared and partitioned. This decides how
 *                 many INDEPENDENT memories HLS needs.
 *   2. BINDING -- which primitive each memory becomes (BRAM / URAM / LUTRAM).
 *   3. PORTS   -- 1-port vs 2-port, which decides whether you get II=1.
 *
 * Shape comes first and dominates. A badly shaped array cannot be rescued by
 * a binding directive.
 * ======================================================================== */

/* NTAPS must be odd -- an even vertical window has no centre row and the lag
 * arithmetic below would be off by half a line. */
#if (NTAPS % 2) == 0
#error "NTAPS must be odd"
#endif

/* SCOPE NOTE: this kernel deliberately does NOT do border replication.
 *
 * Clamping the vertical taps at the top and bottom of the frame needs a
 * runtime-indexed read of the tap register file, which synthesises to NTAPS
 * muxes of NTAPS inputs each -- pure LUTs, and at NTAPS=15 enough of them to
 * swamp the numbers this example exists to show. Border handling is covered
 * properly in example 03; here it would only obscure the storage cost.
 *
 * Consequence: the first and last (NTAPS-1)/2 rows of each frame are computed
 * against zero padding (frame 1) or against the previous frame's bottom rows
 * (frame 2 onward). The testbench therefore checks interior rows only, and
 * says so.
 */
void vfilter(gray_stream_t &src, gray_stream_t &dst,
             ap_uint<16> rows, ap_uint<16> cols)
{
#pragma HLS INTERFACE axis port=src
#pragma HLS INTERFACE axis port=dst
#pragma HLS INTERFACE s_axilite port=rows bundle=ctrl
#pragma HLS INTERFACE s_axilite port=cols bundle=ctrl
#pragma HLS INTERFACE ap_ctrl_none port=return

    /* ---- 1. SHAPE ------------------------------------------------------
     * [NLINES][MAX_COLS] partitioned completely on dim=1.
     *
     * This is the only shape that works for a line buffer, and it is worth
     * being explicit about why:
     *
     *   - dim=1 complete  -> NLINES separate memories, all readable in the
     *                        same cycle. This is REQUIRED: the filter reads
     *                        one pixel from every line every cycle. Without
     *                        it, NLINES reads queue up on one memory's two
     *                        ports and II becomes ceil(NLINES/2).
     *   - dim=2 untouched -> each line stays a deep, narrow memory, which is
     *                        exactly what a BRAM is good at. Partitioning
     *                        dim=2 would turn 1920 pixels into 1920 registers
     *                        and is never what you want here.
     *
     * The cost of the correct shape: you can no longer amortise. NLINES=7 at
     * 1920x8b is 7 memories of 15 Kb each. Each one wastes most of a BRAM18
     * (18 Kb), so you pay 7 BRAM18 for 105 Kb of data that would fit in 6.
     * That rounding loss is the price of parallel access and it is unavoidable
     * -- see the docs for how to claw it back by widening instead.
     */
#if PACKED == 0
    static u8_t linebuf[NLINES][MAX_COLS];
#pragma HLS ARRAY_PARTITION variable=linebuf complete dim=1
#else
    /* ---- PACKED LAYOUT --------------------------------------------------
     * All NLINES delays live in the WIDTH of one memory, MAX_COLS deep.
     *
     * One read returns every line's pixel at column ix simultaneously, so you
     * still get all NLINES taps per cycle -- the thing complete partitioning
     * was for -- but from a SINGLE memory instead of NLINES of them.
     *
     * This is the difference between "NLINES primitives, each mostly empty"
     * and "one primitive, well filled", and it is the only way to use URAM
     * sensibly for line buffers. See the measured numbers in
     * docs/11-memory-resources.md.
     *
     * The catch: the whole word is read and rewritten every cycle, so the
     * memory must be at least NLINES*8 bits wide. That is fine up to URAM's
     * 72-bit native width and BRAM's 36; beyond that the tool stacks
     * primitives side by side and you are back to paying per line. */
    typedef ap_uint<NLINES * 8> lineword_t;
    static lineword_t linebuf[MAX_COLS];
#endif

    /* ---- 2. BINDING ----------------------------------------------------
     * BIND_STORAGE (Vitis HLS) / RESOURCE (Vivado HLS) names the primitive.
     *
     *   type=ram_2p   two ports: one read, one write, same cycle.
     *   impl=bram     block RAM      -- 18/36 Kb each, thousands available
     *   impl=uram     UltraRAM       -- 288 Kb each, MPSoC/Versal only
     *   impl=lutram   distributed RAM-- built from LUTs, tiny and fast
     *   impl=auto     let HLS decide
     *
     * State it explicitly even when `auto` would pick the same thing. `auto`
     * is a heuristic that changes between tool releases, and a design that
     * silently migrates from BRAM to LUTRAM on a version bump can blow your
     * LUT budget or your timing with no source change to point at.
     */
#if HLS_COMPAT_VITIS
  #if   STORAGE == 1
    #pragma HLS BIND_STORAGE variable=linebuf type=ram_2p impl=bram
  #elif STORAGE == 2
    #pragma HLS BIND_STORAGE variable=linebuf type=ram_2p impl=uram
  #elif STORAGE == 3
    #pragma HLS BIND_STORAGE variable=linebuf type=ram_2p impl=lutram
  #else
    #pragma HLS BIND_STORAGE variable=linebuf type=ram_2p impl=auto
  #endif
#else
  #if   STORAGE == 1
    #pragma HLS RESOURCE variable=linebuf core=RAM_2P_BRAM
  #elif STORAGE == 3
    #pragma HLS RESOURCE variable=linebuf core=RAM_2P_LUTRAM
  #else
    /* Vivado HLS has no URAM core; STORAGE=2 falls back to BRAM there. */
    #pragma HLS RESOURCE variable=linebuf core=RAM_2P_BRAM
  #endif
#endif

    /* The vertical tap register file. NTAPS values read every cycle, so it
     * must be in flip-flops -- complete partition on all dimensions. Leaving
     * this as a memory is the single most common reason a filter that "should"
     * be II=1 is II=4. */
    static u8_t taps[NTAPS];
#pragma HLS ARRAY_PARTITION variable=taps complete dim=0

    ap_uint<16> ix = 0, ox = 0, oy = 0;
    const ap_uint<32> n_in = (ap_uint<32>)rows * cols;
    const ap_uint<32> lag  = (ap_uint<32>)cols * (NLINES / 2) + 0;

VFILTER:
    for (ap_uint<32> i = 0; i < n_in + lag; ++i) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=(MAX_ROWS*MAX_COLS + MAX_COLS*NLINES)
#pragma HLS PIPELINE II=1

        u8_t pix = 0;
        if (i < n_in) pix = src.read().data;

        /* ---- 3. PORTS --------------------------------------------------
         * Each line memory does exactly ONE read and ONE write per iteration,
         * at the same address. That is precisely what a simple dual-port RAM
         * (ram_2p) provides: independent read and write ports.
         *
         * The shift is a cascade: line k takes what line k+1 held. Reading
         * every line and then writing every line keeps each memory at 1R+1W.
         * Writing line k from line k+1 inside a loop that also reads line k
         * elsewhere would need 2R+1W and force a true dual-port or a
         * replicated copy -- doubling the memory count.
         */
        u8_t col[NLINES];
#pragma HLS ARRAY_PARTITION variable=col complete dim=0

#if PACKED == 0
    READ_LINES:
        for (int k = 0; k < NLINES; ++k) {
#pragma HLS UNROLL
            col[k] = linebuf[k][ix];
        }
    WRITE_LINES:
        for (int k = 0; k < NLINES - 1; ++k) {
#pragma HLS UNROLL
            linebuf[k][ix] = col[k + 1];
        }
        linebuf[NLINES - 1][ix] = pix;
#else
        /* One read, one shift in registers, one write. Same 1R+1W per cycle
         * at the same address as the unpacked version -- still ram_2p. */
        lineword_t rd = linebuf[ix];
    UNPACK:
        for (int k = 0; k < NLINES; ++k) {
#pragma HLS UNROLL
            col[k] = rd.range(8 * k + 7, 8 * k);
        }
        lineword_t wr;
    REPACK:
        for (int k = 0; k < NLINES - 1; ++k) {
#pragma HLS UNROLL
            wr.range(8 * k + 7, 8 * k) = col[k + 1];
        }
        wr.range(8 * (NLINES - 1) + 7, 8 * (NLINES - 1)) = pix;
        linebuf[ix] = wr;
#endif

        /* Vertical taps: oldest line .. newest line, then the incoming pixel. */
    FILL_TAPS:
        for (int k = 0; k < NLINES; ++k) {
#pragma HLS UNROLL
            taps[k] = col[k];
        }
        taps[NTAPS - 1] = pix;

        if (i >= lag) {
            ap_uint<16> sum = 0;
        ACCUM:
            for (int k = 0; k < NTAPS; ++k) {
#pragma HLS UNROLL
                sum += taps[k];
            }
            gray_axis_t o;
            /* Divide by NTAPS via a compile-time reciprocal: NTAPS is a macro,
             * so this is constant-folded into a multiply-and-shift. A runtime
             * divide here would cost ~30 cycles and II=1 would be impossible. */
            o.data = (u8_t)(sum / NTAPS);
            AXIS_SET_KEEP(o);
            o.user = (oy == 0 && ox == 0) ? 1 : 0;
            o.last = (ox == cols - 1)     ? 1 : 0;
            o.id = 0; o.dest = 0;
            dst.write(o);

            if (ox == cols - 1) { ox = 0; oy = (oy == rows - 1) ? (ap_uint<16>)0 : (ap_uint<16>)(oy + 1); }
            else                { ++ox; }
        }

        if (ix == cols - 1) ix = 0; else ++ix;
    }
}
