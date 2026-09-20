`timescale 1 ns / 1 ps
// ============================================================================
// fx_divide -- fixed-point restoring divider, hand-written SystemVerilog.
//
// Computes  q = (num << FRAC) / den   as a Q<FRAC> fixed-point quotient,
// using one subtract-and-shift per cycle. Variable-latency: it asserts
// ap_done when the iteration count is exhausted.
//
// This is a plausible reason to hand-write RTL rather than let HLS do it: a
// division in HLS either instantiates a large fully-pipelined divider core or
// serialises your whole pipeline, and neither is what you want when the
// divide is rare. Here the cost is one small subtractor and a counter, and
// the HLS side just waits.
//
// ----------------------------------------------------------------------------
// BLACKBOX PORT CONTRACT -- every one of these is mandatory.
//
//   ap_clk, ap_rst          : one clock, one ACTIVE-HIGH reset
//   ap_ce                   : clock enable. NOT optional -- the JSON field
//                             module_clock_enable is required and must name a
//                             real port. Omit it and you get:
//                               ERROR: [HLS 214-145] No 'module_clock_enable'
//                               in 'rtl_common_signal'
//   ap_start/done/idle/ready/continue : the ap_ctrl_chain handshake
//   <data ports>            : one per C function parameter, and NOTHING else.
//                             An RTL port not named in the JSON is an error:
//                               ERROR: [HLS 200-654] Cannot find blackbox RTL
//                               port 'x' in the json file
//                             This is why a blackbox can never own an external
//                             pin -- see rtl/README or docs/13.
// ============================================================================

// ----------------------------------------------------------------------------
// NOTE THE PORT LIST STYLE.
//
// HLS parses the blackbox module header with a Verilog-2001 parser. It does
// NOT understand SystemVerilog port declarations: given `input logic [31:0] x`
// it reads the *keyword* `logic` as the port name and fails with
//
//   ERROR: [HLS 200-654] Cannot find blackbox RTL port 'logic' in the json file
//
// once per port. So the PORT LIST must be plain Verilog-2001 -- `wire`/`reg`,
// and `parameter` without a type. The module BODY can be full SystemVerilog
// (typedef enum, always_ff, unique case, logic, etc.), because the body is only
// ever read by the RTL simulator and by Vivado synthesis, both of which speak
// SystemVerilog. Keep the .sv extension.
// ----------------------------------------------------------------------------
module fx_divide #(
    parameter W    = 32,   // operand width
    parameter FRAC = 16    // fractional bits in the quotient
) (
    input  wire          ap_clk,
    input  wire          ap_rst,
    input  wire          ap_ce,

    input  wire          ap_start,
    output wire          ap_done,
    output wire          ap_idle,
    output wire          ap_ready,
    input  wire          ap_continue,

    input  wire [W-1:0]  num,
    input  wire [W-1:0]  den,
    output wire [W-1:0]  quot,
    output wire          quot_vld
);

    typedef enum logic [1:0] { S_IDLE, S_RUN, S_DONE } state_t;
    state_t state;

    // ------------------------------------------------------------------------
    // Restoring division, done properly.
    //
    // We compute  Q = (num << FRAC) / den.  The dividend is W+FRAC bits wide,
    // so the quotient can be up to W+FRAC bits; we produce all of them and
    // return the low W, which is exactly what the C model's
    //   (ap_uint<64>)(((ap_uint<64>)num << FRAC) / den) & 0xFFFFFFFF
    // does.
    //
    // The invariant that makes this work: the remainder is always strictly
    // less than the divisor before each step, so after shifting one dividend
    // bit in it is less than 2*den and fits in W+1 bits.
    //
    // A NOTE ON THE VERSION THIS REPLACED. The first attempt kept the whole
    // pre-scaled dividend in a 2W-bit remainder and compared that entire
    // register against the divisor each iteration. That comparison is
    // meaningless -- the remainder starts enormously larger than the divisor,
    // so `fits` is always 1, the subtraction never reduces anything, and the
    // register overflows. csim passed (it runs the C model). csynth passed.
    // Only cosim caught it, as "88 mismatching pixels". That is the whole
    // reason docs/13 insists you cosim a blackbox.
    // ------------------------------------------------------------------------
    localparam int DW   = W + FRAC;   // dividend / quotient width
    localparam int ITER = DW;

    reg [DW-1:0]               a_reg;   // dividend, consumed MSB-first
    reg [W:0]                  rem;     // running remainder, W+1 bits
    reg [DW-1:0]               q_reg;   // quotient, built LSB-last
    reg [W-1:0]                d_reg;   // divisor
    reg [$clog2(ITER+1)-1:0]   cnt;

    // One restoring step.
    wire [W:0] rem_next = {rem[W-1:0], a_reg[DW-1]};
    wire       fits     = (rem_next >= {1'b0, d_reg});
    wire [W:0] rem_sub  = rem_next - {1'b0, d_reg};

    always_ff @(posedge ap_clk) begin
        if (ap_rst) begin
            state <= S_IDLE;
            a_reg <= '0;
            rem   <= '0;
            q_reg <= '0;
            d_reg <= '0;
            cnt   <= '0;
        end else if (ap_ce) begin
            unique case (state)

                S_IDLE: begin
                    if (ap_start) begin
                        a_reg <= {num, {FRAC{1'b0}}};   // num << FRAC
                        rem   <= '0;
                        q_reg <= '0;
                        d_reg <= den;
                        cnt   <= '0;
                        state <= S_RUN;
                    end
                end

                S_RUN: begin
                    if (d_reg == '0) begin
                        // Divide by zero: saturate. The C model MUST do the
                        // same or csim and cosim disagree on exactly the input
                        // nobody tested.
                        q_reg <= '1;
                        state <= S_DONE;
                    end else begin
                        rem   <= fits ? rem_sub : rem_next;
                        q_reg <= {q_reg[DW-2:0], fits};
                        a_reg <= {a_reg[DW-2:0], 1'b0};
                        cnt   <= cnt + 1'b1;
                        if (cnt == ITER[$clog2(ITER+1)-1:0] - 1)
                            state <= S_DONE;
                    end
                end

                S_DONE: begin
                    if (ap_continue) state <= S_IDLE;
                end

            endcase
        end
    end

    // quot_vld is aligned with ap_done, not a cycle behind it: HLS samples the
    // data when the handshake says it is valid, so a registered strobe that
    // lags ap_done by a cycle hands it stale data.
    assign quot     = q_reg[W-1:0];
    assign quot_vld = (state == S_DONE);
    assign ap_idle  = (state == S_IDLE) && !ap_start;
    assign ap_done  = (state == S_DONE);
    assign ap_ready = (state == S_DONE) && ap_continue;

endmodule
