`timescale 1 ns / 1 ps
// ============================================================================
// tb_cfg_latch -- RTL testbench proving the ap_stable contract.
//
// csim cannot test this. In C there is no such thing as an argument changing
// during a call, so the C testbench can only prove the arithmetic. The thing
// that actually goes wrong in hardware -- the PS rewriting a shadow register
// while the kernel is mid-frame -- only exists in RTL.
//
// Run:  make sim-sv EX=13_ap_ctrl_latch
// ============================================================================

module tb_cfg_latch;

    localparam int ROWS = 16;
    localparam int COLS = 24;
    localparam time TCLK = 10ns;

    logic clk = 0, rst_n = 0;
    always #(TCLK/2) clk = ~clk;

    // ---- shadow registers (stand-in for the PS-written AXI-Lite bank) ------
    logic [15:0] s_rows, s_cols, s_roi_x, s_roi_y, s_roi_w, s_roi_h;
    logic        go, auto_run;
    logic        busy;
    logic [31:0] run_count;

    // ---- latch -> HLS wires -----------------------------------------------
    logic [15:0] cfg_rows, cfg_cols, cfg_roi_x, cfg_roi_y, cfg_roi_w, cfg_roi_h;
    logic        ap_start, ap_done, ap_idle, ap_ready;

    // ---- AXI-Stream into the kernel ---------------------------------------
    logic [7:0] src_TDATA;
    logic       src_TVALID, src_TREADY, src_TLAST, src_TUSER;
    logic [0:0] src_TKEEP = 1'b1, src_TSTRB = 1'b1, src_TID = '0, src_TDEST = '0;

    logic [79:0] out_r;
    logic        out_r_ap_vld;

    // out_r is the flattened roi_result_t, in DECLARATION ORDER, little end
    // first: {max_val[79:72], min_val[71:64], count[63:32], sum[31:0]}.
    // Reordering the struct in the header silently reshuffles this bus, which
    // is why docs/09 says append-only.
    wire [31:0] r_sum   = out_r[31:0];
    wire [31:0] r_count = out_r[63:32];
    wire [7:0]  r_min   = out_r[71:64];
    wire [7:0]  r_max   = out_r[79:72];

    // ------------------------------------------------------------------------
    hls_cfg_latch #(.CFG_W(16)) u_latch (
        .clk(clk), .rst_n(rst_n),
        .s_rows(s_rows), .s_cols(s_cols),
        .s_roi_x(s_roi_x), .s_roi_y(s_roi_y),
        .s_roi_w(s_roi_w), .s_roi_h(s_roi_h),
        .go(go), .auto_run(auto_run), .busy(busy), .run_count(run_count),
        .cfg_rows(cfg_rows), .cfg_cols(cfg_cols),
        .cfg_roi_x(cfg_roi_x), .cfg_roi_y(cfg_roi_y),
        .cfg_roi_w(cfg_roi_w), .cfg_roi_h(cfg_roi_h),
        .ap_start(ap_start), .ap_done(ap_done),
        .ap_idle(ap_idle), .ap_ready(ap_ready)
    );

    roi_stats u_dut (
        .ap_clk(clk), .ap_rst_n(rst_n),
        .ap_start(ap_start), .ap_done(ap_done),
        .ap_idle(ap_idle), .ap_ready(ap_ready),
        .src_TDATA(src_TDATA), .src_TVALID(src_TVALID), .src_TREADY(src_TREADY),
        .src_TKEEP(src_TKEEP), .src_TSTRB(src_TSTRB),
        .src_TUSER(src_TUSER), .src_TLAST(src_TLAST),
        .src_TID(src_TID), .src_TDEST(src_TDEST),
        .rows(cfg_rows), .cols(cfg_cols),
        .roi_x(cfg_roi_x), .roi_y(cfg_roi_y),
        .roi_w(cfg_roi_w), .roi_h(cfg_roi_h),
        .out_r(out_r), .out_r_ap_vld(out_r_ap_vld)
    );

    // ---- pixel source -------------------------------------------------------
    // Deterministic pattern: pixel(y,x) = (y*COLS + x) & 8'hFF
    function automatic logic [7:0] pix(input int y, input int x);
        return (y*COLS + x) & 8'hFF;
    endfunction

    task automatic drive_frame();
        for (int y = 0; y < ROWS; y++) begin
            for (int x = 0; x < COLS; x++) begin
                @(posedge clk);
                src_TDATA  <= pix(y, x);
                src_TUSER  <= (y == 0 && x == 0);
                src_TLAST  <= (x == COLS-1);
                src_TVALID <= 1'b1;
                // Hold until accepted -- AXI4-Stream requires the producer to
                // keep TVALID and TDATA stable until TREADY.
                @(posedge clk);
                while (!src_TREADY) @(posedge clk);
                src_TVALID <= 1'b0;
            end
        end
        @(posedge clk);
        src_TVALID <= 1'b0;
    endtask

    // ---- golden model -------------------------------------------------------
    task automatic expect_roi(input int rx, ry, rw, rh,
                              output longint unsigned e_sum,
                              output int unsigned e_cnt,
                              output int unsigned e_min, e_max);
        e_sum = 0; e_cnt = 0; e_min = 255; e_max = 0;
        for (int y = 0; y < ROWS; y++)
          for (int x = 0; x < COLS; x++)
            if (x >= rx && x < rx+rw && y >= ry && y < ry+rh) begin
                automatic int unsigned p = pix(y, x);
                e_sum += p; e_cnt++;
                if (p < e_min) e_min = p;
                if (p > e_max) e_max = p;
            end
        if (e_cnt == 0) e_min = 0;
    endtask

    int errors = 0;


    task automatic check(input string name, input int rx, ry, rw, rh);
        longint unsigned e_sum; int unsigned e_cnt, e_min, e_max;
        expect_roi(rx, ry, rw, rh, e_sum, e_cnt, e_min, e_max);
        if (r_sum !== e_sum[31:0] || r_count !== e_cnt ||
            r_min !== e_min[7:0]  || r_max !== e_max[7:0]) begin
            $display("  [%-16s] FAIL got sum=%0d count=%0d min=%0d max=%0d",
                     name, r_sum, r_count, r_min, r_max);
            $display("  %-18s      want sum=%0d count=%0d min=%0d max=%0d",
                     "", e_sum, e_cnt, e_min, e_max);
            errors++;
        end else begin
            $display("  [%-16s] ok   sum=%0d count=%0d min=%0d max=%0d",
                     name, r_sum, r_count, r_min, r_max);
        end
    endtask

    // ---- the tests ----------------------------------------------------------
    initial begin
        src_TVALID = 0; src_TDATA = 0; src_TLAST = 0; src_TUSER = 0;
        go = 0; auto_run = 0;
        s_rows = ROWS; s_cols = COLS;
        s_roi_x = 4; s_roi_y = 3; s_roi_w = 8; s_roi_h = 6;

        repeat (5) @(posedge clk);
        rst_n = 1;
        repeat (5) @(posedge clk);

        $display("\n=== TEST 1: baseline run ===");
        @(posedge clk); go <= 1; @(posedge clk); go <= 0;
        wait (ap_start == 1);
        drive_frame();
        wait (out_r_ap_vld == 1);
        @(posedge clk);
        check("baseline", 4, 3, 8, 6);

        wait (ap_idle == 1);
        repeat (3) @(posedge clk);

        // --------------------------------------------------------------------
        $display("\n=== TEST 2: PS rewrites shadow regs MID-RUN ===");
        $display("    The latch must hold the kernel at the OLD geometry.");
        @(posedge clk); go <= 1; @(posedge clk); go <= 0;
        wait (ap_start == 1);

        fork
            drive_frame();
            begin
                // Hostile PS: scribble all over the shadow registers while the
                // kernel is mid-frame. Without the latch these reach the
                // kernel's ap_stable wires and the result is undefined.
                repeat (40) @(posedge clk);
                s_roi_x <= 16'd0;  s_roi_y <= 16'd0;
                s_roi_w <= 16'd24; s_roi_h <= 16'd16;
                $display("    (PS rewrote ROI to full-frame at t=%0t)", $time);
                repeat (30) @(posedge clk);
                s_roi_w <= 16'd1;  s_roi_h <= 16'd1;
                $display("    (PS rewrote ROI to 1x1 at t=%0t)", $time);
            end
        join_any
        wait (out_r_ap_vld == 1);
        @(posedge clk);
        // MUST still be the geometry latched at ap_start.
        check("held-stable", 4, 3, 8, 6);

        wait (ap_idle == 1);
        repeat (3) @(posedge clk);

        // --------------------------------------------------------------------
        $display("\n=== TEST 3: next run picks up the new config ===");
        s_roi_x = 0; s_roi_y = 0; s_roi_w = 24; s_roi_h = 16;
        @(posedge clk); go <= 1; @(posedge clk); go <= 0;
        wait (ap_start == 1);
        drive_frame();
        wait (out_r_ap_vld == 1);
        @(posedge clk);
        check("new-config", 0, 0, 24, 16);

        wait (ap_idle == 1);
        repeat (3) @(posedge clk);

        // --------------------------------------------------------------------
        $display("\n=== TEST 4: go pulse while busy is remembered ===");
        s_roi_x = 2; s_roi_y = 2; s_roi_w = 4; s_roi_h = 4;
        @(posedge clk); go <= 1; @(posedge clk); go <= 0;
        wait (ap_start == 1);
        fork
            drive_frame();
            begin
                repeat (20) @(posedge clk);
                @(posedge clk); go <= 1; @(posedge clk); go <= 0;
                $display("    (second go pulsed while busy)");
            end
        join_any
        wait (out_r_ap_vld == 1);
        @(posedge clk);
        check("run-A", 2, 2, 4, 4);

        // The pending go must launch a second run rather than being dropped.
        wait (ap_start == 1);
        $display("    pending go launched a second run: ok");
        drive_frame();
        wait (out_r_ap_vld == 1);
        @(posedge clk);
        check("run-B-pending", 2, 2, 4, 4);

        repeat (5) @(posedge clk);
        $display("\nruns completed: %0d", run_count);

        if (errors == 0) $display("\n*** PASS\n");
        else             $display("\n*** FAIL: %0d errors\n", errors);
        $finish;
    end

    // Watchdog: a handshake bug shows up as a hang, so bound the run.
    initial begin
        #5ms;
        $display("\n*** FAIL: TIMEOUT -- ap_* handshake is stuck.");
        $display("    ap_start=%b ap_idle=%b ap_ready=%b ap_done=%b",
                 ap_start, ap_idle, ap_ready, ap_done);
        $finish;
    end

endmodule
