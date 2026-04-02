#pragma once
#include "types.hpp"
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <functional>
#include <unordered_map>
#include <variant>
#include <optional>
#include <cstdint>

namespace fpp {
namespace ir {

// ─── IR Value System ───────────────────────────────────────────────────────
using RegId   = uint32_t;
using BlockId = uint32_t;
using FuncId  = uint32_t;
constexpr RegId   REG_NONE  = UINT32_MAX;
constexpr BlockId BLOCK_NONE = UINT32_MAX;

enum class Opcode : uint8_t {
    // Arithmetic
    Add, Sub, Mul, Div, Mod, Pow, Neg,
    // Bitwise
    BitAnd, BitOr, BitXor, BitNot, Shl, Shr, Ushr,
    // Comparison
    Eq, Ne, Lt, Le, Gt, Ge,
    // Logical
    And, Or, Not,
    // Memory
    Alloc, Free, Load, Store, MemCpy, MemMove, MemSet,
    // GEP (GetElementPtr)
    GEP, FieldPtr, IndexPtr,
    // Conversion
    Trunc, Ext, FpToInt, IntToFp, Bitcast, PtrToInt, IntToPtr,
    // Control Flow
    Jump, Branch, Switch, Return, Unreachable,
    // Function calls
    Call, IndirectCall, TailCall, InvokeCall, /* invoke = call + landingpad */
    // Exception handling
    LandingPad, Resume, Throw,
    // Concurrency
    AtomicLoad, AtomicStore, AtomicCmpXchg, AtomicRMW,
    Fence, SpawnTask, Await, YieldTask, ChanSend, ChanRecv,
    // Phi node
    Phi,
    // Special
    Select, ExtractValue, InsertValue,
    GlobalRef, ConstInt, ConstFp, ConstStr, ConstNull, ConstBool, ConstUnit,
    // Closures
    MakeClosure, ClosureCall, CaptureRef, CaptureCopy,
    // Intrinsics
    Intrinsic,
    // Debug
    DebugLoc, DebugValue,
};

struct IRType {
    types::TypeId typeId;
    bool          isPtr   = false;
    bool          isMut   = false;
    uint32_t      ptrDepth = 0;
};

struct PhiEdge {
    RegId   val;
    BlockId from;
};

struct SwitchCase {
    int64_t  val;
    BlockId  target;
};

struct Instruction {
    Opcode              op;
    RegId               dest;      // REG_NONE if void
    IRType              type;
    std::vector<RegId>  operands;
    // Extras per opcode
    int64_t             immInt  = 0;
    double              immFp   = 0.0;
    std::string         immStr;
    BlockId             target  = BLOCK_NONE;
    BlockId             altTarget = BLOCK_NONE;
    std::vector<SwitchCase> cases;
    std::vector<PhiEdge>    phiEdges;
    std::string         intrinsicName;
    FuncId              callee  = 0;
    std::string         globalName;
    SourceLocation      dbgLoc;
    uint32_t            fieldIdx = 0;
    bool                isTailCall = false;
    bool                isVolatile = false;
    uint32_t            atomicOrder = 0; // memory ordering
};

struct BasicBlock {
    BlockId                  id;
    std::string              label;
    std::vector<Instruction> insts;
    std::vector<BlockId>     preds;
    std::vector<BlockId>     succs;
    bool                     isEntry = false;
    bool                     isExit  = false;
};

struct IRParam {
    std::string name;
    IRType      type;
    RegId       reg;
    bool        byVal  = false;
    bool        isNoAlias = false;
};

struct IRFunction {
    FuncId                   id;
    std::string              name;
    std::string              mangledName;
    std::vector<IRParam>     params;
    IRType                   retType;
    std::vector<BasicBlock>  blocks;
    std::unordered_map<RegId, IRType> regTypes;
    bool                     isExtern  = false;
    bool                     isAsync   = false;
    bool                     isInline  = false;
    bool                     isNaked   = false;
    std::string              linkage;  // "internal", "external", "weak"
    std::string              callingConv; // "fpp", "c", "fast", "cold"
    size_t                   stackFrameSize = 0;
    RegId                    nextReg = 1;
};

struct IRGlobal {
    std::string  name;
    IRType       type;
    bool         isConst;
    bool         isMut;
    std::vector<uint8_t> initializer;
    std::string  linkage;
};

struct IRModule {
    std::string                  name;
    std::vector<IRFunction>      functions;
    std::vector<IRGlobal>        globals;
    std::vector<std::string>     externs;
    std::unordered_map<std::string, std::string> attributes;
    std::string                  targetTriple; // e.g. "x86_64-linux-gnu"
    std::string                  dataLayout;
};

// ─── IR Builder ────────────────────────────────────────────────────────────
class IRBuilder {
public:
    explicit IRBuilder(IRModule& mod);

    FuncId       beginFunction(const std::string& name, std::vector<IRParam> params, IRType ret);
    void         endFunction();
    BlockId      createBlock(const std::string& label = "");
    void         setBlock(BlockId b);
    BlockId      currentBlock() const { return curBlock_; }
    RegId        currentFunc()  const { return curFunc_; }

