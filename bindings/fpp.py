"""
F++ Language — Python interface layer.
Wraps the native C++ engine (fpp_native) with a clean, Pythonic API.
"""

from __future__ import annotations
import sys
import os
import io
import textwrap
import readline
import atexit
import traceback
from pathlib import Path
from typing import Any, Callable, Optional, Union

try:
    import fpp_native as _fpp
except ImportError:
    raise ImportError(
        "F++ native engine not found. Build it with:\n"
        "  cd build && cmake .. && make\n"
        "or install via pip if available."
    )

__version__  = "1.0.0"
__all__ = ["VM", "Lexer", "Parser", "run", "typecheck", "tokenize",
           "transpile_cpp", "dump_ir", "FppError", "REPL"]

# ─── Exceptions ───────────────────────────────────────────────────────────────
class FppError(Exception):
    """Base exception for F++ errors."""

class FppParseError(FppError):
    def __init__(self, errors: list[str]):
        self.errors = errors
        super().__init__("Parse errors:\n" + "\n".join(f"  • {e}" for e in errors))

class FppTypeError(FppError):
    def __init__(self, errors: list[str]):
        self.errors = errors
        super().__init__("Type errors:\n" + "\n".join(f"  • {e}" for e in errors))

class FppRuntimeError(FppError):
    pass

# ─── High-level VM wrapper ────────────────────────────────────────────────────
class VM:
    """
    High-level F++ virtual machine.

    Example:
        vm = VM()
        result = vm.run('''
            fn main() -> i64 {
                let x = 42
                println("The answer is ${x}")
                x
            }
        ''')
        print(result)  # 42
    """

    def __init__(
        self,
        workers: int = 0,
        capture_output: bool = False,
        opt_level: int = 2,
    ):
        import os
        self._workers = workers or os.cpu_count() or 4
        self._opt = opt_level
        self._vm = _fpp.VM(self._workers)
        self._stdout_buf: list[str] = []
        self._stderr_buf: list[str] = []
        self._capture = capture_output

        if capture_output:
            self._vm.set_stdout(self._stdout_buf.append)
            self._vm.set_stderr(self._stderr_buf.append)

    @property
    def stdout(self) -> str:
        return "".join(self._stdout_buf)

    @property
    def stderr(self) -> str:
        return "".join(self._stderr_buf)

    def clear_buffers(self):
        self._stdout_buf.clear()
        self._stderr_buf.clear()

    def run(self, source: str, *, opt_level: Optional[int] = None) -> Any:
        """Compile and execute F++ source code."""
        source = textwrap.dedent(source).strip()
        opt = opt_level if opt_level is not None else self._opt
        errors = _fpp.VM.get_type_errors(source)
        if errors:
            raise FppTypeError(errors)
        try:
            return self._vm.run_opt(source, opt)
        except (ValueError, RuntimeError) as e:
            raise FppRuntimeError(str(e)) from e

    def run_file(self, path: Union[str, Path], *, opt_level: Optional[int] = None) -> Any:
        """Load and execute a .fpp file."""
        source = Path(path).read_text(encoding="utf-8")
        return self.run(source, opt_level=opt_level)

    def bind(self, name: str, fn: Callable) -> "VM":
        """Bind a Python callable as a native F++ function."""
        self._vm.bind_native(name, fn)
        return self

    def set_stdout(self, fn: Callable[[str], None]) -> "VM":
        self._vm.set_stdout(fn)
        return self

    def set_stderr(self, fn: Callable[[str], None]) -> "VM":
        self._vm.set_stderr(fn)
        return self

    def transpile_cpp(self, source: str) -> str:
        """Transpile F++ source to C++."""
        return self._vm.transpile_cpp(textwrap.dedent(source).strip())

    def dump_ir(self, source: str, opt_level: int = 0) -> str:
        """Dump the IR for given F++ source."""
        return self._vm.dump_ir(textwrap.dedent(source).strip(), opt_level)

    def gc(self) -> dict:
        """Trigger garbage collection and return stats."""
        stats = self._vm.gc_stats()
        self._vm.gc()
        return stats

    def gc_stats(self) -> dict:
        return self._vm.gc_stats()

    def __repr__(self) -> str:
        s = self.gc_stats()
        return f"<fpp.VM workers={self._workers} opt=O{self._opt} heap={s['heap_used']//1024}KB>"

