set CFG(top)  histogram
set CFG(srcs) {src/histogram.cpp}
set CFG(tb)   {tb/tb_histogram.cpp}

# METHOD defaults to 1 (forwarding) so `make regress` is green.
#   0 NAIVE  1 FORWARD  2 BANKED  3 UNSAFE
set CFG(cosim_args) "-trace_level none"
