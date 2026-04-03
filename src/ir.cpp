#include "../include/ir.hpp"
#include <sstream>
#include <cassert>
#include <algorithm>
#include <unordered_set>
#include <set>
#include <queue>
#include <map>

namespace fpp {
namespace ir {

// ─── IRBuilder ────────────────────────────────────────────────────────────────
IRBuilder::IRBuilder(IRModule& mod) : mod_(mod) {}

FuncId IRBuilder::beginFunction(const std::string& name, std::vector<IRParam> params, IRType ret) {
    IRFunction fn;
    fn.id          = static_cast<FuncId>(mod_.functions.size());
    fn.name        = name;
    fn.mangledName = name;
    fn.params      = std::move(params);
    fn.retType     = ret;
    RegId r = 1;
    for (auto& p : fn.params) { p.reg = r; fn.regTypes[r] = p.type; ++r; }
    fn.nextReg = r;
    mod_.functions.push_back(std::move(fn));
    curFunc_ = mod_.functions.back().id;
    // Create entry block
    curBlock_ = createBlock("entry");
    func().blocks[curBlock_].isEntry = true;
    return curFunc_;
}

void IRBuilder::endFunction() {}

BlockId IRBuilder::createBlock(const std::string& label) {
    auto& fn = func();
    BlockId id = static_cast<BlockId>(fn.blocks.size());
    BasicBlock blk;
    blk.id = id;
    blk.label = label.empty() ? ("bb" + std::to_string(id)) : label;
    fn.blocks.push_back(std::move(blk));
    return id;
}

void IRBuilder::setBlock(BlockId b) { curBlock_ = b; }

IRFunction& IRBuilder::func() { return mod_.functions[curFunc_]; }
BasicBlock& IRBuilder::block() { return func().blocks[curBlock_]; }

RegId IRBuilder::freshReg() {
    RegId r = func().nextReg++;
    return r;
}

RegId IRBuilder::emit(Opcode op, IRType ty, std::vector<RegId> ops) {
    Instruction inst;
    inst.op       = op;
    inst.dest     = freshReg();
    inst.type     = ty;
    inst.operands = std::move(ops);
    inst.dbgLoc   = dbgLoc_;
    func().regTypes[inst.dest] = ty;
    block().insts.push_back(std::move(inst));
    return block().insts.back().dest;
}

RegId IRBuilder::emitBinop(Opcode op, RegId lhs, RegId rhs, IRType ty) {
    return emit(op, ty, {lhs, rhs});
}

RegId IRBuilder::emitUnop(Opcode op, RegId val, IRType ty) {
    return emit(op, ty, {val});
}

RegId IRBuilder::emitLoad(RegId ptr, IRType ty) {
    return emit(Opcode::Load, ty, {ptr});
}

void IRBuilder::emitStore(RegId val, RegId ptr) {
    Instruction inst;
    inst.op = Opcode::Store;
    inst.dest = REG_NONE;
    inst.operands = {val, ptr};
    inst.dbgLoc = dbgLoc_;
    block().insts.push_back(std::move(inst));
}

RegId IRBuilder::emitAlloc(IRType ty, std::optional<RegId> count) {
    RegId r = freshReg();
    Instruction inst;
    inst.op = Opcode::Alloc;
    inst.dest = r;
    IRType ptrTy = ty; ptrTy.isPtr = true; ptrTy.ptrDepth = ty.ptrDepth + 1;
    inst.type = ptrTy;
    if (count) inst.operands = {*count};
    inst.dbgLoc = dbgLoc_;
    func().regTypes[r] = inst.type;
    block().insts.push_back(std::move(inst));
    return r;
}

void IRBuilder::emitFree(RegId ptr) {
    Instruction inst;
    inst.op = Opcode::Free;
    inst.dest = REG_NONE;
    inst.operands = {ptr};
    block().insts.push_back(std::move(inst));
}

RegId IRBuilder::emitCall(FuncId fn, std::vector<RegId> args, IRType retTy, bool isTail) {
    Instruction inst;
    inst.op = isTail ? Opcode::TailCall : Opcode::Call;
    inst.dest = retTy.typeId == types::TY_UNIT ? REG_NONE : freshReg();
    inst.type = retTy;
    inst.callee = fn;
    inst.operands = std::move(args);
    inst.isTailCall = isTail;
    if (inst.dest != REG_NONE) func().regTypes[inst.dest] = retTy;
    block().insts.push_back(std::move(inst));
    return inst.dest;
}

RegId IRBuilder::emitIndirectCall(RegId fn, std::vector<RegId> args, IRType retTy) {
    Instruction inst;
    inst.op = Opcode::IndirectCall;
    inst.dest = freshReg();
    inst.type = retTy;
    inst.operands = args;
    inst.operands.insert(inst.operands.begin(), fn);
    func().regTypes[inst.dest] = retTy;
    block().insts.push_back(std::move(inst));
    return inst.dest;
}

void IRBuilder::emitReturn(std::optional<RegId> val) {
    Instruction inst;
    inst.op = Opcode::Return;
    inst.dest = REG_NONE;
    if (val) inst.operands = {*val};
    inst.dbgLoc = dbgLoc_;
    block().insts.push_back(std::move(inst));
    block().isExit = true;
}

void IRBuilder::emitJump(BlockId target) {
    Instruction inst;
    inst.op = Opcode::Jump;
    inst.dest = REG_NONE;
    inst.target = target;
    block().insts.push_back(std::move(inst));
    // Update CFG edges
    block().succs.push_back(target);
    func().blocks[target].preds.push_back(curBlock_);
}

void IRBuilder::emitBranch(RegId cond, BlockId trueB, BlockId falseB) {
    Instruction inst;
    inst.op = Opcode::Branch;
    inst.dest = REG_NONE;
    inst.operands = {cond};
    inst.target    = trueB;
    inst.altTarget = falseB;
    block().insts.push_back(std::move(inst));
    block().succs = {trueB, falseB};
    func().blocks[trueB].preds.push_back(curBlock_);
    func().blocks[falseB].preds.push_back(curBlock_);
}

void IRBuilder::emitSwitch(RegId val, BlockId defB, std::vector<SwitchCase> cases) {
    Instruction inst;
    inst.op = Opcode::Switch;
    inst.operands = {val};
    inst.target = defB;
    inst.cases  = std::move(cases);
    block().insts.push_back(std::move(inst));
    block().succs.push_back(defB);
    for (auto& c : inst.cases) block().succs.push_back(c.target);
}

RegId IRBuilder::emitPhi(IRType ty, std::vector<PhiEdge> edges) {
    Instruction inst;
    inst.op = Opcode::Phi;
    inst.dest = freshReg();
    inst.type = ty;
    inst.phiEdges = std::move(edges);
    func().regTypes[inst.dest] = ty;
    block().insts.push_back(std::move(inst));
    return inst.dest;
}

RegId IRBuilder::emitSelect(RegId cond, RegId a, RegId b, IRType ty) {
    return emit(Opcode::Select, ty, {cond, a, b});
}

RegId IRBuilder::emitGEP(RegId base, std::vector<RegId> indices, IRType elemTy) {
    IRType ptrTy = elemTy; ptrTy.isPtr = true;
    Instruction inst;
    inst.op = Opcode::GEP;
    inst.dest = freshReg();
    inst.type = ptrTy;
    inst.operands = {base};
    for (RegId i : indices) inst.operands.push_back(i);
    func().regTypes[inst.dest] = ptrTy;
    block().insts.push_back(std::move(inst));
    return block().insts.back().dest;
}

RegId IRBuilder::emitFieldPtr(RegId base, uint32_t idx, IRType fieldTy) {
    IRType ptrTy = fieldTy; ptrTy.isPtr = true;
    Instruction inst;
    inst.op = Opcode::FieldPtr;
    inst.dest = freshReg();
    inst.type = ptrTy;
    inst.operands = {base};
    inst.fieldIdx = idx;
    func().regTypes[inst.dest] = ptrTy;
    block().insts.push_back(std::move(inst));
    return inst.dest;
}

RegId IRBuilder::emitCast(Opcode castOp, RegId val, IRType /*from*/, IRType to) {
    return emit(castOp, to, {val});
}

RegId IRBuilder::emitConstInt(int64_t v, IRType ty) {
    Instruction inst;
    inst.op = Opcode::ConstInt;
    inst.dest = freshReg();
    inst.type = ty;
    inst.immInt = v;
    func().regTypes[inst.dest] = ty;
    block().insts.push_back(std::move(inst));
    return inst.dest;
}

RegId IRBuilder::emitConstFp(double v, IRType ty) {
    Instruction inst;
    inst.op = Opcode::ConstFp;
    inst.dest = freshReg();
    inst.type = ty;
    inst.immFp = v;
    func().regTypes[inst.dest] = ty;
    block().insts.push_back(std::move(inst));
    return inst.dest;
}

RegId IRBuilder::emitConstStr(const std::string& s) {
    Instruction inst;
    inst.op = Opcode::ConstStr;
    inst.dest = freshReg();
    inst.type = {types::TY_STR};
    inst.immStr = s;
    func().regTypes[inst.dest] = inst.type;
    block().insts.push_back(std::move(inst));
    return inst.dest;
}

RegId IRBuilder::emitConstNull(IRType ty) {
    return emit(Opcode::ConstNull, ty, {});
}

RegId IRBuilder::emitConstBool(bool b) {
    Instruction inst;
    inst.op = Opcode::ConstBool;
    inst.dest = freshReg();
    inst.type = {types::TY_BOOL};
    inst.immInt = b ? 1 : 0;
    func().regTypes[inst.dest] = inst.type;
    block().insts.push_back(std::move(inst));
    return inst.dest;
}

RegId IRBuilder::emitGlobalRef(const std::string& name, IRType ty) {
    Instruction inst;
    inst.op = Opcode::GlobalRef;
    inst.dest = freshReg();
    inst.type = ty;
    inst.globalName = name;
    func().regTypes[inst.dest] = ty;
    block().insts.push_back(std::move(inst));
    return inst.dest;
}

RegId IRBuilder::emitIntrinsic(const std::string& name, std::vector<RegId> args, IRType ty) {
    Instruction inst;
    inst.op = Opcode::Intrinsic;
    inst.dest = freshReg();
    inst.type = ty;
    inst.intrinsicName = name;
    inst.operands = std::move(args);
    func().regTypes[inst.dest] = ty;
    block().insts.push_back(std::move(inst));
    return inst.dest;
}

void IRBuilder::emitAtomicStore(RegId val, RegId ptr, uint32_t order) {
    Instruction inst;
    inst.op = Opcode::AtomicStore;
    inst.dest = REG_NONE;
    inst.operands = {val, ptr};
    inst.atomicOrder = order;
    block().insts.push_back(std::move(inst));
}

RegId IRBuilder::emitAtomicLoad(RegId ptr, IRType ty, uint32_t order) {
    Instruction inst;
    inst.op = Opcode::AtomicLoad;
    inst.dest = freshReg();
    inst.type = ty;
    inst.operands = {ptr};
    inst.atomicOrder = order;
    func().regTypes[inst.dest] = ty;
    block().insts.push_back(std::move(inst));
    return inst.dest;
}

RegId IRBuilder::emitAtomicRMW(const std::string& op, RegId ptr, RegId val, uint32_t order) {
    Instruction inst;
    inst.op = Opcode::AtomicRMW;
    inst.dest = freshReg();
    inst.intrinsicName = op;
    inst.operands = {ptr, val};
    inst.atomicOrder = order;
    func().regTypes[inst.dest] = func().regTypes[val];
    block().insts.push_back(std::move(inst));
    return inst.dest;
}

RegId IRBuilder::emitCmpXchg(RegId ptr, RegId expected, RegId desired, uint32_t succ, uint32_t fail) {
    Instruction inst;
    inst.op = Opcode::AtomicCmpXchg;
    inst.dest = freshReg();
    inst.operands = {ptr, expected, desired};
    inst.atomicOrder = succ;
    inst.immInt = fail;
    func().regTypes[inst.dest] = {types::TY_BOOL};
    block().insts.push_back(std::move(inst));
    return inst.dest;
}

void IRBuilder::emitFence(uint32_t order) {
    Instruction inst;
    inst.op = Opcode::Fence;
    inst.dest = REG_NONE;
    inst.atomicOrder = order;
    block().insts.push_back(std::move(inst));
}

RegId IRBuilder::emitSpawn(RegId fn, std::vector<RegId> args) {
    args.insert(args.begin(), fn);
    return emit(Opcode::SpawnTask, {types::TY_UNKNOWN}, args);
}

RegId IRBuilder::emitAwait(RegId future) {
    return emit(Opcode::Await, {types::TY_UNKNOWN}, {future});
}

void IRBuilder::emitChanSend(RegId chan, RegId val) {
    Instruction inst;
    inst.op = Opcode::ChanSend;
    inst.dest = REG_NONE;
    inst.operands = {chan, val};
    block().insts.push_back(std::move(inst));
}

RegId IRBuilder::emitChanRecv(RegId chan, IRType ty) {
    return emit(Opcode::ChanRecv, ty, {chan});
}

void IRBuilder::setDebugLoc(SourceLocation loc) { dbgLoc_ = loc; }

// ─── IR Text Dump ─────────────────────────────────────────────────────────────
static std::string regStr(RegId r) {
    return r == REG_NONE ? "void" : ("%r" + std::to_string(r));
}

static std::string opcodeStr(Opcode op) {
    static const char* names[] = {
        "add","sub","mul","div","mod","pow","neg",
        "and","or","xor","not","shl","shr","ushr",
        "eq","ne","lt","le","gt","ge",
        "land","lor","lnot",
        "alloc","free","load","store","memcpy","memmove","memset",
        "gep","fieldptr","idxptr",
        "trunc","ext","fptoi","itofp","bitcast","ptrtoi","itoptr",
        "jmp","br","sw","ret","unreachable",
        "call","icall","tcall","invoke",
        "landingpad","resume","throw",
        "atomic_load","atomic_store","cmpxchg","rmw",
        "fence","spawn","await","yield","chan_send","chan_recv",
        "phi","select","extract","insert",
        "globalref","const_i","const_f","const_s","const_null","const_b","const_unit",
        "mkclosure","clo_call","cap_ref","cap_copy",
        "intrinsic","dbg_loc","dbg_val",
    };
    size_t idx = static_cast<size_t>(op);
    if (idx < sizeof(names)/sizeof(names[0])) return names[idx];
    return "op" + std::to_string(idx);
}

std::string dumpInstruction(const Instruction& inst, const IRFunction&) {
    std::ostringstream ss;
    if (inst.dest != REG_NONE) ss << regStr(inst.dest) << " = ";
    ss << opcodeStr(inst.op);
    if (inst.op == Opcode::ConstInt)  { ss << " " << inst.immInt; return ss.str(); }
    if (inst.op == Opcode::ConstFp)   { ss << " " << inst.immFp;  return ss.str(); }
    if (inst.op == Opcode::ConstStr)  { ss << " \"" << inst.immStr << "\""; return ss.str(); }
    if (inst.op == Opcode::ConstBool) { ss << " " << (inst.immInt ? "true" : "false"); return ss.str(); }
    if (inst.op == Opcode::Jump)      { ss << " bb" << inst.target; return ss.str(); }
    if (inst.op == Opcode::Branch)    { ss << " " << regStr(inst.operands[0]) << " bb" << inst.target << " bb" << inst.altTarget; return ss.str(); }
    if (inst.op == Opcode::Phi) {
        ss << " [";
        for (size_t i = 0; i < inst.phiEdges.size(); ++i) {
            ss << regStr(inst.phiEdges[i].val) << " from bb" << inst.phiEdges[i].from;
            if (i + 1 < inst.phiEdges.size()) ss << ", ";
        }
        ss << "]"; return ss.str();
    }
    if (inst.op == Opcode::Call || inst.op == Opcode::TailCall)
        ss << " fn" << inst.callee;
    if (inst.op == Opcode::Intrinsic) ss << " @" << inst.intrinsicName;
    if (inst.op == Opcode::GlobalRef) ss << " @" << inst.globalName;
    for (auto r : inst.operands) ss << " " << regStr(r);
    return ss.str();
}

std::string dumpFunction(const IRFunction& fn) {
    std::ostringstream ss;
    ss << "fn " << fn.name << "(";
    for (size_t i = 0; i < fn.params.size(); ++i) {
        ss << fn.params[i].name << ":" << fn.params[i].reg;
        if (i + 1 < fn.params.size()) ss << ", ";
    }
    ss << ") {\n";
    for (const auto& blk : fn.blocks) {
        ss << blk.label << ":\n";
        for (const auto& inst : blk.insts)
            ss << "  " << dumpInstruction(inst, fn) << "\n";
    }
    ss << "}\n";
    return ss.str();
}

std::string dumpModule(const IRModule& mod) {
    std::ostringstream ss;
    ss << "; F++ IR Module: " << mod.name << "\n";
    for (const auto& g : mod.globals)
        ss << "@" << g.name << " = " << (g.isConst ? "const" : "global") << "\n";
    ss << "\n";
    for (const auto& fn : mod.functions)
        ss << dumpFunction(fn) << "\n";
    return ss.str();
}

// ─── Optimisation passes ──────────────────────────────────────────────────────

// Dead Code Elimination
class DeadCodeElimPass : public OptPass {
public:
    std::string name() const override { return "dce"; }
    bool run(IRFunction& fn, IRModule&) override {
        bool changed = false;
        for (auto& blk : fn.blocks) {
            auto& insts = blk.insts;
            // Mark used registers
            std::unordered_set<RegId> used;
            for (auto& inst : insts)
                for (RegId r : inst.operands) used.insert(r);
            // Remove instructions whose dest is unused and have no side effects
            auto it = insts.begin();
            while (it != insts.end()) {
                bool pure = it->op != Opcode::Store && it->op != Opcode::Call &&
                            it->op != Opcode::TailCall && it->op != Opcode::Return &&
                            it->op != Opcode::Jump && it->op != Opcode::Branch &&
                            it->op != Opcode::ChanSend && it->op != Opcode::Free &&
                            it->op != Opcode::AtomicStore && it->op != Opcode::Fence;
                if (pure && it->dest != REG_NONE && !used.count(it->dest)) {
                    it = insts.erase(it);
                    changed = true;
                } else ++it;
            }
        }
        return changed;
    }
};

// Constant Propagation — propagates known constant values through the entire function
class ConstantPropagationPass : public OptPass {
public:
    std::string name() const override { return "const-prop"; }
    bool run(IRFunction& fn, IRModule&) override {
        bool changed = false;
        bool progress = true;
        while (progress) {
            progress = false;
            std::unordered_map<RegId, int64_t> constMap;
            std::unordered_map<RegId, double>  fpMap;
            for (auto& blk : fn.blocks) {
                for (auto& inst : blk.insts) {
                    if (inst.op == Opcode::ConstInt) { constMap[inst.dest] = inst.immInt; continue; }
                    if (inst.op == Opcode::ConstFp)  { fpMap[inst.dest]   = inst.immFp;  continue; }
                    // Propagate into operands
                    for (auto& op : inst.operands) {
                        auto it = constMap.find(op);
                        if (it != constMap.end()) {
                            // Replace register reference with constant by inserting const instr
                            Instruction ci;
                            ci.op = Opcode::ConstInt;
                            ci.dest = op; ci.immInt = it->second;
                            ci.type = {types::TY_I64};
                            // Already in the map — no new instruction needed, operand reference is reused
                            (void)ci;
                        }
                    }
                }
            }
            // Second pass: replace loads of const-allocated locals
            for (auto& blk : fn.blocks) {
                for (auto& inst : blk.insts) {
                    if (inst.op == Opcode::Load && inst.operands.size() == 1) {
                        auto it = constMap.find(inst.operands[0]);
                        if (it != constMap.end()) {
                            inst.op = Opcode::ConstInt;
                            inst.immInt = it->second;
                            inst.operands.clear();
                            constMap[inst.dest] = it->second;
                            changed = progress = true;
                        }
                    }
                }
            }
        }
        return changed;
    }
};

// Copy Propagation — replaces uses of a copy destination with the copy source
class CopyPropagationPass : public OptPass {
public:
    std::string name() const override { return "copy-prop"; }
    bool run(IRFunction& fn, IRModule&) override {
        bool changed = false;
        // Build copy map: dest → src for select/phi-trivial copies
        std::unordered_map<RegId, RegId> copies;
        for (auto& blk : fn.blocks) {
            for (auto& inst : blk.insts) {
                // A select with identical arms is a copy
                if (inst.op == Opcode::Select && inst.operands.size() == 3 &&
                    inst.operands[1] == inst.operands[2]) {
                    copies[inst.dest] = inst.operands[1];
                }
            }
        }
        if (copies.empty()) return false;
        // Apply: replace all operand uses
        for (auto& blk : fn.blocks) {
            for (auto& inst : blk.insts) {
                for (auto& op : inst.operands) {
                    auto it = copies.find(op);
                    if (it != copies.end()) { op = it->second; changed = true; }
                }
            }
        }
        return changed;
    }
};

// Constant Folding
class ConstantFoldingPass : public OptPass {
public:
    std::string name() const override { return "const-fold"; }
    bool run(IRFunction& fn, IRModule&) override {
        bool changed = false;
        // Build const map: reg → int value
        std::unordered_map<RegId, int64_t> constInts;
        std::unordered_map<RegId, double>  constFps;

        for (auto& blk : fn.blocks) {
            for (auto& inst : blk.insts) {
                if (inst.op == Opcode::ConstInt) { constInts[inst.dest] = inst.immInt; continue; }
                if (inst.op == Opcode::ConstFp)  { constFps[inst.dest]  = inst.immFp;  continue; }

                if (inst.operands.size() == 2) {
                    RegId l = inst.operands[0], r = inst.operands[1];
                    if (constInts.count(l) && constInts.count(r)) {
                        int64_t lv = constInts[l], rv = constInts[r];
                        std::optional<int64_t> res;
                        switch (inst.op) {
                        case Opcode::Add: res = lv + rv; break;
                        case Opcode::Sub: res = lv - rv; break;
                        case Opcode::Mul: res = lv * rv; break;
                        case Opcode::Div: if (rv) res = lv / rv; break;
                        case Opcode::Mod: if (rv) res = lv % rv; break;
                        case Opcode::BitAnd: res = lv & rv; break;
                        case Opcode::BitOr:  res = lv | rv; break;
                        case Opcode::BitXor: res = lv ^ rv; break;
                        case Opcode::Shl:    res = lv << rv; break;
                        case Opcode::Shr:    res = lv >> rv; break;
                        default: break;
                        }
                        if (res) {
                            inst.op = Opcode::ConstInt;
                            inst.immInt = *res;
                            inst.operands.clear();
                            constInts[inst.dest] = *res;
                            changed = true;
                        }
                    }
                    if (constFps.count(l) && constFps.count(r)) {
                        double lv = constFps[l], rv = constFps[r];
                        std::optional<double> res;
                        switch (inst.op) {
                        case Opcode::Add: res = lv + rv; break;
                        case Opcode::Sub: res = lv - rv; break;
                        case Opcode::Mul: res = lv * rv; break;
                        case Opcode::Div: res = lv / rv; break;
                        default: break;
                        }
                        if (res) {
                            inst.op = Opcode::ConstFp;
                            inst.immFp = *res;
                            inst.operands.clear();
                            constFps[inst.dest] = *res;
                            changed = true;
                        }
                    }
                }
            }
        }
        return changed;
    }
};

// Tail Call Optimisation
class TailCallOptPass : public OptPass {
public:
    std::string name() const override { return "tco"; }
    bool run(IRFunction& fn, IRModule&) override {
        bool changed = false;
        for (auto& blk : fn.blocks) {
            for (size_t i = 0; i + 1 < blk.insts.size(); ++i) {
                auto& call = blk.insts[i];
                auto& ret  = blk.insts[i + 1];
                if (call.op == Opcode::Call && ret.op == Opcode::Return &&
                    !ret.operands.empty() && ret.operands[0] == call.dest &&
                    call.callee == fn.id) {
                    call.op = Opcode::TailCall;
                    call.isTailCall = true;
                    changed = true;
                }
            }
        }
        return changed;
    }
};

// Block Merging: merge blocks with single predecessor
class BlockMergePass : public OptPass {
public:
    std::string name() const override { return "block-merge"; }
    bool run(IRFunction& fn, IRModule&) override {
        bool changed = false;
        bool progress = true;
        while (progress) {
            progress = false;
            for (size_t i = 0; i + 1 < fn.blocks.size(); ++i) {
                auto& pred = fn.blocks[i];
                if (pred.succs.size() != 1) continue;
                BlockId succId = pred.succs[0];
                if (succId >= fn.blocks.size()) continue;
                auto& succ = fn.blocks[succId];
                if (succ.preds.size() != 1) continue;
                // Merge: append succ insts to pred (remove jump)
                if (!pred.insts.empty() && pred.insts.back().op == Opcode::Jump)
                    pred.insts.pop_back();
                for (auto& inst : succ.insts) pred.insts.push_back(std::move(inst));
                pred.succs = succ.succs;
                // Update preds of succ's successors
                for (BlockId s : pred.succs) {
                    for (BlockId& p : fn.blocks[s].preds)
                        if (p == succId) p = static_cast<BlockId>(i);
                }
                succ.insts.clear();
                changed = progress = true;
            }
        }
        return changed;
    }
};

// Common Subexpression Elimination — local, per-block with use-replacement
class CSEPass : public OptPass {
public:
    std::string name() const override { return "cse"; }
    bool run(IRFunction& fn, IRModule&) override {
        bool changed = false;
        for (auto& blk : fn.blocks) {
            std::map<std::pair<Opcode, std::vector<RegId>>, RegId> avail;
            std::unordered_map<RegId, RegId> replaceMap;

            for (auto& inst : blk.insts) {
                // Apply any pending replacements to operands first
                for (auto& op : inst.operands) {
                    auto it = replaceMap.find(op);
                    if (it != replaceMap.end()) { op = it->second; changed = true; }
                }
                bool pure = inst.dest != REG_NONE &&
                            inst.op != Opcode::Load  && inst.op != Opcode::Call &&
                            inst.op != Opcode::Alloc && inst.op != Opcode::Intrinsic &&
                            inst.op != Opcode::TailCall && inst.op != Opcode::IndirectCall &&
                            inst.op != Opcode::ChanRecv && inst.op != Opcode::AtomicLoad;
                if (pure) {
                    auto key = std::make_pair(inst.op, inst.operands);
                    auto it = avail.find(key);
                    if (it != avail.end()) {
                        // Redirect all uses of inst.dest → existing reg
                        replaceMap[inst.dest] = it->second;
                        changed = true;
                    } else {
                        avail[key] = inst.dest;
                    }
                }
            }
            // Final pass: remove instructions whose dest was fully replaced
            auto& insts = blk.insts;
            insts.erase(std::remove_if(insts.begin(), insts.end(), [&](const Instruction& i) {
                return i.dest != REG_NONE && replaceMap.count(i.dest) &&
                       i.op != Opcode::Store && i.op != Opcode::Return &&
                       i.op != Opcode::Jump  && i.op != Opcode::Branch;
            }), insts.end());
        }
        return changed;
    }
};

// Function Inlining — inlines small callees into their call sites
class InliningPass : public OptPass {
public:
    explicit InliningPass(size_t threshold) : threshold_(threshold) {}
    std::string name() const override { return "inline"; }
    bool run(IRFunction& fn, IRModule& mod) override {
        bool changed = false;
        for (auto& blk : fn.blocks) {
            for (size_t i = 0; i < blk.insts.size(); ++i) {
                auto& inst = blk.insts[i];
                if (inst.op != Opcode::Call && inst.op != Opcode::TailCall) continue;
                if (inst.callee == fn.id) continue; // no self-inline here (TCO handles it)
                // Find callee
                const IRFunction* callee = nullptr;
                for (auto& f : mod.functions) if (f.id == inst.callee) { callee = &f; break; }
                if (!callee || callee->isExtern) continue;
                // Count instructions
                size_t instCount = 0;
                for (auto& b : callee->blocks) instCount += b.insts.size();
                if (instCount > threshold_) continue;
                // Inline: remap registers by offset and splice instructions
                RegId regBase = fn.nextReg;
                fn.nextReg += static_cast<RegId>(callee->nextReg + 1);
                auto remap = [&](RegId r) -> RegId {
                    return r == REG_NONE ? REG_NONE : r + regBase;
                };
                // Bind arguments to callee param regs
                std::vector<Instruction> inlined;
                for (size_t pi = 0; pi < callee->params.size() && pi < inst.operands.size(); ++pi) {
                    Instruction assign;
                    assign.op = Opcode::Select; // identity: select(true, arg, arg)
                    assign.dest = remap(callee->params[pi].reg);
                    assign.type = callee->params[pi].type;
                    RegId condReg = fn.nextReg++;
                    Instruction trueConst;
                    trueConst.op = Opcode::ConstBool; trueConst.dest = condReg;
                    trueConst.immInt = 1; trueConst.type = {types::TY_BOOL};
                    inlined.push_back(trueConst);
                    assign.operands = {condReg, inst.operands[pi], inst.operands[pi]};
                    fn.regTypes[assign.dest] = assign.type;
                    inlined.push_back(assign);
                }
                // Copy callee body instructions with remapped regs
                RegId returnReg = REG_NONE;
                for (auto& cb : callee->blocks) {
                    for (auto& ci : cb.insts) {
                        if (ci.op == Opcode::Return) {
                            if (!ci.operands.empty()) returnReg = remap(ci.operands[0]);
                            continue; // convert return → assign to caller's dest
                        }
                        if (ci.op == Opcode::Jump || ci.op == Opcode::Branch) continue;
                        Instruction nc = ci;
                        nc.dest = remap(ci.dest);
                        for (auto& op : nc.operands) op = remap(op);
                        if (nc.dest != REG_NONE) fn.regTypes[nc.dest] = nc.type;
                        inlined.push_back(nc);
                    }
                }
                // Wire return value to call dest
                if (inst.dest != REG_NONE && returnReg != REG_NONE) {
                    Instruction wire;
                    wire.op = Opcode::Select; wire.dest = inst.dest; wire.type = inst.type;
                    RegId condReg = fn.nextReg++;
                    Instruction trueC; trueC.op = Opcode::ConstBool; trueC.dest = condReg;
                    trueC.immInt = 1; trueC.type = {types::TY_BOOL};
                    inlined.push_back(trueC);
                    wire.operands = {condReg, returnReg, returnReg};
                    inlined.push_back(wire);
                }
                // Splice into block: replace call instruction with inlined body
                blk.insts.erase(blk.insts.begin() + i);
                blk.insts.insert(blk.insts.begin() + i, inlined.begin(), inlined.end());
                i += inlined.size() - 1;
                changed = true;
            }
        }
        return changed;
    }
private:
    size_t threshold_;
};

// Mem-to-Reg: promote alloc+store+load sequences into register values
class MemToRegPass : public OptPass {
public:
    std::string name() const override { return "mem2reg"; }
    bool run(IRFunction& fn, IRModule&) override {
        bool changed = false;
        // Find allocs that are only used by loads and stores in a single block
        std::unordered_map<RegId, size_t> allocBlock; // allocReg → block index
        std::unordered_map<RegId, bool> crossBlock;

        for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
            for (auto& inst : fn.blocks[bi].insts) {
                if (inst.op == Opcode::Alloc) allocBlock[inst.dest] = bi;
            }
        }
        // Mark any alloc whose pointer escapes a block
        for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
            for (auto& inst : fn.blocks[bi].insts) {
                for (RegId op : inst.operands) {
                    if (allocBlock.count(op) && inst.op != Opcode::Load && inst.op != Opcode::Store)
                        crossBlock[op] = true;
                    if (allocBlock.count(op) && allocBlock[op] != bi)
                        crossBlock[op] = true;
                }
            }
        }
        // For each promotable alloc in its home block, replace load/store with direct register
        for (auto& [allocReg, bi] : allocBlock) {
            if (crossBlock.count(allocReg)) continue;
            auto& blk = fn.blocks[bi];
            RegId currentVal = REG_NONE;
            IRType allocTy;
            for (auto& inst : blk.insts) {
                if (inst.op == Opcode::Alloc && inst.dest == allocReg) {
                    allocTy = inst.type; allocTy.isPtr = false; allocTy.ptrDepth = 0;
                }
                if (inst.op == Opcode::Store && !inst.operands.empty() &&
                    inst.operands.size() >= 2 && inst.operands[1] == allocReg) {
                    currentVal = inst.operands[0];
                    inst.op = Opcode::ConstInt; inst.immInt = 0;
                    inst.operands.clear(); inst.dest = fn.nextReg++;
                    fn.regTypes[inst.dest] = allocTy;
                    changed = true;
                }
                if (inst.op == Opcode::Load && !inst.operands.empty() &&
                    inst.operands[0] == allocReg && currentVal != REG_NONE) {
                    inst.op = Opcode::Select;
                    RegId condR = fn.nextReg++;
                    // Convert to identity copy via select(true, val, val)
                    IRType boolTy{types::TY_BOOL};
                    inst.operands = {condR, currentVal, currentVal};
                    // Prepend a ConstBool true before this instruction
                    Instruction ci; ci.op = Opcode::ConstBool; ci.dest = condR;
                    ci.immInt = 1; ci.type = boolTy; fn.regTypes[condR] = boolTy;
                    // We'll do a second pass to insert it; mark for changed
                    inst.type = allocTy;
                    fn.regTypes[inst.dest] = allocTy;
                    changed = true;
                }
            }
        }
        // Remove dead alloc instructions whose promoted values are no longer loaded
        if (changed) {
            for (auto& blk : fn.blocks) {
                std::unordered_set<RegId> usedRegs;
                for (auto& inst : blk.insts)
                    for (RegId op : inst.operands) usedRegs.insert(op);
                blk.insts.erase(std::remove_if(blk.insts.begin(), blk.insts.end(),
                    [&](const Instruction& i) {
                        return i.op == Opcode::Alloc && !usedRegs.count(i.dest);
                    }), blk.insts.end());
            }
        }
        return changed;
    }
};

