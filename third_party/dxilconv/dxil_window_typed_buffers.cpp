// SPDX-License-Identifier: LGPL-2.1-or-later
// dxilconv_window_typed_buffers: rewrites typed buffer accesses of a DXIL container so that element indices >= 2^28 go
// through "window" resources (see dxilconv_c.h). Metal texture buffers hold at most 2^28 elements; the layer binds, per
// typed buffer descriptor, three more views starting at element w * 2^28 into a shadow descriptor table, and this pass
// makes the shader use them.
//
// For every SRV/UAV R of kind TypedBuffer (space s, registers [lb, ub]) one resource Rw is declared in space
// s + window_space_offset with registers [3 * lb, 3 * ub + 2]; window w (1..3) of register r of R is register
// 3 * r + (w - 1) of Rw. Each bufferLoad/bufferStore/atomicBinOp/atomicCompareExchange through a handle of R is split:
//
//   head:  ... ; if (index < 2^28) goto orig else goto win
//   orig:  the original access                                     -> merge
//   win:   w = min(index >> 28, 3); access through Rw's register   -> merge
//          3 * regIndex + w - 1 at index - (w << 28)
//   merge: PHIs (one per used component of a load, or the atomic's result), rest of the block
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h> // the shim

#include "dxc/DXIL/DxilInstructions.h"
#include "dxc/DXIL/DxilModule.h"
#include "dxc/DXIL/DxilOperations.h"
#include "dxc/DXIL/DxilResource.h"
#include "dxc/DxilContainer/DxilContainer.h"
#include "dxc/DxilContainer/DxilContainerAssembler.h"
#include "dxc/Support/FileIOHelper.h"
#include "dxc/Support/Global.h"
#include "dxc/Support/WinIncludes.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include "dxilconv_c.h"

using namespace llvm;
using namespace hlsl;

namespace {

constexpr unsigned kWindowBits = 28;
constexpr unsigned kWindows = 3;
constexpr uint32_t kWindowSize = 1u << kWindowBits;

struct Error {
    std::string message;
};

// What a handle operand of an access resolves to.
struct Handle {
    DxilResource *res = nullptr;
    CallInst *create = nullptr;   // createHandle (57) or createHandleFromBinding (217)
    CallInst *annotate = nullptr; // annotateHandle (216) of a createHandleFromBinding, else null
    Value *reg_index = nullptr;   // absolute register index the handle was created with
};

struct Access {
    CallInst *call;
    Handle handle;
    unsigned index_operand; // operand of `call` holding the element index
    OP::OpCode op;
};

// A resource created by the pass for R.
struct Window {
    DxilResource *res;
    unsigned lower_bound, upper_bound, space;
};

class Rewriter {
public:
    Rewriter(Module &m, DxilModule &dm, uint32_t space_offset) : m_(m), dm_(dm), space_offset_(space_offset) {}

    // Returns the number of rewritten accesses.
    unsigned run()
    {
        std::vector<Access> accesses;
        for (Function &f : m_) {
            if (f.isDeclaration())
                continue;
            for (BasicBlock &bb : f)
                for (Instruction &i : bb)
                    if (auto *c = dyn_cast<CallInst>(&i))
                        collect(c, accesses);
        }
        for (Access &a : accesses)
            rewrite(a);
        return accesses.size();
    }

private:
    Module &m_;
    DxilModule &dm_;
    uint32_t space_offset_;
    std::map<DxilResource *, Window> windows_;

    DxilResource *find_by_binding(unsigned cls, uint32_t lb, uint32_t ub, uint32_t space)
    {
        auto &list = cls == 0 ? dm_.GetSRVs() : dm_.GetUAVs();
        for (auto &r : list)
            if (r->GetLowerBound() == lb && r->GetUpperBound() == ub && r->GetSpaceID() == space && r->IsTypedBuffer())
                return r.get();
        return nullptr;
    }

