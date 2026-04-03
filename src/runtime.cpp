#include "../include/runtime.hpp"
#include "../include/ir.hpp"
#include "../include/lexer.hpp"
#include "../include/parser.hpp"
#include "../include/types.hpp"
#include "../include/codegen.hpp"
#include <sstream>
#include <cassert>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <iomanip>

namespace fpp {
namespace runtime {

// ─── Value constructors ──────────────────────────────────────────────────────
ValueRef Value::makeNil() {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::Nil; return v;
}
ValueRef Value::makeBool(bool b) {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::Bool; v->prim.b = b;
    v->typeId = types::TY_BOOL; return v;
}
ValueRef Value::makeI32(int32_t x) {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::I32; v->prim.i32 = x;
    v->typeId = types::TY_I32; return v;
}
ValueRef Value::makeI64(int64_t x) {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::I64; v->prim.i64 = x;
    v->typeId = types::TY_I64; return v;
}
ValueRef Value::makeU64(uint64_t x) {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::U64; v->prim.u64 = x;
    v->typeId = types::TY_U64; return v;
}
ValueRef Value::makeF64(double x) {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::F64; v->prim.f64 = x;
    v->typeId = types::TY_F64; return v;
}
ValueRef Value::makeChar(char32_t c) {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::Char; v->prim.ch = c;
    v->typeId = types::TY_CHAR; return v;
}
ValueRef Value::makeString(std::string s) {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::String;
    v->strData = std::make_shared<std::string>(std::move(s));
    v->typeId = types::TY_STR; return v;
}
ValueRef Value::makeArray(std::vector<ValueRef> elems) {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::Array;
    v->arrayData = std::make_shared<std::vector<ValueRef>>(std::move(elems)); return v;
}
ValueRef Value::makeMap(std::unordered_map<std::string,ValueRef> m) {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::Map;
    v->mapData = std::make_shared<std::unordered_map<std::string,ValueRef>>(std::move(m)); return v;
}
ValueRef Value::makeStruct(const FppClass* klass, std::unordered_map<std::string,ValueRef> fields) {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::Struct;
    auto sd = std::make_shared<StructData>(); sd->klass = klass; sd->fields = std::move(fields);
    v->structData = sd; return v;
}
ValueRef Value::makeEnum(std::string name, uint32_t tag, std::vector<ValueRef> payload) {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::Enum;
    auto ed = std::make_shared<EnumVariant>(); ed->name = std::move(name); ed->tag = tag; ed->payload = std::move(payload);
    v->enumData = ed; return v;
}
ValueRef Value::makeClosure(ClosureData data) {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::Closure;
    v->closureData = std::make_shared<ClosureData>(std::move(data)); return v;
}
ValueRef Value::makeNative(NativeFunction fn) {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::NativeFunc;
    v->nativeFn = std::move(fn); return v;
}
ValueRef Value::makeChannel(size_t cap) {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::Channel;
    auto cd = std::make_shared<ChannelData>(); cd->capacity = cap;
    v->chanData = cd; return v;
}
ValueRef Value::makeSome(ValueRef val) {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::Option_Some; v->inner = val; return v;
}
ValueRef Value::makeNone() {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::Option_None; return v;
}
ValueRef Value::makeOk(ValueRef val) {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::Result_Ok; v->inner = val; return v;
}
ValueRef Value::makeErr(ValueRef e) {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::Result_Err; v->errVal = e; return v;
}
ValueRef Value::makeRawPtr(void* p) {
    auto v = std::make_shared<Value>(); v->kind = ValueKind::Ptr; v->prim.rawPtr = p; return v;
}

// ─── Value helpers ────────────────────────────────────────────────────────────
bool Value::isTruthy() const noexcept {
    switch (kind) {
    case ValueKind::Nil:         return false;
    case ValueKind::Bool:        return prim.b;
    case ValueKind::I8:          return prim.i8 != 0;
    case ValueKind::I16:         return prim.i16 != 0;
    case ValueKind::I32:         return prim.i32 != 0;
    case ValueKind::I64:         return prim.i64 != 0;
    case ValueKind::U8:          return prim.u8 != 0;
    case ValueKind::U16:         return prim.u16 != 0;
    case ValueKind::U32:         return prim.u32 != 0;
    case ValueKind::U64:         return prim.u64 != 0;
    case ValueKind::F32:         return prim.f32 != 0.0f;
    case ValueKind::F64:         return prim.f64 != 0.0;
    case ValueKind::String:      return strData && !strData->empty();
    case ValueKind::Array:       return arrayData && !arrayData->empty();
    case ValueKind::Option_None: return false;
    case ValueKind::Option_Some: return true;
    default:                     return true;
    }
}

std::string Value::toString() const {
    switch (kind) {
    case ValueKind::Nil:    return "nil";
    case ValueKind::Bool:   return prim.b ? "true" : "false";
    case ValueKind::I8:     return std::to_string(prim.i8);
    case ValueKind::I16:    return std::to_string(prim.i16);
    case ValueKind::I32:    return std::to_string(prim.i32);
    case ValueKind::I64:    return std::to_string(prim.i64);
    case ValueKind::U8:     return std::to_string(prim.u8);
    case ValueKind::U16:    return std::to_string(prim.u16);
    case ValueKind::U32:    return std::to_string(prim.u32);
    case ValueKind::U64:    return std::to_string(prim.u64);
    case ValueKind::ISize:  return std::to_string(prim.i64);
    case ValueKind::USize:  return std::to_string(prim.u64);
    case ValueKind::F32:    { std::ostringstream ss; ss << prim.f32; return ss.str(); }
    case ValueKind::F64:    { std::ostringstream ss; ss << prim.f64; return ss.str(); }
    case ValueKind::Char:   return std::string(1, static_cast<char>(prim.ch));
    case ValueKind::String: return strData ? *strData : "";
    case ValueKind::Array: {
        if (!arrayData) return "[]";
        std::string s = "[";
        for (size_t i = 0; i < arrayData->size(); ++i) {
            s += (*arrayData)[i]->toString();
            if (i + 1 < arrayData->size()) s += ", ";
        }
        return s + "]";
    }
    case ValueKind::Tuple: {
        if (!arrayData) return "()";
        std::string s = "(";
        for (size_t i = 0; i < arrayData->size(); ++i) {
            s += (*arrayData)[i]->toString();
            if (i + 1 < arrayData->size()) s += ", ";
        }
        return s + ")";
    }
    case ValueKind::Map: {
        if (!mapData) return "{}";
        std::string s = "{";
        bool first = true;
        for (auto& [k, v] : *mapData) {
            if (!first) s += ", ";
            s += k + ": " + v->toString();
            first = false;
        }
        return s + "}";
    }
    case ValueKind::Struct: {
        if (!structData) return "struct{}";
        std::string s = structData->klass ? structData->klass->name : "struct";
        s += " {";
        bool first = true;
        for (auto& [k, v] : structData->fields) {
            if (!first) s += ", ";
            s += k + ": " + v->toString(); first = false;
        }
        return s + "}";
    }
    case ValueKind::Enum:        return enumData ? enumData->name : "enum";
    case ValueKind::Function:
    case ValueKind::Closure:
    case ValueKind::NativeFunc:  return "<fn>";
    case ValueKind::Option_None: return "None";
    case ValueKind::Option_Some: return "Some(" + (inner ? inner->toString() : "") + ")";
    case ValueKind::Result_Ok:   return "Ok(" + (inner ? inner->toString() : "") + ")";
    case ValueKind::Result_Err:  return "Err(" + (errVal ? errVal->toString() : "") + ")";
    case ValueKind::Channel:     return "<chan>";
    case ValueKind::Ptr:         { std::ostringstream ss; ss << "0x" << std::hex << (uintptr_t)prim.rawPtr; return ss.str(); }
    default:                     return "<value>";
    }
}

bool Value::equals(const Value& o) const noexcept {
    if (kind != o.kind) {
        // Allow numeric cross-comparison
        auto asF64 = [](const Value& v) -> double {
            switch (v.kind) {
            case ValueKind::I8:  return v.prim.i8;  case ValueKind::I16: return v.prim.i16;
            case ValueKind::I32: return v.prim.i32; case ValueKind::I64: return v.prim.i64;
            case ValueKind::U8:  return v.prim.u8;  case ValueKind::U16: return v.prim.u16;
            case ValueKind::U32: return v.prim.u32; case ValueKind::U64: return (double)v.prim.u64;
            case ValueKind::F32: return v.prim.f32; case ValueKind::F64: return v.prim.f64;
            default: return std::numeric_limits<double>::quiet_NaN();
            }
        };
        return asF64(*this) == asF64(o);
    }
    switch (kind) {
    case ValueKind::Nil:         return true;
    case ValueKind::Bool:        return prim.b == o.prim.b;
    case ValueKind::I32:         return prim.i32 == o.prim.i32;
    case ValueKind::I64:         return prim.i64 == o.prim.i64;
    case ValueKind::U64:         return prim.u64 == o.prim.u64;
    case ValueKind::F64:         return prim.f64 == o.prim.f64;
    case ValueKind::Char:        return prim.ch  == o.prim.ch;
    case ValueKind::String:      return strData && o.strData && *strData == *o.strData;
    case ValueKind::Option_None: return o.kind == ValueKind::Option_None;
    case ValueKind::Option_Some: return inner && o.inner && inner->equals(*o.inner);
    default:                     return false;
    }
}

int Value::compare(const Value& o) const {
    if (kind == ValueKind::String && o.kind == ValueKind::String)
        return strData->compare(*o.strData);
    // Numeric comparison
    double a = 0, b = 0;
    auto toD = [](const Value& v) {
        switch (v.kind) {
        case ValueKind::I32: return (double)v.prim.i32; case ValueKind::I64: return (double)v.prim.i64;
        case ValueKind::U64: return static_cast<double>(v.prim.u64); case ValueKind::F64: return v.prim.f64;
        default: return 0.0;
        }
    };
    a = toD(*this); b = toD(o);
    return (a < b) ? -1 : (a > b) ? 1 : 0;
}

ValueRef Value::deepCopy() const {
    auto v = std::make_shared<Value>(*this);
    if (strData)    v->strData = std::make_shared<std::string>(*strData);
    if (arrayData) {
        v->arrayData = std::make_shared<std::vector<ValueRef>>();
        for (auto& e : *arrayData) v->arrayData->push_back(e->deepCopy());
    }
    return v;
}

size_t Value::hash() const noexcept {
    switch (kind) {
    case ValueKind::I64: return std::hash<int64_t>{}(prim.i64);
    case ValueKind::F64: return std::hash<double>{}(prim.f64);
    case ValueKind::Bool: return std::hash<bool>{}(prim.b);
    case ValueKind::String: return strData ? std::hash<std::string>{}(*strData) : 0;
    default: return 0;
    }
}

// ─── FppException ─────────────────────────────────────────────────────────────
FppException::FppException(ValueRef v) : value(std::move(v)) {
    message = this->value ? this->value->toString() : "<exception>";
}
void FppException::pushFrame(std::string name, SourceLocation loc) {
    stackTrace.emplace_back(std::move(name), loc);
}

// ─── TaskScheduler ────────────────────────────────────────────────────────────
TaskScheduler::TaskScheduler(size_t numWorkers) {
    for (size_t i = 0; i < numWorkers; ++i) {
        workers_.emplace_back([this] {
            for (;;) {
                std::packaged_task<ValueRef()> task;
                {
                    std::unique_lock<std::mutex> lock(queueMtx_);
                    cv_.wait(lock, [this]{ return stopping_ || !queue_.empty(); });
                    if (stopping_ && queue_.empty()) return;
                    task = std::move(queue_.front());
                    queue_.pop_front();
                }
                ++pending_;
                task();
                --pending_;
            }
        });
    }
}

TaskScheduler::~TaskScheduler() { shutdown(); }

std::future<ValueRef> TaskScheduler::spawn(std::function<ValueRef()> fn) {
    std::packaged_task<ValueRef()> task(std::move(fn));
    auto future = task.get_future();
    {
        std::unique_lock<std::mutex> lock(queueMtx_);
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
    return future;
}

void TaskScheduler::shutdown() {
    stopping_ = true;
    cv_.notify_all();
    for (auto& w : workers_) if (w.joinable()) w.join();
    workers_.clear();
}

void TaskScheduler::runMainLoop() {
    std::packaged_task<ValueRef()> task;
    std::unique_lock<std::mutex> lock(queueMtx_);
    while (!queue_.empty()) {
        task = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        task();
        lock.lock();
    }
}

static void fpp_aligned_free(void* ptr) {
#ifdef _WIN32
    _aligned_free(ptr);
#else
    ::free(ptr);
#endif
}

// ─── GCHeap ───────────────────────────────────────────────────────────────────
GCHeap::GCHeap(size_t initialBytes) : total_(initialBytes), gcThreshold_(initialBytes / 2) {}
GCHeap::~GCHeap() {
    for (auto& b : blocks_) ::free(b.ptr);
}

void* GCHeap::alloc(size_t bytes, size_t align) {
    if (used_.load() + bytes > gcThreshold_) gcMinor();
    void* ptr = nullptr;
#ifdef _WIN32
    ptr = _aligned_malloc(bytes, std::max(align, (size_t)sizeof(void*)));
#elif defined(__APPLE__) || defined(__linux__)
    if (posix_memalign(&ptr, std::max(align, sizeof(void*)), bytes) != 0) ptr = nullptr;
#else
    ptr = std::malloc(bytes);
#endif
    if (!ptr) throw std::bad_alloc();
    std::memset(ptr, 0, bytes);
    {
        std::lock_guard<std::mutex> lk(heapMtx_);
        blocks_.push_back({ptr, bytes, false});
    }
    used_ += bytes;
    return ptr;
}

void GCHeap::free(void* ptr) {
    std::lock_guard<std::mutex> lk(heapMtx_);
    for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
        if (it->ptr == ptr) {
            used_ -= it->size;
            fpp_aligned_free(ptr);
            blocks_.erase(it);
            return;
        }
    }
}

void GCHeap::markValue(ValueRef& v, std::set<void*>& visited) {
    if (!v) return;
    void* ptr = v.get();
    if (visited.count(ptr)) return;
    visited.insert(ptr);

    // Mark this value's heap block if it exists
    for (auto& b : blocks_) {
        if (b.ptr == ptr || (ptr >= b.ptr && ptr < (char*)b.ptr + b.size))
            b.marked = true;
    }

    // Recursively mark all reachable sub-values
    if (v->strData) {
        void* sp = v->strData.get();
        if (!visited.count(sp)) { visited.insert(sp); for (auto& b:blocks_) if(b.ptr==sp) b.marked=true; }
    }
    if (v->arrayData) {
        void* ap = v->arrayData.get();
        if (!visited.count(ap)) { visited.insert(ap); for (auto& b:blocks_) if(b.ptr==ap) b.marked=true; }
        for (auto& elem : *v->arrayData) markValue(elem, visited);
    }
    if (v->mapData) {
        for (auto& [k, val] : *v->mapData) {
            ValueRef vref = val;
            markValue(vref, visited);
        }
    }
    if (v->enumData) {
        for (auto& p : v->enumData->payload) markValue(p, visited);
    }
    if (v->structData) {
        for (auto& [k, field] : v->structData->fields) {
            ValueRef fref = field;
            markValue(fref, visited);
        }
        for (auto& [k, m] : v->structData->methods) {
            ValueRef mref = m;
            markValue(mref, visited);
        }
    }
    if (v->closureData) {
        for (auto& [k, cap] : v->closureData->captures) {
            ValueRef cref = cap;
            markValue(cref, visited);
        }
    }
    if (v->inner)  markValue(v->inner, visited);
    if (v->errVal) markValue(v->errVal, visited);
}

void GCHeap::mark() {
    std::set<void*> visited;
    for (auto* root : roots_) if (root) markValue(*root, visited);
}

void GCHeap::sweep() {
    std::lock_guard<std::mutex> lk(heapMtx_);
    for (auto& b : blocks_) {
        if (!b.marked) {
            used_ -= b.size;
            ::free(b.ptr);
            b.ptr = nullptr;
        }
        b.marked = false;
    }
    blocks_.erase(std::remove_if(blocks_.begin(), blocks_.end(),
        [](const Block& b){ return b.ptr == nullptr; }), blocks_.end());
}

void GCHeap::gc() { mark(); sweep(); }
void GCHeap::gcMinor() {
    // Young-generation collection: scan only recently allocated blocks (those at
    // the tail of the blocks_ list since the last minor GC), marking from roots.
    std::lock_guard<std::mutex> lk(heapMtx_);
    if (blocks_.empty()) return;

    // Consider the last 25% of blocks as the "young" generation
    size_t youngStart = blocks_.size() > 4 ? blocks_.size() * 3 / 4 : 0;

    // Mark all young blocks reachable from roots
    std::set<void*> visited;
    for (auto* root : roots_) {
        if (root && *root) markValue(*root, visited);
    }
    // Mark young blocks reachable from old (remembered-set via pointer scanning)
    // For each old block, scan its memory for pointer-sized values that fall within
    // the address range of any young block, and mark those young blocks live.
    for (size_t i = 0; i < youngStart; ++i) {
        if (!blocks_[i].marked) continue;
        // Scan the old block's memory 8 bytes at a time looking for pointers into young space
        const char* mem = reinterpret_cast<const char*>(blocks_[i].ptr);
        size_t sz = blocks_[i].size;
        for (size_t offset = 0; offset + sizeof(void*) <= sz; offset += sizeof(void*)) {
            void* candidate = nullptr;
            std::memcpy(&candidate, mem + (std::ptrdiff_t)offset, sizeof(void*));
            if (!candidate) continue;
            for (size_t j = youngStart; j < blocks_.size(); ++j) {
                const char* bstart = reinterpret_cast<const char*>(blocks_[j].ptr);
                const char* bend   = bstart + blocks_[j].size;
                if (reinterpret_cast<const char*>(candidate) >= bstart &&
                    reinterpret_cast<const char*>(candidate) <  bend) {
                    blocks_[j].marked = true;
                }
            }
        }
    }

    // Sweep only the young generation
    for (size_t i = youngStart; i < blocks_.size(); ) {
        if (!blocks_[i].marked) {
            used_ -= blocks_[i].size;
            ::free(blocks_[i].ptr);
            blocks_.erase(blocks_.begin() + i);
        } else {
            blocks_[i].marked = false;
            ++i;
        }
    }
}

void GCHeap::addRoot(ValueRef* root)    { roots_.insert(root); }
void GCHeap::removeRoot(ValueRef* root) { roots_.erase(root); }

// ─── VM ───────────────────────────────────────────────────────────────────────
VM::VM(size_t, size_t workers)
    : scheduler_(std::make_unique<TaskScheduler>(workers))
    , heap_(std::make_unique<GCHeap>()) {
    stdoutFn_ = [](const std::string& s){ std::cout << s; };
    stderrFn_ = [](const std::string& s){ std::cerr << s; };
    stdinFn_  = []() -> std::string { std::string s; std::getline(std::cin, s); return s; };
    registerBuiltins();
}

VM::~VM() { scheduler_->shutdown(); }

void VM::print(const std::string& s)    { if (stdoutFn_) stdoutFn_(s); }
void VM::printErr(const std::string& s) { if (stderrFn_) stderrFn_(s); }
std::string VM::readLine()              { return stdinFn_ ? stdinFn_() : ""; }

void VM::throwException(ValueRef v) { pendingException_ = std::move(v); }
ValueRef VM::takeException()        { auto v = *pendingException_; pendingException_.reset(); return v; }

CallFrame& VM::topFrame()       { return callStack_.back(); }
const CallFrame& VM::topFrame() const { return callStack_.back(); }
void VM::pushFrame(CallFrame f) { callStack_.push_back(std::move(f)); }
void VM::popFrame()             { if (!callStack_.empty()) callStack_.pop_back(); }

ValueRef VM::getLocal(const std::string& name) const {
    if (callStack_.empty()) return Value::makeNil();
    auto& locals = callStack_.back().locals;
    auto it = locals.find(name);
    if (it != locals.end()) return it->second;
    auto& caps = callStack_.back().captures;
    auto it2 = caps.find(name);
    return it2 != caps.end() ? it2->second : Value::makeNil();
}

void VM::setLocal(const std::string& name, ValueRef v) {
    if (!callStack_.empty()) callStack_.back().locals[name] = std::move(v);
}

ValueRef VM::getGlobal(const std::string& name) const {
    auto it = globals_.find(name);
    return it != globals_.end() ? it->second : Value::makeNil();
}

void VM::setGlobal(const std::string& name, ValueRef v) { globals_[name] = std::move(v); }

void VM::bindNative(const std::string& name, NativeFunction fn) {
    globals_[name] = Value::makeNative(std::move(fn));
}

void VM::bindClass(FppClass cls) {
    std::string name = cls.name;
    classes_[name] = std::move(cls);
}

void VM::bindModule(const std::string& modName, std::unordered_map<std::string,ValueRef> exports) {
    for (auto& [k, v] : exports)
        globals_[modName + "::" + k] = v;
}

void VM::registerBuiltins() {
    // print / println
    bindNative("print", [this](std::vector<ValueRef> args, VM&) -> ValueRef {
        for (auto& a : args) print(a->toString());
        return Value::makeNil();
    });
    bindNative("println", [this](std::vector<ValueRef> args, VM&) -> ValueRef {
        for (auto& a : args) print(a->toString());
        print("\n");
        return Value::makeNil();
    });
    bindNative("eprint",  [this](std::vector<ValueRef> args, VM&) -> ValueRef {
        for (auto& a : args) printErr(a->toString());
        return Value::makeNil();
    });
    bindNative("eprintln",[this](std::vector<ValueRef> args, VM&) -> ValueRef {
        for (auto& a : args) printErr(a->toString());
        printErr("\n");
        return Value::makeNil();
    });
    // panic
    bindNative("panic", [this](std::vector<ValueRef> args, VM&) -> ValueRef {
        std::string msg = args.empty() ? "explicit panic" : args[0]->toString();
        throwException(Value::makeString(msg));
        return Value::makeNil();
    });
    // assert
    bindNative("assert", [this](std::vector<ValueRef> args, VM&) -> ValueRef {
        bool cond = !args.empty() && args[0]->isTruthy();
        if (!cond) {
            std::string msg = args.size() > 1 ? args[1]->toString() : "assertion failed";
            throwException(Value::makeString(msg));
        }
        return Value::makeNil();
    });
    // assert_eq
    bindNative("assert_eq", [this](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.size() < 2 || !args[0]->equals(*args[1])) {
            std::string msg = "assertion failed: " +
                (args.size() > 0 ? args[0]->toString() : "?") + " == " +
                (args.size() > 1 ? args[1]->toString() : "?");
            throwException(Value::makeString(msg));
        }
        return Value::makeNil();
    });
    // read_line
    bindNative("read_line", [this](std::vector<ValueRef>, VM&) -> ValueRef {
        return Value::makeString(readLine());
    });
    // type_of
    bindNative("type_of", [](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.empty()) return Value::makeString("nil");
        static const char* names[] = {
            "nil","bool","i8","i16","i32","i64","i128","isize",
            "u8","u16","u32","u64","u128","usize","f32","f64","char",
            "str","bytes","array","tuple","map","set","struct","enum","class",
            "fn","closure","native_fn","ptr","ref","box",
            "future","task","channel","error","result_ok","result_err",
            "option_some","option_none","iterator","generator"
        };
        size_t idx = static_cast<size_t>(args[0]->kind);
        return Value::makeString(idx < sizeof(names)/sizeof(names[0]) ? names[idx] : "unknown");
    });
    // len
    bindNative("len", [this](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.empty()) return Value::makeI64(0);
        auto& v = args[0];
        if (v->kind == ValueKind::String && v->strData) return Value::makeI64(v->strData->size());
        if (v->arrayData) return Value::makeI64(v->arrayData->size());
        if (v->mapData)   return Value::makeI64(v->mapData->size());
        throwException(Value::makeString("len: not a collection"));
        return Value::makeNil();
    });
    // to_string
    bindNative("to_string", [](std::vector<ValueRef> args, VM&) -> ValueRef {
        return args.empty() ? Value::makeString("") : Value::makeString(args[0]->toString());
    });
    // parse_int
    bindNative("parse_int", [this](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.empty() || args[0]->kind != ValueKind::String || !args[0]->strData)
            return Value::makeErr(Value::makeString("parse_int: expected string"));
        try {
            return Value::makeOk(Value::makeI64(std::stoll(*args[0]->strData)));
        } catch (...) {
            return Value::makeErr(Value::makeString("parse_int: invalid format"));
        }
    });
    // parse_float
    bindNative("parse_float", [](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.empty() || args[0]->kind != ValueKind::String || !args[0]->strData)
            return Value::makeErr(Value::makeString("parse_float: expected string"));
        try {
            return Value::makeOk(Value::makeF64(std::stod(*args[0]->strData)));
        } catch (...) {
            return Value::makeErr(Value::makeString("parse_float: invalid format"));
        }
    });
    // Math builtins
    bindNative("sqrt",  [](std::vector<ValueRef> a, VM&) -> ValueRef { return Value::makeF64(std::sqrt(a[0]->prim.f64)); });
    bindNative("pow",   [](std::vector<ValueRef> a, VM&) -> ValueRef { return Value::makeF64(std::pow(a[0]->prim.f64, a[1]->prim.f64)); });
    bindNative("abs",   [](std::vector<ValueRef> a, VM&) -> ValueRef { return Value::makeF64(std::abs(a[0]->prim.f64)); });
    bindNative("floor", [](std::vector<ValueRef> a, VM&) -> ValueRef { return Value::makeF64(std::floor(a[0]->prim.f64)); });
    bindNative("ceil",  [](std::vector<ValueRef> a, VM&) -> ValueRef { return Value::makeF64(std::ceil(a[0]->prim.f64)); });
    bindNative("round", [](std::vector<ValueRef> a, VM&) -> ValueRef { return Value::makeF64(std::round(a[0]->prim.f64)); });
    bindNative("sin",   [](std::vector<ValueRef> a, VM&) -> ValueRef { return Value::makeF64(std::sin(a[0]->prim.f64)); });
    bindNative("cos",   [](std::vector<ValueRef> a, VM&) -> ValueRef { return Value::makeF64(std::cos(a[0]->prim.f64)); });
    bindNative("tan",   [](std::vector<ValueRef> a, VM&) -> ValueRef { return Value::makeF64(std::tan(a[0]->prim.f64)); });
    bindNative("log",   [](std::vector<ValueRef> a, VM&) -> ValueRef { return Value::makeF64(std::log(a[0]->prim.f64)); });
    bindNative("log2",  [](std::vector<ValueRef> a, VM&) -> ValueRef { return Value::makeF64(std::log2(a[0]->prim.f64)); });
    bindNative("log10", [](std::vector<ValueRef> a, VM&) -> ValueRef { return Value::makeF64(std::log10(a[0]->prim.f64)); });
    bindNative("exp",   [](std::vector<ValueRef> a, VM&) -> ValueRef { return Value::makeF64(std::exp(a[0]->prim.f64)); });
    bindNative("min",   [](std::vector<ValueRef> a, VM&) -> ValueRef { return a[0]->compare(*a[1]) <= 0 ? a[0] : a[1]; });
    bindNative("max",   [](std::vector<ValueRef> a, VM&) -> ValueRef { return a[0]->compare(*a[1]) >= 0 ? a[0] : a[1]; });
    // Array operations
    bindNative("push", [this](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.size() < 2 || !args[0]->arrayData) { throwException(Value::makeString("push: expected array")); return Value::makeNil(); }
        args[0]->arrayData->push_back(args[1]);
        return Value::makeNil();
    });
    bindNative("pop", [this](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.empty() || !args[0]->arrayData) { throwException(Value::makeString("pop: expected array")); return Value::makeNil(); }
        if (args[0]->arrayData->empty()) return Value::makeNone();
        auto v = args[0]->arrayData->back();
        args[0]->arrayData->pop_back();
        return Value::makeSome(v);
    });
    bindNative("sort", [](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.empty() || !args[0]->arrayData) return Value::makeNil();
        std::sort(args[0]->arrayData->begin(), args[0]->arrayData->end(),
            [](const ValueRef& a, const ValueRef& b){ return a->compare(*b) < 0; });
        return Value::makeNil();
    });
    bindNative("reverse", [](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (!args.empty() && args[0]->arrayData)
            std::reverse(args[0]->arrayData->begin(), args[0]->arrayData->end());
        return Value::makeNil();
    });
    bindNative("contains", [](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.size() < 2 || !args[0]->arrayData) return Value::makeBool(false);
        for (auto& e : *args[0]->arrayData) if (e->equals(*args[1])) return Value::makeBool(true);
        return Value::makeBool(false);
    });
    // String operations
    bindNative("str_split", [](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.size() < 2 || !args[0]->strData || !args[1]->strData) return Value::makeArray({});
        const std::string& s = *args[0]->strData;
        const std::string& delim = *args[1]->strData;
        std::vector<ValueRef> parts;
        size_t pos = 0, prev = 0;
        while ((pos = s.find(delim, prev)) != std::string::npos) {
            parts.push_back(Value::makeString(s.substr(prev, pos - prev)));
            prev = pos + delim.size();
        }
        parts.push_back(Value::makeString(s.substr(prev)));
        return Value::makeArray(std::move(parts));
    });
    bindNative("str_join", [](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.size() < 2 || !args[0]->arrayData || !args[1]->strData) return Value::makeString("");
        std::string result;
        for (size_t i = 0; i < args[0]->arrayData->size(); ++i) {
            if (i) result += *args[1]->strData;
            result += (*args[0]->arrayData)[i]->toString();
        }
        return Value::makeString(result);
    });
    bindNative("str_contains", [](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.size() < 2 || !args[0]->strData || !args[1]->strData) return Value::makeBool(false);
        return Value::makeBool(args[0]->strData->find(*args[1]->strData) != std::string::npos);
    });
    bindNative("str_starts_with", [](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.size() < 2 || !args[0]->strData || !args[1]->strData) return Value::makeBool(false);
        return Value::makeBool(args[0]->strData->rfind(*args[1]->strData, 0) == 0);
    });
    bindNative("str_ends_with", [](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.size() < 2 || !args[0]->strData || !args[1]->strData) return Value::makeBool(false);
        const std::string& s = *args[0]->strData;
        const std::string& suffix = *args[1]->strData;
        return Value::makeBool(s.size() >= suffix.size() && s.compare(s.size()-suffix.size(), suffix.size(), suffix) == 0);
    });
    bindNative("str_trim", [](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.empty() || !args[0]->strData) return Value::makeString("");
        std::string s = *args[0]->strData;
        auto l = s.find_first_not_of(" \t\r\n");
        auto r = s.find_last_not_of(" \t\r\n");
        return Value::makeString(l == std::string::npos ? "" : s.substr(l, r - l + 1));
    });
    bindNative("str_to_upper", [](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.empty() || !args[0]->strData) return Value::makeString("");
        std::string s = *args[0]->strData;
        for (char& c : s) c = std::toupper(c);
        return Value::makeString(s);
    });
    bindNative("str_to_lower", [](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.empty() || !args[0]->strData) return Value::makeString("");
        std::string s = *args[0]->strData;
        for (char& c : s) c = std::tolower(c);
        return Value::makeString(s);
    });
    bindNative("str_replace", [](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.size() < 3 || !args[0]->strData || !args[1]->strData || !args[2]->strData)
            return args.empty() ? Value::makeString("") : args[0];
        std::string s = *args[0]->strData;
        const std::string& from = *args[1]->strData;
        const std::string& to   = *args[2]->strData;
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
        return Value::makeString(s);
    });
    // Channel operations
    bindNative("make_chan", [](std::vector<ValueRef> args, VM&) -> ValueRef {
        size_t cap = (!args.empty() && args[0]->kind == ValueKind::I64) ? args[0]->prim.i64 : 0;
        return Value::makeChannel(cap);
    });
    bindNative("chan_send", [this](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.size() < 2 || args[0]->kind != ValueKind::Channel || !args[0]->chanData) {
            throwException(Value::makeString("chan_send: expected channel")); return Value::makeNil();
        }
        auto& cd = *args[0]->chanData;
        std::unique_lock<std::mutex> lk(cd.mtx);
        cd.notFull.wait(lk, [&]{ return cd.buf.size() < cd.capacity || cd.capacity == 0; });
        cd.buf.push_back(args[1]);
        cd.notEmpty.notify_one();
        return Value::makeNil();
    });
    bindNative("chan_recv", [this](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (args.empty() || args[0]->kind != ValueKind::Channel || !args[0]->chanData) {
            throwException(Value::makeString("chan_recv: expected channel")); return Value::makeNil();
        }
        auto& cd = *args[0]->chanData;
        std::unique_lock<std::mutex> lk(cd.mtx);
        cd.notEmpty.wait(lk, [&]{ return !cd.buf.empty() || cd.closed; });
        if (cd.buf.empty()) return Value::makeNone();
        auto v = cd.buf.front(); cd.buf.pop_front();
        cd.notFull.notify_one();
        return Value::makeSome(v);
    });
    bindNative("chan_close", [](std::vector<ValueRef> args, VM&) -> ValueRef {
        if (!args.empty() && args[0]->chanData) {
            std::lock_guard<std::mutex> lk(args[0]->chanData->mtx);
            args[0]->chanData->closed = true;
            args[0]->chanData->notEmpty.notify_all();
        }
        return Value::makeNil();
    });
}

