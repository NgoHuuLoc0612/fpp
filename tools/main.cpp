// F++ Standalone Interpreter — main.cpp
// Enterprise-grade CLI entry point

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
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>
#include <cstring>
#include <optional>
#include <iomanip>
#include <algorithm>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/ioctl.h>
#endif

namespace fs = std::filesystem;
using namespace fpp;

// ─── Terminal utilities ───────────────────────────────────────────────────────
static bool g_color = true;

static bool termSupportsColor() {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    return h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode);
#else
    return isatty(STDOUT_FILENO) && getenv("TERM") != nullptr;
#endif
}

static std::string ansi(const std::string& code, const std::string& text) {
    if (!g_color) return text;
    return "\033[" + code + "m" + text + "\033[0m";
}

static std::string bold(const std::string& s)    { return ansi("1", s); }
static std::string dim(const std::string& s)     { return ansi("2", s); }
static std::string red(const std::string& s)     { return ansi("31", s); }
static std::string green(const std::string& s)   { return ansi("32", s); }
static std::string yellow(const std::string& s)  { return ansi("33", s); }
static std::string cyan(const std::string& s)    { return ansi("36", s); }
static std::string magenta(const std::string& s) { return ansi("35", s); }

static size_t termWidth() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return (size_t)(csbi.srWindow.Right - csbi.srWindow.Left + 1);
    return 80;
#elif defined(TIOCGWINSZ)
    struct winsize w{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) return w.ws_col;
    return 80;
#else
    return 80;
#endif
}

// ─── CLI options ──────────────────────────────────────────────────────────────
struct Options {
    std::string             file;
    std::string             evalExpr;
    int                     optLevel    = 2;
    bool                    dumpIR      = false;
    bool                    dumpTokens  = false;
    bool                    dumpAST     = false;
    bool                    transpileCpp= false;
    bool                    typeCheckOnly = false;
    bool                    benchmark   = false;
    bool                    interactive = false;
    bool                    noStdlib    = false;
    bool                    verbose     = false;
    bool                    noColor     = false;
    size_t                  workers     = std::thread::hardware_concurrency();
    std::vector<std::string> defines;
    std::vector<std::string> includePaths;
    std::vector<std::string> args;       // script args
};

static void printHelp(const char* prog) {
    std::cout << bold("F++ Programming Language") << " v1.0.0\n\n"
              << bold("USAGE:\n")
              << "  " << prog << " [options] [file] [-- args...]\n\n"
              << bold("OPTIONS:\n")
              << "  " << cyan("-e, --eval <expr>") << "       Evaluate expression\n"
              << "  " << cyan("-O, --opt <0-3>") << "         Optimisation level (default: 2)\n"
              << "  " << cyan("-i, --interactive") << "       Force interactive REPL\n"
              << "  " << cyan("--ir") << "                    Dump IR and exit\n"
              << "  " << cyan("--tokens") << "                Dump tokens and exit\n"
              << "  " << cyan("--ast") << "                   Dump AST and exit\n"
              << "  " << cyan("--cpp") << "                   Transpile to C++ and exit\n"
              << "  " << cyan("--tc, --typecheck") << "       Type check only\n"
              << "  " << cyan("--bench") << "                 Show execution timing\n"
              << "  " << cyan("--no-stdlib") << "             Don't load standard library\n"
              << "  " << cyan("--no-color") << "              Disable ANSI colours\n"
              << "  " << cyan("--workers <N>") << "           Worker threads (default: nproc)\n"
              << "  " << cyan("-D <name>=<value>") << "       Define compile-time constant\n"
              << "  " << cyan("-I <path>") << "               Add include path\n"
              << "  " << cyan("-v, --verbose") << "           Verbose output\n"
              << "  " << cyan("-h, --help") << "              Show this help\n"
              << "  " << cyan("--version") << "               Show version\n\n"
              << bold("EXAMPLES:\n")
              << "  " << prog << "                         # Start REPL\n"
              << "  " << prog << " hello.fpp               # Run file\n"
              << "  " << prog << " -e 'println(1 + 2)'     # Evaluate expression\n"
              << "  " << prog << " -O3 app.fpp             # Run with O3 optimisations\n"
              << "  " << prog << " --ir app.fpp            # Dump IR\n"
              << "  " << prog << " --cpp app.fpp           # Transpile to C++\n"
              << "  " << prog << " --tc app.fpp            # Type-check only\n"
              << "  " << prog << " app.fpp -- arg1 arg2    # Pass args to script\n"
              ;
}

