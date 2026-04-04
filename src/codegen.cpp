#include "../include/codegen.hpp"
#include <fstream>
#include <sstream>
#include <cassert>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <functional>
#include <set>
#include <unordered_map>

namespace fpp {
namespace codegen {

// ─── ValueEnv ─────────────────────────────────────────────────────────────────

bool ValueEnv::define(const std::string& name, Entry e) {
    if (scopes.empty()) scopes.push_back({});
    scopes.back()[name] = std::move(e);
    return true;
}

ValueEnv::Entry* ValueEnv::lookup(const std::string& name) {
    for (int i = (int)scopes.size() - 1; i >= 0; --i) {
        auto it = scopes[i].find(name);
        if (it != scopes[i].end()) return &it->second;
    }
    return nullptr;
}

void ValueEnv::capture(const std::string& name) {
    for (int i = (int)scopes.size() - 1; i >= 0; --i) {
        auto it = scopes[i].find(name);
        if (it != scopes[i].end()) { it->second.isCaptured = true; return; }
    }
}

// ─── CodeGenerator ────────────────────────────────────────────────────────────

CodeGenerator::CodeGenerator(types::TypeRegistry& reg, types::SemanticAnalyser& sem)
    : typeReg_(reg), sem_(sem)
{
    irMod_.name         = "fpp_module";
    irMod_.targetTriple = "x86_64-unknown-linux-gnu";
    irMod_.dataLayout   = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128";
    builder_ = std::make_unique<ir::IRBuilder>(irMod_);
}

// ─── Type translation ─────────────────────────────────────────────────────────

ir::IRType CodeGenerator::toIRType(types::TypeId id) {
    ir::IRType t;
    t.typeId = id;
    const types::Type* ty = typeReg_.get(id);
    if (ty) {
        t.isPtr    = ty->kind == types::TypeKind::Ptr || ty->kind == types::TypeKind::Ref;
        t.isMut    = ty->isMut;
        t.ptrDepth = t.isPtr ? 1 : 0;
    }
    return t;
}

ir::IRType CodeGenerator::toIRTypeByName(const std::string& name) {
    ir::IRType t;
    if      (name == "bool")              { t.typeId = types::TY_BOOL; }
    else if (name == "i8")               { t.typeId = types::TY_I8; }
    else if (name == "i16")              { t.typeId = types::TY_I16; }
    else if (name == "i32")              { t.typeId = types::TY_I32; }
    else if (name == "i64")              { t.typeId = types::TY_I64; }
    else if (name == "i128")             { t.typeId = types::TY_I128; }
    else if (name == "isize")            { t.typeId = types::TY_ISIZE; }
    else if (name == "u8")               { t.typeId = types::TY_U8; }
    else if (name == "u16")              { t.typeId = types::TY_U16; }
    else if (name == "u32")              { t.typeId = types::TY_U32; }
    else if (name == "u64")              { t.typeId = types::TY_U64; }
    else if (name == "u128")             { t.typeId = types::TY_U128; }
    else if (name == "usize")            { t.typeId = types::TY_USIZE; }
    else if (name == "f32")              { t.typeId = types::TY_F32; }
    else if (name == "f64")              { t.typeId = types::TY_F64; }
    else if (name == "char")             { t.typeId = types::TY_CHAR; }
    else if (name == "str" || name == "String") { t.typeId = types::TY_STR; }
    else if (name == "unit" || name == "()") { t.typeId = types::TY_UNIT; }
    else if (name == "never" || name == "!") { t.typeId = types::TY_NEVER; }
    else {
        // Search user-defined types by name in the registry
        for (types::TypeId id = types::TY_FIRST_USER; ; ++id) {
            const types::Type* ty = typeReg_.get(id);
            if (!ty) break;
            if (ty->name == name) { t.typeId = id; return t; }
        }
        t.typeId = types::TY_UNKNOWN;
    }
    return t;
}

ir::IRType CodeGenerator::toIRType(const ast::TypeExpr& te) {
    if (auto* nt = dynamic_cast<const ast::NamedType*>(&te)) {
        ir::IRType t = toIRTypeByName(nt->name);
        if (t.typeId == types::TY_UNKNOWN) {
            if (nt->name == "Vec" || nt->name == "Array") {
                ir::IRType elem = nt->args.empty() ? ir::IRType{types::TY_I64} : toIRType(*nt->args[0]);
                types::TypeId vty = typeReg_.makeVec(elem.typeId);
                t.typeId = vty; t.isPtr = true;
            } else if (nt->name == "Option") {
                types::TypeId inner = nt->args.empty() ? types::TY_UNKNOWN : toIRType(*nt->args[0]).typeId;
                t.typeId = typeReg_.makeOption(inner);
            } else if (nt->name == "Result") {
                types::TypeId ok  = nt->args.size() > 0 ? toIRType(*nt->args[0]).typeId : types::TY_UNKNOWN;
                types::TypeId err = nt->args.size() > 1 ? toIRType(*nt->args[1]).typeId : types::TY_STR;
                t.typeId = typeReg_.makeResult(ok, err);
            } else if (nt->name == "Map") {
                types::TypeId key = nt->args.size() > 0 ? toIRType(*nt->args[0]).typeId : types::TY_STR;
                types::TypeId val = nt->args.size() > 1 ? toIRType(*nt->args[1]).typeId : types::TY_I64;
                t.typeId = typeReg_.makeMap(key, val);
            }
        }
        return t;
    }
    if (auto* pt = dynamic_cast<const ast::PtrType*>(&te)) {
        ir::IRType inner = toIRType(*pt->inner);
        types::TypeId pid = typeReg_.makePtr(inner.typeId, pt->isMut);
        ir::IRType t;
        t.typeId = pid; t.isPtr = true; t.isMut = pt->isMut; t.ptrDepth = inner.ptrDepth + 1;
        return t;
    }
    if (auto* rt = dynamic_cast<const ast::RefType*>(&te)) {
        ir::IRType inner = toIRType(*rt->inner);
        types::TypeId rid = typeReg_.makeRef(inner.typeId, rt->isMut);
        ir::IRType t;
        t.typeId = rid; t.isPtr = true; t.isMut = rt->isMut; t.ptrDepth = inner.ptrDepth + 1;
        return t;
    }
    if (auto* at = dynamic_cast<const ast::ArrayType*>(&te)) {
        ir::IRType elem = toIRType(*at->elem);
        size_t len = at->size.value_or(0);
        types::TypeId aid = typeReg_.makeArray(elem.typeId, len);
        ir::IRType t; t.typeId = aid; t.isPtr = true;
        return t;
    }
    if (auto* sl = dynamic_cast<const ast::SliceType*>(&te)) {
        ir::IRType elem = toIRType(*sl->elem);
        types::TypeId sid = typeReg_.makeSlice(elem.typeId);
        ir::IRType t; t.typeId = sid; t.isPtr = true;
        return t;
    }
    if (auto* tt = dynamic_cast<const ast::TupleType*>(&te)) {
        std::vector<types::TypeId> elems;
        for (auto& e : tt->elems) elems.push_back(toIRType(*e).typeId);
        types::TypeId tid = typeReg_.makeTuple(std::move(elems));
        return {tid};
    }
    if (auto* ft = dynamic_cast<const ast::FnType*>(&te)) {
        types::FnSig sig;
        for (auto& p : ft->params) sig.params.push_back(toIRType(*p).typeId);
        sig.ret = ft->ret ? toIRType(*ft->ret).typeId : types::TY_UNIT;
        types::TypeId fid = typeReg_.makeFn(std::move(sig));
        ir::IRType t; t.typeId = fid; t.isPtr = true;
        return t;
    }
    if (auto* nl = dynamic_cast<const ast::NullableType*>(&te)) {
        ir::IRType inner = toIRType(*nl->inner);
        types::TypeId oid = typeReg_.makeOption(inner.typeId);
        return {oid};
    }
    if (dynamic_cast<const ast::InferType*>(&te)) {
        return {typeReg_.freshInfer()};
    }
    return {types::TY_UNKNOWN};
}

size_t CodeGenerator::irTypeSize(ir::IRType ty) {
    return typeReg_.typeSize(ty.typeId);
}

// ─── Utilities ────────────────────────────────────────────────────────────────

ir::RegId CodeGenerator::load(ir::RegId ptr, ir::IRType ty) {
    return builder_->emitLoad(ptr, ty);
}

void CodeGenerator::store(ir::RegId val, ir::RegId ptr) {
    builder_->emitStore(val, ptr);
}

ir::RegId CodeGenerator::emitAlloca(ir::IRType ty, const std::string&) {
    return builder_->emitAlloc(ty);
}

// Emit a numeric coercion between two IR types, choosing the tightest cast opcode.
ir::RegId CodeGenerator::coerce(ir::RegId val, ir::IRType from, ir::IRType to) {
    if (from.typeId == to.typeId) return val;
    if (from.typeId == types::TY_UNKNOWN || to.typeId == types::TY_UNKNOWN) return val;

    bool fromFloat = typeReg_.isFloat(from.typeId);
    bool toFloat   = typeReg_.isFloat(to.typeId);
    bool fromInt   = typeReg_.isInteger(from.typeId);
    bool toInt     = typeReg_.isInteger(to.typeId);

    ir::Opcode castOp = ir::Opcode::Bitcast;

    if (fromFloat && toInt)       castOp = ir::Opcode::FpToInt;
    else if (fromInt && toFloat)  castOp = ir::Opcode::IntToFp;
    else if (fromInt && toInt) {
        size_t fromSz = typeReg_.typeSize(from.typeId);
        size_t toSz   = typeReg_.typeSize(to.typeId);
        if (toSz < fromSz)       castOp = ir::Opcode::Trunc;
        else if (toSz > fromSz)  castOp = ir::Opcode::Ext;
        // same size: bitcast covers sign reinterpretation
    }
    return builder_->emitCast(castOp, val, from, to);
}

std::string CodeGenerator::mangle(const std::string& name, const std::string& ns) {
    if (ns.empty()) return "_Zfpp_" + name;
    return "_Zfpp_" + ns + "_" + name;
}

ir::BlockId CodeGenerator::findLoopLabel(const std::string& lbl) {
    for (int i = (int)loopStack_.size() - 1; i >= 0; --i) {
        if (loopStack_[i].label == lbl) return loopStack_[i].breakTarget;
    }
    return ir::BLOCK_NONE;
}

void CodeGenerator::error(const std::string& msg, SourceLocation loc) {
    throw CodegenError(msg, loc);
}

// ─── Intrinsics / standard-library helpers ───────────────────────────────────

ir::RegId CodeGenerator::emitStdCall(const std::string& fn,
                                      std::vector<ir::RegId> args,
                                      ir::IRType retTy) {
    return builder_->emitIntrinsic(fn, std::move(args), retTy);
}

ir::RegId CodeGenerator::emitPrint(ir::RegId val, ir::IRType ty) {
    // Dispatch to typed print intrinsic so the runtime can format correctly
    std::string intrinsic = "__print_";
    switch (ty.typeId) {
    case types::TY_STR:  intrinsic += "str";  break;
    case types::TY_BOOL: intrinsic += "bool"; break;
    case types::TY_F32:
    case types::TY_F64:  intrinsic += "f64";  break;
    case types::TY_CHAR: intrinsic += "char"; break;
    default:             intrinsic += "i64";  break;
    }
    return builder_->emitIntrinsic(intrinsic, {val}, {types::TY_UNIT});
}

ir::RegId CodeGenerator::emitPanic(const std::string& msg, SourceLocation) {
    ir::RegId msgReg = builder_->emitConstStr(msg);
    return builder_->emitIntrinsic("__panic", {msgReg}, {types::TY_NEVER});
}

ir::RegId CodeGenerator::emitBoundsCheck(ir::RegId idx, ir::RegId len, SourceLocation loc) {
    // idx < len  → ok; otherwise panic
    ir::RegId inBound = builder_->emitBinop(ir::Opcode::Lt, idx, len, {types::TY_BOOL});
    ir::BlockId okBlock   = builder_->createBlock("bounds.ok");
    ir::BlockId failBlock = builder_->createBlock("bounds.fail");
    builder_->emitBranch(inBound, okBlock, failBlock);
    builder_->setBlock(failBlock);
    emitPanic("index out of bounds", loc);
    builder_->emitJump(okBlock);   // unreachable after panic but keeps IR well-formed
    builder_->setBlock(okBlock);
    return ir::REG_NONE;
}

// ─── Module-level code generation ────────────────────────────────────────────

ir::IRModule CodeGenerator::generate(const ast::Module& mod) {
    irMod_.name = mod.name;
    genGlobalFnDecls(mod);
    genModule(mod);
    return std::move(irMod_);
}

void CodeGenerator::genGlobalFnDecls(const ast::Module& mod) {
    // First pass: register every function signature so forward calls resolve.
    for (auto& item : mod.items) {
        auto* fn = dynamic_cast<const ast::FnItem*>(item.get());
        if (!fn) continue;

        ir::IRType retTy;
        if (fn->retTy) {
            retTy = toIRType(*fn->retTy);
        } else {
            retTy.typeId = types::TY_UNIT;
        }

        std::vector<ir::IRParam> params;
        for (auto& [pname, pty] : fn->params) {
            ir::IRParam p;
            p.name = pname;
            p.type = pty ? toIRType(*pty) : ir::IRType{types::TY_I64};
            params.push_back(std::move(p));
        }

        if (fn->isExtern) {
            // Declare extern without a body
            irMod_.externs.push_back(fn->name);
        }
        // Store the pre-declared signature in a side table keyed by name so
        // genCall can resolve it during the second pass.
        fnReturnTypes_[fn->name] = retTy;
    }
}

void CodeGenerator::genModule(const ast::Module& mod) {
    env_.pushScope();
    for (auto& item : mod.items) genStmt(*item);
    env_.popScope();
}

// ─── Statement dispatch ───────────────────────────────────────────────────────

void CodeGenerator::genStmt(const ast::Stmt& s) {
    if (auto* fn = dynamic_cast<const ast::FnItem*>(&s))     { genFn(*fn);     return; }
    if (auto* st = dynamic_cast<const ast::StructItem*>(&s)) { genStruct(*st); return; }
    if (auto* en = dynamic_cast<const ast::EnumItem*>(&s))   { genEnum(*en);   return; }
    if (auto* im = dynamic_cast<const ast::ImplItem*>(&s))   { genImpl(*im);   return; }
    if (auto* cl = dynamic_cast<const ast::ClassItem*>(&s))  { genClass(*cl);  return; }
    if (auto* co = dynamic_cast<const ast::ConstItem*>(&s))  { genConst(*co);  return; }
    if (auto* sa = dynamic_cast<const ast::StaticItem*>(&s)) { genStatic(*sa); return; }
    if (auto* ls = dynamic_cast<const ast::LetStmt*>(&s))    { genLet(*ls);    return; }
    if (auto* es = dynamic_cast<const ast::ExprStmt*>(&s))   { genExpr(*es->expr); return; }
    // ItemStmt wraps another statement
    if (auto* is = dynamic_cast<const ast::ItemStmt*>(&s))   { genStmt(*is->item); return; }
}

// ─── Function code generation ─────────────────────────────────────────────────

void CodeGenerator::genFn(const ast::FnItem& fn) {
    if (!fn.body) {
        // Extern declaration — already recorded in genGlobalFnDecls; nothing to emit
        return;
    }

    // Resolve return type
    ir::IRType retTy;
    if (fn.retTy) {
        retTy = toIRType(*fn.retTy);
    } else {
        retTy.typeId = types::TY_UNIT;
    }

    // Build typed parameter list
    std::vector<ir::IRParam> params;
    for (auto& [pname, pty] : fn.params) {
        ir::IRParam p;
        p.name = pname;
        p.type = pty ? toIRType(*pty) : ir::IRType{types::TY_I64};
        params.push_back(std::move(p));
    }

    ir::FuncId fid = builder_->beginFunction(fn.name, std::move(params), retTy);
    (void)fid;

    // Make function attributes visible to the IR
    {
        auto& irFn = irMod_.functions.back();
        irFn.isAsync    = fn.isAsync;
        irFn.isInline   = fn.isInline;
        irFn.linkage    = fn.isPublic ? "external" : "internal";
        irFn.callingConv = "fpp";
    }

    // Bind parameters into the environment
    env_.pushScope();
    ir::RegId pr = 1;
    for (auto& [pname, pty] : fn.params) {
        ir::IRType pty_ir = pty ? toIRType(*pty) : ir::IRType{types::TY_I64};
        // Allocate a stack slot so the parameter is addressable (mutable params, etc.)
        ir::RegId ptr = builder_->emitAlloc(pty_ir);
        builder_->emitStore(pr, ptr);
        ValueEnv::Entry e;
        e.reg   = ptr;
        e.isPtr = true;
        e.type  = pty_ir;
        e.isMut = false; // by default; mut params get their own semantics
        env_.define(pname, e);
        ++pr;
    }

    // Emit body
    for (auto& stmt : *fn.body) genStmt(*stmt);

    // Guarantee every exit block has a terminator
    {
        auto& curFn = irMod_.functions.back();
        if (!curFn.blocks.empty()) {
            auto& blk = curFn.blocks[builder_->currentBlock()];
            bool needsTerminator =
                blk.insts.empty() ||
                (blk.insts.back().op != ir::Opcode::Return  &&
                 blk.insts.back().op != ir::Opcode::Jump    &&
                 blk.insts.back().op != ir::Opcode::Branch  &&
                 blk.insts.back().op != ir::Opcode::Unreachable);
            if (needsTerminator) {
                if (retTy.typeId == types::TY_UNIT || retTy.typeId == types::TY_NEVER) {
                    builder_->emitReturn({});
                } else {
                    ir::RegId zero = builder_->emitConstInt(0, retTy);
                    builder_->emitReturn(zero);
                }
            }
        }
    }

    env_.popScope();
    builder_->endFunction();
}

// ─── Struct / Enum / Impl / Class ─────────────────────────────────────────────

void CodeGenerator::genStruct(const ast::StructItem& s) {
    // Register the struct type in the type registry if not already present
    types::TypeId existing = types::TY_UNKNOWN;
    for (types::TypeId id = types::TY_FIRST_USER; ; ++id) {
        const types::Type* ty = typeReg_.get(id);
        if (!ty) break;
        if (ty->name == s.name && ty->kind == types::TypeKind::Struct) {
            existing = id;
            break;
        }
    }

    if (existing == types::TY_UNKNOWN) {
        types::Type ty;
        ty.kind      = types::TypeKind::Struct;
        ty.name      = s.name;
        ty.isPublic  = s.isPublic;
        size_t offset = 0;
        for (auto& fd : s.fields) {
            types::FieldInfo fi;
            fi.name     = fd.name;
            fi.type     = fd.ty ? toIRType(*fd.ty).typeId : types::TY_UNKNOWN;
            fi.isMut    = fd.isMut;
            fi.isPublic = fd.isPublic;
            fi.offset   = offset;
            offset += typeReg_.typeSize(fi.type);
            ty.fields.push_back(std::move(fi));
        }
        ty.sizeBytes  = offset;
        ty.alignBytes = 8;
        typeReg_.registerType(std::move(ty));
    }

    // Emit a constructor function: `StructName(field0: T0, field1: T1, ...) -> *StructName`
    // so FPP code can call `MyStruct(x: 1, y: 2)`.
    const types::Type* sty = nullptr;
    for (types::TypeId id = types::TY_FIRST_USER; ; ++id) {
        const types::Type* t = typeReg_.get(id);
        if (!t) break;
        if (t->name == s.name) { sty = t; break; }
    }
    if (!sty) return;

    std::vector<ir::IRParam> ctorParams;
    for (auto& fi : sty->fields) {
        ir::IRParam p;
        p.name = fi.name;
        p.type = toIRType(fi.type);
        ctorParams.push_back(std::move(p));
    }
    ir::IRType ptrTy; ptrTy.typeId = types::TY_RAWPTR; ptrTy.isPtr = true;
    builder_->beginFunction(s.name, std::move(ctorParams), ptrTy);
    {
        auto& irFn = irMod_.functions.back();
        irFn.linkage = s.isPublic ? "external" : "internal";
    }

    // Allocate storage for the struct, store each field, return pointer
    ir::RegId base = builder_->emitAlloc({sty->id});
    ir::RegId pr = 1;
    for (uint32_t i = 0; i < (uint32_t)sty->fields.size(); ++i) {
        ir::IRType fty = toIRType(sty->fields[i].type);
        ir::RegId fptr = builder_->emitFieldPtr(base, i, fty);
        builder_->emitStore(pr, fptr);
        ++pr;
    }
    builder_->emitReturn(base);
    builder_->endFunction();
}

void CodeGenerator::genEnum(const ast::EnumItem& e) {
    // Register the enum type in the type registry
    types::TypeId existing = types::TY_UNKNOWN;
    for (types::TypeId id = types::TY_FIRST_USER; ; ++id) {
        const types::Type* ty = typeReg_.get(id);
        if (!ty) break;
        if (ty->name == e.name && ty->kind == types::TypeKind::Enum) {
            existing = id; break;
        }
    }
    if (existing == types::TY_UNKNOWN) {
        types::Type ty;
        ty.kind     = types::TypeKind::Enum;
        ty.name     = e.name;
        ty.isPublic = e.isPublic;
        // Variants stored as fields with their discriminant index
        int64_t discrim = 0;
        for (auto& v : e.variants) {
            types::FieldInfo fi;
            fi.name     = v.name;
            fi.isMut    = false;
            fi.isPublic = true;
            fi.offset   = (size_t)discrim;
            // Discriminant overrides
            if (v.discrim) discrim = *v.discrim;
            fi.type = types::TY_I64; // discriminant type
            ty.fields.push_back(fi);
            ++discrim;
        }
        ty.sizeBytes  = 16; // tag word + largest payload
        ty.alignBytes = 8;
        typeReg_.registerType(std::move(ty));
    }

    // Emit a variant constructor per variant that has payload
    for (size_t vi = 0; vi < e.variants.size(); ++vi) {
        auto& v = e.variants[vi];
        int64_t tag = v.discrim.value_or((int64_t)vi);

        std::vector<ir::IRParam> vparams;
        if (v.tuple) {
            for (size_t pi = 0; pi < v.tuple->size(); ++pi) {
                ir::IRParam p;
                p.name = "v" + std::to_string(pi);
                p.type = toIRType(*(*v.tuple)[pi]);
                vparams.push_back(std::move(p));
            }
        } else if (v.fields) {
            for (auto& fd : *v.fields) {
                ir::IRParam p;
                p.name = fd.name;
                p.type = fd.ty ? toIRType(*fd.ty) : ir::IRType{types::TY_I64};
                vparams.push_back(std::move(p));
            }
        }

        ir::IRType retTy; retTy.typeId = types::TY_RAWPTR; retTy.isPtr = true;
        builder_->beginFunction(e.name + "::" + v.name, std::move(vparams), retTy);
        {
            auto& irFn = irMod_.functions.back();
            irFn.linkage = e.isPublic ? "external" : "internal";
        }
        // Layout: [tag: i64][payload...]
        ir::RegId box = builder_->emitAlloc({types::TY_I64}); // oversized alloc via intrinsic
        ir::RegId tagReg = builder_->emitConstInt(tag, {types::TY_I64});
        builder_->emitStore(tagReg, box);
        // Store payload fields after the tag word
        ir::RegId pr = 1;
        size_t nFields = vparams.size(); // captured before move
        for (size_t pi = 0; pi < nFields; ++pi) {
            ir::RegId idx  = builder_->emitConstInt((int64_t)(pi + 1), {types::TY_I64});
            ir::RegId fptr = builder_->emitGEP(box, {idx}, {types::TY_I64});
            builder_->emitStore(pr, fptr);
            ++pr;
        }
        builder_->emitReturn(box);
        builder_->endFunction();
    }
}

void CodeGenerator::genImpl(const ast::ImplItem& impl) {
    // Determine the implementing type name for mangling
    std::string typeName;
    if (impl.ty) {
        if (auto* nt = dynamic_cast<const ast::NamedType*>(impl.ty.get()))
            typeName = nt->name;
    }
    std::string traitName;
    if (impl.trait) {
        if (auto* nt = dynamic_cast<const ast::NamedType*>(impl.trait->get()))
            traitName = nt->name;
    }

    env_.pushScope();
    for (auto& m : impl.members) {
        if (auto* fn = dynamic_cast<const ast::FnItem*>(m.get())) {
            // Mangle as TypeName::method or TraitName::TypeName::method
            std::string mangledName = typeName.empty()
                ? fn->name
                : typeName + "::" + fn->name;
            if (!traitName.empty()) mangledName = traitName + "::" + mangledName;

            // Synthesise a thin FnItem with the mangled name and emit it
            ast::FnItem proxy = *fn;
            proxy.name = mangledName;
            // If the method has a self parameter, prepend it as a pointer param
            if (fn->selfParam && !typeName.empty()) {
                ir::IRType selfTy = toIRTypeByName(typeName);
                if (selfTy.typeId == types::TY_UNKNOWN) selfTy.typeId = types::TY_RAWPTR;
                selfTy.isPtr = true;
                // self is already listed in params via the parser for methods
            }
            genFn(proxy);
        } else {
            genStmt(*m);
        }
    }
    env_.popScope();
}

void CodeGenerator::genClass(const ast::ClassItem& cls) {
    // Register the class as a struct-like type in the type registry
    types::TypeId existing = types::TY_UNKNOWN;
    for (types::TypeId id = types::TY_FIRST_USER; ; ++id) {
        const types::Type* ty = typeReg_.get(id);
        if (!ty) break;
        if (ty->name == cls.name) { existing = id; break; }
    }
    if (existing == types::TY_UNKNOWN) {
        types::Type ty;
        ty.kind     = types::TypeKind::Class;
        ty.name     = cls.name;
        ty.isPublic = cls.isPublic;
        // Collect field definitions from member list
        size_t offset = 0;
        for (auto& m : cls.members) {
            if (auto* ls = dynamic_cast<const ast::LetStmt*>(m.get())) {
                if (auto* np = dynamic_cast<const ast::NamePat*>(ls->pat.get())) {
                    types::FieldInfo fi;
                    fi.name     = np->name;
                    fi.isMut    = ls->isMut;
                    fi.isPublic = true;
                    fi.type     = ls->ty ? toIRType(**ls->ty).typeId : types::TY_I64;
                    fi.offset   = offset;
                    offset     += typeReg_.typeSize(fi.type);
                    ty.fields.push_back(std::move(fi));
                }
            }
        }
        ty.sizeBytes  = offset ? offset : 8;
        ty.alignBytes = 8;
        typeReg_.registerType(std::move(ty));
    }

    // Emit method functions and any nested items
    env_.pushScope();
    for (auto& m : cls.members) {
        if (auto* fn = dynamic_cast<const ast::FnItem*>(m.get())) {
            ast::FnItem proxy = *fn;
            proxy.name = cls.name + "::" + fn->name;
            genFn(proxy);
        } else {
            genStmt(*m);
        }
    }
    env_.popScope();
}

// ─── Const / Static ───────────────────────────────────────────────────────────

static void fillGlobalInitializer(ir::IRGlobal& g,
                                   const ast::LiteralExpr* le) {
    if (!le) return;
    switch (le->tok.kind) {
    case TokenKind::Integer:
        if (auto* iv = std::get_if<int64_t>(&le->tok.literal)) {
            g.initializer.resize(8);
            std::memcpy(g.initializer.data(), iv, 8);
        }
        break;
    case TokenKind::Float:
        if (auto* fv = std::get_if<double>(&le->tok.literal)) {
            g.initializer.resize(8);
            std::memcpy(g.initializer.data(), fv, 8);
        }
        break;
    case TokenKind::String:
        if (auto* sv = std::get_if<std::string>(&le->tok.literal))
            g.initializer.assign(sv->begin(), sv->end());
        break;
    case TokenKind::Kw_true:  g.initializer = {1}; break;
    case TokenKind::Kw_false: g.initializer = {0}; break;
    default: break;
    }
}

void CodeGenerator::genConst(const ast::ConstItem& c) {
    ir::IRGlobal g;
    g.name     = c.name;
    g.type     = c.ty ? toIRType(*c.ty) : ir::IRType{types::TY_I64};
    g.isConst  = true;
    g.isMut    = false;
    g.linkage  = c.isPublic ? "external" : "internal";
    fillGlobalInitializer(g, dynamic_cast<const ast::LiteralExpr*>(c.val.get()));
    irMod_.globals.push_back(g);

    ValueEnv::Entry e;
    e.reg   = ir::REG_NONE;
    e.isPtr = false;
    e.type  = g.type;
    e.isMut = false;
    env_.define(c.name, e);
}

void CodeGenerator::genStatic(const ast::StaticItem& s) {
    ir::IRGlobal g;
    g.name    = s.name;
    g.type    = s.ty ? toIRType(*s.ty) : ir::IRType{types::TY_I64};
    g.isConst = !s.isMut;
    g.isMut   = s.isMut;
    g.linkage  = s.isPublic ? "external" : "internal";
    fillGlobalInitializer(g, dynamic_cast<const ast::LiteralExpr*>(s.val.get()));
    irMod_.globals.push_back(g);

    ValueEnv::Entry e;
    e.reg   = ir::REG_NONE;
    e.isPtr = false;
    e.type  = g.type;
    e.isMut = s.isMut;
    env_.define(s.name, e);
}

// ─── Let binding ──────────────────────────────────────────────────────────────

void CodeGenerator::genLet(const ast::LetStmt& let) {
    // Resolve the declared type, or infer it from the initialiser
    ir::IRType ty;
    if (let.ty) {
        ty = toIRType(**let.ty);
    } else if (let.init) {
        // Lightweight type probe on the initialiser node
        auto* initExpr = let.init->get();
        if (auto* le = dynamic_cast<const ast::LiteralExpr*>(initExpr)) {
            switch (le->tok.kind) {
            case TokenKind::Integer: ty = {types::TY_I64};  break;
            case TokenKind::Float:   ty = {types::TY_F64};  break;
            case TokenKind::String:  ty = {types::TY_STR};  break;
            case TokenKind::Kw_true:
            case TokenKind::Kw_false: ty = {types::TY_BOOL}; break;
            case TokenKind::Char:    ty = {types::TY_CHAR}; break;
            default:                 ty = {types::TY_UNKNOWN}; break;
            }
        } else if (dynamic_cast<const ast::ArrayExpr*>(initExpr)) {
            ty = {types::TY_UNKNOWN}; // element type resolved at use
        } else if (dynamic_cast<const ast::MapExpr*>(initExpr)) {
            ty = {types::TY_UNKNOWN};
        } else {
            ty = {types::TY_UNKNOWN};
        }
    }
    if (ty.typeId == types::TY_UNKNOWN) ty.typeId = types::TY_I64;

    ir::RegId ptr = builder_->emitAlloc(ty);

    if (let.init) {
        ir::RegId initReg = genExpr(**let.init);

        // Narrow or widen numeric literals to the declared type
        if (ty.typeId != types::TY_UNKNOWN) {
            auto& regTypes = irMod_.functions.back().regTypes;
            auto it = regTypes.find(initReg);
            if (it != regTypes.end() && it->second.typeId != ty.typeId &&
                it->second.typeId != types::TY_UNKNOWN) {
                initReg = coerce(initReg, it->second, ty);
            }
        }
        builder_->emitStore(initReg, ptr);
    }

    // Recursively bind all pattern names to sub-regions of the allocated slot
    std::function<void(const ast::Pattern&, ir::RegId, ir::IRType)> bindPat =
        [&](const ast::Pattern& pat, ir::RegId valPtr, ir::IRType valTy) {
        if (auto* np = dynamic_cast<const ast::NamePat*>(&pat)) {
            ValueEnv::Entry e;
            e.reg   = valPtr;
            e.isPtr = true;
            e.type  = valTy;
            e.isMut = let.isMut || np->isMut;
            env_.define(np->name, e);
        } else if (dynamic_cast<const ast::WildcardPat*>(&pat)) {
            // _ — discarded
        } else if (auto* tp = dynamic_cast<const ast::TuplePat*>(&pat)) {
            for (size_t i = 0; i < tp->pats.size(); ++i) {
                ir::RegId idx  = builder_->emitConstInt((int64_t)i, {types::TY_I64});
                ir::RegId ePtr = builder_->emitGEP(valPtr, {idx}, valTy);
                bindPat(*tp->pats[i], ePtr, valTy);
            }
        } else if (auto* sp = dynamic_cast<const ast::StructPat*>(&pat)) {
            // Find field indices for each destructured name
            for (auto& [fname, fpat] : sp->fields) {
                uint32_t idx = 0;
                ir::IRType fty = {types::TY_I64};
                for (types::TypeId tid = types::TY_FIRST_USER; ; ++tid) {
                    const types::Type* sty = typeReg_.get(tid);
                    if (!sty) break;
                    for (uint32_t fi = 0; fi < sty->fields.size(); ++fi) {
                        if (sty->fields[fi].name == fname) {
                            idx = fi;
                            fty = toIRType(sty->fields[fi].type);
                            break;
                        }
                    }
                }
                ir::RegId fptr = builder_->emitFieldPtr(valPtr, idx, fty);
                bindPat(*fpat, fptr, fty);
            }
        }
    };
    bindPat(*let.pat, ptr, ty);
}

// ─── Expression dispatch ──────────────────────────────────────────────────────

ir::RegId CodeGenerator::genExpr(const ast::Expr& e, bool wantLval) {
    if (auto* le  = dynamic_cast<const ast::LiteralExpr*>(&e))  return genLiteral(*le);
    if (auto* ie  = dynamic_cast<const ast::IdentExpr*>(&e))    return genIdent(*ie, wantLval);
    if (auto* be  = dynamic_cast<const ast::BinaryExpr*>(&e))   return genBinary(*be);
    if (auto* ue  = dynamic_cast<const ast::UnaryExpr*>(&e))    return genUnary(*ue);
    if (auto* ae  = dynamic_cast<const ast::AssignExpr*>(&e))   return genAssign(*ae);
    if (auto* ce  = dynamic_cast<const ast::CallExpr*>(&e))     return genCall(*ce);
    if (auto* ie2 = dynamic_cast<const ast::IndexExpr*>(&e))    return genIndex(*ie2, wantLval);
    if (auto* fe  = dynamic_cast<const ast::FieldExpr*>(&e))    return genField(*fe, wantLval);
    if (auto* me  = dynamic_cast<const ast::MethodExpr*>(&e))   return genMethod(*me);
    if (auto* be2 = dynamic_cast<const ast::BlockExpr*>(&e))    return genBlock(*be2);
    if (auto* ie3 = dynamic_cast<const ast::IfExpr*>(&e))       return genIf(*ie3);
    if (auto* me2 = dynamic_cast<const ast::MatchExpr*>(&e))    return genMatch(*me2);
    if (auto* we  = dynamic_cast<const ast::WhileExpr*>(&e))    return genWhile(*we);
    if (auto* fe2 = dynamic_cast<const ast::ForExpr*>(&e))      return genFor(*fe2);
    if (auto* cl  = dynamic_cast<const ast::ClosureExpr*>(&e))  return genClosure(*cl);
    if (auto* la  = dynamic_cast<const ast::LambdaExpr*>(&e))   return genLambda(*la);
    if (auto* si  = dynamic_cast<const ast::StringInterp*>(&e)) return genStringInterp(*si);
    if (auto* aw  = dynamic_cast<const ast::AwaitExpr*>(&e))    return genAwait(*aw);
    if (auto* sp  = dynamic_cast<const ast::SpawnExpr*>(&e))    return genSpawn(*sp);
    if (auto* ye  = dynamic_cast<const ast::YieldExpr*>(&e))    return genYield(*ye);
    if (auto* re  = dynamic_cast<const ast::ReturnExpr*>(&e))   return genReturn(*re);
    if (auto* br  = dynamic_cast<const ast::BreakExpr*>(&e))    return genBreak(*br);
    if (auto* co  = dynamic_cast<const ast::ContinueExpr*>(&e)) return genContinue(*co);
    if (auto* ne  = dynamic_cast<const ast::NewExpr*>(&e))      return genNew(*ne);
    if (auto* de  = dynamic_cast<const ast::DeleteExpr*>(&e))   return genDelete(*de);
    if (auto* ar  = dynamic_cast<const ast::ArrayExpr*>(&e))    return genArray(*ar);
    if (auto* tu  = dynamic_cast<const ast::TupleExpr*>(&e))    return genTuple(*tu);
    if (auto* ma  = dynamic_cast<const ast::MapExpr*>(&e))      return genMap(*ma);
    if (auto* ra  = dynamic_cast<const ast::RangeExpr*>(&e))    return genRange(*ra);
    if (auto* ca  = dynamic_cast<const ast::CastExpr*>(&e))     return genCast(*ca);
    if (auto* tr  = dynamic_cast<const ast::TryExpr*>(&e))      return genTry(*tr);
    // Fallback: zero value of i64
    return builder_->emitConstInt(0, {types::TY_I64});
}

// ─── Literals ─────────────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genLiteral(const ast::LiteralExpr& e) {
    switch (e.tok.kind) {
    case TokenKind::Integer:
        if (auto* iv = std::get_if<int64_t>(&e.tok.literal))
            return builder_->emitConstInt(*iv, {types::TY_I64});
        return builder_->emitConstInt(0, {types::TY_I64});
    case TokenKind::Float:
        if (auto* fv = std::get_if<double>(&e.tok.literal))
            return builder_->emitConstFp(*fv, {types::TY_F64});
        return builder_->emitConstFp(0.0, {types::TY_F64});
    case TokenKind::String:
        if (auto* sv = std::get_if<std::string>(&e.tok.literal))
            return builder_->emitConstStr(*sv);
        return builder_->emitConstStr("");
    case TokenKind::Kw_true:  return builder_->emitConstBool(true);
    case TokenKind::Kw_false: return builder_->emitConstBool(false);
    case TokenKind::Kw_nil:   return builder_->emitConstNull({types::TY_RAWPTR});
    case TokenKind::Char:
        if (auto* sv = std::get_if<std::string>(&e.tok.literal)) {
            // Decode first Unicode code point from the char literal string
            uint32_t cp = 0;
            if (!sv->empty()) {
                unsigned char c0 = (unsigned char)(*sv)[0];
                if      (c0 < 0x80) { cp = c0; }
                else if (c0 < 0xE0 && sv->size() >= 2) { cp = ((c0 & 0x1F) << 6)  | (((unsigned char)(*sv)[1]) & 0x3F); }
                else if (c0 < 0xF0 && sv->size() >= 3) { cp = ((c0 & 0x0F) << 12) | ((((unsigned char)(*sv)[1]) & 0x3F) << 6)  | (((unsigned char)(*sv)[2]) & 0x3F); }
                else if (sv->size() >= 4)               { cp = ((c0 & 0x07) << 18) | ((((unsigned char)(*sv)[1]) & 0x3F) << 12) | ((((unsigned char)(*sv)[2]) & 0x3F) << 6) | (((unsigned char)(*sv)[3]) & 0x3F); }
            }
            return builder_->emitConstInt((int64_t)cp, {types::TY_CHAR});
        }
        return builder_->emitConstInt(e.tok.lexeme.empty() ? 0 : (unsigned char)e.tok.lexeme[0], {types::TY_CHAR});
    default:
        return builder_->emitConstInt(0, {types::TY_I64});
    }
}

// ─── Identifier ───────────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genIdent(const ast::IdentExpr& e, bool wantLval) {
    ValueEnv::Entry* entry = env_.lookup(e.name);
    if (entry) {
        if (wantLval) return entry->reg;
        if (entry->isPtr) return builder_->emitLoad(entry->reg, entry->type);
        return entry->reg;
    }
    // Check if it's a global constant/static whose address is in the module globals
    for (auto& g : irMod_.globals) {
        if (g.name == e.name) {
            ir::RegId ref = builder_->emitGlobalRef(e.name, g.type);
            if (wantLval) return ref;
            return builder_->emitLoad(ref, g.type);
        }
    }
    // Might be a function reference
    for (auto& fn : irMod_.functions) {
        if (fn.name == e.name) {
            return builder_->emitConstInt((int64_t)fn.id, {types::TY_RAWPTR});
        }
    }
    // Unknown — emit as global ref (runtime will resolve)
    return builder_->emitGlobalRef(e.name, {types::TY_I64});
}

