# F++ Programming Language

[![Build](https://img.shields.io/badge/build-passing-brightgreen)]()
[![C++](https://img.shields.io/badge/C%2B%2B-20-blue)]()
[![Python](https://img.shields.io/badge/Python-3.9%2B-blue)]()
[![License](https://img.shields.io/badge/license-MIT-green)]()

**F++** is a statically-typed, multi-paradigm programming language with a high-performance C++ engine exposed to Python via pybind11.

---

## Architecture

```
fpp/
├── include/            # C++ headers (the language engine)
│   ├── lexer.hpp       # Tokeniser — full Unicode, string interpolation
│   ├── parser.hpp      # Pratt parser — full grammar
│   ├── ast.hpp         # AST node hierarchy (100+ node types)
│   ├── types.hpp       # Type system, inference, semantic analysis
│   ├── ir.hpp          # SSA intermediate representation + opt passes
│   ├── codegen.hpp     # AST → IR code generator + backends
│   └── runtime.hpp     # VM, GC, async scheduler, value system
├── src/                # C++ implementations
│   ├── lexer.cpp
│   ├── parser.cpp
│   ├── types.cpp
│   ├── ir.cpp
│   ├── codegen.cpp
│   └── runtime.cpp
├── stdlib/
│   └── stdlib.cpp      # Standard library (algo, math, io, time, regex, collections, env)
├── bindings/
│   ├── fpp_bindings.cpp  # pybind11 extension module
│   └── fpp.py            # Python wrapper + REPL + CLI
├── tools/
│   ├── main.cpp          # Standalone interpreter
│   └── tests.cpp         # Comprehensive test suite
├── examples/
│   └── examples.fpp      # Feature showcase
└── CMakeLists.txt
```

---

## Language Features

### Types
| Type | Description |
|------|-------------|
| `i8` `i16` `i32` `i64` `i128` `isize` | Signed integers |
| `u8` `u16` `u32` `u64` `u128` `usize` | Unsigned integers |
| `f32` `f64` | Floating point |
| `bool` `char` `str` | Primitives |
| `[T]` `[T; N]` | Slices and arrays |
| `(A, B, C)` | Tuples |
| `*T` `&T` `&mut T` | Pointers and references |
| `fn(A, B) -> C` | Function types |
| `Option<T>` `Result<T, E>` | Sum types |
| `Vec<T>` `Map<K,V>` | Collections |

### Control Flow
```fpp
// if-else as expression
let sign = if x > 0 { 1 } else if x < 0 { -1 } else { 0 }

// Pattern matching
match value {
    0        => "zero",
    1..=9    => "single digit",
    Point { x, y } if x == y => "diagonal",
    _        => "other",
}

// For / while
for item in collection { ... }
while condition { ... }

// Break with value
let result = 'label: {
    for x in 0..100 {
        if x * x > 1000 { break 'label x }
    }
    0
}
```

### Functions & Closures
```fpp
fn add<T: Numeric>(a: T, b: T) -> T { a + b }

let double = fn(x: i64) -> i64 { x * 2 }
let triple = lambda(x) => x * 3

fn make_adder(n: i64) -> fn(i64) -> i64 {
    lambda(x) => x + n
}
```

### Structs, Enums, Classes, Traits
```fpp
struct Point { pub x: f64, pub y: f64 }

impl Point {
    pub fn distance(&self, other: Point) -> f64 {
        sqrt((self.x-other.x)**2 + (self.y-other.y)**2)
    }
}

enum Result<T, E> { Ok(T), Err(E) }

trait Drawable { fn draw(&self) }

class Widget: Drawable, Serializable {
    pub fn draw(&self) { println("drawing widget") }
}
```

### Async / Concurrency
```fpp
async fn fetch(url: str) -> str { ... }

let data = await fetch("https://api.example.com")

// Channels
let ch = make_chan(32)
spawn producer(ch)
let val = chan_recv(ch)

// Parallel execution
let tasks = [spawn compute(1), spawn compute(2)]
let results = map(tasks, await)
```

### Error Handling
```fpp
fn parse(s: str) -> Result<i64, ParseError> {
    let n = str_to_int(s)?    // ? propagates Err
    if n < 0 { return Err(ParseError::Negative(n)) }
    Ok(n)
}

try {
    let val = parse("hello")?
} catch (e: ParseError) {
    println("caught: ${e}")
}
```

### String Interpolation
```fpp
let name = "world"
let greeting = "Hello, ${name}! 2 + 2 = ${2 + 2}"
```

---

## Standard Library

### `algo` module
`map`, `filter`, `reduce`, `flat_map`, `zip`, `enumerate`, `find`, `find_index`,
`any`, `all`, `none_`, `sort_by`, `group_by`, `flatten`, `unique`, `take`, `skip`,
`sum`, `product`, `min_by`, `max_by`, `chunk`, `window`, `partition`, `count_if`,
`range`, `binary_search`, `quicksort`, `merge_sort`

### `math` module
Full trigonometry (`sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, ...),
`sqrt`, `cbrt`, `pow`, `exp`, `log`, `log2`, `log10`, `abs`, `floor`, `ceil`,
`round`, `clamp`, `lerp`, `gcd`, `lcm`, `factorial`, `fibonacci`, `binomial`,
`is_prime`, `next_prime`, `primes_up_to`, random functions (`random`, `random_int`,
`random_float`, `shuffle`, `sample`, `seed_rng`), constants `PI`, `E`, `TAU`, `INF`

### `io` module
`read_file`, `write_file`, `append_file`, `read_lines`, `file_exists`, `file_size`,
`list_dir`, `mkdir`, `remove`, `rename`, `cwd`

### `time` module
`now_ms`, `now_us`, `monotonic_ns`, `sleep_ms`, `sleep_us`

### `re` module
`match`, `search`, `find_all`, `replace`, `split`

### `collections` module
Stack, Queue, `map_get`, `map_set`, `map_delete`, `map_keys`, `map_values`,
`map_entries`, `map_merge`

### `env` module
`get`, `set`, `args`

---

## Building

### Prerequisites
```bash
# Ubuntu/Debian
sudo apt install build-essential cmake python3-dev

# Install pybind11
pip install pybind11
```

### Build
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Install Python module
```bash
cd build && cmake --install .
# or for development:
cd build/lib && python3 -c "import fpp_native; print('OK')"
```

---

## Usage

### Standalone interpreter
```bash
# Interactive REPL
./build/bin/fpp

# Run file
./build/bin/fpp examples/examples.fpp

# Evaluate expression
./build/bin/fpp -e 'println("Hello, F++!")'

# With optimisation
./build/bin/fpp -O3 script.fpp

# Dump IR
./build/bin/fpp --ir script.fpp

# Transpile to C++
./build/bin/fpp --cpp script.fpp

# Type check only
./build/bin/fpp --tc script.fpp

# Benchmark
./build/bin/fpp --bench script.fpp
```

### Python API
```python
import sys; sys.path.insert(0, 'build/lib')
from fpp import VM, run, typecheck, tokenize, dump_ir, transpile_cpp

# Quick run
result = run('''
    fn main() -> i64 {
        let nums = [1, 2, 3, 4, 5]
        reduce(nums, fn(a: i64, b: i64) -> i64 { a + b }, 0)
    }
''')
print(result)  # 15

# Full VM with native bindings
vm = VM(workers=4, opt_level=3)

# Bind Python function as native F++ function
vm.bind("py_sqrt", lambda args: args[0] ** 0.5)

# Capture output
vm2 = VM(capture_output=True)
vm2.run('println("hello from F++")')
print(vm2.stdout)  # "hello from F++\n"

# Type checking
print(typecheck('fn f(x: i64) -> i64 { x + 1 }'))  # True

# Token inspection
tokens = tokenize('let x = 42')
for tok in tokens:
    print(tok)

# IR dump
print(dump_ir('fn main() -> i64 { 2 + 3 }', opt_level=2))

# Transpile to C++
print(transpile_cpp('fn add(a: i64, b: i64) -> i64 { a + b }'))
```

### Python REPL
```python
from fpp import REPL
REPL(opt_level=2).run()
```

---

## IR Optimisation Passes

| Pass | O0 | O1 | O2 | O3 | Os |
|------|----|----|----|----|-----|
| Constant Folding | | ✓ | ✓ | ✓ | ✓ |
| Dead Code Elimination | | ✓ | ✓ | ✓ | ✓ |
| Block Merging | | ✓ | ✓ | ✓ | ✓ |
| Constant Propagation | | | ✓ | ✓ | |
| CSE | | | ✓ | ✓ | |
| Copy Propagation | | | ✓ | ✓ | |
| Tail Call Optimisation | | | ✓ | ✓ | |
| Jump Threading | | | ✓ | ✓ | |
| Inlining | | | | ✓ | |
| Loop Invariant Motion | | | | ✓ | |
| Strength Reduction | | | | ✓ | |
| GVN | | | | ✓ | |
| Loop Unrolling | | | | ✓ | |
| Vectorisation | | | | ✓ | |
| Escape Analysis | | | | ✓ | |

---

## Running Tests
```bash
./build/bin/fpp_tests
```

---

## License
MIT © NgoHuuLoc0612