static Options parseArgs(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--") {
            for (int j = i+1; j < argc; ++j) opts.args.push_back(argv[j]);
            break;
        }
        if (a == "-h" || a == "--help")       { printHelp(argv[0]); exit(0); }
        if (a == "--version")                 { std::cout << "fpp 1.0.0\n"; exit(0); }
        if (a == "--ir")                      { opts.dumpIR = true; continue; }
        if (a == "--tokens")                  { opts.dumpTokens = true; continue; }
        if (a == "--ast")                     { opts.dumpAST = true; continue; }
        if (a == "--cpp")                     { opts.transpileCpp = true; continue; }
        if (a == "--tc" || a == "--typecheck"){ opts.typeCheckOnly = true; continue; }
        if (a == "--bench")                   { opts.benchmark = true; continue; }
        if (a == "--no-stdlib")               { opts.noStdlib = true; continue; }
        if (a == "--no-color")                { opts.noColor = true; g_color = false; continue; }
        if (a == "-i" || a == "--interactive"){ opts.interactive = true; continue; }
        if (a == "-v" || a == "--verbose")    { opts.verbose = true; continue; }
        if ((a == "-e" || a == "--eval") && i+1 < argc)  { opts.evalExpr = argv[++i]; continue; }
        if ((a == "-O" || a == "--opt") && i+1 < argc)   { opts.optLevel = std::stoi(argv[++i]); continue; }
        if (a.substr(0,2) == "-O" && a.size() == 3)      { opts.optLevel = a[2]-'0'; continue; }
        if ((a == "--workers") && i+1 < argc)             { opts.workers = std::stoul(argv[++i]); continue; }
        if (a.substr(0,2) == "-D" && a.size() > 2)       { opts.defines.push_back(a.substr(2)); continue; }
        if (a.substr(0,2) == "-I" && a.size() > 2)       { opts.includePaths.push_back(a.substr(2)); continue; }
        if (a[0] != '-' && opts.file.empty())             { opts.file = a; continue; }
        std::cerr << red("Unknown option: ") << a << "\n";
    }
    return opts;
}

// ─── Compilation pipeline ────────────────────────────────────────────────────
struct CompileResult {
    bool            ok = false;
    ir::IRModule    irMod;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    double          parseMs  = 0;
    double          semMs    = 0;
    double          codegenMs= 0;
    double          optMs    = 0;
};

static CompileResult compile(const std::string& source, const std::string& filename,
                              const Options& opts) {
    CompileResult res;
    auto T = [](auto start) {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    };

    // ── Lex ──────────────────────────────────────────────────────────────────
    auto t0 = std::chrono::steady_clock::now();
    Lexer lx(source, filename);
    std::vector<Token> tokens;
    try {
        tokens = lx.tokenize();
    } catch (LexerError& e) {
        res.errors.push_back(std::string("lex: ") + e.what());
        return res;
    }

    // ── Parse ─────────────────────────────────────────────────────────────────
    Parser parser(tokens, filename);
    auto mod = parser.parse();
    res.parseMs = T(t0);

    if (!parser.errors().empty()) {
        for (auto& e : parser.errors())
            res.errors.push_back("parse[" + std::to_string(e.loc.line) + ":" +
                                  std::to_string(e.loc.col) + "]: " + e.what());
        return res;
    }

    // ── Semantic analysis ─────────────────────────────────────────────────────
    auto t1 = std::chrono::steady_clock::now();
    types::TypeRegistry reg;
    types::SemanticAnalyser sem(reg);
    sem.analyse(mod);
    res.semMs = T(t1);

    for (auto& d : sem.diagnostics()) {
        std::string loc = "[" + std::to_string(d.loc.line) + ":" + std::to_string(d.loc.col) + "]";
        if (d.level >= types::Diagnostic::Level::Error)
            res.errors.push_back("type" + loc + ": " + d.message);
        else if (d.level == types::Diagnostic::Level::Warning)
            res.warnings.push_back("warn" + loc + ": " + d.message);
    }
    if (!res.errors.empty()) return res;

    // ── Code generation ───────────────────────────────────────────────────────
    auto t2 = std::chrono::steady_clock::now();
    codegen::CodeGenerator cg(reg, sem);
    res.irMod = cg.generate(mod);
    res.codegenMs = T(t2);

    // ── Optimisation ──────────────────────────────────────────────────────────
    auto t3 = std::chrono::steady_clock::now();
    ir::OptPipeline pipeline;
    switch (opts.optLevel) {
    case 0: pipeline = ir::OptPipeline::makeO0(); break;
    case 1: pipeline = ir::OptPipeline::makeO1(); break;
    case 3: pipeline = ir::OptPipeline::makeO3(); break;
    default: pipeline = ir::OptPipeline::makeO2(); break;
    }
    pipeline.run(res.irMod);
    res.optMs = T(t3);

    res.ok = true;
    return res;
}

