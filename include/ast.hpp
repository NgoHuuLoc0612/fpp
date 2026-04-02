#pragma once
#include "lexer.hpp"
#include <memory>
#include <vector>
#include <optional>
#include <string>
#include <variant>
#include <functional>

namespace fpp {
namespace ast {

// ─── Forward declarations ──────────────────────────────────────────────────
struct Expr; struct Stmt; struct TypeExpr; struct Pattern;
using ExprPtr   = std::unique_ptr<Expr>;
using StmtPtr   = std::unique_ptr<Stmt>;
using TypePtr   = std::unique_ptr<TypeExpr>;
using PatPtr    = std::unique_ptr<Pattern>;
using ExprList  = std::vector<ExprPtr>;
using StmtList  = std::vector<StmtPtr>;
using ParamList = std::vector<std::pair<std::string, TypePtr>>;

// ─── Types ─────────────────────────────────────────────────────────────────
struct TypeExpr {
    SourceLocation loc;
    virtual ~TypeExpr() = default;
};

struct NamedType    : TypeExpr { std::string name; std::vector<TypePtr> args; };
struct PtrType      : TypeExpr { TypePtr inner; bool isMut; };
struct RefType      : TypeExpr { TypePtr inner; bool isMut; };
struct ArrayType    : TypeExpr { TypePtr elem; std::optional<size_t> size; };
struct SliceType    : TypeExpr { TypePtr elem; };
struct TupleType    : TypeExpr { std::vector<TypePtr> elems; };
struct FnType       : TypeExpr { std::vector<TypePtr> params; TypePtr ret; };
struct NullableType : TypeExpr { TypePtr inner; };
struct InferType    : TypeExpr {};

// ─── Patterns ──────────────────────────────────────────────────────────────
struct Pattern {
    SourceLocation loc;
    virtual ~Pattern() = default;
};

struct WildcardPat  : Pattern {};
struct NamePat      : Pattern { std::string name; bool isMut; };
struct LiteralPat   : Pattern { Token tok; };
struct TuplePat     : Pattern { std::vector<PatPtr> pats; };
struct StructPat    : Pattern { std::string name; std::vector<std::pair<std::string,PatPtr>> fields; };
struct EnumPat      : Pattern { std::string path; std::vector<PatPtr> inner; };
struct RangePat     : Pattern { PatPtr lo; PatPtr hi; bool inclusive; };
struct OrPat        : Pattern { std::vector<PatPtr> alts; };
struct GuardPat     : Pattern { PatPtr pat; ExprPtr guard; };

// ─── Expressions ───────────────────────────────────────────────────────────
struct Expr {
    SourceLocation loc;
    virtual ~Expr() = default;
};

struct LiteralExpr   : Expr { Token tok; };
struct IdentExpr     : Expr { std::string name; };
struct TupleExpr     : Expr { ExprList elems; };
struct ArrayExpr     : Expr { ExprList elems; };
struct MapExpr       : Expr { std::vector<std::pair<ExprPtr,ExprPtr>> pairs; };
struct SetExpr       : Expr { ExprList elems; };
struct UnaryExpr     : Expr { Token op; ExprPtr expr; };
struct BinaryExpr    : Expr { Token op; ExprPtr lhs; ExprPtr rhs; };
struct AssignExpr    : Expr { Token op; ExprPtr lhs; ExprPtr rhs; };
struct CallExpr      : Expr { ExprPtr callee; ExprList args; std::vector<std::string> kwNames; ExprList kwArgs; };
struct IndexExpr     : Expr { ExprPtr obj; ExprPtr idx; };
struct FieldExpr     : Expr { ExprPtr obj; std::string field; bool isSafe; };
struct MethodExpr    : Expr { ExprPtr obj; std::string method; ExprList args; bool isSafe; };
struct CastExpr      : Expr { ExprPtr expr; TypePtr ty; };
struct TypeofExpr    : Expr { ExprPtr expr; };
struct SizeofExpr    : Expr { TypePtr ty; };
struct RangeExpr     : Expr { ExprPtr lo; ExprPtr hi; bool inclusive; };
struct ClosureExpr   : Expr { ParamList params; std::optional<TypePtr> retTy; StmtList body; bool isAsync; };
struct BlockExpr     : Expr { StmtList stmts; std::optional<ExprPtr> tail; };
struct IfExpr        : Expr { ExprPtr cond; ExprPtr then; std::optional<ExprPtr> els; };
struct MatchExpr     : Expr {
    struct Arm { PatPtr pat; std::optional<ExprPtr> guard; ExprPtr body; };
    ExprPtr expr; std::vector<Arm> arms;
};
struct WhileExpr     : Expr { std::optional<ExprPtr> cond; ExprPtr body; std::optional<std::string> label; };
struct ForExpr       : Expr { PatPtr pat; ExprPtr iter; ExprPtr body; std::optional<std::string> label; };
struct ReturnExpr    : Expr { std::optional<ExprPtr> val; };
struct BreakExpr     : Expr { std::optional<std::string> label; std::optional<ExprPtr> val; };
struct ContinueExpr  : Expr { std::optional<std::string> label; };
struct YieldExpr     : Expr { std::optional<ExprPtr> val; };
struct AwaitExpr     : Expr { ExprPtr expr; };
struct SpawnExpr     : Expr { ExprPtr expr; };
struct TryExpr       : Expr { ExprPtr expr; };
struct NewExpr       : Expr { TypePtr ty; ExprList args; };
struct DeleteExpr    : Expr { ExprPtr ptr; };
struct StringInterp  : Expr { std::vector<std::variant<std::string, ExprPtr>> parts; };
struct LambdaExpr    : Expr { ParamList params; ExprPtr body; };

// ─── Statements ────────────────────────────────────────────────────────────
struct Stmt {
    SourceLocation loc;
    virtual ~Stmt() = default;
};

struct ExprStmt  : Stmt { ExprPtr expr; bool hasSemi; };
struct LetStmt   : Stmt { PatPtr pat; std::optional<TypePtr> ty; std::optional<ExprPtr> init; bool isMut; };
struct ItemStmt  : Stmt { StmtPtr item; };

// ─── Top-level Items ────────────────────────────────────────────────────────
struct Attribute {
    std::string name;
    std::vector<std::string> args;
};

struct GenericParam {
    std::string name;
    std::vector<TypePtr> bounds;
    std::optional<TypePtr> defaultTy;
};

struct FnItem : Stmt {
    std::string name;
    std::vector<GenericParam> generics;
    ParamList params;
    std::optional<std::string> selfParam; // "self", "&self", "&mut self"
    TypePtr retTy;
    std::optional<StmtList> body;
    bool isAsync, isExtern, isUnsafe, isInline, isPublic, isStatic;
    std::vector<Attribute> attrs;
};

struct FieldDef { std::string name; TypePtr ty; bool isMut; bool isPublic; };
struct VariantDef {
    std::string name;
    std::optional<std::vector<FieldDef>> fields;   // struct variant
    std::optional<std::vector<TypePtr>>  tuple;    // tuple variant
    std::optional<int64_t>               discrim;  // C-like
};

struct StructItem : Stmt {
    std::string name;
    std::vector<GenericParam> generics;
    std::vector<FieldDef> fields;
    std::vector<Attribute> attrs;
    bool isPublic;
};

struct EnumItem : Stmt {
    std::string name;
    std::vector<GenericParam> generics;
    std::vector<VariantDef> variants;
    std::vector<Attribute> attrs;
    bool isPublic;
};

struct TraitItem : Stmt {
    std::string name;
    std::vector<GenericParam> generics;
    std::vector<TypePtr> superTraits;
    StmtList members;
    bool isPublic;
};

struct ImplItem : Stmt {
    std::vector<GenericParam> generics;
    std::optional<TypePtr> trait;
    TypePtr ty;
    StmtList members;
    std::optional<ExprPtr> whereClause;
};

struct ClassItem : Stmt {
    std::string name;
    std::vector<GenericParam> generics;
    std::vector<TypePtr> bases;
    StmtList members;
    bool isPublic;
    bool isFinal;
    std::vector<Attribute> attrs;
};

struct InterfaceItem : Stmt {
    std::string name;
    std::vector<GenericParam> generics;
    std::vector<TypePtr> extends;
    StmtList members;
    bool isPublic;
};

struct TypeAliasItem : Stmt {
    std::string name;
    std::vector<GenericParam> generics;
    TypePtr ty;
    bool isPublic;
};

struct ModuleItem : Stmt {
    std::string name;
    std::optional<StmtList> body;
    bool isPublic;
};

struct ImportItem : Stmt {
    std::vector<std::string> path;
    std::variant<
        std::monostate,                               // use path::*
        std::string,                                  // use path as alias
        std::vector<std::pair<std::string,std::optional<std::string>>> // use path::{a, b as c}
    > spec;
};

struct ConstItem : Stmt {
    std::string name;
    TypePtr ty;
    ExprPtr val;
    bool isPublic;
};

struct StaticItem : Stmt {
    std::string name;
    TypePtr ty;
    ExprPtr val;
    bool isMut;
    bool isPublic;
};

struct MacroItem : Stmt {
    std::string name;
    std::vector<std::string> params;
    StmtList body;
};

// ─── Top-level compilation unit ────────────────────────────────────────────
struct Module {
    std::string name;
    std::string file;
    StmtList items;
};

} // namespace ast
} // namespace fpp