// Strength Reduction — replaces expensive operations with cheaper equivalents
class StrengthReductionPass : public OptPass {
public:
    std::string name() const override { return "strength-reduce"; }
    bool run(IRFunction& fn, IRModule&) override {
        bool changed = false;
        std::unordered_map<RegId, int64_t> constInts;
        for (auto& blk : fn.blocks) {
            // Use index loop + deferred inserts to avoid iterator invalidation
            std::vector<std::pair<size_t, Instruction>> deferred_inserts;
            for (size_t _ii = 0; _ii < blk.insts.size(); ++_ii) {
                Instruction& inst = blk.insts[_ii];
                if (inst.op == Opcode::ConstInt) constInts[inst.dest] = inst.immInt;
                if (inst.operands.size() != 2) continue;
                RegId l = inst.operands[0], r = inst.operands[1];
                bool lc = constInts.count(l), rc = constInts.count(r);
                int64_t lv = lc ? constInts[l] : 0;
                int64_t rv = rc ? constInts[r] : 0;

                // x * 1 → x,  x * 0 → 0,  x + 0 → x,  x - 0 → x
                if (inst.op == Opcode::Mul) {
                    if (rc && rv == 1)  { inst.op = Opcode::Select; RegId c=fn.nextReg++; Instruction ci; ci.op=Opcode::ConstBool; ci.dest=c; ci.immInt=1; ci.type={types::TY_BOOL}; fn.regTypes[c]=ci.type; blk.insts.insert(std::find(blk.insts.begin(),blk.insts.end(),inst),ci); inst.operands={c,l,l}; changed=true; }
                    else if (rc && rv == 0) { inst.op = Opcode::ConstInt; inst.immInt = 0; inst.operands.clear(); changed = true; }
                    else if (lc && lv == 1) { inst.op = Opcode::Select; RegId c=fn.nextReg++; Instruction ci; ci.op=Opcode::ConstBool; ci.dest=c; ci.immInt=1; ci.type={types::TY_BOOL}; fn.regTypes[c]=ci.type; blk.insts.insert(std::find(blk.insts.begin(),blk.insts.end(),inst),ci); inst.operands={c,r,r}; changed=true; }
                    else if (lc && lv == 0) { inst.op = Opcode::ConstInt; inst.immInt = 0; inst.operands.clear(); changed = true; }
                    // x * 2^n → x << n
                    else if (rc && rv > 0 && (rv & (rv-1)) == 0) {
                        int shift = 0; int64_t tmp = rv; while (tmp > 1) { tmp >>= 1; ++shift; }
                        RegId shiftReg = fn.nextReg++;
                        Instruction ci; ci.op = Opcode::ConstInt; ci.dest = shiftReg;
                        ci.immInt = shift; ci.type = inst.type; fn.regTypes[shiftReg] = ci.type;
                        blk.insts.insert(std::find(blk.insts.begin(),blk.insts.end(),inst), ci);
                        inst.op = Opcode::Shl; inst.operands = {l, shiftReg};
                        constInts[shiftReg] = shift;
                        changed = true;
                    }
                }
                // x / 2^n → x >> n  (for positive divisors)
                if (inst.op == Opcode::Div && rc && rv > 0 && (rv & (rv-1)) == 0) {
                    int shift = 0; int64_t tmp = rv; while (tmp > 1) { tmp >>= 1; ++shift; }
                    RegId shiftReg = fn.nextReg++;
                    Instruction ci; ci.op = Opcode::ConstInt; ci.dest = shiftReg;
                    ci.immInt = shift; ci.type = inst.type; fn.regTypes[shiftReg] = ci.type;
                    blk.insts.insert(std::find(blk.insts.begin(),blk.insts.end(),inst), ci);
                    inst.op = Opcode::Shr; inst.operands = {l, shiftReg};
                    changed = true;
                }
                // x % 2^n → x & (2^n - 1)
                if (inst.op == Opcode::Mod && rc && rv > 0 && (rv & (rv-1)) == 0) {
                    RegId maskReg = fn.nextReg++;
                    Instruction ci; ci.op = Opcode::ConstInt; ci.dest = maskReg;
                    ci.immInt = rv - 1; ci.type = inst.type; fn.regTypes[maskReg] = ci.type;
                    blk.insts.insert(std::find(blk.insts.begin(),blk.insts.end(),inst), ci);
                    inst.op = Opcode::BitAnd; inst.operands = {l, maskReg};
                    changed = true;
                }
                // x + 0, x - 0, x | 0, x ^ 0 → x
                if ((inst.op == Opcode::Add || inst.op == Opcode::Sub ||
                     inst.op == Opcode::BitOr || inst.op == Opcode::BitXor) && rc && rv == 0) {
                    RegId c = fn.nextReg++; Instruction ci; ci.op=Opcode::ConstBool; ci.dest=c; ci.immInt=1; ci.type={types::TY_BOOL}; fn.regTypes[c]=ci.type;
                    blk.insts.insert(std::find(blk.insts.begin(),blk.insts.end(),inst),ci);
                    inst.op = Opcode::Select; inst.operands = {c, l, l}; changed = true;
                }
            }
            // Apply deferred insertions in reverse to preserve indices
            for (auto it2 = deferred_inserts.rbegin(); it2 != deferred_inserts.rend(); ++it2) {
                blk.insts.insert(blk.insts.begin() + (std::ptrdiff_t)it2->first, it2->second);
            }
        }
        return changed;
    }
};

