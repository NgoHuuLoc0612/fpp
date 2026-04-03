#ifdef _WIN32
#  ifndef _USE_MATH_DEFINES
#    define _USE_MATH_DEFINES
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define _CRT_SECURE_NO_WARNINGS
#endif
#include <pybind11/pybind11.h>
#include <stdexcept>
#include <string>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include <pybind11/complex.h>

#include "../include/lexer.hpp"
#include "../include/parser.hpp"
#include "../include/types.hpp"
#include "../include/ir.hpp"
#include "../include/codegen.hpp"
#include "../include/runtime.hpp"
// stdlib is linked from fpp_core

namespace py = pybind11;

// Forward declaration — defined in stdlib/stdlib.cpp
namespace fpp { namespace stdlib { void registerAll(runtime::VM& vm); } }
using namespace fpp;

// ─── Python-facing Value conversion ──────────────────────────────────────────
static py::object valueToPy(const runtime::ValueRef& v) {
    if (!v) return py::none();
    switch (v->kind) {
    case runtime::ValueKind::Nil:         return py::none();
    case runtime::ValueKind::Bool:        return py::bool_(v->prim.b);
    case runtime::ValueKind::I8:          return py::int_((int)v->prim.i8);
    case runtime::ValueKind::I16:         return py::int_((int)v->prim.i16);
    case runtime::ValueKind::I32:         return py::int_(v->prim.i32);
    case runtime::ValueKind::I64:         return py::int_(v->prim.i64);
    case runtime::ValueKind::ISize:       return py::int_(v->prim.i64);
    case runtime::ValueKind::U8:          return py::int_((unsigned)v->prim.u8);
    case runtime::ValueKind::U16:         return py::int_((unsigned)v->prim.u16);
    case runtime::ValueKind::U32:         return py::int_(v->prim.u32);
    case runtime::ValueKind::U64:         return py::int_(v->prim.u64);
    case runtime::ValueKind::USize:       return py::int_(v->prim.u64);
    case runtime::ValueKind::F32:         return py::float_((double)v->prim.f32);
    case runtime::ValueKind::F64:         return py::float_(v->prim.f64);
    case runtime::ValueKind::Char:        return py::str(std::string(1, (char)v->prim.ch));
    case runtime::ValueKind::String:      return v->strData ? py::str(*v->strData) : py::str("");
    case runtime::ValueKind::Bytes: {
        if (!v->bytesData) return py::bytes("", 0);
        return py::bytes(reinterpret_cast<const char*>(v->bytesData->data()), v->bytesData->size());
    }
    case runtime::ValueKind::Array:
    case runtime::ValueKind::Tuple: {
        py::list lst;
        if (v->arrayData) for (auto& e : *v->arrayData) lst.append(valueToPy(e));
        return lst;
    }
    case runtime::ValueKind::Map: {
        py::dict d;
        if (v->mapData) for (auto& [k, val] : *v->mapData) d[py::str(k)] = valueToPy(val);
        return d;
    }
    case runtime::ValueKind::Set: {
        py::set s;
        if (v->arrayData) for (auto& e : *v->arrayData) s.add(valueToPy(e));
        return s;
    }
    case runtime::ValueKind::Struct:
    case runtime::ValueKind::Class: {
        py::dict d;
        if (v->structData) {
            if (v->structData->klass)
                d["__class__"] = py::str(v->structData->klass->name);
            for (auto& [k, val] : v->structData->fields) d[py::str(k)] = valueToPy(val);
        }
        return d;
    }
    case runtime::ValueKind::Enum: {
        py::dict d;
        if (v->enumData) {
            d["variant"] = py::str(v->enumData->name);
            d["tag"]     = py::int_(v->enumData->tag);
            py::list payload;
            for (auto& p : v->enumData->payload) payload.append(valueToPy(p));
            d["payload"] = payload;
        }
        return d;
    }
    case runtime::ValueKind::Option_None: return py::none();
    case runtime::ValueKind::Option_Some: return valueToPy(v->inner);
    case runtime::ValueKind::Result_Ok:   return py::make_tuple(py::bool_(true),  valueToPy(v->inner));
    case runtime::ValueKind::Result_Err:  return py::make_tuple(py::bool_(false), valueToPy(v->errVal));
    case runtime::ValueKind::Function:
    case runtime::ValueKind::Closure:
    case runtime::ValueKind::NativeFunc:  return py::str("<fn>");
    case runtime::ValueKind::Ptr:         return py::int_(reinterpret_cast<uintptr_t>(v->prim.rawPtr));
    case runtime::ValueKind::Ref:
    case runtime::ValueKind::Box:         return v->inner ? valueToPy(v->inner) : py::none();
    case runtime::ValueKind::Future:      return py::str("<future>");
    case runtime::ValueKind::Task:        return py::str("<task>");
    case runtime::ValueKind::Channel:     return py::str("<channel>");
    case runtime::ValueKind::Error:       return py::str(v->strData ? *v->strData : "<error>");
    case runtime::ValueKind::Iterator:
    case runtime::ValueKind::Generator:   return py::str("<generator>");
    default:                              return py::str(v->toString());
    }
}