static void printDiagnostics(const CompileResult& res, const Options& opts) {
    for (auto& w : res.warnings)
        std::cerr << yellow("warning: ") << w << "\n";
    for (auto& e : res.errors)
        std::cerr << red("error: ") << e << "\n";
    if (opts.verbose) {
        std::cerr << dim("  parse:   ") << std::fixed << std::setprecision(2) << res.parseMs   << "ms\n"
                  << dim("  sema:    ")                                         << res.semMs     << "ms\n"
                  << dim("  codegen: ")                                         << res.codegenMs << "ms\n"
                  << dim("  opt:     ")                                         << res.optMs     << "ms\n";
    }
}

// ─── Token dump ───────────────────────────────────────────────────────────────
static void dumpTokens(const std::string& source, const std::string& filename) {
    Lexer lx(source, filename);
    try {
        auto tokens = lx.tokenize();
        std::cout << cyan("Token dump: ") << tokens.size() << " tokens\n";
        std::cout << std::left
                  << std::setw(6)  << "Line"
                  << std::setw(6)  << "Col"
                  << std::setw(18) << "Kind"
                  << "Lexeme\n";
        std::cout << std::string(60, '-') << "\n";
        for (auto& t : tokens) {
            if (t.kind == TokenKind::Eof) break;
            std::cout << std::setw(6)  << t.loc.line
                      << std::setw(6)  << t.loc.col
                      << std::setw(18) << std::to_string((int)t.kind)
                      << dim(t.lexeme) << "\n";
        }
    } catch (LexerError& e) {
        std::cerr << red("Lex error: ") << e.what() << "\n";
    }
}

// ─── REPL ─────────────────────────────────────────────────────────────────────
static const char* REPL_BANNER = R"(
  ╔═══════════════════════════════════════════╗
  ║    F++ Interactive REPL  v1.0.0           ║
  ║    Type :help for commands, :q to exit    ║
  ╚═══════════════════════════════════════════╝
)";

static void printReplHelp() {
    std::cout << "\nF++ REPL Commands:\n"
              << "  :help          Show this help\n"
              << "  :q / :quit     Exit\n"
              << "  :reset         Reset VM\n"
              << "  :opt <0-3>     Set optimisation level\n"
              << "  :ir            Show IR of last input\n"
              << "  :cpp           Transpile last input to C++\n"
              << "  :tc            Type-check last input\n"
              << "  :load <file>   Load and run file\n"
              << "  :clear         Clear accumulated definitions\n"
              << "  :gc            Trigger garbage collection\n"
              << "  :bench         Toggle benchmark mode\n"
              << "  :defs          Show accumulated definitions\n"
              << "\nMulti-line: end with \\ to continue.\n\n";
}

static std::string readLine(const std::string& prompt) {
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return ":quit";
    return line;
}

