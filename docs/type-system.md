# Marmot Type System

Marmot uses a Hindley-Milner based, nominal type system with bidirectional inference. The implementation follows Algorithm W and extends it with algebraic data types, type classes, associated types, deriving, type-definition constraints, and exhaustive pattern matching.

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
- **Compile-time exhaustiveness checking** for `match` on unions and `Bool`

## Primitive Types

| Type | Description |
|------|-------------|
| `Int` | 64-bit signed integer |
| `Float` | 64-bit IEEE 754 double |
| `Byte` | 8-bit unsigned integer |
| `Word` | 64-bit unsigned integer |
| `Bool` | `true` or `false`, a built-in rather than a library union — see the design spec's section 8 for the measurement behind that |
| `Text` | UTF-8 string |
| `Unit` | Unit value type |

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

A `Float` converts to an integer type with `as` by truncating toward zero:
`3.99 as Int` is `3` and `-3.99 as Int` is `-3`. A `Float` that `Int` cannot
hold, or a NaN, becomes the minimum `Int`, `-9223372036854775808`; the program
does not stop. That is what the x86-64 conversion instruction produces, so the
conversion costs nothing there, and other targets check the range to give the
same result. `as Byte` converts to `Int` first and then wraps as `Int as Byte`
does, so `-1.0 as Byte` is `255` and `300.0 as Byte` is `44`. `as Word` is exact
for every value in `Word`'s range and otherwise goes through `Int` the same way,
so `-1.0 as Word` is `18446744073709551615`.

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

Both are internally represented as `Int` handles but carry compile-time type parameters. `Worker<T>` is produced by `Concurrency::Spawn` and consumed by `Concurrency::Join`. `Channel<T>` is produced by `Concurrency::MakeChannel(capacity)`, which takes `T` from context, and used with `->` (send) and `<-` (receive).

### Cells

`Cell<T>` is a box whose contents can change. It is available once
`MarmotPrelude/Cell.mmt` is imported; `Cell` is not a keyword, so a program's own
type named `Cell` takes precedence over it.

| Operation | Type | Meaning |
|---|---|---|
| `Cell::New(value)` | `fn<T>(T) -> Cell<T>` | A new cell holding `value`. |
| `Cell::Get(cell)` | `fn<T>(Cell<T>) -> T` | The value currently stored. |
| `Cell::Set(cell, value)` | `fn<T>(Cell<T>, T) -> T` | Stores `value` and evaluates to it. |

A cell is a reference: passing it, capturing it, or storing it in an array,
tuple or record shares the one cell. Bindings stay immutable, so the only state
that changes is a cell's contents, and a program's mutable state can be found by
looking for `Cell<...>` in its types.

Deliberately not provided: `Equatable`, `Hashable` or `Orderable` instances for
cells (compare contents with `Cell::Get(a) == Cell::Get(b)`), operators such as
`!c` or `c := v`, and any tracking of mutation in function types. A cell that
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

Operators that do not bind equally cannot be mixed in one expression without
parentheses. `a + b * c` is an error; write `a + (b * c)`. A chain of operators
that bind equally needs none, so `a + b - c` and `x && y && z` are unchanged.

The classes are `*` `/` `%`; `+` `-`; `++`; `<<` `>>`; `<` `<=` `>` `>=`;
`==` `!=`; `&`; `^`; `|`; `&&`; `||`. Concatenation is its own class, so
`"n = " ++ (count as Text)` stays explicit about what is concatenated and what is
arithmetic.

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

`Transferable` generates field-by-field serialization for structs and tag+payload serialization for unions. All fields/variants must themselves satisfy `Transferable`. Types that cannot be transferable (ranges, `Worker<T>`) produce a compile-time constraint-failure error. Functions are transferable: one crosses as its procedure index plus a copy of its captured cells. What a closure captured is not part of its type, so those captures are not checked.

Current support is intentionally narrow:

- `Equatable`, `Hashable`, and `Transferable` are supported structural derives
- `Map`, `Bind`, and `Unwrap` are supported container derives
- Structural deriving is limited to non-generic, non-recursive structs and unions for `Equatable` and `Hashable`
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
    Next: fn(iter: Iter) -> Option<Item>;
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

    def Next = fn(counter: Counter) -> Option<Int> => {
        if counter.current >= counter.end
        then Option::None()
        else {
            def value = counter.current;
            counter.current = counter.current + 1;
            Option::Some(value)
        }
    };
};
```

Use projection syntax to refer to an associated type in other signatures:

```marmot
def NextValue = fn<Iter>(iter: Iter) -> Option<Iterable::Item<Iter>>
    where Iterable<Iter> => Iterable::Next(iter);
```

Associated types are especially useful when one type parameter logically determines another, while multi-parameter type classes remain available for other cases.

### Operator-Backed Type Classes

Several operators are wired into the type checker and code generator so they can dispatch through type classes when the operands are not handled entirely as builtins.

- `as` can use `Convertable<From, To>`
- `++` can use `Concatenable<T>`
- `#` can use `Countable<T>`
- `==` and `!=` can use `Equatable<T>`
- `<`, `<=`, `>`, and `>=` can use `Orderable<T>`

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

The prelude ships the instances the built-in types need: `Orderable` for `Int`, `Float`, `Byte`, `Word` and `Text`, `Equatable` and `Hashable` for the primitives, `Concatenable` and `Countable` for `Text` and arrays. Instances for other types come from user code or `deriving`.

### No Assignment Operators

There is no assignment, compound or otherwise (`=`, `+=`, `&=` and the rest were
removed in v2). A computed value is bound to a new name with `def`; state that
has to change in place lives in a [`Cell<T>`](#cells).

## Pattern Matching

Pattern matching is expression-oriented:

```marmot
def Unwrap = fn(option: Option<Int>) -> Int =>
    match option with
        case Option::Some(value) => value
        case Option::None() => 0;
```

### Exhaustiveness Rules

`match` expressions are checked for exhaustiveness:

- **Union scrutinees** must cover every variant unless a catch-all arm is present
- **`Bool` scrutinees** must cover both `true` and `false` unless a catch-all arm is present
- **Other scrutinee types** require a catch-all arm

A catch-all arm is an unguarded `case` whose pattern always matches: `case _`, a
bare binding such as `case n`, or a tuple of those. There is no `default` keyword.

Coverage is currently tracked at the top-level pattern. For unions, matching a variant counts as covering that variant even if nested sub-patterns are not themselves exhaustive.

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

Marmot uses Hindley-Milner inference with unification and an occurs check.

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

### Never

`Never` is the bottom type for computations that do not produce a normal value, such as non-returning control-flow paths.

### Undecided Type

During parsing, omitted annotations can temporarily be represented as undecided type slots. These become inference variables during type checking.

## Implementation Architecture

The type system is implemented primarily in `compiler/src/Compiler/TypeChecker/` and the shared type representation in `common/src/Common/`.

### Type Representation

Types include:

- Primitive types
- Type variables
- Generic parameters
- Arrays, tuples, functions, structs, and unions
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
6. **Top-level exhaustiveness only**: nested-pattern exhaustiveness is not fully checked yet.
7. **Focused deriving support**: only the currently supported targets and shapes are derivable.

## Future Considerations

Likely extensions include:

- Richer unification error messages
- Deeper nested-pattern exhaustiveness analysis
- Broader deriving support
- Default method implementations for type classes
