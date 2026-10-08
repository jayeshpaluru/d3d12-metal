#include "common/trace.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "bridge/mtlb.h"
#include "common/config.h"
#include "common/log.h"

namespace d3d12m {

bool g_trace_enabled = config_flag("TRACE");
bool g_profile_enabled = config_flag("PROFILE");

namespace {

struct Registry {
    std::mutex mutex;
    std::vector<TraceSite *> sites;  // in order of first call
    std::chrono::steady_clock::time_point last_dump = std::chrono::steady_clock::now();
    bool at_exit = false;
};

Registry &registry()
{
    static Registry *r = new Registry;  // never destroyed: calls may come while the process shuts down
    return *r;
}

// "HRESULT __attribute__((stdcall)) d3d12m::(anonymous namespace)::Foo::Bar(UINT) [...]" -> "Foo::Bar".
std::string method_name(const char *pretty)
{
    std::string text = pretty;
    // query_interfaces<...>(Derived *self, ...) [Derived = X]: show it as X::QueryInterface.
    if (text.find("query_interfaces") != std::string::npos) {
        const size_t at = text.find("Derived = ");
        if (at != std::string::npos) {
            const size_t start = at + 10;
            const size_t end = text.find_first_of("];,", start);
            text = text.substr(start, end - start) + "::QueryInterface(";
        }
    }
    for (size_t at; (at = text.find("__attribute__((")) != std::string::npos;)
        text.erase(at, text.find("))", at) + 3 - at);
    for (const char *noise : {"(anonymous namespace)::", "{anonymous}::", "d3d12m::"}) {
        for (size_t at; (at = text.find(noise)) != std::string::npos;)
            text.erase(at, strlen(noise));
    }
    const size_t paren = text.find('(');
    if (paren != std::string::npos)
        text.resize(paren);
    const size_t space = text.find_last_of(' ');
    return space == std::string::npos ? text : text.substr(space + 1);
}

unsigned thread_number()
{
    static std::atomic<unsigned> next{1};
    static thread_local const unsigned id = next.fetch_add(1);
    return id;
}

const char *hresult_name(HRESULT hr)
{
    switch (static_cast<uint32_t>(hr)) {
    case 0: return "S_OK";
    case 1: return "S_FALSE";
    case 0x80004001u: return "E_NOTIMPL";
    case 0x80004002u: return "E_NOINTERFACE";
    case 0x80004003u: return "E_POINTER";
    case 0x80004005u: return "E_FAIL";
    case 0x8007000Eu: return "E_OUTOFMEMORY";
    case 0x80070057u: return "E_INVALIDARG";
    case 0x887A0001u: return "DXGI_ERROR_INVALID_CALL";
    case 0x887A0002u: return "DXGI_ERROR_NOT_FOUND";
    case 0x887A0004u: return "DXGI_ERROR_UNSUPPORTED";
    case 0x887A0005u: return "DXGI_ERROR_DEVICE_REMOVED";
    case 0x887A000Au: return "DXGI_ERROR_WAS_STILL_DRAWING";
    default: return nullptr;
    }
}

struct IidName {
    GUID iid;
    const char *name;
};

const std::vector<IidName> &iid_names()
{
    static const std::vector<IidName> names = {
#define IID_ENTRY(type) {__uuidof(type), #type},
        IID_ENTRY(IUnknown)
        IID_ENTRY(ID3D12Object) IID_ENTRY(ID3D12DeviceChild) IID_ENTRY(ID3D12Pageable)
        IID_ENTRY(ID3D12Device) IID_ENTRY(ID3D12Device1) IID_ENTRY(ID3D12Device2) IID_ENTRY(ID3D12Device3)
        IID_ENTRY(ID3D12Device4) IID_ENTRY(ID3D12Device5) IID_ENTRY(ID3D12Device6) IID_ENTRY(ID3D12Device7)
        IID_ENTRY(ID3D12Device8) IID_ENTRY(ID3D12Device9) IID_ENTRY(ID3D12Device10)
        IID_ENTRY(ID3D12CommandQueue) IID_ENTRY(ID3D12CommandAllocator) IID_ENTRY(ID3D12CommandList)
        IID_ENTRY(ID3D12GraphicsCommandList) IID_ENTRY(ID3D12GraphicsCommandList1) IID_ENTRY(ID3D12GraphicsCommandList2)
        IID_ENTRY(ID3D12GraphicsCommandList3) IID_ENTRY(ID3D12GraphicsCommandList4) IID_ENTRY(ID3D12GraphicsCommandList5)
        IID_ENTRY(ID3D12GraphicsCommandList6) IID_ENTRY(ID3D12GraphicsCommandList7)
        IID_ENTRY(ID3D12Resource) IID_ENTRY(ID3D12Resource1) IID_ENTRY(ID3D12Resource2) IID_ENTRY(ID3D12Heap)
        IID_ENTRY(ID3D12Heap1) IID_ENTRY(ID3D12Fence) IID_ENTRY(ID3D12Fence1) IID_ENTRY(ID3D12PipelineState)
        IID_ENTRY(ID3D12RootSignature) IID_ENTRY(ID3D12DescriptorHeap) IID_ENTRY(ID3D12QueryHeap)
        IID_ENTRY(ID3D12CommandSignature) IID_ENTRY(ID3D12RootSignatureDeserializer)
        IID_ENTRY(ID3D12VersionedRootSignatureDeserializer) IID_ENTRY(ID3D12DeviceFactory)
        IID_ENTRY(ID3D12SDKConfiguration) IID_ENTRY(ID3D12SDKConfiguration1)
        IID_ENTRY(ID3D12DeviceRemovedExtendedDataSettings) IID_ENTRY(ID3D12DeviceRemovedExtendedData)
        IID_ENTRY(IDXGIObject) IID_ENTRY(IDXGIDeviceSubObject) IID_ENTRY(IDXGIFactory) IID_ENTRY(IDXGIFactory1)
        IID_ENTRY(IDXGIFactory2) IID_ENTRY(IDXGIFactory3) IID_ENTRY(IDXGIFactory4) IID_ENTRY(IDXGIFactory5)
        IID_ENTRY(IDXGIAdapter) IID_ENTRY(IDXGIAdapter1) IID_ENTRY(IDXGIAdapter2) IID_ENTRY(IDXGIAdapter3)
        IID_ENTRY(IDXGIOutput) IID_ENTRY(IDXGIOutput1) IID_ENTRY(IDXGIOutput2)
        IID_ENTRY(IDXGIOutput3) IID_ENTRY(IDXGIOutput4) IID_ENTRY(IDXGIOutput5) IID_ENTRY(IDXGIOutput6)
        IID_ENTRY(IDXGISwapChain) IID_ENTRY(IDXGISwapChain1) IID_ENTRY(IDXGISwapChain2) IID_ENTRY(IDXGISwapChain3)
        IID_ENTRY(IDXGISwapChain4)
#ifdef _WIN32
        IID_ENTRY(IDXGIFactory6) IID_ENTRY(IDXGIFactory7) IID_ENTRY(IDXGIAdapter4) IID_ENTRY(IDXGIDevice) IID_ENTRY(IDXGISurface)
#endif
#undef IID_ENTRY
    };
    return names;
}

// "<site name>" lines of the method list.
std::string method_list(const std::vector<TraceSite *> &sites)
{
    std::string text = "# calls\tmethod (in order of first call)\n";
    char count[32];
    for (const TraceSite *site : sites) {
        std::snprintf(count, sizeof(count), "%llu\t", static_cast<unsigned long long>(site->count.load()));
        text += count;
        text += method_name(site->function);
        text += '\n';
    }
    return text;
}

void write_method_list()
{
    Registry &r = registry();
    std::vector<TraceSite *> sites;
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        sites = r.sites;
        r.last_dump = std::chrono::steady_clock::now();
    }
    const std::string text = method_list(sites);
    if (config_get("LOG_FILE") && *config_get("LOG_FILE")) {
        mtlb_log_write(1, text.data(), text.size());
    } else {
        log_text(text.data(), text.size());
    }
}

void register_site(TraceSite &site)
{
    Registry &r = registry();
    bool first_site;
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        r.sites.push_back(&site);
        first_site = !r.at_exit;
        r.at_exit = true;
    }
    if (first_site)
        std::atexit(write_method_list);
}

} // namespace