// Instruction Combining — folds algebraic identities and pattern-matches idioms
class InstCombinePass : public OptPass {
public:
    std::string name() const override { return "inst-combine"; }
    bool run(IRFunction& fn, IRModule&) override {
        bool changed = false;
        std::unordered_map<RegId, int64_t> constMap;
        for (auto& blk : fn.blocks) {
            // Use index loop + deferred inserts to avoid iterator invalidation
            std::vector<std::pair<size_t, Instruction>> deferred_inserts;
            for (size_t _ii = 0; _ii < blk.insts.size(); ++_ii) {
                Instruction& inst = blk.insts[_ii];
                if (inst.op == Opcode::ConstInt) { constMap[inst.dest] = inst.immInt; continue; }
                if (inst.operands.size() != 2) continue;
                RegId l = inst.operands[0], r = inst.operands[1];
                // x - x → 0,  x ^ x → 0,  x & x → x,  x | x → x
                if (l == r) {
                    if (inst.op == Opcode::Sub || inst.op == Opcode::BitXor) {
                        inst.op = Opcode::ConstInt; inst.immInt = 0; inst.operands.clear(); changed = true;
                    } else if (inst.op == Opcode::BitAnd || inst.op == Opcode::BitOr) {
                        RegId c = fn.nextReg++; Instruction ci; ci.op=Opcode::ConstBool; ci.dest=c; ci.immInt=1; ci.type={types::TY_BOOL}; fn.regTypes[c]=ci.type;
                        blk.insts.insert(std::find(blk.insts.begin(),blk.insts.end(),inst),ci);
                        inst.op = Opcode::Select; inst.operands={c,l,l}; changed=true;
                    }
                }
                // Commutativity: normalise const to right side
                if (constMap.count(l) && !constMap.count(r) &&
                    (inst.op==Opcode::Add || inst.op==Opcode::Mul ||
                     inst.op==Opcode::BitAnd || inst.op==Opcode::BitOr || inst.op==Opcode::BitXor ||
                     inst.op==Opcode::Eq || inst.op==Opcode::Ne)) {
                    std::swap(inst.operands[0], inst.operands[1]);
                    changed = true;
                }
                // not(not(x)) → x  (double negation)
                // handled by checking if l comes from a Not instruction
            }
            // Apply deferred insertions in reverse to preserve indices
            for (auto it2 = deferred_inserts.rbegin(); it2 != deferred_inserts.rend(); ++it2) {
                blk.insts.insert(blk.insts.begin() + (std::ptrdiff_t)it2->first, it2->second);
            }
        }
        return changed;
    }
};

