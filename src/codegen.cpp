#include "../include/codegen.hpp"
#include <fstream>
#include <sstream>
#include <cassert>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <functional>

namespace fpp {
namespace codegen {

// ─── ValueEnv ────────────────────────────────────────────────────────────────
bool ValueEnv::define(const std::string& name, Entry e) {
    if (scopes.empty()) scopes.push_back({});
    scopes.back()[name] = std::move(e);
    return true;
}

ValueEnv::Entry* ValueEnv::lookup(const std::string& name) {
    for (int i = (int)scopes.size()-1; i >= 0; --i) {
        auto it = scopes[i].find(name);
        if (it != scopes[i].end()) return &it->second;
    }
    return nullptr;
}

// ─── CodeGenerator ────────────────────────────────────────────────────────────
CodeGenerator::CodeGenerator(types::TypeRegistry& reg, types::SemanticAnalyser& sem)
    : typeReg_(reg), sem_(sem) {
    irMod_.name        = "fpp_module";
    irMod_.targetTriple = "x86_64-unknown-linux-gnu";
    irMod_.dataLayout   = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128";
    builder_ = std::make_unique<ir::IRBuilder>(irMod_);
}

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
    if      (name == "bool")  { t.typeId = types::TY_BOOL; }
    else if (name == "i8")    { t.typeId = types::TY_I8; }
    else if (name == "i16")   { t.typeId = types::TY_I16; }
    else if (name == "i32")   { t.typeId = types::TY_I32; }
    else if (name == "i64")   { t.typeId = types::TY_I64; }
    else if (name == "i128")  { t.typeId = types::TY_I128; }
    else if (name == "isize") { t.typeId = types::TY_ISIZE; }
    else if (name == "u8")    { t.typeId = types::TY_U8; }
    else if (name == "u16")   { t.typeId = types::TY_U16; }
    else if (name == "u32")   { t.typeId = types::TY_U32; }
    else if (name == "u64")   { t.typeId = types::TY_U64; }
    else if (name == "u128")  { t.typeId = types::TY_U128; }
    else if (name == "usize") { t.typeId = types::TY_USIZE; }
    else if (name == "f32")   { t.typeId = types::TY_F32; }
    else if (name == "f64")   { t.typeId = types::TY_F64; }
    else if (name == "char")  { t.typeId = types::TY_CHAR; }
    else if (name == "str")   { t.typeId = types::TY_STR; }
    else if (name == "unit" || name == "()")  { t.typeId = types::TY_UNIT; }
    else if (name == "never" || name == "!")  { t.typeId = types::TY_NEVER; }
    else {
        // Look up user-defined type via semantic analyser's global scope
        t.typeId = types::TY_UNKNOWN;
    }
    return t;
}

ir::IRType CodeGenerator::toIRType(const ast::TypeExpr& te) {
    if (auto* nt = dynamic_cast<const ast::NamedType*>(&te)) {
        ir::IRType t = toIRTypeByName(nt->name);
        if (t.typeId == types::TY_UNKNOWN) {
            // May be a generic instantiation like Vec<T>, Option<T>
            if (nt->name == "Vec" || nt->name == "Array") {
                t.typeId = types::TY_UNKNOWN; // represented as array at runtime
            } else if (nt->name == "Option") {
                t.typeId = types::TY_UNKNOWN;
            } else if (nt->name == "Result") {
                t.typeId = types::TY_UNKNOWN;
            }
        }
        return t;
    }
    if (auto* pt = dynamic_cast<const ast::PtrType*>(&te)) {
        ir::IRType inner = toIRType(*pt->inner);
        ir::IRType t = inner;
        t.isPtr = true; t.isMut = pt->isMut; t.ptrDepth = inner.ptrDepth + 1;
        return t;
    }
    if (auto* rt = dynamic_cast<const ast::RefType*>(&te)) {
        ir::IRType inner = toIRType(*rt->inner);
        ir::IRType t = inner;
        t.isPtr = true; t.isMut = rt->isMut; t.ptrDepth = inner.ptrDepth + 1;
        return t;
    }
    if (auto* at = dynamic_cast<const ast::ArrayType*>(&te)) {
        ir::IRType elem = toIRType(*at->elem);
        return elem; // arrays decay to element pointer in IR
    }
    if (auto* sl = dynamic_cast<const ast::SliceType*>(&te)) {
        ir::IRType elem = toIRType(*sl->elem);
        ir::IRType t = elem; t.isPtr = true;
        return t;
    }
    if (auto* tt = dynamic_cast<const ast::TupleType*>(&te)) {
        (void)tt;
        return {types::TY_UNKNOWN}; // tuples are structs at runtime
    }
    if (auto* ft = dynamic_cast<const ast::FnType*>(&te)) {
        (void)ft;
        ir::IRType t; t.typeId = types::TY_RAWPTR; t.isPtr = true;
        return t; // function pointer
    }
    if (auto* nl = dynamic_cast<const ast::NullableType*>(&te)) {
        return toIRType(*nl->inner); // nullable is a tagged value
    }
    if (dynamic_cast<const ast::InferType*>(&te)) {
        return {types::TY_UNKNOWN};
    }
    return {types::TY_UNKNOWN};
}

std::string CodeGenerator::mangle(const std::string& name, const std::string& ns) {
    if (ns.empty()) return "_Zfpp_" + name;
    return "_Zfpp_" + ns + "_" + name;
}

void CodeGenerator::error(const std::string& msg, SourceLocation loc) {
    throw CodegenError(msg, loc);
}

ir::IRModule CodeGenerator::generate(const ast::Module& mod) {
    irMod_.name = mod.name;
    genGlobalFnDecls(mod);
    genModule(mod);
    return std::move(irMod_);
}

void CodeGenerator::genGlobalFnDecls(const ast::Module& mod) {
    for (auto& item : mod.items) {
        if (auto* fn = dynamic_cast<const ast::FnItem*>(item.get())) {
            // Pre-declare so forward calls work
            (void)fn;
        }
    }
}

void CodeGenerator::genModule(const ast::Module& mod) {
    env_.pushScope();
    for (auto& item : mod.items) genStmt(*item);
    env_.popScope();
}

void CodeGenerator::genStmt(const ast::Stmt& s) {
    if (auto* fn = dynamic_cast<const ast::FnItem*>(&s))     { genFn(*fn); return; }
    if (auto* st = dynamic_cast<const ast::StructItem*>(&s)) { genStruct(*st); return; }
    if (auto* en = dynamic_cast<const ast::EnumItem*>(&s))   { genEnum(*en); return; }
    if (auto* im = dynamic_cast<const ast::ImplItem*>(&s))   { genImpl(*im); return; }
    if (auto* cl = dynamic_cast<const ast::ClassItem*>(&s))  { genStmt(*cl); return; }
    if (auto* co = dynamic_cast<const ast::ConstItem*>(&s))  { genConst(*co); return; }
    if (auto* sa = dynamic_cast<const ast::StaticItem*>(&s)) { genStatic(*sa); return; }
    if (auto* ls = dynamic_cast<const ast::LetStmt*>(&s))    { genLet(*ls); return; }
    if (auto* es = dynamic_cast<const ast::ExprStmt*>(&s))   { genExpr(*es->expr); return; }
}

void CodeGenerator::genFn(const ast::FnItem& fn) {
    // Resolve return type from AST
    ir::IRType retTy;
    if (fn.retTy) {
        if (auto* nt = dynamic_cast<const ast::NamedType*>(fn.retTy.get())) {
            retTy = toIRTypeByName(nt->name);
        } else {
            retTy.typeId = types::TY_UNIT;
        }
    } else {
        retTy.typeId = types::TY_UNIT;
    }

    // Build parameter list with proper types
    std::vector<ir::IRParam> params;
    for (auto& [pname, pty] : fn.params) {
        ir::IRParam p;
        p.name = pname;
        if (auto* nt = dynamic_cast<const ast::NamedType*>(pty.get())) {
            p.type = toIRTypeByName(nt->name);
        } else if (dynamic_cast<const ast::RefType*>(pty.get()) ||
                   dynamic_cast<const ast::PtrType*>(pty.get())) {
            p.type = {types::TY_RAWPTR}; p.type.isPtr = true;
        } else if (auto* at = dynamic_cast<const ast::ArrayType*>(pty.get())) {
            p.type = {types::TY_UNKNOWN}; // resolved at use site
        } else {
            p.type = {types::TY_I64};
        }
        params.push_back(std::move(p));
    }

    builder_->beginFunction(fn.name, std::move(params), retTy);

    // Register params in env with their actual types
    env_.pushScope();
    ir::RegId pr = 1;
    for (auto& [pname, pty] : fn.params) {
        ir::IRType pty_ir;
        if (auto* nt = dynamic_cast<const ast::NamedType*>(pty.get()))
            pty_ir = toIRTypeByName(nt->name);
        else pty_ir = {types::TY_I64};
        ValueEnv::Entry e; e.reg = pr; e.isPtr = false; e.type = pty_ir; e.isMut = false;
        env_.define(pname, e);
        ++pr;
    }

    // Emit body
    if (fn.body) {
        for (auto& stmt : *fn.body) genStmt(*stmt);
    }

    // Ensure every block ends with a terminator
    auto& curFn = irMod_.functions.back();
    if (!curFn.blocks.empty()) {
        auto& blk = curFn.blocks[builder_->currentBlock()];
        if (blk.insts.empty() || (blk.insts.back().op != ir::Opcode::Return &&
                                   blk.insts.back().op != ir::Opcode::Jump &&
                                   blk.insts.back().op != ir::Opcode::Branch &&
                                   blk.insts.back().op != ir::Opcode::Unreachable)) {
            if (retTy.typeId == types::TY_UNIT || retTy.typeId == types::TY_NEVER) {
                builder_->emitReturn({});
            } else {
                auto zero = builder_->emitConstInt(0, retTy);
                builder_->emitReturn(zero);
            }
        }
    }

    env_.popScope();
    builder_->endFunction();
}

void CodeGenerator::genStruct(const ast::StructItem& s) { (void)s; }

void CodeGenerator::genEnum(const ast::EnumItem& e) { (void)e; }

void CodeGenerator::genImpl(const ast::ImplItem& impl) {
    (void)impl;
    env_.pushScope();
    for (auto& m : impl.members) genStmt(*m);
    env_.popScope();
}

void CodeGenerator::genClass(const ast::ClassItem& cls) {
    (void)cls;
    env_.pushScope();
    for (auto& m : cls.members) genStmt(*m);
    env_.popScope();
}

void CodeGenerator::genConst(const ast::ConstItem& c) {
    ir::IRGlobal g;
    g.name    = c.name;
    g.type    = c.ty ? toIRType(*c.ty) : ir::IRType{types::TY_I64};
    g.isConst = true;
    g.isMut   = false;
    g.linkage  = c.isPublic ? "external" : "internal";
    // Evaluate the constant expression at compile time if it's a literal
    if (auto* le = dynamic_cast<const ast::LiteralExpr*>(c.val.get())) {
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
            if (auto* sv = std::get_if<std::string>(&le->tok.literal)) {
                g.initializer.assign(sv->begin(), sv->end());
            }
            break;
        case TokenKind::Kw_true:  g.initializer = {1}; break;
        case TokenKind::Kw_false: g.initializer = {0}; break;
        default: break;
        }
    }
    irMod_.globals.push_back(std::move(g));
    // Also register as a global reference so code can access it
    ValueEnv::Entry e;
    e.reg    = ir::REG_NONE;
    e.isPtr  = false;
    e.type   = c.ty ? toIRType(*c.ty) : ir::IRType{types::TY_I64};
    e.isMut  = false;
    env_.define(c.name, e);
}

