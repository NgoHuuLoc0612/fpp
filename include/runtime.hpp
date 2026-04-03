#pragma once
#ifdef _WIN32
#  ifndef _USE_MATH_DEFINES
#    define _USE_MATH_DEFINES
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define _CRT_SECURE_NO_WARNINGS
#endif
#include "types.hpp"
#include "ir.hpp"
#include <memory>
#include <vector>
#include <unordered_map>
#include <string>
#include <variant>
#include <functional>
#include <optional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <future>
#include <any>

namespace fpp {
namespace runtime {

// ─── Forward declarations ──────────────────────────────────────────────────
struct Value;
struct Object;
struct FppClass;
class  Heap;
class  VM;
using ValueRef = std::shared_ptr<Value>;
using ObjRef   = std::shared_ptr<Object>;

// ─── Primitive Value ───────────────────────────────────────────────────────
enum class ValueKind : uint8_t {
    Nil, Bool, I8, I16, I32, I64, I128, ISize,
    U8, U16, U32, U64, U128, USize,
    F32, F64, Char,
    String, Bytes,
    Array, Tuple, Map, Set,
    Struct, Enum, Class,
    Function, Closure, NativeFunc,
    Ptr, Ref, Box,
    Future, Task, Channel,
    Error, Result_Ok, Result_Err,
    Option_Some, Option_None,
    Iterator, Generator,
};

using NativeFunction = std::function<ValueRef(std::vector<ValueRef>, VM&)>;

struct EnumVariant {
    std::string name;
    uint32_t    tag;
    std::vector<ValueRef> payload;
};

struct StructData {
    const FppClass*                           klass;  // non-owning
    std::unordered_map<std::string, ValueRef> fields;
    std::unordered_map<std::string, ValueRef> methods; // bound methods
};

struct ClosureData {
    std::function<ValueRef(std::vector<ValueRef>, VM&)> fn;
    std::unordered_map<std::string, ValueRef>           captures;
};

struct ChannelData {
    std::deque<ValueRef>     buf;
    size_t                   capacity;
    std::mutex               mtx;
    std::condition_variable  notFull;
    std::condition_variable  notEmpty;
    bool                     closed = false;
    std::atomic<size_t>      senders{0};
};

struct GeneratorState {
    enum class Status { Running, Yielded, Done };
    std::function<ValueRef(VM&)>   resume;
    std::optional<ValueRef>        yieldVal;
    Status                         status = Status::Running;
};

struct Value {
    ValueKind kind;
    types::TypeId typeId = types::TY_UNKNOWN;

    // Flat primitive storage
    union {
        bool     b;
        int8_t   i8;
        int16_t  i16;
        int32_t  i32;
        int64_t  i64;
        uint8_t  u8;
        uint16_t u16;
        uint32_t u32;
        uint64_t u64;
        float    f32;
        double   f64;
        char32_t ch;
        void*    rawPtr;
    } prim{};

    // Heap-allocated data
    std::shared_ptr<std::string>      strData;
    std::shared_ptr<std::vector<uint8_t>> bytesData;
    std::shared_ptr<std::vector<ValueRef>> arrayData;
    std::shared_ptr<std::unordered_map<std::string, ValueRef>> mapData;
    std::shared_ptr<EnumVariant>      enumData;
    std::shared_ptr<StructData>       structData;
    std::shared_ptr<ClosureData>      closureData;
    std::shared_ptr<ChannelData>      chanData;
    std::shared_ptr<GeneratorState>   genData;
    std::shared_ptr<std::future<ValueRef>> futureData;
    NativeFunction                    nativeFn;
    ValueRef                          inner; // Box<T>, Option<T>, Result<T>
    ValueRef                          errVal;