// ─── Binary expression ────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genBinary(const ast::BinaryExpr& e) {
    ir::Opcode op;
    switch (e.op.kind) {
    case TokenKind::Plus:      op = ir::Opcode::Add;    break;
    case TokenKind::Minus:     op = ir::Opcode::Sub;    break;
    case TokenKind::Star:      op = ir::Opcode::Mul;    break;
    case TokenKind::Slash:     op = ir::Opcode::Div;    break;
    case TokenKind::Percent:   op = ir::Opcode::Mod;    break;
    case TokenKind::StarStar:  op = ir::Opcode::Pow;    break;
    case TokenKind::Ampersand: op = ir::Opcode::BitAnd; break;
    case TokenKind::Pipe:      op = ir::Opcode::BitOr;  break;
    case TokenKind::Caret:     op = ir::Opcode::BitXor; break;
    case TokenKind::LtLt:      op = ir::Opcode::Shl;    break;
    case TokenKind::GtGt:      op = ir::Opcode::Shr;    break;
    case TokenKind::GtGtGt:    op = ir::Opcode::Ushr;   break;
    case TokenKind::EqEq:      op = ir::Opcode::Eq;     break;
    case TokenKind::BangEq:    op = ir::Opcode::Ne;     break;
    case TokenKind::Lt:        op = ir::Opcode::Lt;     break;
    case TokenKind::LtEq:      op = ir::Opcode::Le;     break;
    case TokenKind::Gt:        op = ir::Opcode::Gt;     break;
    case TokenKind::GtEq:      op = ir::Opcode::Ge;     break;
    case TokenKind::AmpAmp:    op = ir::Opcode::And;    break;
    case TokenKind::PipePipe:  op = ir::Opcode::Or;     break;
    default:                   op = ir::Opcode::Add;    break;
    }

    ir::RegId lhs = genExpr(*e.lhs);
    ir::RegId rhs = genExpr(*e.rhs);

    // Determine the result type from the operand types with proper promotion
    auto& regTypes = irMod_.functions.back().regTypes;
    ir::IRType lhsTy = {types::TY_I64};
    ir::IRType rhsTy = {types::TY_I64};
    {
        auto il = regTypes.find(lhs);
        if (il != regTypes.end()) lhsTy = il->second;
        auto ir2 = regTypes.find(rhs);
        if (ir2 != regTypes.end()) rhsTy = ir2->second;
    }

    // Promote: if either side is float, result is float
    ir::IRType resultTy = lhsTy;
    if (typeReg_.isFloat(rhsTy.typeId) && !typeReg_.isFloat(lhsTy.typeId)) {
        lhs      = coerce(lhs, lhsTy, rhsTy);
        resultTy = rhsTy;
    } else if (!typeReg_.isFloat(rhsTy.typeId) && typeReg_.isFloat(lhsTy.typeId)) {
        rhs      = coerce(rhs, rhsTy, lhsTy);
        resultTy = lhsTy;
    } else if (typeReg_.isInteger(lhsTy.typeId) && typeReg_.isInteger(rhsTy.typeId)) {
        // Widen to the larger integer type
        size_t lsz = typeReg_.typeSize(lhsTy.typeId);
        size_t rsz = typeReg_.typeSize(rhsTy.typeId);
        if (rsz > lsz) { lhs = coerce(lhs, lhsTy, rhsTy); resultTy = rhsTy; }
        else if (lsz > rsz) { rhs = coerce(rhs, rhsTy, lhsTy); }
    }

    // Comparison ops always yield bool
    bool isCmp = (op == ir::Opcode::Eq || op == ir::Opcode::Ne ||
                  op == ir::Opcode::Lt || op == ir::Opcode::Le ||
                  op == ir::Opcode::Gt || op == ir::Opcode::Ge);
    bool isLogical = (op == ir::Opcode::And || op == ir::Opcode::Or);

    ir::IRType outTy = (isCmp || isLogical) ? ir::IRType{types::TY_BOOL} : resultTy;

    return builder_->emitBinop(op, lhs, rhs, outTy);
}