const char *trace_iid_name(const GUID &iid, char *buffer, size_t size)
{
    for (const IidName &entry : iid_names()) {
        if (entry.iid == iid)
            return entry.name;
    }
    std::snprintf(buffer, size, "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", static_cast<unsigned>(iid.Data1),
                  iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3], iid.Data4[4],
                  iid.Data4[5], iid.Data4[6], iid.Data4[7]);
    return buffer;
}

void TraceLine::printf(const char *format, ...)
{
    if (size_ >= sizeof(text_) - 1)
        return;
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(text_ + size_, sizeof(text_) - size_, format, args);
    va_end(args);
    if (n > 0)
        size_ = std::min(size_ + static_cast<size_t>(n), sizeof(text_) - 1);
}

void TraceLine::append(const char *text, size_t length)
{
    length = std::min(length, sizeof(text_) - 1 - size_);
    std::memcpy(text_ + size_, text, length);
    size_ += length;
    text_[size_] = 0;
}

bool trace_count(TraceSite &site, uint64_t *count)
{
    *count = site.count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (*count == 1)
        register_site(site);
    return *count <= kTraceFirst || *count % kTraceEvery == 0;
}

void trace_emit(const TraceSite &site, uint64_t count, const TraceLine &args, const HRESULT *result)
{
    TraceLine line;
    line.printf("d3d12-metal: trace t%u %s(", thread_number(), method_name(site.function).c_str());
    line.append(args.data(), args.size());
    line.append(")", 1);
    if (result) {
        const char *name = hresult_name(*result);
        if (name)
            line.printf(" = %s", name);
        else
            line.printf(" = 0x%08x", static_cast<unsigned>(*result));
    }
    if (count > kTraceFirst)
        line.printf(" [call %llu]", static_cast<unsigned long long>(count));
    line.append("\n", 1);
    log_text(line.data(), line.size());
}

