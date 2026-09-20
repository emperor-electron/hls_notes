#ifndef AXIS_PASSTHROUGH_H
#define AXIS_PASSTHROUGH_H

#include "hls_compat.h"

void axis_passthrough(vid_stream_t &src, vid_stream_t &dst);

void axis_passthrough_ctrl(vid_stream_t &src, vid_stream_t &dst,
                           ap_uint<1> bypass);

#endif