// ─── IR execution engine ──────────────────────────────────────────────────────
ValueRef VM::execute(const ir::IRModule& mod, const std::string& entryFn) {
    currentModule_ = &mod;
    // Register all globals with proper type-based initialisation
    for (auto& g : mod.globals) {
        if (globals_.count(g.name)) continue; // already bound (e.g. native)
        if (!g.initializer.empty()) {
            ValueRef val;
            // Reconstruct value from raw bytes according to declared type
            switch (g.type.typeId) {
            case types::TY_BOOL: {
                bool b = false;
                std::memcpy(&b, g.initializer.data(), std::min(g.initializer.size(), (size_t)sizeof(b)));
                val = Value::makeBool(b); break;
            }
            case types::TY_I8: case types::TY_I16: case types::TY_I32: case types::TY_I64:
            case types::TY_ISIZE: {
                int64_t v = 0;
                std::memcpy(&v, g.initializer.data(), std::min(g.initializer.size(), (size_t)sizeof(v)));
                val = Value::makeI64(v); break;
            }
            case types::TY_U8: case types::TY_U16: case types::TY_U32: case types::TY_U64:
            case types::TY_USIZE: {
                uint64_t v = 0;
                std::memcpy(&v, g.initializer.data(), std::min(g.initializer.size(), (size_t)sizeof(v)));
                val = Value::makeU64(v); break;
            }
            case types::TY_F32: {
                float v = 0;
                std::memcpy(&v, g.initializer.data(), std::min(g.initializer.size(), (size_t)sizeof(v)));
                val = Value::makeF64((double)v); break;
            }
            case types::TY_F64: {
                double v = 0;
                std::memcpy(&v, g.initializer.data(), std::min(g.initializer.size(), (size_t)sizeof(v)));
                val = Value::makeF64(v); break;
            }
            case types::TY_STR: {
                std::string s(g.initializer.begin(), g.initializer.end());
                val = Value::makeString(std::move(s)); break;
            }
            default: {
                // Treat as raw byte array
                std::vector<ValueRef> bytes;
                for (uint8_t byte : g.initializer)
                    bytes.push_back(Value::makeU64(byte));
                val = Value::makeArray(std::move(bytes)); break;
            }
            }
            globals_[g.name] = val;
        } else {
            // Zero-initialise based on type
            switch (g.type.typeId) {
            case types::TY_BOOL:                                 globals_[g.name] = Value::makeBool(false); break;
            case types::TY_F32: case types::TY_F64:             globals_[g.name] = Value::makeF64(0.0); break;
            case types::TY_STR:                                  globals_[g.name] = Value::makeString(""); break;
            default:                                             globals_[g.name] = Value::makeI64(0); break;
            }
        }
    }
    // Find entry
    const ir::IRFunction* entry = nullptr;
    for (auto& fn : mod.functions) if (fn.name == entryFn || fn.mangledName == entryFn) { entry = &fn; break; }
    if (!entry) throw std::runtime_error("Entry function not found: " + entryFn);
    return execFunction(*entry, {});
}

