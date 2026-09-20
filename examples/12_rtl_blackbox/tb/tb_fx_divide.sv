`timescale 1 ns / 1 ps
// ============================================================================
// Standalone unit test for rtl/fx_divide.sv.
//
// WHY THIS EXISTS SEPARATELY FROM COSIM.
//
// cosim is the only thing that proves the C model and the RTL agree -- but it
// takes ~40 s per run, reports a divergence as "88 mismatching pixels" three
// layers away from the cause, and rebuilds the whole design each time. This
// testbench runs in about a second and compares the divider directly against
// the same expression the C model uses.
//
// Develop the RTL against THIS, then use cosim as the integration check. The
// first version of fx_divide.sv passed csim (which runs the C model, not the
// RTL) and failed cosim; this testbench would have caught it immediately.
//
// Run:  make sim-sv EX=12_rtl_blackbox TB_TOP=tb_fx_divide
// ============================================================================
module tb_fx_divide;
  localparam W=32, FRAC=16;
  logic clk=0, rst=1, ce=1, start=0, cont=1;
  logic done, idle, ready, vld;
  logic [W-1:0] num, den, quot;
  always #5 clk = ~clk;

  fx_divide #(.W(W), .FRAC(FRAC)) dut (
    .ap_clk(clk), .ap_rst(rst), .ap_ce(ce),
    .ap_start(start), .ap_done(done), .ap_idle(idle),
    .ap_ready(ready), .ap_continue(cont),
    .num(num), .den(den), .quot(quot), .quot_vld(vld));

  int errs = 0;
  task automatic run(input int unsigned n, d);
    longint unsigned expect_q;
    expect_q = (d == 0) ? 64'hFFFFFFFF
                        : ((longint'(n) << FRAC) / d) & 64'hFFFFFFFF;
    @(posedge clk); num <= n; den <= d; start <= 1;
    wait (ready == 1); @(posedge clk); start <= 0;
    if (quot !== expect_q[31:0]) begin
      $display("  FAIL %0d/%0d -> got %0d expected %0d", n, d, quot, expect_q[31:0]);
      errs++;
    end else
      $display("  ok   %0d/%0d = %0d (%.4f)", n, d, quot, real'(quot)/65536.0);
    wait (idle == 1); @(posedge clk);
  endtask

  initial begin
    repeat(4) @(posedge clk); rst = 0; repeat(2) @(posedge clk);
    run(255,1); run(255,255); run(255,128); run(255,3); run(255,7);
    run(1,255); run(0,255); run(255,0); run(0,0); run(1,1);
    run(65535,255); run(255,2); run(100,7); run(255,200);
    if (errs) $display("\n*** FAIL: %0d errors", errs);
    else      $display("\n*** PASS");
    $finish;
  end
  initial begin #500us; $display("*** FAIL: TIMEOUT"); $finish; end
endmodule