    // ── Constructors ──────────────────────────────────────────────────────
    static ValueRef makeNil();
    static ValueRef makeBool(bool v);
    static ValueRef makeI32(int32_t v);
    static ValueRef makeI64(int64_t v);
    static ValueRef makeU64(uint64_t v);
    static ValueRef makeF64(double v);
    static ValueRef makeChar(char32_t c);
    static ValueRef makeString(std::string s);
    static ValueRef makeArray(std::vector<ValueRef> elems);
    static ValueRef makeMap(std::unordered_map<std::string,ValueRef> m);
    static ValueRef makeStruct(const FppClass* klass, std::unordered_map<std::string,ValueRef> fields);
    static ValueRef makeEnum(std::string name, uint32_t tag, std::vector<ValueRef> payload);
    static ValueRef makeClosure(ClosureData data);
    static ValueRef makeNative(NativeFunction fn);
    static ValueRef makeChannel(size_t cap);
    static ValueRef makeSome(ValueRef v);
    static ValueRef makeNone();
    static ValueRef makeOk(ValueRef v);
    static ValueRef makeErr(ValueRef e);
    static ValueRef makeRawPtr(void* p);

    // ── Helpers ───────────────────────────────────────────────────────────
    bool        isNil()     const noexcept { return kind == ValueKind::Nil; }
    bool        isTruthy()  const noexcept;
    std::string toString()  const;
    bool        equals(const Value& o) const noexcept;
    int         compare(const Value& o) const;
    ValueRef    deepCopy()  const;
    size_t      hash()      const noexcept;
};

// ─── Runtime Class Descriptor ──────────────────────────────────────────────
struct FppClass {
    std::string   name;
    types::TypeId typeId;
    const FppClass* base = nullptr;
    std::vector<const FppClass*> interfaces;
    std::unordered_map<std::string, types::TypeId> fieldTypes;
    std::unordered_map<std::string, ValueRef>      methodTable; // vtable
    std::unordered_map<std::string, ValueRef>      staticMethods;
    std::function<ValueRef(VM&, std::vector<ValueRef>)> constructor;
    std::function<void(StructData&)>               destructor;
    bool isFinal = false;
};

// ─── Call Frame ────────────────────────────────────────────────────────────
struct CallFrame {
    std::string                               name;
    std::unordered_map<std::string, ValueRef> locals;
    std::unordered_map<std::string, ValueRef> captures;
    ValueRef                                  returnVal;
    size_t                                    pc;       // program counter into IR bytecode
    SourceLocation                            callSite;
    bool                                      isAsync = false;
};

// ─── Exception System ──────────────────────────────────────────────────────
struct FppException : public std::exception {
    ValueRef   value;
    std::string message;
    std::vector<std::pair<std::string, SourceLocation>> stackTrace;

    explicit FppException(ValueRef v);
    const char* what() const noexcept override { return message.c_str(); }
    void pushFrame(std::string name, SourceLocation loc);
};

// ─── Task / Async runtime ──────────────────────────────────────────────────
class TaskScheduler {
public:
    explicit TaskScheduler(size_t numWorkers = std::thread::hardware_concurrency());
    ~TaskScheduler();

    std::future<ValueRef> spawn(std::function<ValueRef()> task);
    void                  shutdown();
    size_t                pendingTasks() const { return pending_.load(); }
    void                  runMainLoop();  // cooperative scheduling for single-threaded mode

private:
    std::vector<std::thread>          workers_;
    std::deque<std::packaged_task<ValueRef()>> queue_;
    std::mutex                        queueMtx_;
    std::condition_variable           cv_;
    std::atomic<bool>                 stopping_{false};
    std::atomic<size_t>               pending_{0};
};

// ─── Memory Manager / Heap ─────────────────────────────────────────────────
class GCHeap {
public:
    explicit GCHeap(size_t initialBytes = 64 * 1024 * 1024 /* 64MB */);
    ~GCHeap();