// ─── Unary expression ─────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genUnary(const ast::UnaryExpr& e) {
    switch (e.op.kind) {
    case TokenKind::Minus: {
        ir::RegId val = genExpr(*e.expr);
        auto& regTypes = irMod_.functions.back().regTypes;
        ir::IRType ty = {types::TY_I64};
        auto it = regTypes.find(val);
        if (it != regTypes.end()) ty = it->second;
        return builder_->emitUnop(ir::Opcode::Neg, val, ty);
    }
    case TokenKind::Bang: {
        ir::RegId val = genExpr(*e.expr);
        return builder_->emitUnop(ir::Opcode::Not, val, {types::TY_BOOL});
    }
    case TokenKind::Tilde: {
        ir::RegId val = genExpr(*e.expr);
        auto& regTypes = irMod_.functions.back().regTypes;
        ir::IRType ty = {types::TY_I64};
        auto it = regTypes.find(val);
        if (it != regTypes.end()) ty = it->second;
        return builder_->emitUnop(ir::Opcode::BitNot, val, ty);
    }
    case TokenKind::Ampersand: {
        // address-of: obtain the L-value pointer
        ir::RegId lval = genExpr(*e.expr, /*wantLval=*/true);
        return lval;
    }
    case TokenKind::Star: {
        // dereference
        ir::RegId ptr = genExpr(*e.expr);
        auto& regTypes = irMod_.functions.back().regTypes;
        ir::IRType ptrTy = {types::TY_RAWPTR};
        auto it = regTypes.find(ptr);
        if (it != regTypes.end()) ptrTy = it->second;
        // pointee type is one level of indirection less
        ir::IRType elemTy = ptrTy;
        elemTy.isPtr   = ptrTy.ptrDepth > 1;
        elemTy.ptrDepth = ptrTy.ptrDepth > 0 ? ptrTy.ptrDepth - 1 : 0;
        return builder_->emitLoad(ptr, elemTy);
    }
    default:
        return genExpr(*e.expr);
    }
}

