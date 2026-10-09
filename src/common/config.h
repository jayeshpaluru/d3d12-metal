// SPDX-License-Identifier: LGPL-2.1-or-later
// Layer options. Every D3D12METAL_<KEY> environment variable can also be set in d3d12metal.conf, next to
// d3d12.dll (key=value lines, '#' comments, `key` in lower case: log=1, log_file=/unix/path, trace=1,
// stats=1, dump_failed=/dir, cache_dir=...). A game started by Steam does not see the environment of the shell,
// the file is the way to configure it. The file is read once; an environment variable overrides it.
// D3D12METAL_CONF names another file.
#pragma once

#include <cstddef>

namespace d3d12m {

// The value of option `key` ("LOG_FILE"), or null when it is not set.
const char *config_get(const char *key);
// True when the option is set to something other than "" or "0".
bool config_flag(const char *key);

// Calls `fn(name, value, user)` for each entry of the configuration file (name "D3D12METAL_<KEY>"), so the
// unix side of a Wine process, which has its own environment, can be told.
void config_for_each_file_entry(void (*fn)(const char *name, const char *value, void *user), void *user);
// Where the file was read from ("" when there is none).
const char *config_file_path();

} // namespace d3d12m

// C entry points for the Wine transport (src/bridge/wine).
extern "C" int d3d12m_option_enabled(const char *key);
extern "C" void d3d12m_log_text(const char *text, size_t length);