# ─── Module-level conveniences ────────────────────────────────────────────────
def run(source: str, opt_level: int = 2) -> Any:
    """Create a VM and run F++ source. Returns the result."""
    vm = VM(opt_level=opt_level)
    return vm.run(source)

def typecheck(source: str) -> bool:
    """Return True if the source passes type checking."""
    return _fpp.VM.typecheck(textwrap.dedent(source).strip())

def get_type_errors(source: str) -> list[str]:
    """Return a list of type error messages."""
    return list(_fpp.VM.get_type_errors(textwrap.dedent(source).strip()))

def tokenize(source: str) -> list[dict]:
    """Return a list of token dicts with keys: kind, lexeme, line, col."""
    return list(_fpp.VM.tokenize(textwrap.dedent(source).strip()))

def transpile_cpp(source: str) -> str:
    """Transpile F++ source to C++ code."""
    return _fpp.transpile_cpp(textwrap.dedent(source).strip())

def dump_ir(source: str, opt_level: int = 0) -> str:
    """Dump the IR for F++ source."""
    return _fpp.dump_ir(textwrap.dedent(source).strip(), opt_level)

# ─── Decorator: @fpp_fn ──────────────────────────────────────────────────────
def fpp_fn(source: str, opt_level: int = 2):
    """
    Decorator that compiles F++ source and returns a callable.

    Usage:
        @fpp_fn('''
            fn add(a: i64, b: i64) -> i64 { a + b }
        ''')
        def add_fpp(): ...

        result = add_fpp(3, 4)  # calls F++ add
    """
    def decorator(py_fn: Callable):
        vm = VM(opt_level=opt_level)
        fn_name = py_fn.__name__
        try:
            vm.run(source)  # register all defs
        except FppError:
            pass  # definitions may not need main

        import functools
        @functools.wraps(py_fn)
        def wrapper(*args):
            arg_src = ", ".join(repr(a) for a in args)
            call_src = source + f"\nfn main() {{ {fn_name}({arg_src}) }}"
            return vm.run(call_src)
        return wrapper
    return decorator

# ─── REPL ────────────────────────────────────────────────────────────────────
BANNER = """\
╔══════════════════════════════════════════╗
║   F++ Interactive REPL  v{version:<14} ║
║   Type :help for commands, :quit to exit ║
╚══════════════════════════════════════════╝
""".format(version=__version__)

HELP = """\
F++ REPL Commands:
  :help            Show this help
  :quit / :exit    Exit the REPL
  :reset           Reset the VM and clear history
  :opt <0-3>       Set optimisation level
  :ir [code]       Show IR for expression/statement
  :cpp [code]      Transpile to C++
  :tc [code]       Type-check code
  :load <file>     Load and execute a .fpp file
  :gc              Run garbage collection
  :stats           Show VM stats
  :history         Show command history

Multi-line mode: end a line with \\ to continue on next line.
"""