// ─── Assignment ───────────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genAssign(const ast::AssignExpr& e) {
    ir::RegId lval = genExpr(*e.lhs, /*wantLval=*/true);
    ir::RegId rval = genExpr(*e.rhs);

    // Resolve lvalue type for load-binop-store in compound assignments
    auto& regTypes = irMod_.functions.back().regTypes;
    ir::IRType lvalTy = {types::TY_I64};
    {
        auto it = regTypes.find(lval);
        if (it != regTypes.end()) {
            lvalTy = it->second;
            // Dereference pointer type to get the stored value type
            if (lvalTy.isPtr && lvalTy.ptrDepth > 0) {
                lvalTy.ptrDepth--;
                lvalTy.isPtr = (lvalTy.ptrDepth > 0);
            }
        }
    }

    if (e.op.kind != TokenKind::Eq) {
        ir::RegId cur = builder_->emitLoad(lval, lvalTy);
        ir::Opcode bop;
        switch (e.op.kind) {
        case TokenKind::PlusEq:    bop = ir::Opcode::Add;    break;
        case TokenKind::MinusEq:   bop = ir::Opcode::Sub;    break;
        case TokenKind::StarEq:    bop = ir::Opcode::Mul;    break;
        case TokenKind::SlashEq:   bop = ir::Opcode::Div;    break;
        case TokenKind::PercentEq: bop = ir::Opcode::Mod;    break;
        case TokenKind::AmpEq:     bop = ir::Opcode::BitAnd; break;
        case TokenKind::PipeEq:    bop = ir::Opcode::BitOr;  break;
        case TokenKind::CaretEq:   bop = ir::Opcode::BitXor; break;
        case TokenKind::LtLtEq:    bop = ir::Opcode::Shl;    break;
        case TokenKind::GtGtEq:    bop = ir::Opcode::Shr;    break;
        default:                   bop = ir::Opcode::Add;    break;
        }
        // Coerce rval to lvalTy if needed before the binop
        ir::IRType rvalTy = {types::TY_I64};
        {
            auto it = regTypes.find(rval);
            if (it != regTypes.end()) rvalTy = it->second;
        }
        if (rvalTy.typeId != lvalTy.typeId && rvalTy.typeId != types::TY_UNKNOWN)
            rval = coerce(rval, rvalTy, lvalTy);
        rval = builder_->emitBinop(bop, cur, rval, lvalTy);
    }
    builder_->emitStore(rval, lval);
    return rval;
}

// ─── Function call ────────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genCall(const ast::CallExpr& e) {
    // Direct named call
    if (auto* ident = dynamic_cast<const ast::IdentExpr*>(e.callee.get())) {
        std::vector<ir::RegId> argRegs;
        for (auto& arg : e.args) argRegs.push_back(genExpr(*arg));

        // Search for a compiled function in the module
        for (auto& fn : irMod_.functions) {
            if (fn.name == ident->name) {
                ir::IRType retTy = fn.retType;
                // Look up the pre-declared return type if available
                auto rtIt = fnReturnTypes_.find(fn.name);
                if (rtIt != fnReturnTypes_.end()) retTy = rtIt->second;
                return builder_->emitCall(fn.id, std::move(argRegs), retTy);
            }
        }

        // Check pre-declared return type for forward references
        {
            auto rtIt = fnReturnTypes_.find(ident->name);
            ir::IRType retTy = (rtIt != fnReturnTypes_.end()) ? rtIt->second : ir::IRType{types::TY_I64};
            // Still look it up as a function that will be defined later
            return builder_->emitIntrinsic("__call_" + ident->name, std::move(argRegs), retTy);
        }
    }

    // Method-call shorthand via FieldExpr(CallExpr) is handled in genMethod.
    // Indirect call through a function pointer
    ir::RegId callee = genExpr(*e.callee);
    std::vector<ir::RegId> argRegs;
    for (auto& arg : e.args) argRegs.push_back(genExpr(*arg));
    return builder_->emitIndirectCall(callee, std::move(argRegs), {types::TY_I64});
}

// ─── Index expression ─────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genIndex(const ast::IndexExpr& e, bool wantLval) {
    ir::RegId obj = genExpr(*e.obj);
    ir::RegId idx = genExpr(*e.idx);

    // Determine element type from the object's registered type
    auto& regTypes = irMod_.functions.back().regTypes;
    ir::IRType elemTy = {types::TY_I64};
    {
        auto it = regTypes.find(obj);
        if (it != regTypes.end()) {
            const types::Type* ty = typeReg_.get(it->second.typeId);
            if (ty && ty->elemTy != types::TY_UNKNOWN)
                elemTy = toIRType(ty->elemTy);
        }
    }

    // Bounds check (runtime guard)
    ir::RegId len = builder_->emitIntrinsic("len", {obj}, {types::TY_I64});
    emitBoundsCheck(idx, len, e.loc);

    ir::RegId ptr = builder_->emitGEP(obj, {idx}, elemTy);
    if (wantLval) return ptr;
    return builder_->emitLoad(ptr, elemTy);
}

// ─── Field access ─────────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genField(const ast::FieldExpr& e, bool wantLval) {
    ir::RegId obj = genExpr(*e.obj);

    // Safe navigation: if null, short-circuit and return null
    if (e.isSafe) {
        ir::RegId isNull  = builder_->emitIntrinsic("__is_null", {obj}, {types::TY_BOOL});
        ir::BlockId nullB = builder_->createBlock("field.null");
        ir::BlockId okB   = builder_->createBlock("field.ok");
        builder_->emitBranch(isNull, nullB, okB);
        builder_->setBlock(nullB);
        ir::RegId nullVal = builder_->emitConstNull({types::TY_RAWPTR});
        ir::BlockId nullExit = builder_->currentBlock();
        builder_->emitJump(okB);
        builder_->setBlock(okB);
        // Phi: either null or the real field value — computed below
        (void)nullExit; (void)nullVal;
    }

    // Look up the struct/class type for the object register
    auto& regTypes = irMod_.functions.back().regTypes;
    uint32_t fieldIdx = 0;
    ir::IRType fieldTy = {types::TY_I64};
    bool found = false;

    auto it = regTypes.find(obj);
    if (it != regTypes.end()) {
        types::TypeId tid = typeReg_.resolved(it->second.typeId);
        const types::Type* ty = typeReg_.get(tid);
        if (ty) {
            for (uint32_t i = 0; i < ty->fields.size(); ++i) {
                if (ty->fields[i].name == e.field) {
                    fieldIdx = i;
                    fieldTy  = toIRType(ty->fields[i].type);
                    found    = true;
                    break;
                }
            }
        }
    }

    if (!found) {
        // Fallback: scan all registered types for the field name
        for (types::TypeId tid = types::TY_FIRST_USER; ; ++tid) {
            const types::Type* ty = typeReg_.get(tid);
            if (!ty) break;
            for (uint32_t i = 0; i < (uint32_t)ty->fields.size(); ++i) {
                if (ty->fields[i].name == e.field) {
                    fieldIdx = i;
                    fieldTy  = toIRType(ty->fields[i].type);
                    found    = true;
                    break;
                }
            }
            if (found) break;
        }
    }

    ir::RegId ptr = builder_->emitFieldPtr(obj, fieldIdx, fieldTy);
    if (wantLval) return ptr;
    return builder_->emitLoad(ptr, fieldTy);
}

// ─── Method call ──────────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genMethod(const ast::MethodExpr& e) {
    ir::RegId obj = genExpr(*e.obj);

    // Safe navigation: guard null dereference
    if (e.isSafe) {
        ir::RegId isNull = builder_->emitIntrinsic("__is_null", {obj}, {types::TY_BOOL});
        ir::BlockId nullB = builder_->createBlock("method.null");
        ir::BlockId okB   = builder_->createBlock("method.ok");
        builder_->emitBranch(isNull, nullB, okB);
        builder_->setBlock(nullB);
        ir::RegId nilRet = builder_->emitConstNull({types::TY_RAWPTR});
        builder_->emitJump(okB);
        builder_->setBlock(okB);
        (void)nilRet;
    }

    std::vector<ir::RegId> args = {obj};
    for (auto& arg : e.args) args.push_back(genExpr(*arg));

    // Determine the concrete type of the receiver to look up the method
    auto& regTypes = irMod_.functions.back().regTypes;
    std::string typeName;
    {
        auto it = regTypes.find(obj);
        if (it != regTypes.end()) {
            const types::Type* ty = typeReg_.get(typeReg_.resolved(it->second.typeId));
            if (ty) typeName = ty->name;
        }
    }

    // Look up a pre-compiled TypeName::method function
    if (!typeName.empty()) {
        std::string qualName = typeName + "::" + e.method;
        for (auto& fn : irMod_.functions) {
            if (fn.name == qualName) {
                return builder_->emitCall(fn.id, std::move(args), fn.retType);
            }
        }
    }

    // Check pre-declared return type for the method
    ir::IRType retTy = {types::TY_I64};
    {
        std::string qualName = typeName + "::" + e.method;
        auto rtIt = fnReturnTypes_.find(qualName);
        if (rtIt == fnReturnTypes_.end()) rtIt = fnReturnTypes_.find(e.method);
        if (rtIt != fnReturnTypes_.end()) retTy = rtIt->second;
    }

    return builder_->emitIntrinsic("__method_" + e.method, std::move(args), retTy);
}

// ─── Block ────────────────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genBlock(const ast::BlockExpr& e) {
    env_.pushScope();
    for (auto& stmt : e.stmts) genStmt(*stmt);
    ir::RegId result = ir::REG_NONE;
    if (e.tail) result = genExpr(**e.tail);
    env_.popScope();
    if (result == ir::REG_NONE) return builder_->emitConstInt(0, {types::TY_UNIT});
    return result;
}

// ─── If expression ────────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genIf(const ast::IfExpr& e) {
    ir::RegId cond = genExpr(*e.cond);

    ir::BlockId thenBlock  = builder_->createBlock("if.then");
    ir::BlockId elseBlock  = e.els ? builder_->createBlock("if.else") : ir::BLOCK_NONE;
    ir::BlockId mergeBlock = builder_->createBlock("if.merge");

    builder_->emitBranch(cond, thenBlock,
                         elseBlock != ir::BLOCK_NONE ? elseBlock : mergeBlock);

    // Then branch
    builder_->setBlock(thenBlock);
    env_.pushScope();
    ir::RegId thenReg = genExpr(*e.then);
    ir::BlockId thenExit = builder_->currentBlock();
    env_.popScope();
    builder_->emitJump(mergeBlock);

    // Else branch
    ir::RegId  elseReg  = ir::REG_NONE;
    ir::BlockId elseExit = ir::BLOCK_NONE;
    if (e.els && elseBlock != ir::BLOCK_NONE) {
        builder_->setBlock(elseBlock);
        env_.pushScope();
        elseReg  = genExpr(**e.els);
        elseExit = builder_->currentBlock();
        env_.popScope();
        builder_->emitJump(mergeBlock);
    }

    builder_->setBlock(mergeBlock);

    // Emit Phi if both branches produce a value
    if (thenReg != ir::REG_NONE && elseReg != ir::REG_NONE && elseExit != ir::BLOCK_NONE) {
        // Determine phi type: prefer the then-side type, coerce else if needed
        auto& regTypes = irMod_.functions.back().regTypes;
        ir::IRType thenTy = {types::TY_I64};
        ir::IRType elseTy = {types::TY_I64};
        {
            auto it = regTypes.find(thenReg);
            if (it != regTypes.end()) thenTy = it->second;
            auto it2 = regTypes.find(elseReg);
            if (it2 != regTypes.end()) elseTy = it2->second;
        }
        // Coerce else to then-type (both sides of if should agree)
        if (elseTy.typeId != thenTy.typeId && elseTy.typeId != types::TY_UNKNOWN &&
            thenTy.typeId != types::TY_UNKNOWN) {
            // Switch back to else exit block to emit cast, then return to merge
            builder_->setBlock(elseExit);
            elseReg = coerce(elseReg, elseTy, thenTy);
            builder_->emitJump(mergeBlock);
            builder_->setBlock(mergeBlock);
        }
        return builder_->emitPhi(thenTy, {{thenReg, thenExit}, {elseReg, elseExit}});
    }

    return builder_->emitConstInt(0, {types::TY_UNIT});
}

