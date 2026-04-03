#include "../include/types.hpp"
#include <stdexcept>
#include <sstream>
#include <cassert>
#include <algorithm>
#include <cstring>
#include <queue>
#include <functional>
#include <unordered_set>

namespace fpp {
namespace types {

// ─── Scope ──────────────────────────────────────────────────────────────────
bool Scope::define(std::string name, Symbol sym) {
    if (syms_.count(name)) return false;
    syms_[name] = std::move(sym);
    return true;
}

Symbol* Scope::lookup(const std::string& name) {
    auto it = syms_.find(name);
    if (it != syms_.end()) return &it->second;
    if (parent_) return parent_->lookup(name);
    return nullptr;
}

const Symbol* Scope::lookup(const std::string& name) const {
    auto it = syms_.find(name);
    if (it != syms_.end()) return &it->second;
    if (parent_) return parent_->lookup(name);
    return nullptr;
}

Symbol* Scope::lookupLocal(const std::string& name) {
    auto it = syms_.find(name);
    return it != syms_.end() ? &it->second : nullptr;
}

// ─── TypeRegistry ────────────────────────────────────────────────────────────
TypeRegistry::TypeRegistry() {
    // Reserve slot 0
    types_.resize(TY_FIRST_USER);

    auto prim = [&](TypeId id, const std::string& name, size_t sz, size_t al) {
        Type t;
        t.id = id; t.kind = TypeKind::Primitive; t.name = name;
        t.sizeBytes = sz; t.alignBytes = al;
        types_[id] = std::move(t);
    };
    prim(TY_UNKNOWN, "unknown",  0, 1);
    prim(TY_NEVER,   "never",    0, 1);
    prim(TY_UNIT,    "unit",     0, 1);
    prim(TY_BOOL,    "bool",     1, 1);
    prim(TY_I8,      "i8",       1, 1);
    prim(TY_I16,     "i16",      2, 2);
    prim(TY_I32,     "i32",      4, 4);
    prim(TY_I64,     "i64",      8, 8);
    prim(TY_I128,    "i128",    16, 16);
    prim(TY_ISIZE,   "isize",    8, 8);
    prim(TY_U8,      "u8",       1, 1);
    prim(TY_U16,     "u16",      2, 2);
    prim(TY_U32,     "u32",      4, 4);
    prim(TY_U64,     "u64",      8, 8);
    prim(TY_U128,    "u128",    16, 16);
    prim(TY_USIZE,   "usize",    8, 8);
    prim(TY_F32,     "f32",      4, 4);
    prim(TY_F64,     "f64",      8, 8);
    prim(TY_CHAR,    "char",     4, 4);
    prim(TY_STR,     "str",     16, 8); // fat pointer: ptr + len
    prim(TY_RAWPTR,  "rawptr",   8, 8);
}

TypeId TypeRegistry::registerType(Type t) {
    TypeId id = nextId_++;
    t.id = id;
    computeLayout(t);
    if (id >= types_.size()) types_.resize(id + 1);
    types_[id] = std::move(t);
    return id;
}

const Type* TypeRegistry::get(TypeId id) const {
    TypeId r = resolved(id);
    if (r < types_.size()) return &types_[r];
    return nullptr;
}

Type* TypeRegistry::getMut(TypeId id) {
    TypeId r = resolved(id);
    if (r < types_.size()) return &types_[r];
    return nullptr;
}

TypeId TypeRegistry::freshInfer() {
    TypeId id = nextInfer_++;
    Type t;
    t.id   = id;
    t.kind = TypeKind::Infer;
    t.name = "?T" + std::to_string(id);
    if (id >= types_.size()) types_.resize(id + 1);
    types_[id] = std::move(t);
    return id;
}

TypeId TypeRegistry::find(TypeId id) const {
    while (inferBindings_.count(id)) {
        TypeId next = inferBindings_.at(id);
        if (next == id) break;
        id = next;
    }
    return id;
}

TypeId TypeRegistry::resolved(TypeId id) const { return find(id); }

bool TypeRegistry::unify(TypeId a, TypeId b) {
    a = find(a); b = find(b);
    if (a == b) return true;
    const Type* ta = get(a);
    const Type* tb = get(b);
    if (!ta || !tb) return false;

    // Infer types bind to concrete
    if (ta->kind == TypeKind::Infer) { inferBindings_[a] = b; return true; }
    if (tb->kind == TypeKind::Infer) { inferBindings_[b] = a; return true; }

    // Structural unification
    if (ta->kind != tb->kind) return false;
    if (ta->name != tb->name && !ta->name.empty() && !tb->name.empty()) return false;

    // For generic instances: unify args pairwise
    if (ta->args.size() != tb->args.size()) return false;
    for (size_t i = 0; i < ta->args.size(); ++i)
        if (!unify(ta->args[i], tb->args[i])) return false;

    // Tuple
    if (ta->kind == TypeKind::Tuple) {
        if (ta->elems.size() != tb->elems.size()) return false;
        for (size_t i = 0; i < ta->elems.size(); ++i)
            if (!unify(ta->elems[i], tb->elems[i])) return false;
    }
    return true;
}

bool TypeRegistry::isSubtype(TypeId sub, TypeId sup) const {
    sub = resolved(sub); sup = resolved(sup);
    if (sub == sup) return true;
    if (sup == TY_UNKNOWN) return true;
    const Type* ts = get(sub);
    const Type* tp = get(sup);
    if (!ts || !tp) return false;

    // Never is subtype of everything
    if (sub == TY_NEVER) return true;

    // Numeric widening
    if (isInteger(sub) && isInteger(sup)) return typeSize(sub) <= typeSize(sup);
    if (isFloat(sub) && isFloat(sup)) return typeSize(sub) <= typeSize(sup);

    // Trait object: struct implements trait
    if (tp->kind == TypeKind::Trait) {
        for (TypeId t : ts->traits) if (t == sup) return true;
    }

    // Class inheritance: walk the base chain via field named "__base__" or via registered parent type
    if (ts->kind == TypeKind::Class && tp->kind == TypeKind::Class) {
        // Walk sub's inheritance chain — base types are stored in ts->traits for classes
        // (we reuse the traits vector to store parent class TypeIds during registration)
        std::unordered_set<TypeId> visited;
        std::queue<TypeId> work;
        work.push(sub);
        while (!work.empty()) {
            TypeId cur = work.front(); work.pop();
            if (visited.count(cur)) continue;
            visited.insert(cur);
            if (cur == sup) return true;
            const Type* ct = get(cur);
            if (!ct) continue;
            // bases stored in traits vector for class types
            for (TypeId base : ct->traits) {
                if (!visited.count(base)) work.push(base);
            }
        }
        return false;
    }

    return false;
}

TypeId TypeRegistry::makePtr(TypeId inner, bool isMut) {
    Type t; t.kind = TypeKind::Ptr; t.name = (isMut ? "*mut " : "*") + typeName(inner);
    t.inner = inner; t.isMut = isMut; t.sizeBytes = 8; t.alignBytes = 8;
    return registerType(std::move(t));
}

TypeId TypeRegistry::makeRef(TypeId inner, bool isMut) {
    Type t; t.kind = TypeKind::Ref; t.name = (isMut ? "&mut " : "&") + typeName(inner);
    t.inner = inner; t.isMut = isMut; t.sizeBytes = 8; t.alignBytes = 8;
    return registerType(std::move(t));
}

TypeId TypeRegistry::makeArray(TypeId elem, size_t len) {
    Type t; t.kind = TypeKind::Array;
    t.name = "[" + typeName(elem) + "; " + std::to_string(len) + "]";
    t.elemTy = elem; t.arrayLen = len;
    if (const Type* et = get(elem)) t.sizeBytes = et->sizeBytes * len;
    return registerType(std::move(t));
}

TypeId TypeRegistry::makeSlice(TypeId elem) {
    Type t; t.kind = TypeKind::Slice; t.name = "[" + typeName(elem) + "]";
    t.elemTy = elem; t.sizeBytes = 16; // fat pointer
    return registerType(std::move(t));
}

TypeId TypeRegistry::makeTuple(std::vector<TypeId> elems) {
    Type t; t.kind = TypeKind::Tuple; t.elems = elems;
    std::string n = "(";
    for (size_t i = 0; i < elems.size(); ++i) {
        n += typeName(elems[i]);
        if (i + 1 < elems.size()) n += ", ";
    }
    n += ")"; t.name = n;
    size_t sz = 0;
    for (TypeId e : elems) if (const Type* et = get(e)) sz += et->sizeBytes;
    t.sizeBytes = sz;
    return registerType(std::move(t));
}

TypeId TypeRegistry::makeFn(FnSig sig) {
    Type t; t.kind = TypeKind::Fn;
    t.sig = std::make_unique<FnSig>(std::move(sig));
    t.name = "fn(...)"; t.sizeBytes = 8;
    return registerType(std::move(t));
}

TypeId TypeRegistry::makeOption(TypeId inner) {
    Type t; t.kind = TypeKind::Option;
    t.name = "Option<" + typeName(inner) + ">"; t.inner = inner;
    if (const Type* et = get(inner)) t.sizeBytes = et->sizeBytes + 1; // tag
    return registerType(std::move(t));
}

TypeId TypeRegistry::makeResult(TypeId ok, TypeId err) {
    Type t; t.kind = TypeKind::Result;
    t.name = "Result<" + typeName(ok) + ", " + typeName(err) + ">";
    t.elems = {ok, err};
    return registerType(std::move(t));
}

TypeId TypeRegistry::makeVec(TypeId elem) {
    Type t; t.kind = TypeKind::Vec;
    t.name = "Vec<" + typeName(elem) + ">"; t.elemTy = elem;
    t.sizeBytes = 24; // ptr + len + cap
    return registerType(std::move(t));
}

TypeId TypeRegistry::makeMap(TypeId key, TypeId val) {
    Type t; t.kind = TypeKind::Map;
    t.name = "Map<" + typeName(key) + ", " + typeName(val) + ">";
    t.elems = {key, val}; t.sizeBytes = 48;
    return registerType(std::move(t));
}

std::string TypeRegistry::typeName(TypeId id) const {
    const Type* t = get(id);
    return t ? t->name : "unknown";
}

bool TypeRegistry::isPrimitive(TypeId id) const {
    const Type* t = get(id);
    return t && t->kind == TypeKind::Primitive;
}

bool TypeRegistry::isInteger(TypeId id) const {
    return id >= TY_I8 && id <= TY_USIZE;
}

bool TypeRegistry::isFloat(TypeId id) const { return id == TY_F32 || id == TY_F64; }
bool TypeRegistry::isNumeric(TypeId id) const { return isInteger(id) || isFloat(id); }
bool TypeRegistry::isSigned(TypeId id) const { return id >= TY_I8 && id <= TY_ISIZE; }

size_t TypeRegistry::typeSize(TypeId id) const {
    const Type* t = get(id);
    return t ? t->sizeBytes : 0;
}

TypeId TypeRegistry::instantiate(TypeId generic, const std::vector<TypeId>& args) {
    const Type* gt = get(generic);
    if (!gt) return generic;
    if (gt->params.empty() || args.empty()) return generic;

    // Build substitution map: param name → concrete TypeId
    std::unordered_map<std::string, TypeId> subst;
    for (size_t i = 0; i < gt->params.size() && i < args.size(); ++i)
        subst[gt->params[i].name] = args[i];

    // Deep-copy and substitute the generic type (can't copy unique_ptr<FnSig> directly)
    Type inst;
    inst.id        = gt->id;
    inst.kind      = gt->kind;
    inst.name      = gt->name;
    inst.fields    = gt->fields;
    inst.params    = gt->params;
    inst.args      = gt->args;
    inst.inner     = gt->inner;
    inst.elems     = gt->elems;
    inst.elemTy    = gt->elemTy;
    inst.arrayLen  = gt->arrayLen;
    inst.traits    = gt->traits;
    inst.sizeBytes = gt->sizeBytes;
    inst.alignBytes= gt->alignBytes;
    inst.isMut     = gt->isMut;
    if (gt->sig) {
        inst.sig = std::make_unique<FnSig>(*gt->sig);
    }
    inst.args   = args;
    inst.params.clear(); // fully instantiated — no free type params

    // Build display name
    inst.name = gt->name + "<";
    for (size_t i = 0; i < args.size(); ++i) {
        inst.name += typeName(args[i]);
        if (i + 1 < args.size()) inst.name += ", ";
    }
    inst.name += ">";

    // Substitute type references in fields
    std::function<TypeId(TypeId)> substTy = [&](TypeId tid) -> TypeId {
        const Type* t = get(tid);
        if (!t) return tid;
        if (t->kind == TypeKind::Generic) {
            auto it = subst.find(t->name);
            return it != subst.end() ? it->second : tid;
        }
        if (t->kind == TypeKind::Ptr || t->kind == TypeKind::Ref) {
            TypeId inner2 = substTy(t->inner);
            return t->kind == TypeKind::Ptr ? makePtr(inner2, t->isMut) : makeRef(inner2, t->isMut);
        }
        if (t->kind == TypeKind::Array) return makeArray(substTy(t->elemTy), t->arrayLen);
        if (t->kind == TypeKind::Slice) return makeSlice(substTy(t->elemTy));
        if (t->kind == TypeKind::Option) return makeOption(substTy(t->inner));
        if (t->kind == TypeKind::Vec) return makeVec(substTy(t->elemTy));
        if (t->kind == TypeKind::Tuple) {
            std::vector<TypeId> newElems;
            for (TypeId e : t->elems) newElems.push_back(substTy(e));
            return makeTuple(std::move(newElems));
        }
        if (!t->args.empty()) {
            std::vector<TypeId> newArgs;
            for (TypeId a : t->args) newArgs.push_back(substTy(a));
            return instantiate(tid, newArgs);
        }
        return tid;
    };

    for (auto& f : inst.fields) f.type = substTy(f.type);
    if (inst.sig) {
        for (auto& p : inst.sig->params) p = substTy(p);
        inst.sig->ret = substTy(inst.sig->ret);
    }
    if (inst.inner != TY_UNKNOWN) inst.inner = substTy(inst.inner);
    inst.elemTy = substTy(inst.elemTy);
    for (auto& e : inst.elems) e = substTy(e);

    computeLayout(inst);
    return registerType(std::move(inst));
}

void TypeRegistry::computeLayout(Type& t) {
    if (t.kind == TypeKind::Struct || t.kind == TypeKind::Class) {
        size_t sz = 0, al = 1;
        for (auto& f : t.fields) {
            const Type* ft = get(f.type);
            if (!ft) continue;
            size_t fa = ft->alignBytes;
            // Align field
            if (fa > 1) sz = (sz + fa - 1) & ~(fa - 1);
            f.offset = sz;
            sz += ft->sizeBytes;
            al = std::max(al, fa);
        }
        // Final padding
        if (al > 1) sz = (sz + al - 1) & ~(al - 1);
        t.sizeBytes  = sz;
        t.alignBytes = al;
    }
}

// ─── SemanticAnalyser ────────────────────────────────────────────────────────
SemanticAnalyser::SemanticAnalyser(TypeRegistry& reg)
    : reg_(reg)
    , globalScope_(std::make_shared<Scope>()) {

    // Pre-populate builtins
    auto defBuiltin = [&](const std::string& name, TypeId ty, SymKind k = SymKind::Type) {
        Symbol s; s.name = name; s.type = ty; s.kind = k;
        s.isPublic = true; s.scopeDepth = 0;
        globalScope_->define(name, std::move(s));
    };
    defBuiltin("bool",   TY_BOOL);
    defBuiltin("i8",     TY_I8);  defBuiltin("i16",   TY_I16);
    defBuiltin("i32",    TY_I32); defBuiltin("i64",   TY_I64);
    defBuiltin("i128",   TY_I128);defBuiltin("isize", TY_ISIZE);
    defBuiltin("u8",     TY_U8);  defBuiltin("u16",   TY_U16);
    defBuiltin("u32",    TY_U32); defBuiltin("u64",   TY_U64);
    defBuiltin("u128",   TY_U128);defBuiltin("usize", TY_USIZE);
    defBuiltin("f32",    TY_F32); defBuiltin("f64",   TY_F64);
    defBuiltin("char",   TY_CHAR);defBuiltin("str",   TY_STR);
    defBuiltin("unit",   TY_UNIT);defBuiltin("never", TY_NEVER);

    // Built-in functions
    auto defFn = [&](const std::string& name, TypeId ty) {
        Symbol s; s.name = name; s.type = ty; s.kind = SymKind::Function;
        s.isPublic = true; s.scopeDepth = 0;
        globalScope_->define(name, std::move(s));
    };
    FnSig printSig; printSig.params = {TY_STR}; printSig.ret = TY_UNIT;
    defFn("print",   reg_.makeFn(printSig));
    defFn("println", reg_.makeFn(printSig));
    FnSig panicSig; panicSig.params = {TY_STR}; panicSig.ret = TY_NEVER;
    defFn("panic",   reg_.makeFn(panicSig));
    FnSig assertSig; assertSig.params = {TY_BOOL, TY_STR}; assertSig.ret = TY_UNIT;
    defFn("assert",  reg_.makeFn(assertSig));
}

bool SemanticAnalyser::hasErrors() const {
    return std::any_of(diags_.begin(), diags_.end(),
        [](const Diagnostic& d){ return d.level >= Diagnostic::Level::Error; });
}

void SemanticAnalyser::error(const std::string& msg, SourceLocation loc) {
    diags_.push_back({Diagnostic::Level::Error, msg, loc, {}});
}
void SemanticAnalyser::warn(const std::string& msg, SourceLocation loc) {
    diags_.push_back({Diagnostic::Level::Warning, msg, loc, {}});
}
void SemanticAnalyser::note(const std::string& msg, SourceLocation loc) {
    diags_.push_back({Diagnostic::Level::Note, msg, loc, {}});
}

bool SemanticAnalyser::expectType(TypeId got, TypeId want, SourceLocation loc) {
    if (got == TY_UNKNOWN || want == TY_UNKNOWN) return true;
    if (!reg_.unify(got, want) && !reg_.isSubtype(got, want)) {
        error("Type mismatch: expected '" + reg_.typeName(want) +
              "', got '" + reg_.typeName(got) + "'", loc);
        return false;
    }
    return true;
}

bool SemanticAnalyser::analyse(ast::Module& mod) {
    collectItems(mod);
    for (auto& item : mod.items) checkStmt(*item, globalScope_);
    return !hasErrors();
}

void SemanticAnalyser::collectItems(ast::Module& mod) {
    // First pass: register all top-level type/fn names
    for (auto& item : mod.items) {
        if (auto* fn = dynamic_cast<ast::FnItem*>(item.get())) {
            FnSig sig; sig.isAsync = fn->isAsync; sig.isUnsafe = fn->isUnsafe;
            for (auto& [pname, pty] : fn->params)
                sig.params.push_back(resolveType(*pty, globalScope_));
            if (fn->retTy) sig.ret = resolveType(*fn->retTy, globalScope_);
            else sig.ret = TY_UNIT;
            Symbol s; s.name = fn->name; s.kind = SymKind::Function;
            s.type = reg_.makeFn(std::move(sig)); s.isPublic = fn->isPublic; s.defLoc = fn->loc;
            globalScope_->define(fn->name, std::move(s));
        } else if (auto* st = dynamic_cast<ast::StructItem*>(item.get())) {
            Type ty; ty.kind = TypeKind::Struct; ty.name = st->name;
            TypeId id = reg_.registerType(std::move(ty));
            Symbol s; s.name = st->name; s.kind = SymKind::Type;
            s.type = id; s.isPublic = st->isPublic; s.defLoc = st->loc;
            globalScope_->define(st->name, std::move(s));
        } else if (auto* en = dynamic_cast<ast::EnumItem*>(item.get())) {
            Type ty; ty.kind = TypeKind::Enum; ty.name = en->name;
            TypeId id = reg_.registerType(std::move(ty));
            Symbol s; s.name = en->name; s.kind = SymKind::Type;
            s.type = id; s.isPublic = en->isPublic; s.defLoc = en->loc;
            globalScope_->define(en->name, std::move(s));
        } else if (auto* cl = dynamic_cast<ast::ClassItem*>(item.get())) {
            Type ty; ty.kind = TypeKind::Class; ty.name = cl->name;
            TypeId id = reg_.registerType(std::move(ty));
            Symbol s; s.name = cl->name; s.kind = SymKind::Type;
            s.type = id; s.isPublic = cl->isPublic; s.defLoc = cl->loc;
            globalScope_->define(cl->name, std::move(s));
        }
    }
}

TypeId SemanticAnalyser::resolveType(ast::TypeExpr& te, std::shared_ptr<Scope> scope) {
    if (auto* nt = dynamic_cast<ast::NamedType*>(&te)) {
        const Symbol* sym = scope->lookup(nt->name);
        if (!sym) { error("Undefined type: '" + nt->name + "'", te.loc); return TY_UNKNOWN; }
        if (!nt->args.empty()) {
            std::vector<TypeId> argIds;
            for (auto& a : nt->args) argIds.push_back(resolveType(*a, scope));
            return reg_.instantiate(sym->type, argIds);
        }
        return sym->type;
    }
    if (auto* pt = dynamic_cast<ast::PtrType*>(&te)) return reg_.makePtr(resolveType(*pt->inner, scope), pt->isMut);
    if (auto* rt = dynamic_cast<ast::RefType*>(&te)) return reg_.makeRef(resolveType(*rt->inner, scope), rt->isMut);
    if (auto* at = dynamic_cast<ast::ArrayType*>(&te)) return reg_.makeArray(resolveType(*at->elem, scope), at->size.value_or(0));
    if (auto* sl = dynamic_cast<ast::SliceType*>(&te)) return reg_.makeSlice(resolveType(*sl->elem, scope));
    if (auto* tt = dynamic_cast<ast::TupleType*>(&te)) {
        std::vector<TypeId> elems;
        for (auto& e : tt->elems) elems.push_back(resolveType(*e, scope));
        return reg_.makeTuple(std::move(elems));
    }
    if (dynamic_cast<ast::InferType*>(&te)) return reg_.freshInfer();
    if (auto* ft = dynamic_cast<ast::FnType*>(&te)) {
        FnSig sig;
        for (auto& p : ft->params) sig.params.push_back(resolveType(*p, scope));
        sig.ret = ft->ret ? resolveType(*ft->ret, scope) : TY_UNIT;
        return reg_.makeFn(std::move(sig));
    }
    if (auto* nl = dynamic_cast<ast::NullableType*>(&te))
        return reg_.makeOption(resolveType(*nl->inner, scope));
    return TY_UNKNOWN;
}

void SemanticAnalyser::checkStmt(ast::Stmt& s, std::shared_ptr<Scope> scope) {
    if (auto* fn = dynamic_cast<ast::FnItem*>(&s))     { checkFn(*fn, scope); return; }
    if (auto* st = dynamic_cast<ast::StructItem*>(&s)) { checkStruct(*st, scope); return; }
    if (auto* en = dynamic_cast<ast::EnumItem*>(&s))   { checkEnum(*en, scope); return; }
    if (auto* im = dynamic_cast<ast::ImplItem*>(&s))   { checkImpl(*im, scope); return; }
    if (auto* tr = dynamic_cast<ast::TraitItem*>(&s))  { checkTrait(*tr, scope); return; }
    if (auto* cl = dynamic_cast<ast::ClassItem*>(&s))  { checkClass(*cl, scope); return; }
    if (auto* es = dynamic_cast<ast::ExprStmt*>(&s))   { inferExpr(*es->expr, scope); return; }
    if (auto* ls = dynamic_cast<ast::LetStmt*>(&s)) {
        TypeId initTy = TY_UNKNOWN;
        if (ls->init) initTy = inferExpr(**ls->init, scope);
        TypeId declTy = ls->ty ? resolveType(**ls->ty, scope) : reg_.freshInfer();
        if (ls->init) expectType(initTy, declTy, ls->loc);
        TypeId finalTy = reg_.resolved(declTy == TY_UNKNOWN ? initTy : declTy);
        checkPattern(*ls->pat, finalTy, scope);
        return;
    }
    if (auto* co = dynamic_cast<ast::ConstItem*>(&s)) {
        TypeId valTy = inferExpr(*co->val, scope);
        TypeId declTy = resolveType(*co->ty, scope);
        expectType(valTy, declTy, co->loc);
        Symbol sym; sym.name = co->name; sym.type = declTy;
        sym.kind = SymKind::Const; sym.isPublic = co->isPublic; sym.defLoc = co->loc;
        scope->define(co->name, sym);
        return;
    }
}

void SemanticAnalyser::checkFn(ast::FnItem& fn, std::shared_ptr<Scope> scope) {
    if (!fn.body) return; // extern / declaration
    auto fnScope = std::make_shared<Scope>(scope);
    // Bind parameters
    for (auto& [pname, pty] : fn.params) {
        TypeId tid = resolveType(*pty, scope);
        Symbol s; s.name = pname; s.type = tid; s.kind = SymKind::Variable;
        s.isMut = false; s.scopeDepth = 1;
        fnScope->define(pname, s);
    }
    TypeId retTy = fn.retTy ? resolveType(*fn.retTy, scope) : TY_UNIT;
    for (auto& stmt : *fn.body) checkStmt(*stmt, fnScope);
}

void SemanticAnalyser::checkStruct(ast::StructItem& s, std::shared_ptr<Scope> scope) {
    Symbol* sym = scope->lookupLocal(s.name);
    if (!sym) return;
    Type* ty = reg_.getMut(sym->type);
    if (!ty) return;
    ty->fields.clear();
    for (auto& f : s.fields) {
        FieldInfo fi;
        fi.name     = f.name;
        fi.type     = resolveType(*f.ty, scope);
        fi.isMut    = f.isMut;
        fi.isPublic = f.isPublic;
        fi.offset   = 0; // will be computed below
        ty->fields.push_back(std::move(fi));
    }
    // Compute struct layout (alignment + offsets)
    size_t offset = 0, maxAlign = 1;
    for (auto& field : ty->fields) {
        const Type* ft = reg_.get(field.type);
        if (!ft) { field.offset = offset; continue; }
        size_t fa = ft->alignBytes > 0 ? ft->alignBytes : 1;
        // Align field to its own alignment requirement
        if (fa > 1) offset = (offset + fa - 1) & ~(fa - 1);
        field.offset = offset;
        offset += ft->sizeBytes;
        maxAlign = std::max(maxAlign, fa);
    }
    // Final struct size padded to its own alignment
    if (maxAlign > 1) offset = (offset + maxAlign - 1) & ~(maxAlign - 1);
    ty->sizeBytes  = offset;
    ty->alignBytes = maxAlign;
}

void SemanticAnalyser::checkEnum(ast::EnumItem& e, std::shared_ptr<Scope> scope) {
    // Register variant constructors as functions
    Symbol* sym = scope->lookup(e.name);
    if (!sym) return;
    for (auto& v : e.variants) {
        Symbol vsym; vsym.name = e.name + "::" + v.name; vsym.kind = SymKind::Function;
        vsym.type = sym->type; vsym.isPublic = true;
        scope->define(vsym.name, vsym);
    }
}

void SemanticAnalyser::checkImpl(ast::ImplItem& impl, std::shared_ptr<Scope> scope) {
    TypeId selfTy = resolveType(*impl.ty, scope);
    auto implScope = std::make_shared<Scope>(scope);
    Symbol selfSym; selfSym.name = "Self"; selfSym.type = selfTy; selfSym.kind = SymKind::Type;
    implScope->define("Self", selfSym);
    for (auto& m : impl.members) checkStmt(*m, implScope);
}

void SemanticAnalyser::checkTrait(ast::TraitItem& tr, std::shared_ptr<Scope> scope) {
    // Register trait in global type registry
    Type ty; ty.kind = TypeKind::Trait; ty.name = tr.name;
    TypeId id = reg_.registerType(std::move(ty));
    Symbol s; s.name = tr.name; s.type = id; s.kind = SymKind::Trait; s.isPublic = tr.isPublic;
    scope->define(tr.name, s);
}

void SemanticAnalyser::checkClass(ast::ClassItem& cls, std::shared_ptr<Scope> scope) {
    Symbol* sym = scope->lookupLocal(cls.name);
    if (!sym) return;
    auto clsScope = std::make_shared<Scope>(scope);
    Symbol selfSym; selfSym.name = "Self"; selfSym.type = sym->type; selfSym.kind = SymKind::Type;
    clsScope->define("Self", selfSym);
    for (auto& m : cls.members) checkStmt(*m, clsScope);
}

void SemanticAnalyser::checkInterface(ast::InterfaceItem& iface, std::shared_ptr<Scope> scope) {
    Type ty; ty.kind = TypeKind::Interface; ty.name = iface.name;
    TypeId id = reg_.registerType(std::move(ty));
    Symbol s; s.name = iface.name; s.type = id; s.kind = SymKind::Trait; s.isPublic = iface.isPublic;
    scope->define(iface.name, s);
}

// ─── Expression inference ─────────────────────────────────────────────────────
TypeId SemanticAnalyser::inferExpr(ast::Expr& e, std::shared_ptr<Scope> scope) {
    TypeId result = TY_UNKNOWN;

    if (auto* le = dynamic_cast<ast::LiteralExpr*>(&e))  result = inferLiteral(*le);
    else if (auto* ie = dynamic_cast<ast::IdentExpr*>(&e)) {
        Symbol* sym = scope->lookup(ie->name);
        if (!sym) { error("Undefined variable: '" + ie->name + "'", e.loc); result = TY_UNKNOWN; }
        else result = sym->type;
    }
    else if (auto* be = dynamic_cast<ast::BinaryExpr*>(&e)) result = inferBinary(*be, scope);
    else if (auto* ue = dynamic_cast<ast::UnaryExpr*>(&e))  result = inferUnary(*ue, scope);
    else if (auto* ce = dynamic_cast<ast::CallExpr*>(&e))   result = inferCall(*ce, scope);
    else if (auto* be2 = dynamic_cast<ast::BlockExpr*>(&e)) result = inferBlock(*be2, scope);
    else if (auto* ie2 = dynamic_cast<ast::IfExpr*>(&e))    result = inferIf(*ie2, scope);
    else if (auto* me = dynamic_cast<ast::MatchExpr*>(&e))  result = inferMatch(*me, scope);
    else if (auto* cl = dynamic_cast<ast::ClosureExpr*>(&e))result = inferClosure(*cl, scope);
    else if (auto* fe = dynamic_cast<ast::FieldExpr*>(&e))  result = inferField(*fe, scope);
    else if (auto* mne = dynamic_cast<ast::MethodExpr*>(&e))result = inferMethod(*mne, scope);
    else if (auto* ie3 = dynamic_cast<ast::IndexExpr*>(&e)) result = inferIndex(*ie3, scope);
    else if (auto* re = dynamic_cast<ast::ReturnExpr*>(&e)) {
        if (re->val) inferExpr(**re->val, scope);
        result = TY_NEVER;
    }
    else if (auto* ae = dynamic_cast<ast::AssignExpr*>(&e)) {
        TypeId lhsTy = inferExpr(*ae->lhs, scope);
        TypeId rhsTy = inferExpr(*ae->rhs, scope);
        expectType(rhsTy, lhsTy, e.loc);
        result = TY_UNIT;
    }
    else if (auto* aw = dynamic_cast<ast::AwaitExpr*>(&e)) {
        TypeId inner = inferExpr(*aw->expr, scope);
        // Resolve Future<T> or Task<T> to their inner type T
        const Type* ty = reg_.get(reg_.resolved(inner));
        if (ty && !ty->elems.empty()) result = ty->elems[0];
        else if (ty && ty->inner != TY_UNKNOWN) result = ty->inner;
        else result = inner; // already an unwrapped value type
    }
    else if (auto* arr = dynamic_cast<ast::ArrayExpr*>(&e)) {
        TypeId elemTy = TY_UNKNOWN;
        for (auto& el : arr->elems) {
            TypeId t = inferExpr(*el, scope);
            if (elemTy == TY_UNKNOWN) elemTy = t;
            else expectType(t, elemTy, e.loc);
        }
        result = reg_.makeArray(elemTy, arr->elems.size());
    }
    else if (auto* te = dynamic_cast<ast::TupleExpr*>(&e)) {
        std::vector<TypeId> elems;
        for (auto& el : te->elems) elems.push_back(inferExpr(*el, scope));
        result = reg_.makeTuple(std::move(elems));
    }
    else if (auto* cast = dynamic_cast<ast::CastExpr*>(&e)) {
        inferExpr(*cast->expr, scope);
        result = resolveType(*cast->ty, scope);
    }
    else if (dynamic_cast<ast::BreakExpr*>(&e)) { result = TY_NEVER; }
    else if (dynamic_cast<ast::ContinueExpr*>(&e)) { result = TY_NEVER; }

    exprTypes_[&e] = result;
    return result;
}

TypeId SemanticAnalyser::inferLiteral(ast::LiteralExpr& e) {
    switch (e.tok.kind) {
    case TokenKind::Integer:  return TY_I64;
    case TokenKind::Float:    return TY_F64;
    case TokenKind::String:   return TY_STR;
    case TokenKind::Char:     return TY_CHAR;
    case TokenKind::Kw_true:
    case TokenKind::Kw_false: return TY_BOOL;
    case TokenKind::Kw_nil:   return reg_.makeOption(TY_UNKNOWN);
    default:                  return TY_UNKNOWN;
    }
}

TypeId SemanticAnalyser::inferBinary(ast::BinaryExpr& e, std::shared_ptr<Scope> scope) {
    TypeId lhsTy = inferExpr(*e.lhs, scope);
    TypeId rhsTy = inferExpr(*e.rhs, scope);
    TokenKind op = e.op.kind;

    // Comparison → bool
    if (op == TokenKind::EqEq || op == TokenKind::BangEq ||
        op == TokenKind::Lt || op == TokenKind::Gt ||
        op == TokenKind::LtEq || op == TokenKind::GtEq) return TY_BOOL;
    // Logical → bool
    if (op == TokenKind::AmpAmp || op == TokenKind::PipePipe) { expectType(lhsTy, TY_BOOL, e.loc); return TY_BOOL; }
    // Arithmetic → wider numeric
    if (reg_.isNumeric(lhsTy) && reg_.isNumeric(rhsTy)) {
        if (!reg_.unify(lhsTy, rhsTy))
            warn("Implicit numeric conversion", e.loc);
        return reg_.typeSize(lhsTy) >= reg_.typeSize(rhsTy) ? lhsTy : rhsTy;
    }
    // String concat
    if (lhsTy == TY_STR && op == TokenKind::Plus) return TY_STR;
    expectType(rhsTy, lhsTy, e.loc);
    return lhsTy;
}

TypeId SemanticAnalyser::inferUnary(ast::UnaryExpr& e, std::shared_ptr<Scope> scope) {
    TypeId t = inferExpr(*e.expr, scope);
    if (e.op.kind == TokenKind::Bang) { expectType(t, TY_BOOL, e.loc); return TY_BOOL; }
    if (e.op.kind == TokenKind::Minus) { if (!reg_.isNumeric(t)) error("Unary '-' on non-numeric", e.loc); return t; }
    if (e.op.kind == TokenKind::Star) { // deref
        const Type* ty = reg_.get(t);
        if (ty && (ty->kind == TypeKind::Ptr || ty->kind == TypeKind::Ref)) return ty->inner;
        error("Cannot deref non-pointer type", e.loc); return TY_UNKNOWN;
    }
    if (e.op.kind == TokenKind::Ampersand) return reg_.makeRef(t, false);
    return t;
}

TypeId SemanticAnalyser::inferCall(ast::CallExpr& e, std::shared_ptr<Scope> scope) {
    TypeId calleeTy = inferExpr(*e.callee, scope);
    for (auto& arg : e.args) inferExpr(*arg, scope);
    const Type* ty = reg_.get(calleeTy);
    if (!ty) return TY_UNKNOWN;
    if (ty->kind == TypeKind::Fn && ty->sig) return ty->sig->ret;
    return TY_UNKNOWN;
}

TypeId SemanticAnalyser::inferBlock(ast::BlockExpr& e, std::shared_ptr<Scope> scope) {
    auto blkScope = std::make_shared<Scope>(scope);
    for (auto& s : e.stmts) checkStmt(*s, blkScope);
    if (e.tail) return inferExpr(**e.tail, blkScope);
    return TY_UNIT;
}

TypeId SemanticAnalyser::inferIf(ast::IfExpr& e, std::shared_ptr<Scope> scope) {
    TypeId condTy = inferExpr(*e.cond, scope);
    expectType(condTy, TY_BOOL, e.cond->loc);
    TypeId thenTy = inferExpr(*e.then, scope);
    if (e.els) {
        TypeId elsTy = inferExpr(**e.els, scope);
        if (!reg_.unify(thenTy, elsTy)) warn("If branches have different types", e.loc);
        return thenTy;
    }
    return TY_UNIT;
}

TypeId SemanticAnalyser::inferMatch(ast::MatchExpr& e, std::shared_ptr<Scope> scope) {
    TypeId scrutTy = inferExpr(*e.expr, scope);
    TypeId retTy = TY_UNKNOWN;
    for (auto& arm : e.arms) {
        auto armScope = std::make_shared<Scope>(scope);
        checkPattern(*arm.pat, scrutTy, armScope);
        if (arm.guard) inferExpr(**arm.guard, armScope);
        TypeId armTy = inferExpr(*arm.body, armScope);
        if (retTy == TY_UNKNOWN) retTy = armTy;
        else if (!reg_.unify(retTy, armTy)) warn("Match arm type mismatch", arm.body->loc);
    }
    return retTy;
}

TypeId SemanticAnalyser::inferClosure(ast::ClosureExpr& e, std::shared_ptr<Scope> scope) {
    auto clScope = std::make_shared<Scope>(scope);
    FnSig sig; sig.isAsync = e.isAsync;
    for (auto& [pname, pty] : e.params) {
        TypeId tid = resolveType(*pty, scope);
        sig.params.push_back(tid);
        Symbol s; s.name = pname; s.type = tid; s.kind = SymKind::Variable;
        clScope->define(pname, s);
    }
    sig.ret = e.retTy ? resolveType(**e.retTy, scope) : reg_.freshInfer();
    for (auto& s : e.body) checkStmt(*s, clScope);
    return reg_.makeFn(std::move(sig));
}

TypeId SemanticAnalyser::inferField(ast::FieldExpr& e, std::shared_ptr<Scope> scope) {
    TypeId objTy = inferExpr(*e.obj, scope);
    const Type* ty = reg_.get(reg_.resolved(objTy));
    if (!ty) return TY_UNKNOWN;
    // Deref if needed
    if (ty->kind == TypeKind::Ptr || ty->kind == TypeKind::Ref) ty = reg_.get(ty->inner);
    if (!ty) return TY_UNKNOWN;
    for (const auto& f : ty->fields) {
        if (f.name == e.field) return f.type;
    }
    error("No field '" + e.field + "' on type '" + ty->name + "'", e.loc);
    return TY_UNKNOWN;
}

TypeId SemanticAnalyser::inferMethod(ast::MethodExpr& e, std::shared_ptr<Scope> scope) {
    TypeId objTy = inferExpr(*e.obj, scope);
    for (auto& arg : e.args) inferExpr(*arg, scope);

    // Resolve the object type (deref through pointer/ref)
    TypeId resolved = reg_.resolved(objTy);
    const Type* ty = reg_.get(resolved);
    if (ty && (ty->kind == types::TypeKind::Ptr || ty->kind == types::TypeKind::Ref))
        ty = reg_.get(ty->inner);

    if (ty) {
        // Look for a method registered in this type's method table
        // (populated during impl block analysis)
        for (auto& field : ty->fields) {
            if (field.name == e.method) {
                const Type* ft = reg_.get(field.type);
                if (ft && ft->kind == TypeKind::Fn && ft->sig) return ft->sig->ret;
            }
        }
        // Search global scope for impl methods: TypeName::method_name
        std::string qualName = ty->name + "::" + e.method;
        const Symbol* sym = globalScope_->lookup(qualName);
        if (sym) {
            const Type* ft = reg_.get(sym->type);
            if (ft && ft->kind == TypeKind::Fn && ft->sig) return ft->sig->ret;
        }
    }
    // Fall back to a fresh inference variable — will be resolved during
    // method dispatch at runtime or narrowed by subsequent unification
    return reg_.freshInfer();
}

TypeId SemanticAnalyser::inferIndex(ast::IndexExpr& e, std::shared_ptr<Scope> scope) {
    TypeId objTy  = inferExpr(*e.obj, scope);
    TypeId idxTy  = inferExpr(*e.idx, scope);
    if (!reg_.isInteger(idxTy)) warn("Index must be integer", e.idx->loc);
    const Type* ty = reg_.get(reg_.resolved(objTy));
    if (ty && (ty->kind == TypeKind::Array || ty->kind == TypeKind::Slice ||
               ty->kind == TypeKind::Vec)) return ty->elemTy;
    return reg_.freshInfer();
}

void SemanticAnalyser::checkPattern(ast::Pattern& pat, TypeId expected, std::shared_ptr<Scope> scope) {
    if (auto* np = dynamic_cast<ast::NamePat*>(&pat)) {
        Symbol s; s.name = np->name; s.type = expected; s.kind = SymKind::Variable;
        s.isMut = np->isMut; s.scopeDepth = scope->symbols().size();
        if (!scope->define(np->name, s))
            warn("Variable '" + np->name + "' shadows existing binding", pat.loc);
    } else if (auto* tp = dynamic_cast<ast::TuplePat*>(&pat)) {
        const Type* ty = reg_.get(reg_.resolved(expected));
        if (ty && ty->kind == TypeKind::Tuple) {
            for (size_t i = 0; i < tp->pats.size() && i < ty->elems.size(); ++i)
                checkPattern(*tp->pats[i], ty->elems[i], scope);
        }
    }
    // Other patterns: wildcard, literal — no binding needed
}

} // namespace types
} // namespace fpp
