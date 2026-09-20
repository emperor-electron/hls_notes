set CFG(top)  ctrl_demo
set CFG(srcs) {src/ctrl_demo.cpp}
set CFG(tb)   {tb/tb_ctrl_demo.cpp}

set CFG(cflags)  "-DMAX_ROWS=1080 -DMAX_COLS=1920"
set CFG(tbflags) "-DMAX_ROWS=1080 -DMAX_COLS=1920"

set CFG(cosim_args) "-trace_level none"
