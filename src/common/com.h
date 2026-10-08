// Small helpers for implementing COM objects.
#pragma once

#include <atomic>

#include "common/d3d12_uuids.h"

namespace d3d12m {

// Implements IUnknown reference counting for the most-derived interface `I`.
// The final class provides QueryInterface (see query_interfaces below).
template <typename I>
class RefCounted : public I {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = --refs_;
        if (n == 0)
            delete this;
        return n;
    }

protected:
    virtual ~RefCounted() = default;

private:
    std::atomic<ULONG> refs_{1};
};

// Answers QueryInterface for `self` with the first interface in Is whose IID
// matches `riid`. Every interface must be a base of Derived, so one object
// serves its whole inheritance chain (IUnknown ... most-derived interface).
template <typename... Is, typename Derived>
HRESULT query_interfaces(Derived *self, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    void *found = nullptr;
    (void)((riid == __uuidof(Is) ? (found = static_cast<Is *>(self), true) : false) || ...);
    *out = found;
    if (!found)
        return E_NOINTERFACE;
    self->AddRef();
    return S_OK;
}

// Hands a newly created object (holding its creation reference) to the caller
// as `riid`: queries the interface, then drops the creation reference. The
// object is destroyed when the query fails.
template <typename T>
HRESULT hand_out(T *object, REFIID riid, void **out)
{
    HRESULT hr = object->QueryInterface(riid, out);
    object->Release();
    return hr;
}

// Releases a COM pointer and clears it.
template <typename T>
void safe_release(T *&p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

} // namespace d3d12m
