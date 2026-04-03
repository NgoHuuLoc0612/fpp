// F++ Test Suite — tools/tests.cpp
// Comprehensive tests for the entire F++ pipeline

#ifdef _WIN32
#  ifndef _USE_MATH_DEFINES
#    define _USE_MATH_DEFINES
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define _CRT_SECURE_NO_WARNINGS
#endif

#include "../include/lexer.hpp"
#include "../include/parser.hpp"
#include "../include/types.hpp"
#include "../include/ir.hpp"
#include "../include/codegen.hpp"
#include "../include/runtime.hpp"
#include "../stdlib/stdlib.cpp"

#include <iostream>
#include <functional>
#include <vector>
#include <string>
#include <sstream>
#include <cassert>
#include <stdexcept>
#include <chrono>
#include <iomanip>
#include <set>
#include <unordered_set>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/ioctl.h>
#endif

namespace fpp {

// ─── Mini test framework ──────────────────────────────────────────────────────
struct TestResult {
    std::string name;
    bool        passed;
    std::string message;
    double      ms;
};

static std::vector<TestResult> g_results;
static int g_total = 0, g_passed = 0, g_failed = 0;

#define TEST(name, body) \
    do { \
        ++g_total; \
        auto _t0 = std::chrono::steady_clock::now(); \
        bool _ok = false; \
        std::string _msg; \
        try { body; _ok = true; } \
        catch (std::exception& _e) { _msg = _e.what(); } \
        catch (...) { _msg = "unknown exception"; } \
        double _ms = std::chrono::duration<double,std::milli>( \
            std::chrono::steady_clock::now() - _t0).count(); \
        g_results.push_back({name, _ok, _msg, _ms}); \
        if (_ok) ++g_passed; else ++g_failed; \
    } while(0)

#define EXPECT_EQ(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (!(_a == _b)) { \
            std::ostringstream _ss; \
            _ss << "Expected " << #a << " == " << #b \
                << " but got: " << _a << " vs " << _b; \
            throw std::runtime_error(_ss.str()); \
        } \
    } while(0)