static runtime::ValueRef pyToValue(py::object obj) {
    if (obj.is_none()) return runtime::Value::makeNil();
    if (py::isinstance<py::bool_>(obj))  return runtime::Value::makeBool(obj.cast<bool>());
    if (py::isinstance<py::int_>(obj))   return runtime::Value::makeI64(obj.cast<int64_t>());
    if (py::isinstance<py::float_>(obj)) return runtime::Value::makeF64(obj.cast<double>());
    if (py::isinstance<py::str>(obj))    return runtime::Value::makeString(obj.cast<std::string>());
    if (py::isinstance<py::list>(obj)) {
        std::vector<runtime::ValueRef> elems;
        for (auto& item : obj.cast<py::list>()) elems.push_back(pyToValue(py::reinterpret_borrow<py::object>(item)));
        return runtime::Value::makeArray(std::move(elems));
    }
    if (py::isinstance<py::dict>(obj)) {
        std::unordered_map<std::string, runtime::ValueRef> map;
        for (auto& [k, v] : obj.cast<py::dict>())
            map[py::str(k).cast<std::string>()] = pyToValue(py::reinterpret_borrow<py::object>(v));
        return runtime::Value::makeMap(std::move(map));
    }
    if (py::isinstance<py::tuple>(obj)) {
        py::tuple t = obj.cast<py::tuple>();
        std::vector<runtime::ValueRef> elems;
        for (size_t i = 0; i < t.size(); ++i) elems.push_back(pyToValue(py::reinterpret_borrow<py::object>(t[i])));
        return runtime::Value::makeArray(std::move(elems));
    }
    return runtime::Value::makeString(py::str(obj).cast<std::string>());
}

// ─── Python-exposed wrappers ──────────────────────────────────────────────────
class PyLexer {
public:
    PyLexer(const std::string& src, const std::string& file = "<stdin>")
        : lexer_(src, file) {}

    py::list tokenize() {
        auto toks = lexer_.tokenize();
        py::list result;
        for (auto& t : toks) {
            py::dict d;
            d["kind"]   = (int)t.kind;
            d["lexeme"] = t.lexeme;
            d["line"]   = t.loc.line;
            d["col"]    = t.loc.col;
            result.append(d);
        }
        return result;
    }
private:
    Lexer lexer_;
};

class PyParser {
public:
    PyParser(const std::string& src, const std::string& file = "<stdin>") {
        Lexer lx(src, file);
        toks_ = lx.tokenize();
        file_ = file;
    }

    py::object parse() {
        Parser p(toks_, file_);
        mod_ = std::make_shared<ast::Module>(p.parse());
        errors_.clear();
        for (auto& e : p.errors()) errors_.push_back(e.what());
        return py::bool_(errors_.empty());
    }

    py::list getErrors() const {
        py::list l;
        for (auto& e : errors_) l.append(py::str(e));
        return l;
    }

    std::shared_ptr<ast::Module> getModule() const { return mod_; }

private:
    std::vector<Token>               toks_;
    std::string                      file_;
    std::shared_ptr<ast::Module>     mod_;
    std::vector<std::string>         errors_;
};

class PyVM {
public:
    PyVM(size_t workers = std::thread::hardware_concurrency())
        : vm_(std::make_unique<runtime::VM>(8*1024*1024, workers)) {
        stdlib::registerAll(*vm_);
    }

    // Run F++ source code and return result
    py::object run(const std::string& source) {
        try {
            Lexer lx(source, "<run>");
            auto toks = lx.tokenize();
            Parser parser(std::move(toks), "<run>");
            auto mod = parser.parse();
            if (!parser.errors().empty()) {
                throw std::runtime_error(std::string("Parse errors: ") + parser.errors()[0].what());
            }
            types::TypeRegistry reg;
            types::SemanticAnalyser sem(reg);
            sem.analyse(mod);
            codegen::CodeGenerator cg(reg, sem);
            auto irMod = cg.generate(mod);
            ir::OptPipeline::makeO2().run(irMod);
            auto result = vm_->execute(irMod);
            return valueToPy(result);
        } catch (runtime::FppException& e) {
            throw std::invalid_argument(std::string("F++ exception: ") + e.message);
        } catch (std::exception& e) {
            throw std::runtime_error(e.what());
        }
    }