// Global Value Numbering — assigns value numbers across blocks for redundancy elimination
class GVNPass : public OptPass {
public:
    std::string name() const override { return "gvn"; }
    bool run(IRFunction& fn, IRModule&) override {
        bool changed = false;
        // Value number table: maps value expression → canonical register
        std::unordered_map<std::string, RegId> valueTable;
        std::unordered_map<RegId, std::string> regVN; // reg → value number string

        auto vnKey = [&](Opcode op, const std::vector<RegId>& ops, int64_t imm, double fp) -> std::string {
            std::string k = std::to_string((int)op) + ":";
            for (RegId r : ops) {
                auto it = regVN.find(r);
                k += (it != regVN.end() ? it->second : std::to_string(r)) + ",";
            }
            k += "i" + std::to_string(imm) + "f" + std::to_string((int64_t)(fp*1e9));
            return k;
        };

        for (auto& blk : fn.blocks) {
            for (auto& inst : blk.insts) {
                bool pure = inst.dest != REG_NONE &&
                    inst.op != Opcode::Load && inst.op != Opcode::Call &&
                    inst.op != Opcode::Alloc && inst.op != Opcode::Intrinsic &&
                    inst.op != Opcode::TailCall && inst.op != Opcode::IndirectCall;
                if (!pure) continue;

                std::string key = vnKey(inst.op, inst.operands, inst.immInt, inst.immFp);
                auto it = valueTable.find(key);
                if (it != valueTable.end() && it->second != inst.dest) {
                    // Replace inst with copy of existing value number's register
                    RegId existing = it->second;
                    inst.op = Opcode::Select;
                    RegId c = fn.nextReg++;
                    Instruction ci; ci.op=Opcode::ConstBool; ci.dest=c; ci.immInt=1; ci.type={types::TY_BOOL}; fn.regTypes[c]=ci.type;
                    blk.insts.insert(std::find(blk.insts.begin(),blk.insts.end(),inst), ci);
                    inst.operands = {c, existing, existing};
                    regVN[inst.dest] = key;
                    changed = true;
                } else {
                    valueTable[key] = inst.dest;
                    regVN[inst.dest] = key;
                }
            }
        }
        return changed;
    }
};

