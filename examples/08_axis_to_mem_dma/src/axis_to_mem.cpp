#include "axis_to_mem.h"
#include <string.h>

/* ===========================================================================
 * AXI4-Stream in, AXI4 master out -- writing a frame to DDR.
 *
 * This is the boundary where a video pipeline stops being a pipeline and
 * starts being a memory system, and the failure modes change completely:
 *   - backpressure now comes from the AXI interconnect and the DDR
 *     controller, which is bursty and unpredictable, not from a neighbouring
 *     HLS block with a known FIFO depth;
 *   - the latency you must absorb is tens to hundreds of cycles, not one line;
 *   - and you can now deadlock against something you do not control.
 *
 * If you can use the Xilinx Video Frame Buffer Write IP instead of writing
 * this, do. It is already verified against every DDR controller Xilinx ships.
 * Write your own when you need a layout or a side effect it does not support.
 * ======================================================================== */

/* -------------------------------------------------------------------------
 * Stage 1: gather one line from the stream into a local buffer.
 * ---------------------------------------------------------------------- */
static void gather_line(vid_stream_t &src, memword_t line[MAX_COLS], int cols)
{
#pragma HLS INLINE off
GATHER:
    for (int x = 0; x < cols; ++x) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_COLS
#pragma HLS PIPELINE II=1
        line[x] = (memword_t)(ap_uint<32>)src.read().data;
    }
}

/* -------------------------------------------------------------------------
 * Stage 2: burst the line out to DDR.
 * ---------------------------------------------------------------------- */
static void burst_line(const memword_t line[MAX_COLS], memword_t *mem,
                       int y, int stride, int cols)
{
#pragma HLS INLINE off
    /* The address is computed HERE, inside the stage, not at the call site.
     * Writing `burst_line(line, mem + y*stride, cols)` in the dataflow region
     * is an expression between stage calls, which breaks canonical form
     * (rule 4) and earns you:
     *   WARNING: [HLS 214-113] Either use an argument of the function or
     *            declare the variable inside the dataflow loop body
     * HLS may still build it, but the pointer arithmetic becomes a
     * loop-carried value shared by the stages -- exactly the invisible
     * dependency DATAFLOW does not model. Pass the scalars, do the maths
     * inside. */
    memword_t *dst = mem + (long)y * (long)stride;


    /* memcpy is the most reliable way to get a real AXI burst out of HLS.
     * A hand-written loop CAN infer a burst, but only if HLS can prove the
     * accesses are sequential, contiguous, and of known length -- and any of
     * the following quietly breaks that proof:
     *   - a conditional inside the loop
     *   - a non-unit stride
     *   - an access pattern HLS cannot linearise (e.g. mem[y*stride + x]
     *     where both y and x vary in the same loop)
     *   - reading and writing the same m_axi bundle in one loop
     * When burst inference fails you do not get an error. You get single-beat
     * AXI transactions: ~16x the latency, ~1/16 the bandwidth, and a design
     * that misses frame rate for no visible reason. ALWAYS check the synthesis
     * log for "Burst read/write of length N" and confirm N is what you expect.
     */
    memcpy(dst, line, (size_t)cols * sizeof(memword_t));
}

/* ======================================================================== */

