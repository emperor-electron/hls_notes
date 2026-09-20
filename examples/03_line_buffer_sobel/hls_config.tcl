set CFG(top)  sobel_3x3
set CFG(srcs) {src/sobel.cpp}
set CFG(tb)   {tb/tb_sobel.cpp}

set CFG(cflags)  "-DMAX_ROWS=1080 -DMAX_COLS=1920"
set CFG(tbflags) "-DMAX_ROWS=1080 -DMAX_COLS=1920"

set CFG(cosim_args) "-trace_level none"

# Example of a directive you would sweep rather than hard-code. Left commented
# because the source already carries the version-correct form.
#
# set CFG(directives) {
#     set_directive_array_partition -type complete -dim 1 "sobel_3x3" linebuf
# }
