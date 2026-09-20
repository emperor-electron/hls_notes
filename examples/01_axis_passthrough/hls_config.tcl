# Per-example build configuration. Sourced by scripts/build.tcl.

set CFG(top)  axis_passthrough
set CFG(srcs) {src/axis_passthrough.cpp}
set CFG(tb)   {tb/tb_axis_passthrough.cpp}

# Co-simulation of an ap_ctrl_none design with an unbounded while(true) is NOT
# supported: cosim needs the RTL to signal completion and a free-running block
# never does. See examples 04-07 for the bounded-loop form that does cosim.
# Left here as documentation rather than deleted, so nobody "fixes" it.
set CFG(cosim_args) ""
