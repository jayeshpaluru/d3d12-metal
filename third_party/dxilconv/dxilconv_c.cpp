// A C interface to Microsoft's DXBC -> DXIL converter (DirectXShaderCompiler, projects/dxilconv; University of
// Illinois/NCSA licence). Replaces the Windows-only dxilconv.dll entry points (DllMain, DxcCreateInstance).
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <windows.h>  // the shim

#include "dxc/Support/Global.h"
#include "dxc/Support/WinIncludes.h"
#include "llvm/Support/FileSystem.h"

#include "DxbcConverter.h"
#include "dxilconv_c.h"

// DxbcConverter.h declares the interface with __declspec(uuid), which the emulated __uuidof of the non-Windows
// WinAdapter does not see; DxbcConverter.cpp's QueryInterface needs this definition.
template <> GUID __emulated_uuidof<IDxbcConverter>()
{
    static const IID iid = guid_from_string("5F956ED5-78D1-4B15-8247-F7187614A041");
    return iid;
}

// Defined in DxbcConverter.cpp.
HRESULT CreateDxbcConverter(REFIID riid, LPVOID *ppv);

namespace {

std::once_flag g_init_once;
HRESULT g_init_result = E_FAIL;

// What dxilconv.dll's DllMain does on process attach.
HRESULT init()
{
    HRESULT hr = DxcInitThreadMalloc();
    if (FAILED(hr))
        return hr;
    DxcSetThreadMallocToDefault();
    if (llvm::sys::fs::SetupPerThreadFileSystem())
        hr = E_FAIL;
    DxcClearThreadMalloc();
    return hr;
}

} // namespace

extern "C" int dxilconv_convert(const void *dxbc, uint32_t dxbc_size, void **dxil, uint32_t *dxil_size, char *diag,
                                uint32_t diag_size)
{
    if (diag && diag_size)
        diag[0] = 0;
    if (!dxbc || !dxbc_size || !dxil || !dxil_size)
        return E_INVALIDARG;
    *dxil = nullptr;
    *dxil_size = 0;
    std::call_once(g_init_once, [] { g_init_result = init(); });
    if (FAILED(g_init_result))
        return g_init_result;

    DxcThreadMalloc thread_malloc(nullptr);
    CComPtr<IDxbcConverter> converter;
    HRESULT hr = CreateDxbcConverter(__uuidof(IDxbcConverter), reinterpret_cast<void **>(&converter));
    if (FAILED(hr))
        return hr;
    LPVOID out = nullptr;
    UINT32 out_size = 0;
    LPWSTR messages = nullptr;
    hr = converter->Convert(dxbc, dxbc_size, L"", &out, &out_size, &messages);
    if (messages) {
        if (diag && diag_size) {
            std::string text;
            for (const wchar_t *p = messages; *p; ++p)
                text += *p < 128 ? static_cast<char>(*p) : '?';
            std::strncpy(diag, text.c_str(), diag_size - 1);
            diag[diag_size - 1] = 0;
        }
        CoTaskMemFree(messages);
    }
    if (FAILED(hr) || !out) {
        if (out)
            CoTaskMemFree(out);
        return FAILED(hr) ? hr : E_FAIL;
    }
    *dxil = std::malloc(out_size);
    if (!*dxil) {
        CoTaskMemFree(out);
        return E_OUTOFMEMORY;
    }
    std::memcpy(*dxil, out, out_size);
    *dxil_size = out_size;
    CoTaskMemFree(out);
    return 0;
}

extern "C" void dxilconv_free(void *dxil)
{
    std::free(dxil);
}
