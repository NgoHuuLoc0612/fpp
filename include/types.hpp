#pragma once
#include "ast.hpp"
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>
#include <optional>
#include <set>
#include <functional>

namespace fpp {
namespace types {

// ─── Type IDs ──────────────────────────────────────────────────────────────
using TypeId = uint32_t;
constexpr TypeId TY_UNKNOWN  = 0;
constexpr TypeId TY_NEVER    = 1;
constexpr TypeId TY_UNIT     = 2;
constexpr TypeId TY_BOOL     = 3;
constexpr TypeId TY_I8       = 4;
constexpr TypeId TY_I16      = 5;
constexpr TypeId TY_I32      = 6;
constexpr TypeId TY_I64      = 7;
constexpr TypeId TY_I128     = 8;
constexpr TypeId TY_ISIZE    = 9;
constexpr TypeId TY_U8       = 10;
constexpr TypeId TY_U16      = 11;
constexpr TypeId TY_U32      = 12;
constexpr TypeId TY_U64      = 13;
constexpr TypeId TY_U128     = 14;
constexpr TypeId TY_USIZE    = 15;
constexpr TypeId TY_F32      = 16;
constexpr TypeId TY_F64      = 17;
constexpr TypeId TY_CHAR     = 18;
constexpr TypeId TY_STR      = 19;
constexpr TypeId TY_RAWPTR   = 20;
constexpr TypeId TY_FIRST_USER = 64;

// ─── Type Representation ───────────────────────────────────────────────────
enum class TypeKind {
    Primitive, Struct, Enum, Tuple, Array, Slice, Ptr, Ref,
    Fn, Closure, Trait, TraitObject, Generic, Infer, Never, Alias,
    Class, Interface, Option, Result, Vec, Map, Set, String,
};

struct TraitBound {
    TypeId trait;
    std::vector<TypeId> args;
};

struct TypeParam {
    std::string name;
    std::vector<TraitBound> bounds;
    std::optional<TypeId> defaultTy;
};

struct FieldInfo {
    std::string name;
    TypeId      type;
    bool        isMut;
    bool        isPublic;
    size_t      offset; // byte offset for layout
};

struct FnSig {
    std::vector<TypeId> params;
    TypeId              ret;
    bool                isVariadic;
    bool                isAsync;
    bool                isUnsafe;
    std::vector<TypeParam> generics;
};

struct Type {
    TypeId   id;
    TypeKind kind;
    std::string name;

    // Struct/Class/Enum fields
    std::vector<FieldInfo>  fields;
    // For Fn/Closure
    std::unique_ptr<FnSig>  sig;
    // Generic parameters
    std::vector<TypeParam>  params;
    // Instantiation args (for generic instances)
    std::vector<TypeId>     args;
    // For Alias
    TypeId inner = TY_UNKNOWN;
    // For Tuple
    std::vector<TypeId> elems;
    // For Array
    TypeId elemTy  = TY_UNKNOWN;
    size_t arrayLen = 0;
    // Implemented traits
    std::vector<TypeId> traits;
    // Size in bytes
    size_t sizeBytes = 0;
    size_t alignBytes = 1;
    // Pointer mutability
    bool isMut = false;
    // Visibility
    bool isPublic = false;

    // Explicit copy operations (needed because unique_ptr<FnSig> disables implicit copy)
    Type() = default;
    Type(Type&&) = default;
    Type& operator=(Type&&) = default;
    Type(const Type& o)
        : id(o.id), kind(o.kind), name(o.name), fields(o.fields)
        , params(o.params), args(o.args), inner(o.inner)
        , elems(o.elems), elemTy(o.elemTy), arrayLen(o.arrayLen)
        , traits(o.traits), sizeBytes(o.sizeBytes), alignBytes(o.alignBytes)
        , isMut(o.isMut), isPublic(o.isPublic)
    {
        if (o.sig) sig = std::make_unique<FnSig>(*o.sig);
    }
    Type& operator=(const Type& o) {
        if (this == &o) return *this;
        id = o.id; kind = o.kind; name = o.name; fields = o.fields;
        params = o.params; args = o.args; inner = o.inner;
        elems = o.elems; elemTy = o.elemTy; arrayLen = o.arrayLen;
        traits = o.traits; sizeBytes = o.sizeBytes; alignBytes = o.alignBytes;
        isMut = o.isMut; isPublic = o.isPublic;
        sig = o.sig ? std::make_unique<FnSig>(*o.sig) : nullptr;
        return *this;
    }
};

// ─── Symbol Table ──────────────────────────────────────────────────────────
enum class SymKind { Variable, Function, Type, Module, Trait, Const, Static };

struct Symbol {
    std::string name;
    TypeId      type;
    SymKind     kind;
    bool        isMut;
    bool        isPublic;
    SourceLocation defLoc;
    size_t      scopeDepth;
    std::optional<int64_t> constVal; // for compile-time constants
};

class Scope {
public:
    explicit Scope(std::shared_ptr<Scope> parent = nullptr)
        : parent_(std::move(parent)) {}

    bool        define(std::string name, Symbol sym);
    Symbol*     lookup(const std::string& name);
    const Symbol* lookup(const std::string& name) const;
    Symbol*     lookupLocal(const std::string& name);
    std::shared_ptr<Scope> parent() const { return parent_; }
    const std::unordered_map<std::string,Symbol>& symbols() const { return syms_; }

private:
    std::shared_ptr<Scope> parent_;
    std::unordered_map<std::string, Symbol> syms_;
};

// ─── Type Registry ─────────────────────────────────────────────────────────
class TypeRegistry {
public:
    TypeRegistry();