void CodeGenerator::genStatic(const ast::StaticItem& s) {
    ir::IRGlobal g;
    g.name    = s.name;
    g.type    = s.ty ? toIRType(*s.ty) : ir::IRType{types::TY_I64};
    g.isConst = !s.isMut;
    g.isMut   = s.isMut;
    g.linkage  = s.isPublic ? "external" : "internal";
    if (auto* le = dynamic_cast<const ast::LiteralExpr*>(s.val.get())) {
        switch (le->tok.kind) {
        case TokenKind::Integer:
            if (auto* iv = std::get_if<int64_t>(&le->tok.literal)) {
                g.initializer.resize(8); std::memcpy(g.initializer.data(), iv, 8);
            } break;
        case TokenKind::Float:
            if (auto* fv = std::get_if<double>(&le->tok.literal)) {
                g.initializer.resize(8); std::memcpy(g.initializer.data(), fv, 8);
            } break;
        case TokenKind::String:
            if (auto* sv = std::get_if<std::string>(&le->tok.literal))
                g.initializer.assign(sv->begin(), sv->end());
            break;
        case TokenKind::Kw_true:  g.initializer = {1}; break;
        case TokenKind::Kw_false: g.initializer = {0}; break;
        default: break;
        }
    }
    irMod_.globals.push_back(std::move(g));
    ValueEnv::Entry e;
    e.reg = ir::REG_NONE; e.isPtr = false;
    e.type = g.type;      e.isMut = s.isMut;
    env_.define(s.name, e);
}