void trace_stub(const char *function)
{
    // A stub's TraceSite lives with the static of its first caller; it only needs a stable address.
    static std::mutex mutex;
    static std::vector<std::pair<std::string, TraceSite *>> stubs;
    TraceSite *site = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto &entry : stubs) {
            if (entry.first == function)
                site = entry.second;
        }
        if (!site) {
            site = new TraceSite{function};  // `function` is a string literal
            stubs.emplace_back(function, site);
        }
    }
    uint64_t count;
    if (!trace_count(*site, &count))
        return;
    TraceLine none;
    trace_emit(*site, count, none, nullptr);
}

uint64_t profile_now()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()).count());
}

namespace {
std::mutex &g_profile_mutex = *new std::mutex;  // never destroyed: methods can be called during shutdown
std::vector<TraceSite *> &g_profile_sites = *new std::vector<TraceSite *>;
} // namespace

uint64_t profile_begin(TraceSite &site)
{
    const uint64_t calls = site.calls.fetch_add(1, std::memory_order_relaxed);
    if (calls == 0) {
        std::lock_guard<std::mutex> lock(g_profile_mutex);
        g_profile_sites.push_back(&site);
    }
    return calls % kProfileSample == 0 ? profile_now() : 0;
}

// What reading the clock twice back to back costs (the least of many tries): a timed call's interval includes the
// second read and the tail of the first, which would otherwise be counted as time of the method (a clock read costs
// hundreds of nanoseconds under Wine, many times what a cheap method does).
static uint64_t clock_overhead()
{
    static const uint64_t overhead = [] {
        uint64_t least = UINT64_MAX;
        for (int i = 0; i < 256; ++i) {
            const uint64_t a = profile_now(), b = profile_now();
            least = std::min(least, b - a);
        }
        return least;
    }();
    return overhead;
}

void profile_end(TraceSite &site, uint64_t start)
{
    const uint64_t elapsed = profile_now() - start, overhead = clock_overhead();
    site.nanos.fetch_add((elapsed > overhead ? elapsed - overhead : 0) * kProfileSample, std::memory_order_relaxed);
}