ValueRef VM::execFunction(const ir::IRFunction& fn, std::vector<ValueRef> args) {
    CallFrame frame;
    frame.name = fn.name;
    for (size_t i = 0; i < fn.params.size() && i < args.size(); ++i)
        frame.locals[fn.params[i].name] = args[i];
    pushFrame(std::move(frame));

    std::unordered_map<ir::RegId, ValueRef> regs;
    // Bind param registers
    for (size_t i = 0; i < fn.params.size() && i < args.size(); ++i)
        regs[fn.params[i].reg] = args[i];

    ir::BlockId curBlock = 0; // entry
    ValueRef retVal = Value::makeNil();

    while (curBlock < fn.blocks.size()) {
        if (hasException()) { popFrame(); throw FppException(takeException()); }
        ir::BlockId nextBlock = ir::BLOCK_NONE;
        retVal = execBlock(fn.blocks[curBlock], fn, regs, nextBlock);
        if (nextBlock == ir::BLOCK_NONE) break; // returned
        curBlock = nextBlock;
    }

    popFrame();
    return retVal;
}

ValueRef VM::execBlock(const ir::BasicBlock& blk, const ir::IRFunction& fn,
                        std::unordered_map<ir::RegId, ValueRef>& regs,
                        ir::BlockId& nextBlock) {
    ValueRef retVal = Value::makeNil();
    for (auto& inst : blk.insts) {
        bool returned = false;
        retVal = execInst(inst, fn, regs, nextBlock, returned);
        if (returned || hasException()) return retVal;
        if (nextBlock != ir::BLOCK_NONE && inst.op != ir::Opcode::Branch &&
            inst.op != ir::Opcode::Jump && inst.op != ir::Opcode::Switch) continue;
        if (inst.op == ir::Opcode::Branch || inst.op == ir::Opcode::Jump || inst.op == ir::Opcode::Switch)
            return retVal;
    }
    return retVal;
}