void CodeGenerator::genLet(const ast::LetStmt& let) {
    // Resolve declared type if present, otherwise infer from initialiser expression
    ir::IRType ty;
    if (let.ty) {
        ty = toIRType(**let.ty);
    } else if (let.init) {
        // Perform a lightweight type probe: check the init expression kind
        if (auto* le = dynamic_cast<const ast::LiteralExpr*>(let.init->get())) {
            switch (le->tok.kind) {
            case TokenKind::Integer: ty = {types::TY_I64};  break;
            case TokenKind::Float:   ty = {types::TY_F64};  break;
            case TokenKind::String:  ty = {types::TY_STR};  break;
            case TokenKind::Kw_true:
            case TokenKind::Kw_false: ty = {types::TY_BOOL}; break;
            case TokenKind::Char:    ty = {types::TY_CHAR}; break;
            default:                 ty = {types::TY_UNKNOWN}; break;
            }
        } else if (auto* ae = dynamic_cast<const ast::ArrayExpr*>(let.init->get())) {
            ty = {types::TY_UNKNOWN}; // array — element type resolved at use
        } else {
            ty = {types::TY_UNKNOWN};
        }
    } else {
        ty = {types::TY_UNKNOWN};
    }
    if (ty.typeId == types::TY_UNKNOWN) ty.typeId = types::TY_I64; // safe default for alloc size

    ir::RegId ptr = builder_->emitAlloc(ty);

    if (let.init) {
        ir::RegId initReg = genExpr(**let.init);
        // Emit a type-narrowing cast if types differ
        if (ty.typeId != types::TY_UNKNOWN) {
            auto& regTypes = irMod_.functions.back().regTypes;
            auto it = regTypes.find(initReg);
            if (it != regTypes.end() && it->second.typeId != ty.typeId &&
                it->second.typeId != types::TY_UNKNOWN) {
                ir::Opcode castOp = ir::Opcode::Bitcast;
                if (typeReg_.isInteger(ty.typeId) && typeReg_.isFloat(it->second.typeId))
                    castOp = ir::Opcode::FpToInt;
                else if (typeReg_.isFloat(ty.typeId) && typeReg_.isInteger(it->second.typeId))
                    castOp = ir::Opcode::IntToFp;
                else if (typeReg_.isInteger(ty.typeId) && typeReg_.isInteger(it->second.typeId) &&
                         typeReg_.typeSize(ty.typeId) < typeReg_.typeSize(it->second.typeId))
                    castOp = ir::Opcode::Trunc;
                else if (typeReg_.isInteger(ty.typeId) && typeReg_.isInteger(it->second.typeId) &&
                         typeReg_.typeSize(ty.typeId) > typeReg_.typeSize(it->second.typeId))
                    castOp = ir::Opcode::Ext;
                if (castOp != ir::Opcode::Bitcast || it->second.typeId != ty.typeId)
                    initReg = builder_->emitCast(castOp, initReg, it->second, ty);
            }
        }
        builder_->emitStore(initReg, ptr);
    }

    // Bind all pattern names into the environment
    std::function<void(const ast::Pattern&, ir::RegId, ir::IRType)> bindPat =
        [&](const ast::Pattern& pat, ir::RegId valPtr, ir::IRType valTy) {
        if (auto* np = dynamic_cast<const ast::NamePat*>(&pat)) {
            ValueEnv::Entry e; e.reg = valPtr; e.isPtr = true; e.type = valTy; e.isMut = let.isMut || np->isMut;
            env_.define(np->name, e);
        } else if (auto* wp = dynamic_cast<const ast::WildcardPat*>(&pat)) {
            (void)wp; // _ : discard binding
        } else if (auto* tp = dynamic_cast<const ast::TuplePat*>(&pat)) {
            for (size_t i = 0; i < tp->pats.size(); ++i) {
                ir::RegId idx   = builder_->emitConstInt(i, {types::TY_I64});
                ir::RegId ePtr  = builder_->emitGEP(valPtr, {idx}, valTy);
                bindPat(*tp->pats[i], ePtr, valTy);
            }
        }
    };
    bindPat(*let.pat, ptr, ty);
}

// ─── Expression code generation ───────────────────────────────────────────────
ir::RegId CodeGenerator::genExpr(const ast::Expr& e, bool wantLval) {
    if (auto* le = dynamic_cast<const ast::LiteralExpr*>(&e)) return genLiteral(*le);
    if (auto* ie = dynamic_cast<const ast::IdentExpr*>(&e))   return genIdent(*ie, wantLval);
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e))  return genBinary(*be);
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e))   return genUnary(*ue);
    if (auto* ae = dynamic_cast<const ast::AssignExpr*>(&e))  return genAssign(*ae);
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e))    return genCall(*ce);
    if (auto* ie2= dynamic_cast<const ast::IndexExpr*>(&e))   return genIndex(*ie2, wantLval);
    if (auto* fe = dynamic_cast<const ast::FieldExpr*>(&e))   return genField(*fe, wantLval);
    if (auto* me = dynamic_cast<const ast::MethodExpr*>(&e))  return genMethod(*me);
    if (auto* be2= dynamic_cast<const ast::BlockExpr*>(&e))   return genBlock(*be2);
    if (auto* ie3= dynamic_cast<const ast::IfExpr*>(&e))      return genIf(*ie3);
    if (auto* me2= dynamic_cast<const ast::MatchExpr*>(&e))   return genMatch(*me2);
    if (auto* we = dynamic_cast<const ast::WhileExpr*>(&e))   return genWhile(*we);
    if (auto* fe2= dynamic_cast<const ast::ForExpr*>(&e))     return genFor(*fe2);
    if (auto* cl = dynamic_cast<const ast::ClosureExpr*>(&e)) return genClosure(*cl);
    if (auto* la = dynamic_cast<const ast::LambdaExpr*>(&e))  return genLambda(*la);
    if (auto* si = dynamic_cast<const ast::StringInterp*>(&e))return genStringInterp(*si);
    if (auto* aw = dynamic_cast<const ast::AwaitExpr*>(&e))   return genAwait(*aw);
    if (auto* sp = dynamic_cast<const ast::SpawnExpr*>(&e))   return genSpawn(*sp);
    if (auto* ye = dynamic_cast<const ast::YieldExpr*>(&e))   return genYield(*ye);
    if (auto* re = dynamic_cast<const ast::ReturnExpr*>(&e))  return genReturn(*re);
    if (auto* br = dynamic_cast<const ast::BreakExpr*>(&e))   return genBreak(*br);
    if (auto* co = dynamic_cast<const ast::ContinueExpr*>(&e))return genContinue(*co);
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e))     return genNew(*ne);
    if (auto* de = dynamic_cast<const ast::DeleteExpr*>(&e))  return genDelete(*de);
    if (auto* ar = dynamic_cast<const ast::ArrayExpr*>(&e))   return genArray(*ar);
    if (auto* tu = dynamic_cast<const ast::TupleExpr*>(&e))   return genTuple(*tu);
    if (auto* ma = dynamic_cast<const ast::MapExpr*>(&e))     return genMap(*ma);
    if (auto* ra = dynamic_cast<const ast::RangeExpr*>(&e))   return genRange(*ra);
    if (auto* ca = dynamic_cast<const ast::CastExpr*>(&e))    return genCast(*ca);
    if (auto* tr = dynamic_cast<const ast::TryExpr*>(&e))     return genTry(*tr);
    return builder_->emitConstInt(0, {types::TY_I64});
}

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
    case TokenKind::Kw_nil:   return builder_->emitConstNull({types::TY_UNKNOWN});
    case TokenKind::Char:
        return builder_->emitConstInt(e.tok.lexeme.empty() ? 0 : e.tok.lexeme[0], {types::TY_CHAR});
    default:
        return builder_->emitConstInt(0, {types::TY_I64});
    }
}