void profile_report(unsigned frames)
{
    if (!g_profile_enabled)
        return;
    struct Row {
        std::string name;
        double calls, nanos;
    };
    static std::vector<std::pair<uint64_t, uint64_t>> last;  // per site: calls, nanos at the previous report
    std::vector<Row> rows;
    std::lock_guard<std::mutex> lock(g_profile_mutex);
    last.resize(g_profile_sites.size(), {0, 0});
    double total = 0;
    for (size_t i = 0; i < g_profile_sites.size(); ++i) {
        TraceSite &site = *g_profile_sites[i];
        const uint64_t calls = site.calls.load(), nanos = site.nanos.load();
        rows.push_back({method_name(site.function), double(calls - last[i].first) / frames, double(nanos - last[i].second) / frames});
        last[i] = {calls, nanos};
        total += rows.back().nanos;
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.nanos > b.nanos; });
    log_printf("d3d12-metal api profile (per frame): %.2f ms in %zu methods (nested calls counted in each)", total / 1e6, rows.size());
    for (size_t i = 0; i < std::min<size_t>(rows.size(), 14); ++i)
        log_printf("d3d12-metal   %7.3f ms  %9.1f calls  %6.0f ns each  %s", rows[i].nanos / 1e6, rows[i].calls,
                   rows[i].calls > 0 ? rows[i].nanos / rows[i].calls : 0.0, rows[i].name.c_str());
}

void trace_frame()
{
    if (!g_trace_enabled)
        return;
    Registry &r = registry();
    bool due;
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        due = !r.sites.empty() && std::chrono::steady_clock::now() - r.last_dump > std::chrono::seconds(5);
    }
    if (due)
        write_method_list();
}

void trace_text(TraceLine &line, const wchar_t *text)
{
    if (!text) {
        line.printf("null");
        return;
    }
    char narrow[130];
    size_t n = 0;
    for (; text[n] && n < sizeof(narrow) - 2; ++n)
        narrow[n] = text[n] >= 0x20 && text[n] < 0x7f ? static_cast<char>(text[n]) : '?';
    narrow[n] = 0;
    line.printf("L\"%s%s\"", narrow, text[n] ? "..." : "");
}

void trace_text(TraceLine &line, const char *text)
{
    if (!text)
        line.printf("null");
    else
        line.printf("\"%.120s\"", text);
}

void trace_guid(TraceLine &line, const GUID &iid)
{
    char buffer[64];
    line.printf("%s", trace_iid_name(iid, buffer, sizeof(buffer)));
}

// ---------------------------------------------------------------------------
// Summaries of descriptor structures
// ---------------------------------------------------------------------------

void trace_describe(TraceLine &line, const D3D12_RESOURCE_DESC *d)
{
    line.printf("{dim=%d %llux%u depth=%u mips=%u fmt=%d samples=%u layout=%d flags=0x%x}", static_cast<int>(d->Dimension),
                static_cast<unsigned long long>(d->Width), d->Height, d->DepthOrArraySize, d->MipLevels,
                static_cast<int>(d->Format), d->SampleDesc.Count, static_cast<int>(d->Layout), static_cast<unsigned>(d->Flags));
}

void trace_describe(TraceLine &line, const D3D12_RESOURCE_DESC1 *d)
{
    trace_describe(line, reinterpret_cast<const D3D12_RESOURCE_DESC *>(d));
}

void trace_describe(TraceLine &line, const D3D12_HEAP_PROPERTIES *d)
{
    line.printf("{type=%d cpu_page=%d pool=%d}", static_cast<int>(d->Type), static_cast<int>(d->CPUPageProperty),
                static_cast<int>(d->MemoryPoolPreference));
}

void trace_describe(TraceLine &line, const D3D12_HEAP_DESC *d)
{
    line.printf("{size=%llu type=%d align=%llu flags=0x%x}", static_cast<unsigned long long>(d->SizeInBytes),
                static_cast<int>(d->Properties.Type), static_cast<unsigned long long>(d->Alignment), static_cast<unsigned>(d->Flags));
}

void trace_describe(TraceLine &line, const D3D12_COMMAND_QUEUE_DESC *d)
{
    line.printf("{type=%d priority=%d flags=0x%x}", static_cast<int>(d->Type), d->Priority, static_cast<unsigned>(d->Flags));
}

void trace_describe(TraceLine &line, const D3D12_DESCRIPTOR_HEAP_DESC *d)
{
    line.printf("{type=%d count=%u flags=0x%x}", static_cast<int>(d->Type), d->NumDescriptors, static_cast<unsigned>(d->Flags));
}

void trace_describe(TraceLine &line, const D3D12_QUERY_HEAP_DESC *d)
{
    line.printf("{type=%d count=%u}", static_cast<int>(d->Type), d->Count);
}