ValueRef VM::execInst(const ir::Instruction& inst, const ir::IRFunction& fn,
                       std::unordered_map<ir::RegId, ValueRef>& regs,
                       ir::BlockId& nextBlock, bool& returned) {
    auto getR = [&](ir::RegId r) -> ValueRef {
        auto it = regs.find(r);
        if (it != regs.end()) return it->second;
        // Try globals
        return Value::makeNil();
    };
    auto setR = [&](ir::RegId r, ValueRef v) { if (r != ir::REG_NONE) regs[r] = std::move(v); };

    switch (inst.op) {
    case ir::Opcode::ConstInt:
        setR(inst.dest, Value::makeI64(inst.immInt)); break;
    case ir::Opcode::ConstFp:
        setR(inst.dest, Value::makeF64(inst.immFp)); break;
    case ir::Opcode::ConstStr:
        setR(inst.dest, Value::makeString(inst.immStr)); break;
    case ir::Opcode::ConstBool:
        setR(inst.dest, Value::makeBool(inst.immInt != 0)); break;
    case ir::Opcode::ConstNull:
        setR(inst.dest, Value::makeNil()); break;
    case ir::Opcode::GlobalRef: {
        auto it = globals_.find(inst.globalName);
        setR(inst.dest, it != globals_.end() ? it->second : Value::makeNil());
        break;
    }
    case ir::Opcode::Add: case ir::Opcode::Sub: case ir::Opcode::Mul:
    case ir::Opcode::Div: case ir::Opcode::Mod: case ir::Opcode::Pow:
    case ir::Opcode::BitAnd: case ir::Opcode::BitOr: case ir::Opcode::BitXor:
    case ir::Opcode::Shl: case ir::Opcode::Shr: case ir::Opcode::Ushr:
    case ir::Opcode::Eq: case ir::Opcode::Ne: case ir::Opcode::Lt:
    case ir::Opcode::Le: case ir::Opcode::Gt: case ir::Opcode::Ge:
    case ir::Opcode::And: case ir::Opcode::Or: {
        auto lhs = getR(inst.operands[0]);
        auto rhs = getR(inst.operands[1]);
        setR(inst.dest, applyBinop(inst.op, *lhs, *rhs));
        break;
    }
    case ir::Opcode::Neg: {
        auto v = getR(inst.operands[0]);
        setR(inst.dest, applyUnop(inst.op, *v));
        break;
    }
    case ir::Opcode::Not: {
        auto v = getR(inst.operands[0]);
        setR(inst.dest, Value::makeBool(!v->isTruthy()));
        break;
    }
    case ir::Opcode::Load: {
        auto ptr = getR(inst.operands[0]);
        if (!ptr) { setR(inst.dest, Value::makeNil()); break; }
        // Dereference: if the pointer's inner is set (heap box), return that
        if (ptr->inner) { setR(inst.dest, ptr->inner); break; }
        // If the pointer is itself a raw pointer to a known struct field
        if (ptr->kind == ValueKind::Ptr && ptr->prim.rawPtr) {
            // Raw pointer: reconstruct value from the pointed-to memory by type
            switch (inst.type.typeId) {
            case types::TY_BOOL: setR(inst.dest, Value::makeBool(*reinterpret_cast<bool*>(ptr->prim.rawPtr))); break;
            case types::TY_I8:   setR(inst.dest, Value::makeI64(*reinterpret_cast<int8_t*>(ptr->prim.rawPtr))); break;
            case types::TY_I16:  setR(inst.dest, Value::makeI64(*reinterpret_cast<int16_t*>(ptr->prim.rawPtr))); break;
            case types::TY_I32:  setR(inst.dest, Value::makeI32(*reinterpret_cast<int32_t*>(ptr->prim.rawPtr))); break;
            case types::TY_I64:  setR(inst.dest, Value::makeI64(*reinterpret_cast<int64_t*>(ptr->prim.rawPtr))); break;
            case types::TY_U64:  setR(inst.dest, Value::makeU64(*reinterpret_cast<uint64_t*>(ptr->prim.rawPtr))); break;
            case types::TY_F32:  setR(inst.dest, Value::makeF64(*reinterpret_cast<float*>(ptr->prim.rawPtr))); break;
            case types::TY_F64:  setR(inst.dest, Value::makeF64(*reinterpret_cast<double*>(ptr->prim.rawPtr))); break;
            default:             setR(inst.dest, ptr); break;
            }
            break;
        }
        // Fallback: the "pointer" is actually the value itself (e.g. a Box<T>)
        setR(inst.dest, ptr);
        break;
    }
    case ir::Opcode::Store: {
        auto val = getR(inst.operands[0]);
        auto ptr = getR(inst.operands[1]);
        ptr->inner = val;
        break;
    }
    case ir::Opcode::Alloc: {
        auto box = std::make_shared<Value>(); box->kind = ValueKind::Box;
        setR(inst.dest, box);
        break;
    }
    case ir::Opcode::Call:
    case ir::Opcode::TailCall: {
        std::vector<ValueRef> args;
        for (auto r : inst.operands) args.push_back(getR(r));
        // Lookup function by callee id
        if (inst.callee < fn.id || inst.callee >= currentModule_->functions.size()) {
            // Try global
            break;
        }
        const ir::IRFunction* callee = nullptr;
        for (auto& f : currentModule_->functions) if (f.id == inst.callee) { callee = &f; break; }
        if (!callee) break;
        auto ret = execFunction(*callee, std::move(args));
        setR(inst.dest, ret);
        if (hasException()) { returned = true; return ret; }
        break;
    }
    case ir::Opcode::IndirectCall: {
        auto fn_val = getR(inst.operands[0]);
        std::vector<ValueRef> args;
        for (size_t i = 1; i < inst.operands.size(); ++i) args.push_back(getR(inst.operands[i]));
        ValueRef ret;
        if (fn_val->kind == ValueKind::NativeFunc && fn_val->nativeFn)
            ret = fn_val->nativeFn(std::move(args), *this);
        else if (fn_val->kind == ValueKind::Closure && fn_val->closureData)
            ret = fn_val->closureData->fn(std::move(args), *this);
        else ret = Value::makeNil();
        setR(inst.dest, ret);
        break;
    }
    case ir::Opcode::Intrinsic: {
        const std::string& iname = inst.intrinsicName;
        std::vector<ValueRef> args;
        for (ir::RegId r : inst.operands) args.push_back(getR(r));

        // ── Closure / function construction ──────────────────────────────────
        if (iname == "__make_closure") {
            // args[0] = function ID (i64), args[1..] = captured values
            if (!args.empty()) {
                int64_t funcId = args[0]->prim.i64;
                runtime::ClosureData cd;
                // Build capture environment
                std::unordered_map<std::string, ValueRef> captures;
                for (size_t ci = 1; ci < args.size(); ++ci)
                    captures["__cap_" + std::to_string(ci - 1)] = args[ci];
                cd.captures = std::move(captures);
                // Bind the actual function for dispatch
                const ir::IRFunction* targetFn = nullptr;
                if (currentModule_) {
                    for (auto& f : currentModule_->functions)
                        if ((int64_t)f.id == funcId) { targetFn = &f; break; }
                }
                if (targetFn) {
                    const ir::IRFunction* fnPtr = targetFn;
                    cd.fn = [this, fnPtr](std::vector<ValueRef> callArgs, VM&) -> ValueRef {
                        return execFunction(*fnPtr, std::move(callArgs));
                    };
                }
                setR(inst.dest, Value::makeClosure(std::move(cd)));
            } else {
                setR(inst.dest, Value::makeNil());
            }
            break;
        }

        // ── Array / collection construction ──────────────────────────────────
        if (iname == "__make_array") {
            setR(inst.dest, Value::makeArray(std::move(args)));
            break;
        }
        if (iname == "__make_tuple") {
            auto v = Value::makeArray(std::move(args));
            v->kind = ValueKind::Tuple;
            setR(inst.dest, v);
            break;
        }
        if (iname == "__make_map") {
            std::unordered_map<std::string, ValueRef> map;
            for (size_t i = 0; i + 1 < args.size(); i += 2)
                map[args[i]->toString()] = args[i + 1];
            setR(inst.dest, Value::makeMap(std::move(map)));
            break;
        }
        if (iname == "__make_range") {
            // args: lo, hi, inclusive_bool
            int64_t lo = args.size() > 0 ? args[0]->prim.i64 : 0;
            int64_t hi = args.size() > 1 ? args[1]->prim.i64 : 0;
            bool inc   = args.size() > 2 && args[2]->isTruthy();
            std::vector<ValueRef> elems;
            for (int64_t i = lo; i < hi + (inc ? 1 : 0); ++i)
                elems.push_back(Value::makeI64(i));
            setR(inst.dest, Value::makeArray(std::move(elems)));
            break;
        }

        // ── Option / Result helpers ──────────────────────────────────────────
        if (iname == "__is_ok") {
            bool isOk = !args.empty() && args[0]->kind == ValueKind::Result_Ok;
            setR(inst.dest, Value::makeBool(isOk));
            break;
        }
        if (iname == "__is_err") {
            bool isErr = !args.empty() && args[0]->kind == ValueKind::Result_Err;
            setR(inst.dest, Value::makeBool(isErr));
            break;
        }
        if (iname == "__is_some") {
            bool isSome = !args.empty() && args[0]->kind == ValueKind::Option_Some;
            setR(inst.dest, Value::makeBool(isSome));
            break;
        }
        if (iname == "__is_none") {
            bool isNone = args.empty() || args[0]->kind == ValueKind::Option_None;
            setR(inst.dest, Value::makeBool(isNone));
            break;
        }
        if (iname == "__unwrap") {
            if (!args.empty()) {
                auto& v = args[0];
                if (v->kind == ValueKind::Result_Ok)   setR(inst.dest, v->inner ? v->inner : Value::makeNil());
                else if (v->kind == ValueKind::Option_Some) setR(inst.dest, v->inner ? v->inner : Value::makeNil());
                else if (v->kind == ValueKind::Result_Err) {
                    throwException(v->errVal ? v->errVal : Value::makeString("unwrap called on Err"));
                    setR(inst.dest, Value::makeNil());
                } else {
                    setR(inst.dest, v);
                }
            }
            break;
        }
        if (iname == "__unwrap_or") {
            if (args.size() >= 2) {
                auto& v = args[0]; auto& def = args[1];
                if (v->kind == ValueKind::Result_Ok || v->kind == ValueKind::Option_Some)
                    setR(inst.dest, v->inner ? v->inner : def);
                else setR(inst.dest, def);
            }
            break;
        }

        // ── Generator / yield ────────────────────────────────────────────────
        if (iname == "__yield") {
            // Yield is implemented as storing the value and marking the frame
            if (!args.empty()) callStack_.back().returnVal = args[0];
            // In a coroutine context this would suspend; here we record it
            setR(inst.dest, Value::makeNil());
            break;
        }

        // ── Method dispatch ───────────────────────────────────────────────────
        if (iname.substr(0, 9) == "__method_") {
            std::string methodName = iname.substr(9);
            // args[0] = self (struct/class value)
            if (!args.empty()) {
                auto& self = args[0];
                // Look up method in the struct's method table
                if (self->structData) {
                    auto mit = self->structData->methods.find(methodName);
                    if (mit != self->structData->methods.end()) {
                        auto& mfn = mit->second;
                        std::vector<ValueRef> callArgs(args.begin(), args.end());
                        if (mfn->kind == ValueKind::NativeFunc)
                            setR(inst.dest, mfn->nativeFn(std::move(callArgs), *this));
                        else if (mfn->kind == ValueKind::Closure && mfn->closureData)
                            setR(inst.dest, mfn->closureData->fn(std::move(callArgs), *this));
                        else setR(inst.dest, Value::makeNil());
                        break;
                    }
                }
                // Look up method in registered classes
                for (auto& clsEntry : classes_) { auto& cls = clsEntry.second;
                    auto mit = cls.methodTable.find(methodName);
                    if (mit != cls.methodTable.end()) {
                        auto& mfn = mit->second;
                        if (mfn->kind == ValueKind::NativeFunc)
                            setR(inst.dest, mfn->nativeFn(args, *this));
                        else if (mfn->kind == ValueKind::Closure && mfn->closureData)
                            setR(inst.dest, mfn->closureData->fn(args, *this));
                        break;
                    }
                }
                // String methods via native dispatch
                if (self->kind == ValueKind::String) {
                    auto it2 = globals_.find("str_" + methodName);
                    if (it2 != globals_.end() && it2->second->kind == ValueKind::NativeFunc)
                        setR(inst.dest, it2->second->nativeFn(args, *this));
                }
            }
            if (inst.dest != ir::REG_NONE && regs.find(inst.dest) == regs.end())
                setR(inst.dest, Value::makeNil());
            break;
        }

        // ── Bounds check ─────────────────────────────────────────────────────
        if (iname == "__bounds_check") {
            if (args.size() >= 2) {
                int64_t idx = args[0]->prim.i64;
                int64_t len = args[1]->prim.i64;
                if (idx < 0 || idx >= len)
                    throwException(Value::makeString(
                        "index out of bounds: index " + std::to_string(idx) +
                        " is out of range [0, " + std::to_string(len) + ")"));
            }
            setR(inst.dest, Value::makeNil());
            break;
        }

        // ── Fallthrough: look up in global native registry ────────────────────
        {
            auto it = globals_.find(iname);
            if (it != globals_.end() && it->second->kind == ValueKind::NativeFunc) {
                setR(inst.dest, it->second->nativeFn(std::move(args), *this));
            } else if (inst.dest != ir::REG_NONE) {
                setR(inst.dest, Value::makeNil());
            }
        }
        break;
    }
    case ir::Opcode::Return: {
        ValueRef ret = inst.operands.empty() ? Value::makeNil() : getR(inst.operands[0]);
        returned = true;
        return ret;
    }
    case ir::Opcode::Jump:
        nextBlock = inst.target;
        break;
    case ir::Opcode::Branch: {
        auto cond = getR(inst.operands[0]);
        nextBlock = cond->isTruthy() ? inst.target : inst.altTarget;
        break;
    }
    case ir::Opcode::Switch: {
        auto val = getR(inst.operands[0]);
        int64_t v = val->kind == ValueKind::I64 ? val->prim.i64 : val->prim.i32;
        nextBlock = inst.target; // default
        for (auto& c : inst.cases) if (c.val == v) { nextBlock = c.target; break; }
        break;
    }
    case ir::Opcode::Phi: {
        // In the interpreter we can't properly do SSA phi; use first incoming edge
        if (!inst.phiEdges.empty())
            setR(inst.dest, getR(inst.phiEdges[0].val));
        break;
    }
    case ir::Opcode::Select: {
        auto cond = getR(inst.operands[0]);
        setR(inst.dest, cond->isTruthy() ? getR(inst.operands[1]) : getR(inst.operands[2]));
        break;
    }
    case ir::Opcode::GEP:
    case ir::Opcode::FieldPtr: {
        auto base = getR(inst.operands[0]);
        if (!base) { setR(inst.dest, Value::makeNil()); break; }

        if (inst.op == ir::Opcode::FieldPtr) {
            // Struct field pointer: return a Box containing the field value
            if (base->structData) {
                uint32_t idx = inst.fieldIdx;
                auto& fields = base->structData->fields;
                if (idx < fields.size()) {
                    auto it = fields.begin();
                    std::advance(it, idx);
                    auto box = std::make_shared<Value>();
                    box->kind = ValueKind::Box;
                    box->inner = it->second;
                    setR(inst.dest, box);
                } else {
                    setR(inst.dest, Value::makeNil());
                }
            } else {
                // For non-struct types, wrap in a box that aliases the base
                auto box = std::make_shared<Value>();
                box->kind = ValueKind::Box;
                box->inner = base;
                setR(inst.dest, box);
            }
        } else {
            // GEP: array/slice element pointer
            // operands[0] = base, operands[1..] = indices
            ValueRef cur = base;
            bool gepError = false;
            for (size_t gidx = 1; gidx < inst.operands.size() && !gepError; ++gidx) {
                auto idxVal = getR(inst.operands[gidx]);
                int64_t i = idxVal ? idxVal->prim.i64 : 0;
                if (cur->arrayData) {
                    auto& arr = *cur->arrayData;
                    if (i >= 0 && i < (int64_t)arr.size()) {
                        auto box = std::make_shared<Value>();
                        box->kind = ValueKind::Box;
                        box->inner = arr[i];
                        cur = box;
                    } else {
                        throwException(Value::makeString(
                            "index out of bounds: index " + std::to_string(i) +
                            " for array of length " + std::to_string(arr.size())));
                        setR(inst.dest, Value::makeNil());
                        gepError = true;
                    }
                } else if (cur->strData) {
                    auto& s = *cur->strData;
                    if (i >= 0 && i < (int64_t)s.size()) {
                        auto box = std::make_shared<Value>();
                        box->kind = ValueKind::Box;
                        box->inner = Value::makeU64((uint8_t)s[i]);
                        cur = box;
                    } else {
                        throwException(Value::makeString(
                            "string index out of bounds: index " + std::to_string(i) +
                            " for string of length " + std::to_string(s.size())));
                        setR(inst.dest, Value::makeNil());
                        gepError = true;
                    }
                } else {
                    if (cur->prim.rawPtr) {
                        auto box = std::make_shared<Value>();
                        box->kind = ValueKind::Ptr;
                        box->prim.rawPtr = reinterpret_cast<char*>(cur->prim.rawPtr) + i;
                        cur = box;
                    }
                }
            }
            if (!gepError) setR(inst.dest, cur);
        }
        break;
    }
    case ir::Opcode::Trunc: case ir::Opcode::Ext: case ir::Opcode::FpToInt:
    case ir::Opcode::IntToFp: case ir::Opcode::Bitcast: {
        auto val = getR(inst.operands[0]);
        setR(inst.dest, applyCast(inst.op, val, inst.type.typeId));
        break;
    }
    case ir::Opcode::SpawnTask: {
        auto fn_val = getR(inst.operands[0]);
        std::vector<ValueRef> args;
        for (size_t i = 1; i < inst.operands.size(); ++i) args.push_back(getR(inst.operands[i]));
        auto spawnArgs = args; // copy to avoid shadowing
        auto future = scheduler_->spawn([fn_val, spawnArgs, this]() mutable -> ValueRef {
            if (fn_val->kind == ValueKind::NativeFunc && fn_val->nativeFn)
                return fn_val->nativeFn(spawnArgs, *this);
            return Value::makeNil();
        });
        auto fv = std::make_shared<Value>(); fv->kind = ValueKind::Future;
        fv->futureData = std::make_shared<std::future<ValueRef>>(std::move(future));
        setR(inst.dest, fv);
        break;
    }
    case ir::Opcode::Await: {
        auto futVal = getR(inst.operands[0]);
        if (futVal->kind == ValueKind::Future && futVal->futureData) {
            setR(inst.dest, futVal->futureData->get());
        } else {
            setR(inst.dest, futVal);
        }
        break;
    }
    case ir::Opcode::ChanSend: {
        auto chan = getR(inst.operands[0]);
        auto val  = getR(inst.operands[1]);
        if (chan->kind == ValueKind::Channel && chan->chanData) {
            auto& cd = *chan->chanData;
            std::unique_lock<std::mutex> lk(cd.mtx);
            cd.buf.push_back(val);
            cd.notEmpty.notify_one();
        }
        break;
    }
    case ir::Opcode::ChanRecv: {
        auto chan = getR(inst.operands[0]);
        if (chan->kind == ValueKind::Channel && chan->chanData) {
            auto& cd = *chan->chanData;
            std::unique_lock<std::mutex> lk(cd.mtx);
            cd.notEmpty.wait(lk, [&]{ return !cd.buf.empty() || cd.closed; });
            if (!cd.buf.empty()) {
                setR(inst.dest, Value::makeSome(cd.buf.front()));
                cd.buf.pop_front();
                cd.notFull.notify_one();
            } else {
                setR(inst.dest, Value::makeNone());
            }
        }
        break;
    }
    case ir::Opcode::Throw: {
        throwException(getR(inst.operands[0]));
        returned = true;
        break;
    }
    default:
        break;
    }
    return Value::makeNil();
}