    void*  alloc(size_t bytes, size_t align = 8);
    void   free(void* ptr);
    void   gc();           // full GC cycle
    void   gcMinor();      // incremental / minor GC
    void   addRoot(ValueRef* root);
    void   removeRoot(ValueRef* root);
    size_t heapUsed()  const { return used_.load(); }
    size_t heapTotal() const { return total_; }
    void   setThreshold(size_t bytes) { gcThreshold_ = bytes; }

private:
    struct Block { void* ptr; size_t size; bool marked; };
    std::vector<Block>  blocks_;
    std::set<ValueRef*> roots_;
    std::atomic<size_t> used_{0};
    size_t              total_;
    size_t              gcThreshold_;
    std::mutex          heapMtx_;

    void mark();
    void sweep();
    void markValue(ValueRef& v, std::set<void*>& visited);
};

// ─── VM / Interpreter ──────────────────────────────────────────────────────
class VM {
public:
    explicit VM(size_t stackSize = 8 * 1024 * 1024 /* 8MB */,
                size_t workers   = std::thread::hardware_concurrency());
    ~VM();

    // Execute a compiled IR module
    ValueRef execute(const ir::IRModule& mod, const std::string& entryFn = "main");

    // REPL-mode: evaluate individual expressions/statements
    ValueRef eval(const std::string& source);

    // Register a native function into the global namespace
    void bindNative(const std::string& name, NativeFunction fn);
    void bindClass(FppClass cls);
    void bindModule(const std::string& modName, std::unordered_map<std::string,ValueRef> exports);

    // Stack & frame access
    CallFrame&       topFrame();
    const CallFrame& topFrame() const;
    void             pushFrame(CallFrame f);
    void             popFrame();
    size_t           stackDepth() const { return callStack_.size(); }

    // Value access helpers
    ValueRef         getLocal(const std::string& name) const;
    void             setLocal(const std::string& name, ValueRef v);
    ValueRef         getGlobal(const std::string& name) const;
    void             setGlobal(const std::string& name, ValueRef v);

    // Scheduling
    TaskScheduler&   scheduler() { return *scheduler_; }
    GCHeap&          heap()      { return *heap_; }

    // Error handling
    void             throwException(ValueRef v);
    bool             hasException() const { return !!pendingException_; }
    ValueRef         takeException();

    // Standard streams
    void             setStdout(std::function<void(const std::string&)> fn) { stdoutFn_ = fn; }
    void             setStderr(std::function<void(const std::string&)> fn) { stderrFn_ = fn; }
    void             setStdin(std::function<std::string()> fn)             { stdinFn_  = fn; }
    void             print(const std::string& s);
    void             printErr(const std::string& s);
    std::string      readLine();

private:
    std::vector<CallFrame>                              callStack_;
    std::unordered_map<std::string, ValueRef>           globals_;
    std::unordered_map<std::string, FppClass>           classes_;
    std::unique_ptr<TaskScheduler>                      scheduler_;
    std::unique_ptr<GCHeap>                             heap_;
    std::optional<ValueRef>                             pendingException_;
    std::function<void(const std::string&)>             stdoutFn_;
    std::function<void(const std::string&)>             stderrFn_;
    std::function<std::string()>                        stdinFn_;

    // IR execution engine
    ValueRef  execFunction(const ir::IRFunction& fn, std::vector<ValueRef> args);
    ValueRef  execBlock(const ir::BasicBlock& blk, const ir::IRFunction& fn,
                        std::unordered_map<ir::RegId, ValueRef>& regs,
                        ir::BlockId& nextBlock);
    ValueRef  execInst(const ir::Instruction& inst, const ir::IRFunction& fn,
                       std::unordered_map<ir::RegId, ValueRef>& regs,
                       ir::BlockId& nextBlock, bool& returned);
    ValueRef  applyBinop(ir::Opcode op, const Value& lhs, const Value& rhs);
    ValueRef  applyUnop(ir::Opcode op, const Value& val);
    ValueRef  applyCast(ir::Opcode op, ValueRef val, types::TypeId targetTy);
    ValueRef  resolveGlobal(const std::string& name, const ir::IRModule& mod);
    void      registerBuiltins();
    const ir::IRModule* currentModule_ = nullptr;
};

} // namespace runtime
} // namespace fpp
