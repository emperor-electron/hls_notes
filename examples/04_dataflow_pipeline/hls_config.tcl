set CFG(top)  video_pipeline
set CFG(srcs) {src/pipeline.cpp}
set CFG(tb)   {tb/tb_pipeline.cpp}

set CFG(cflags)  "-DMAX_ROWS=1080 -DMAX_COLS=1920"
set CFG(tbflags) "-DMAX_ROWS=1080 -DMAX_COLS=1920"

# Waveform ON for this one. In a dataflow design the waveform is the only way
# to see which channel is full and which stage is blocked. It is slow and
# large; that is the price of being able to debug a hang at all.
set CFG(cosim_args) "-trace_level all"

set CFG(directives) {
    # strict_mode error turns a canonical-form violation from a warning you
    # scroll past into a hard build failure. Worth it on every dataflow design.
    catch { config_dataflow -strict_mode error }
}