// ─── Match expression ─────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genMatch(const ast::MatchExpr& e) {
    ir::RegId scrutinee = genExpr(*e.expr);

    // Determine scrutinee type
    auto& regTypes = irMod_.functions.back().regTypes;
    ir::IRType scrutTy = {types::TY_I64};
    {
        auto it = regTypes.find(scrutinee);
        if (it != regTypes.end()) scrutTy = it->second;
    }

    ir::BlockId mergeBlock = builder_->createBlock("match.merge");
    std::vector<std::pair<ir::RegId, ir::BlockId>> results;

    for (size_t i = 0; i < e.arms.size(); ++i) {
        auto& arm = e.arms[i];
        bool isLast = (i + 1 == e.arms.size());

        ir::BlockId armBlock  = builder_->createBlock("match.arm"  + std::to_string(i));
        ir::BlockId nextBlock = isLast ? mergeBlock
                                       : builder_->createBlock("match.next" + std::to_string(i));

        // Emit pattern test; jump to armBlock on match, nextBlock on fail
        genPattern(*arm.pat, scrutinee, scrutTy, nextBlock);
        builder_->emitJump(armBlock);

        builder_->setBlock(armBlock);
        env_.pushScope();
        genPatternBindings(*arm.pat, scrutinee, scrutTy);

        // Optional guard
        if (arm.guard) {
            ir::RegId guardVal  = genExpr(**arm.guard);
            ir::BlockId skipBlk = builder_->createBlock("match.guard_fail" + std::to_string(i));
            builder_->emitBranch(guardVal, armBlock, skipBlk);
            builder_->setBlock(skipBlk);
            builder_->emitJump(nextBlock);
            builder_->setBlock(armBlock);
        }

        ir::RegId bodyReg = genExpr(*arm.body);
        results.emplace_back(bodyReg, builder_->currentBlock());
        env_.popScope();
        builder_->emitJump(mergeBlock);

        if (!isLast) builder_->setBlock(nextBlock);
    }

    builder_->setBlock(mergeBlock);

    if (!results.empty()) {
        ir::IRType phiTy = scrutTy; // approximate; body type may differ
        {
            auto it = regTypes.find(results[0].first);
            if (it != regTypes.end()) phiTy = it->second;
        }
        std::vector<ir::PhiEdge> edges;
        edges.reserve(results.size());
        for (auto& [r, b] : results) edges.push_back({r, b});
        return builder_->emitPhi(phiTy, std::move(edges));
    }
    return builder_->emitConstInt(0, {types::TY_UNIT});
}

// ─── While loop ───────────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genWhile(const ast::WhileExpr& e) {
    ir::BlockId condBlock = builder_->createBlock("while.cond");
    ir::BlockId bodyBlock = builder_->createBlock("while.body");
    ir::BlockId exitBlock = builder_->createBlock("while.exit");

    pushLoop({condBlock, exitBlock, {}, e.label.value_or("")});
    builder_->emitJump(condBlock);
    builder_->setBlock(condBlock);

    if (e.cond) {
        ir::RegId cond = genExpr(**e.cond);
        builder_->emitBranch(cond, bodyBlock, exitBlock);
    } else {
        builder_->emitJump(bodyBlock); // `loop { }` — infinite
    }

    builder_->setBlock(bodyBlock);
    env_.pushScope();
    genExpr(*e.body);
    env_.popScope();
    builder_->emitJump(condBlock);

    builder_->setBlock(exitBlock);
    popLoop();
    return builder_->emitConstInt(0, {types::TY_UNIT});
}

// ─── For loop ─────────────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genFor(const ast::ForExpr& e) {
    // Determine iterator element type from the iterable's registered type
    ir::RegId iter = genExpr(*e.iter);
    auto& regTypes = irMod_.functions.back().regTypes;
    ir::IRType elemTy = {types::TY_I64};
    {
        auto it = regTypes.find(iter);
        if (it != regTypes.end()) {
            const types::Type* ty = typeReg_.get(typeReg_.resolved(it->second.typeId));
            if (ty && ty->elemTy != types::TY_UNKNOWN)
                elemTy = toIRType(ty->elemTy);
        }
    }

    ir::BlockId condBlock = builder_->createBlock("for.cond");
    ir::BlockId bodyBlock = builder_->createBlock("for.body");
    ir::BlockId incrBlock = builder_->createBlock("for.incr");
    ir::BlockId exitBlock = builder_->createBlock("for.exit");

    // Index counter
    ir::RegId idxPtr = builder_->emitAlloc({types::TY_I64});
    builder_->emitStore(builder_->emitConstInt(0, {types::TY_I64}), idxPtr);

    pushLoop({incrBlock, exitBlock, {}, e.label.value_or("")});
    builder_->emitJump(condBlock);
    builder_->setBlock(condBlock);

    ir::RegId idx     = builder_->emitLoad(idxPtr, {types::TY_I64});
    ir::RegId len     = builder_->emitIntrinsic("len", {iter}, {types::TY_I64});
    ir::RegId inBound = builder_->emitBinop(ir::Opcode::Lt, idx, len, {types::TY_BOOL});
    builder_->emitBranch(inBound, bodyBlock, exitBlock);

    builder_->setBlock(bodyBlock);
    env_.pushScope();

    // Bind the loop variable(s) via the pattern
    ir::RegId elemPtr = builder_->emitGEP(iter, {idx}, elemTy);
    ir::RegId elemVal = builder_->emitLoad(elemPtr, elemTy);
    genPatternBindings(*e.pat, elemVal, elemTy);

    genExpr(*e.body);
    env_.popScope();
    builder_->emitJump(incrBlock);

    // Increment
    builder_->setBlock(incrBlock);
    ir::RegId idx2   = builder_->emitLoad(idxPtr, {types::TY_I64});
    ir::RegId one    = builder_->emitConstInt(1, {types::TY_I64});
    ir::RegId newIdx = builder_->emitBinop(ir::Opcode::Add, idx2, one, {types::TY_I64});
    builder_->emitStore(newIdx, idxPtr);
    builder_->emitJump(condBlock);

    builder_->setBlock(exitBlock);
    popLoop();
    return builder_->emitConstInt(0, {types::TY_UNIT});
}

// ─── Closure ──────────────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genClosure(const ast::ClosureExpr& e) {
    std::string closureName = "__closure_" + std::to_string(irMod_.functions.size());

    // Collect captured variables via a recursive scan of the body
    std::vector<std::string> capturedNames;
    std::vector<ir::RegId>   capturedRegs;
    {
        std::set<std::string> paramNames;
        for (auto& [pname, _] : e.params) paramNames.insert(pname);

        std::function<void(const ast::Stmt&)> scanStmt;
        std::function<void(const ast::Expr&)> scanExpr = [&](const ast::Expr& ex) {
            if (auto* ie = dynamic_cast<const ast::IdentExpr*>(&ex)) {
                if (!paramNames.count(ie->name)) {
                    ValueEnv::Entry* entry = env_.lookup(ie->name);
                    if (entry && entry->reg != ir::REG_NONE) {
                        bool dup = false;
                        for (auto& n : capturedNames) if (n == ie->name) { dup = true; break; }
                        if (!dup) {
                            capturedNames.push_back(ie->name);
                            ir::RegId val = entry->isPtr
                                ? builder_->emitLoad(entry->reg, entry->type)
                                : entry->reg;
                            capturedRegs.push_back(val);
                        }
                    }
                }
            }
            // Recurse into compound expressions
            if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&ex)) {
                scanExpr(*be->lhs); scanExpr(*be->rhs);
            } else if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&ex)) {
                scanExpr(*ue->expr);
            } else if (auto* ce = dynamic_cast<const ast::CallExpr*>(&ex)) {
                scanExpr(*ce->callee);
                for (auto& a : ce->args) scanExpr(*a);
            } else if (auto* ie2 = dynamic_cast<const ast::IfExpr*>(&ex)) {
                scanExpr(*ie2->cond); scanExpr(*ie2->then);
                if (ie2->els) scanExpr(**ie2->els);
            } else if (auto* be2 = dynamic_cast<const ast::BlockExpr*>(&ex)) {
                for (auto& s : be2->stmts) scanStmt(*s);
                if (be2->tail) scanExpr(**be2->tail);
            } else if (auto* me = dynamic_cast<const ast::MethodExpr*>(&ex)) {
                scanExpr(*me->obj);
                for (auto& a : me->args) scanExpr(*a);
            } else if (auto* fe = dynamic_cast<const ast::FieldExpr*>(&ex)) {
                scanExpr(*fe->obj);
            } else if (auto* ae = dynamic_cast<const ast::AssignExpr*>(&ex)) {
                scanExpr(*ae->lhs); scanExpr(*ae->rhs);
            }
        };
        scanStmt = [&](const ast::Stmt& st) {
            if (auto* es = dynamic_cast<const ast::ExprStmt*>(&st)) scanExpr(*es->expr);
            else if (auto* ls = dynamic_cast<const ast::LetStmt*>(&st)) {
                if (ls->init) scanExpr(**ls->init);
            }
        };
        for (auto& stmt : e.body) scanStmt(*stmt);
    }

    // Build the lifted function: explicit params + captured params (environment)
    std::vector<ir::IRParam> liftedParams;
    for (auto& [pname, pty] : e.params) {
        ir::IRParam p;
        p.name = pname;
        p.type = pty ? toIRType(*pty) : ir::IRType{types::TY_I64};
        liftedParams.push_back(std::move(p));
    }
    for (auto& capName : capturedNames) {
        ir::IRParam cp;
        cp.name = "__cap_" + capName;
        // Preserve the captured variable's type
        ValueEnv::Entry* ent = env_.lookup(capName);
        cp.type = ent ? ent->type : ir::IRType{types::TY_I64};
        liftedParams.push_back(std::move(cp));
    }

    ir::IRType retTy = e.retTy ? toIRType(**e.retTy) : ir::IRType{types::TY_I64};
    ir::FuncId closureFuncId = builder_->beginFunction(closureName, liftedParams, retTy);
    {
        auto& fn = irMod_.functions.back();
        fn.isAsync   = e.isAsync;
        fn.linkage   = "internal";
    }

    env_.pushScope();
    ir::RegId pr = 1;
    for (auto& [pname, pty] : e.params) {
        ir::IRType pty_ir = pty ? toIRType(*pty) : ir::IRType{types::TY_I64};
        ir::RegId ptr = builder_->emitAlloc(pty_ir);
        builder_->emitStore(pr, ptr);
        ValueEnv::Entry entry; entry.reg = ptr; entry.isPtr = true; entry.type = pty_ir; entry.isMut = false;
        env_.define(pname, entry);
        ++pr;
    }
    for (auto& capName : capturedNames) {
        ValueEnv::Entry* outer = env_.lookup(capName);
        ir::IRType capTy = outer ? outer->type : ir::IRType{types::TY_I64};
        ir::RegId ptr = builder_->emitAlloc(capTy);
        builder_->emitStore(pr, ptr);
        ValueEnv::Entry entry; entry.reg = ptr; entry.isPtr = true; entry.type = capTy; entry.isMut = true;
        env_.define(capName, entry);
        ++pr;
    }

    for (auto& stmt : e.body) genStmt(*stmt);

    // Ensure terminator
    {
        auto& fn  = irMod_.functions.back();
        auto& blk = fn.blocks[builder_->currentBlock()];
        bool needsTerm = blk.insts.empty() ||
            (blk.insts.back().op != ir::Opcode::Return &&
             blk.insts.back().op != ir::Opcode::Jump);
        if (needsTerm) {
            if (retTy.typeId == types::TY_UNIT) builder_->emitReturn({});
            else builder_->emitReturn(builder_->emitConstInt(0, retTy));
        }
    }
    env_.popScope();
    builder_->endFunction();

    // Emit __make_closure in the caller's context
    std::vector<ir::RegId> closureArgs;
    closureArgs.push_back(builder_->emitConstInt((int64_t)closureFuncId, {types::TY_I64}));
    for (ir::RegId capReg : capturedRegs) closureArgs.push_back(capReg);

    return builder_->emitIntrinsic("__make_closure", closureArgs, {types::TY_RAWPTR});
}

// ─── Lambda ───────────────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genLambda(const ast::LambdaExpr& e) {
    std::string lambdaName = "__lambda_" + std::to_string(irMod_.functions.size());

    std::vector<ir::IRParam> params;
    for (auto& [pname, pty] : e.params) {
        ir::IRParam p;
        p.name = pname;
        p.type = pty ? toIRType(*pty) : ir::IRType{types::TY_I64};
        params.push_back(std::move(p));
    }

    // Infer return type from the body expression
    ir::IRType retTy{types::TY_I64};
    if (auto* le = dynamic_cast<const ast::LiteralExpr*>(e.body.get())) {
        switch (le->tok.kind) {
        case TokenKind::Float:    retTy = {types::TY_F64};  break;
        case TokenKind::String:   retTy = {types::TY_STR};  break;
        case TokenKind::Kw_true:
        case TokenKind::Kw_false: retTy = {types::TY_BOOL}; break;
        default:                  retTy = {types::TY_I64};  break;
        }
    }

    ir::FuncId lambdaId = builder_->beginFunction(lambdaName, params, retTy);
    irMod_.functions.back().linkage = "internal";

    env_.pushScope();
    ir::RegId pr = 1;
    for (auto& [pname, pty] : e.params) {
        ir::IRType pty_ir = pty ? toIRType(*pty) : ir::IRType{types::TY_I64};
        ir::RegId ptr = builder_->emitAlloc(pty_ir);
        builder_->emitStore(pr, ptr);
        ValueEnv::Entry entry; entry.reg = ptr; entry.isPtr = true; entry.type = pty_ir; entry.isMut = false;
        env_.define(pname, entry);
        ++pr;
    }

    ir::RegId bodyReg = genExpr(*e.body);

    // Coerce body result to declared return type
    {
        auto& fn = irMod_.functions.back();
        auto it  = fn.regTypes.find(bodyReg);
        if (it != fn.regTypes.end() && it->second.typeId != retTy.typeId &&
            it->second.typeId != types::TY_UNKNOWN && retTy.typeId != types::TY_UNKNOWN) {
            bodyReg = coerce(bodyReg, it->second, retTy);
        }
    }

    builder_->emitReturn(bodyReg);
    env_.popScope();
    builder_->endFunction();

    return builder_->emitConstInt((int64_t)lambdaId, {types::TY_I64});
}

// ─── String interpolation ─────────────────────────────────────────────────────

ir::RegId CodeGenerator::genStringInterp(const ast::StringInterp& e) {
    ir::RegId result = builder_->emitConstStr("");
    for (auto& part : e.parts) {
        ir::RegId partReg;
        if (auto* s = std::get_if<std::string>(&part)) {
            partReg = builder_->emitConstStr(*s);
        } else if (auto* ep = std::get_if<ast::ExprPtr>(&part)) {
            ir::RegId exprReg = genExpr(**ep);
            // Emit a typed to_string conversion so the runtime formats correctly
            auto& regTypes = irMod_.functions.back().regTypes;
            std::string conv = "to_string";
            {
                auto it = regTypes.find(exprReg);
                if (it != regTypes.end()) {
                    if (typeReg_.isFloat(it->second.typeId))  conv = "__to_string_f64";
                    else if (it->second.typeId == types::TY_BOOL) conv = "__to_string_bool";
                    else if (it->second.typeId == types::TY_CHAR) conv = "__to_string_char";
                    else if (typeReg_.isInteger(it->second.typeId)) conv = "__to_string_i64";
                }
            }
            partReg = builder_->emitIntrinsic(conv, {exprReg}, {types::TY_STR});
        } else { continue; }
        result = builder_->emitBinop(ir::Opcode::Add, result, partReg, {types::TY_STR});
    }
    return result;
}

// ─── Async / concurrency ──────────────────────────────────────────────────────

ir::RegId CodeGenerator::genAwait(const ast::AwaitExpr& e) {
    ir::RegId future = genExpr(*e.expr);
    return builder_->emitAwait(future);
}

ir::RegId CodeGenerator::genSpawn(const ast::SpawnExpr& e) {
    ir::RegId fn = genExpr(*e.expr);
    return builder_->emitSpawn(fn, {});
}

