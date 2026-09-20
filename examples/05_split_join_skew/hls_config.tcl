set CFG(top)  unsharp_mask
set CFG(srcs) {src/unsharp.cpp}
set CFG(tb)   {tb/tb_unsharp.cpp}

# MAX_COLS deliberately small (64) so that cosim runs in a reasonable time AND
# so the default SKEW_DEPTH (MAX_COLS+8 = 72) is only just big enough --
# sweeping SKEW_DEPTH down by a few tens finds the deadlock cliff immediately.
set CFG(cflags)  "-DMAX_ROWS=64 -DMAX_COLS=64"
set CFG(tbflags) "-DMAX_ROWS=64 -DMAX_COLS=64"

# Waveform on: when this deadlocks you want to see which FIFO is full.
set CFG(cosim_args) "-trace_level all"

set CFG(directives) {
    catch { config_dataflow -strict_mode error }
}
