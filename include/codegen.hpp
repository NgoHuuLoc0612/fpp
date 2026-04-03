#pragma once
#include "ast.hpp"
#include "types.hpp"
#include "ir.hpp"
#include <memory>
#include <unordered_map>
#include <string>
#include <stack>
#include <vector>
#include <optional>

namespace fpp {
namespace codegen {

struct CodegenError : public std::runtime_error {
    SourceLocation loc;
    CodegenError(const std::string& msg, SourceLocation l)
        : std::runtime_error(msg), loc(std::move(l)) {}
};

// Maps AST names to IR registers / global refs
struct ValueEnv {
    struct Entry {
        ir::RegId   reg;         // Register holding the value (or pointer)
        bool        isPtr;       // Is the reg a pointer (L-value)?
        ir::IRType  type;
        bool        isMut;
        bool        isCaptured = false;
    };

    std::vector<std::unordered_map<std::string, Entry>> scopes;

    void   pushScope()  { scopes.push_back({}); }
    void   popScope()   { if (!scopes.empty()) scopes.pop_back(); }
    bool   define(const std::string& name, Entry e);
    Entry* lookup(const std::string& name);
    void   capture(const std::string& name);
};

// Loop context (for break/continue targets)
struct LoopContext {
    ir::BlockId continueTarget;
    ir::BlockId breakTarget;
    std::optional<ir::RegId> breakVal; // if break with value
    std::string label;
};

class CodeGenerator {
public:
    CodeGenerator(types::TypeRegistry& reg, types::SemanticAnalyser& sem);

    ir::IRModule generate(const ast::Module& mod);

private:
    types::TypeRegistry&    typeReg_;
    types::SemanticAnalyser& sem_;
    ir::IRModule            irMod_;
    std::unique_ptr<ir::IRBuilder> builder_;
    ValueEnv                env_;
    std::vector<LoopContext> loopStack_;

    // ── Top-level code generation ─────────────────────────────────────────
    void genModule(const ast::Module& mod);
    void genFn(const ast::FnItem& fn);
    void genStruct(const ast::StructItem& s);
    void genEnum(const ast::EnumItem& e);
    void genImpl(const ast::ImplItem& impl);
    void genClass(const ast::ClassItem& cls);
    void genConst(const ast::ConstItem& c);
    void genStatic(const ast::StaticItem& s);
    void genGlobalFnDecls(const ast::Module& mod);

    // ── Statement codegen ─────────────────────────────────────────────────
    void genStmt(const ast::Stmt& s);
    void genLet(const ast::LetStmt& let);

    // ── Expression codegen — returns a register ────────────────────────────
    ir::RegId genExpr(const ast::Expr& e, bool wantLval = false);
    ir::RegId genLiteral(const ast::LiteralExpr& e);
    ir::RegId genIdent(const ast::IdentExpr& e, bool wantLval);
    ir::RegId genBinary(const ast::BinaryExpr& e);
    ir::RegId genUnary(const ast::UnaryExpr& e);
    ir::RegId genAssign(const ast::AssignExpr& e);
    ir::RegId genCall(const ast::CallExpr& e);
    ir::RegId genIndex(const ast::IndexExpr& e, bool wantLval);
    ir::RegId genField(const ast::FieldExpr& e, bool wantLval);
    ir::RegId genMethod(const ast::MethodExpr& e);
    ir::RegId genBlock(const ast::BlockExpr& e);
    ir::RegId genIf(const ast::IfExpr& e);
    ir::RegId genMatch(const ast::MatchExpr& e);
    ir::RegId genWhile(const ast::WhileExpr& e);
    ir::RegId genFor(const ast::ForExpr& e);
    ir::RegId genClosure(const ast::ClosureExpr& e);
    ir::RegId genLambda(const ast::LambdaExpr& e);
    ir::RegId genStringInterp(const ast::StringInterp& e);
    ir::RegId genAwait(const ast::AwaitExpr& e);
    ir::RegId genSpawn(const ast::SpawnExpr& e);
    ir::RegId genYield(const ast::YieldExpr& e);
    ir::RegId genReturn(const ast::ReturnExpr& e);
    ir::RegId genBreak(const ast::BreakExpr& e);
    ir::RegId genContinue(const ast::ContinueExpr& e);
    ir::RegId genNew(const ast::NewExpr& e);
    ir::RegId genDelete(const ast::DeleteExpr& e);
    ir::RegId genArray(const ast::ArrayExpr& e);
    ir::RegId genTuple(const ast::TupleExpr& e);
    ir::RegId genMap(const ast::MapExpr& e);
    ir::RegId genRange(const ast::RangeExpr& e);
    ir::RegId genCast(const ast::CastExpr& e);
    ir::RegId genTry(const ast::TryExpr& e);