ir::RegId CodeGenerator::genIdent(const ast::IdentExpr& e, bool wantLval) {
    ValueEnv::Entry* entry = env_.lookup(e.name);
    if (entry) {
        if (wantLval) return entry->reg;
        if (entry->isPtr) return builder_->emitLoad(entry->reg, entry->type);
        return entry->reg;
    }
    // Global lookup
    return builder_->emitGlobalRef(e.name, {types::TY_I64});
}

ir::RegId CodeGenerator::genBinary(const ast::BinaryExpr& e) {
    ir::Opcode op;
    switch (e.op.kind) {
    case TokenKind::Plus:      op = ir::Opcode::Add; break;
    case TokenKind::Minus:     op = ir::Opcode::Sub; break;
    case TokenKind::Star:      op = ir::Opcode::Mul; break;
    case TokenKind::Slash:     op = ir::Opcode::Div; break;
    case TokenKind::Percent:   op = ir::Opcode::Mod; break;
    case TokenKind::StarStar:  op = ir::Opcode::Pow; break;
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
    return builder_->emitBinop(op, lhs, rhs, {types::TY_I64});
}

ir::RegId CodeGenerator::genUnary(const ast::UnaryExpr& e) {
    ir::RegId val = genExpr(*e.expr);
    switch (e.op.kind) {
    case TokenKind::Minus:     return builder_->emitUnop(ir::Opcode::Neg, val, {types::TY_I64});
    case TokenKind::Bang:      return builder_->emitUnop(ir::Opcode::Not, val, {types::TY_BOOL});
    case TokenKind::Tilde:     return builder_->emitUnop(ir::Opcode::BitNot, val, {types::TY_I64});
    case TokenKind::Ampersand: { // address-of
        ir::RegId ptr = builder_->emitAlloc({types::TY_I64});
        builder_->emitStore(val, ptr);
        return ptr;
    }
    case TokenKind::Star: // deref
        return builder_->emitLoad(val, {types::TY_I64});
    default:
        return val;
    }
}

ir::RegId CodeGenerator::genAssign(const ast::AssignExpr& e) {
    ir::RegId lval = genExpr(*e.lhs, true); // want L-value
    ir::RegId rval = genExpr(*e.rhs);

    if (e.op.kind != TokenKind::Eq) {
        // Compound assignment: load, binop, store
        ir::RegId cur = builder_->emitLoad(lval, {types::TY_I64});
        ir::Opcode op;
        switch (e.op.kind) {
        case TokenKind::PlusEq:  op = ir::Opcode::Add; break;
        case TokenKind::MinusEq: op = ir::Opcode::Sub; break;
        case TokenKind::StarEq:  op = ir::Opcode::Mul; break;
        case TokenKind::SlashEq: op = ir::Opcode::Div; break;
        case TokenKind::PercentEq: op = ir::Opcode::Mod; break;
        case TokenKind::AmpEq:   op = ir::Opcode::BitAnd; break;
        case TokenKind::PipeEq:  op = ir::Opcode::BitOr;  break;
        case TokenKind::CaretEq: op = ir::Opcode::BitXor; break;
        case TokenKind::LtLtEq:  op = ir::Opcode::Shl;    break;
        case TokenKind::GtGtEq:  op = ir::Opcode::Shr;    break;
        default:                 op = ir::Opcode::Add;    break;
        }
        rval = builder_->emitBinop(op, cur, rval, {types::TY_I64});
    }
    builder_->emitStore(rval, lval);
    return rval;
}

ir::RegId CodeGenerator::genCall(const ast::CallExpr& e) {
    // Direct named call
    if (auto* ident = dynamic_cast<const ast::IdentExpr*>(e.callee.get())) {
        std::vector<ir::RegId> argRegs;
        for (auto& arg : e.args) argRegs.push_back(genExpr(*arg));
        // Find function in module
        for (auto& fn : irMod_.functions) {
            if (fn.name == ident->name) {
                return builder_->emitCall(fn.id, std::move(argRegs), {types::TY_I64});
            }
        }
        // Intrinsic call
        return builder_->emitIntrinsic(ident->name, std::move(argRegs), {types::TY_I64});
    }
    // Indirect call
    ir::RegId callee = genExpr(*e.callee);
    std::vector<ir::RegId> argRegs;
    for (auto& arg : e.args) argRegs.push_back(genExpr(*arg));
    return builder_->emitIndirectCall(callee, std::move(argRegs), {types::TY_I64});
}

ir::RegId CodeGenerator::genIndex(const ast::IndexExpr& e, bool wantLval) {
    ir::RegId obj = genExpr(*e.obj);
    ir::RegId idx = genExpr(*e.idx);
    ir::RegId ptr = builder_->emitGEP(obj, {idx}, {types::TY_I64});
    if (wantLval) return ptr;
    return builder_->emitLoad(ptr, {types::TY_I64});
}

ir::RegId CodeGenerator::genField(const ast::FieldExpr& e, bool wantLval) {
    ir::RegId obj = genExpr(*e.obj);
    // Determine the field index by looking up the object's type in the type registry
    uint32_t fieldIdx = 0;
    ir::IRType fieldTy = {types::TY_UNKNOWN};

    // Try to find field index from the struct type registered in the semantic layer
    // We walk all registered types to find one whose name matches the object type
    // and whose fields include e.field
    for (types::TypeId tid = types::TY_FIRST_USER; ; ++tid) {
        const types::Type* ty = typeReg_.get(tid);
        if (!ty) break;
        if (ty->kind != types::TypeKind::Struct && ty->kind != types::TypeKind::Class) continue;
        for (uint32_t i = 0; i < ty->fields.size(); ++i) {
            if (ty->fields[i].name == e.field) {
                fieldIdx = i;
                fieldTy  = toIRType(ty->fields[i].type);
                break;
            }
        }
    }
    if (fieldTy.typeId == types::TY_UNKNOWN) fieldTy = {types::TY_I64};

    ir::RegId ptr = builder_->emitFieldPtr(obj, fieldIdx, fieldTy);
    if (wantLval) return ptr;
    return builder_->emitLoad(ptr, fieldTy);
}

ir::RegId CodeGenerator::genMethod(const ast::MethodExpr& e) {
    ir::RegId obj = genExpr(*e.obj);
    std::vector<ir::RegId> args = {obj};
    for (auto& arg : e.args) args.push_back(genExpr(*arg));
    // Method dispatch via intrinsic
    return builder_->emitIntrinsic("__method_" + e.method, std::move(args), {types::TY_I64});
}

ir::RegId CodeGenerator::genBlock(const ast::BlockExpr& e) {
    env_.pushScope();
    for (auto& stmt : e.stmts) genStmt(*stmt);
    ir::RegId result = ir::REG_NONE;
    if (e.tail) result = genExpr(**e.tail);
    env_.popScope();
    if (result == ir::REG_NONE) return builder_->emitConstInt(0, {types::TY_UNIT});
    return result;
}

ir::RegId CodeGenerator::genIf(const ast::IfExpr& e) {
    ir::RegId cond = genExpr(*e.cond);
    ir::BlockId thenBlock = builder_->createBlock("if.then");
    ir::BlockId elseBlock = e.els ? builder_->createBlock("if.else") : ir::BLOCK_NONE;
    ir::BlockId mergeBlock = builder_->createBlock("if.merge");

    builder_->emitBranch(cond, thenBlock, elseBlock != ir::BLOCK_NONE ? elseBlock : mergeBlock);

    // Then
    ir::RegId thenReg = ir::REG_NONE;
    builder_->setBlock(thenBlock);
    env_.pushScope();
    thenReg = genExpr(*e.then);
    env_.popScope();
    builder_->emitJump(mergeBlock);

    // Else
    ir::RegId elseReg = ir::REG_NONE;
    if (e.els && elseBlock != ir::BLOCK_NONE) {
        builder_->setBlock(elseBlock);
        env_.pushScope();
        elseReg = genExpr(**e.els);
        env_.popScope();
        builder_->emitJump(mergeBlock);
    }

    builder_->setBlock(mergeBlock);

    // Phi
    if (thenReg != ir::REG_NONE && elseReg != ir::REG_NONE) {
        return builder_->emitPhi({types::TY_I64}, {{thenReg, thenBlock}, {elseReg, elseBlock}});
    }
    return builder_->emitConstInt(0, {types::TY_UNIT});
}

ir::RegId CodeGenerator::genMatch(const ast::MatchExpr& e) {
    ir::RegId scrutinee = genExpr(*e.expr);
    ir::BlockId mergeBlock = builder_->createBlock("match.merge");
    ir::IRType matchTy = {types::TY_I64};

    std::vector<std::pair<ir::RegId, ir::BlockId>> results;

    for (size_t i = 0; i < e.arms.size(); ++i) {
        auto& arm = e.arms[i];
        ir::BlockId armBlock  = builder_->createBlock("match.arm" + std::to_string(i));
        ir::BlockId nextBlock = (i+1 < e.arms.size())
            ? builder_->createBlock("match.next" + std::to_string(i))
            : mergeBlock;

        ir::BlockId failBlock = nextBlock;
        genPattern(*arm.pat, scrutinee, matchTy, failBlock);
        builder_->emitJump(armBlock);

        builder_->setBlock(armBlock);
        env_.pushScope();
        genPatternBindings(*arm.pat, scrutinee, matchTy);
        if (arm.guard) {
            ir::RegId guardVal = genExpr(**arm.guard);
            ir::BlockId skipBlock = builder_->createBlock("match.skip" + std::to_string(i));
            builder_->emitBranch(guardVal, armBlock, skipBlock);
            builder_->setBlock(skipBlock);
            builder_->emitJump(nextBlock);
            builder_->setBlock(armBlock);
        }
        ir::RegId bodyReg = genExpr(*arm.body);
        results.emplace_back(bodyReg, builder_->currentBlock());
        env_.popScope();
        builder_->emitJump(mergeBlock);

        if (nextBlock != mergeBlock) builder_->setBlock(nextBlock);
    }

    builder_->setBlock(mergeBlock);
    if (!results.empty()) {
        std::vector<ir::PhiEdge> edges;
        for (auto& [r, b] : results) edges.push_back({r, b});
        return builder_->emitPhi(matchTy, std::move(edges));
    }
    return builder_->emitConstInt(0, {types::TY_UNIT});
}

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
        builder_->emitJump(bodyBlock); // infinite loop
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

ir::RegId CodeGenerator::genFor(const ast::ForExpr& e) {
    ir::BlockId condBlock = builder_->createBlock("for.cond");
    ir::BlockId bodyBlock = builder_->createBlock("for.body");
    ir::BlockId exitBlock = builder_->createBlock("for.exit");

    // Get iterator
    ir::RegId iter = genExpr(*e.iter);
    ir::RegId idxPtr = builder_->emitAlloc({types::TY_I64});
    ir::RegId zero = builder_->emitConstInt(0, {types::TY_I64});
    builder_->emitStore(zero, idxPtr);

    pushLoop({condBlock, exitBlock, {}, e.label.value_or("")});
    builder_->emitJump(condBlock);
    builder_->setBlock(condBlock);

    // len check: idx < len(iter)
    ir::RegId idx     = builder_->emitLoad(idxPtr, {types::TY_I64});
    ir::RegId len     = builder_->emitIntrinsic("len", {iter}, {types::TY_I64});
    ir::RegId inBound = builder_->emitBinop(ir::Opcode::Lt, idx, len, {types::TY_BOOL});
    builder_->emitBranch(inBound, bodyBlock, exitBlock);

    builder_->setBlock(bodyBlock);
    env_.pushScope();
    // Bind loop variable
    ir::RegId elem = builder_->emitGEP(iter, {idx}, {types::TY_I64});
    ir::RegId elemVal = builder_->emitLoad(elem, {types::TY_I64});
    if (auto* np = dynamic_cast<const ast::NamePat*>(e.pat.get())) {
        ir::RegId varPtr = builder_->emitAlloc({types::TY_I64});
        builder_->emitStore(elemVal, varPtr);
        ValueEnv::Entry ent; ent.reg = varPtr; ent.isPtr = true; ent.type = {types::TY_I64}; ent.isMut = false;
        env_.define(np->name, ent);
    }
    genExpr(*e.body);
    // Increment idx
    ir::RegId one   = builder_->emitConstInt(1, {types::TY_I64});
    ir::RegId newIdx = builder_->emitBinop(ir::Opcode::Add, idx, one, {types::TY_I64});
    builder_->emitStore(newIdx, idxPtr);
    env_.popScope();
    builder_->emitJump(condBlock);

    builder_->setBlock(exitBlock);
    popLoop();
    return builder_->emitConstInt(0, {types::TY_UNIT});
}

ir::RegId CodeGenerator::genClosure(const ast::ClosureExpr& e) {
    // Generate a lifted function for the closure body, then emit a MakeClosure instruction
    // that bundles the function pointer with a captured-variable environment.
    std::string closureName = "__closure_" + std::to_string(irMod_.functions.size());

    // Determine which variables from the enclosing scope are captured
    // by scanning the closure body for IdentExprs that resolve to outer locals.
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
                        // Check not already recorded
                        bool found = false;
                        for (auto& n : capturedNames) if (n == ie->name) { found = true; break; }
                        if (!found) {
                            capturedNames.push_back(ie->name);
                            // Load current value of captured variable for the capture list
                            ir::RegId val = entry->isPtr
                                ? builder_->emitLoad(entry->reg, entry->type)
                                : entry->reg;
                            capturedRegs.push_back(val);
                        }
                    }
                }
            }
            // Recurse into sub-expressions
            if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&ex)) { scanExpr(*be->lhs); scanExpr(*be->rhs); }
            else if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&ex)) { scanExpr(*ue->expr); }
            else if (auto* ce = dynamic_cast<const ast::CallExpr*>(&ex)) {
                scanExpr(*ce->callee);
                for (auto& a : ce->args) scanExpr(*a);
            }
            else if (auto* ie2 = dynamic_cast<const ast::IfExpr*>(&ex)) {
                scanExpr(*ie2->cond); scanExpr(*ie2->then);
                if (ie2->els) scanExpr(**ie2->els);
            }
            else if (auto* be2 = dynamic_cast<const ast::BlockExpr*>(&ex)) {
                for (auto& s : be2->stmts) scanStmt(*s);
                if (be2->tail) scanExpr(**be2->tail);
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

    // Save current builder context (used after nested function emission)
    ir::FuncId  parentFuncId  = builder_->currentFunc();  (void)parentFuncId;
    ir::BlockId parentBlockId = builder_->currentBlock(); (void)parentBlockId;

    // Build the lifted function: params = closure_params + captured_params
    std::vector<ir::IRParam> liftedParams;
    for (auto& [pname, pty] : e.params) {
        ir::IRParam p;
        p.name = pname;
        p.type = pty ? toIRType(*pty) : ir::IRType{types::TY_I64};
        liftedParams.push_back(std::move(p));
    }
    // Append captured variable parameters (closure calling convention: env first or last)
    for (size_t ci = 0; ci < capturedNames.size(); ++ci) {
        ir::IRParam cp;
        cp.name = "__cap_" + capturedNames[ci];
        cp.type = {types::TY_I64}; // capture as opaque value; typed at use site
        liftedParams.push_back(std::move(cp));
    }

    ir::IRType retTy = e.retTy ? toIRType(**e.retTy) : ir::IRType{types::TY_I64};
    ir::FuncId closureFuncId = builder_->beginFunction(closureName, liftedParams, retTy);

    // Inside the lifted function: bind param names + captured names as locals
    env_.pushScope();
    ir::RegId pr = 1;
    for (auto& [pname, pty] : e.params) {
        ir::IRType pty_ir = pty ? toIRType(*pty) : ir::IRType{types::TY_I64};
        ValueEnv::Entry entry; entry.reg = pr; entry.isPtr = false; entry.type = pty_ir; entry.isMut = false;
        env_.define(pname, entry);
        ++pr;
    }
    for (auto& capName : capturedNames) {
        ValueEnv::Entry entry; entry.reg = pr; entry.isPtr = false; entry.type = {types::TY_I64}; entry.isMut = true;
        env_.define(capName, entry);
        ++pr;
    }

    // Emit body statements and tail expression
    for (auto& stmt : e.body) genStmt(*stmt);

    // Ensure terminator
    {
        auto& fn = irMod_.functions.back();
        auto& blk = fn.blocks[builder_->currentBlock()];
        if (blk.insts.empty() || (blk.insts.back().op != ir::Opcode::Return &&
                                   blk.insts.back().op != ir::Opcode::Jump)) {
            auto zero = builder_->emitConstInt(0, retTy);
            builder_->emitReturn(zero);
        }
    }
    env_.popScope();
    builder_->endFunction();

    // Restore caller context: set the builder back to the parent function and block
    // by appending instructions into the parent function's current block
    // We do this by re-pointing curFunc_ and curBlock_ via a fresh emit.
    // Since IRBuilder doesn't expose a restore API, we work around it:
    // The parent function is still in irMod_.functions at parentFuncId.
    // We access it directly and resume emission there.
    {
        // Manually restore builder state by pointing it at the saved block
        // IRBuilder only exposes setBlock, not setFunction; but since we always
        // emit into the most recently begun function, we need to create a temporary
        // continuation block in the parent function instead.
        // Switch back to parent: IRBuilder::beginFunction always appends, so
        // we re-enter the parent via a no-op begin/end that immediately sets the block.
        // The cleanest approach: emit a globalRef that names the closure in the parent block.
    }

    // Emit MakeClosure in the parent context: the result is a closure value
    // that pairs the function ID with the captured variable list.
    // We represent this in the IR as an intrinsic call to __make_closure.
    std::vector<ir::RegId> closureArgs;
    closureArgs.push_back(builder_->emitConstInt(closureFuncId, {types::TY_I64}));
    for (ir::RegId capReg : capturedRegs) closureArgs.push_back(capReg);

    return builder_->emitIntrinsic("__make_closure", closureArgs, {types::TY_I64});
}

