// DXBC (Shader Model 4 and 5) support: such shaders are converted to DXIL by Microsoft's dxilconv
// (third_party/dxilconv, libdxilconv.dylib) before Metal Shader Converter sees them.
//
// The library is loaded on first use, so a build or an install without it only loses DXBC shaders. It is
// searched in D3D12METAL_DXILCONV (a file), next to this module, and in the build tree's directory for the
// architecture (D3D12METAL_DXILCONV_DIR, set by meson).
#include "dxbc.h"

#include <dlfcn.h>

#include <cstdlib>
#include <cstring>
#include <mutex>

#include "dxilconv_c.h"

namespace mtlb {

namespace {

constexpr uint32_t kFourCC_DXBC = 'D' | 'X' << 8 | 'B' << 16 | 'C' << 24;
constexpr uint32_t kFourCC_DXIL = 'D' | 'X' << 8 | 'I' << 16 | 'L' << 24;

using ConvertFn = decltype(&dxilconv_convert);
using FreeFn = decltype(&dxilconv_free);

struct Converter {
    ConvertFn convert = nullptr;
    FreeFn free = nullptr;
    std::string error;
};

bool try_load(const std::string &path, Converter &out)
{
    void *lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        return false;
    out.convert = reinterpret_cast<ConvertFn>(dlsym(lib, "dxilconv_convert"));
    out.free = reinterpret_cast<FreeFn>(dlsym(lib, "dxilconv_free"));
    if (out.convert && out.free)
        return true;
    dlclose(lib);
    out.convert = nullptr;
    return false;
}

const Converter &converter()
{
    static Converter instance;
    static std::once_flag once;
    std::call_once(once, [] {
        std::vector<std::string> candidates;
        if (const char *path = std::getenv("D3D12METAL_DXILCONV"))
            candidates.push_back(path);
        Dl_info info;
        if (dladdr(reinterpret_cast<void *>(&converter), &info) && info.dli_fname) {
            std::string self = info.dli_fname;
            candidates.push_back(self.substr(0, self.find_last_of('/') + 1) + "libdxilconv.dylib");
        }
#ifdef D3D12METAL_DXILCONV_DIR
        candidates.push_back(std::string(D3D12METAL_DXILCONV_DIR) + "/libdxilconv.dylib");
#endif
        for (const std::string &candidate : candidates)
            if (try_load(candidate, instance))
                return;
        instance.error = "libdxilconv.dylib not found (tools/build-dxilconv.sh builds it; D3D12METAL_DXILCONV names it)";
    });
    return instance;
}

} // namespace

bool is_dxbc_only(const void *data, uint64_t size)
{
    // Container: fourcc, 16-byte hash, version (u32), total size (u32), part count (u32), part offsets (u32 each).
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    constexpr uint64_t kHeader = 32;
    uint32_t magic;
    if (size < kHeader)
        return false;
    std::memcpy(&magic, bytes, 4);
    if (magic != kFourCC_DXBC)
        return false;
    uint32_t parts;
    std::memcpy(&parts, bytes + 28, 4);
    if (parts > (size - kHeader) / 4)
        return false;
    for (uint32_t i = 0; i < parts; ++i) {
        uint32_t offset, fourcc;
        std::memcpy(&offset, bytes + kHeader + 4 * i, 4);
        if (offset > size - 4)
            return false;
        std::memcpy(&fourcc, bytes + offset, 4);
        if (fourcc == kFourCC_DXIL)
            return false;
    }
    return true;
}

bool dxbc_to_dxil(const void *dxbc, uint64_t size, std::vector<uint8_t> &dxil, std::string &error)
{
    const Converter &c = converter();
    if (!c.convert) {
        error = c.error;
        return false;
    }
    void *blob = nullptr;
    uint32_t blob_size = 0;
    char diag[512];
    const int hr = c.convert(dxbc, static_cast<uint32_t>(size), &blob, &blob_size, diag, sizeof(diag));
    if (hr < 0 || !blob) {
        error = "DXBC to DXIL conversion failed (HRESULT 0x" + [&] {
            char text[16];
            std::snprintf(text, sizeof(text), "%08x", static_cast<unsigned>(hr));
            return std::string(text);
        }() + ")";
        if (diag[0])
            error += std::string(": ") + diag;
        return false;
    }
    dxil.assign(static_cast<uint8_t *>(blob), static_cast<uint8_t *>(blob) + blob_size);
    c.free(blob);
    return true;
}

} // namespace mtlb