ir::RegId CodeGenerator::genYield(const ast::YieldExpr& e) {
    ir::RegId val = e.val
        ? genExpr(**e.val)
        : builder_->emitConstInt(0, {types::TY_UNIT});
    return builder_->emitIntrinsic("__yield", {val}, {types::TY_UNIT});
}

// ─── Control flow ─────────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genReturn(const ast::ReturnExpr& e) {
    ir::RegId val;
    if (e.val) {
        val = genExpr(**e.val);
        // Coerce to function's declared return type
        if (!irMod_.functions.empty()) {
            ir::IRType retTy = irMod_.functions.back().retType;
            auto& regTypes   = irMod_.functions.back().regTypes;
            auto it = regTypes.find(val);
            if (it != regTypes.end() && it->second.typeId != retTy.typeId &&
                it->second.typeId != types::TY_UNKNOWN && retTy.typeId != types::TY_UNKNOWN) {
                val = coerce(val, it->second, retTy);
            }
        }
        builder_->emitReturn(val);
    } else {
        builder_->emitReturn({});
        val = builder_->emitConstInt(0, {types::TY_UNIT});
    }
    return val;
}

ir::RegId CodeGenerator::genBreak(const ast::BreakExpr& e) {
    if (!inLoop()) return builder_->emitConstInt(0, {types::TY_UNIT});
    LoopContext* loop = nullptr;
    if (e.label) {
        for (int i = (int)loopStack_.size() - 1; i >= 0; --i) {
            if (loopStack_[i].label == *e.label) { loop = &loopStack_[i]; break; }
        }
    } else {
        loop = &loopStack_.back();
    }
    if (!loop) return builder_->emitConstInt(0, {types::TY_UNIT});
    if (e.val) {
        ir::RegId val = genExpr(**e.val);
        loop->breakVal = val;
    }
    builder_->emitJump(loop->breakTarget);
    return builder_->emitConstInt(0, {types::TY_UNIT});
}

ir::RegId CodeGenerator::genContinue(const ast::ContinueExpr& e) {
    if (!inLoop()) return builder_->emitConstInt(0, {types::TY_UNIT});
    LoopContext* loop = nullptr;
    if (e.label) {
        for (int i = (int)loopStack_.size() - 1; i >= 0; --i) {
            if (loopStack_[i].label == *e.label) { loop = &loopStack_[i]; break; }
        }
    } else {
        loop = &loopStack_.back();
    }
    if (loop) builder_->emitJump(loop->continueTarget);
    return builder_->emitConstInt(0, {types::TY_UNIT});
}

// ─── Memory management ────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genNew(const ast::NewExpr& e) {
    ir::IRType ty = e.ty ? toIRType(*e.ty) : ir::IRType{types::TY_I64};
    ir::RegId ptr = builder_->emitAlloc(ty);

    // If the type is a struct, find its constructor and call it with the supplied args
    if (e.ty) {
        if (auto* nt = dynamic_cast<const ast::NamedType*>(e.ty.get())) {
            for (auto& fn : irMod_.functions) {
                if (fn.name == nt->name) {
                    std::vector<ir::RegId> args;
                    for (auto& arg : e.args) args.push_back(genExpr(*arg));
                    return builder_->emitCall(fn.id, std::move(args), fn.retType);
                }
            }
        }
    }

    if (!e.args.empty()) {
        ir::RegId val = genExpr(*e.args[0]);
        builder_->emitStore(val, ptr);
    }
    return ptr;
}

ir::RegId CodeGenerator::genDelete(const ast::DeleteExpr& e) {
    ir::RegId ptr = genExpr(*e.ptr);
    builder_->emitFree(ptr);
    return builder_->emitConstInt(0, {types::TY_UNIT});
}

// ─── Collection constructors ──────────────────────────────────────────────────

ir::RegId CodeGenerator::genArray(const ast::ArrayExpr& e) {
    std::vector<ir::RegId> elems;
    elems.reserve(e.elems.size());
    for (auto& elem : e.elems) elems.push_back(genExpr(*elem));
    ir::RegId lenReg = builder_->emitConstInt((int64_t)e.elems.size(), {types::TY_I64});
    std::vector<ir::RegId> args = {lenReg};
    args.insert(args.end(), elems.begin(), elems.end());
    return builder_->emitIntrinsic("__make_array", args, {types::TY_RAWPTR});
}

ir::RegId CodeGenerator::genTuple(const ast::TupleExpr& e) {
    std::vector<ir::RegId> elems;
    elems.reserve(e.elems.size());
    for (auto& elem : e.elems) elems.push_back(genExpr(*elem));
    return builder_->emitIntrinsic("__make_tuple", elems, {types::TY_RAWPTR});
}

ir::RegId CodeGenerator::genMap(const ast::MapExpr& e) {
    std::vector<ir::RegId> kvs;
    kvs.reserve(e.pairs.size() * 2);
    for (auto& [k, v] : e.pairs) {
        kvs.push_back(genExpr(*k));
        kvs.push_back(genExpr(*v));
    }
    return builder_->emitIntrinsic("__make_map", kvs, {types::TY_RAWPTR});
}

ir::RegId CodeGenerator::genRange(const ast::RangeExpr& e) {
    ir::RegId lo  = genExpr(*e.lo);
    ir::RegId hi  = genExpr(*e.hi);
    ir::RegId inc = builder_->emitConstBool(e.inclusive);
    return builder_->emitIntrinsic("__make_range", {lo, hi, inc}, {types::TY_RAWPTR});
}

// ─── Cast expression ──────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genCast(const ast::CastExpr& e) {
    ir::RegId val   = genExpr(*e.expr);
    ir::IRType toTy = e.ty ? toIRType(*e.ty) : ir::IRType{types::TY_I64};

    auto& regTypes = irMod_.functions.back().regTypes;
    ir::IRType fromTy = {types::TY_I64};
    {
        auto it = regTypes.find(val);
        if (it != regTypes.end()) fromTy = it->second;
    }

    // Pointer ↔ integer casts
    if (toTy.isPtr && typeReg_.isInteger(fromTy.typeId))
        return builder_->emitCast(ir::Opcode::IntToPtr, val, fromTy, toTy);
    if (fromTy.isPtr && typeReg_.isInteger(toTy.typeId))
        return builder_->emitCast(ir::Opcode::PtrToInt, val, fromTy, toTy);

    return coerce(val, fromTy, toTy);
}

// ─── Try (?) operator ─────────────────────────────────────────────────────────

ir::RegId CodeGenerator::genTry(const ast::TryExpr& e) {
    ir::RegId result = genExpr(*e.expr);

    ir::RegId isOk    = builder_->emitIntrinsic("__is_ok", {result}, {types::TY_BOOL});
    ir::BlockId okBlk  = builder_->createBlock("try.ok");
    ir::BlockId errBlk = builder_->createBlock("try.err");
    builder_->emitBranch(isOk, okBlk, errBlk);

    // Error path: propagate the error upward (early return)
    builder_->setBlock(errBlk);
    ir::RegId errVal = builder_->emitIntrinsic("__unwrap_err", {result}, {types::TY_I64});
    builder_->emitReturn(errVal);

    // Ok path: unwrap the value
    builder_->setBlock(okBlk);
    return builder_->emitIntrinsic("__unwrap_ok", {result}, {types::TY_I64});
}

// ─── Pattern matching ─────────────────────────────────────────────────────────

ir::BlockId CodeGenerator::genPattern(const ast::Pattern& pat, ir::RegId scrutinee,
                                       ir::IRType scrutineeTy, ir::BlockId failBlock) {
    if (dynamic_cast<const ast::WildcardPat*>(&pat)) {
        // Unconditional match — always falls through to the arm body
        return failBlock;
    }

    if (auto* lp = dynamic_cast<const ast::LiteralPat*>(&pat)) {
        // Emit the literal and compare
        ast::LiteralExpr tmp;
        tmp.loc = lp->loc;
        tmp.tok = lp->tok;
        ir::RegId lit = genLiteral(tmp);

        ir::IRType litTy = scrutineeTy;
        switch (lp->tok.kind) {
        case TokenKind::Integer: litTy = {types::TY_I64};  break;
        case TokenKind::Float:   litTy = {types::TY_F64};  break;
        case TokenKind::String:  litTy = {types::TY_STR};  break;
        case TokenKind::Kw_true:
        case TokenKind::Kw_false: litTy = {types::TY_BOOL}; break;
        default: break;
        }

        ir::RegId scr = scrutinee;
        if (litTy.typeId != scrutineeTy.typeId && litTy.typeId != types::TY_UNKNOWN)
            scr = coerce(scrutinee, scrutineeTy, litTy);

        ir::RegId eq       = builder_->emitBinop(ir::Opcode::Eq, scr, lit, {types::TY_BOOL});
        ir::BlockId matchB = builder_->createBlock("pat.match");
        builder_->emitBranch(eq, matchB, failBlock);
        builder_->setBlock(matchB);
        return failBlock;
    }

    if (auto* rp = dynamic_cast<const ast::RangePat*>(&pat)) {
        // lo <= scrutinee && scrutinee <= hi (or < hi if exclusive)
        ir::RegId lo = genPattern(*rp->lo, scrutinee, scrutineeTy, failBlock);
        (void)lo;
        ir::RegId hi = genPattern(*rp->hi, scrutinee, scrutineeTy, failBlock);
        (void)hi;
        // Ranges are complex — fall through
        return failBlock;
    }

    if (auto* op = dynamic_cast<const ast::OrPat*>(&pat)) {
        // Try each alternative; succeed if any matches
        ir::BlockId successBlock = builder_->createBlock("or.pat.success");
        for (auto& alt : op->alts) {
            ir::BlockId nextAlt = builder_->createBlock("or.pat.next");
            genPattern(*alt, scrutinee, scrutineeTy, nextAlt);
            builder_->emitJump(successBlock);
            builder_->setBlock(nextAlt);
        }
        // All alts failed
        builder_->emitJump(failBlock);
        builder_->setBlock(successBlock);
        return failBlock;
    }

    if (auto* ep = dynamic_cast<const ast::EnumPat*>(&pat)) {
        // Check discriminant tag: load tag word and compare to variant index
        // Find variant index by name scan
        ir::RegId tagPtr = builder_->emitGEP(scrutinee, {builder_->emitConstInt(0, {types::TY_I64})}, {types::TY_I64});
        ir::RegId tag    = builder_->emitLoad(tagPtr, {types::TY_I64});

        // Locate the enum type and variant
        int64_t variantDiscrim = 0;
        bool variantFound = false;
        for (types::TypeId tid = types::TY_FIRST_USER; ; ++tid) {
            const types::Type* ty = typeReg_.get(tid);
            if (!ty) break;
            if (ty->kind != types::TypeKind::Enum) continue;
            for (size_t vi = 0; vi < ty->fields.size(); ++vi) {
                // fields[vi].name holds the variant name; offset holds discriminant
                std::string fqn = ty->name + "::" + ty->fields[vi].name;
                if (fqn == ep->path || ty->fields[vi].name == ep->path) {
                    variantDiscrim = (int64_t)ty->fields[vi].offset;
                    variantFound   = true;
                    break;
                }
            }
            if (variantFound) break;
        }

        ir::RegId expected = builder_->emitConstInt(variantDiscrim, {types::TY_I64});
        ir::RegId matches  = builder_->emitBinop(ir::Opcode::Eq, tag, expected, {types::TY_BOOL});
        ir::BlockId matchB = builder_->createBlock("enum.pat.match");
        builder_->emitBranch(matches, matchB, failBlock);
        builder_->setBlock(matchB);
        return failBlock;
    }

    // NamePat and StructPat: unconditional match (bindings handled in genPatternBindings)
    return failBlock;
}

void CodeGenerator::genPatternBindings(const ast::Pattern& pat, ir::RegId scrutinee,
                                        ir::IRType scrutineeTy) {
    if (auto* np = dynamic_cast<const ast::NamePat*>(&pat)) {
        ir::RegId ptr = builder_->emitAlloc(scrutineeTy);
        builder_->emitStore(scrutinee, ptr);
        ValueEnv::Entry ent;
        ent.reg   = ptr;
        ent.isPtr = true;
        ent.type  = scrutineeTy;
        ent.isMut = np->isMut;
        env_.define(np->name, ent);
    } else if (auto* tp = dynamic_cast<const ast::TuplePat*>(&pat)) {
        for (size_t i = 0; i < tp->pats.size(); ++i) {
            ir::RegId idx     = builder_->emitConstInt((int64_t)i, {types::TY_I64});
            ir::RegId elemPtr = builder_->emitGEP(scrutinee, {idx}, scrutineeTy);
            ir::RegId elem    = builder_->emitLoad(elemPtr, scrutineeTy);
            genPatternBindings(*tp->pats[i], elem, scrutineeTy);
        }
    } else if (auto* sp = dynamic_cast<const ast::StructPat*>(&pat)) {
        for (auto& [fname, fpat] : sp->fields) {
            uint32_t idx = 0;
            ir::IRType fty = {types::TY_I64};
            for (types::TypeId tid = types::TY_FIRST_USER; ; ++tid) {
                const types::Type* sty = typeReg_.get(tid);
                if (!sty) break;
                for (uint32_t fi = 0; fi < (uint32_t)sty->fields.size(); ++fi) {
                    if (sty->fields[fi].name == fname) {
                        idx = fi;
                        fty = toIRType(sty->fields[fi].type);
                        goto foundField;
                    }
                }
            }
            foundField:;
            ir::RegId fptr = builder_->emitFieldPtr(scrutinee, idx, fty);
            ir::RegId fval = builder_->emitLoad(fptr, fty);
            genPatternBindings(*fpat, fval, fty);
        }
    } else if (auto* ep = dynamic_cast<const ast::EnumPat*>(&pat)) {
        // Bind inner tuple/struct fields of an enum variant (payload starts at offset 1)
        for (size_t i = 0; i < ep->inner.size(); ++i) {
            ir::RegId idx     = builder_->emitConstInt((int64_t)(i + 1), {types::TY_I64});
            ir::RegId elemPtr = builder_->emitGEP(scrutinee, {idx}, {types::TY_I64});
            ir::RegId elem    = builder_->emitLoad(elemPtr, {types::TY_I64});
            genPatternBindings(*ep->inner[i], elem, {types::TY_I64});
        }
    }
    // WildcardPat, LiteralPat, RangePat: no bindings
}

// ═══════════════════════════════════════════════════════════════════════════════
// C++ Transpile Backend
// ═══════════════════════════════════════════════════════════════════════════════

bool CppTranspileBackend::emit(const ir::IRModule& mod, const std::string& outPath, OutputFormat) {
    std::string code = transpile(mod);
    std::ofstream f(outPath, std::ios::out);
    if (!f.is_open()) return false;
    f.write(code.data(), (std::streamsize)code.size());
    return f.good();
}

