# Marmot

A statically-typed functional programming language featuring algebraic data types, pattern matching, typeclasses, and a module system. Compiles to bytecode for the Marmot Virtual Machine with garbage-collected memory management.

## Key Features

- **Static Type System** - Strong static typing with bidirectional inference, generics, and constraints
- **Pattern Matching** - Exhaustive pattern matching on unions and `Bool`
- **Algebraic Data Types** - Structs (product types) and unions (sum types)
- **Type Aliases** - Create readable names for complex types
- **Typeclasses** - Constrained generics with associated types and instance dispatch
- **Deriving** - Generate structural and container helpers from type declarations
- **Module System** - Explicit imports/exports with privacy enforcement
- **Package System** - Early manifest-based package loading with native FFI bindings
- **Pipe Operator** - Functional composition with `|>`, inferred lambdas, and `|> match with`
- **Ranges** - Elegant `start..step..end` syntax for loops
- **Closures** - First-class functions with lexical scoping
- **Expression-Oriented** - Everything is an expression with a value

Current scope note: there is no `async` / `await` surface in the current language.

## Quick Start

## Installation

From the repository root, on Windows or Linux:
```bash
python scripts/dev.py doctor               # check the toolchain
python scripts/dev.py install --rebuild    # build Release and the marmot tool, then install
python scripts/dev.py uninstall
```

`install` copies `marmotc`, `marmotvm`, the `marmot` tool and the prelude. On
Windows it goes to `%LOCALAPPDATA%\Marmot` and sets `MARMOT_PATH` and `PATH` for
new terminals. On Linux it goes to `~/.local/share/marmot` (or
`$XDG_DATA_HOME/marmot`) and writes an `env.sh` there for your shell profile to
source. Without `--rebuild` it installs the build made last; `--build` picks
one.

## Getting Started

Marmot has three programs:

- `marmot`, the project tool (Rust, in `tool/`): projects, packages, running and
  testing
- `marmotc`, the compiler: it compiles a program to a `.mmc` file and never runs
  one
- `marmotvm`, the virtual machine: it runs a `.mmc` file and compiles nothing

After all three are on your `PATH`, a basic workflow uses the tool:

```powershell
marmot init hello-world
cd hello-world

marmot check src/Main.mmt
marmot fmt
marmot run src/Main.mmt
marmot test
```

`marmot run` builds the program into `target/` at the project root and runs it
in `marmotvm`; `marmot test` does the same for every test, in parallel. A second
`marmot run` skips the build when the plan, the compiler and every file the
program was built from are unchanged, and `--rebuild` builds anyway.
Inside a project, `marmot fmt` formats the project's sources (`--check` only
checks them), leaving installed packages, the prelude and `marmot_path`
registries alone; `marmot fmt <file|dir>...` formats just those.

Every `.mmt` source file must begin with an explicit `module` declaration. Short snippets below may omit it for brevity, but complete file examples include it.

### Hello World
```marmot-test name=readme/hello_world path=.doc_examples/readme_hello_world.mmt
module Main

// Path import (relative or absolute)
import { "../MarmotPrelude/IO.mmt" }

IO::PrintLine("Hello, Marmot!");
```

### Basic Types & Variables
```marmot-test name=readme/primitive_types path=.doc_examples/readme_primitive_types.mmt module=ReadmePrimitiveTypes
def number : Int = 42;
def pi : Float = 3.14159;
def message : Text = "Hello";
def flag : Bool = true;
def items : Array<Int> = [1, 2, 3, 4, 5];
```

### Tuples and Destructuring
```marmot-test name=readme/tuples_destructuring path=.doc_examples/readme/tuples_destructuring.mmt module=ReadmeTuplesDestructuring
def pair = (42, "answer");
def (count, label) = pair;
```

### Functions
```marmot-test name=readme/functions path=.doc_examples/readme_functions.mmt module=ReadmeFunctions
// Simple function
def square = fn(x: Int) -> Int => x * x;

// Function with type inference
def add = fn(a: Int, b: Int) -> Int => a + b;

// Generic function
def identity = fn<T>(value: T) -> T => value;

// Higher-order function
def apply = fn<T, R>(f: fn(T) -> R, value: T) -> R => f(value);
```