    // ── Pattern matching codegen ──────────────────────────────────────────
    // Returns block to jump to if match fails
    ir::BlockId genPattern(const ast::Pattern& pat, ir::RegId scrutinee,
                           ir::IRType scrutineeTy, ir::BlockId failBlock);
    void        genPatternBindings(const ast::Pattern& pat, ir::RegId scrutinee,
                                   ir::IRType scrutineeTy);

    // ── Type translation ──────────────────────────────────────────────────
    ir::IRType  toIRType(types::TypeId id);
    ir::IRType  toIRType(const ast::TypeExpr& te);
    ir::IRType  toIRTypeByName(const std::string& name);
    size_t      irTypeSize(ir::IRType ty);

    // ── Utilities ─────────────────────────────────────────────────────────
    ir::RegId   load(ir::RegId ptr, ir::IRType ty);
    void        store(ir::RegId val, ir::RegId ptr);
    ir::RegId   alloca(ir::IRType ty, const std::string& name = "");
    std::string mangle(const std::string& name, const std::string& ns = "");
    ir::RegId   coerce(ir::RegId val, ir::IRType from, ir::IRType to);
    void        pushLoop(LoopContext ctx) { loopStack_.push_back(ctx); }
    void        popLoop()               { loopStack_.pop_back(); }
    LoopContext& currentLoop()          { return loopStack_.back(); }
    bool        inLoop()               const { return !loopStack_.empty(); }
    ir::BlockId findLoopLabel(const std::string& lbl);

    // ── Intrinsic / std calls ─────────────────────────────────────────────
    ir::RegId   emitStdCall(const std::string& fn, std::vector<ir::RegId> args, ir::IRType retTy);
    ir::RegId   emitPrint(ir::RegId val, ir::IRType ty);
    ir::RegId   emitPanic(const std::string& msg, SourceLocation loc);
    ir::RegId   emitBoundsCheck(ir::RegId idx, ir::RegId len, SourceLocation loc);

    void error(const std::string& msg, SourceLocation loc);
};

// ─── Native backend: IR → platform-specific output ─────────────────────────
enum class OutputFormat { LLVMIR, NativeASM, Object, SharedLib, Exe, Bytecode };

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string name() const = 0;
    virtual bool emit(const ir::IRModule& mod, const std::string& outPath, OutputFormat fmt) = 0;
};

// ─── Bytecode backend (used by the treewalk VM) ────────────────────────────
class BytecodeBackend : public Backend {
public:
    std::string name() const override { return "fpp-bytecode"; }
    bool emit(const ir::IRModule& mod, const std::string& outPath, OutputFormat fmt) override;
    std::vector<uint8_t> compile(const ir::IRModule& mod);
};

// ─── Transpile-to-C++ backend ─────────────────────────────────────────────
class CppTranspileBackend : public Backend {
public:
    std::string name() const override { return "fpp-to-cpp"; }
    bool emit(const ir::IRModule& mod, const std::string& outPath, OutputFormat fmt) override;
    std::string transpile(const ir::IRModule& mod);
private:
    std::string emitFn(const ir::IRFunction& fn, const ir::IRModule& mod);
    std::string emitInst(const ir::Instruction& inst, const ir::IRFunction& fn);
    std::string irTypeToCpp(const ir::IRType& ty);
    std::string opcodeToOp(ir::Opcode op);
};

} // namespace codegen
} // namespace fpp