    // Resolves `h` to a typed buffer resource handle; res stays null for anything else.
    Handle resolve(Value *h)
    {
        Handle out;
        auto *c = dyn_cast<CallInst>(h);
        if (!c)
            return out;
        if (DxilInst_CreateHandle ch{c}) {
            auto *cls = dyn_cast<ConstantInt>(ch.get_resourceClass());
            auto *range = dyn_cast<ConstantInt>(ch.get_rangeId());
            if (!cls || !range || cls->getZExtValue() > 1)
                return out;
            auto &list = cls->getZExtValue() == 0 ? dm_.GetSRVs() : dm_.GetUAVs();
            if (range->getZExtValue() >= list.size())
                return out;
            DxilResource *r = list[range->getZExtValue()].get();
            if (r->IsTypedBuffer()) {
                out.res = r;
                out.create = c;
                out.reg_index = ch.get_index();
            }
            return out;
        }
        if (DxilInst_AnnotateHandle ah{c}) {
            auto *inner = dyn_cast<CallInst>(ah.get_res());
            if (!inner)
                return out;
            DxilInst_CreateHandleFromBinding cb{inner};
            if (!cb)
                return out;
            auto *bind = dyn_cast<Constant>(cb.get_bind());
            if (!bind)
                return out;
            auto field = [&](unsigned i) { return cast<ConstantInt>(bind->getAggregateElement(i))->getZExtValue(); };
            unsigned cls = field(3);
            if (cls > 1)
                return out;
            if (DxilResource *r = find_by_binding(cls, field(0), field(1), field(2))) {
                out.res = r;
                out.create = inner;
                out.annotate = c;
                out.reg_index = cb.get_index();
            }
        }
        return out;
    }

    void collect(CallInst *c, std::vector<Access> &accesses)
    {
        Function *callee = c->getCalledFunction();
        if (!callee || !OP::IsDxilOpFunc(callee))
            return;
        OP::OpCode op = OP::getOpCode(c);
        // Operand index of the element index (coordinate 0) per opcode.
        unsigned idx_op;
        switch (op) {
        case OP::OpCode::BufferLoad:
        case OP::OpCode::BufferStore:
            idx_op = 2;
            break;
        case OP::OpCode::AtomicBinOp:
            idx_op = 3;
            break;
        case OP::OpCode::AtomicCompareExchange:
            idx_op = 2;
            break;
        default:
            return;
        }
        Handle h = resolve(c->getArgOperand(1));
        if (!h.res)
            return;
        // A constant index below 2^28 never needs a window.
        if (auto *ci = dyn_cast<ConstantInt>(c->getArgOperand(idx_op)))
            if (ci->getZExtValue() < kWindowSize)
                return;
        accesses.push_back({c, h, idx_op, op});
    }

    const Window &window_for(DxilResource *r)
    {
        auto it = windows_.find(r);
        if (it != windows_.end())
            return it->second;

        uint64_t lb = 3ull * r->GetLowerBound();
        if (lb > UINT_MAX - 1)
            throw Error{"register " + std::to_string(r->GetLowerBound()) + " is too large to scale"};
        uint64_t space = uint64_t(r->GetSpaceID()) + space_offset_;
        if (space > UINT_MAX)
            throw Error{"register space overflows"};
        uint64_t ub = r->IsUnbounded() ? UINT_MAX : 3ull * r->GetUpperBound() + 2;
        if (ub >= UINT_MAX)
            ub = UINT_MAX; // unbounded

        auto w = std::make_unique<DxilResource>(*r);
        w->SetGlobalName(r->GetGlobalName() + "_window");
        w->SetSpaceID(unsigned(space));
        w->SetLowerBound(unsigned(lb));
        w->SetRangeSize(ub == UINT_MAX ? UINT_MAX : unsigned(ub - lb + 1));
        w->SetHasCounter(false);
        DxilResource *raw = w.get();
        unsigned id = r->IsRW() ? dm_.AddUAV(std::move(w)) : dm_.AddSRV(std::move(w));
        raw->SetID(id);
        return windows_[r] = Window{raw, unsigned(lb), unsigned(ub), unsigned(space)};
    }