### Control Flow
```marmot-test name=readme/control_flow path=.doc_examples/readme_control_flow.mmt module=ReadmeControlFlow
import { "../MarmotPrelude/IO.mmt" }

def x = 3;

// If-else expression
def result = if x > 0 then "positive" else "non-positive";

// For loop with ranges
for i in 0..1..10 {
    IO::PrintLine(i as Text);
};

// For loop with arrays
def names = ["Alice", "Bob", "Charlie"];
for name in names {
    IO::PrintLine(name);
};

// Array length operator
def arr = [1, 2, 3, 4, 5];
def len = #arr;  // 5

// A loop that carries state is a named recursive helper: there is no `loop`,
// `break` or assignment to reach for.
def SumTo = fn(count: Int, limit: Int, total: Int) -> Int =>
    if count >= limit then total else SumTo(count + 1, limit, total + count);

def sum = SumTo(0, 10, 0);  // 45
```

### Records (Product Types)
```marmot-test name=readme/records path=.doc_examples/readme_records.mmt module=ReadmeRecords
type Point = {
    x: Float,
    y: Float,
};

type Box<T> = {
    value: T,
};

def origin = Point(0.0, 0.0);
def boxed = Box(42);
def x_coord = origin.x;
```

### Sums (Union Types)
```marmot-test name=readme/unions path=.doc_examples/readme_unions.mmt module=ReadmeUnions
type Option<T> = None | Some(T);

type List<T> = Cons(T, List<T>) | Nil;

def maybe_value = Option::Some(42);
def empty_value : Option<Int> = Option::None();
def empty_list : List<Int> = List::Nil();
```

### Type Aliases and Distinct Types

`alias` abbreviates a type expression: the name and what it stands for are
interchangeable. `type Name = Existing` introduces a *distinct* type over that
representation, which does not mix with it by accident.

```marmot-test name=readme/type_aliases path=.doc_examples/readme_type_aliases.mmt module=ReadmeTypeAliases
// An alias is transparent: UserId and Int are the same type.
alias UserId = Int;
alias Name = Text;

def user_id: UserId = 42;
def user_name: Name = "Alice";

// Aliases abbreviate any type expression, generic ones included.
type Pair<A, B> = { first: A, second: B };
alias IntPair = Pair<Int, Int>;

def coords: IntPair = Pair(1, 2);

// `type` over an existing type is distinct, not transparent: a Meters is not an
// Int until it is converted, which is what keeps the two from being mixed up.
type Meters = Int;

def distance: Meters = 12 as Meters;
def raw: Int = distance as Int;
```

### Pattern Matching
```marmot-test name=readme/pattern_matching path=.doc_examples/readme_pattern_matching.mmt module=ReadmePatternMatching
type Result<T, E> = Ok(T) | Err(E);

def handle_result = fn(result: Result<Int, Text>) -> Text =>
    match result with
        case Result::Ok(value) => "Success: " ++ (value as Text)
        case Result::Err(msg) => "Error: " ++ msg;
```

### Typeclasses
```marmot-test name=readme/typeclasses path=.doc_examples/readme_typeclasses.mmt module=ReadmeTypeclasses
// Define a typeclass
class Show<T> {
    show: fn(value: T) -> Text;
};

// Implement for Int
instance Show<Int> {
    def show = fn(value: Int) -> Text => value as Text;
};

// Use with constraints
def display = fn<T>(value: T) -> Text where Show<T> => Show::show(value);

def message = display(42);  // "42"
```

### Associated Types
```marmot-test name=readme/associated_types path=.doc_examples/readme/associated_types.mmt module=ReadmeAssociatedTypes
type Option<T> = None | Some(T);

// Next yields the item and the rest of the sequence, so an iterator is a value
// rather than something that advances in place.
class Iterable<Iter> {
    type Item;
    Next: fn(iter: Iter) -> Option<(Item, Iter)>;
};

def NextValue = fn<Iter>(iter: Iter) -> Option<(Iterable::Item<Iter>, Iter)>
    where Iterable<Iter> => Iterable::Next(iter);
```

