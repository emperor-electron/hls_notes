set CFG(top)  matmul
set CFG(srcs) {src/matmul.cpp}
set CFG(tb)   {tb/tb_matmul.cpp}
set CFG(cflags)  "-DMAX_DIM=128"
set CFG(tbflags) "-DMAX_DIM=128"
set CFG(cosim_args) "-trace_level none"