ValueRef VM::applyBinop(ir::Opcode op, const Value& lhs, const Value& rhs) {
    // Helper: get numeric value as double
    auto toD = [](const Value& v) -> double {
        switch (v.kind) {
        case ValueKind::I8:  return v.prim.i8;   case ValueKind::I16: return v.prim.i16;
        case ValueKind::I32: return v.prim.i32;  case ValueKind::I64: return v.prim.i64;
        case ValueKind::U8:  return v.prim.u8;   case ValueKind::U16: return v.prim.u16;
        case ValueKind::U32: return v.prim.u32;  case ValueKind::U64: return (double)v.prim.u64;
        case ValueKind::F32: return v.prim.f32;  case ValueKind::F64: return v.prim.f64;
        case ValueKind::Bool: return v.prim.b ? 1.0 : 0.0;
        default: return 0.0;
        }
    };
    auto toI = [](const Value& v) -> int64_t {
        switch (v.kind) {
        case ValueKind::I8:  return v.prim.i8;  case ValueKind::I16: return v.prim.i16;
        case ValueKind::I32: return v.prim.i32; case ValueKind::I64: return v.prim.i64;
        case ValueKind::U8:  return v.prim.u8;  case ValueKind::U16: return v.prim.u16;
        case ValueKind::U32: return v.prim.u32; case ValueKind::U64: return (int64_t)v.prim.u64;
        case ValueKind::Bool: return v.prim.b ? 1 : 0;
        default: return 0;
        }
    };

    bool isIntOp = lhs.kind != ValueKind::F32 && lhs.kind != ValueKind::F64 &&
                   rhs.kind != ValueKind::F32 && rhs.kind != ValueKind::F64;

    // String concat
    if (op == ir::Opcode::Add && lhs.kind == ValueKind::String && rhs.kind == ValueKind::String) {
        std::string s = (lhs.strData ? *lhs.strData : "") + (rhs.strData ? *rhs.strData : "");
        return Value::makeString(std::move(s));
    }

    switch (op) {
    case ir::Opcode::Add: return isIntOp ? Value::makeI64(toI(lhs)+toI(rhs)) : Value::makeF64(toD(lhs)+toD(rhs));
    case ir::Opcode::Sub: return isIntOp ? Value::makeI64(toI(lhs)-toI(rhs)) : Value::makeF64(toD(lhs)-toD(rhs));
    case ir::Opcode::Mul: return isIntOp ? Value::makeI64(toI(lhs)*toI(rhs)) : Value::makeF64(toD(lhs)*toD(rhs));
    case ir::Opcode::Div:
        if (isIntOp) { int64_t d=toI(rhs); return Value::makeI64(d?toI(lhs)/d:0); }
        return Value::makeF64(toD(lhs)/toD(rhs));
    case ir::Opcode::Mod: { int64_t d=toI(rhs); return Value::makeI64(d?toI(lhs)%d:0); }
    case ir::Opcode::Pow: return Value::makeF64(std::pow(toD(lhs), toD(rhs)));
    case ir::Opcode::BitAnd: return Value::makeI64(toI(lhs)&toI(rhs));
    case ir::Opcode::BitOr:  return Value::makeI64(toI(lhs)|toI(rhs));
    case ir::Opcode::BitXor: return Value::makeI64(toI(lhs)^toI(rhs));
    case ir::Opcode::Shl:    return Value::makeI64(toI(lhs)<<toI(rhs));
    case ir::Opcode::Shr:    return Value::makeI64(toI(lhs)>>toI(rhs));
    case ir::Opcode::Ushr:   return Value::makeI64((int64_t)((uint64_t)toI(lhs)>>toI(rhs)));
    case ir::Opcode::Eq:     return Value::makeBool(lhs.equals(rhs));
    case ir::Opcode::Ne:     return Value::makeBool(!lhs.equals(rhs));
    case ir::Opcode::Lt:     return Value::makeBool(lhs.compare(rhs)<0);
    case ir::Opcode::Le:     return Value::makeBool(lhs.compare(rhs)<=0);
    case ir::Opcode::Gt:     return Value::makeBool(lhs.compare(rhs)>0);
    case ir::Opcode::Ge:     return Value::makeBool(lhs.compare(rhs)>=0);
    case ir::Opcode::And:    return Value::makeBool(lhs.isTruthy()&&rhs.isTruthy());
    case ir::Opcode::Or:     return Value::makeBool(lhs.isTruthy()||rhs.isTruthy());
    default:                 return Value::makeNil();
    }
}