### Deriving
```marmot-test name=readme/deriving path=.doc_examples/readme_deriving.mmt module=ReadmeDeriving
import { "../MarmotPrelude/Equatable.mmt", "../MarmotPrelude/Hashable.mmt" }

type Point = {
    x: Int,
    y: Int
} deriving (Equatable, Hashable);

type OptionBox<T> = Empty | Full(T) deriving (Map, Bind, Unwrap);
```

### Module System
```marmot
// Define a module (MyModule.mmt)
module MyModule
public export { add, multiply }

def add = fn(a: Int, b: Int) -> Int => a + b;
def multiply = fn(a: Int, b: Int) -> Int => a * b;
def internal = fn() -> Int => 100;  // Not exported

// Use in another file
// Path import (relative or absolute)
import { "./MyModule.mmt" }

// Or search in MARMOT_PATH (semicolon-separated on Windows, colon-separated on Unix)
// import { <MyModule> }

def result = MyModule::add(5, 3);
```

### Package System

Marmot has local packages: versioned dependencies resolved from package directories on disk, vendored into the project, and locked in `marmot.lock`, with optional native FFI libraries. There is no remote registry yet.

**Package structure:**
```text
PackageName/
  package.marmot
  PackageName.mmt
  lib/
    windows/x64/packagename.dll
```

The `marmot` tool reads the manifests: `marmot install PackageName` adds the
dependency, resolves it and vendors it into `packages/`, and `marmot run` hands
the compiler a [build plan](docs/plan-file.md) with the package on the search
path and its native library's location. Exported symbols still come from the
module's `public export` / `private export` blocks. `marmotc` on its own reads
no manifests: it finds `<Name>` imports through `MARMOT_PATH`, and native
libraries through `--library-path`, `MARMOT_LIBRARY_PATH` and the declaring
module's directory.

**Using a package:**
```bash
marmot install PackageName
marmot run
```

```marmot
import { <PackageName> }

def result = PackageName::NativeFunction(arg1, arg2);
```

See [Package System](docs/package-system.md) for the current manifest fields, dynamic-loading behavior, and FFI ABI limits.

### Pipe Operator
```marmot-test name=readme/pipelines path=.doc_examples/readme_pipelines.mmt module=ReadmePipelines
def double = fn(x: Int) -> Int => x * 2;
type Result<T, E> = Ok(T) | Err(E);

def transform = fn(value: Int) -> Result<Int, Text> => {
    if value > 10
    then Result::Ok(value + 1)
    else Result::Err("too small")
};

def result =
    5
    |> double
    |> fn(x) => { x + 1 }
    |> transform
    |> match with
        case Result::Ok(value) => value
        case Result::Err(_) => 0;
```

In pattern position, `_` is a wildcard that ignores the matched value and does not bind a local. Outside patterns, `_` remains a normal identifier.

### Closures
```marmot-test name=readme/closures path=.doc_examples/readme_closures.mmt module=ReadmeClosures
def make_adder = fn(amount: Int) -> fn(Int) -> Int => fn(n: Int) -> Int => n + amount;

def add_ten = make_adder(10);
def first = add_ten(1);   // 11
def second = add_ten(5);  // 15
```

### Cells
A name never changes, but a cell's contents can. Every name, closure, array or
record holding the same cell sees `Cell::Set`, which stores a value and evaluates
to it.
```marmot-test name=readme/cells path=.doc_examples/readme_cells.mmt module=ReadmeCells
import { "../MarmotPrelude/Cell.mmt" }

def counter = Cell::New(0);                                  // Cell<Int>
def Next = fn() -> Int => Cell::Set(counter, Cell::Get(counter) + 1);
def first = Next();   // 1
def second = Next();  // 2
```

Knuth's Man or Boy test, which needs a closure to change a variable its creator
can see, reads like the original:
```marmot-test name=readme/man_or_boy path=.doc_examples/readme_man_or_boy.mmt module=ReadmeManOrBoy
import { "../MarmotPrelude/Cell.mmt" }

def A = fn(k: Int, x1: fn() -> Int, x2: fn() -> Int, x3: fn() -> Int, x4: fn() -> Int, x5: fn() -> Int) -> Int =>
{
    def cell = Cell::New(k);
    def B = fn() -> Int => A(Cell::Set(cell, Cell::Get(cell) - 1), B, x1, x2, x3, x4);
    if k > 0 then B() else x4() + x5()
};

def answer = A(10, fn() -> Int => 1, fn() -> Int => -1, fn() -> Int => -1, fn() -> Int => 1, fn() -> Int => 0);  // -67
```

