// SPDX-License-Identifier: LGPL-2.1-or-later
// Log output of the backend: stderr, and D3D12METAL_LOG_FILE when it is set (the front-end writes to the same
// file through mtlb_log_write).
#pragma once

#include <cstddef>

// A whole line (with its newline) to stderr and the log file.
extern "C" void d3d12metal_log_text(const char *text, size_t length);

// printf-style message with the "d3d12-metal: " prefix and a newline.
void backend_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