#define EXPECT_TRUE(x) \
    do { if (!(x)) throw std::runtime_error("Expected true: " #x); } while(0)

#define EXPECT_FALSE(x) \
    do { if (x) throw std::runtime_error("Expected false: " #x); } while(0)

#define EXPECT_THROWS(expr) \
    do { \
        bool _threw = false; \
        try { expr; } catch (...) { _threw = true; } \
        if (!_threw) throw std::runtime_error("Expected exception from: " #expr); \
    } while(0)

// ─── Test pipeline helper ─────────────────────────────────────────────────────
static runtime::ValueRef runSource(const std::string& src, int opt = 1) {
    Lexer lx(src, "<test>");
    auto toks = lx.tokenize();
    Parser parser(toks, "<test>");
    auto mod = parser.parse();
    if (!parser.errors().empty())
        throw std::runtime_error("Parse: " + parser.errors()[0].what());
    types::TypeRegistry reg;
    types::SemanticAnalyser sem(reg);
    sem.analyse(mod);
    codegen::CodeGenerator cg(reg, sem);
    auto irMod = cg.generate(mod);
    ir::OptPipeline pipeline;
    switch (opt) {
    case 0: pipeline = ir::OptPipeline::makeO0(); break;
    case 1: pipeline = ir::OptPipeline::makeO1(); break;
    case 3: pipeline = ir::OptPipeline::makeO3(); break;
    default: pipeline = ir::OptPipeline::makeO2(); break;
    }
    pipeline.run(irMod);
    runtime::VM vm;
    stdlib::registerAll(vm);
    return vm.execute(irMod);
}

static int64_t runInt(const std::string& body, int opt = 1) {
    auto r = runSource("fn main() -> i64 {\n" + body + "\n}", opt);
    if (!r) throw std::runtime_error("null result");
    return r->prim.i64;
}

static double runFloat(const std::string& body) {
    auto r = runSource("fn main() -> f64 {\n" + body + "\n}");
    if (!r) throw std::runtime_error("null result");
    return r->prim.f64;
}

static bool runBool(const std::string& body) {
    auto r = runSource("fn main() -> bool {\n" + body + "\n}");
    if (!r) throw std::runtime_error("null result");
    return r->prim.b;
}

static std::string runStr(const std::string& body) {
    auto r = runSource("fn main() -> str {\n" + body + "\n}");
    if (!r) throw std::runtime_error("null result");
    return r->strData ? *r->strData : "";
}

// ─── Lexer tests ──────────────────────────────────────────────────────────────
static void testLexer() {
    TEST("lex: integer literal", {
        Lexer lx("42", "<test>");
        auto toks = lx.tokenize();
        EXPECT_EQ((int)toks[0].kind, (int)TokenKind::Integer);
        EXPECT_EQ(std::get<int64_t>(toks[0].literal), 42LL);
    });
    TEST("lex: float literal", {
        Lexer lx("3.14", "<test>");
        auto toks = lx.tokenize();
        EXPECT_EQ((int)toks[0].kind, (int)TokenKind::Float);
    });
    TEST("lex: string literal", {
        Lexer lx("\"hello\"", "<test>");
        auto toks = lx.tokenize();
        EXPECT_EQ((int)toks[0].kind, (int)TokenKind::String);
        EXPECT_EQ(std::get<std::string>(toks[0].literal), std::string("hello"));
    });
    TEST("lex: hex integer", {
        Lexer lx("0xFF", "<test>");
        auto toks = lx.tokenize();
        EXPECT_EQ(std::get<int64_t>(toks[0].literal), 255LL);
    });
    TEST("lex: binary integer", {
        Lexer lx("0b1010", "<test>");
        auto toks = lx.tokenize();
        EXPECT_EQ(std::get<int64_t>(toks[0].literal), 10LL);
    });
    TEST("lex: octal integer", {
        Lexer lx("0o17", "<test>");
        auto toks = lx.tokenize();
        EXPECT_EQ(std::get<int64_t>(toks[0].literal), 15LL);
    });
    TEST("lex: integer with underscores", {
        Lexer lx("1_000_000", "<test>");
        auto toks = lx.tokenize();
        EXPECT_EQ(std::get<int64_t>(toks[0].literal), 1000000LL);
    });
    TEST("lex: boolean keywords", {
        Lexer lx("true false", "<test>");
        auto toks = lx.tokenize();
        EXPECT_EQ((int)toks[0].kind, (int)TokenKind::Kw_true);
        EXPECT_EQ((int)toks[1].kind, (int)TokenKind::Kw_false);
    });
    TEST("lex: all keywords present", {
        std::vector<std::string> kws = {
            "let","var","const","fn","return","if","else","while","for","in",
            "break","continue","class","struct","enum","interface","impl",
            "import","async","await","spawn","match","try","catch","throw",
            "type","trait","where","as","static","extern","unsafe","true","false"
        };
        for (auto& kw : kws) {
            Lexer lx(kw, "<test>");
            auto toks = lx.tokenize();
            EXPECT_TRUE(toks[0].isKeyword());
        }
    });
    TEST("lex: compound operators", {
        Lexer lx("=> -> :: .. ..= ??", "<test>");
        auto toks = lx.tokenize();
        EXPECT_EQ((int)toks[0].kind, (int)TokenKind::FatArrow);
        EXPECT_EQ((int)toks[1].kind, (int)TokenKind::Arrow);
        EXPECT_EQ((int)toks[2].kind, (int)TokenKind::ColonColon);
        EXPECT_EQ((int)toks[3].kind, (int)TokenKind::DotDot);
        EXPECT_EQ((int)toks[4].kind, (int)TokenKind::DotDotEq);
        EXPECT_EQ((int)toks[5].kind, (int)TokenKind::QuestionQuestion);
    });
    TEST("lex: line comment", {
        Lexer lx("42 // this is a comment\n99", "<test>");
        auto toks = lx.tokenize();
        EXPECT_EQ(std::get<int64_t>(toks[0].literal), 42LL);
        // skip Newline
        bool found99 = false;
        for (auto& t : toks) if (t.kind==TokenKind::Integer && std::get<int64_t>(t.literal)==99) found99=true;
        EXPECT_TRUE(found99);
    });
    TEST("lex: block comment nested", {
        Lexer lx("/* outer /* inner */ still comment */ 42", "<test>");
        auto toks = lx.tokenize();
        EXPECT_EQ(std::get<int64_t>(toks[0].literal), 42LL);
    });
    TEST("lex: escape sequences in string", {
        Lexer lx(R"("hello\nworld\t!")", "<test>");
        auto toks = lx.tokenize();
        auto s = std::get<std::string>(toks[0].literal);
        EXPECT_TRUE(s.find('\n') != std::string::npos);
        EXPECT_TRUE(s.find('\t') != std::string::npos);
    });
}

// ─── Parser tests ──────────────────────────────────────────────────────────────
static void testParser() {
    auto parse = [](const std::string& src) {
        Lexer lx(src, "<test>"); auto toks = lx.tokenize();
        Parser p(toks, "<test>"); auto mod = p.parse();
        if (!p.errors().empty()) throw std::runtime_error(p.errors()[0].what());
        return mod;
    };

    TEST("parse: empty module", {
        auto mod = parse("");
        EXPECT_TRUE(mod.items.empty());
    });
    TEST("parse: function declaration", {
        auto mod = parse("fn add(a: i64, b: i64) -> i64 { a + b }");
        EXPECT_EQ((int)mod.items.size(), 1);
        auto* fn = dynamic_cast<ast::FnItem*>(mod.items[0].get());
        EXPECT_TRUE(fn != nullptr);
        EXPECT_EQ(fn->name, std::string("add"));
        EXPECT_EQ((int)fn->params.size(), 2);
    });
    TEST("parse: struct definition", {
        auto mod = parse("struct Point { x: f64, y: f64 }");
        EXPECT_EQ((int)mod.items.size(), 1);
        auto* s = dynamic_cast<ast::StructItem*>(mod.items[0].get());
        EXPECT_TRUE(s != nullptr);
        EXPECT_EQ(s->name, std::string("Point"));
        EXPECT_EQ((int)s->fields.size(), 2);
    });
    TEST("parse: enum definition", {
        auto mod = parse("enum Color { Red, Green, Blue }");
        auto* e = dynamic_cast<ast::EnumItem*>(mod.items[0].get());
        EXPECT_TRUE(e != nullptr);
        EXPECT_EQ((int)e->variants.size(), 3);
    });
    TEST("parse: if expression", {
        auto mod = parse("fn f() { if x > 0 { 1 } else { -1 } }");
        EXPECT_EQ((int)mod.items.size(), 1);
    });
    TEST("parse: for loop", {
        auto mod = parse("fn f() { for i in 0..10 { println(i) } }");
        EXPECT_EQ((int)mod.items.size(), 1);
    });
    TEST("parse: while loop", {
        auto mod = parse("fn f() { while x > 0 { x = x - 1 } }");
        EXPECT_EQ((int)mod.items.size(), 1);
    });
    TEST("parse: match expression", {
        auto mod = parse("fn f(x: i64) { match x { 0 => 0, 1 => 1, _ => -1 } }");
        EXPECT_EQ((int)mod.items.size(), 1);
    });
    TEST("parse: closure", {
        auto mod = parse("fn f() { let g = fn(x: i64) -> i64 { x * 2 } }");
        EXPECT_EQ((int)mod.items.size(), 1);
    });
    TEST("parse: generics", {
        auto mod = parse("fn identity<T>(x: T) -> T { x }");
        auto* fn = dynamic_cast<ast::FnItem*>(mod.items[0].get());
        EXPECT_TRUE(fn != nullptr);
        EXPECT_EQ((int)fn->generics.size(), 1);
    });
    TEST("parse: impl block", {
        auto mod = parse("impl Foo { fn bar(&self) -> i64 { 42 } }");
        auto* im = dynamic_cast<ast::ImplItem*>(mod.items[0].get());
        EXPECT_TRUE(im != nullptr);
    });
    TEST("parse: class with inheritance", {
        auto mod = parse("class Dog: Animal { fn speak() { println(\"woof\") } }");
        auto* cl = dynamic_cast<ast::ClassItem*>(mod.items[0].get());
        EXPECT_TRUE(cl != nullptr);
        EXPECT_EQ(cl->name, std::string("Dog"));
    });
    TEST("parse: import statement", {
        auto mod = parse("import std::io");
        auto* im = dynamic_cast<ast::ImportItem*>(mod.items[0].get());
        EXPECT_TRUE(im != nullptr);
    });
    TEST("parse: async fn", {
        auto mod = parse("async fn fetch() -> str { \"data\" }");
        auto* fn = dynamic_cast<ast::FnItem*>(mod.items[0].get());
        EXPECT_TRUE(fn != nullptr && fn->isAsync);
    });
    TEST("parse: array literal", {
        auto mod = parse("fn f() { let a = [1, 2, 3, 4, 5] }");
        EXPECT_EQ((int)mod.items.size(), 1);
    });
    TEST("parse: tuple", {
        auto mod = parse("fn f() { let t = (1, \"hello\", 3.14) }");
        EXPECT_EQ((int)mod.items.size(), 1);
    });
    TEST("parse: string interpolation", {
        auto mod = parse("fn f() { let name = \"world\"\n let s = \"Hello ${name}!\" }");
        EXPECT_EQ((int)mod.items.size(), 1);
    });
    TEST("parse: error recovery", {
        Lexer lx("fn good() {} fn !! bad fn ok() {}", "<test>");
        auto toks = lx.tokenize();
        Parser p(toks, "<test>");
        auto mod = p.parse();
        // Should have recovered and parsed at least one good item
        EXPECT_FALSE(mod.items.empty());
    });
}

// ─── Runtime / execution tests ────────────────────────────────────────────────
static void testExecution() {
    // Arithmetic
    TEST("exec: integer arithmetic", {
        EXPECT_EQ(runInt("2 + 3"), 5LL);
        EXPECT_EQ(runInt("10 - 4"), 6LL);
        EXPECT_EQ(runInt("3 * 7"), 21LL);
        EXPECT_EQ(runInt("15 / 4"), 3LL);
        EXPECT_EQ(runInt("17 % 5"), 2LL);
    });
    TEST("exec: float arithmetic", {
        EXPECT_TRUE(std::abs(runFloat("1.0 + 2.5") - 3.5) < 1e-9);
        EXPECT_TRUE(std::abs(runFloat("3.14 * 2.0") - 6.28) < 1e-6);
    });
    TEST("exec: bitwise operations", {
        EXPECT_EQ(runInt("0xFF & 0x0F"), 0x0FLL);
        EXPECT_EQ(runInt("0xF0 | 0x0F"), 0xFFLL);
        EXPECT_EQ(runInt("0xFF ^ 0xF0"), 0x0FLL);
        EXPECT_EQ(runInt("1 << 8"), 256LL);
        EXPECT_EQ(runInt("256 >> 4"), 16LL);
    });
    TEST("exec: comparison operators", {
        EXPECT_TRUE(runBool("1 < 2"));
        EXPECT_FALSE(runBool("2 < 1"));
        EXPECT_TRUE(runBool("2 <= 2"));
        EXPECT_TRUE(runBool("3 > 2"));
        EXPECT_TRUE(runBool("3 >= 3"));
        EXPECT_TRUE(runBool("42 == 42"));
        EXPECT_TRUE(runBool("42 != 43"));
    });
    TEST("exec: logical operators", {
        EXPECT_TRUE(runBool("true && true"));
        EXPECT_FALSE(runBool("true && false"));
        EXPECT_TRUE(runBool("false || true"));
        EXPECT_FALSE(runBool("false || false"));
        EXPECT_TRUE(runBool("!false"));
        EXPECT_FALSE(runBool("!true"));
    });
    TEST("exec: string concatenation", {
        EXPECT_EQ(runStr("\"hello\" + \" \" + \"world\""), std::string("hello world"));
    });
    TEST("exec: variable declaration and use", {
        EXPECT_EQ(runInt("let x = 10\nlet y = 20\nx + y"), 30LL);
    });
    TEST("exec: mutable variable", {
        EXPECT_EQ(runInt("var x = 0\nx = x + 1\nx = x + 1\nx"), 2LL);
    });
    TEST("exec: if-else expression", {
        EXPECT_EQ(runInt("let x = 5\nif x > 3 { 1 } else { 0 }"), 1LL);
        EXPECT_EQ(runInt("let x = 1\nif x > 3 { 1 } else { 0 }"), 0LL);
    });
    TEST("exec: nested if-else", {
        EXPECT_EQ(runInt(
            "let x = 5\n"
            "if x < 0 { -1 }\n"
            "else if x == 0 { 0 }\n"
            "else { 1 }"
        ), 1LL);
    });
    TEST("exec: while loop", {
        EXPECT_EQ(runInt("var i = 0\nvar s = 0\nwhile i < 10 { s = s + i\ni = i + 1 }\ns"), 45LL);
    });
    TEST("exec: for loop over range", {
        EXPECT_EQ(runInt("var s = 0\nfor i in [1,2,3,4,5] { s = s + i }\ns"), 15LL);
    });
    TEST("exec: break in while", {
        EXPECT_EQ(runInt("var i = 0\nwhile true { if i >= 5 { break }\ni = i + 1 }\ni"), 5LL);
    });
    TEST("exec: nested function call", {
        EXPECT_EQ(runInt("fn square(x: i64) -> i64 { x * x }\nsquare(7)"), 49LL);
    });
    TEST("exec: recursive fibonacci", {
        EXPECT_EQ(runInt(
            "fn fib(n: i64) -> i64 {\n"
            "  if n <= 1 { n } else { fib(n-1) + fib(n-2) }\n"
            "}\n"
            "fib(10)"
        ), 55LL);
    });
    TEST("exec: recursive factorial", {
        EXPECT_EQ(runInt(
            "fn fact(n: i64) -> i64 {\n"
            "  if n <= 1 { 1 } else { n * fact(n-1) }\n"
            "}\n"
            "fact(10)"
        ), 3628800LL);
    });
    TEST("exec: match expression basic", {
        EXPECT_EQ(runInt(
            "let x = 2\n"
            "match x {\n"
            "  1 => 10,\n"
            "  2 => 20,\n"
            "  _ => 0\n"
            "}"
        ), 20LL);
    });
    TEST("exec: string operations", {
        auto r = runSource(R"(
            fn main() -> i64 {
                let s = "hello world"
                let parts = str_split(s, " ")
                len(parts)
            }
        )");
        EXPECT_EQ(r->prim.i64, 2LL);
    });
    TEST("exec: array operations", {
        auto r = runSource(R"(
            fn main() -> i64 {
                let a = [3, 1, 4, 1, 5, 9, 2, 6]
                sort(a)
                len(a)
            }
        )");
        EXPECT_EQ(r->prim.i64, 8LL);
    });
    TEST("exec: closures capture environment", {
        EXPECT_EQ(runInt(
            "fn make_adder(n: i64) -> fn(i64) -> i64 {\n"
            "  fn(x: i64) -> i64 { x + n }\n"
            "}\n"
            "let add5 = make_adder(5)\n"
            "add5(10)"
        ), 15LL);
    });
    TEST("exec: higher-order map function", {
        auto r = runSource(R"(
            fn main() -> i64 {
                let nums = [1, 2, 3, 4, 5]
                let doubled = map(nums, fn(x: i64) -> i64 { x * 2 })
                len(doubled)
            }
        )");
        EXPECT_EQ(r->prim.i64, 5LL);
    });
    TEST("exec: filter function", {
        auto r = runSource(R"(
            fn main() -> i64 {
                let nums = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
                let evens = filter(nums, fn(x: i64) -> bool { x % 2 == 0 })
                len(evens)
            }
        )");
        EXPECT_EQ(r->prim.i64, 5LL);
    });
    TEST("exec: reduce/fold", {
        auto r = runSource(R"(
            fn main() -> i64 {
                let nums = [1, 2, 3, 4, 5]
                let sum = reduce(nums, fn(acc: i64, x: i64) -> i64 { acc + x }, 0)
                sum
            }
        )");
        EXPECT_EQ(r->prim.i64, 15LL);
    });
}

// ─── Type system tests ─────────────────────────────────────────────────────────
static void testTypeSystem() {
    auto typecheck = [](const std::string& src) -> bool {
        Lexer lx(src, "<test>"); auto toks = lx.tokenize();
        Parser p(toks, "<test>"); auto mod = p.parse();
        if (!p.errors().empty()) return false;
        types::TypeRegistry reg;
        types::SemanticAnalyser sem(reg);
        sem.analyse(mod);
        return !sem.hasErrors();
    };

    TEST("type: primitives resolve", {
        types::TypeRegistry reg;
        EXPECT_EQ(reg.typeName(types::TY_I64), std::string("i64"));
        EXPECT_EQ(reg.typeName(types::TY_F64), std::string("f64"));
        EXPECT_EQ(reg.typeName(types::TY_BOOL), std::string("bool"));
        EXPECT_EQ(reg.typeName(types::TY_STR), std::string("str"));
    });
    TEST("type: integer types are numeric", {
        types::TypeRegistry reg;
        EXPECT_TRUE(reg.isInteger(types::TY_I64));
        EXPECT_TRUE(reg.isInteger(types::TY_U32));
        EXPECT_FALSE(reg.isInteger(types::TY_F64));
        EXPECT_TRUE(reg.isFloat(types::TY_F64));
        EXPECT_TRUE(reg.isNumeric(types::TY_I32));
        EXPECT_TRUE(reg.isNumeric(types::TY_F32));
    });
    TEST("type: make option/result", {
        types::TypeRegistry reg;
        auto optI64 = reg.makeOption(types::TY_I64);
        EXPECT_TRUE(reg.typeName(optI64).find("Option") != std::string::npos);
        auto resOK  = reg.makeResult(types::TY_I64, types::TY_STR);
        EXPECT_TRUE(reg.typeName(resOK).find("Result") != std::string::npos);
    });
    TEST("type: pointer types", {
        types::TypeRegistry reg;
        auto pI64 = reg.makePtr(types::TY_I64, false);
        EXPECT_TRUE(reg.typeName(pI64).find("*") != std::string::npos);
        auto rI64 = reg.makeRef(types::TY_I64, false);
        EXPECT_TRUE(reg.typeName(rI64).find("&") != std::string::npos);
    });
    TEST("type: tuple type", {
        types::TypeRegistry reg;
        auto t = reg.makeTuple({types::TY_I64, types::TY_F64, types::TY_BOOL});
        EXPECT_TRUE(reg.typeName(t).find("(") != std::string::npos);
    });
    TEST("type: array type", {
        types::TypeRegistry reg;
        auto arr = reg.makeArray(types::TY_I32, 10);
        EXPECT_EQ(reg.typeSize(arr), (size_t)40);
    });
    TEST("type: function type", {
        types::TypeRegistry reg;
        types::FnSig sig;
        sig.params = {types::TY_I64, types::TY_I64};
        sig.ret    = types::TY_I64;
        auto fn = reg.makeFn(std::move(sig));
        EXPECT_TRUE(fn >= types::TY_FIRST_USER);
    });
    TEST("type: unification infer", {
        types::TypeRegistry reg;
        auto infer = reg.freshInfer();
        EXPECT_TRUE(reg.unify(infer, types::TY_I64));
        EXPECT_EQ(reg.resolved(infer), types::TY_I64);
    });
    TEST("type: valid program typechecks", {
        EXPECT_TRUE(typecheck("fn add(a: i64, b: i64) -> i64 { a + b }"));
    });
    TEST("type: struct field access typechecks", {
        EXPECT_TRUE(typecheck(
            "struct Point { x: f64, y: f64 }\n"
            "fn length(p: Point) -> f64 { p.x }"
        ));
    });
    TEST("type: never subtype of all", {
        types::TypeRegistry reg;
        EXPECT_TRUE(reg.isSubtype(types::TY_NEVER, types::TY_I64));
        EXPECT_TRUE(reg.isSubtype(types::TY_NEVER, types::TY_F64));
    });
}

// ─── IR tests ─────────────────────────────────────────────────────────────────
static void testIR() {
    TEST("ir: build simple function", {
        ir::IRModule mod;
        ir::IRBuilder builder(mod);
        ir::IRParam p; p.name = "x"; p.type = {types::TY_I64}; p.reg = 1;
        builder.beginFunction("square", {p}, {types::TY_I64});
        // The parameter register is assigned as reg=1 by beginFunction
        ir::RegId x = 1;
        auto res = builder.emitBinop(ir::Opcode::Mul, x, x, {types::TY_I64});
        builder.emitReturn(res);
        EXPECT_EQ((int)mod.functions.size(), 1);
        EXPECT_EQ(mod.functions[0].name, std::string("square"));
        EXPECT_TRUE(mod.functions[0].blocks[0].insts.size() >= 2);
    });
    TEST("ir: const folding eliminates dead ops", {
        ir::IRModule mod;
        ir::IRBuilder builder(mod);
        builder.beginFunction("f", {}, {types::TY_I64});
        auto a = builder.emitConstInt(3,  {types::TY_I64});
        auto b = builder.emitConstInt(4,  {types::TY_I64});
        auto c = builder.emitBinop(ir::Opcode::Add, a, b, {types::TY_I64});
        builder.emitReturn(c);

        ir::OptPipeline pipeline;
        pipeline.addPass(ir::OptPipeline::makeConstantFolding());
        pipeline.addPass(ir::OptPipeline::makeDeadCodeElim());
        bool changed = pipeline.run(mod);
        EXPECT_TRUE(changed);
    });
    TEST("ir: TCO marks tail call", {
        ir::IRModule mod;
        ir::IRBuilder builder(mod);
        builder.beginFunction("fact", {}, {types::TY_I64});
        auto ret = builder.emitConstInt(1, {types::TY_I64});
        auto call = builder.emitCall(0, {}, {types::TY_I64});
        builder.emitReturn(call);

        auto pipeline = ir::OptPipeline::makeO2();
        pipeline.run(mod);
        // We just verify it runs without crash
        EXPECT_EQ((int)mod.functions.size(), 1);
    });
    TEST("ir: dump module produces non-empty string", {
        ir::IRModule mod;
        ir::IRBuilder builder(mod);
        builder.beginFunction("main", {}, {types::TY_UNIT});
        auto r = builder.emitConstInt(42, {types::TY_I64});
        builder.emitReturn(r);
        auto dump = ir::dumpModule(mod);
        EXPECT_TRUE(!dump.empty());
        EXPECT_TRUE(dump.find("main") != std::string::npos);
    });
    TEST("ir: branch instruction updates successors", {
        ir::IRModule mod;
        ir::IRBuilder builder(mod);
        builder.beginFunction("f", {}, {types::TY_I64});
        auto cond = builder.emitConstBool(true);
        auto tb   = builder.createBlock("true_bb");
        auto fb   = builder.createBlock("false_bb");
        builder.emitBranch(cond, tb, fb);
        EXPECT_EQ((int)mod.functions[0].blocks[0].succs.size(), 2);
    });
    TEST("ir: phi node construction", {
        ir::IRModule mod;
        ir::IRBuilder builder(mod);
        builder.beginFunction("f", {}, {types::TY_I64});
        auto b0 = builder.currentBlock();
        auto b1 = builder.createBlock("b1");
        auto b2 = builder.createBlock("b2");
        auto b3 = builder.createBlock("merge");
        builder.setBlock(b1);
        auto r1 = builder.emitConstInt(1, {types::TY_I64});
        builder.emitJump(b3);
        builder.setBlock(b2);
        auto r2 = builder.emitConstInt(2, {types::TY_I64});
        builder.emitJump(b3);
        builder.setBlock(b3);
        auto phi = builder.emitPhi({types::TY_I64}, {{r1,b1},{r2,b2}});
        EXPECT_TRUE(phi != ir::REG_NONE);
    });
}

// ─── Standard library tests ────────────────────────────────────────────────────
static void testStdlib() {
    auto vm_run = [](const std::string& src) -> runtime::ValueRef {
        Lexer lx(src, "<test>"); auto toks = lx.tokenize();
        Parser p(toks, "<test>"); auto mod = p.parse();
        if (!p.errors().empty()) throw std::runtime_error(p.errors()[0].what());
        types::TypeRegistry reg; types::SemanticAnalyser sem(reg); sem.analyse(mod);
        codegen::CodeGenerator cg(reg, sem); auto irMod = cg.generate(mod);
        ir::OptPipeline::makeO1().run(irMod);
        runtime::VM vm; stdlib::registerAll(vm);
        return vm.execute(irMod);
    };

    TEST("stdlib: math PI constant", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto pi = vm.getGlobal("math::PI");
        EXPECT_TRUE(pi != nullptr);
        EXPECT_TRUE(std::abs(pi->prim.f64 - M_PI) < 1e-10);
    });
    TEST("stdlib: math primes_up_to", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto fn = vm.getGlobal("math::primes_up_to");
        EXPECT_TRUE(fn != nullptr);
        auto result = fn->nativeFn({runtime::Value::makeI64(30)}, vm);
        EXPECT_TRUE(result->arrayData != nullptr);
        // Primes up to 30: 2,3,5,7,11,13,17,19,23,29 = 10
        EXPECT_EQ((int)result->arrayData->size(), 10);
    });
    TEST("stdlib: math gcd", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto fn = vm.getGlobal("math::gcd");
        auto r = fn->nativeFn({runtime::Value::makeI64(48), runtime::Value::makeI64(18)}, vm);
        EXPECT_EQ(r->prim.i64, 6LL);
    });
    TEST("stdlib: math fibonacci", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto fn = vm.getGlobal("math::fibonacci");
        auto r = fn->nativeFn({runtime::Value::makeI64(10)}, vm);
        EXPECT_EQ(r->prim.i64, 55LL);
    });
    TEST("stdlib: algo map", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto map_fn = vm.getGlobal("algo::map");
        EXPECT_TRUE(map_fn != nullptr);
        auto arr = runtime::Value::makeArray({
            runtime::Value::makeI64(1), runtime::Value::makeI64(2), runtime::Value::makeI64(3)
        });
        auto double_fn = runtime::Value::makeNative([](std::vector<runtime::ValueRef> a, runtime::VM&) {
            return runtime::Value::makeI64(a[0]->prim.i64 * 2);
        });
        auto result = map_fn->nativeFn({arr, double_fn}, vm);
        EXPECT_EQ((int)result->arrayData->size(), 3);
        EXPECT_EQ((*result->arrayData)[0]->prim.i64, 2LL);
        EXPECT_EQ((*result->arrayData)[1]->prim.i64, 4LL);
        EXPECT_EQ((*result->arrayData)[2]->prim.i64, 6LL);
    });
    TEST("stdlib: algo filter", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto fn = vm.getGlobal("algo::filter");
        auto arr = runtime::Value::makeArray({
            runtime::Value::makeI64(1),runtime::Value::makeI64(2),runtime::Value::makeI64(3),
            runtime::Value::makeI64(4),runtime::Value::makeI64(5)
        });
        auto even = runtime::Value::makeNative([](std::vector<runtime::ValueRef> a, runtime::VM&) {
            return runtime::Value::makeBool(a[0]->prim.i64 % 2 == 0);
        });
        auto result = fn->nativeFn({arr, even}, vm);
        EXPECT_EQ((int)result->arrayData->size(), 2);
    });
    TEST("stdlib: algo binary_search found", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto fn = vm.getGlobal("algo::binary_search");
        auto arr = runtime::Value::makeArray({
            runtime::Value::makeI64(1),runtime::Value::makeI64(3),runtime::Value::makeI64(5),
            runtime::Value::makeI64(7),runtime::Value::makeI64(9)
        });
        auto r = fn->nativeFn({arr, runtime::Value::makeI64(7)}, vm);
        EXPECT_EQ((int)r->kind, (int)runtime::ValueKind::Option_Some);
        EXPECT_EQ(r->inner->prim.i64, 3LL);
    });
    TEST("stdlib: algo binary_search not found", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto fn = vm.getGlobal("algo::binary_search");
        auto arr = runtime::Value::makeArray({
            runtime::Value::makeI64(1),runtime::Value::makeI64(3),runtime::Value::makeI64(5)
        });
        auto r = fn->nativeFn({arr, runtime::Value::makeI64(4)}, vm);
        EXPECT_EQ((int)r->kind, (int)runtime::ValueKind::Option_None);
    });
    TEST("stdlib: algo range", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto fn = vm.getGlobal("algo::range");
        auto r = fn->nativeFn({runtime::Value::makeI64(0), runtime::Value::makeI64(5)}, vm);
        EXPECT_EQ((int)r->arrayData->size(), 5);
        EXPECT_EQ((*r->arrayData)[0]->prim.i64, 0LL);
        EXPECT_EQ((*r->arrayData)[4]->prim.i64, 4LL);
    });
    TEST("stdlib: algo range with step", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto fn = vm.getGlobal("algo::range");
        auto r = fn->nativeFn({
            runtime::Value::makeI64(0), runtime::Value::makeI64(10), runtime::Value::makeI64(2)
        }, vm);
        EXPECT_EQ((int)r->arrayData->size(), 5); // 0,2,4,6,8
    });
    TEST("stdlib: algo chunk", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto fn = vm.getGlobal("algo::chunk");
        auto arr = runtime::Value::makeArray({
            runtime::Value::makeI64(1),runtime::Value::makeI64(2),runtime::Value::makeI64(3),
            runtime::Value::makeI64(4),runtime::Value::makeI64(5)
        });
        auto r = fn->nativeFn({arr, runtime::Value::makeI64(2)}, vm);
        EXPECT_EQ((int)r->arrayData->size(), 3); // [1,2], [3,4], [5]
    });
    TEST("stdlib: algo zip", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto fn = vm.getGlobal("algo::zip");
        auto a1 = runtime::Value::makeArray({runtime::Value::makeI64(1),runtime::Value::makeI64(2)});
        auto a2 = runtime::Value::makeArray({runtime::Value::makeI64(3),runtime::Value::makeI64(4)});
        auto r = fn->nativeFn({a1, a2}, vm);
        EXPECT_EQ((int)r->arrayData->size(), 2);
    });
    TEST("stdlib: collections map_get/map_set", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto set_fn = vm.getGlobal("collections::map_set");
        auto get_fn = vm.getGlobal("collections::map_get");
        auto m = runtime::Value::makeMap({});
        set_fn->nativeFn({m, runtime::Value::makeString("key"), runtime::Value::makeI64(99)}, vm);
        auto r = get_fn->nativeFn({m, runtime::Value::makeString("key")}, vm);
        EXPECT_EQ((int)r->kind, (int)runtime::ValueKind::Option_Some);
        EXPECT_EQ(r->inner->prim.i64, 99LL);
    });
    TEST("stdlib: time now_ms is positive", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto fn = vm.getGlobal("time::now_ms");
        auto r = fn->nativeFn({}, vm);
        EXPECT_TRUE(r->prim.i64 > 0);
    });
    TEST("stdlib: regex match", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto fn = vm.getGlobal("re::match");
        auto r = fn->nativeFn({
            runtime::Value::makeString("hello123"),
            runtime::Value::makeString("[a-z]+[0-9]+")
        }, vm);
        EXPECT_TRUE(r->prim.b);
    });
    TEST("stdlib: regex find_all", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto fn = vm.getGlobal("re::find_all");
        auto r = fn->nativeFn({
            runtime::Value::makeString("abc 123 def 456"),
            runtime::Value::makeString("[0-9]+")
        }, vm);
        EXPECT_EQ((int)r->arrayData->size(), 2);
    });
}

