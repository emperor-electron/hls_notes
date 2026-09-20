set CFG(top)  ccm
set CFG(srcs) {src/ccm.cpp}
set CFG(tb)   {tb/tb_ccm.cpp}

set CFG(cflags)  "-DMAX_ROWS=1080 -DMAX_COLS=1920"
set CFG(tbflags) "-DMAX_ROWS=1080 -DMAX_COLS=1920"

set CFG(cosim_args) "-trace_level none"