    // Instruction emitters
    RegId emit(Opcode op, IRType ty, std::vector<RegId> ops = {});
    RegId emitBinop(Opcode op, RegId lhs, RegId rhs, IRType ty);
    RegId emitUnop(Opcode op, RegId val, IRType ty);
    RegId emitLoad(RegId ptr, IRType ty);
    void  emitStore(RegId val, RegId ptr);
    RegId emitAlloc(IRType ty, std::optional<RegId> count = {});
    void  emitFree(RegId ptr);
    RegId emitCall(FuncId fn, std::vector<RegId> args, IRType retTy, bool isTail = false);
    RegId emitIndirectCall(RegId fn, std::vector<RegId> args, IRType retTy);
    void  emitReturn(std::optional<RegId> val);
    void  emitJump(BlockId target);
    void  emitBranch(RegId cond, BlockId trueB, BlockId falseB);
    void  emitSwitch(RegId val, BlockId defB, std::vector<SwitchCase> cases);
    RegId emitPhi(IRType ty, std::vector<PhiEdge> edges);
    RegId emitSelect(RegId cond, RegId a, RegId b, IRType ty);
    RegId emitGEP(RegId base, std::vector<RegId> indices, IRType elemTy);
    RegId emitFieldPtr(RegId base, uint32_t idx, IRType fieldTy);
    RegId emitCast(Opcode castOp, RegId val, IRType from, IRType to);
    RegId emitConstInt(int64_t v, IRType ty);
    RegId emitConstFp(double v, IRType ty);
    RegId emitConstStr(const std::string& s);
    RegId emitConstNull(IRType ty);
    RegId emitConstBool(bool b);
    RegId emitGlobalRef(const std::string& name, IRType ty);
    RegId emitIntrinsic(const std::string& name, std::vector<RegId> args, IRType ty);
    void  emitAtomicStore(RegId val, RegId ptr, uint32_t order);
    RegId emitAtomicLoad(RegId ptr, IRType ty, uint32_t order);
    RegId emitAtomicRMW(const std::string& op, RegId ptr, RegId val, uint32_t order);
    RegId emitCmpXchg(RegId ptr, RegId expected, RegId desired, uint32_t succ, uint32_t fail);
    void  emitFence(uint32_t order);
    RegId emitSpawn(RegId fn, std::vector<RegId> args);
    RegId emitAwait(RegId future);
    void  emitChanSend(RegId chan, RegId val);
    RegId emitChanRecv(RegId chan, IRType ty);

    void  setDebugLoc(SourceLocation loc);

    IRModule& module() { return mod_; }

private:
    IRModule&  mod_;
    FuncId     curFunc_  = 0;
    BlockId    curBlock_ = 0;
    SourceLocation dbgLoc_;

    IRFunction& func();
    BasicBlock& block();
    RegId       freshReg();
};

// ─── IR → Textual dump (for debugging / IR output) ────────────────────────
std::string dumpModule(const IRModule& mod);
std::string dumpFunction(const IRFunction& fn);
std::string dumpInstruction(const Instruction& inst, const IRFunction& fn);

// ─── Optimisation passes ───────────────────────────────────────────────────
class OptPass {
public:
    virtual ~OptPass() = default;
    virtual std::string name() const = 0;
    virtual bool run(IRFunction& fn, IRModule& mod) = 0;
};

class OptPipeline {
public:
    void addPass(std::unique_ptr<OptPass> p) { passes_.push_back(std::move(p)); }
    bool run(IRModule& mod);

    // Factory methods for standard passes
    static std::unique_ptr<OptPass> makeDeadCodeElim();
    static std::unique_ptr<OptPass> makeConstantFolding();
    static std::unique_ptr<OptPass> makeConstantPropagation();
    static std::unique_ptr<OptPass> makeCommonSubexprElim();
    static std::unique_ptr<OptPass> makeCopyPropagation();
    static std::unique_ptr<OptPass> makeInlining(size_t threshold = 50);
    static std::unique_ptr<OptPass> makeLoopInvariantMotion();
    static std::unique_ptr<OptPass> makeStrengthReduction();
    static std::unique_ptr<OptPass> makeTailCallOpt();
    static std::unique_ptr<OptPass> makeMemToReg();
    static std::unique_ptr<OptPass> makeBlockMerge();
    static std::unique_ptr<OptPass> makeJumpThreading();
    static std::unique_ptr<OptPass> makeInstCombine();
    static std::unique_ptr<OptPass> makeReassociation();
    static std::unique_ptr<OptPass> makeGlobalValueNumbering();
    static std::unique_ptr<OptPass> makeLoopUnroll(size_t maxTrip = 8);
    static std::unique_ptr<OptPass> makeVectorize();
    static std::unique_ptr<OptPass> makeEscapeAnalysis();

    // Preset pipelines
    static OptPipeline makeO0();
    static OptPipeline makeO1();
    static OptPipeline makeO2();
    static OptPipeline makeO3();
    static OptPipeline makeOs(); // size-optimised

private:
    std::vector<std::unique_ptr<OptPass>> passes_;
};

} // namespace ir
} // namespace fpp