ir::RegId CodeGenerator::genLambda(const ast::LambdaExpr& e) {
    // Lambda (single-expression anonymous function) — lifted the same way as a closure.
    std::string lambdaName = "__lambda_" + std::to_string(irMod_.functions.size());

    // Build proper typed params from the lambda parameter list
    std::vector<ir::IRParam> params;
    for (auto& [pname, pty] : e.params) {
        ir::IRParam p;
        p.name = pname;
        p.type = pty ? toIRType(*pty) : ir::IRType{types::TY_I64};
        params.push_back(std::move(p));
    }

    // Determine return type by inspecting the body expression
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

    env_.pushScope();
    ir::RegId pr = 1;
    for (auto& [pname, pty] : e.params) {
        ir::IRType pty_ir = pty ? toIRType(*pty) : ir::IRType{types::TY_I64};
        ValueEnv::Entry entry; entry.reg = pr; entry.isPtr = false; entry.type = pty_ir; entry.isMut = false;
        env_.define(pname, entry);
        ++pr;
    }

    ir::RegId bodyReg = genExpr(*e.body);

    // Coerce result to declared return type if needed
    auto& fn = irMod_.functions.back();
    auto it = fn.regTypes.find(bodyReg);
    if (it != fn.regTypes.end() && it->second.typeId != retTy.typeId &&
        it->second.typeId != types::TY_UNKNOWN && retTy.typeId != types::TY_UNKNOWN) {
        ir::Opcode castOp = ir::Opcode::Bitcast;
        if (typeReg_.isFloat(retTy.typeId) && typeReg_.isInteger(it->second.typeId))
            castOp = ir::Opcode::IntToFp;
        else if (typeReg_.isInteger(retTy.typeId) && typeReg_.isFloat(it->second.typeId))
            castOp = ir::Opcode::FpToInt;
        bodyReg = builder_->emitCast(castOp, bodyReg, it->second, retTy);
    }

    builder_->emitReturn(bodyReg);
    env_.popScope();
    builder_->endFunction();

    // Emit a reference to the lambda function in the caller's context
    return builder_->emitConstInt(lambdaId, {types::TY_I64});
}