    TypeId         registerType(Type t);
    const Type*    get(TypeId id) const;
    Type*          getMut(TypeId id);
    TypeId         freshInfer();
    TypeId         instantiate(TypeId generic, const std::vector<TypeId>& args);
    bool           isSubtype(TypeId sub, TypeId sup) const;
    bool           unify(TypeId a, TypeId b);
    TypeId         resolved(TypeId id) const;
    TypeId         makePtr(TypeId inner, bool isMut);
    TypeId         makeRef(TypeId inner, bool isMut);
    TypeId         makeArray(TypeId elem, size_t len);
    TypeId         makeSlice(TypeId elem);
    TypeId         makeTuple(std::vector<TypeId> elems);
    TypeId         makeFn(FnSig sig);
    TypeId         makeOption(TypeId inner);
    TypeId         makeResult(TypeId ok, TypeId err);
    TypeId         makeVec(TypeId elem);
    TypeId         makeMap(TypeId key, TypeId val);
    std::string    typeName(TypeId id) const;
    bool           isPrimitive(TypeId id) const;
    bool           isInteger(TypeId id) const;
    bool           isFloat(TypeId id) const;
    bool           isNumeric(TypeId id) const;
    bool           isSigned(TypeId id) const;
    size_t         typeSize(TypeId id) const;

private:
    std::vector<Type>  types_;
    std::unordered_map<TypeId, TypeId> inferBindings_; // union-find
    TypeId             nextId_ = TY_FIRST_USER;
    TypeId             nextInfer_ = 100000;

    TypeId find(TypeId id) const;
    void   computeLayout(Type& t);
};

// ─── Semantic Analyser ─────────────────────────────────────────────────────
struct Diagnostic {
    enum class Level { Note, Warning, Error, Fatal };
    Level          level;
    std::string    message;
    SourceLocation loc;
    std::vector<std::pair<SourceLocation, std::string>> notes;
};

class SemanticAnalyser {
public:
    explicit SemanticAnalyser(TypeRegistry& reg);

    bool analyse(ast::Module& mod);
    const std::vector<Diagnostic>& diagnostics() const { return diags_; }
    bool hasErrors() const;

    // Exposed for testing
    TypeId inferExpr(ast::Expr& e, std::shared_ptr<Scope> scope);
    void   checkStmt(ast::Stmt& s, std::shared_ptr<Scope> scope);

private:
    TypeRegistry&          reg_;
    std::vector<Diagnostic> diags_;
    std::shared_ptr<Scope>  globalScope_;
    std::unordered_map<ast::Expr*, TypeId> exprTypes_;

    void error(const std::string& msg, SourceLocation loc);
    void warn(const std::string& msg, SourceLocation loc);
    void note(const std::string& msg, SourceLocation loc);

    // Item passes
    void collectItems(ast::Module& mod);
    void checkFn(ast::FnItem& fn, std::shared_ptr<Scope> scope);
    void checkStruct(ast::StructItem& s, std::shared_ptr<Scope> scope);
    void checkEnum(ast::EnumItem& e, std::shared_ptr<Scope> scope);
    void checkImpl(ast::ImplItem& impl, std::shared_ptr<Scope> scope);
    void checkTrait(ast::TraitItem& tr, std::shared_ptr<Scope> scope);
    void checkClass(ast::ClassItem& cls, std::shared_ptr<Scope> scope);
    void checkInterface(ast::InterfaceItem& iface, std::shared_ptr<Scope> scope);

    // Expr inference
    TypeId inferLiteral(ast::LiteralExpr& e);
    TypeId inferBinary(ast::BinaryExpr& e, std::shared_ptr<Scope> scope);
    TypeId inferUnary(ast::UnaryExpr& e, std::shared_ptr<Scope> scope);
    TypeId inferCall(ast::CallExpr& e, std::shared_ptr<Scope> scope);
    TypeId inferClosure(ast::ClosureExpr& e, std::shared_ptr<Scope> scope);
    TypeId inferIf(ast::IfExpr& e, std::shared_ptr<Scope> scope);
    TypeId inferMatch(ast::MatchExpr& e, std::shared_ptr<Scope> scope);
    TypeId inferBlock(ast::BlockExpr& e, std::shared_ptr<Scope> scope);
    TypeId inferIndex(ast::IndexExpr& e, std::shared_ptr<Scope> scope);
    TypeId inferField(ast::FieldExpr& e, std::shared_ptr<Scope> scope);
    TypeId inferMethod(ast::MethodExpr& e, std::shared_ptr<Scope> scope);

    // Pattern checking
    void   checkPattern(ast::Pattern& pat, TypeId expected, std::shared_ptr<Scope> scope);

    // Type resolution
    TypeId resolveType(ast::TypeExpr& te, std::shared_ptr<Scope> scope);
    bool   expectType(TypeId got, TypeId want, SourceLocation loc);

    // Borrow check state
    struct BorrowState {
        bool moved = false;
        bool borrowed = false;
        bool mutBorrowed = false;
        size_t borrowCount = 0;
    };
    std::unordered_map<std::string, BorrowState> borrowStates_;
    void checkBorrow(const std::string& name, bool isMut, SourceLocation loc);
    void releaseBorrow(const std::string& name);
};

} // namespace types
} // namespace fpp
