# Strata — Pythonic C/C++ Bidirectional Transpiler

**Strata** is a human-readable, zero-overhead **1-to-1 syntactic representation of C and C++** inspired by Python's clean naming, type annotations, and block indentation.

Just as **LMON is JSON and JSON is LMON**, **Strata is C/C++ and C/C++ is Strata**. Every Strata construct maps directly and deterministically to an exact C/C++ equivalent—with **no curly braces (`{` `}`) for blocks**, **no trailing semicolons (`;`)**, **Pythonic function and variable type annotations (`def`, `name: type -> ret`)**, **concise fixed-width type aliases (`i8`..`i64`, `u8`..`u64`, `f32`, `f64`, `usize`, `cstr`, `str`)**, and **zero runtime or ABI overhead**.

- **Extension:** `.st` (or `.strata`)
- **Target Languages:** ISO C11 / C17 / C23 and ISO C++17 / C++20
- **Encoding:** UTF-8 (without BOM)
- **Version:** 1.0

---

## Design Goals

1. **Bijective C/C++ Structural Equivalence:** Every Strata file transpiles 1-to-1 into clean, human-readable C/C++ and converts back via `--from-c` with zero hidden runtime, garbage collector, or non-standard ABI wrapper.
2. **Pythonic Readability:**
   - Indentation-delimited blocks (`:`) instead of `{` and `}` (including automatic `};` termination on `struct`, `class`, `union`, and `enum` definitions).
   - No statement-ending semicolons (`;`).
   - Python-style `def name(arg: type) -> ret_type:` functions and `name: type = value` declarations.
   - Pythonic keywords and logical operators (`elif`, `and`, `or`, `not`, `nil` / `None`, `self`, `pass`, `in`).
   - `#` single-line comments and `import` / `from ... import` / `define` preprocessor directives.
3. **Auto-Detected Indentation & Universal Newlines:** Supports any consistent indentation unit per file (`W` spaces or `W` tabs, locked on the first indented line) and universal line endings (`\n`, `\r\n`, `\r`).
4. **Blazing-Fast Single-File C Transpiler:** [`strata.c`](strata.c) is a self-contained, zero-dependency C11 implementation that transpiles at **3,100+ files/s (`225+ MiB/s`)**.

---

## Side-by-Side Comparison

### Strata (`.st`)

```python
# Service node evaluator in Strata
import stdio.h
import stdint.h
import stdbool.h

define MAX_GREETINGS 3

enum Status:
    OK = 0
    ERR = 1

struct Point:
    x: f64
    y: f64

def clamp(val: i32, lo: i32, hi: i32) -> i32:
    if val < lo:
        return lo
    elif val > hi:
        return hi
    else:
        return val

def greet(name: cstr, count: i32) -> i32:
    if name == nil or count <= 0:
        return 0
    for i in 0..count:
        printf("[%d] Hello from Strata, %s!\n", i + 1, name)
    return count

def main() -> i32:
    sent: i32 = greet("World", MAX_GREETINGS)
    if sent > 0 and not (sent > 10):
        printf("Clamped: %d\n", clamp(sent, 0, 2))
    return 0
```

### Equivalent C (`.c`)

```c
// Service node evaluator in Strata
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_GREETINGS 3

enum Status {
    OK = 0,
    ERR = 1,
};

struct Point {
    double x;
    double y;
};

int32_t clamp(int32_t val, int32_t lo, int32_t hi) {
    if (val < lo) {
        return lo;
    } else if (val > hi) {
        return hi;
    } else {
        return val;
    }
}

int32_t greet(const char* name, int32_t count) {
    if (name == NULL || count <= 0) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        printf("[%d] Hello from Strata, %s!\n", i + 1, name);
    }
    return count;
}

int32_t main(void) {
    int32_t sent = greet("World", MAX_GREETINGS);
    if (sent > 0 && !(sent > 10)) {
        printf("Clamped: %d\n", clamp(sent, 0, 2));
    }
    return 0;
}
```

---

## Strata 1.0 Specification

### 1. Encoding, Universal Newlines, and Auto-Detected Indentation

1. **UTF-8:** Source files MUST be valid UTF-8 without a byte-order mark (`U+FEFF` at byte 0 is a syntax error).
2. **Universal Newlines (`LF`, `CRLF`, `CR`):** Strata supports `\n`, `\r\n`, and `\r` line endings interchangeably outside string literals.
3. **Blank Lines:** Lines containing zero characters or only ASCII spaces (`0x20`) and/or tabs (`0x09`) before a line ending are preserved as blank lines without affecting indentation tracking.
4. **Auto-Detected Indentation (Spaces or Tabs):**
   - Root declarations sit at indentation level `0`.
   - The **first indented non-blank line** locks the file's `indent_unit` to either `W` ASCII spaces (`0x20`) or `W` ASCII tabs (`0x09`).
   - Every subsequent non-blank line MUST indent by an exact multiple `N * indent_unit` (`level = count / W`).
   - Mixing spaces and tabs in indentation, jumping more than one indentation level after a `:` block header, or leaving a dangling `:` block header without an indented body is a **syntax error**.