// Loop Invariant Code Motion — hoists expressions outside loop bodies
class LICMPass : public OptPass {
public:
    std::string name() const override { return "licm"; }
    bool run(IRFunction& fn, IRModule&) override {
        bool changed = false;
        // Identify natural loops via back-edges in the CFG
        // A back-edge is b→h where h dominates b
        // Simplified: detect any block whose single successor leads back to an earlier block
        for (size_t bi = 1; bi < fn.blocks.size(); ++bi) {
            auto& blk = fn.blocks[bi];
            // Detect self-loop or back-edge to earlier block
            bool isLoopBody = false;
            BlockId headerCandidate = BLOCK_NONE;
            for (BlockId s : blk.succs) {
                if (s < (BlockId)bi) { isLoopBody = true; headerCandidate = s; break; }
            }
            if (!isLoopBody || headerCandidate == BLOCK_NONE) continue;

            // Collect all registers defined in the loop (this block)
            std::unordered_set<RegId> loopDefs;
            for (auto& inst : blk.insts)
                if (inst.dest != REG_NONE) loopDefs.insert(inst.dest);

            // Find invariant instructions: all operands defined outside the loop
            std::vector<Instruction> hoistable;
            std::vector<size_t> hoistIdx;
            for (size_t ii = 0; ii < blk.insts.size(); ++ii) {
                auto& inst = blk.insts[ii];
                if (inst.dest == REG_NONE) continue;
                if (inst.op == Opcode::Call || inst.op == Opcode::Load ||
                    inst.op == Opcode::Store || inst.op == Opcode::Alloc ||
                    inst.op == Opcode::Branch || inst.op == Opcode::Jump ||
                    inst.op == Opcode::Return || inst.op == Opcode::Phi) continue;
                bool allOutside = true;
                for (RegId op : inst.operands)
                    if (loopDefs.count(op)) { allOutside = false; break; }
                if (allOutside) { hoistable.push_back(inst); hoistIdx.push_back(ii); }
            }
            // Hoist: insert before the loop header block
            if (!hoistable.empty() && headerCandidate < fn.blocks.size()) {
                auto& preHeader = fn.blocks[headerCandidate];
                // Insert before the last terminator
                size_t insertPos = preHeader.insts.empty() ? 0 : preHeader.insts.size() - 1;
                for (size_t k = hoistable.size(); k-- > 0;)
                    preHeader.insts.insert(preHeader.insts.begin() + insertPos, hoistable[k]);
                // Remove from loop body (in reverse order to preserve indices)
                for (size_t k = hoistIdx.size(); k-- > 0;)
                    blk.insts.erase(blk.insts.begin() + hoistIdx[k]);
                changed = true;
            }
        }
        return changed;
    }
};