    // Creates the handle of window `w` (1..3) of `h`, whose register is 3 * regIndex + w - 1 of the window resource.
    Value *make_handle(IRBuilder<> &b, const Handle &h, const Window &win, Value *w)
    {
        LLVMContext &ctx = m_.getContext();
        Type *i32 = Type::getInt32Ty(ctx);
        Value *reg = b.CreateAdd(b.CreateMul(h.reg_index, ConstantInt::get(i32, 3), "", true), b.CreateSub(w, ConstantInt::get(i32, 1)));
        Value *non_uniform = ConstantInt::getTrue(ctx);
        CallInst *create = h.create;
        Function *create_fn = create->getCalledFunction();
        if (DxilInst_CreateHandle{create}) {
            Value *args[] = {create->getArgOperand(0), create->getArgOperand(1),
                             ConstantInt::get(Type::getInt32Ty(ctx), win.res->GetID()), reg, non_uniform};
            return b.CreateCall(create_fn, args, "tbw.handle");
        }
        // createHandleFromBinding + annotateHandle
        Constant *old_bind = cast<Constant>(create->getArgOperand(1));
        Constant *bind_fields[] = {ConstantInt::get(i32, win.lower_bound), ConstantInt::get(i32, win.upper_bound),
                                   ConstantInt::get(i32, win.space), old_bind->getAggregateElement(3)};
        Constant *bind = ConstantStruct::get(cast<StructType>(old_bind->getType()), bind_fields);
        Value *args[] = {create->getArgOperand(0), bind, reg, non_uniform};
        Value *handle = b.CreateCall(create_fn, args, "tbw.handle");
        Value *aargs[] = {h.annotate->getArgOperand(0), handle, h.annotate->getArgOperand(2)};
        return b.CreateCall(h.annotate->getCalledFunction(), aargs, "tbw.handle");
    }

    void rewrite(Access &a)
    {
        const Window &win = window_for(a.handle.res);
        CallInst *i = a.call;
        Function *f = i->getParent()->getParent();
        LLVMContext &ctx = m_.getContext();
        Type *i32 = Type::getInt32Ty(ctx);
        Value *index = i->getArgOperand(a.index_operand);

        BasicBlock *head = i->getParent();
        BasicBlock *orig = head->splitBasicBlock(BasicBlock::iterator(i), "tbw.orig");
        BasicBlock *merge = orig->splitBasicBlock(std::next(BasicBlock::iterator(i)), "tbw.merge");
        BasicBlock *winbb = BasicBlock::Create(ctx, "tbw.win", f, merge);

        head->getTerminator()->eraseFromParent();
        IRBuilder<> b(head);
        b.CreateCondBr(b.CreateICmpULT(index, ConstantInt::get(i32, kWindowSize)), orig, winbb);

        b.SetInsertPoint(winbb);
        Value *w0 = b.CreateLShr(index, kWindowBits);
        Value *w = b.CreateSelect(b.CreateICmpUGT(w0, ConstantInt::get(i32, kWindows)), ConstantInt::get(i32, kWindows), w0);
        Value *local = b.CreateSub(index, b.CreateShl(w, kWindowBits));
        Value *handle = make_handle(b, a.handle, win, w);
        CallInst *wi = cast<CallInst>(i->clone());
        wi->setOperand(1, handle);
        wi->setOperand(a.index_operand, local);
        b.Insert(wi);

        if (a.op == OP::OpCode::BufferStore) {
            b.CreateBr(merge);
            return;
        }

        // Value-producing accesses: a PHI in merge per used component (loads) or for the result (atomics).
        std::vector<Instruction *> users;
        for (User *u : i->users())
            users.push_back(cast<Instruction>(u));
        if (users.empty()) {
            b.CreateBr(merge);
            return;
        }

        std::map<unsigned, PHINode *> component_phis;
        PHINode *whole = nullptr;
        auto phi_at_merge = [&](Type *ty) { return PHINode::Create(ty, 2, "tbw.phi", &*merge->begin()); };
        for (Instruction *u : users) {
            auto *ev = a.op == OP::OpCode::BufferLoad ? dyn_cast<ExtractValueInst>(u) : nullptr;
            if (!ev) {
                if (!whole) {
                    whole = phi_at_merge(i->getType());
                    whole->addIncoming(i, orig);
                    whole->addIncoming(wi, winbb);
                }
                u->replaceUsesOfWith(i, whole);
                continue;
            }
            unsigned k = ev->getIndices()[0];
            PHINode *&phi = component_phis[k];
            if (!phi) {
                IRBuilder<> bo(orig->getTerminator());
                Value *vo = bo.CreateExtractValue(i, k);
                Value *vw = b.CreateExtractValue(wi, k);
                phi = phi_at_merge(ev->getType());
                phi->addIncoming(vo, orig);
                phi->addIncoming(vw, winbb);
            }
            ev->replaceAllUsesWith(phi);
            ev->eraseFromParent();
        }
        b.CreateBr(merge);
    }
};

std::once_flag g_init_once;
HRESULT g_init_result = E_FAIL;

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

void set_diag(char *diag, uint32_t diag_size, const std::string &text)
{
    if (!diag || !diag_size)
        return;
    std::strncpy(diag, text.c_str(), diag_size - 1);
    diag[diag_size - 1] = 0;
}

// Appends the part's header and payload.
void append_bytes(std::vector<uint8_t> &out, const void *data, size_t size)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    out.insert(out.end(), p, p + size);
}

