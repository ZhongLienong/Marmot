# Marmot Type System

Marmot's type system is nominal. The checker unifies types and uses any
expected type to guide inference. Polymorphic definitions declare their
type parameters explicitly.

Unless a snippet explicitly shows a full file, the examples below focus on the type-system surface only. Complete `.mmt` source files still need an explicit `module` declaration.

## Overview

The type system provides:

- **Full local inference** for most expressions
- **Bidirectional inference** for constructors and lambdas when an expected type is available
- **Parametric polymorphism** for functions, structs, unions, and aliases
- **Ad-hoc polymorphism** through type classes and instances
- **Associated types** for type classes whose output types are determined by an input type
- **Nominal algebraic data types** through structs and unions
- **Deriving** for common structural and container operations
- **Constraint propagation** from both function signatures and type definitions
- **Compile-time exhaustiveness checking** for `match`, through nested patterns

## Primitive Types

| Type | Description |
|------|-------------|
| `Int` | 64-bit signed integer |
| `Float` | 64-bit IEEE 754 double |
| `Byte` | 8-bit unsigned integer |
| `Word` | 64-bit unsigned integer |
| `Bool` | Built-in Boolean with the values `true` and `false` |
| `Text` | UTF-8 string |
| `Unit` | A type with one value, `()`; used when a result carries no payload |

A decimal integer literal is an `Int`. A hex or binary literal is a `Byte` up to
`0xFF`, an `Int` above it, and a `Word` past the largest `Int`, so `0x0F` is a
`Byte` and `0x100` an `Int`; a smaller `Word` is written with `as Word`. Literals do not take their type from where
they are used: to mask an `Int`, write `value & 255` or `value & (0xFF as Int)`.

Integer arithmetic wraps: `Int` in two's complement, `Byte` and `Word` modulo
2^8 and 2^64, so `9223372036854775807 + 1` is `-9223372036854775808`. Division
truncates toward zero, and the one quotient that overflows, the minimum `Int`
divided by `-1`, wraps to itself with a remainder of `0`. Dividing by zero stops
the program with `DivisionByZero`. A shift by the type's width or more, or by a
negative count, shifts every bit out: `0`, or `-1` for a negative `Int` shifted
right. A compile-time constant is folded by the same rules the program runs by.

## Composite Types

### Arrays

Homogeneous arrays use `Array<T>`:

```marmot
Array<Int>
Array<Array<Text>>
```

Empty array literals require context:

```marmot
def values : Array<Int> = [];
```

Use `#` to get the length:

```marmot
def arr = [1, 2, 3, 4, 5];
def len = #arr;
```

Arrays are iterable:

```marmot
for name in ["Alice", "Bob", "Charlie"] {
    IO::PrintLine(name);
};
```

### Tuples

Tuples are fixed-size heterogeneous values:

```marmot
(Int, Text)
(Bool, Int, Float)
```

### Functions

Function types are first-class:

```marmot
fn(Int) -> Bool
fn(Int, Int) -> Int
fn() -> Unit
fn(fn(Int) -> Int) -> Int
```

### Concurrency Types

`Worker<T>` and `Channel<T>` are opaque handle types for the concurrency system:

```marmot
Worker<Int>       // handle to a worker that returns Int
Channel<Text>     // handle to a channel carrying Text values
```

Both are internally represented as `Int` handles but carry compile-time type parameters. `Worker<T>` is produced by `Concurrency::Spawn` and consumed by `Concurrency::Join`. The spawning VM owns the handle and can join, cancel, or poll that worker. `Channel<T>` is produced by `Concurrency::MakeChannel(capacity)`, which takes `T` from context, and used with `Concurrency::Send` and `Concurrency::Receive`. `Receive` returns `Option<T>`.

### References

`Ref<T>` is a box whose contents can change. The type and the native
`ref`, `*` and `:=` forms are available without an import. `Ref` is not a
keyword, so a program's own type named `Ref` takes precedence over it. A
declared or `use`d value named `ref` also keeps its ordinary name resolution.

| Native form | Result | Meaning |
|---|---|---|
| `ref value` | `Ref<T>` for a value of type `T` | A new reference holding `value`. |
| `*cell` | `T` for a `Ref<T>` | The value currently stored. |
| `cell := value` | `Unit` | Stores a value of the cell's element type. |

A cell is a reference: passing it, capturing it, or storing it in an array,
tuple or record shares the one cell. Bindings stay immutable, so the only state
that changes is a cell's contents, and a program's mutable state can be found by
looking for `Ref<...>` in its types.