void trace_describe(TraceLine &line, const D3D12_COMMAND_SIGNATURE_DESC *d)
{
    line.printf("{stride=%u args=%u first=%d}", d->ByteStride, d->NumArgumentDescs,
                d->NumArgumentDescs ? static_cast<int>(d->pArgumentDescs[0].Type) : -1);
}

void trace_describe(TraceLine &line, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d)
{
    line.printf("{vs=%llu ps=%llu ds=%llu hs=%llu gs=%llu rtv=%u dsv=%d samples=%u topology=%d}",
                (unsigned long long)d->VS.BytecodeLength, (unsigned long long)d->PS.BytecodeLength,
                (unsigned long long)d->DS.BytecodeLength, (unsigned long long)d->HS.BytecodeLength,
                (unsigned long long)d->GS.BytecodeLength, d->NumRenderTargets, static_cast<int>(d->DSVFormat), d->SampleDesc.Count,
                static_cast<int>(d->PrimitiveTopologyType));
}

void trace_describe(TraceLine &line, const D3D12_COMPUTE_PIPELINE_STATE_DESC *d)
{
    line.printf("{cs=%llu}", (unsigned long long)d->CS.BytecodeLength);
}

void trace_describe(TraceLine &line, const D3D12_PIPELINE_STATE_STREAM_DESC *d)
{
    line.printf("{bytes=%llu}", (unsigned long long)d->SizeInBytes);
}

void trace_describe(TraceLine &line, const D3D12_VIEWPORT *d)
{
    line.printf("{%g,%g %gx%g z=%g..%g}", d->TopLeftX, d->TopLeftY, d->Width, d->Height, d->MinDepth, d->MaxDepth);
}

void trace_describe(TraceLine &line, const D3D12_CLEAR_VALUE *d)
{
    line.printf("{fmt=%d %g,%g,%g,%g}", static_cast<int>(d->Format), d->Color[0], d->Color[1], d->Color[2], d->Color[3]);
}

void trace_describe(TraceLine &line, const D3D12_SHADER_RESOURCE_VIEW_DESC *d)
{
    line.printf("{fmt=%d dim=%d}", static_cast<int>(d->Format), static_cast<int>(d->ViewDimension));
}

void trace_describe(TraceLine &line, const D3D12_UNORDERED_ACCESS_VIEW_DESC *d)
{
    line.printf("{fmt=%d dim=%d}", static_cast<int>(d->Format), static_cast<int>(d->ViewDimension));
}

void trace_describe(TraceLine &line, const D3D12_RENDER_TARGET_VIEW_DESC *d)
{
    line.printf("{fmt=%d dim=%d}", static_cast<int>(d->Format), static_cast<int>(d->ViewDimension));
}

void trace_describe(TraceLine &line, const D3D12_DEPTH_STENCIL_VIEW_DESC *d)
{
    line.printf("{fmt=%d dim=%d flags=0x%x}", static_cast<int>(d->Format), static_cast<int>(d->ViewDimension),
                static_cast<unsigned>(d->Flags));
}

void trace_describe(TraceLine &line, const DXGI_SWAP_CHAIN_DESC *d)
{
    line.printf("{%ux%u fmt=%d buffers=%u effect=%d flags=0x%x windowed=%d hwnd=0x%llx}", d->BufferDesc.Width,
                d->BufferDesc.Height, static_cast<int>(d->BufferDesc.Format), d->BufferCount,
                static_cast<int>(d->SwapEffect), d->Flags, d->Windowed, static_cast<unsigned long long>((uintptr_t)d->OutputWindow));
}

void trace_describe(TraceLine &line, const DXGI_SWAP_CHAIN_DESC1 *d)
{
    line.printf("{%ux%u fmt=%d buffers=%u effect=%d scaling=%d alpha=%d flags=0x%x}", d->Width, d->Height,
                static_cast<int>(d->Format), d->BufferCount, static_cast<int>(d->SwapEffect),
                static_cast<int>(d->Scaling), static_cast<int>(d->AlphaMode), d->Flags);
}

void trace_describe(TraceLine &line, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *d)
{
    line.printf("{windowed=%d}", d->Windowed);
}

void trace_describe(TraceLine &line, const D3D12_RESOURCE_BARRIER *d)
{
    line.printf("{type=%d flags=0x%x}", static_cast<int>(d->Type), static_cast<unsigned>(d->Flags));
}

} // namespace d3d12m