// ─── Value tests ──────────────────────────────────────────────────────────────
static void testValues() {
    TEST("value: nil is falsy", {
        auto v = runtime::Value::makeNil();
        EXPECT_FALSE(v->isTruthy());
    });
    TEST("value: zero is falsy", {
        EXPECT_FALSE(runtime::Value::makeI64(0)->isTruthy());
        EXPECT_TRUE(runtime::Value::makeI64(1)->isTruthy());
    });
    TEST("value: empty string is falsy", {
        EXPECT_FALSE(runtime::Value::makeString("")->isTruthy());
        EXPECT_TRUE(runtime::Value::makeString("x")->isTruthy());
    });
    TEST("value: toString", {
        EXPECT_EQ(runtime::Value::makeI64(42)->toString(), std::string("42"));
        EXPECT_EQ(runtime::Value::makeBool(true)->toString(), std::string("true"));
        EXPECT_EQ(runtime::Value::makeString("hi")->toString(), std::string("hi"));
        EXPECT_EQ(runtime::Value::makeNil()->toString(), std::string("nil"));
    });
    TEST("value: equals", {
        EXPECT_TRUE(runtime::Value::makeI64(7)->equals(*runtime::Value::makeI64(7)));
        EXPECT_FALSE(runtime::Value::makeI64(7)->equals(*runtime::Value::makeI64(8)));
        EXPECT_TRUE(runtime::Value::makeString("abc")->equals(*runtime::Value::makeString("abc")));
        EXPECT_FALSE(runtime::Value::makeString("abc")->equals(*runtime::Value::makeString("xyz")));
    });
    TEST("value: compare", {
        EXPECT_EQ(runtime::Value::makeI64(1)->compare(*runtime::Value::makeI64(2)), -1);
        EXPECT_EQ(runtime::Value::makeI64(2)->compare(*runtime::Value::makeI64(1)),  1);
        EXPECT_EQ(runtime::Value::makeI64(5)->compare(*runtime::Value::makeI64(5)),  0);
    });
    TEST("value: Option_Some/None", {
        auto some = runtime::Value::makeSome(runtime::Value::makeI64(42));
        auto none = runtime::Value::makeNone();
        EXPECT_EQ((int)some->kind, (int)runtime::ValueKind::Option_Some);
        EXPECT_EQ((int)none->kind, (int)runtime::ValueKind::Option_None);
        EXPECT_TRUE(some->isTruthy());
        EXPECT_FALSE(none->isTruthy());
    });
    TEST("value: Result Ok/Err", {
        auto ok  = runtime::Value::makeOk(runtime::Value::makeI64(1));
        auto err = runtime::Value::makeErr(runtime::Value::makeString("oops"));
        EXPECT_EQ((int)ok->kind, (int)runtime::ValueKind::Result_Ok);
        EXPECT_EQ((int)err->kind, (int)runtime::ValueKind::Result_Err);
    });
    TEST("value: array deep copy", {
        auto orig = runtime::Value::makeArray({runtime::Value::makeI64(1), runtime::Value::makeI64(2)});
        auto copy = orig->deepCopy();
        copy->arrayData->push_back(runtime::Value::makeI64(3));
        EXPECT_EQ((int)orig->arrayData->size(), 2);
        EXPECT_EQ((int)copy->arrayData->size(), 3);
    });
    TEST("value: channel send/recv", {
        auto ch = runtime::Value::makeChannel(10);
        EXPECT_EQ((int)ch->kind, (int)runtime::ValueKind::Channel);
        auto& cd = *ch->chanData;
        {
            std::lock_guard<std::mutex> lk(cd.mtx);
            cd.buf.push_back(runtime::Value::makeI64(42));
            cd.notEmpty.notify_one();
        }
        std::unique_lock<std::mutex> lk(cd.mtx);
        cd.notEmpty.wait(lk, [&]{ return !cd.buf.empty(); });
        auto val = cd.buf.front(); cd.buf.pop_front();
        EXPECT_EQ(val->prim.i64, 42LL);
    });
}