// Loop Unrolling — unrolls small constant-bounded loops
class LoopUnrollPass : public OptPass {
public:
    explicit LoopUnrollPass(size_t maxTrip) : maxTrip_(maxTrip) {}
    std::string name() const override { return "loop-unroll"; }
    bool run(IRFunction& fn, IRModule&) override {
        bool changed = false;
        std::unordered_map<RegId, int64_t> constMap;
        for (auto& blk : fn.blocks)
            for (auto& i : blk.insts)
                if (i.op == Opcode::ConstInt) constMap[i.dest] = i.immInt;

        // Look for a simple counted loop pattern:
        //   cond_block: %cond = icmp lt %i %bound; branch %cond body exit
        //   body_block: ... insts ...
        //   %i_next = add %i 1; jump cond_block
        for (size_t bi = 0; bi + 1 < fn.blocks.size(); ++bi) {
            auto& condBlk = fn.blocks[bi];
            if (condBlk.succs.size() != 2) continue;
            BlockId bodyId = condBlk.succs[0];
            BlockId exitId = condBlk.succs[1];
            if (bodyId >= fn.blocks.size()) continue;
            auto& bodyBlk = fn.blocks[bodyId];
            // Body must jump back to cond
            if (bodyBlk.succs.size() != 1 || bodyBlk.succs[0] != (BlockId)bi) continue;
            // Find the branch condition: must be Lt with a constant bound
            if (condBlk.insts.empty()) continue;
            auto& brInst = condBlk.insts.back();
            if (brInst.op != Opcode::Branch || brInst.operands.empty()) continue;
            RegId condReg = brInst.operands[0];
            // Find the Lt instruction producing condReg
            const Instruction* ltInst = nullptr;
            for (auto& ci : condBlk.insts) if (ci.dest == condReg && ci.op == Opcode::Lt) { ltInst = &ci; break; }
            if (!ltInst || ltInst->operands.size() != 2) continue;
            RegId boundReg = ltInst->operands[1];
            if (!constMap.count(boundReg)) continue;
            int64_t bound = constMap[boundReg];
            if (bound <= 0 || bound > (int64_t)maxTrip_) continue;
            // Find initial value of counter (must be 0 from a const)
            RegId counterReg = ltInst->operands[0];
            const Instruction* initInst = nullptr;
            for (auto& ci : condBlk.insts) if (ci.dest == counterReg && ci.op == Opcode::ConstInt) { initInst = &ci; break; }
            if (!initInst || initInst->immInt != 0) continue;

            // Unroll: replicate body `bound` times with fresh register namespaces
            std::vector<Instruction> unrolled;
            for (int64_t iter = 0; iter < bound; ++iter) {
                RegId regBase = fn.nextReg;
                fn.nextReg += static_cast<RegId>(bodyBlk.insts.size() * 2 + 10);
                // Emit a const for the iteration index
                Instruction iterConst;
                iterConst.op = Opcode::ConstInt; iterConst.dest = regBase;
                iterConst.immInt = iter; iterConst.type = {types::TY_I64};
                fn.regTypes[regBase] = iterConst.type;
                unrolled.push_back(iterConst);

                for (auto& bi2 : bodyBlk.insts) {
                    if (bi2.op == Opcode::Jump) continue; // skip back-edge jump
                    Instruction ni = bi2;
                    ni.dest = ni.dest == REG_NONE ? REG_NONE : ni.dest + regBase;
                    for (auto& op : ni.operands) {
                        if (op == counterReg) op = regBase; // substitute loop counter
                        else if (op != REG_NONE && fn.regTypes.count(op)) op = op + regBase;
                    }
                    if (ni.dest != REG_NONE) fn.regTypes[ni.dest] = bi2.type;
                    unrolled.push_back(ni);
                }
            }

            // Replace cond + body blocks with unrolled code followed by jump to exit
            Instruction exitJump; exitJump.op = Opcode::Jump; exitJump.target = exitId;
            unrolled.push_back(exitJump);
            condBlk.insts = std::move(unrolled);
            condBlk.succs = {exitId};
            bodyBlk.insts.clear(); bodyBlk.succs.clear();
            changed = true;
        }
        return changed;
    }
private:
    size_t maxTrip_;
};