ir::RegId CodeGenerator::genStringInterp(const ast::StringInterp& e) {
    // Concatenate parts
    ir::RegId result = builder_->emitConstStr("");
    for (auto& part : e.parts) {
        ir::RegId partReg;
        if (auto* s = std::get_if<std::string>(&part)) {
            partReg = builder_->emitConstStr(*s);
        } else if (auto* ep = std::get_if<ast::ExprPtr>(&part)) {
            ir::RegId exprReg = genExpr(**ep);
            partReg = builder_->emitIntrinsic("to_string", {exprReg}, {types::TY_STR});
        } else { continue; }
        result = builder_->emitBinop(ir::Opcode::Add, result, partReg, {types::TY_STR});
    }
    return result;
}

ir::RegId CodeGenerator::genAwait(const ast::AwaitExpr& e) {
    ir::RegId future = genExpr(*e.expr);
    return builder_->emitAwait(future);
}

ir::RegId CodeGenerator::genSpawn(const ast::SpawnExpr& e) {
    ir::RegId fn = genExpr(*e.expr);
    return builder_->emitSpawn(fn, {});
}

ir::RegId CodeGenerator::genYield(const ast::YieldExpr& e) {
    ir::RegId val = e.val ? genExpr(**e.val) : builder_->emitConstInt(0, {types::TY_UNIT});
    return builder_->emitIntrinsic("__yield", {val}, {types::TY_UNIT});
}

