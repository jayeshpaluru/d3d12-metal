// Option and log plumbing of the backend (see mtlb_configure and mtlb_log_write in bridge/mtlb.h).
#include "bridge/metal/log.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "bridge/mtlb.h"

extern "C" volatile int d3d12metal_option_epoch = 0;

namespace {

// The log file, opened on first use in append mode: every line is one write(2), so lines of the front-end and
// of the backend (and of several threads) never mix.
int log_fd()
{
    static const int fd = [] {
        const char *path = std::getenv("D3D12METAL_LOG_FILE");
        return path && *path ? open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644) : -1;
    }();
    return fd;
}

void write_all(int fd, const char *text, size_t length)
{
    while (length) {
        const ssize_t n = write(fd, text, length);
        if (n <= 0)
            return;
        text += n;
        length -= static_cast<size_t>(n);
    }
}

} // namespace

extern "C" void mtlb_configure(const char *name, const char *value)
{
    if (name && value)
        setenv(name, value, 0);
    d3d12metal_option_epoch = d3d12metal_option_epoch + 1;
}

extern "C" void mtlb_log_write(uint32_t channel, const char *text, uint64_t length)
{
    if (!text)
        return;
    if (channel == 0) {
        if (const int fd = log_fd(); fd >= 0)
            write_all(fd, text, static_cast<size_t>(length));
    } else if (channel == 1) {
        const char *path = std::getenv("D3D12METAL_LOG_FILE");
        if (!path || !*path)
            return;
        const std::string target = std::string(path) + ".methods";
        const std::string temp = target + ".tmp";
        const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0)
            return;
        write_all(fd, text, static_cast<size_t>(length));
        close(fd);
        rename(temp.c_str(), target.c_str());
    }
}

extern "C" void d3d12metal_log_text(const char *text, size_t length)
{
    write_all(STDERR_FILENO, text, length);
    mtlb_log_write(0, text, length);
}

void backend_log(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    int length = std::snprintf(line, sizeof(line) - 1, "d3d12-metal: ");
    length += std::vsnprintf(line + length, sizeof(line) - 1 - length, format, args);
    va_end(args);
    if (length >= static_cast<int>(sizeof(line)) - 1)
        length = sizeof(line) - 2;
    line[length++] = '\n';
    d3d12metal_log_text(line, static_cast<size_t>(length));
}
