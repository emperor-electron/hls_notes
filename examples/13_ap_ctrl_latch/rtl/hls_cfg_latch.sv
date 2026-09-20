`timescale 1 ns / 1 ps
// ============================================================================
// hls_cfg_latch -- drive an HLS ap_ctrl_hs block and hold its ap_stable
//                  inputs stable for the duration of each call.
//
// This is the piece that makes `ap_stable` a true statement rather than a
// hopeful one.
//
//   PS  --AXI-Lite-->  [shadow regs]  --+
//                                       |   (this module)
//                           +-----------+------------+
//                           |  latch on the cycle    |
//                           |  ap_start is asserted  |
//                           +-----------+------------+
//                                       |
//                           cfg_* wires (held constant)
//                                       v
//                              +--------------------+
//                    ap_start->|   HLS roi_stats    |->ap_done
//                              +--------------------+
//
// WHY THIS IS NEEDED
// ------------------
// An ap_stable port is a bare wire. HLS reads it whenever it likes and assumes
// it never changes during a call. If the PS writes a shadow register while the
// kernel is mid-frame and that write reaches the wire, part of the datapath
// sees the old value and part the new. There is no error, no warning, and no
// reproducible pattern -- you just get an occasional wrong answer.
//
// So: the PS writes shadow registers at any time; this module copies them to
// the kernel's wires exactly once per call, on the cycle it launches the call.
//
// HANDSHAKE RULES (ap_ctrl_hs), which this module obeys:
//   * ap_start must be held high until ap_ready is asserted. It is a LEVEL,
//     not a pulse. Pulsing it for one cycle is the single most common way to
//     get a block that "sometimes doesn't start".
//   * ap_done is asserted for one cycle (unless ap_continue is used) when the
//     result is valid.
//   * ap_idle is high when the block is doing nothing. It is the safe moment
//     to change anything.
//   * ap_ready indicates the block has consumed its inputs and can accept a
//     new ap_start. For a non-pipelined block it coincides with ap_done.
// ============================================================================

module hls_cfg_latch #(
    parameter CFG_W = 16
) (
    input  wire               clk,
    input  wire               rst_n,

    // ---- shadow registers, written by the PS at any time ------------------
    input  wire [CFG_W-1:0]   s_rows,
    input  wire [CFG_W-1:0]   s_cols,
    input  wire [CFG_W-1:0]   s_roi_x,
    input  wire [CFG_W-1:0]   s_roi_y,
    input  wire [CFG_W-1:0]   s_roi_w,
    input  wire [CFG_W-1:0]   s_roi_h,

    // ---- run control ------------------------------------------------------
    input  wire               go,          // pulse: request one run
    input  wire               auto_run,    // level: run continuously
    output reg                busy,
    output reg  [31:0]        run_count,

    // ---- to the HLS block -------------------------------------------------
    output reg  [CFG_W-1:0]   cfg_rows,
    output reg  [CFG_W-1:0]   cfg_cols,
    output reg  [CFG_W-1:0]   cfg_roi_x,
    output reg  [CFG_W-1:0]   cfg_roi_y,
    output reg  [CFG_W-1:0]   cfg_roi_w,
    output reg  [CFG_W-1:0]   cfg_roi_h,

    output reg                ap_start,
    input  wire               ap_done,
    input  wire               ap_idle,
    input  wire               ap_ready
);

    typedef enum logic [1:0] {
        S_IDLE,     // waiting for a run request
        S_LAUNCH,   // ap_start asserted, waiting for ap_ready
        S_RUN       // running, waiting for ap_done
    } state_t;

    state_t state;

    // A `go` pulse can arrive while we are busy; remember it so the request is
    // not lost. Dropping it silently is how you get "every other trigger is
    // ignored" behaviour that looks like a software bug.
    reg go_pending;

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            state      <= S_IDLE;
            ap_start   <= 1'b0;
            busy       <= 1'b0;
            go_pending <= 1'b0;
            run_count  <= 32'd0;
            cfg_rows   <= '0;
            cfg_cols   <= '0;
            cfg_roi_x  <= '0;
            cfg_roi_y  <= '0;
            cfg_roi_w  <= '0;
            cfg_roi_h  <= '0;
        end else begin
            if (go) go_pending <= 1'b1;

            unique case (state)

                S_IDLE: begin
                    ap_start <= 1'b0;
                    busy     <= 1'b0;
                    // Only launch when the kernel is genuinely idle. Asserting
                    // ap_start while ap_idle is low is not illegal, but it
                    // queues a run with configuration you are about to change,
                    // which defeats the purpose of this module.
                    if ((go_pending || auto_run) && ap_idle) begin
                        // ---- THE LATCH ----------------------------------
                        // Every config value is sampled on the SAME clock
                        // edge that raises ap_start. From here until ap_done
                        // the kernel's inputs cannot move, whatever the PS
                        // does to the shadow registers.
                        cfg_rows   <= s_rows;
                        cfg_cols   <= s_cols;
                        cfg_roi_x  <= s_roi_x;
                        cfg_roi_y  <= s_roi_y;
                        cfg_roi_w  <= s_roi_w;
                        cfg_roi_h  <= s_roi_h;

                        ap_start   <= 1'b1;
                        busy       <= 1'b1;
                        go_pending <= 1'b0;
                        state      <= S_LAUNCH;
                    end
                end

                S_LAUNCH: begin
                    // HOLD ap_start until ap_ready. This is the rule people
                    // break. ap_start is a level; a one-cycle pulse is only
                    // reliable if ap_ready happens to be high that cycle.
                    if (ap_ready) begin
                        ap_start <= 1'b0;

                        // ---- ap_ready and ap_done CAN BE THE SAME CYCLE ----
                        // On a NON-pipelined ap_ctrl_hs block they always are:
                        // the block consumes its inputs and finishes at the
                        // same moment, so both strobe together.
                        //
                        // A naive two-state FSM that consumes ap_ready here
                        // and then waits for ap_done in S_RUN deadlocks -- the
                        // ap_done it is waiting for already went past. The
                        // symptom is that the FIRST run completes correctly
                        // and the block never starts again, which reads like
                        // a software bug and is not.
                        //
                        // Only a PIPELINED block (ap_ready early, ap_done
                        // later, so a new run can be launched while the
                        // previous one drains) actually needs S_RUN.
                        if (ap_done) begin
                            run_count <= run_count + 32'd1;
                            state     <= S_IDLE;
                        end else begin
                            state     <= S_RUN;
                        end
                    end
                end

                S_RUN: begin
                    // Only reached on a pipelined block, where ap_ready came
                    // strictly before ap_done.
                    if (ap_done) begin
                        run_count <= run_count + 32'd1;
                        state     <= S_IDLE;
                    end
                end

            endcase
        end
    end

endmodule
