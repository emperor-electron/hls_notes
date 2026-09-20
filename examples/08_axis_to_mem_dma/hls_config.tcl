set CFG(top)  axis_to_mem
set CFG(srcs) {src/axis_to_mem.cpp}
set CFG(tb)   {tb/tb_axis_to_mem.cpp}

set CFG(cflags)  "-DMAX_ROWS=64 -DMAX_COLS=64"
set CFG(tbflags) "-DMAX_ROWS=64 -DMAX_COLS=64"

# cosim of an m_axi design needs the `depth=` on the INTERFACE pragma to cover
# the whole buffer the TB allocates, including the stride padding, or you get
# an out-of-bounds abort that looks like a design bug.
set CFG(cosim_args) "-trace_level none"
