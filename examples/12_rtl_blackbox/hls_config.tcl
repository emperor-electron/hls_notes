set CFG(top)      normalize
set CFG(srcs)     {src/normalize.cpp}
set CFG(blackbox) {src/fx_divide.json}
set CFG(tb)       {tb/tb_normalize.cpp}

# NOTE: src/fx_divide_model.cpp is NOT in CFG(srcs). The blackbox JSON names it
# as the C model and HLS adds it itself; listing it twice is a duplicate-symbol
# link error in csim.

set CFG(cflags)  "-DMAX_ROWS=1080 -DMAX_COLS=1920"
set CFG(tbflags) "-DMAX_ROWS=1080 -DMAX_COLS=1920"

# Cosim is the ONLY stage that runs the real SystemVerilog.
set CFG(cosim_args) "-trace_level none"
