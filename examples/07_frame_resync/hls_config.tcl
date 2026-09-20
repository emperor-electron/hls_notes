set CFG(top)  frame_resync
set CFG(srcs) {src/frame_resync.cpp}
set CFG(tb)   {tb/tb_frame_resync.cpp}

set CFG(cflags)  "-DMAX_ROWS=1080 -DMAX_COLS=1920"
set CFG(tbflags) "-DMAX_ROWS=1080 -DMAX_COLS=1920"

# Uses HLS_FOREVER_ON (unbounded while(true) in RTL), so it does not cosim.
# Verify with csim here and in an RTL testbench in Vivado. See docs/08.
set CFG(cosim_args) ""