void axis_to_mem(vid_stream_t &src, memword_t *mem,
                 int rows, int cols, int stride)
{
#pragma HLS INTERFACE axis port=src

    /* -- The AXI4 master --------------------------------------------------
     * depth=      csim/cosim only. It tells the simulator how big the buffer
     *             behind the pointer is. It has NO effect on hardware. Getting
     *             it wrong gives you an out-of-bounds abort in cosim that
     *             looks like a design bug and is not.
     *
     * offset=slave  puts the base address in an AXI4-Lite register, so software
     *             sets it at runtime. The alternative, offset=direct, makes it
     *             a top-level port you must drive from PL.
     *
     * max_write_burst_length=256  is the AXI4 maximum. HLS defaults to 16,
     *             which costs you ~16x the address-phase overhead. Raise it.
     *             But note: AXI4 bursts MUST NOT cross a 4 KB boundary. HLS
     *             handles the split for you; the interconnect will not forgive
     *             you if you bypass HLS and do it yourself.
     *
     * num_write_outstanding=8  lets the block issue the next burst before the
     *             previous write response arrives. Without outstanding writes
     *             you serialise on DDR round-trip latency (~100+ cycles) once
     *             per line, which at 1080p is ~108k wasted cycles per frame.
     *             Each outstanding transaction costs a buffer slot, sized by
     *             max_write_burst_length -- so 8 x 256 x 32 bits = 64 Kb of
     *             BRAM. That is the actual trade: BRAM for bandwidth.
     */
#pragma HLS INTERFACE m_axi port=mem offset=slave bundle=gmem \
        depth=(MAX_ROWS*MAX_COLS) \
        max_write_burst_length=256 num_write_outstanding=8

#pragma HLS INTERFACE s_axilite port=mem    bundle=ctrl
#pragma HLS INTERFACE s_axilite port=rows   bundle=ctrl
#pragma HLS INTERFACE s_axilite port=cols   bundle=ctrl
#pragma HLS INTERFACE s_axilite port=stride bundle=ctrl

    /* -- Block-level protocol ---------------------------------------------
     * ap_ctrl_hs here, NOT ap_ctrl_none.
     *
     * This is the one place in the repo where the handshake is right. A frame
     * writer is a per-frame operation with a destination address that software
     * chooses: software sets `mem`, pulses ap_start, waits for ap_done, then
     * flips to the other buffer. That is a genuine call/return, and ap_done is
     * how software knows the frame is safe to read.
     *
     * Using ap_ctrl_none here would mean software has no way to know when a
     * frame is complete, so it would race the writer and tear.
     */
#pragma HLS INTERFACE s_axilite port=return bundle=ctrl

ROWS:
    /* The loop counter of a loop containing DATAFLOW must be a plain `int`
     * declared in the loop header and initialised to 0. An ap_uint<> counter
     * gets you:
     *   WARNING: [HLS 214-107] Since the loop counter is not declared in loop
     *            header and/or initialized to '0', the compiler may not
     *            successfully process the dataflow loop
     * "may not successfully process" means it can silently fall back to
     * sequential execution -- the exact 2x throughput loss the DATAFLOW was
     * there to prevent, with only a warning to show for it. This is the one
     * place in the repo where a plain `int` is the right choice. */
    for (int y = 0; y < rows; ++y) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=MAX_ROWS

        /* -- DATAFLOW INSIDE A LOOP -------------------------------------
         * This makes gather_line(line N+1) overlap burst_line(line N).
         * Without it the two serialise and you halve your throughput.
         *
         * `line` becomes a PING-PONG buffer (PIPO), not a FIFO: HLS allocates
         * two copies and swaps them each iteration. That is why it must be
         * declared INSIDE the dataflow region and why it must be written
         * completely by the producer before the consumer reads it.
         *
         * Cost: 2 x MAX_COLS x 32 bits = 2 x 1920 x 32 = 123 Kb ~= 4 BRAM36.
         * That is the price of not halving your bandwidth. Note it is charged
         * per instance, so four of these in a design is 16 BRAM36.
         *
         * A common mistake: declaring `line` outside the loop to "save BRAM".
         * That breaks the ping-pong (it is now a single shared buffer with a
         * loop-carried dependency), HLS serialises the stages, and you get the
         * BRAM saving you asked for plus half the performance you did not.
         */
#pragma HLS DATAFLOW
        memword_t line[MAX_COLS];

        gather_line(src, line, cols);
        burst_line(line, mem, y, stride, cols);
    }
}

/* ===========================================================================
 * DEADLOCKING AGAINST THE INTERCONNECT
 *
 * The blocking-graph argument from example 07 assumed the only things that can
 * stall you are HLS streams whose depths you know. An m_axi port breaks that
 * assumption: the AXI interconnect can stall you for reasons that depend on
 * other masters, DDR refresh, and arbitration.
 *
 * That alone is not a deadlock -- it is just slow. It becomes a deadlock when
 * you create a cycle THROUGH memory. The two ways this happens in practice:
 *
 * 1. READ-AFTER-WRITE ON THE SAME BUNDLE, IN THE SAME LOOP.
 *      #pragma HLS INTERFACE m_axi port=mem bundle=gmem
 *      ...
 *      for (...) { mem[i] = f(mem[j]); }
 *    HLS may issue the read burst and the write burst on one bundle whose
 *    read and write channels share a buffer. If the write channel backs up
 *    while the read is waiting, neither completes. Fix: put reads and writes
 *    on SEPARATE bundles (bundle=gmem_rd / bundle=gmem_wr). Two AXI ports cost
 *    you interconnect slaves; a deadlock costs you a product.
 *
 * 2. BACKPRESSURE LOOPING THROUGH THE PS.
 *    Your writer stalls on DDR; DDR is slow because the PS is servicing an
 *    interrupt; the interrupt handler is blocked waiting on your ap_done.
 *    This is a real system-level deadlock and no amount of HLS pragmas fixes
 *    it. The fix is architectural: never let software's forward progress
 *    depend on a frame that software itself is throttling. Give the writer its
 *    own AXI port with a guaranteed QoS slot, or double-buffer so software is
 *    never waiting on the frame currently being written.
 *
 * DIAGNOSING IT: in cosim, a hang with the AXI write-address channel valid and
 * ready low forever is case 1. In hardware, read the AXI performance monitor
 * or just watch whether awvalid is stuck high. If awvalid is high and awready
 * is low, the problem is downstream of you and no HLS change will help.
 * ======================================================================== */
