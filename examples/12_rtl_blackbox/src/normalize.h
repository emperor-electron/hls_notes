#ifndef NORMALIZE_H
#define NORMALIZE_H

#include "hls_compat.h"

#ifndef MAX_COLS
#define MAX_COLS 1920
#endif
#ifndef MAX_ROWS
#define MAX_ROWS 1080
#endif

/* Fractional bits in the divider's quotient. Must match the FRAC parameter on
 * the SystemVerilog module -- the JSON has no way to check this for you, so a
 * mismatch is a silent numerical error. Keeping it in a shared header and
 * passing it to the RTL via a parameter override is the only safe pattern. */
#define FX_FRAC 16

/* The blackbox function. Declared here, modelled in fx_divide_model.cpp for
 * csim, and replaced by rtl/fx_divide.sv at csynth. */
void fx_divide(ap_uint<32> num, ap_uint<32> den, ap_uint<32> &quot);

void normalize(gray_stream_t &src, gray_stream_t &dst,
               ap_uint<16> rows, ap_uint<16> cols,
               ap_uint<32> target);

#endif