## Language Features

### Type System
- **Primitive Types**: `Int`, `Float`, `Byte`, `Word`, `Bool`, `Text`, `Unit`
- **Composite Types**: `Array<T>`, tuples, structs, unions
- **Function Types**: `fn(T1, T2) -> R`
- **Type Aliases**: `type UserId = Int;` for readable type names
- **Generic Parameters**: Single and multiple type parameters
- **Type Constraints**: `where` constraints on functions, structs, and unions
- **Associated Types**: Projections such as `Iterable::Item<Iter>`
- **Deriving**: `Equatable`, `Hashable`, `Map`, `Bind`, and `Unwrap`
- **Type Inference**: Automatic type deduction for instantiation, constructors, and context-aware lambdas
- **Tuple Destructuring**: `def (x, y) = pair;`

#### Numeric Limits

**Integer (Int)** - 64-bit signed integer:
- Maximum value: `9223372036854775807` (2^63 - 1)
- Minimum value: `-9223372036854775807` (practical limit)
- **Note**: The literal `-9223372036854775808` (INT64_MIN) causes a parser overflow and should be avoided

**Float** - Double-precision floating point:
- Range: Approximately ±1.7E+308
- Precision: ~15-17 decimal digits

**Unicode Text Support**:
- Full UTF-8 encoding support
- String length returns code point count (not byte count)
- Supports multi-byte characters from all Unicode planes

### Operators
- **Arithmetic**: `+`, `-`, `*`, `/`, `%`
- **Comparison**: `==`, `!=`, `<`, `>`, `<=`, `>=`, with typeclass dispatch for user-defined types through `Equatable<T>` and `Orderable<T>`
- **Logical**: `&&`, `||`, `!`
- **Bitwise**: `&`, `|`, `^`, `<<`, `>>`
- **Casts**: `as`, with builtin primitive conversions and constrained dispatch through `Convertable<From, To>`
- **Concatenation**: `++` for `Text` and `Array<T>`, with constrained dispatch through `Concatenable<T>`
- **Pipe**: `|>` (function composition)
- **Length**: `#` for arrays and other countable shapes, with constrained dispatch through `Countable<T>`