static void runREPL(const Options& opts) {
    std::cout << cyan(REPL_BANNER) << "\n";
    std::cout << dim("Workers: ") << opts.workers
              << dim("  Opt: O") << opts.optLevel << "\n\n";

    auto fpp_vm = std::make_unique<runtime::VM>(8*1024*1024, opts.workers);
    runtime::VM& vm = *fpp_vm;
    if (!opts.noStdlib) stdlib::registerAll(vm);

    int    optLevel = opts.optLevel;
    bool   bench    = opts.benchmark;
    std::vector<std::string> sessionDefs;
    std::string lastInput;
    std::vector<std::string> buf;

    auto compile_and_run = [&](const std::string& input) {
        std::string defs;
        for (auto& d : sessionDefs) defs += d + "\n";

        // Try wrapping as main expression
        std::string src = defs + "\nfn main() {\n" + input + "\n}";

        auto t0 = std::chrono::steady_clock::now();

        Lexer lx(src, "<repl>");
        std::vector<Token> toks;
        try { toks = lx.tokenize(); }
        catch (LexerError& e) { std::cerr << red("lex: ") << e.what() << "\n"; return; }

        Parser parser(toks, "<repl>");
        auto mod = parser.parse();
        if (!parser.errors().empty()) {
            // Try as top-level definition
            std::string src2 = defs + "\n" + input;
            Lexer lx2(src2, "<repl>");
            try {
                auto toks2 = lx2.tokenize();
                Parser p2(toks2, "<repl>");
                auto mod2 = p2.parse();
                if (p2.errors().empty()) {
                    sessionDefs.push_back(input);
                    std::cout << green("✓ ") << dim("(definition registered)") << "\n";
                    return;
                }
            } catch (...) {}
            for (auto& e : parser.errors())
                std::cerr << red("parse: ") << e.what() << "\n";
            return;
        }

        types::TypeRegistry reg;
        types::SemanticAnalyser sem(reg);
        sem.analyse(mod);

        bool hasErrors = false;
        for (auto& d : sem.diagnostics()) {
            if (d.level >= types::Diagnostic::Level::Error) {
                std::cerr << red("type: ") << d.message << "\n";
                hasErrors = true;
            } else if (d.level == types::Diagnostic::Level::Warning) {
                std::cerr << yellow("warn: ") << d.message << "\n";
            }
        }
        if (hasErrors) return;

        codegen::CodeGenerator cg(reg, sem);
        auto irMod = cg.generate(mod);

        ir::OptPipeline pipeline;
        switch (optLevel) {
        case 0: pipeline = ir::OptPipeline::makeO0(); break;
        case 1: pipeline = ir::OptPipeline::makeO1(); break;
        case 3: pipeline = ir::OptPipeline::makeO3(); break;
        default: pipeline = ir::OptPipeline::makeO2(); break;
        }
        pipeline.run(irMod);

        try {
            auto result = vm.execute(irMod);
            double ms = std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now() - t0).count();

            if (result && !result->isNil()) {
                std::cout << green("=> ") << bold(result->toString()) << "\n";
            }
            if (bench)
                std::cout << dim("   (" + std::to_string(ms).substr(0,6) + " ms)") << "\n";
        } catch (runtime::FppException& e) {
            std::cerr << red("exception: ") << e.message << "\n";
            for (auto& [fn, loc] : e.stackTrace)
                std::cerr << dim("  at " + fn + " (" + loc.file + ":" + std::to_string(loc.line) + ")\n");
        } catch (std::exception& e) {
            std::cerr << red("error: ") << e.what() << "\n";
        }
    };

    for (;;) {
        bool cont = !buf.empty();
        std::string prompt = cont
            ? dim("... ")
            : cyan("f++ ") + bold("> ");
        std::string line = readLine(prompt);

        // EOF / quit
        if (line == ":quit" || line == ":q" || line == ":exit")
            break;
        if (std::cin.eof()) break;

        // Multiline
        if (!line.empty() && line.back() == '\\') {
            buf.push_back(line.substr(0, line.size()-1));
            continue;
        }
        buf.push_back(line);
        std::string input;
        for (size_t i = 0; i < buf.size(); ++i) {
            input += buf[i];
            if (i+1 < buf.size()) input += "\n";
        }
        buf.clear();

        if (input.empty()) continue;

        // REPL Commands
        std::string stripped = input;
        while (!stripped.empty() && (stripped[0]==' '||stripped[0]=='\t')) stripped.erase(0,1);

        if (stripped == ":help") { printReplHelp(); continue; }
        if (stripped == ":reset") {
            fpp_vm = std::make_unique<runtime::VM>(8*1024*1024, opts.workers);
            runtime::VM& vm = *fpp_vm;
            if (!opts.noStdlib) stdlib::registerAll(vm);
            sessionDefs.clear();
            std::cout << green("✓ VM reset\n");
            continue;
        }
        if (stripped.substr(0, 4) == ":opt") {
            try {
                optLevel = std::stoi(stripped.substr(4));
                std::cout << green("✓ ") << "Opt level: O" << optLevel << "\n";
            } catch (...) { std::cerr << red("Usage: :opt <0-3>\n"); }
            continue;
        }
        // FIX: Removed invalid Python-style string.join() syntax.
        // :ir command — build source from session defs + lastInput
        if (stripped == ":ir") {
            if (!lastInput.empty()) {
                std::string dsrc;
                for (auto& d : sessionDefs) dsrc += d + "\n";
                dsrc += "\nfn main() {\n" + lastInput + "\n}";
                try {
                    Lexer lx(dsrc, "<ir>"); auto toks = lx.tokenize();
                    Parser p(toks, "<ir>"); auto mod = p.parse();
                    types::TypeRegistry r; types::SemanticAnalyser s(r); s.analyse(mod);
                    codegen::CodeGenerator cg(r, s); auto irm = cg.generate(mod);
                    std::cout << dim(ir::dumpModule(irm));
                } catch (std::exception& e) {
                    std::cerr << red("error: ") << e.what() << "\n";
                }
            }
            continue;
        }
        if (stripped == ":cpp") {
            std::string dsrc;
            for (auto& d : sessionDefs) dsrc += d + "\n";
            dsrc += "\nfn main() {\n" + lastInput + "\n}";
            try {
                Lexer lx(dsrc,"<cpp>"); auto toks=lx.tokenize();
                Parser p(toks,"<cpp>"); auto mod=p.parse();
                types::TypeRegistry r; types::SemanticAnalyser s(r); s.analyse(mod);
                codegen::CodeGenerator cg(r, s); auto irm=cg.generate(mod);
                codegen::CppTranspileBackend back;
                std::cout << back.transpile(irm) << "\n";
            } catch (std::exception& e) {
                std::cerr << red("error: ") << e.what() << "\n";
            }
            continue;
        }
        if (stripped == ":clear") { sessionDefs.clear(); std::cout << green("✓ Defs cleared\n"); continue; }
        if (stripped == ":bench") { bench = !bench; std::cout << (bench?"✓ Benchmark ON":"✓ Benchmark OFF") << "\n"; continue; }
        if (stripped == ":gc") {
            vm.heap().gc();
            std::cout << green("✓ GC done  ")
                      << dim("heap: ") << vm.heap().heapUsed()/1024 << "KB\n";
            continue;
        }
        if (stripped == ":defs") {
            if (sessionDefs.empty()) std::cout << dim("(no definitions)\n");
            for (auto& d : sessionDefs) std::cout << dim(d) << "\n";
            continue;
        }
        if (stripped.substr(0,5) == ":load") {
            std::string path = stripped.substr(5);
            while (!path.empty() && path[0]==' ') path.erase(0,1);
            std::ifstream f(path);
            if (!f) { std::cerr << red("Cannot open: ") << path << "\n"; continue; }
            std::ostringstream ss; ss << f.rdbuf();
            input = ss.str();
        }

        lastInput = input;
        compile_and_run(input);
    }

    std::cout << cyan("\nGoodbye! 👋\n");
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    g_color = termSupportsColor();

    Options opts = parseArgs(argc, argv);
    if (opts.noColor) g_color = false;

    // ── REPL mode ─────────────────────────────────────────────────────────────
    if (opts.file.empty() && opts.evalExpr.empty() && !opts.interactive) {
        runREPL(opts);
        return 0;
    }
    if (opts.interactive) {
        runREPL(opts);
        return 0;
    }

    // ── Get source ────────────────────────────────────────────────────────────
    std::string source;
    std::string filename = "<eval>";

    if (!opts.evalExpr.empty()) {
        source = "fn main() {\n" + opts.evalExpr + "\n}";
    } else {
        filename = opts.file;
        if (!fs::exists(filename)) {
            // Try adding .fpp extension
            if (fs::exists(filename + ".fpp")) filename += ".fpp";
            else {
                std::cerr << red("error: ") << "File not found: " << filename << "\n";
                return 1;
            }
        }
        std::ifstream f(filename);
        if (!f) {
            std::cerr << red("error: ") << "Cannot open: " << filename << "\n";
            return 1;
        }
        std::ostringstream ss; ss << f.rdbuf();
        source = ss.str();
    }

    // ── Token dump ────────────────────────────────────────────────────────────
    if (opts.dumpTokens) {
        dumpTokens(source, filename);
        return 0;
    }

    // ── Compile ───────────────────────────────────────────────────────────────
    auto t_total = std::chrono::steady_clock::now();
    CompileResult res = compile(source, filename, opts);
    printDiagnostics(res, opts);

    if (!res.ok) return 1;

    // ── Type check only ───────────────────────────────────────────────────────
    if (opts.typeCheckOnly) {
        std::cout << green("✓ ") << "Type check passed: " << filename << "\n";
        return 0;
    }

    // ── IR dump ───────────────────────────────────────────────────────────────
    if (opts.dumpIR) {
        std::cout << ir::dumpModule(res.irMod);
        return 0;
    }

    // ── C++ transpilation ─────────────────────────────────────────────────────
    if (opts.transpileCpp) {
        codegen::CppTranspileBackend backend;
        std::cout << backend.transpile(res.irMod);
        return 0;
    }

    // ── Execute ───────────────────────────────────────────────────────────────
    runtime::VM vm(8*1024*1024, opts.workers);
    if (!opts.noStdlib) stdlib::registerAll(vm);

    // Bind script args
    {
        std::vector<runtime::ValueRef> argVals;
        for (auto& a : opts.args) argVals.push_back(runtime::Value::makeString(a));
        vm.setGlobal("__args__", runtime::Value::makeArray(argVals));
    }

    int exitCode = 0;
    try {
        auto t0 = std::chrono::steady_clock::now();
        auto result = vm.execute(res.irMod);
        double totalMs = std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now() - t_total).count();

        // If main returns int, use as exit code
        if (result && (result->kind == runtime::ValueKind::I32 ||
                       result->kind == runtime::ValueKind::I64)) {
            exitCode = (int)result->prim.i64;
        }

        if (opts.benchmark) {
            double runMs = std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            std::cerr << dim("\n──────── Timing ────────\n")
                      << dim("  Parse:   ") << std::fixed << std::setprecision(3) << res.parseMs   << " ms\n"
                      << dim("  Sema:    ")                                        << res.semMs     << " ms\n"
                      << dim("  Codegen: ")                                        << res.codegenMs << " ms\n"
                      << dim("  Opt O")   << opts.optLevel << ":  "               << res.optMs     << " ms\n"
                      << dim("  Execute: ")                                        << runMs         << " ms\n"
                      << dim("  Total:   ")                                        << totalMs       << " ms\n"
                      << dim("────────────────────────\n");
        }
        if (opts.verbose && result) {
            std::cerr << dim("result: ") << result->toString() << "\n";
        }
    } catch (runtime::FppException& e) {
        std::cerr << red("exception: ") << bold(e.message) << "\n";
        for (auto& [fn, loc] : e.stackTrace)
            std::cerr << dim("  at " + fn + " (") << loc.file << ":"
                      << loc.line << dim(")") << "\n";
        exitCode = 1;
    } catch (std::exception& e) {
        std::cerr << red("error: ") << e.what() << "\n";
        exitCode = 1;
    }

    return exitCode;
}
