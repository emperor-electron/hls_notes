set CFG(top)  roi_stats
set CFG(srcs) {src/roi_stats.cpp}
set CFG(tb)   {tb/tb_roi_stats.cpp}

set CFG(cflags)  "-DMAX_ROWS=1080 -DMAX_COLS=1920"
set CFG(tbflags) "-DMAX_ROWS=1080 -DMAX_COLS=1920"

set CFG(cosim_args) "-trace_level none"