// Returns 1 rewritten, 0 untouched, negative on error.
int window(const void *dxil, uint32_t size, uint32_t space_offset, void **out, uint32_t *out_size, std::string &diag)
{
    const auto *header = static_cast<const DxilContainerHeader *>(dxil);
    if (size < sizeof(DxilContainerHeader) || !IsValidDxilContainer(header, size)) {
        diag = "not a DXIL container";
        return E_INVALIDARG;
    }
    const DxilProgramHeader *program = nullptr;
    uint32_t program_size = 0;
    std::vector<const DxilPartHeader *> parts;
    for (uint32_t i = 0; i < header->PartCount; ++i) {
        const DxilPartHeader *part = GetDxilContainerPart(header, i);
        parts.push_back(part);
        if (part->PartFourCC == DFCC_DXIL) {
            program = reinterpret_cast<const DxilProgramHeader *>(part + 1);
            program_size = part->PartSize;
        }
    }
    if (!program || !IsValidDxilProgramHeader(program, program_size)) {
        diag = "no DXIL part";
        return E_INVALIDARG;
    }

    const char *bitcode;
    uint32_t bitcode_size;
    GetDxilProgramBitcode(program, &bitcode, &bitcode_size);

    LLVMContext ctx;
    bool load_error = false;
    auto on_diag = [&load_error](const DiagnosticInfo &d) { load_error |= d.getSeverity() == DS_Error; };
    std::unique_ptr<MemoryBuffer> buffer = MemoryBuffer::getMemBufferCopy(StringRef(bitcode, bitcode_size));
    ErrorOr<std::unique_ptr<Module>> parsed = parseBitcodeFile(buffer->getMemBufferRef(), ctx, on_diag);
    if (!parsed || load_error) {
        diag = "cannot parse the DXIL bitcode";
        return E_INVALIDARG;
    }
    std::unique_ptr<Module> module = std::move(parsed.get());
    DxilModule &dm = module->GetOrCreateDxilModule();
    if (dm.GetShaderModel()->IsLib())
        return 0;

    unsigned rewritten = Rewriter(*module, dm, space_offset).run();
    if (!rewritten)
        return 0;
    dm.ReEmitDxilResources();

    // The new DXIL part: the program header (versions copied) and the bitcode.
    std::vector<uint8_t> dxil_part;
    {
        SmallVector<char, 0> bc;
        raw_svector_ostream bc_stream(bc);
        WriteBitcodeToFile(module.get(), bc_stream);
        bc_stream.flush();
        DxilProgramHeader ph = *program;
        InitProgramHeader(ph, program->ProgramVersion, program->BitcodeHeader.DxilVersion, uint32_t(bc.size()));
        append_bytes(dxil_part, &ph, sizeof(ph));
        append_bytes(dxil_part, bc.data(), bc.size());
        dxil_part.resize((dxil_part.size() + 3) & ~size_t(3));
    }

    // PSV0 lists the resource bindings; regenerate it from the updated module.
    std::vector<uint8_t> psv_part;
    bool has_psv = false;
    for (const DxilPartHeader *p : parts)
        has_psv |= p->PartFourCC == DFCC_PipelineStateValidation;
    if (has_psv) {
        std::unique_ptr<DxilPartWriter> psv(NewPSVWriter(dm));
        psv_part.resize(psv->size());
        CComPtr<AbstractMemoryStream> stream;
        IFT(CreateFixedSizeMemoryStream(psv_part.data(), psv_part.size(), &stream));
        psv->write(stream);
    }

    std::unique_ptr<DxilContainerWriter> writer(NewDxilContainerWriter(true));  // (parts keep their sizes, aligned or not)
    for (const DxilPartHeader *p : parts) {
        const uint8_t *data = reinterpret_cast<const uint8_t *>(p + 1);
        const std::vector<uint8_t> *replacement =
            p->PartFourCC == DFCC_DXIL ? &dxil_part : p->PartFourCC == DFCC_PipelineStateValidation ? &psv_part : nullptr;
        if (replacement) {
            writer->AddPart(p->PartFourCC, uint32_t(replacement->size()), [replacement](AbstractMemoryStream *s) {
                ULONG written = 0;
                s->Write(replacement->data(), replacement->size(), &written);
            });
        } else {
            uint32_t part_size = p->PartSize;
            writer->AddPart(p->PartFourCC, part_size, [data, part_size](AbstractMemoryStream *s) {
                ULONG written = 0;
                s->Write(data, part_size, &written);
            });
        }
    }
    uint32_t total = writer->size();
    void *result = std::malloc(total);
    if (!result)
        return E_OUTOFMEMORY;
    CComPtr<AbstractMemoryStream> stream;
    IFT(CreateFixedSizeMemoryStream(static_cast<LPBYTE>(result), total, &stream));
    writer->write(stream);
    *out = result;
    *out_size = total;
    return 1;
}

} // namespace