    // Run with optimisation level
    py::object runOpt(const std::string& source, int optLevel = 2) {
        try {
            Lexer lx(source, "<run>");
            auto toks = lx.tokenize();
            Parser parser(std::move(toks), "<run>");
            auto mod = parser.parse();
            if (!parser.errors().empty())
                throw std::runtime_error(std::string("Parse errors: ") + parser.errors()[0].what());
            types::TypeRegistry reg;
            types::SemanticAnalyser sem(reg);
            sem.analyse(mod);
            codegen::CodeGenerator cg(reg, sem);
            auto irMod = cg.generate(mod);
            switch (optLevel) {
            case 0: ir::OptPipeline::makeO0().run(irMod); break;
            case 1: ir::OptPipeline::makeO1().run(irMod); break;
            case 3: ir::OptPipeline::makeO3().run(irMod); break;
            default: ir::OptPipeline::makeO2().run(irMod); break;
            }
            return valueToPy(vm_->execute(irMod));
        } catch (std::exception& e) { throw std::runtime_error(e.what()); }
    }

    // Transpile F++ source to C++
    std::string transpileCpp(const std::string& source) {
        Lexer lx(source, "<trans>");
        auto toks = lx.tokenize();
        Parser parser(std::move(toks), "<trans>");
        auto mod = parser.parse();
        if (!parser.errors().empty())
            throw std::runtime_error(std::string("Parse error: ") + parser.errors()[0].what());
        types::TypeRegistry reg;
        types::SemanticAnalyser sem(reg);
        sem.analyse(mod);
        codegen::CodeGenerator cg(reg, sem);
        auto irMod = cg.generate(mod);
        codegen::CppTranspileBackend backend;
        return backend.transpile(irMod);
    }

    // Dump IR
    std::string dumpIR(const std::string& source, int optLevel = 0) {
        Lexer lx(source, "<ir>");
        auto toks = lx.tokenize();
        Parser parser(std::move(toks), "<ir>");
        auto mod = parser.parse();
        if (!parser.errors().empty())
            throw std::runtime_error(std::string("Parse error: ") + parser.errors()[0].what());
        types::TypeRegistry reg;
        types::SemanticAnalyser sem(reg);
        sem.analyse(mod);
        codegen::CodeGenerator cg(reg, sem);
        auto irMod = cg.generate(mod);
        if (optLevel > 0) ir::OptPipeline::makeO2().run(irMod);
        return ir::dumpModule(irMod);
    }

    // Bind a Python callable as a native F++ function
    void bindNative(const std::string& name, py::object callable) {
        vm_->bindNative(name, [callable](std::vector<runtime::ValueRef> args, runtime::VM&) -> runtime::ValueRef {
            py::gil_scoped_acquire gil;
            py::list pyArgs;
            for (auto& a : args) pyArgs.append(valueToPy(a));
            try {
                py::object result = callable(*pyArgs);
                return pyToValue(result);
            } catch (py::error_already_set& e) {
                return runtime::Value::makeErr(runtime::Value::makeString(e.what()));
            }
        });
    }

    // Set stdout handler
    void setStdout(py::object fn) {
        vm_->setStdout([fn](const std::string& s) {
            py::gil_scoped_acquire gil;
            fn(py::str(s));
        });
    }

    // Set stderr handler
    void setStderr(py::object fn) {
        vm_->setStderr([fn](const std::string& s) {
            py::gil_scoped_acquire gil;
            fn(py::str(s));
        });
    }

    // Get GC stats
    py::dict gcStats() const {
        py::dict d;
        d["heap_used"]  = vm_->heap().heapUsed();
        d["heap_total"] = vm_->heap().heapTotal();
        return d;
    }

    void runGC() { vm_->heap().gc(); }

    // Type checking utilities
    static py::list tokenize(const std::string& src) {
        PyLexer lx(src);
        return lx.tokenize();
    }

    static bool typecheck(const std::string& src) {
        try {
            Lexer lx(src, "<tc>");
            auto toks = lx.tokenize();
            Parser parser(std::move(toks), "<tc>");
            auto mod = parser.parse();
            if (!parser.errors().empty()) return false;
            types::TypeRegistry reg;
            types::SemanticAnalyser sem(reg);
            return sem.analyse(mod);
        } catch (...) { return false; }
    }