5. **No Trailing Whitespace:** Non-blank lines MUST NOT contain trailing spaces or tabs before the line ending.

---

### 2. Comments & Preprocessor Directives

Because Strata uses `#` for Python-style comments, C preprocessor directives are expressed as clean, bracket-free keywords at the start of a line.

In particular, `import` distinguishes between **Strata modules (`.st`)** and **untranspiled C/C++ files (`.h` / `.c` / `.hpp`)** by whether a file extension is present:

- **Without an extension (`import math_utils`):** Refers to a Strata `.st` module (`math_utils.st`), transpiling to `#include "math_utils.h"`.
- **With an explicit `.h` or `.c` extension (`import stdio.h`):** Imports an untranspiled C/C++ header or source file directly (`#include <stdio.h>`).

| Strata Syntax | C / C++ Equivalent | Notes |
| :--- | :--- | :--- |
| `# comment text` | `// comment text` | Full-line or inline comment (preceded by space outside quotes). |
| `import math_utils` | `#include "math_utils.h"` | Extensionless `import` is exclusively for Strata `.st` modules. |
| `from math_utils import *` | `#include "math_utils.h"` | Python-style `from` import of a Strata `.st` module. |
| `import stdio.h` | `#include <stdio.h>` | Explicit `.h` / `.c` extension imports an untranspiled C/C++ file directly. |
| `import sys/stat.h` | `#include <sys/stat.h>` | Explicit path with `.h` extension imports system header directly. |
| `import "vendor.c"` | `#include "vendor.c"` | Quoted `.h` or `.c` path imports a local untranspiled C file directly. |
| `define MAX_LEN 1024` | `#define MAX_LEN 1024` | Object-like or function-like macro definition. |
| `ifdef DEBUG` / `ifndef NDEBUG` | `#ifdef DEBUG` / `#ifndef NDEBUG` | Conditional compilation guards. |
| `endif` | `#endif` | End conditional compilation guard. |
| `pragma once` | `#pragma once` | Compiler/header pragma directive. |

---

### 3. Concise Fixed-Width & Primitive Type Aliases

Strata provides concise, unambiguous primitive type names alongside all standard C/C++ types:

| Strata Type | C / C++ Type | Description |
| :--- | :--- | :--- |
| `i8`, `i16`, `i32`, `i64` | `int8_t`, `int16_t`, `int32_t`, `int64_t` | Signed fixed-width integers (`<stdint.h>`). |
| `u8`, `u16`, `u32`, `u64` | `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t` | Unsigned fixed-width integers (`<stdint.h>`). |
| `usize`, `isize` | `size_t`, `ptrdiff_t` | Pointer-sized unsigned and signed integers (`<stddef.h>`). |
| `f32`, `f64` | `float`, `double` | 32-bit and 64-bit IEEE-754 floating-point numbers. |
| `bool` | `bool` | Boolean (`true` / `false`). |
| `byte` | `uint8_t` | Alias for unsigned 8-bit byte. |
| `cstr` | `const char*` | Immutable NUL-terminated C string pointer. |
| `str` | `char*` | Mutable NUL-terminated C string pointer. |
| `any` | `void*` | Generic untyped pointer. |

---

### 4. Keywords, Logical Operators, and Literals

| Strata Token | C / C++ Equivalent | Description |
| :--- | :--- | :--- |
| `and` | `&&` | Logical AND operator. |
| `or` | `\|\|` | Logical OR operator. |
| `not x` | `!x` | Logical NOT operator. |
| `elif cond:` | `} else if (cond) {` | Else-if branch header. |
| `nil` / `None` | `NULL` | Null pointer constant. |
| `True` / `False` | `true` / `false` | Boolean literals (`true` and `false` are canonical). |
| `self` | `this` | C++ instance pointer. |
| `pass` | _(empty block)_ | Explicit no-op placeholder inside an empty block (`{}`). |
| `type Alias = Target` | `typedef Target Alias;` | Clean type alias declaration. |

---

### 5. Variable & Field Declarations (`name: type = value`)

Variables, struct/class fields, and constants use Pythonic postfix type annotations:

| Strata Syntax | C / C++ Equivalent |
| :--- | :--- |
| `count: i32 = 42` | `int32_t count = 42;` |
| `const pi: f64 = 3.14159` | `const double pi = 3.14159;` |
| `static retries: u8 = 3` | `static uint8_t retries = 3;` |
| `name: cstr = "Strata"` | `const char* name = "Strata";` |
| `buf: char[64]` | `char buf[64];` |
| `matrix: f32[4][4]` | `float matrix[4][4];` |
| `node_ptr: Node* = nil` | `Node* node_ptr = NULL;` |

Notice how array dimensions attached to the type (`buf: char[64]`) automatically transpose to valid C declarator syntax (`char buf[64];`).

---

### 6. Functions & Methods (`def`)

Functions and methods are declared with `def` (or `fn`), typed parameters `(name: type, ...)`, and an optional return type `-> ret_type`:

- **Function with return type:**
  ```python
  def add(a: i32, b: i32) -> i32:
      return a + b
  ```
  Transpiles to:
  ```c
  int32_t add(int32_t a, int32_t b) {
      return a + b;
  }
  ```
- **Void function (omitted `->`):**
  If `-> ret_type` is omitted (except on `main`, which defaults to `int`), the return type is `void`, and an empty parameter list `()` emits `(void)` in C:
  ```python
  def reset_state():
      pass
  ```
  Transpiles to:
  ```c
  void reset_state(void) {
  }
  ```
- **Qualifiers & Forward Declarations:**
  - `static def helper(x: i32) -> i32:` → `static int32_t helper(int32_t x) {`
  - `inline def fast_sq(x: f64) -> f64:` → `inline double fast_sq(double x) {`
  - Omitting the trailing `:` emits a forward prototype:
    `def compute(x: i32) -> i32` → `int32_t compute(int32_t x);`

---

### 7. Control Flow & Range Loops

Parentheses around `if`, `elif`, `while`, `for`, and `switch` conditions are omitted in Strata and automatically synthesized in C/C++:

1. **Conditionals (`if` / `elif` / `else`):**
   ```python
   if x > 10 and not done:
       step_a()
   elif x == 0 or retry:
       step_b()
   else:
       step_c()
   ```
2. **Range-Based & C-Style `for` Loops:**
   - **Exclusive integer range (`a..b`):**
     `for i in 0..n:` → `for (int i = 0; i < n; i++) {`
   - **Inclusive integer range (`a..=b`):**
     `for i in 1..=10:` → `for (int i = 1; i <= 10; i++) {`
   - **Typed or stepped range (`for i: usize in 0..len step 2:`):**
     → `for (size_t i = 0; i < len; i += 2) {`
   - **C++ iterable range (`for item in vec:`):**
     → `for (auto item : vec) {`
   - **Classic 3-clause C loop (`for i: i32 = 0; i < n; i++:`):**
     → `for (int32_t i = 0; i < n; i++) {`
3. **`while` and `do` / `while` Loops:**
   ```python
   while count > 0:
       count -= 1

   do:
       attempt += 1
   while attempt < 3
   ```
4. **`switch` / `match` and `case` / `default`:**
   ```python
   switch kind:
       case 0:
           return "circle"
       case 1:
           return "rect"
       default:
           return "unknown"
   ```

---

### 8. Structs, Classes, Enums, and Unions

Block types that require a trailing semicolon in C/C++ (`struct`, `class`, `union`, `enum`) automatically close with `};` when their indentation block ends, and `enum` members automatically receive trailing commas:

```python
enum Color:
    RED = 0
    GREEN = 1
    BLUE = 2

struct Particle:
    pos: f32[3]
    mass: f32
    active: bool

class Counter(BaseCounter):
    public:
        count: i64
        def increment(delta: i64) -> i64:
            self->count += delta
            return self->count
```

Transpiles to:

```cpp
enum Color {
    RED = 0,
    GREEN = 1,
    BLUE = 2,
};

struct Particle {
    float pos[3];
    float mass;
    bool active;
};

class Counter : public BaseCounter {
public:
    int64_t count;
    int64_t increment(int64_t delta) {
        this->count += delta;
        return this->count;
    }
};
```

---

### 9. Formal Grammar (EBNF)

