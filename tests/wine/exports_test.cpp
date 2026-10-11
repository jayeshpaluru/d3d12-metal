// SPDX-License-Identifier: LGPL-2.1-or-later
// Reads the export tables of dxgi.dll and d3d12.dll the way the AMD GPU Services library and other hooking code
// does (no GetProcAddress, so no forwarder resolution): every export must be real code in an executable section,
// not a forwarder string in the export directory. Also calls CreateDXGIFactory2 through the raw table address.
#include "wine_test.h"

#include <cstdint>

namespace {

using CreateDXGIFactory2Fn = HRESULT(WINAPI *)(UINT, REFIID, void **);

const IMAGE_SECTION_HEADER *section_of(const IMAGE_NT_HEADERS *nt, DWORD rva)
{
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (rva >= s[i].VirtualAddress && rva < s[i].VirtualAddress + s[i].Misc.VirtualSize)
            return &s[i];
    return nullptr;
}

// Checks every export of `name`; returns the table address of `wanted` (or null).
void *check_exports(const char *name, const char *wanted)
{
    const auto base = reinterpret_cast<const uint8_t *>(GetModuleHandleA(name));
    CHECK(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + reinterpret_cast<const IMAGE_DOS_HEADER *>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const auto *exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(base + dir.VirtualAddress);
    const auto *functions = reinterpret_cast<const DWORD *>(base + exports->AddressOfFunctions);
    const auto *names = reinterpret_cast<const DWORD *>(base + exports->AddressOfNames);
    const auto *ordinals = reinterpret_cast<const WORD *>(base + exports->AddressOfNameOrdinals);

    void *found = nullptr;
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        const char *export_name = reinterpret_cast<const char *>(base + names[i]);
        const DWORD rva = functions[ordinals[i]];
        const bool forwarder = rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size;
        const IMAGE_SECTION_HEADER *section = section_of(nt, rva);
        const bool code = section && (section->Characteristics & IMAGE_SCN_MEM_EXECUTE);
        std::printf("exports_test: %s!%s rva=0x%x%s%s\n", name, export_name, static_cast<unsigned>(rva), forwarder ? " FORWARDER" : "",
                    code ? "" : " NOT CODE");
        CHECK(!forwarder);
        CHECK(code);
        if (wanted && std::strcmp(export_name, wanted) == 0)
            found = const_cast<uint8_t *>(base + rva);
    }
    return found;
}

} // namespace

int main()
{
    CHECK(LoadLibraryA("d3d12.dll"));
    CHECK(LoadLibraryA("dxgi.dll"));
    check_exports("d3d12.dll", nullptr);
    void *create = check_exports("dxgi.dll", "CreateDXGIFactory2");
    CHECK(create);

    // Games import d3d12.dll and dxgi.dll by the ordinals of the Windows runtime.
    const HMODULE d3d12 = GetModuleHandleA("d3d12.dll"), dxgi = GetModuleHandleA("dxgi.dll");
    const struct { HMODULE module; WORD ordinal; const char *name; } by_ordinal[] = {
        {d3d12, 100, "GetBehaviorValue"}, {d3d12, 101, "D3D12CreateDevice"}, {d3d12, 102, "D3D12GetDebugInterface"},
        {d3d12, 103, "D3D12CoreCreateLayeredDevice"}, {d3d12, 104, "D3D12CoreGetLayeredDeviceSize"},
        {d3d12, 105, "D3D12CoreRegisterLayers"}, {d3d12, 106, "D3D12CreateRootSignatureDeserializer"},
        {d3d12, 107, "D3D12CreateVersionedRootSignatureDeserializer"}, {d3d12, 108, "D3D12EnableExperimentalFeatures"},
        {d3d12, 109, "D3D12SerializeRootSignature"}, {d3d12, 110, "D3D12SerializeVersionedRootSignature"},
        {dxgi, 10, "CreateDXGIFactory"}, {dxgi, 11, "CreateDXGIFactory1"}, {dxgi, 23, "CreateDXGIFactory2"},
    };
    for (const auto &e : by_ordinal) {
        FARPROC byname = GetProcAddress(e.module, e.name), byord = GetProcAddress(e.module, MAKEINTRESOURCEA(e.ordinal));
        std::printf("exports_test: %s ordinal %u\n", e.name, e.ordinal);
        CHECK(byname);
        CHECK(byname == byord);
    }

    ComPtr<IDXGIFactory4> factory;
    CHECK_HR(reinterpret_cast<CreateDXGIFactory2Fn>(create)(0, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    CHECK_HR(factory->EnumAdapters1(0, &adapter));
    std::printf("exports_test: PASS\n");
    return 0;
}