Deliberately not provided: `Equatable`, `Hashable` or `Orderable` instances for
cells (compare contents with `*a == *b`) or any tracking of mutation in
function types. `!` remains Boolean negation. A cell that
reaches another worker is copied; see [Multicore Runtime](multicore-runtime.md).

### Ranges

Ranges are used directly in `for` loops:

```marmot
for i in 0..1..10 {
    IO::PrintLine(i as Text);
};

for i in 10..-1..0 {
    IO::PrintLine(i as Text);
};
```

Custom iteration is expressed through the `Iterable` type class and its associated `Item` type.

## Operator Grouping

Operators use the precedence and associativity in [the grammar](grammar.md#expressions).
`a + b * c` means `a + (b * c)`; `a - b - c` means `(a - b) - c`.
Parentheses override precedence, and comparison and Boolean operators combine
normally: `index >= 0 && index < #items`.

Arithmetic and bitwise operators use the operation classes exported by
`Numeric.mmt`: `Add`, `Subtract`, `Multiply`, `Divide`, `Remainder`, `BitAnd`,
`BitOr`, `BitXor`, `ShiftLeft`, `ShiftRight`, `Positive`, `Negate` and `BitNot`.
Binary classes take the left and right operand types; unary classes take one
type. Each declares an associated `Output` and a method with the class's name.
Thus `a + b` selects the same implementation as `Add::Add(a, b)`, and operands
and results may have different types. Primitive arithmetic remains available
without an import; importing `Numeric.mmt` makes its classes and primitive
instances available for explicit calls, generic constraints and custom instances.
A named method can be used as a value where its function type is known.
Primitive instances specialize to the same scalar instructions as native syntax.
`&&` and `||` remain Boolean operations with short-circuit evaluation.

## Algebraic Data Types

### Records

Records are nominal product types:

```marmot
type Point =
{
    x: Int,
    y: Int
};

type Box<T> =
{
    value: T
};
```

### Unions

Unions are nominal tagged sums:

```marmot
type Option<T> = None | Some(T);
type Result<T, E> = Ok(T) | Err(E);
```

### Constructor Type Argument Inference

A generic constructor takes its type arguments from the constructor arguments and the
surrounding expected type. There is no form for stating them, so every type parameter has
to be reachable from one of those two:

```marmot
def some_int = Option::Some(42);
def empty_int : Option<Int> = Option::None();

def Wrap = fn<T>(value: T) -> Option<T> => Option::Some(value);
```

A zero-arity variant has no argument to infer from, so it needs an expected type. An
annotation on the binding is the usual way to supply one, as `empty_int` does above.

Inference must resolve every omitted type parameter. If no argument or expected type pins it down, construction fails:

```marmot-test name=type-system/unresolved_none kind=failure path=.doc_examples/type_system/unresolved_none.mmt module=TypeSystemUnresolvedNone
type Option<T> = None | Some(T);
def unresolved = Option::None();  // error: missing type context
```

### Deriving

Marmot supports `deriving` on structs and unions for a focused set of generated operations.

Structural deriving:

```marmot
type Point =
{
    x: Int,
    y: Int
} deriving (Equatable, Hashable);
```

`Orderable` compares record fields in declaration order. The first field that
differs decides. For unions, it compares the variants first, also in
declaration order. If `Dot` comes first, `Shape::Dot() < Shape::Circle(9.0)`.
For two values of the same variant, it compares their fields in order. Each
field's type needs an `Orderable` instance, as does each type parameter of a
generic type.

```marmot
type Version = { major: Int, minor: Int, label: Text } deriving (Equatable, Orderable);
```

`Convertable` derives the conversion to `Text`, so `value as Text` shows the
value as it is written: a record with its field names, `Version { major = 1,
minor = 2, label = beta }`, and a variant with its union, `Shape::Box(3, 4)`.
Each field shows as its own `as Text` gives it, so a text field is not quoted,
and its type needs a conversion to `Text`: the built-in types have one, an
array has one when its elements do, and a generic type's instance requires
`Convertable<T, Text>` of each type parameter.

Container deriving on unions:

```marmot
type OptionBox<T> = Empty | Full(T) deriving (Map, Bind, Unwrap);

def mapped = OptionBoxMap(OptionBox::Full(5), fn(x) => { x + 1 });
```

Transferable deriving for concurrency:

```marmot
type Point =
{
    x: Float,
    y: Float
} deriving (Transferable);

type Result = Ok(Int) | Err(Text) deriving (Transferable);
```

Use `deriving (Transferable)` to generate a marker instance for a struct or
union. Its fields or variant payloads must satisfy `Transferable`. The VM
uses the aggregate's shape to copy it. For a function, the VM copies the
procedure index and the captured values. The function type describes its
parameters and result, so the checker cannot inspect captures from the type.

Current support is intentionally narrow:

- `Equatable`, `Hashable`, `Orderable`, `Convertable` to `Text`, and `Transferable` are supported structural derives
- `Map`, `Bind`, and `Unwrap` are supported container derives
- Structural deriving works on generic and recursive structs and unions; the instance for `Box<T>` requires `Equatable<T>` (or `Hashable<T>`) of each type parameter
- `Transferable` deriving works on structs and unions including recursive union shapes
- Container deriving maps only the first type parameter and supports pass-through variants, single-value variants, and recursive self fields

## Type Aliases and Distinct Types

`alias` gives a type expression a shorter name. The alias and what it stands for are the same type:

```marmot
alias UserId = Int;
alias Name = Text;

alias IntPair = Pair<Int, Int>;
alias IntArray = Array<Int>;
```

Aliases can be imported and exported like other symbols. They are fully interchangeable with the underlying type and have no runtime cost.

`type Name = Existing` is the other half of that pair and does the opposite: it introduces a **distinct** type over the representation, so the two do not mix by accident. Conversion is explicit, through `as`, and costs nothing at runtime:

```marmot
type Meters = Int;

def distance : Meters = 12 as Meters;
def raw : Int = distance as Int;
```

A distinct type gets its own typeclass instances, because instance lookup is keyed on the type's name rather than its representation.

## Generics and Constraints

Functions, structs, unions, and aliases can all be parameterized:

```marmot
def identity = fn<T>(value: T) -> T => value;

type Pair<A, B> =
{
    first: A,
    second: B
};
```

Functions can require type class constraints:

```marmot
class Show<T> {
    show: fn(value: T) -> Text;
};

def Display = fn<T>(value: T) -> Text where Show<T> => Show::show(value);
```

Type definitions can also carry constraints:

```marmot
type Box<T> where Show<T> = {
    value: T
};

def ShowBox = fn<T>(box: Box<T>) -> Text => Show::show(box.value);
```

Constraints attached to a struct or union are checked when the type is instantiated and automatically propagate when a function accepts that type.

A generic parameter stands for whatever type a caller picks, so a body may not decide it. `fn<T>(x: T) -> T => 5` is rejected, because the body only works when `T` is `Int`. The same goes for treating two parameters as one type. An operator applies to a generic parameter only where a constraint names the class that provides it: `Add<L, R>` for `+`, `Negate<T>` for unary `-`, and the corresponding operation class for the other arithmetic and bitwise operators. `Equatable<T>`, `Orderable<T>`, `Concatenable<T>`, `Countable<T>` and `Convertable<From, To>` provide `==`, `<`, `++`, `#` and `as`. Primitive shifts take an `Int` amount. The `Numeric<T>` marker constrains primitive range bounds and has instances for `Int`, `Float`, `Byte` and `Word`; it does not imply arithmetic capabilities.

```marmot
def Sum = fn<L, R>(a: L, b: R) -> Add::Output<L, R> where Add<L, R> => a + b;
```

## Type Classes

Marmot supports both single-parameter and multi-parameter type classes.

### Class Definitions and Instances

```marmot
class Show<T> {
    show: fn(value: T) -> Text;
};

instance Show<Int> {
    def show = fn(value: Int) -> Text => value as Text;
};
```

Methods are accessed through qualified syntax:

```marmot
def text = Show::show(42);
```

### Associated Types

Associated types let a class determine a related type from its instance head:

```marmot
class Iterable<Iter>
{
    type Item;
    Next: fn(iter: Iter) -> Option<(Item, Iter)>;
};
```

Instances bind the associated type:

```marmot
type Counter =
{
    current: Int,
    end: Int
};

instance Iterable<Counter>
{
    type Item = Int;

    def Next = fn(counter: Counter) -> Option<(Int, Counter)> =>
        if counter.current >= counter.end
        then Option::None()
        else Option::Some((counter.current, { counter with current = counter.current + 1 }));
};
```

Use projection syntax to refer to an associated type in other signatures:

```marmot
def NextValue = fn<Iter>(iter: Iter) -> Option<(Iterable::Item<Iter>, Iter)>
    where Iterable<Iter> => Iterable::Next(iter);
```

Associated types are especially useful when one type parameter logically determines another, while multi-parameter type classes remain available for other cases.

### Operator-Backed Type Classes

Several operators are wired into the type checker and lowering so they can dispatch through type classes when the operands are not handled entirely as builtins.

- `as` can use `Convertable<From, To>`
- `++` can use `Concatenable<T>`
- `#` can use `Countable<T>`
- `==` and `!=` can use `Equatable<T>`
- `<`, `<=`, `>`, and `>=` can use `Orderable<T>`. Its `Compare` method returns `Orderable::Ordering`: `Less`, `Equal`, `Greater` or `Unordered`. All four relational operators return false for `Unordered`, including NaN Float comparisons. Derived ordering propagates an unordered field comparison.

Examples:

```marmot
def ConvertIt = fn<From, To>(value: From) -> To
    where Convertable<From, To> => {
    value as To
};

def Join = fn<T>(left: T, right: T) -> T
    where Concatenable<T> => {
    left ++ right
};
```

The current implementation mixes direct builtin lowering with these hooks:

- `as` prefers direct builtin casts for concrete primitive conversions, but generic and constrained code can lower through `Convertable`
- `++` is builtin for `Text` and `Array<T>`, and can also lower through `Concatenable`
- `#` is builtin for arrays, has specialized lowering for prelude `List`, `Map`, and `Set` shapes, and otherwise falls back to `Countable`
- equality and ordering use builtin lowering for the primitive cases and typeclass dispatch for user-defined cases

The shipped prelude provides the following related modules:

- `Convertable`
- `Concatenable`
- `Countable`
- `Equatable`
- `Orderable`

The concrete coverage is intentionally uneven today. For example, `Orderable` ships instances only for `Int`, `Float`, `Byte`, `Word` and `Text`; instances for user types come from user code.

### Immutable Bindings

`def` binds a name once; neither `=` nor a compound assignment can rebind it.
A computed value is bound to a new name with `def`. State that has to change
in place lives in a [`Ref<T>`](#references), created with `ref`, read with `*` and
written with `:=`. A bare reference name shares the box; dereferencing it
produces the value currently inside it.

## Pattern Matching

Pattern matching is expression-oriented:

```marmot
def Unwrap = fn(option: Option<Int>) -> Int =>
    match option with
        case Option::Some(value) => value
        case Option::None() => 0;
```

### Exhaustiveness Rules

`match` expressions are checked for exhaustiveness at every depth: every value
of the scrutinee's type must reach an unguarded case. A union covers its variants
and their fields, a `Bool` covers `true` and `false`, a tuple or record covers each
combination of its fields, and `Unit` covers `()`. An `Int` is covered by the
literals and range patterns such as `0..10` that take it, so ranges that
together take every `Int` need no catch-all; a missing `Int` is named by the value
nearest zero in the gap. For `Float`, `Text`, `Byte` and `Word`, exhaustiveness
requires a case that
takes any value there. So do arrays, unless a case uses `..`, as below.

A case with a guard covers nothing, since its guard may fail. A non-exhaustive
match is an error that names values no case matches:

```text
Match expression type error: non-exhaustive match: no case matches Result::Ok(Option::None())
```

An array pattern with `..` can match any array long enough to fit the other
elements in the pattern. Those elements still have to match. For example,
`case []` covers the empty array and `case [first, ..rest]` covers the rest.

A catch-all arm is an unguarded `case` whose pattern always matches: `case _`, a
bare binding such as `case n`, or a tuple of those. There is no `default` keyword.

Every case must also be able to run: a case whose values the unguarded cases
above it already match is an error, and so is an alternative of `|` that the
cases and alternatives before it cover. `case 256` then `case 0x100` is one, as is
`case _` after cases that already cover every value.

### Pipe Into Match

The pipe operator can feed directly into a `match`:

```marmot
def pipeline_result =
    5
    |> fn(x) => { x + 1 }
    |> Transform
    |> match with
        case Result::Ok(v) => v
        case Result::Err(_) => 0;
```

This is equivalent to matching on the result of the previous pipeline stage.
In pattern position, `_` is a wildcard that ignores the matched value and does not bind a local. Outside patterns, `_` remains a valid identifier.

## Type Inference

Marmot uses unification with an occurs check to solve inference variables.

### Core Inference Flow

1. Fresh type variables are created for unknown types.
2. Constraints are collected while traversing the AST.
3. Unification solves those constraints.
4. Substitutions are applied to produce the final types.

### Bidirectional Inference

Expected-type context is used in two especially common places:

- **Constructors** infer omitted generic arguments from arguments and surrounding type context
- **Lambdas** may omit parameter annotations and return types when a function type is already expected

Examples:

```marmot
def doubler : fn(Int) -> Int = fn(x) => { x * 2 };
def mapped = OptionMap(Option::Some(1), fn(x) => { x + 1 });
```

Lambdas still need a surrounding function type when annotations are omitted:

```marmot-test name=type-system/lambda_missing_context kind=failure path=.doc_examples/type_system/lambda_missing_context.mmt module=TypeSystemLambdaMissingContext
def identity = fn(x) => { x };  // error: no expected function type
```

The fully explicit lambda syntax remains valid:

```marmot
fn(x: Int) -> Int => { x + 1 }
```

## Special Types

### Unit

`Unit` has exactly one value, `()`. It lets an expression or function finish
without carrying a useful result. Printing and reference writes return
`Unit`; a function declared to return it may use either as its final expression.

```marmot-test name=type-system/unit_reference_write path=.doc_examples/type_system/unit_reference_write.mmt module=TypeSystemUnitReferenceWrite
def Reset = fn(counter: Ref<Int>) -> Unit => counter := 0;
def counter = ref 3;
def completed : Unit = Reset(counter);
```

Because `Unit` is an ordinary type, generic APIs need no separate success form
for operations without a payload. For example, `IO::TryWriteFile` returns
`Result<Unit, IOError>`: success carries `()`, and failure carries an error.
Returning `Unit` does not imply that a function is pure or that it must finish.

### Never

`Never` has no values. It is the bottom type for computations that cannot
return normally, such as a panic or a function that recurses forever.
`Panic::Panic` returns `Never`, so a panic branch can fit the result type of
another branch without producing a dummy value:

```marmot-test name=type-system/never_branch path=.doc_examples/type_system/never_branch.mmt module=TypeSystemNeverBranch
import { "../../projects/marmotstdlib/MarmotPrelude/Prelude/Panic.mmt" }

def RequirePositive = fn(n: Int) -> Int =>
    if n > 0 then n
    else Panic::Panic("Expected a positive number");

def value : Int = RequirePositive(3);
```

The type checker accepts the `Never` branch alongside the `Int` branch. The
reverse does not hold: where `Never` itself is expected, as in the body of a
function declared `-> Never`, only an expression of type `Never` fits. The
compiler also uses `Never` to mark control flow after such a call as unreachable.
Replacing its return type with `Unit` would lose that information and make
these branch result types disagree.

### Undecided Type

During parsing, omitted annotations can temporarily be represented as undecided type slots. These become inference variables during type checking.

## Implementation Architecture

The type system is implemented primarily in `projects/marmotc/src/Compiler/TypeChecker/` and the type representation in `projects/marmotc/src/Compiler/AbstractSyntaxTree/Type.h`.

### Type Representation

Types include:

- Primitive types
- Type variables
- Generic parameters
- Arrays, references, tuples, functions, structs, and unions
- Type class constraints and associated type projections

### Type Checker Responsibilities

The type checker handles:

- Scoped type environments
- Unification and substitution
- Freshening of polymorphic types
- Constraint solving for type classes
- Associated type resolution
- Exhaustiveness checking for `match`

### Key Runtime-Independent Registries

- **Type environment stack** for scoped bindings
- **Type substitution map** for inference variables
- **Class registry** for type class declarations and associated types
- **Instance registry** for concrete instance resolution
- **Active constraint set** for function and type-definition constraints

## Limitations

Current limitations include:

1. **No higher-kinded types**: abstractions like `Functor` and `Monad` cannot be expressed directly.
2. **Nominal ADTs**: structs and unions compare by name rather than by structure.
3. **No GADTs or general type-level functions**: associated types are supported, but richer type-level computation is not.
4. **No existential types**: type parameters cannot be hidden inside values.
5. **No rank-N polymorphism**: polymorphic types cannot appear in arbitrary positions.
6. **Focused deriving support**: only the currently supported targets and shapes are derivable.

## Future Considerations

Likely extensions include:

- Richer unification error messages
- Broader deriving support
- Default method implementations for type classes