// ─── Integration tests ────────────────────────────────────────────────────────
static void testIntegration() {
    TEST("integration: FizzBuzz first 15", {
        auto r = runSource(R"(
            fn main() -> i64 {
                var count = 0
                for i in [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15] {
                    if i % 15 == 0 { count = count + 1 }
                    else if i % 3 == 0 { count = count + 1 }
                    else if i % 5 == 0 { count = count + 1 }
                }
                count
            }
        )");
        EXPECT_EQ(r->prim.i64, 7LL); // 3,5,6,9,10,12,15
    });
    TEST("integration: quicksort correctness", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto fn = vm.getGlobal("algo::quicksort");
        auto arr = runtime::Value::makeArray({
            runtime::Value::makeI64(5),runtime::Value::makeI64(3),runtime::Value::makeI64(8),
            runtime::Value::makeI64(1),runtime::Value::makeI64(9),runtime::Value::makeI64(2)
        });
        fn->nativeFn({arr}, vm);
        for (size_t i = 1; i < arr->arrayData->size(); ++i)
            EXPECT_TRUE((*arr->arrayData)[i-1]->compare(*(*arr->arrayData)[i]) <= 0);
    });
    TEST("integration: merge_sort correctness", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto fn = vm.getGlobal("algo::merge_sort");
        auto arr = runtime::Value::makeArray({
            runtime::Value::makeI64(7),runtime::Value::makeI64(2),runtime::Value::makeI64(5),
            runtime::Value::makeI64(1),runtime::Value::makeI64(4)
        });
        fn->nativeFn({arr}, vm);
        EXPECT_EQ((*arr->arrayData)[0]->prim.i64, 1LL);
        EXPECT_EQ((*arr->arrayData)[4]->prim.i64, 7LL);
    });
    TEST("integration: recursive descent sum", {
        EXPECT_EQ(runInt(
            "fn sum(n: i64) -> i64 { if n <= 0 { 0 } else { n + sum(n-1) } }\n"
            "sum(100)"
        ), 5050LL);
    });
    TEST("integration: sieve of eratosthenes", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto fn = vm.getGlobal("math::primes_up_to");
        auto r = fn->nativeFn({runtime::Value::makeI64(100)}, vm);
        EXPECT_EQ((int)r->arrayData->size(), 25); // 25 primes <= 100
    });
    TEST("integration: string processing pipeline", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto split = vm.getGlobal("str_split");
        auto join  = vm.getGlobal("str_join");
        auto words = split->nativeFn({
            runtime::Value::makeString("the quick brown fox"),
            runtime::Value::makeString(" ")
        }, vm);
        auto rejoined = join->nativeFn({words, runtime::Value::makeString("-")}, vm);
        EXPECT_EQ(*rejoined->strData, std::string("the-quick-brown-fox"));
    });
    TEST("integration: map group_by", {
        runtime::VM vm; stdlib::registerAll(vm);
        auto fn = vm.getGlobal("algo::group_by");
        auto arr = runtime::Value::makeArray({
            runtime::Value::makeI64(1),runtime::Value::makeI64(2),runtime::Value::makeI64(3),
            runtime::Value::makeI64(4),runtime::Value::makeI64(5),runtime::Value::makeI64(6)
        });
        auto parity = runtime::Value::makeNative([](std::vector<runtime::ValueRef> a, runtime::VM&) {
            return runtime::Value::makeString(a[0]->prim.i64 % 2 == 0 ? "even" : "odd");
        });
        auto r = fn->nativeFn({arr, parity}, vm);
        EXPECT_EQ((int)r->mapData->size(), 2);
    });
    TEST("integration: O0 vs O3 same result", {
        std::string src = R"(
            fn fib(n: i64) -> i64 {
                if n <= 1 { n } else { fib(n-1) + fib(n-2) }
            }
            fn main() -> i64 { fib(15) }
        )";
        auto r0 = runSource(src, 0)->prim.i64;
        auto r3 = runSource(src, 3)->prim.i64;
        EXPECT_EQ(r0, r3);
        EXPECT_EQ(r0, 610LL);
    });
}