There is no assignment operator, compound or otherwise, and no in-place
mutation: `def` binds a name once and the name never changes value.
Where a program genuinely needs shared, changing state, it uses a `Cell<T>`
(see [Cells](#cells)), whose contents change while every binding stays fixed.
Concatenating onto an existing binding produces a new value under a new name:

```marmot
def updated_items = items ++ [value];
def updated_text = text ++ suffix;
```

### Advanced Features
- **Recursive Data Types**: Self-referential unions for lists, trees
- **Exhaustive Matching**: Compiler-enforced pattern coverage
- **Constructor Inference**: Generic constructor arguments can be inferred from arguments and context
- **Pipe Into Match**: Pipelines can flow directly into `match with`
- **Range Expressions**: `start..step..end` with positive/negative steps
- **Array Iteration**: `for x in array` iterates over elements
- **Float Ranges**: Support for decimal step values
- **Nested Generics**: Complex generic type compositions
- **Cross-Module Typeclasses**: Import and use classes, instances, and associated types across modules

## Standard Library

`MarmotPrelude` is the standard-library layer that ships with the repo. The main module groups are:

- **Core ADTs** - `Prelude/Option.mmt`, `Prelude/Result.mmt`, `Prelude/List.mmt`
- **Collections** - `Collections/Map.mmt`, `Collections/Set.mmt`
- **Effects** - `IO.mmt`, `System.mmt`, `DateTime.mmt`
- **Built-in helpers** - `TextUtil.mmt`, `ArrayUtil.mmt`, `Math.mmt`
- **Shared state** - `Cell.mmt` (`Cell::New`, `Cell::Get`, `Cell::Set`)
- **Helper / typeclass modules** - `Concatenable`, `Convertable`, `Countable`, `Equatable`, `Hashable`, `Iterable`, `Orderable`, `Prelude/Panic`

The public IO and system surface now prefers typed wrappers over sentinel values. Common entry points include:

- `IO::TryReadFile`, `IO::TryWriteFile`, `IO::TryAppendToFile`, `IO::TryDeleteFile`, `IO::TryRenameFile`, and `IO::TryGetFileSize`
- `System::TryGetEnv`, `System::CurrentDirectory`, `System::TrySetCurrentDirectory`, `System::Run`, `System::CurrentPlatform`, and `System::CurrentProcessId`
- `DateTime::LocalNow`, `DateTime::UtcNow`, `DateTime::NowUnixMillis`, and `DateTime::FormatLocal`
- `TextUtil::Length`, `TextUtil::Split`, `TextUtil::Replace`, `TextUtil::Trim`, and `TextUtil::Reverse`
- `ArrayUtil::WithAppended`, `ArrayUtil::Slice`, and `ArrayUtil::Reverse` (arrays are immutable; these return new arrays)

`Prelude/Result.mmt` uses `Result::Ok` and `Result::Err`. The older `Result::OK` and `Result::Error` spellings are removed from the public prelude API.

See [Prelude](docs/prelude.md) for module-by-module notes and examples covering the typed `IO`, `System`, `DateTime`, `TextUtil`, `ArrayUtil`, and helper/typeclass modules.

## Foreign Function Interface (FFI)

Marmot has two FFI call paths:

- `CALL_FOREIGN_INDEXED` for built-in runtime functions registered in `MidoriFFIRegistry`
- `CALL_FOREIGN` for generic external functions, including package-provided dynamic libraries

The examples below describe the generic `CALL_FOREIGN` ABI used by ordinary external and package functions.

### Declaring Foreign Functions

Use the `foreign` keyword to declare external functions:

```marmot
// Declare a foreign function
foreign "MIDORI_FFI_Print" Print : fn(Text) -> Unit;

// With multiple parameters
foreign "MIDORI_FFI_WriteFile" WriteFile : fn(Text, Text) -> Bool;

// Returning complex types
foreign "MIDORI_FFI_ReadBinaryFile" ReadBinaryFile : fn(Text) -> Array<Byte>;

// From a native library: the symbol, then the library that exports it
foreign "marmot_image_width" Width : fn(Int) -> Int from "marmot_image";

// Several functions from one library
foreign "marmot_image"
{
    "marmot_image_width" ImageWidth : fn(Int) -> Int;
    "marmot_image_height" ImageHeight : fn(Int) -> Int;
}
```

A declaration without `from` must name a builtin. See
[Build Plans](docs/plan-file.md#native-libraries) for how `marmotvm` finds a
library.

### Supported Surface

- Raw scalars: `Int`, `Float`, `Bool`, `Byte`, `Word`, `Unit`
- `Text`
- `Array<T>`

### FFI Function Signature

All FFI functions must follow this signature:

```cpp
extern "C" {
    MIDORI_STDLIB_API void MIDORI_FFI_FunctionName(void** args, void* ret) noexcept;
}
```

`ret` always points at an 8-byte return slot. `args` is a `void**`, but generic dynamic calls do not carry the richer builtin metadata from `CALL_FOREIGN_INDEXED`.

### Type Marshalling

#### Raw Scalar Types

For generic dynamic FFI, raw scalar bits are stored in the pointer-sized `args[i]` slot itself. Read them by copying from `&args[i]`, not by treating `args[i]` as a pointer to the scalar value.

```cpp
int64_t value;
std::memcpy(&value, &args[0], sizeof(value));

bool flag;
std::memcpy(&flag, &args[1], sizeof(flag));
```

Return raw scalars by copying the value bytes into `ret`:

```cpp
int64_t result = 42;
std::memcpy(ret, &result, sizeof(result));

bool success = true;
std::memcpy(ret, &success, sizeof(success));

// Unit
std::memset(ret, 0, sizeof(double));
```

#### Text Type

Receive text arguments as a C string pointer:

```cpp
const char* text = static_cast<const char*>(args[0]);
```

Return text as a `malloc`-allocated `char*` written into the 8-byte return slot:

```cpp
const size_t size = std::strlen(data) + 1;
char* result = static_cast<char*>(std::malloc(size));
std::memcpy(result, data, size);

const int64_t ptr = reinterpret_cast<int64_t>(result);
std::memcpy(ret, &ptr, sizeof(int64_t));
```

The VM copies returned text into Marmot-managed storage and then frees the original string.

#### Array Type

Receive arrays through an array-view struct:

```cpp
struct ArrayArgument {
    void* data;
    int length;
};

const ArrayArgument* array = static_cast<const ArrayArgument*>(args[0]);
```

Return arrays as a heap-allocated wrapper pointing at a heap-allocated element buffer:

```cpp
struct FFIArray {
    void* data;
    int length;
};

std::uint64_t* array_data =
    static_cast<std::uint64_t*>(std::malloc(length * sizeof(std::uint64_t)));

for (int i = 0; i < length; i++) {
    int64_t value = i * 10;
    std::memcpy(&array_data[i], &value, sizeof(value));
}

FFIArray* result = static_cast<FFIArray*>(std::malloc(sizeof(FFIArray)));
result->data = array_data;
result->length = length;

const int64_t ptr = reinterpret_cast<int64_t>(result);
std::memcpy(ret, &ptr, sizeof(int64_t));
```

For flat scalar arrays, each element slot should use Marmot's 8-byte runtime value layout.

### Memory Management Rules

- Use `malloc` / `free` compatible allocation for returned text and arrays.
- Do not free returned buffers after writing their pointer into `ret`; the VM takes ownership.
- Returned arrays are wrapped through `MidoriArray::FromFFI`.
- Short returned arrays are copied into Marmot small-object storage and their original FFI buffer is freed.
- Longer returned arrays are adopted directly without an element copy.
- The outer `FFIArray` wrapper itself is always freed by the VM.
- FFI code should not access the VM's garbage collector or internal runtime objects directly.
- Return values still have to fit in the 8-byte `ret` slot.

See [Package System](docs/package-system.md) for the current manifest-driven loading flow, runtime lookup order, and dynamic ABI limits.

## Development

### Building Marmot

`scripts/dev.py` runs every everyday task the same way on Windows and Linux;
`python scripts/dev.py` lists its commands and `<command> --help` explains one.
On Windows it loads the MSVC environment itself, so any terminal will do.

```bash
python scripts/dev.py doctor                    # toolchain check, and what is built
python scripts/dev.py build                     # marmotc and marmotvm (Development)
python scripts/dev.py build all --build Release # plus the unit tests and the marmot tool
python scripts/dev.py configure --fresh         # reconfigure, e.g. after changing CXX
python scripts/dev.py clean
```

Underneath are CMake presets: `x64-debug`, `x64-development`, `x64-release` and
`x64-experimental` on Windows, and the same four as `linux-*` on Linux. Builds
write the executables to `out/build/ninja/<preset>/out/`. The compiler needs a
C++23 standard library with `<print>`, `<expected>` and `std::ranges::to`: MSVC
19.37+, GCC 14+, or Clang 19+. On Linux the scripts pick one that is new enough
when a build tree is first configured; `CXX` overrides the choice. By hand:
```bash
cmake --preset linux-development
cmake --build --preset linux-development --target marmotc marmotvm
```

### Running Programs

`python scripts/dev.py run path/to/program.mmt` builds a program with the
checkout's compiler and runs it, with the checkout's prelude first on
`MARMOT_PATH`; `--check` only type-checks it. `marmot run` does the same with
the installed tools. By hand:

```bash
marmotc check path/to/program.mmt    # type-check only
marmotc build path/to/program.mmt    # compile to path/to/program.mmc (or -o)
marmotvm path/to/program.mmc         # run the compiled program
```

### Running Tests

See [Testing Guide](docs/testing.md) for the `tests/` vs `test/` split, helper
usage, and every check.

```bash
python scripts/dev.py gate                      # everything a commit must pass
python scripts/dev.py test                      # the language suite under test/
python scripts/dev.py test --category closure
python scripts/dev.py test --test closure/simple.mmt
python scripts/dev.py unit --tag "[runtime]"    # the C++ unit tests
python scripts/dev.py snapshot closure/new_test.mmt
```

Debug and Development builds include the unit tests. Release builds leave them
out unless configured with them (`dev.py configure --build Release --unit-tests`;
`dev.py gate --build Release` does that itself).

The `marmot` tool runs the language suite too, from the repository root, with
`MARMOTC` naming the compiler (marmotvm is found beside it) and `MARMOT_PATH`
the prelude:
```bash
marmot test
marmot test static_analyzer
marmot test --pattern recursive
```

Test fixtures are file-based:
```bash
# Add a new program test under test/<category>/<name>.mmt
# Put tests under a failure/ directory when they should fail compilation
# Add <name>.expected to assert stdout/stderr or compile-fail diagnostic snapshots
# Snapshot comparison strips ANSI codes and repo-root path prefixes first
# Add <name>.warnings.json to assert warning code, line, and message fragments
```

## Example Programs

### Recursive Fibonacci
```marmot-test name=readme/recursive_fibonacci path=.doc_examples/readme/recursive_fibonacci.mmt module=ReadmeRecursiveFibonacci
def fib = fn(n: Int) -> Int => {
    if n <= 1 then n else fib(n - 1) + fib(n - 2)
};
```

### Binary Tree
```marmot-test name=readme/tree_height path=.doc_examples/readme_tree_height.mmt module=ReadmeTreeHeight
type Tree<T> = Leaf(T) | Node(Tree<T>, Tree<T>);

def height = fn<T>(tree: Tree<T>) -> Int =>
    match tree with
        case Tree::Leaf(_) => 1
        case Tree::Node(left, right) => {
            def left_height = height(left);
            def right_height = height(right);
            1 + (if left_height > right_height
                 then left_height
                 else right_height)
        };
```

### Generic Linked List
```marmot-test name=readme/list_recursion path=.doc_examples/readme_list_recursion.mmt module=ReadmeListRecursion
type List<T> = Cons(T, List<T>) | Nil;

def length = fn<T>(list: List<T>) -> Int =>
    match list with
        case List::Cons(head, tail) => 1 + length(tail)
        case List::Nil => 0;

def map = fn<A, B>(list: List<A>, f: fn(A) -> B) -> List<B> =>
    match list with
        case List::Cons(head, tail) =>
            List::Cons(f(head), map(tail, f))
        case List::Nil =>
            List::Nil();
```

## Architecture

- **Frontend**: Lexer → Module Manager → Parser → Type Checker → Static Analyzer
- **Optimizer**: Constant folding, strength reduction, constant branch elimination, local constant propagation, dead code elimination, canonicalization cleanup, closure lifting, and tail call optimization; rerun until a fixpoint or the 8-iteration cap is reached
- **Backend**: Bytecode generator → Linker
- **Runtime**: Single `VirtualMachine` execution path with a non-moving, generational (bitmap mark-sweep) garbage collector

See [Runtime Architecture](docs/runtime-architecture.md) for details on VM execution, closure capture, and memory management.

## Documentation

See the [docs](docs/) folder for detailed technical documentation:

- [Type System](docs/type-system.md) - Type inference, type classes, and algebraic data types
- [Grammar](docs/grammar.md) - the language's syntax, fixed at v1
- [Prelude](docs/prelude.md) - Standard-library module map and typed wrapper examples
- [Compilation Workflow](docs/compilation-workflow.md) - Complete pipeline from lexing to linking
- [Feature Matrix](docs/feature-matrix.md) - Current feature status, stability levels, and primary automated coverage
- [Versioning Policy](docs/versioning-policy.md) - Compatibility rules for releases, deprecation, and breaking changes
- [Error Reporting](docs/error-reporting.md) - Structured diagnostics, warning/error codes, and machine-readable output
- [Diagnostic Format](docs/diagnostic-format.md) - Stable JSON envelope and diagnostic object schema for tooling
- [Formatting](docs/formatting.md) - Canonical formatter usage and current scope
- [Package System](docs/package-system.md) - Creating and using packages with native FFI bindings
- [Project Standard](docs/project-standard.md) - Standard project layout and manifest
- [Build Plans](docs/plan-file.md) - Compiling from an explicit JSON list of inputs instead of project discovery
- [Runtime Architecture](docs/runtime-architecture.md) - VM execution, closure capture, and garbage collection
- [Testing Guide](docs/testing.md) - Choosing between implementation tests and regression tests, with helper and command references

Editor assets:

- `editors/vscode/` - sample VSCode extension with syntax highlighting and on-save diagnostics
