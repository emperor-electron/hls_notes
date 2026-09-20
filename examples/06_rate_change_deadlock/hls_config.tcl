set CFG(top)  feature_scan
set CFG(srcs) {src/feature_scan.cpp}
set CFG(tb)   {tb/tb_feature_scan.cpp}

# VARIANT defaults to 1 (the recommended fix) so `make regress` is green.
# Override to see the failure:
#   HLS_SWEEP_CFLAGS=-DVARIANT=0 make csim EX=06_rate_change_deadlock
set CFG(cflags)  "-DMAX_ROWS=64 -DMAX_COLS=64"
set CFG(tbflags) "-DMAX_ROWS=64 -DMAX_COLS=64"

set CFG(cosim_args) "-trace_level all"

set CFG(directives) {
    catch { config_dataflow -strict_mode error }
}