```ebnf
module         = { blank_line | comment_line | directive_line | statement(0) } ;

statement(n)   = indent(n) ( simple_stmt | compound_stmt(n) ) ;

simple_stmt    = ( func_proto | type_alias | var_decl | expr_stmt | "pass" ) [ inline_comment ] EOL ;
compound_stmt(n)
               = block_header ":" [ inline_comment ] EOL
                 { blank_line | comment_line }
                 statement(n + 1) { blank_line | comment_line | statement(n + 1) } ;

block_header   = func_def
               | ( "if" | "elif" | "while" | "switch" | "match" ) expr
               | "else" | "do" | "try" | "default"
               | "case" expr
               | "for" ( range_clause | c_for_clause )
               | ( "struct" | "class" | "union" | "enum" | "namespace" ) ident [ "(" ident ")" ] ;

func_def       = [ "static" | "inline" | "virtual" ] ( "def" | "fn" ) ident "(" [ param_list ] ")" [ "const" ] [ "->" type_spec ] ;
func_proto     = [ "static" | "inline" ] ( "def" | "fn" ) ident "(" [ param_list ] ")" [ "->" type_spec ] ;
param_list     = param { "," param } ;
param          = ident ":" type_spec [ "=" expr ] | "..." ;

var_decl       = [ "const" | "static" | "extern" ] ident ":" type_spec [ "=" expr ] ;
type_alias     = "type" ident "=" type_spec ;
range_clause   = ident [ ":" type_spec ] "in" expr [ ( ".." | "..=" ) expr [ "step" expr ] ] ;

directive_line = ( "import" ( strata_module | c_header_file )
                 | "from" ( strata_module | c_header_file ) "import" ( "*" | ident_list )
                 | "define" ident [ expr ]
                 | ( "ifdef" | "ifndef" | "pragma" ) rest_of_line
                 | "endif" ) EOL ;
strata_module  = ident ;                       (* Extensionless: resolves to .st module -> "ident.h" *)
c_header_file  = path_stem "." ( "h" | "c" | "hpp" ) ; (* Explicit extension: resolves to <path.h> *)
```

---

## Single-File C Reference Implementation (`strata.c`)

[`strata.c`](strata.c) is a zero-dependency C11 library and CLI tool providing bidirectional Strata ↔ C/C++ transpilation, syntax validation, formatting, direct compilation, and benchmarking.

### Build & CLI Usage

```sh
# Build the strata binary
cc -O3 -std=c11 -Wall -Wextra strata.c -o strata

# Transpile a .st file to C/C++ on stdout
./strata examples/hello.st

# Transpile, compile with cc, and run immediately
./strata --run examples/hello.st

# Compile a .st file directly to a native binary
./strata --compile examples/shapes.st -o shapes && ./shapes

# Format/canonicalize Strata (default 4 spaces, or any N spaces / tab)
./strata --strata examples/hello.st
./strata --strata --indent tab examples/hello.st

# Convert existing C/C++ source code into Strata
./strata --from-c input.c
./strata --from-c --indent tab input.c

# Validate Strata syntax & indentation rules only
./strata --check examples/hello.st

# Run built-in conformance tests & throughput benchmark
./strata --test && ./strata --bench
```

### Examples Directory

See [`examples/`](examples/) for sample `.st` programs ready to transpile or run:

- [`examples/hello.st`](examples/hello.st) — Minimal introduction to Strata types, `def` functions, `for ..` ranges, and `and`/`or`/`not`
- [`examples/shapes.st`](examples/shapes.st) — `struct`, `enum`, pointers, and `switch`/`case` geometry calculator
- [`examples/tabs.st`](examples/tabs.st) — Tab-indented Fibonacci calculator demonstrating auto-detected `\t` indentation and inclusive `2..=n` ranges
- [`examples/cal.st`](examples/cal.st) — Full real-world Unix `cal` CLI utility ported from [`util-linux/misc-utils/cal.c`](https://raw.githubusercontent.com/util-linux/util-linux/refs/heads/master/misc-utils/cal.c) (Gregorian 1752 reformation, Julian `-j`, ISO/US week numbers `-w`, `-1`/`-3`/`-y`/`-Y`/`-n`/`-S`, vertical `-v`, and ANSI highlight)

---

## Benchmarks: Strata vs C Source Size & Transpilation Speed

Measured on macOS (`Clang -O3`) across a 250-function systems module (`Node` structs, pointer arithmetic, loops, and multi-branch conditionals):

| Metric | 4-Space Strata (`.st`) | Tab `\t` Strata (`.st`) | Equivalent C (`.c`) |
| :--- | :--- | :--- | :--- |
| **Source Payload Size** | **`83.2 KiB`** (`-9.8%` vs C) | **`70.0 KiB`** (**`-24.1%`** vs C) | `92.2 KiB` (`94,435` bytes) |
| **Strata → C Speed** | **`2,772 files/s`** (`225.1 MiB/s`) | **`3,155 files/s`** (`215.6 MiB/s`) | — |
| **C → Strata Speed** | **`1,676 files/s`** (`150.9 MiB/s`) | **`1,710 files/s`** (`154.0 MiB/s`) | — |