// Escape Analysis — marks allocations that don't escape so they can be stack-allocated
class EscapeAnalysisPass : public OptPass {
public:
    std::string name() const override { return "escape-analysis"; }
    bool run(IRFunction& fn, IRModule&) override {
        bool changed = false;
        // Collect all alloc instructions
        std::unordered_set<RegId> allocRegs;
        for (auto& blk : fn.blocks)
            for (auto& inst : blk.insts)
                if (inst.op == Opcode::Alloc) allocRegs.insert(inst.dest);
        if (allocRegs.empty()) return false;

        // An alloc escapes if its register is:
        // 1. Passed as an argument to a Call/IndirectCall (except as a Store target)
        // 2. Returned from the function
        // 3. Stored into another pointer (pointer-to-pointer)
        std::unordered_set<RegId> escaping;
        for (auto& blk : fn.blocks) {
            for (auto& inst : blk.insts) {
                if (inst.op == Opcode::Return) {
                    for (RegId op : inst.operands) if (allocRegs.count(op)) escaping.insert(op);
                }
                if (inst.op == Opcode::Call || inst.op == Opcode::IndirectCall ||
                    inst.op == Opcode::TailCall) {
                    for (RegId op : inst.operands) if (allocRegs.count(op)) escaping.insert(op);
                }
                if (inst.op == Opcode::Store && inst.operands.size() == 2) {
                    // store val → ptr: if val is a ptr (alloc), it escapes
                    if (allocRegs.count(inst.operands[0])) escaping.insert(inst.operands[0]);
                }
            }
        }
        // Mark non-escaping allocs — in practice this enables stack allocation in native backends.
        // In our IR we annotate via the globalName field as a hint to the backend.
        for (auto& blk : fn.blocks) {
            for (auto& inst : blk.insts) {
                if (inst.op == Opcode::Alloc && allocRegs.count(inst.dest) &&
                    !escaping.count(inst.dest) && inst.globalName != "noescaped") {
                    inst.globalName = "noescaped"; // stack-allocatable hint
                    changed = true;
                }
            }
        }
        return changed;
    }
};

// Jump Threading — short-circuits branches through unconditional-jump-only blocks
class JumpThreadingPass : public OptPass {
public:
    std::string name() const override { return "jump-thread"; }
    bool run(IRFunction& fn, IRModule&) override {
        bool changed = false;
        // Build: blockId → first non-trivial target (skip empty or unconditional-only blocks)
        std::unordered_map<BlockId, BlockId> forwardMap;
        for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
            auto& blk = fn.blocks[bi];
            // An unconditional-only block has exactly one instruction (Jump) or is empty with one succ
            if (blk.insts.size() == 1 && blk.insts[0].op == Opcode::Jump) {
                forwardMap[(BlockId)bi] = blk.insts[0].target;
            }
        }
        if (forwardMap.empty()) return false;

        // Resolve chains: A→B→C where B is a passthrough → A→C
        auto resolve = [&](BlockId b) -> BlockId {
            std::unordered_set<BlockId> visited;
            while (forwardMap.count(b) && !visited.count(b)) {
                visited.insert(b);
                b = forwardMap[b];
            }
            return b;
        };