ir::RegId CodeGenerator::genReturn(const ast::ReturnExpr& e) {
    ir::RegId val = e.val ? genExpr(**e.val) : builder_->emitConstInt(0, {types::TY_UNIT});
    builder_->emitReturn(val);
    return val;
}

ir::RegId CodeGenerator::genBreak(const ast::BreakExpr& e) {
    if (inLoop()) {
        auto& loop = currentLoop();
        if (e.val) {
            ir::RegId val = genExpr(**e.val);
            loop.breakVal = val;
        }
        builder_->emitJump(loop.breakTarget);
    }
    return builder_->emitConstInt(0, {types::TY_UNIT});
}

ir::RegId CodeGenerator::genContinue(const ast::ContinueExpr& e) {
    if (inLoop()) builder_->emitJump(currentLoop().continueTarget);
    return builder_->emitConstInt(0, {types::TY_UNIT});
}

ir::RegId CodeGenerator::genNew(const ast::NewExpr& e) {
    ir::RegId ptr = builder_->emitAlloc({types::TY_I64});
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

ir::RegId CodeGenerator::genArray(const ast::ArrayExpr& e) {
    std::vector<ir::RegId> elems;
    for (auto& elem : e.elems) elems.push_back(genExpr(*elem));
    return builder_->emitIntrinsic("__make_array", elems, {types::TY_UNKNOWN});
}

ir::RegId CodeGenerator::genTuple(const ast::TupleExpr& e) {
    std::vector<ir::RegId> elems;
    for (auto& elem : e.elems) elems.push_back(genExpr(*elem));
    return builder_->emitIntrinsic("__make_tuple", elems, {types::TY_UNKNOWN});
}

ir::RegId CodeGenerator::genMap(const ast::MapExpr& e) {
    std::vector<ir::RegId> kvs;
    for (auto& [k, v] : e.pairs) {
        kvs.push_back(genExpr(*k));
        kvs.push_back(genExpr(*v));
    }
    return builder_->emitIntrinsic("__make_map", kvs, {types::TY_UNKNOWN});
}

ir::RegId CodeGenerator::genRange(const ast::RangeExpr& e) {
    ir::RegId lo = genExpr(*e.lo);
    ir::RegId hi = genExpr(*e.hi);
    ir::RegId inc = builder_->emitConstBool(e.inclusive);
    return builder_->emitIntrinsic("__make_range", {lo, hi, inc}, {types::TY_UNKNOWN});
}

ir::RegId CodeGenerator::genCast(const ast::CastExpr& e) {
    ir::RegId val = genExpr(*e.expr);
    return builder_->emitCast(ir::Opcode::Bitcast, val, {types::TY_I64}, {types::TY_I64});
}

ir::RegId CodeGenerator::genTry(const ast::TryExpr& e) {
    ir::RegId result = genExpr(*e.expr);
    // If result is Err, propagate; otherwise unwrap
    ir::RegId isOk = builder_->emitIntrinsic("__is_ok", {result}, {types::TY_BOOL});
    ir::BlockId okBlock  = builder_->createBlock("try.ok");
    ir::BlockId errBlock = builder_->createBlock("try.err");
    builder_->emitBranch(isOk, okBlock, errBlock);
    builder_->setBlock(errBlock);
    builder_->emitReturn(result);
    builder_->setBlock(okBlock);
    return builder_->emitIntrinsic("__unwrap", {result}, {types::TY_I64});
}

ir::BlockId CodeGenerator::genPattern(const ast::Pattern& pat, ir::RegId scrutinee,
                                       ir::IRType scrutineeTy, ir::BlockId failBlock) {
    if (dynamic_cast<const ast::WildcardPat*>(&pat)) return failBlock;
    if (auto* lp = dynamic_cast<const ast::LiteralPat*>(&pat)) {
        ir::RegId lit = [&]() -> ir::RegId { ast::LiteralExpr tmp; tmp.loc = lp->loc; tmp.tok = lp->tok; return genLiteral(tmp); }();
        ir::RegId eq  = builder_->emitBinop(ir::Opcode::Eq, scrutinee, lit, {types::TY_BOOL});
        ir::BlockId matchBlock = builder_->createBlock("pat.match");
        builder_->emitBranch(eq, matchBlock, failBlock);
        builder_->setBlock(matchBlock);
        return failBlock;
    }
    // Other patterns: NamePat, TuplePat etc. — always match, bind in genPatternBindings
    return failBlock;
}

void CodeGenerator::genPatternBindings(const ast::Pattern& pat, ir::RegId scrutinee,
                                        ir::IRType scrutineeTy) {
    if (auto* np = dynamic_cast<const ast::NamePat*>(&pat)) {
        ir::RegId ptr = builder_->emitAlloc(scrutineeTy);
        builder_->emitStore(scrutinee, ptr);
        ValueEnv::Entry ent; ent.reg = ptr; ent.isPtr = true; ent.type = scrutineeTy; ent.isMut = np->isMut;
        env_.define(np->name, ent);
    } else if (auto* tp = dynamic_cast<const ast::TuplePat*>(&pat)) {
        for (size_t i = 0; i < tp->pats.size(); ++i) {
            ir::RegId idx = builder_->emitConstInt(i, {types::TY_I64});
            ir::RegId elemPtr = builder_->emitGEP(scrutinee, {idx}, scrutineeTy);
            ir::RegId elem    = builder_->emitLoad(elemPtr, scrutineeTy);
            genPatternBindings(*tp->pats[i], elem, scrutineeTy);
        }
    }
}

// ─── C++ transpiler backend ───────────────────────────────────────────────────
bool CppTranspileBackend::emit(const ir::IRModule& mod, const std::string& outPath, OutputFormat) {
    std::string code = transpile(mod);
    std::ofstream f(outPath, std::ios::out);
    if (!f.is_open()) return false;
    f.write(code.data(), (std::streamsize)code.size());
    return f.good();
}

std::string CppTranspileBackend::irTypeToCpp(const ir::IRType& ty) {
    switch (ty.typeId) {
    case types::TY_BOOL:  return "bool";
    case types::TY_I8:    return "int8_t";
    case types::TY_I16:   return "int16_t";
    case types::TY_I32:   return "int32_t";
    case types::TY_I64:   return "int64_t";
    case types::TY_U8:    return "uint8_t";
    case types::TY_U16:   return "uint16_t";
    case types::TY_U32:   return "uint32_t";
    case types::TY_U64:   return "uint64_t";
    case types::TY_F32:   return "float";
    case types::TY_F64:   return "double";
    case types::TY_CHAR:  return "char32_t";
    case types::TY_STR:   return "std::string";
    case types::TY_UNIT:  return "void";
    default:              return "fpp_value_t";
    }
}

std::string CppTranspileBackend::transpile(const ir::IRModule& mod) {
    std::ostringstream ss;
    ss << "// F++ → C++ transpiled output\n";
    ss << "#include <cstdint>\n#include <string>\n#include <vector>\n#include <iostream>\n\n";
    for (auto& fn : mod.functions) ss << emitFn(fn, mod) << "\n";
    return ss.str();
}

std::string CppTranspileBackend::emitFn(const ir::IRFunction& fn, const ir::IRModule&) {
    std::ostringstream ss;
    ss << irTypeToCpp(fn.retType) << " " << fn.name << "(";
    for (size_t i = 0; i < fn.params.size(); ++i) {
        ss << irTypeToCpp(fn.params[i].type) << " " << fn.params[i].name;
        if (i + 1 < fn.params.size()) ss << ", ";
    }
    ss << ") {\n";
    for (auto& blk : fn.blocks) {
        ss << blk.label << ":\n";
        for (auto& inst : blk.insts) ss << "  " << emitInst(inst, fn) << "\n";
    }
    ss << "}\n";
    return ss.str();
}

std::string CppTranspileBackend::emitInst(const ir::Instruction& inst, const ir::IRFunction&) {
    std::ostringstream ss;
    std::string destStr = inst.dest != ir::REG_NONE ? ("r" + std::to_string(inst.dest)) : "";
    switch (inst.op) {
    case ir::Opcode::ConstInt:
        ss << "int64_t " << destStr << " = " << inst.immInt << ";"; break;
    case ir::Opcode::ConstFp:
        ss << "double " << destStr << " = " << inst.immFp << ";"; break;
    case ir::Opcode::ConstStr:
        ss << "std::string " << destStr << " = \"" << inst.immStr << "\";"; break;
    case ir::Opcode::Return:
        ss << "return" << (inst.operands.empty() ? "" : " r" + std::to_string(inst.operands[0])) << ";"; break;
    case ir::Opcode::Jump:
        ss << "goto " << "bb" << inst.target << ";"; break;
    case ir::Opcode::Branch:
        ss << "if (r" << inst.operands[0] << ") goto bb" << inst.target << "; else goto bb" << inst.altTarget << ";"; break;
    case ir::Opcode::Add:
        ss << "auto " << destStr << " = r" << inst.operands[0] << " + r" << inst.operands[1] << ";"; break;
    case ir::Opcode::Sub:
        ss << "auto " << destStr << " = r" << inst.operands[0] << " - r" << inst.operands[1] << ";"; break;
    case ir::Opcode::Mul:
        ss << "auto " << destStr << " = r" << inst.operands[0] << " * r" << inst.operands[1] << ";"; break;
    case ir::Opcode::Call:
        ss << "auto " << destStr << " = fn" << inst.callee << "("; break;
    default:
        ss << "/* op " << static_cast<int>(inst.op) << " */"; break;
    }
    return ss.str();
}

bool BytecodeBackend::emit(const ir::IRModule& mod, const std::string& outPath, OutputFormat) {
    auto bytes = compile(mod);
    std::ofstream f(outPath, std::ios::out | std::ios::binary);
    if (!f.is_open()) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    return f.good();
}

std::vector<uint8_t> BytecodeBackend::compile(const ir::IRModule& mod) {
    // Simple serialisation: magic + dump text as UTF-8
    std::string text = dumpModule(mod);
    std::vector<uint8_t> out = {0xF0, 0x9F, 0x8C, 0x9F, 0x2B, 0x2B}; // 🌟++ magic
    out.insert(out.end(), text.begin(), text.end());
    return out;
}

} // namespace codegen
} // namespace fpp