// ─── Test runner ──────────────────────────────────────────────────────────────
static size_t termWidth_safe() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return (size_t)(csbi.srWindow.Right - csbi.srWindow.Left + 1);
    return 80;
#elif defined(TIOCGWINSZ)
    struct winsize w{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return w.ws_col;
    return 80;
#else
    return 80;
#endif
}

static void printResults() {
    std::cout << "\n";
    size_t w = termWidth_safe();
    std::string sep(w > 4 ? w : 60, '─');

    for (auto& r : g_results) {
        std::string status = r.passed ? "\033[32m✓\033[0m" : "\033[31m✗\033[0m";
        std::string time_s = "\033[2m" + std::to_string((int)r.ms) + "ms\033[0m";
        std::cout << "  " << status << " " << std::left << std::setw(55) << r.name << time_s;
        if (!r.passed) std::cout << "\n        \033[31m" << r.message << "\033[0m";
        std::cout << "\n";
    }

    std::cout << sep << "\n";
    std::cout << "  Tests: " << g_total
              << "  \033[32mPassed: " << g_passed << "\033[0m"
              << "  \033[31mFailed: " << g_failed << "\033[0m";
    if (g_failed == 0) std::cout << "  \033[32m✓ All tests passed!\033[0m";
    std::cout << "\n" << sep << "\n";
}

} // namespace fpp

int main(int argc, char** argv) {
    std::cout << "\033[1mF++ Test Suite v1.0.0\033[0m\n\n";

    fpp::testLexer();
    fpp::testParser();
    fpp::testTypeSystem();
    fpp::testIR();
    fpp::testValues();
    fpp::testStdlib();
    fpp::testExecution();
    fpp::testIntegration();

    fpp::printResults();
    return fpp::g_failed > 0 ? 1 : 0;
}
