# Marmot Prelude

`MarmotPrelude/` is the standard-library layer that ships with the repository. It wraps the runtime FFI surface in Marmot-facing modules and collects the core algebraic data types, collections, and built-in helpers that ordinary programs use.

The examples below assume a program that lives next to `MarmotPrelude/` in the repo or in a project with the same layout. If you expose top-level prelude modules on `MARMOT_PATH`, imports such as `IO`, `System`, `DateTime`, `TextUtil`, and `ArrayUtil` can also use search-path imports.

Complete `.mmt` source files still need an explicit `module` declaration even when a focused snippet below omits it.

The documented examples in this file are mirrored by `test/prelude/success/documentation_examples.mmt`.

## Module Map

- `Prelude/Option.mmt`, `Prelude/Result.mmt`, and `Prelude/List.mmt` provide the core ADTs and helper functions used throughout the rest of the prelude.
- `Collections/Map.mmt` and `Collections/Set.mmt` provide persistent hash-based collections: `Map::With` adds a key or replaces its value and returns a new map, `Map::Without` removes one, and `Set::SetWith` of a value already present changes nothing.
- `IO.mmt`, `System.mmt`, and `DateTime.mmt` are the effectful modules. Their public surface favors `Option` and `Result` wrappers rather than sentinel return values.
- `TextUtil.mmt`, `ArrayUtil.mmt`, and `Math.mmt` provide the common text, array, and numeric helpers that sit above the raw runtime builtins.
- `Concatenable.mmt`, `Convertable.mmt`, `Countable.mmt`, `Equatable.mmt`, `Hashable.mmt`, `Indexable.mmt`, `Iterable.mmt`, `Orderable.mmt`, and `Transferable.mmt` expose the helper and typeclass surface used by operators, collections, and concurrency.
- `Iter.mmt` provides lazy sequence pipelines over any `Iterable`. See [Sequence Pipelines](#sequence-pipelines).
- `Prelude/Panic.mmt` contains the simple panic helper used by many tests and examples.
- `Cell.mmt` makes `Cell<T>` and the compiler-provided `Cell::New`, `Cell::Get` and `Cell::Set` available; the module itself declares nothing. See [Cells](type-system.md#cells).
- `Concurrency.mmt` declares `WorkerError` (`Cancelled | Failed(Text)`), the error half of the `Result<T, WorkerError>` that `Concurrency::Join` evaluates to, `JoinedOrPanic` for code that treats a worker failure as fatal, and `ParallelMap`, which is written in Marmot rather than provided by the compiler. The compiler requires the `WorkerError` declaration to have exactly that shape.

## Helper and Typeclass Modules

The prelude is not only collections and IO wrappers. It also ships the public helper/typeclass modules that the compiler and standard data structures lean on:

- `Concatenable` backs `++` and currently ships concrete instances for `Text` and `Array<T>`.
- `Convertable` exposes the generic conversion surface used by `as` in constrained code and ships the current primitive conversion instances.
- `Countable` exposes the generic counting surface used by `#`. The prelude currently ships a `Text` instance; arrays and several standard collections also have direct lowering paths in the compiler/runtime.
- `Equatable` and `Hashable` provide the comparison and hashing surface used by derived code and collections.
- `Indexable` backs `x[i]`. It takes two type parameters, `Indexable<C, I>`, so the index type is not fixed to `Int`, and exposes an `Element` associated type. The prelude ships an `Array<T>` instance; arrays also keep a direct lowering path in the compiler, which the instance body itself relies on. `Text` has no instance yet - its element type follows from the planned newtype over `Array<Byte>`.
- `Iterable` provides the `Item` associated type and `Next` method used by `for` loops and iterable-based comprehensions.
- `Orderable` defines the ordering interface used by comparison operators for user-defined types. The module exports the class surface; concrete instances are typically user-defined.
- `Transferable` is the marker typeclass for values that can cross worker boundaries in the concurrency system. Built-in instances cover all primitive types, `Array<T>`, `Channel<T>`, and function types — a function crosses as its procedure index plus a copy of its captured cells, which is what lets `Concurrency::Spawn` take a function value and `ParallelMap` be written in Marmot. A closure's captures are not part of its type, so they are not checked: a closure that captured a `Worker<T>` crosses with a handle that means nothing on the other side. User-defined structs and unions can `deriving (Transferable)`. Transferability is enforced at compile time by `Concurrency::Spawn`, `Concurrency::Join`, `Concurrency::MakeChannel`, `->`, and `<-`. `Concurrency::Close`, `Concurrency::IsDone` and `Concurrency::Cancel` complete that surface.
- `Prelude/Panic` provides `Panic::Panic`, which is used heavily by the regression tests and small examples.

## Sequence Pipelines

`Iter.mmt` turns a sequence into a pipeline. Every stage takes its source as its
first argument, so stages compose with the pipe operator:

```marmot
def evens = values
	|> Iter::OfArray
	|> Iter::Filter(IsEven)
	|> Iter::Map(Double)
	|> Iter::ToArray;
```

- Sources: `OfArray(values)`, or any value with an `Iterable` instance, such as
  `List::ListIterNew(list)`, a `MapIter` or a `SetIter`.
- Stages: `Map(source, f)`, `Filter(source, keep)`, `Take(source, count)`. Each is
  a value with its own `Iterable` instance, so `for` loops and comprehensions
  consume one directly.
- Consumers: `ToArray(source)`, `Fold(source, initial, step)`, `Count(source)`.

Stages are lazy. Nothing runs until a consumer asks for an element, and a `Take`
in front of a `Map` means the mapped function runs only for the elements taken.

A stage's element type is tied to its source by an equality constraint
(`Iterable::Item<S> ~ A`). That constraint both solves — it is what tells
`Iter::ToArray` what it is collecting — and checks: a `Map` whose function takes
`Text` over a source of `Int` is a compile error.

## Result Naming

`Prelude/Result.mmt` uses `Result::Ok` and `Result::Err` as the only public constructor spellings.

Migration note:

- Replace `Result::OK` with `Result::Ok`.
- Replace `Result::Error` with `Result::Err`.
- The legacy spellings are no longer part of the public prelude API.

## Typed IO and System APIs

`IO` models file-system failures as `Result<_, IOError>`. `System` uses `Option<Text>` for environment lookup, `Result<_, SystemError>` for fallible process and directory operations, and a `Platform` union for platform detection.

```marmot-test name=prelude/typed_io_system path=.doc_example_prelude_typed_io_system.mmt module=PreludeTypedIOSystem
import
{
    "./MarmotPrelude/IO.mmt",
    "./MarmotPrelude/System.mmt",
    "./MarmotPrelude/Prelude/Option.mmt",
    "./MarmotPrelude/Prelude/Result.mmt"
}
use Option.{Option}
use Result.{Result}
use System.{Platform}

def config_text =
    match IO::TryReadFile("app.conf") with
        case Result::Ok(contents) => contents
        case Result::Err(_) => "release=debug"
    ;

def cache_dir =
    match System::TryGetEnv("MARMOT_CACHE_DIR") with
        case Option::Some(path) => path
        case Option::None() => ".marmot-cache"
    ;

def platform_name =
    match System::CurrentPlatform() with
        case Platform::Windows() => "windows"
        case Platform::MacOS() => "macos"
        case Platform::Linux() => "linux"
        case Platform::WebAssembly() => "webassembly"
        case Platform::Unknown(name) => name
    ;
```

Public IO entry points:

- `TryReadFile`, `TryWriteFile`, `TryAppendToFile`
- `TryReadBinaryFile`, `TryWriteBinaryFile`
- `TryDeleteFile`, `TryRenameFile`, `TryGetFileSize`
- Console helpers such as `PrintLine`, `PrintErrorLine`, `ReadInput`, and `ReadLine`

Public system entry points:

- `TryGetEnv`, `SetEnv`
- `CurrentDirectory`, `TrySetCurrentDirectory`
- `Run`, `CurrentPlatform`, `CurrentProcessId`
- `Exit`, `Sleep`

The raw foreign declarations remain module-internal implementation details and are not the public API to build against.

## Date and Time

`DateTime` exposes explicit local and UTC structs instead of leaving callers to reconstruct ambient parts manually.

```marmot-test name=prelude/datetime path=.doc_example_prelude_datetime.mmt module=PreludeDateTime
import { "./MarmotPrelude/DateTime.mmt" }

def local = DateTime::LocalNow();
def utc = DateTime::UtcNow();
def timestamp = DateTime::NowUnixMillis();
def formatted = DateTime::FormatLocal("%Y-%m-%d %H:%M:%S");
def offset = local.timezone_offset_minutes;
```

Public date/time entry points:

- `LocalNow`, `UtcNow`
- `NowUnixMillis`, `TimezoneOffsetMinutes`
- `FormatLocal`

## Text and Array Helpers

`TextUtil` groups the common string-style operations already available in the runtime, while `ArrayUtil` collects the ordinary array helpers. Arrays are immutable: `WithAppended`, `WithInserted`, `WithReplaced`, and `WithRemoved` return a new array with the requested change rather than mutating the original, and `Slice` and `Reverse` likewise return new arrays. Build an array up front with a comprehension or `List`, or grow one incrementally with `WithAppended` when the number of elements is small - calling it in a loop is quadratic.

```marmot-test name=prelude/text_array_helpers path=.doc_example_prelude_text_array_helpers.mmt module=PreludeTextArrayHelpers
import
{
    "./MarmotPrelude/TextUtil.mmt",
    "./MarmotPrelude/ArrayUtil.mmt"
}

def words = TextUtil::Split(TextUtil::Trim("  alpha beta  "), " ");
def headline = TextUtil::Replace("marmot docs", "docs", "prelude");

def numbers = [1, 2, 3];
def grown = ArrayUtil::WithAppended(numbers, 4);
def replaced = ArrayUtil::WithReplaced(grown, 0, 9);

def reversed = ArrayUtil::Reverse(replaced);
def window = ArrayUtil::Slice(replaced, 1, 3);
```

Public text helpers:

- `Length`, `Contains`, `Substring`
- `Split`, `Replace`, `Trim`, `Reverse`
- `FromCodePoint(Int) -> Option<Text>`: the text of one Unicode scalar value; `None` for a surrogate, anything past U+10FFFF, and U+0000, which text cannot hold
- `CodePointAt(Text, Int) -> Option<Int>`: the code point at a code-point index, `None` past the end
- `ParseInt(Text) -> Option<Int>`, `ParseFloat(Text) -> Option<Float>`: the number the whole text spells, or `None`. `text as Int` and `text as Float` are the same conversions, but stop the program with `InvalidConversion` when the text is not a number

Public array helpers:

- `Length`, `Contains`
- `Slice`, `Reverse`, `Copy`, `Concat`
- `WithAppended`, `WithReplaced`, `WithInserted`, `WithRemoved`

## Core ADTs and Collections

The core prelude modules stay intentionally small:

- `Prelude/Option.mmt`: `Option`, `OptionMap`, `OptionBind`, `OptionIsSome`, `OptionIsNone`, `OptionOrElse`, `OptionUnwrap`, `OptionUnwrapOrElse`, `OptionUnwrapOrPanic`
- `Prelude/Result.mmt`: `Result`, `ResultMap`, `ResultBind`, `ResultMapError`, `ResultIsOk`, `ResultIsError`, `ResultUnwrap`, `ResultUnwrapOrElse`, `ResultUnwrapOrPanic`
- `Prelude/List.mmt`: `List`, `ListMap`, `ListLength`, `ListAppend`, `ListFilter`, `ListReverse`, `ListFold`, `ListFromArray`, `ListToArray`
- `Collections/Map.mmt`: `Empty`, `Count`, `IsEmpty`, `Get`, `GetOr`, `ContainsKey`, `With`, `Without`, `Entries`, `Keys`, `Values`, `IterNew`
- `Collections/Set.mmt`: `SetEmpty`, `SetCount`, `SetIsEmpty`, `SetWith`, `SetWithout`, `SetHas`, `SetToArray`, `SetIterNew`

## Math

`Math.mmt` exports the numeric constants and wrappers used by ordinary Marmot code:

- constants such as `Pi`, `E`, and `Tau`
- transcendental and trigonometric helpers such as `SquareRoot`, `Pow`, `Sin`, and `Atan2`
- bounds helpers such as `Min`, `Max`, `Clamp`, `Sign`, and `Hypot`
- randomness helpers such as `Random`, `RandomInt`, and `RandomFloat`
- unit-conversion helpers such as `ToRadians` and `ToDegrees`
