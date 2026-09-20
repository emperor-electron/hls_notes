# =============================================================================
# hls_notes -- top-level driver
#
#   make list                       show all examples
#   make csim   EX=01_axis_passthrough
#   make synth  EX=03_line_buffer_sobel
#   make cosim  EX=05_split_join_skew
#   make export EX=02_rgb_to_gray
#   make all    EX=04_dataflow_pipeline
#   make regress                    csim + csynth every example, summary CSV
#   make clean                      remove every hls_proj/ and vitis_hls.log
#
# Override the tool or target:
#   make synth EX=... HLS=vivado_hls PART=xc7z020clg400-1 PERIOD=6.0
# =============================================================================

# ---------------------------------------------------------------------------
# Linker workaround for Vitis HLS <= 2023.2 on glibc >= 2.38 (Ubuntu 24.04+).
#
# csim links with the binutils 2.37 bundled in tps/lnx64, which cannot read the
# .relr.dyn section that modern glibc emits:
#
#   ld: /lib/x86_64-linux-gnu/libm.so.6: unknown type [0x13] section '.relr.dyn'
#   ld: skipping incompatible ... when searching for libm.so.6
#   ld: cannot find crt1.o
#
# csynth is unaffected (it never links against the system libc), so the
# symptom is "synthesis works, csim cannot build" -- which looks like a
# testbench problem and is not.
#
# COMPILER_PATH points the bundled gcc at the system ld; LIBRARY_PATH lets that
# ld find the multiarch CRT objects. Set HLS_LD_FIX=0 to disable on a machine
# that does not need it.
# ---------------------------------------------------------------------------
HLS_LD_FIX        ?= 1
HLS_COMPILER_PATH ?= /usr/bin
HLS_LIBRARY_PATH  ?= /usr/lib/x86_64-linux-gnu
ifeq ($(HLS_LD_FIX),1)
export COMPILER_PATH := $(HLS_COMPILER_PATH)
export LIBRARY_PATH  := $(HLS_LIBRARY_PATH)
endif

HLS     ?= vitis_hls
PART    ?= xczu7ev-ffvc1156-2-e
PERIOD  ?= 3.33
SOL     ?= solution1
BUILD   := scripts/build.tcl
EXAMPLES := $(notdir $(wildcard examples/*))
CSV     ?= build/qor.csv

# `vitis_hls -f` returns the script's exit code, so a failing csynth fails make.
define run_hls
	@mkdir -p build
	$(HLS) -f $(BUILD) -tclargs \
		example=$(EX) stage=$(1) part=$(PART) period=$(PERIOD) \
		solution=$(SOL) csv=$(CSV)
endef

.PHONY: list csim synth cosim export all regress sim-sv clean help
.DEFAULT_GOAL := help

help:
	@sed -n '2,20p' Makefile | sed 's/^# \?//'

list:
	@printf '%s\n' $(EXAMPLES)

check-ex:
	@test -n "$(EX)" || { echo "error: set EX=<example>, see 'make list'"; exit 1; }
	@test -d examples/$(EX) || { echo "error: no examples/$(EX)"; exit 1; }

csim:   check-ex ; $(call run_hls,csim)
synth:  check-ex ; $(call run_hls,csynth)
cosim:  check-ex ; $(call run_hls,cosim)
export: check-ex ; $(call run_hls,export)
all:    check-ex ; $(call run_hls,all)

# Full regression. Keeps going after a failure so you get the whole picture,
# then fails at the end if anything broke.
regress:
	@mkdir -p build
	@rm -f $(CSV) build/regress.log
	@fail=0; \
	for ex in $(EXAMPLES); do \
		echo "=== $$ex ==="; \
		$(HLS) -f $(BUILD) -tclargs example=$$ex stage=csim \
			part=$(PART) period=$(PERIOD) csv=$(CSV) \
			>>build/regress.log 2>&1 || { echo "  CSIM FAILED"; fail=1; continue; }; \
		$(HLS) -f $(BUILD) -tclargs example=$$ex stage=csynth \
			part=$(PART) period=$(PERIOD) csv=$(CSV) \
			>>build/regress.log 2>&1 || { echo "  CSYNTH FAILED"; fail=1; continue; }; \
		echo "  ok"; \
	done; \
	echo; echo "QoR table:"; column -s, -t $(CSV) 2>/dev/null || cat $(CSV); \
	exit $$fail

# ---------------------------------------------------------------------------
# SystemVerilog simulation (xsim) against the HLS-GENERATED RTL.
#
# Some properties cannot be tested in C at all -- the ap_stable contract, the
# ap_ctrl_hs handshake, backpressure with randomised TREADY. Those need an RTL
# testbench driving the real generated Verilog.
#
# Requires the example to have been synthesised first (we do it automatically)
# and to provide tb/*.sv plus optionally rtl/*.sv.
#
#   make sim-sv EX=13_ap_ctrl_latch
# ---------------------------------------------------------------------------
XVLOG  ?= xvlog
XELAB  ?= xelab
XSIM   ?= xsim
SVSOL  ?= sv
TB_TOP ?= tb_cfg_latch

sim-sv: check-ex
	@test -n "$(wildcard examples/$(EX)/tb/*.sv)" || 	  { echo "error: examples/$(EX)/tb/*.sv not found"; exit 1; }
	@echo "=== synthesising $(EX) (solution $(SVSOL)) ==="
	@$(HLS) -f $(BUILD) -tclargs example=$(EX) stage=csynth 	    part=$(PART) period=$(PERIOD) solution=$(SVSOL) >/dev/null
	@echo "=== elaborating and running xsim ==="
	@rm -rf examples/$(EX)/xsim_run && mkdir -p examples/$(EX)/xsim_run
	@cd examples/$(EX)/xsim_run && 	  $(XVLOG) -sv -L uvm 	     ../hls_proj/$(SVSOL)/syn/verilog/*.v 	     $(if $(wildcard $(CURDIR)/examples/$(EX)/rtl/*.sv),../rtl/*.sv,) 	     ../tb/*.sv > xvlog.log 2>&1 || { tail -30 xvlog.log; exit 1; } && 	  $(XELAB) -debug typical $(TB_TOP) -s sim > xelab.log 2>&1 || 	     { tail -30 xelab.log; exit 1; } && 	  $(XSIM) sim -runall | tee xsim.log && 	  ! grep -q "FAIL" xsim.log

clean:
	rm -rf examples/*/hls_proj examples/*/xsim_run build \
	       vitis_hls.log vivado_hls.log \
	       *.log .Xil hs_err_pid*.log