class REPL:
    """
    Interactive Read-Eval-Print Loop for F++.

    Usage:
        from fpp import REPL
        REPL().run()
    """

    HISTORY_FILE = Path.home() / ".fpp_history"

    def __init__(self, opt_level: int = 2, use_colors: bool = True):
        self.vm = VM(capture_output=False, opt_level=opt_level)
        self.opt = opt_level
        self.history: list[str] = []
        self.session_defs: list[str] = []  # accumulated top-level defs
        self.colors = use_colors and sys.stdout.isatty()

        # readline setup
        try:
            readline.parse_and_bind("tab: complete")
            readline.set_completer(self._complete)
            if self.HISTORY_FILE.exists():
                readline.read_history_file(self.HISTORY_FILE)
            atexit.register(readline.write_history_file, self.HISTORY_FILE)
        except Exception:
            pass

    def _c(self, code: str, text: str) -> str:
        if not self.colors:
            return text
        codes = {"red": "31", "green": "32", "yellow": "33", "blue": "34",
                 "magenta": "35", "cyan": "36", "bold": "1", "dim": "2", "reset": "0"}
        return f"\033[{codes.get(code, '0')}m{text}\033[0m"

    def _complete(self, text: str, state: int) -> Optional[str]:
        keywords = [
            "fn", "let", "var", "const", "if", "else", "while", "for", "in",
            "return", "break", "continue", "match", "class", "struct", "enum",
            "impl", "trait", "import", "pub", "async", "await", "spawn", "yield",
            "true", "false", "nil", "type", "where", "as", "new", "del",
        ]
        options = [k for k in keywords if k.startswith(text)]
        return options[state] if state < len(options) else None

    def _prompt(self, cont: bool = False) -> str:
        if cont:
            return self._c("dim", "... ")
        return self._c("cyan", "f++ ") + self._c("bold", "> ")

    def _eval(self, code: str) -> Any:
        """Evaluate code, accumulating top-level definitions."""
        code = code.strip()
        if not code:
            return None

        # Wrap bare expressions in main if needed
        is_def = any(code.startswith(kw) for kw in
                     ("fn ", "struct ", "class ", "enum ", "trait ", "impl ",
                      "type ", "const ", "pub ", "import ", "mod "))

        if is_def:
            self.session_defs.append(code)
            # Validate
            combined = "\n".join(self.session_defs)
            errors = get_type_errors(combined)
            if errors:
                self.session_defs.pop()
                raise FppTypeError(errors)
            return None
        else:
            # Wrap in main
            defs = "\n".join(self.session_defs)
            src = f"{defs}\nfn main() {{\n{code}\n}}"
            return self.vm.run(src, opt_level=self.opt)

    def _handle_command(self, line: str) -> bool:
        """Handle REPL commands. Returns True if handled."""
        parts = line.split(None, 1)
        cmd = parts[0].lower()
        arg = parts[1] if len(parts) > 1 else ""

        if cmd in (":quit", ":exit", ":q"):
            print(self._c("cyan", "Goodbye! 👋"))
            sys.exit(0)
        elif cmd == ":help":
            print(HELP)
        elif cmd == ":reset":
            self.vm = VM(capture_output=False, opt_level=self.opt)
            self.session_defs.clear()
            print(self._c("green", "VM reset."))
        elif cmd == ":opt":
            try:
                self.opt = int(arg.strip())
                print(self._c("green", f"Optimisation level set to O{self.opt}"))
            except ValueError:
                print(self._c("red", "Usage: :opt <0-3>"))
        elif cmd == ":ir":
            src = arg or "\n".join(self.session_defs) or "fn main() {}"
            try:
                print(self._c("dim", dump_ir(src, self.opt)))
            except Exception as e:
                print(self._c("red", str(e)))
        elif cmd == ":cpp":
            src = arg or "\n".join(self.session_defs) or "fn main() {}"
            try:
                print(transpile_cpp(src))
            except Exception as e:
                print(self._c("red", str(e)))
        elif cmd == ":tc":
            src = arg or "\n".join(self.session_defs)
            errors = get_type_errors(src)
            if errors:
                for e in errors: print(self._c("red", f"  ✗ {e}"))
            else:
                print(self._c("green", "  ✓ No type errors"))
        elif cmd == ":load":
            path = arg.strip()
            try:
                result = self.vm.run_file(path, opt_level=self.opt)
                if result is not None:
                    print(self._c("green", f"=> ") + repr(result))
            except FileNotFoundError:
                print(self._c("red", f"File not found: {path}"))
            except FppError as e:
                print(self._c("red", str(e)))
        elif cmd == ":gc":
            stats = self.vm.gc()
            print(self._c("dim", f"GC done. Heap: {stats.get('heap_used',0)//1024}KB used"))
        elif cmd == ":stats":
            s = self.vm.gc_stats()
            print(f"  Heap used:  {s.get('heap_used',0):>10,} bytes")
            print(f"  Heap total: {s.get('heap_total',0):>10,} bytes")
        elif cmd == ":history":
            for i, h in enumerate(self.history[-20:], 1):
                print(f"  {i:3}. {h}")
        else:
            return False
        return True

    def run(self):
        """Start the interactive REPL."""
        print(BANNER)
        buf = []

        while True:
            try:
                cont = bool(buf)
                line = input(self._prompt(cont))
            except (EOFError, KeyboardInterrupt):
                if buf:
                    buf.clear()
                    print()
                    continue
                print("\n" + self._c("cyan", "Goodbye!"))
                break

            # Multi-line continuation
            if line.endswith("\\"):
                buf.append(line[:-1])
                continue
            buf.append(line)
            full = "\n".join(buf)
            buf.clear()

            if not full.strip():
                continue

            # REPL commands
            stripped = full.strip()
            if stripped.startswith(":"):
                self._handle_command(stripped)
                continue

            self.history.append(stripped)

            try:
                result = self._eval(stripped)
                if result is not None:
                    print(self._c("green", "=> ") + self._c("bold", repr(result)))
            except FppParseError as e:
                print(self._c("red", "Parse error:"))
                for err in e.errors:
                    print(f"  {self._c('red', '✗')} {err}")
            except FppTypeError as e:
                print(self._c("red", "Type error:"))
                for err in e.errors:
                    print(f"  {self._c('yellow', '✗')} {err}")
            except FppRuntimeError as e:
                print(self._c("red", f"Runtime error: {e}"))
            except KeyboardInterrupt:
                print(self._c("yellow", " [interrupted]"))
            except Exception as e:
                print(self._c("red", f"Error: {e}"))
                if os.environ.get("FPP_DEBUG"):
                    traceback.print_exc()

