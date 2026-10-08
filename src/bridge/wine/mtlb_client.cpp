// PE side of the Wine unix-call transport: every mtlb_* function packs its
// arguments into a parameter struct and calls the unix module d3d12metal.so
// through ntdll's __wine_unix_call_dispatcher.
//
// The unix module is loaded on the first call (not in DllMain: loader lock).
// Pointers cross unchanged: a Wine process has one address space.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "bridge/mtlb.h"
#include "bridge/wine/mtlb_wine_common.h"
#include "mtlb_wine_params.h"

#define MTLB_WINE_SIDE "pe"
#define MTLB_THREAD_LOCAL thread_local

namespace {

constexpr int kMemoryWineLoadUnixLibByName = 1002;
constexpr wchar_t kUnixLibName[] = L"d3d12metal";

using NtQueryVirtualMemoryFn = LONG(WINAPI *)(HANDLE, const void *, int, void *, SIZE_T, SIZE_T *);
using DispatcherFn = LONG(WINAPI *)(UINT64, unsigned, void *);

struct UnicodeString {
    USHORT length;
    USHORT maximum_length;
    PWSTR buffer;
};

struct Transport {
    DispatcherFn *dispatcher = nullptr;
    UINT64 handle = 0;
    std::string error;  // empty once connected

    bool ready() const { return error.empty(); }
};

// Loads the unix module from `name` (a bare name searched in WINEDLLPATH and
// Wine's directories, or an NT path).
bool load_unix_lib(NtQueryVirtualMemoryFn query, const wchar_t *name, size_t length, UINT64 *handle)
{
    UnicodeString string = {static_cast<USHORT>(length * sizeof(wchar_t)),
                            static_cast<USHORT>((length + 1) * sizeof(wchar_t)), const_cast<PWSTR>(name)};
    UINT64 result[2] = {};
    if (query(GetCurrentProcess(), &string, kMemoryWineLoadUnixLibByName, result, sizeof(result), nullptr) != 0)
        return false;
    *handle = result[1];
    return true;
}

Transport connect()
{
    Transport t;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto query = reinterpret_cast<NtQueryVirtualMemoryFn>(
        reinterpret_cast<void *>(GetProcAddress(ntdll, "NtQueryVirtualMemory")));
    // The export is a pointer to the dispatcher, not the dispatcher.
    t.dispatcher = reinterpret_cast<DispatcherFn *>(
        reinterpret_cast<void *>(GetProcAddress(ntdll, "__wine_unix_call_dispatcher")));
    if (!query || !t.dispatcher || !GetProcAddress(ntdll, "wine_get_version")) {
        t.error = "not running under Wine (ntdll has no unix call dispatcher)";
        return t;
    }

    // First choice: x86_64-unix/d3d12metal.so next to this DLL; then a search of WINEDLLPATH.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&connect), &self);
    std::wstring path = L"\\??\\";
    wchar_t module_path[MAX_PATH];
    DWORD length = self ? GetModuleFileNameW(self, module_path, MAX_PATH) : 0;
    bool loaded = false;
    if (length > 0 && length < MAX_PATH) {
        path += module_path;
        path.resize(path.find_last_of(L'\\') + 1);
        path += L"x86_64-unix\\d3d12metal.so";
        loaded = load_unix_lib(query, path.c_str(), path.size(), &t.handle);
    }
    if (!loaded)
        loaded = load_unix_lib(query, kUnixLibName, wcslen(kUnixLibName), &t.handle);
    if (!loaded) {
        t.error = "cannot load d3d12metal.so: put it in an x86_64-unix directory next to d3d12.dll "
                  "or in one of the WINEDLLPATH directories";
        return t;
    }
    return t;
}

const Transport &transport()
{
    static const Transport t = connect();
    return t;
}

} // namespace

// Calls function `index` of the unix module. Returns false (after logging once)
// when the transport is unavailable.
static bool mtlb_wine_call(unsigned index, void *params, const char *name)
{
    const Transport &t = transport();
    if (!t.ready()) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            fprintf(stderr, "d3d12-metal: %s\n", t.error.c_str());
        }
        return false;
    }
    const LONG status = (*t.dispatcher)(t.handle, index, params);
    if (status != 0) {
        fprintf(stderr, "d3d12-metal: unix call %s failed: 0x%08lx\n", name, static_cast<unsigned long>(status));
        return false;
    }
    return true;
}

extern "C" {
#include "mtlb_wine_client.inc"
}
