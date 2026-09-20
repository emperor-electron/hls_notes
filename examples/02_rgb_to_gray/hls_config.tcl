set CFG(top)  rgb_to_gray
set CFG(srcs) {src/rgb_to_gray.cpp}
set CFG(tb)   {tb/tb_rgb_to_gray.cpp}

# Keep the compile-time maxima small for the regression build so the reported
# latency corresponds to the TB's frame. Bump these (or override from a sweep)
# when you want the real 1080p numbers.
set CFG(cflags)  "-DMAX_ROWS=1080 -DMAX_COLS=1920"
set CFG(tbflags) "-DMAX_ROWS=1080 -DMAX_COLS=1920"

# This one DOES co-simulate: the bounded loop reaches a completion point even
# under ap_ctrl_none.
set CFG(cosim_args) "-trace_level none"
