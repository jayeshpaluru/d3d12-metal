#include "common/log.h"

#include <cstdarg>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "bridge/mtlb.h"
#include "common/config.h"
#include "common/platform.h"

namespace d3d12m {

namespace {

bool log_file_enabled()
{
    static const bool enabled = [] {
        const char *path = config_get("LOG_FILE");
        return path && *path;
    }();
    return enabled;
}

void write_line(const char *text, size_t length)
{
    std::fwrite(text, 1, length, stderr);
    if (log_file_enabled())
        mtlb_log_write(0, text, length);
}

// The first line of a log: which process and configuration the rest belongs to.
void write_banner()
{
    char line[1024];
    const char *conf = config_file_path();
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    const int length = std::snprintf(line, sizeof(line),
                                     "d3d12-metal: log started: exe=%s pid=%lu conf=%s log=%d trace=%d stats=%d log_file=%s\n",
                                     platform_executable_name().c_str(), pid, *conf ? conf : "(none)",
                                     config_flag("LOG"), g_trace_enabled, config_flag("STATS"),
                                     log_file_enabled() ? config_get("LOG_FILE") : "(none)");
    write_line(line, static_cast<size_t>(length));
}

} // namespace

void log_text(const char *text, size_t length)
{
    // A log line produced while one is being written (a failing transport reporting itself) is dropped.
    static thread_local bool busy = false;
    if (busy) {
        std::fwrite(text, 1, length, stderr);
        return;
    }
    busy = true;
    static const bool banner = (write_banner(), true);
    (void)banner;
    write_line(text, length);
    busy = false;
}

void log_printf(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    int length = std::vsnprintf(line, sizeof(line) - 1, format, args);
    va_end(args);
    if (length < 0)
        return;
    if (length > static_cast<int>(sizeof(line)) - 2)
        length = sizeof(line) - 2;
    line[length++] = '\n';
    log_text(line, static_cast<size_t>(length));
}

} // namespace d3d12m

extern "C" void d3d12m_log_text(const char *text, size_t length)
{
    d3d12m::log_text(text, length);
}