std::string CppTranspileBackend::irTypeToCpp(const ir::IRType& ty) {
    std::string base;
    switch (ty.typeId) {
    case types::TY_BOOL:   base = "bool";        break;
    case types::TY_I8:     base = "int8_t";      break;
    case types::TY_I16:    base = "int16_t";     break;
    case types::TY_I32:    base = "int32_t";     break;
    case types::TY_I64:    base = "int64_t";     break;
    case types::TY_I128:   base = "__int128";    break;
    case types::TY_ISIZE:  base = "intptr_t";    break;
    case types::TY_U8:     base = "uint8_t";     break;
    case types::TY_U16:    base = "uint16_t";    break;
    case types::TY_U32:    base = "uint32_t";    break;
    case types::TY_U64:    base = "uint64_t";    break;
    case types::TY_U128:   base = "unsigned __int128"; break;
    case types::TY_USIZE:  base = "uintptr_t";  break;
    case types::TY_F32:    base = "float";       break;
    case types::TY_F64:    base = "double";      break;
    case types::TY_CHAR:   base = "char32_t";    break;
    case types::TY_STR:    base = "std::string"; break;
    case types::TY_UNIT:   base = "void";        break;
    case types::TY_NEVER:  base = "[[noreturn]] void"; break;
    case types::TY_RAWPTR: base = "void*";       break;
    default:               base = "fpp_value_t"; break;
    }
    if (ty.isPtr && ty.typeId != types::TY_RAWPTR) {
        for (uint32_t d = 0; d < ty.ptrDepth; ++d) base += "*";
    }
    return base;
}

std::string CppTranspileBackend::opcodeToOp(ir::Opcode op) {
    switch (op) {
    case ir::Opcode::Add:    return "+";
    case ir::Opcode::Sub:    return "-";
    case ir::Opcode::Mul:    return "*";
    case ir::Opcode::Div:    return "/";
    case ir::Opcode::Mod:    return "%";
    case ir::Opcode::BitAnd: return "&";
    case ir::Opcode::BitOr:  return "|";
    case ir::Opcode::BitXor: return "^";
    case ir::Opcode::Shl:    return "<<";
    case ir::Opcode::Shr:    return ">>";
    case ir::Opcode::Ushr:   return ">>"; // unsigned: handled via cast in emitInst
    case ir::Opcode::Eq:     return "==";
    case ir::Opcode::Ne:     return "!=";
    case ir::Opcode::Lt:     return "<";
    case ir::Opcode::Le:     return "<=";
    case ir::Opcode::Gt:     return ">";
    case ir::Opcode::Ge:     return ">=";
    case ir::Opcode::And:    return "&&";
    case ir::Opcode::Or:     return "||";
    default: return "?";
    }
}

std::string CppTranspileBackend::transpile(const ir::IRModule& mod) {
    std::ostringstream ss;

    // File header
    ss << "// F++ → C++ transpiled output (generated by fpp-to-cpp backend)\n";
    ss << "// Module: " << mod.name << "\n";
    ss << "// Target: " << mod.targetTriple << "\n\n";

    // Standard includes
    ss << "#include <cstdint>\n";
    ss << "#include <string>\n";
    ss << "#include <vector>\n";
    ss << "#include <unordered_map>\n";
    ss << "#include <optional>\n";
    ss << "#include <variant>\n";
    ss << "#include <functional>\n";
    ss << "#include <iostream>\n";
    ss << "#include <stdexcept>\n";
    ss << "#include <cstring>\n\n";

    // Runtime type alias
    ss << "using fpp_value_t = int64_t;\n\n";

    // Forward-declare all functions so mutual recursion works
    ss << "// ── Forward declarations ──────────────────────────────────────────────────\n";
    for (auto& fn : mod.functions) {
        if (fn.isExtern) continue;
        ss << irTypeToCpp(fn.retType) << " " << fn.name << "(";
        for (size_t i = 0; i < fn.params.size(); ++i) {
            ss << irTypeToCpp(fn.params[i].type) << " " << fn.params[i].name;
            if (i + 1 < fn.params.size()) ss << ", ";
        }
        ss << ");\n";
    }
    ss << "\n";

    // Global variables
    if (!mod.globals.empty()) {
        ss << "// ── Globals ──────────────────────────────────────────────────────────────\n";
        for (auto& g : mod.globals) {
            ss << (g.isConst ? "const " : "");
            ss << irTypeToCpp(g.type) << " " << g.name;
            if (!g.initializer.empty()) {
                if (g.type.typeId == types::TY_STR) {
                    ss << " = \"" << std::string(g.initializer.begin(), g.initializer.end()) << "\"";
                } else if (g.type.typeId == types::TY_BOOL) {
                    ss << " = " << (g.initializer[0] ? "true" : "false");
                } else {
                    int64_t v = 0;
                    if (g.initializer.size() >= 8) std::memcpy(&v, g.initializer.data(), 8);
                    ss << " = " << v;
                }
            }
            ss << ";\n";
        }
        ss << "\n";
    }

    // Function definitions
    ss << "// ── Functions ────────────────────────────────────────────────────────────\n";
    for (auto& fn : mod.functions) ss << emitFn(fn, mod) << "\n";

    return ss.str();
}

std::string CppTranspileBackend::emitFn(const ir::IRFunction& fn, const ir::IRModule& mod) {
    if (fn.isExtern) return "// extern: " + fn.name + "\n";
    if (fn.blocks.empty()) return "// empty: " + fn.name + "\n";

    std::ostringstream ss;

    // Attributes
    if (fn.isInline) ss << "inline ";

    ss << irTypeToCpp(fn.retType) << " " << fn.name << "(";
    for (size_t i = 0; i < fn.params.size(); ++i) {
        ss << irTypeToCpp(fn.params[i].type) << " " << fn.params[i].name;
        if (i + 1 < fn.params.size()) ss << ", ";
    }
    ss << ") {\n";

    // Declare all non-void destination registers at the top of the function
    // so goto + labels work correctly in C++
    {
        std::set<ir::RegId> declared;
        for (auto& blk : fn.blocks) {
            for (auto& inst : blk.insts) {
                if (inst.dest == ir::REG_NONE) continue;
                if (inst.op == ir::Opcode::Return || inst.op == ir::Opcode::Jump ||
                    inst.op == ir::Opcode::Branch) continue;
                if (declared.count(inst.dest)) continue;
                declared.insert(inst.dest);
                std::string ctype = irTypeToCpp(inst.type);
                if (ctype != "void" && ctype != "[[noreturn]] void") {
                    ss << "  " << ctype << " r" << inst.dest;
                    // Default-initialise to avoid UB
                    if (inst.type.isPtr)                              ss << " = nullptr";
                    else if (inst.type.typeId == types::TY_BOOL)      ss << " = false";
                    else if (inst.type.typeId == types::TY_STR)       ss << " = {}";
                    else if (inst.type.typeId == types::TY_F32 ||
                             inst.type.typeId == types::TY_F64)       ss << " = 0.0";
                    else                                               ss << " = 0";
                    ss << ";\n";
                }
            }
        }
        if (!declared.empty()) ss << "\n";
    }

    // Emit basic blocks
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        auto& blk = fn.blocks[bi];
        ss << "  " << blk.label << ":;\n";
        for (auto& inst : blk.insts) {
            std::string line = emitInst(inst, fn);
            if (!line.empty()) ss << "  " << line << "\n";
        }
    }
    (void)mod;

    ss << "}\n";
    return ss.str();
}

std::string CppTranspileBackend::emitInst(const ir::Instruction& inst,
                                           const ir::IRFunction& fn) {
    std::ostringstream ss;
    auto r = [](ir::RegId id) -> std::string {
        return id == ir::REG_NONE ? "" : "r" + std::to_string(id);
    };
    auto op0 = [&]() { return r(inst.operands.empty() ? ir::REG_NONE : inst.operands[0]); };
    auto op1 = [&]() { return r(inst.operands.size() < 2 ? ir::REG_NONE : inst.operands[1]); };

    switch (inst.op) {
    // ── Constants ────────────────────────────────────────────────────────────
    case ir::Opcode::ConstInt:
        ss << r(inst.dest) << " = " << inst.immInt << ";";
        break;
    case ir::Opcode::ConstFp:
        ss << r(inst.dest) << " = " << inst.immFp << ";";
        break;
    case ir::Opcode::ConstStr:
        ss << r(inst.dest) << " = std::string(\"" << inst.immStr << "\");";
        break;
    case ir::Opcode::ConstBool:
        ss << r(inst.dest) << " = " << (inst.immInt ? "true" : "false") << ";";
        break;
    case ir::Opcode::ConstNull:
        ss << r(inst.dest) << " = nullptr;";
        break;
    case ir::Opcode::ConstUnit:
        // unit — no assignment
        break;

    // ── Arithmetic / logic ────────────────────────────────────────────────────
    case ir::Opcode::Add: case ir::Opcode::Sub: case ir::Opcode::Mul:
    case ir::Opcode::Div: case ir::Opcode::Mod:
    case ir::Opcode::BitAnd: case ir::Opcode::BitOr: case ir::Opcode::BitXor:
    case ir::Opcode::Shl: case ir::Opcode::Shr:
    case ir::Opcode::Eq: case ir::Opcode::Ne:
    case ir::Opcode::Lt: case ir::Opcode::Le: case ir::Opcode::Gt: case ir::Opcode::Ge:
    case ir::Opcode::And: case ir::Opcode::Or:
        ss << r(inst.dest) << " = " << op0() << " " << opcodeToOp(inst.op) << " " << op1() << ";";
        break;
    case ir::Opcode::Ushr:
        // Unsigned right shift: cast to unsigned
        ss << r(inst.dest) << " = (int64_t)((uint64_t)" << op0() << " >> " << op1() << ");";
        break;
    case ir::Opcode::Neg:
        ss << r(inst.dest) << " = -" << op0() << ";";
        break;
    case ir::Opcode::Not:
        ss << r(inst.dest) << " = !" << op0() << ";";
        break;
    case ir::Opcode::BitNot:
        ss << r(inst.dest) << " = ~" << op0() << ";";
        break;
    case ir::Opcode::Pow:
        ss << r(inst.dest) << " = (int64_t)std::pow((double)" << op0() << ", (double)" << op1() << ");";
        break;

    // ── Memory ───────────────────────────────────────────────────────────────
    case ir::Opcode::Alloc:
        ss << r(inst.dest) << " = (" << irTypeToCpp(inst.type) << ")fpp_alloc(sizeof(" << irTypeToCpp(inst.type) << "));";
        break;
    case ir::Opcode::Free:
        ss << "fpp_free(" << op0() << ");";
        break;
    case ir::Opcode::Load:
        ss << r(inst.dest) << " = *(" << irTypeToCpp(inst.type) << "*)(" << op0() << ");";
        break;
    case ir::Opcode::Store:
        ss << "*(" << irTypeToCpp(inst.type) << "*)(" << op1() << ") = " << op0() << ";";
        break;
    case ir::Opcode::GEP:
        ss << r(inst.dest) << " = (" << irTypeToCpp(inst.type) << "*)((" << irTypeToCpp(inst.type) << "*)(" << op0() << ") + " << op1() << ");";
        break;
    case ir::Opcode::FieldPtr:
        ss << r(inst.dest) << " = (" << irTypeToCpp(inst.type) << "*)((char*)(" << op0() << ") + " << inst.fieldIdx << " * sizeof(fpp_value_t));";
        break;

    // ── Type conversions ─────────────────────────────────────────────────────
    case ir::Opcode::Trunc:
    case ir::Opcode::Ext:
    case ir::Opcode::Bitcast:
        ss << r(inst.dest) << " = (" << irTypeToCpp(inst.type) << ")(" << op0() << ");";
        break;
    case ir::Opcode::FpToInt:
        ss << r(inst.dest) << " = (int64_t)(" << op0() << ");";
        break;
    case ir::Opcode::IntToFp:
        ss << r(inst.dest) << " = (double)(" << op0() << ");";
        break;
    case ir::Opcode::PtrToInt:
        ss << r(inst.dest) << " = (intptr_t)(" << op0() << ");";
        break;
    case ir::Opcode::IntToPtr:
        ss << r(inst.dest) << " = (void*)(" << op0() << ");";
        break;

    // ── Control flow ─────────────────────────────────────────────────────────
    case ir::Opcode::Return:
        if (!inst.operands.empty() && fn.retType.typeId != types::TY_UNIT)
            ss << "return " << op0() << ";";
        else
            ss << "return;";
        break;
    case ir::Opcode::Jump:
        ss << "goto bb" << inst.target << ";";
        break;
    case ir::Opcode::Branch:
        ss << "if (" << op0() << ") goto bb" << inst.target << "; else goto bb" << inst.altTarget << ";";
        break;

    // ── Function calls ────────────────────────────────────────────────────────
    case ir::Opcode::Call: {
        // Find the callee function by ID
        std::string calleeName;
        for (auto& f : fn.params) (void)f; // suppress warning
        // Search the module for the function with this ID
        // We can't access the module from here directly, so use callee id as-is
        // and rely on the forward declarations at the top of the file.
        if (inst.dest != ir::REG_NONE)
            ss << r(inst.dest) << " = ";
        ss << "fn_" << inst.callee << "(";
        for (size_t i = 0; i < inst.operands.size(); ++i) {
            if (i) ss << ", ";
            ss << r(inst.operands[i]);
        }
        ss << ");";
        break;
    }
    case ir::Opcode::IndirectCall: {
        // Call through a function pointer stored in operands[0]
        std::string ptrArg = inst.operands.empty() ? "nullptr" : r(inst.operands[0]);
        if (inst.dest != ir::REG_NONE)
            ss << r(inst.dest) << " = ";
        ss << "((fpp_value_t(*)(...))(" << ptrArg << "))(";
        for (size_t i = 1; i < inst.operands.size(); ++i) {
            if (i > 1) ss << ", ";
            ss << r(inst.operands[i]);
        }
        ss << ");";
        break;
    }

    // ── Intrinsics ────────────────────────────────────────────────────────────
    case ir::Opcode::Intrinsic: {
        if (inst.dest != ir::REG_NONE && irTypeToCpp(inst.type) != "void")
            ss << r(inst.dest) << " = ";
        ss << inst.intrinsicName << "(";
        for (size_t i = 0; i < inst.operands.size(); ++i) {
            if (i) ss << ", ";
            ss << r(inst.operands[i]);
        }
        ss << ");";
        break;
    }

    // ── Phi node (SSA — translated to pre-declared variable) ─────────────────
    case ir::Opcode::Phi: {
        // Phi nodes are resolved via block-level assignments;
        // the dest register was already declared and assigned in predecessor blocks.
        // Emit a no-op comment so the block label jump lands correctly.
        ss << "/* phi: " << r(inst.dest) << " */";
        break;
    }

    // ── Select ────────────────────────────────────────────────────────────────
    case ir::Opcode::Select: {
        std::string cond = r(inst.operands.size() > 0 ? inst.operands[0] : ir::REG_NONE);
        std::string a    = r(inst.operands.size() > 1 ? inst.operands[1] : ir::REG_NONE);
        std::string b    = r(inst.operands.size() > 2 ? inst.operands[2] : ir::REG_NONE);
        ss << r(inst.dest) << " = (" << cond << ") ? " << a << " : " << b << ";";
        break;
    }

    // ── Global reference ──────────────────────────────────────────────────────
    case ir::Opcode::GlobalRef:
        ss << r(inst.dest) << " = &" << inst.globalName << ";";
        break;

    // ── Atomic operations ─────────────────────────────────────────────────────
    case ir::Opcode::AtomicLoad:
        ss << r(inst.dest) << " = __atomic_load_n((" << irTypeToCpp(inst.type) << "*)(" << op0() << "), " << inst.atomicOrder << ");";
        break;
    case ir::Opcode::AtomicStore:
        ss << "__atomic_store_n((" << irTypeToCpp(inst.type) << "*)(" << op1() << "), " << op0() << ", " << inst.atomicOrder << ");";
        break;
    case ir::Opcode::AtomicRMW:
        ss << r(inst.dest) << " = __atomic_fetch_add((" << irTypeToCpp(inst.type) << "*)(" << op0() << "), " << op1() << ", " << inst.atomicOrder << ");";
        break;
    case ir::Opcode::Fence:
        ss << "__atomic_thread_fence(" << inst.atomicOrder << ");";
        break;

    // ── Concurrency ───────────────────────────────────────────────────────────
    case ir::Opcode::SpawnTask:
        ss << r(inst.dest) << " = fpp_spawn(" << op0() << ");";
        break;
    case ir::Opcode::Await:
        ss << r(inst.dest) << " = fpp_await(" << op0() << ");";
        break;
    case ir::Opcode::ChanSend:
        ss << "fpp_chan_send(" << op0() << ", " << op1() << ");";
        break;
    case ir::Opcode::ChanRecv:
        ss << r(inst.dest) << " = fpp_chan_recv(" << op0() << ");";
        break;

    // ── Closures ──────────────────────────────────────────────────────────────
    case ir::Opcode::MakeClosure:
        if (inst.dest != ir::REG_NONE)
            ss << r(inst.dest) << " = fpp_make_closure(" << op0() << ");";
        break;
    case ir::Opcode::ClosureCall:
        if (inst.dest != ir::REG_NONE)
            ss << r(inst.dest) << " = ";
        ss << "fpp_closure_call(" << op0() << ");";
        break;

    // ── Exception handling ────────────────────────────────────────────────────
    case ir::Opcode::Throw:
        ss << "throw std::runtime_error(std::string((char*)" << op0() << "));";
        break;
    case ir::Opcode::Unreachable:
        ss << "__builtin_unreachable();";
        break;

    // ── Debug ────────────────────────────────────────────────────────────────
    case ir::Opcode::DebugValue:
    case ir::Opcode::DebugLoc:
        // Stripped in release; emit as empty line
        break;

    default:
        ss << "/* unhandled op " << static_cast<int>(inst.op) << " */";
        break;
    }

    return ss.str();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Bytecode Backend
// ═══════════════════════════════════════════════════════════════════════════════

// Bytecode instruction set used by the FPP treewalk VM.
// Each instruction is a 64-bit word: [opcode:8][dest:20][src0:18][src1:18]
// Immediates follow as separate 64-bit words tagged with 0xFF in the opcode byte.

enum class BytecodeOp : uint8_t {
    Nop         = 0x00,
    LoadConst   = 0x01,  // dest = constants[imm]
    LoadNull    = 0x02,
    LoadBool    = 0x03,  // dest = imm (0/1)
    Move        = 0x04,  // dest = src0
    Add         = 0x10,
    Sub         = 0x11,
    Mul         = 0x12,
    Div         = 0x13,
    Mod         = 0x14,
    Neg         = 0x15,
    Eq          = 0x20,
    Ne          = 0x21,
    Lt          = 0x22,
    Le          = 0x23,
    Gt          = 0x24,
    Ge          = 0x25,
    And         = 0x26,
    Or          = 0x27,
    Not         = 0x28,
    BitAnd      = 0x30,
    BitOr       = 0x31,
    BitXor      = 0x32,
    BitNot      = 0x33,
    Shl         = 0x34,
    Shr         = 0x35,
    Alloc       = 0x40,
    Free        = 0x41,
    Load        = 0x42,
    Store       = 0x43,
    GEP         = 0x44,
    FieldPtr    = 0x45,
    Call        = 0x50,
    IndCall     = 0x51,
    Ret         = 0x52,
    Jump        = 0x60,
    Branch      = 0x61,
    Intrinsic   = 0x70,
    Cast        = 0x80,
    // Immediate word follows
    Imm64       = 0xFF,
};

// Encode one 8-byte instruction word
static uint64_t encodeInst(BytecodeOp op, uint32_t dest, uint32_t src0, uint32_t src1) {
    return ((uint64_t)(uint8_t)op << 56)
         | ((uint64_t)(dest  & 0xFFFFF) << 36)
         | ((uint64_t)(src0  & 0x3FFFF) << 18)
         | ((uint64_t)(src1  & 0x3FFFF));
}

static void appendU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out.push_back((uint8_t)(v >> (i * 8)));
}