ValueRef VM::applyUnop(ir::Opcode op, const Value& val) {
    switch (op) {
    case ir::Opcode::Neg:
        if (val.kind == ValueKind::F64) return Value::makeF64(-val.prim.f64);
        if (val.kind == ValueKind::I64) return Value::makeI64(-val.prim.i64);
        return Value::makeI64(-(int64_t)val.prim.i32);
    case ir::Opcode::BitNot:
        return Value::makeI64(~(int64_t)val.prim.i64);
    case ir::Opcode::Not:
        return Value::makeBool(!val.isTruthy());
    default:
        return Value::makeNil();
    }
}

ValueRef VM::applyCast(ir::Opcode op, ValueRef val, types::TypeId targetTy) {
    double d = 0;
    int64_t i = 0;
    if (val->kind == ValueKind::F64) d = val->prim.f64;
    else if (val->kind == ValueKind::I64) { d = (double)val->prim.i64; i = val->prim.i64; }
    else if (val->kind == ValueKind::I32) { d = val->prim.i32; i = val->prim.i32; }
    switch (targetTy) {
    case types::TY_I8:  { auto vi = Value::makeI64(i); vi->kind = ValueKind::I8; vi->prim.i8 = static_cast<int8_t>(i); return vi; }
    case types::TY_I16: { auto vi = Value::makeI64(i); vi->kind = ValueKind::I16; vi->prim.i16 = static_cast<int16_t>(i); return vi; }
    case types::TY_I32: return Value::makeI32((int32_t)i);
    case types::TY_I64: return Value::makeI64(i);
    case types::TY_U64: return Value::makeU64((uint64_t)i);
    case types::TY_F32: { auto v = Value::makeF64((float)d); v->kind = ValueKind::F32; return v; }
    case types::TY_F64: return Value::makeF64(d);
    case types::TY_BOOL: return Value::makeBool(i != 0);
    default:            return val;
    }
}

ValueRef VM::eval(const std::string& source) {
    Lexer lx(source, "<eval>");
    auto toks = lx.tokenize();
    Parser parser(std::move(toks), "<eval>");
    auto mod = parser.parse();
    if (!parser.errors().empty()) {
        throw std::runtime_error("Parse error: " + parser.errors()[0].what());
    }
    types::TypeRegistry reg;
    types::SemanticAnalyser sem(reg);
    sem.analyse(mod);
    codegen::CodeGenerator cg(reg, sem);
    auto irMod = cg.generate(mod);
    ir::OptPipeline::makeO1().run(irMod);
    return execute(irMod, "main");
}



} // namespace runtime
} // namespace fpp