# ─── CLI entry point ─────────────────────────────────────────────────────────
def main():
    import argparse

    parser = argparse.ArgumentParser(
        prog="fpp",
        description="F++ Programming Language Interpreter",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
            Examples:
              fpp                         # Start interactive REPL
              fpp script.fpp              # Run a file
              fpp -e 'println("hello")'   # Run an expression
              fpp --ir script.fpp         # Dump IR
              fpp --cpp script.fpp        # Transpile to C++
              fpp --tc script.fpp         # Type check only
        """),
    )
    parser.add_argument("file",          nargs="?",          help="F++ script to run")
    parser.add_argument("-e", "--eval",                      help="Evaluate F++ expression/statement")
    parser.add_argument("-O", "--opt",   type=int, default=2,help="Optimisation level 0-3 (default: 2)")
    parser.add_argument("--ir",          action="store_true", help="Dump IR and exit")
    parser.add_argument("--cpp",         action="store_true", help="Transpile to C++ and exit")
    parser.add_argument("--tc",          action="store_true", help="Type-check only")
    parser.add_argument("--no-color",    action="store_true", help="Disable ANSI colour output")
    parser.add_argument("--workers",     type=int, default=0, help="Number of worker threads")
    parser.add_argument("--version",     action="version",    version=f"F++ {__version__}")

    args = parser.parse_args()

    def get_source() -> str:
        if args.eval:
            return args.eval
        if args.file:
            return Path(args.file).read_text(encoding="utf-8")
        return None

    source = get_source()

    # IR dump
    if args.ir and source:
        print(dump_ir(source, args.opt))
        return

    # Transpile
    if args.cpp and source:
        print(transpile_cpp(source))
        return

    # Type check
    if args.tc and source:
        errors = get_type_errors(source)
        if errors:
            for e in errors:
                print(f"error: {e}", file=sys.stderr)
            sys.exit(1)
        print("OK — no type errors")
        return

    # Run file or expression
    if source:
        vm = VM(workers=args.workers or 0, opt_level=args.opt)
        try:
            result = vm.run(source)
            if result is not None and not args.eval:
                pass  # scripts print their own output
            elif result is not None:
                print(result)
        except FppError as e:
            print(f"error: {e}", file=sys.stderr)
            sys.exit(1)
        return

    # Interactive REPL
    REPL(opt_level=args.opt, use_colors=not args.no_color).run()

if __name__ == "__main__":
    main()