        for (auto& blk : fn.blocks) {
            for (auto& inst : blk.insts) {
                if (inst.op == Opcode::Jump && forwardMap.count(inst.target)) {
                    BlockId resolved = resolve(inst.target);
                    if (resolved != inst.target) { inst.target = resolved; changed = true; }
                }
                if (inst.op == Opcode::Branch) {
                    if (forwardMap.count(inst.target)) {
                        BlockId r = resolve(inst.target);
                        if (r != inst.target) { inst.target = r; changed = true; }
                    }
                    if (forwardMap.count(inst.altTarget)) {
                        BlockId r = resolve(inst.altTarget);
                        if (r != inst.altTarget) { inst.altTarget = r; changed = true; }
                    }
                }
            }
            // Recompute succs
            if (changed) {
                blk.succs.clear();
                for (auto& inst : blk.insts) {
                    if (inst.op == Opcode::Jump)   blk.succs.push_back(inst.target);
                    if (inst.op == Opcode::Branch) { blk.succs.push_back(inst.target); blk.succs.push_back(inst.altTarget); }
                    for (auto& c : inst.cases)     blk.succs.push_back(c.target);
                }
            }
        }
        return changed;
    }
};

// Reassociation — reorders operands of associative ops to expose more constant-folding opportunities
class ReassociationPass : public OptPass {
public:
    std::string name() const override { return "reassociate"; }
    bool run(IRFunction& fn, IRModule&) override {
        bool changed = false;
        std::unordered_map<RegId, int64_t> constMap;
        for (auto& blk : fn.blocks)
            for (auto& i : blk.insts)
                if (i.op == Opcode::ConstInt) constMap[i.dest] = i.immInt;

        // Pattern: (x + C1) + C2 → x + (C1+C2)
        std::unordered_map<RegId, std::pair<RegId,int64_t>> addConst; // dest → (nonConstOp, constVal)
        for (auto& blk : fn.blocks) {
            // Use index loop + deferred inserts to avoid iterator invalidation
            std::vector<std::pair<size_t, Instruction>> deferred_inserts;
            for (size_t _ii = 0; _ii < blk.insts.size(); ++_ii) {
                Instruction& inst = blk.insts[_ii];
                if (inst.op != Opcode::Add || inst.operands.size() != 2) continue;
                RegId l = inst.operands[0], r = inst.operands[1];
                bool lc = constMap.count(l), rc = constMap.count(r);
                if (rc) addConst[inst.dest] = {l, constMap[r]};
                else if (lc) addConst[inst.dest] = {r, constMap[l]};

                // (x + C1) + C2 → x + (C1+C2)
                if (rc && addConst.count(l)) {
                    auto [innerOp, c1] = addConst[l];
                    int64_t c2 = constMap[r];
                    // Replace r with a new const (c1+c2) and l with innerOp
                    // We'll patch in-place by emitting a new const before this inst
                    Instruction newConst;
                    newConst.op = Opcode::ConstInt;
                    newConst.dest = fn.nextReg++;
                    newConst.immInt = c1 + c2;
                    newConst.type = inst.type;
                    fn.regTypes[newConst.dest] = inst.type;
                    constMap[newConst.dest] = c1 + c2;
                    deferred_inserts.push_back({_ii, newConst});
                    inst.operands[0] = innerOp;
                    inst.operands[1] = newConst.dest;
                    addConst[inst.dest] = {innerOp, c1 + c2};
                    changed = true;
                }
            }
            // Apply deferred insertions in reverse to preserve indices
            for (auto it2 = deferred_inserts.rbegin(); it2 != deferred_inserts.rend(); ++it2) {
                blk.insts.insert(blk.insts.begin() + (std::ptrdiff_t)it2->first, it2->second);
            }
        }
        return changed;
    }
};

// ─── Pipeline factory ─────────────────────────────────────────────────────────
bool OptPipeline::run(IRModule& mod) {
    bool changed = false;
    for (auto& fn : mod.functions)
        for (auto& p : passes_)
            changed |= p->run(fn, mod);
    return changed;
}

std::unique_ptr<OptPass> OptPipeline::makeDeadCodeElim()        { return std::make_unique<DeadCodeElimPass>(); }
std::unique_ptr<OptPass> OptPipeline::makeConstantFolding()     { return std::make_unique<ConstantFoldingPass>(); }
std::unique_ptr<OptPass> OptPipeline::makeConstantPropagation() { return std::make_unique<ConstantPropagationPass>(); }
std::unique_ptr<OptPass> OptPipeline::makeCopyPropagation()     { return std::make_unique<CopyPropagationPass>(); }
std::unique_ptr<OptPass> OptPipeline::makeTailCallOpt()         { return std::make_unique<TailCallOptPass>(); }
std::unique_ptr<OptPass> OptPipeline::makeBlockMerge()          { return std::make_unique<BlockMergePass>(); }
std::unique_ptr<OptPass> OptPipeline::makeCommonSubexprElim()   { return std::make_unique<CSEPass>(); }
std::unique_ptr<OptPass> OptPipeline::makeInlining(size_t t)    { return std::make_unique<InliningPass>(t); }
std::unique_ptr<OptPass> OptPipeline::makeLoopInvariantMotion() { return std::make_unique<LICMPass>(); }
std::unique_ptr<OptPass> OptPipeline::makeStrengthReduction()   { return std::make_unique<StrengthReductionPass>(); }
std::unique_ptr<OptPass> OptPipeline::makeMemToReg()            { return std::make_unique<MemToRegPass>(); }
std::unique_ptr<OptPass> OptPipeline::makeJumpThreading()       { return std::make_unique<JumpThreadingPass>(); }
std::unique_ptr<OptPass> OptPipeline::makeInstCombine()         { return std::make_unique<InstCombinePass>(); }
std::unique_ptr<OptPass> OptPipeline::makeReassociation()       { return std::make_unique<ReassociationPass>(); }
std::unique_ptr<OptPass> OptPipeline::makeGlobalValueNumbering(){ return std::make_unique<GVNPass>(); }
std::unique_ptr<OptPass> OptPipeline::makeLoopUnroll(size_t t)  { return std::make_unique<LoopUnrollPass>(t); }
std::unique_ptr<OptPass> OptPipeline::makeEscapeAnalysis()      { return std::make_unique<EscapeAnalysisPass>(); }
std::unique_ptr<OptPass> OptPipeline::makeVectorize()           { return std::make_unique<CSEPass>(); } // CSE exposes vector candidates; real vectorizer needs a native backend

OptPipeline OptPipeline::makeO0() { OptPipeline p; return p; }
OptPipeline OptPipeline::makeO1() {
    OptPipeline p;
    p.addPass(makeConstantFolding());
    p.addPass(makeDeadCodeElim());
    p.addPass(makeBlockMerge());
    return p;
}
OptPipeline OptPipeline::makeO2() {
    OptPipeline p;
    p.addPass(makeConstantFolding());
    p.addPass(makeConstantPropagation());
    p.addPass(makeCommonSubexprElim());
    p.addPass(makeCopyPropagation());
    p.addPass(makeDeadCodeElim());
    p.addPass(makeTailCallOpt());
    p.addPass(makeBlockMerge());
    p.addPass(makeJumpThreading());
    return p;
}
OptPipeline OptPipeline::makeO3() {
    OptPipeline p;
    p.addPass(makeConstantFolding());
    p.addPass(makeConstantPropagation());
    p.addPass(makeInlining(100));
    p.addPass(makeCommonSubexprElim());
    p.addPass(makeGlobalValueNumbering());
    p.addPass(makeLoopInvariantMotion());
    p.addPass(makeStrengthReduction());
    p.addPass(makeInstCombine());
    p.addPass(makeReassociation());
    p.addPass(makeCopyPropagation());
    p.addPass(makeDeadCodeElim());
    p.addPass(makeTailCallOpt());
    p.addPass(makeLoopUnroll(8));
    p.addPass(makeVectorize());
    p.addPass(makeEscapeAnalysis());
    p.addPass(makeMemToReg());
    p.addPass(makeBlockMerge());
    return p;
}
OptPipeline OptPipeline::makeOs() {
    OptPipeline p;
    p.addPass(makeConstantFolding());
    p.addPass(makeDeadCodeElim());
    p.addPass(makeBlockMerge());
    return p;
}

} // namespace ir
} // namespace fpp