    static py::list getTypeErrors(const std::string& src) {
        py::list errors;
        try {
            Lexer lx(src, "<tc>");
            auto toks = lx.tokenize();
            Parser parser(std::move(toks), "<tc>");
            auto mod = parser.parse();
            for (auto& e : parser.errors()) errors.append(py::str(e.what()));
            if (!parser.errors().empty()) return errors;
            types::TypeRegistry reg;
            types::SemanticAnalyser sem(reg);
            sem.analyse(mod);
            for (auto& d : sem.diagnostics()) {
                if (d.level >= types::Diagnostic::Level::Error)
                    errors.append(py::str(d.message));
            }
        } catch (std::exception& e) { errors.append(py::str(e.what())); }
        return errors;
    }

private:
    std::unique_ptr<runtime::VM> vm_;
};

// ─── pybind11 module ──────────────────────────────────────────────────────────
PYBIND11_MODULE(fpp_native, m) {
    m.doc() = "F++ Programming Language — native C++ engine";

    m.attr("__version__") = "1.0.0";
    m.attr("__author__")  = "F++ Team";

    // Lexer
    py::class_<PyLexer>(m, "Lexer")
        .def(py::init<const std::string&, const std::string&>(),
             py::arg("source"), py::arg("filename") = "<stdin>")
        .def("tokenize", &PyLexer::tokenize,
             "Tokenise F++ source and return list of token dicts");

    // Parser
    py::class_<PyParser>(m, "Parser")
        .def(py::init<const std::string&, const std::string&>(),
             py::arg("source"), py::arg("filename") = "<stdin>")
        .def("parse",      &PyParser::parse,     "Parse source, return True if successful")
        .def("get_errors", &PyParser::getErrors, "Return list of parse error messages");

    // VM
    py::class_<PyVM>(m, "VM")
        .def(py::init<size_t>(), py::arg("workers") = (size_t)std::thread::hardware_concurrency(),
             "Create a new F++ VM with the given number of worker threads")
        .def("run",          &PyVM::run,         py::arg("source"),
             "Compile and run F++ source code, return result as Python object")
        .def("run_opt",      &PyVM::runOpt,      py::arg("source"), py::arg("opt_level") = 2,
             "Run with specific optimisation level (0-3)")
        .def("transpile_cpp",&PyVM::transpileCpp,py::arg("source"),
             "Transpile F++ source to C++ code")
        .def("dump_ir",      &PyVM::dumpIR,      py::arg("source"), py::arg("opt_level") = 0,
             "Dump the IR for given source")
        .def("bind_native",  &PyVM::bindNative,  py::arg("name"), py::arg("callable"),
             "Bind a Python callable as a native F++ function")
        .def("set_stdout",   &PyVM::setStdout,   py::arg("fn"),
             "Redirect F++ stdout to a Python callable(str)")
        .def("set_stderr",   &PyVM::setStderr,   py::arg("fn"),
             "Redirect F++ stderr to a Python callable(str)")
        .def("gc_stats",     &PyVM::gcStats,     "Return GC statistics dict")
        .def("gc",           &PyVM::runGC,       "Trigger a full GC cycle")
        .def_static("tokenize",     &PyVM::tokenize,    py::arg("source"), "Tokenise F++ source")
        .def_static("typecheck",    &PyVM::typecheck,   py::arg("source"), "Typecheck F++ source, return True if OK")
        .def_static("get_type_errors", &PyVM::getTypeErrors, py::arg("source"), "Get type errors for F++ source");

    // Convenience function: run a snippet
    m.def("run", [](const std::string& src, int opt) {
        PyVM vm;
        return vm.runOpt(src, opt);
    }, py::arg("source"), py::arg("opt_level") = 2,
    "Convenience: create a VM and run F++ source code");

    m.def("tokenize", &PyVM::tokenize,
          "Tokenise F++ source, return list of token dicts");

    m.def("typecheck", &PyVM::typecheck,
          "Typecheck F++ source, return True if no errors");

    m.def("transpile_cpp", [](const std::string& src) {
        PyVM vm; return vm.transpileCpp(src);
    }, "Transpile F++ source to C++");

    m.def("dump_ir", [](const std::string& src, int opt) {
        PyVM vm; return vm.dumpIR(src, opt);
    }, py::arg("source"), py::arg("opt_level") = 0, "Dump IR for F++ source");
}