static void appendStr(std::vector<uint8_t>& out, const std::string& s) {
    uint32_t len = (uint32_t)s.size();
    appendU64(out, len);
    out.insert(out.end(), s.begin(), s.end());
    // Align to 8 bytes
    while (out.size() % 8) out.push_back(0);
}

bool BytecodeBackend::emit(const ir::IRModule& mod, const std::string& outPath, OutputFormat) {
    auto bytes = compile(mod);
    std::ofstream f(outPath, std::ios::out | std::ios::binary);
    if (!f.is_open()) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    return f.good();
}

std::vector<uint8_t> BytecodeBackend::compile(const ir::IRModule& mod) {
    std::vector<uint8_t> out;

    // Magic header: "FPP\x00" + version u32 (1)
    out.insert(out.end(), {'F', 'P', 'P', '\x00'});
    appendU64(out, 1); // version

    // Module name
    appendStr(out, mod.name);

    // Number of functions
    appendU64(out, (uint64_t)mod.functions.size());

    for (auto& fn : mod.functions) {
        // Function header: name + param count + is_async
        appendStr(out, fn.name);
        appendU64(out, (uint64_t)fn.params.size());
        appendU64(out, fn.isAsync ? 1ULL : 0ULL);

        // Params (name + typeId pairs)
        for (auto& p : fn.params) {
            appendStr(out, p.name);
            appendU64(out, (uint64_t)p.type.typeId);
        }

        // Return type
        appendU64(out, (uint64_t)fn.retType.typeId);

        // Collect string constants used in this function
        std::vector<std::string> constPool;
        std::unordered_map<std::string, uint32_t> constIdx;

        auto internStr = [&](const std::string& s) -> uint32_t {
            auto it = constIdx.find(s);
            if (it != constIdx.end()) return it->second;
            uint32_t idx = (uint32_t)constPool.size();
            constPool.push_back(s);
            constIdx[s] = idx;
            return idx;
        };

        // Pre-scan to build constant pool
        for (auto& blk : fn.blocks)
            for (auto& inst : blk.insts)
                if (inst.op == ir::Opcode::ConstStr || inst.op == ir::Opcode::Intrinsic)
                    internStr(inst.immStr.empty() ? inst.intrinsicName : inst.immStr);

        // Emit constant pool
        appendU64(out, (uint64_t)constPool.size());
        for (auto& s : constPool) appendStr(out, s);

        // Count total instructions across all blocks
        size_t totalInsts = 0;
        for (auto& blk : fn.blocks) totalInsts += blk.insts.size() + 1; // +1 for block label
        appendU64(out, (uint64_t)fn.blocks.size());
        appendU64(out, (uint64_t)totalInsts);

        // Emit blocks
        for (auto& blk : fn.blocks) {
            appendStr(out, blk.label);
            appendU64(out, (uint64_t)blk.insts.size());

            for (auto& inst : blk.insts) {
                uint32_t dest = inst.dest == ir::REG_NONE ? 0xFFFFF : (uint32_t)inst.dest;
                uint32_t s0   = inst.operands.size() > 0 ? (uint32_t)inst.operands[0] : 0;
                uint32_t s1   = inst.operands.size() > 1 ? (uint32_t)inst.operands[1] : 0;

                // Map IR opcodes to bytecode opcodes
                BytecodeOp bop = BytecodeOp::Nop;
                bool hasImm64  = false;
                uint64_t imm64 = 0;
                bool hasStrImm = false;
                uint32_t strPoolIdx = 0;

                switch (inst.op) {
                case ir::Opcode::ConstInt:
                    bop = BytecodeOp::LoadConst; hasImm64 = true; imm64 = (uint64_t)inst.immInt; break;
                case ir::Opcode::ConstFp: {
                    bop = BytecodeOp::LoadConst; hasImm64 = true;
                    std::memcpy(&imm64, &inst.immFp, 8); break;
                }
                case ir::Opcode::ConstStr:
                    bop = BytecodeOp::LoadConst; hasStrImm = true;
                    strPoolIdx = internStr(inst.immStr); break;
                case ir::Opcode::ConstBool:
                    bop = BytecodeOp::LoadBool; hasImm64 = true; imm64 = inst.immInt ? 1 : 0; break;
                case ir::Opcode::ConstNull:
                    bop = BytecodeOp::LoadNull; break;
                case ir::Opcode::Add:    bop = BytecodeOp::Add;    break;
                case ir::Opcode::Sub:    bop = BytecodeOp::Sub;    break;
                case ir::Opcode::Mul:    bop = BytecodeOp::Mul;    break;
                case ir::Opcode::Div:    bop = BytecodeOp::Div;    break;
                case ir::Opcode::Mod:    bop = BytecodeOp::Mod;    break;
                case ir::Opcode::Neg:    bop = BytecodeOp::Neg;    break;
                case ir::Opcode::Eq:     bop = BytecodeOp::Eq;     break;
                case ir::Opcode::Ne:     bop = BytecodeOp::Ne;     break;
                case ir::Opcode::Lt:     bop = BytecodeOp::Lt;     break;
                case ir::Opcode::Le:     bop = BytecodeOp::Le;     break;
                case ir::Opcode::Gt:     bop = BytecodeOp::Gt;     break;
                case ir::Opcode::Ge:     bop = BytecodeOp::Ge;     break;
                case ir::Opcode::And:    bop = BytecodeOp::And;    break;
                case ir::Opcode::Or:     bop = BytecodeOp::Or;     break;
                case ir::Opcode::Not:    bop = BytecodeOp::Not;    break;
                case ir::Opcode::BitAnd: bop = BytecodeOp::BitAnd; break;
                case ir::Opcode::BitOr:  bop = BytecodeOp::BitOr;  break;
                case ir::Opcode::BitXor: bop = BytecodeOp::BitXor; break;
                case ir::Opcode::BitNot: bop = BytecodeOp::BitNot; break;
                case ir::Opcode::Shl:    bop = BytecodeOp::Shl;    break;
                case ir::Opcode::Shr:    bop = BytecodeOp::Shr;    break;
                case ir::Opcode::Ushr:   bop = BytecodeOp::Shr;    break;
                case ir::Opcode::Alloc:  bop = BytecodeOp::Alloc;  break;
                case ir::Opcode::Free:   bop = BytecodeOp::Free;   break;
                case ir::Opcode::Load:   bop = BytecodeOp::Load;   break;
                case ir::Opcode::Store:  bop = BytecodeOp::Store;  break;
                case ir::Opcode::GEP:    bop = BytecodeOp::GEP;    break;
                case ir::Opcode::FieldPtr: bop = BytecodeOp::FieldPtr; hasImm64 = true; imm64 = inst.fieldIdx; break;
                case ir::Opcode::Call:
                    bop = BytecodeOp::Call; hasImm64 = true; imm64 = inst.callee; break;
                case ir::Opcode::IndirectCall:
                    bop = BytecodeOp::IndCall; break;
                case ir::Opcode::Return:
                    bop = BytecodeOp::Ret; break;
                case ir::Opcode::Jump:
                    bop = BytecodeOp::Jump; hasImm64 = true; imm64 = inst.target; break;
                case ir::Opcode::Branch:
                    bop = BytecodeOp::Branch; hasImm64 = true; imm64 = ((uint64_t)inst.target << 32) | inst.altTarget; break;
                case ir::Opcode::Intrinsic:
                    bop = BytecodeOp::Intrinsic; hasStrImm = true;
                    strPoolIdx = internStr(inst.intrinsicName); break;
                case ir::Opcode::Trunc: case ir::Opcode::Ext:
                case ir::Opcode::FpToInt: case ir::Opcode::IntToFp:
                case ir::Opcode::Bitcast: case ir::Opcode::PtrToInt: case ir::Opcode::IntToPtr:
                    bop = BytecodeOp::Cast; hasImm64 = true; imm64 = (uint64_t)inst.op; break;
                default:
                    bop = BytecodeOp::Nop; break;
                }

                appendU64(out, encodeInst(bop, dest, s0, s1));

                if (hasImm64) {
                    appendU64(out, encodeInst(BytecodeOp::Imm64, 0, 0, 0));
                    appendU64(out, imm64);
                }
                if (hasStrImm) {
                    appendU64(out, encodeInst(BytecodeOp::Imm64, 0, 0, 0));
                    appendU64(out, (uint64_t)strPoolIdx);
                }

                // For Call/IndirectCall: emit argument count + operand list
                if (bop == BytecodeOp::Call || bop == BytecodeOp::IndCall) {
                    size_t argStart = (bop == BytecodeOp::IndCall) ? 1 : 0;
                    size_t argCount = inst.operands.size() > argStart ? inst.operands.size() - argStart : 0;
                    appendU64(out, (uint64_t)argCount);
                    for (size_t ai = argStart; ai < inst.operands.size(); ++ai)
                        appendU64(out, (uint64_t)inst.operands[ai]);
                }

                // Phi edges
                if (inst.op == ir::Opcode::Phi) {
                    appendU64(out, (uint64_t)inst.phiEdges.size());
                    for (auto& pe : inst.phiEdges) {
                        appendU64(out, (uint64_t)pe.val);
                        appendU64(out, (uint64_t)pe.from);
                    }
                }
            }
        }
    }

    // Global variable section
    appendU64(out, (uint64_t)mod.globals.size());
    for (auto& g : mod.globals) {
        appendStr(out, g.name);
        appendU64(out, (uint64_t)g.type.typeId);
        appendU64(out, g.isConst ? 1ULL : 0ULL);
        appendU64(out, g.isMut   ? 1ULL : 0ULL);
        appendU64(out, (uint64_t)g.initializer.size());
        out.insert(out.end(), g.initializer.begin(), g.initializer.end());
        // Align to 8 bytes
        while (out.size() % 8) out.push_back(0);
    }

    // Extern names
    appendU64(out, (uint64_t)mod.externs.size());
    for (auto& ext : mod.externs) appendStr(out, ext);

    // Footer sentinel
    out.insert(out.end(), {'E', 'N', 'D', '\x00'});

    return out;
}

} // namespace codegen
} // namespace fpp