extern "C" int dxilconv_window_typed_buffers(const void *dxil, uint32_t size, uint32_t window_space_offset, void **out,
                                             uint32_t *out_size, char *diag, uint32_t diag_size)
{
    if (diag && diag_size)
        diag[0] = 0;
    if (!dxil || !size || !out || !out_size)
        return E_INVALIDARG;
    std::call_once(g_init_once, [] { g_init_result = init(); });
    if (FAILED(g_init_result))
        return g_init_result;

    DxcThreadMalloc thread_malloc(nullptr);
    std::string text;
    try {
        int r = window(dxil, size, window_space_offset, out, out_size, text);
        set_diag(diag, diag_size, text);
        return r;
    } catch (const Error &e) {
        set_diag(diag, diag_size, e.message);
    } catch (const hlsl::Exception &e) {
        char code[16];
        std::snprintf(code, sizeof(code), "%08x", static_cast<unsigned>(e.hr));
        set_diag(diag, diag_size, std::string("DXC exception 0x") + code + (e.what() && *e.what() ? std::string(": ") + e.what() : ""));
    } catch (const std::exception &e) {
        set_diag(diag, diag_size, e.what());
    } catch (...) {
        set_diag(diag, diag_size, "unknown exception");
    }
    return E_FAIL;
}
