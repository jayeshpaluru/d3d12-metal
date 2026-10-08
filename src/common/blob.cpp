#include "common/blob.h"

#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

#include "common/com.h"

namespace d3d12m {

namespace {

class Blob final : public RefCounted<ID3DBlob> {
public:
    Blob(const void *data, size_t size) : bytes_(size)
    {
        if (size)
            std::memcpy(bytes_.data(), data, size);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        return query_interfaces<IUnknown, ID3DBlob>(this, riid, out);
    }

    LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return bytes_.data(); }
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return bytes_.size(); }

private:
    std::vector<uint8_t> bytes_;
};

} // namespace

HRESULT create_blob(const void *data, size_t size, ID3DBlob **out)
{
    if (!out || (size && !data))
        return E_INVALIDARG;
    *out = new (std::nothrow) Blob(data, size);
    return *out ? S_OK : E_OUTOFMEMORY;
}

HRESULT create_blob_from_string(const std::string &text, ID3DBlob **out)
{
    return create_blob(text.data(), text.size(), out);
}

} // namespace d3d12m
