set CFG(top)  vfilter
set CFG(srcs) {src/vfilter.cpp}
set CFG(tb)   {tb/tb_vfilter.cpp}

# Full 1920-wide line buffers so the reported BRAM/URAM numbers are the real
# ones. The testbench frame stays small; MAX_COLS only sizes the memory.
set CFG(cflags)  "-DMAX_ROWS=1080 -DMAX_COLS=1920"
set CFG(tbflags) "-DMAX_ROWS=1080 -DMAX_COLS=1920"

set CFG(cosim_args) "-trace_level none"
