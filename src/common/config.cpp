// SPDX-License-Identifier: LGPL-2.1-or-later
#include "common/config.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "bridge/mtlb.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace d3d12m {

namespace {

struct Config {
    std::map<std::string, std::string> values;  // upper-case key -> value
    std::string path;

    Config()
    {
        if (const char *override_path = std::getenv("D3D12METAL_CONF")) {
            parse(override_path);
        } else {
#ifdef _WIN32
            // d3d12metal.conf next to this DLL.
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&config_get), &self);
            char module_path[MAX_PATH];
            const DWORD length = self ? GetModuleFileNameA(self, module_path, MAX_PATH) : 0;
            if (length > 0 && length < MAX_PATH) {
                std::string dir(module_path, length);
                dir.resize(dir.find_last_of('\\') + 1);
                parse(dir + "d3d12metal.conf");
            }
#endif
        }
#ifndef _WIN32
        // One process, one environment: the backend reads the options from it. (The Wine transport tells the
        // unix side when it connects.)
        for (const auto &[key, value] : values)
            mtlb_configure(("D3D12METAL_" + key).c_str(), value.c_str());
#endif
    }

    void parse(const std::string &file)
    {
        FILE *f = std::fopen(file.c_str(), "r");
        if (!f)
            return;
        path = file;
        char line[2048];
        while (std::fgets(line, sizeof(line), f)) {
            std::string text(line);
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
                text.pop_back();
            const size_t start = text.find_first_not_of(" \t");
            if (start == std::string::npos || text[start] == '#' || text[start] == ';')
                continue;
            const size_t eq = text.find('=', start);
            if (eq == std::string::npos)
                continue;
            std::string key = text.substr(start, eq - start);
            std::string value = text.substr(eq + 1);
            while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back())))
                key.pop_back();
            value.erase(0, value.find_first_not_of(" \t"));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                value = value.substr(1, value.size() - 2);
            for (char &c : key)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (!key.empty())
                values[key] = value;
        }
        std::fclose(f);
    }
};

const Config &config()
{
    static const Config c;
    return c;
}

} // namespace

const char *config_get(const char *key)
{
    const std::string name = std::string("D3D12METAL_") + key;
    if (const char *env = std::getenv(name.c_str()))
        return env;
    const Config &c = config();
    auto it = c.values.find(key);
    return it == c.values.end() ? nullptr : it->second.c_str();
}

bool config_flag(const char *key)
{
    const char *value = config_get(key);
    return value && *value && *value != '0';
}

void config_for_each_file_entry(void (*fn)(const char *, const char *, void *), void *user)
{
    for (const auto &[key, value] : config().values)
        fn(("D3D12METAL_" + key).c_str(), value.c_str(), user);
}

const char *config_file_path()
{
    return config().path.c_str();
}

} // namespace d3d12m

extern "C" int d3d12m_option_enabled(const char *key)
{
    return d3d12m::config_flag(key);
}
