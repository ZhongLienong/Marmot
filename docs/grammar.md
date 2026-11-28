# Grammar

This document describes the grammar accepted by the current compiler. It is a
snapshot of the language as it is today, not a promise that accepted programs
will keep compiling unchanged in future releases.

Forms removed from earlier versions (`return`, `loop`, `break`, `continue`,
binding assignment and its compound forms, `new`, `defun`, `struct`, `union`,
`default`, and the `spawn`, `join` and `channel` keywords) are not accepted by
the current compiler. Their spellings currently behave as ordinary identifiers
where the grammar permits identifiers; future language changes may reuse them.

The notation: `X?` optional, `X*` zero or more, `X+` one or more,
`X (',' X)*` a separated list, `'x'` a literal, `UPPERCASE` a token class.

## Lexical

```
IDENTIFIER   letter or '_', then letters, digits or '_'
INTEGER      decimal digits, or 0x / 0X hex, or 0b / 0B binary
FLOAT        digits '.' digits
TEXT         '"' characters '"', with \n \r \t \b \f \0 \\ \" \xNN \u{...} escapes
COMMENT      '//' to end of line, or '/*' to the first '*/' (no nesting)
```

A file may start with a UTF-8 byte order mark, which is not part of the program.
Whitespace separates tokens outside text literals. `\xNN` inserts one byte
from two hexadecimal digits. `\u{...}` encodes a Unicode scalar value in
UTF-8. Unknown and malformed escapes are lexer errors.

**Keywords.** `alias` `as` `case` `class` `def` `deriving` `else` `export`
`false` `fn` `for` `foreign` `if` `import` `in` `instance` `match` `module`
`private` `public` `then` `true` `type` `use` `where` `with`

`ref` is a contextual prefix form that creates a reference. A declared or
`use`d name spelled `ref` still resolves as an ordinary name.

**Type names.** `Array` `Bool` `Byte` `Channel` `Float` `Int` `Never` `Range`
`Text` `Unit` `Word` `Worker`

**Symbols.** `-> => ( ) { } [ ] , . .. ; + ++ - << >> % * / | || |> ^ & && !
!= = == > >= < <= : :: := ~ #`

## Program

```
program      = module item* ;
module       = 'module' IDENTIFIER ('.' IDENTIFIER)* ;
item         = import | use | export | declaration | expression ';' ;
import       = 'import' '{' importPath (',' importPath)* ','? '}' ;
importPath   = TEXT | systemName ;         // "./path.mmt" or <Name>
systemName   = '<' IDENTIFIER ('.' IDENTIFIER)* '>' ;
use          = 'use' moduleName '.' '{' IDENTIFIER (',' IDENTIFIER)* ','? '}' ;
export       = ('public' | 'private') 'export' '{' IDENTIFIER (',' IDENTIFIER)* ','? '}' ;
moduleName   = IDENTIFIER ('.' IDENTIFIER)* ;
```

A program is its top-level items, run in order; there is no entry function, and
a definition named `main` is an ordinary one.

Write `<Name>` to find a module on the search path. To import a
file, put its path in quotes. After importing the module, refer to an exported
name as `Module::name`. Use `use` to bring the bare name into scope.

## Declarations

```
declaration  = define | typeDecl | alias | class | instance | foreign ;

define       = 'def' binder (':' type)? '=' expression ';'
             | 'def' '(' binder (',' binder)+ ')' '=' expression ';' ;
binder       = IDENTIFIER | '_' ;

typeDecl     = 'type' IDENTIFIER typeParams? constraints? '='
               (record deriving? | variants deriving? | type) ';' ;
record       = '{' field (',' field)* ','? '}' ;
field        = IDENTIFIER ':' type ;
variants     = variant ('|' variant)+ | '|' variant ('|' variant)* ;
variant      = IDENTIFIER ('(' type (',' type)* ')')? ;
deriving     = 'deriving' '(' IDENTIFIER (',' IDENTIFIER)* ')' ;
             // a record derives Equatable, Hashable, Orderable, Convertible
             // or Transferable;
             // a union those, plus Map, Bind and Unwrap

alias        = 'alias' IDENTIFIER typeParams? '=' type ';' ;

class        = 'class' IDENTIFIER typeParams '{' classMember* '}' ';' ;
classMember  = IDENTIFIER ':' type ';'                     // a method's signature
             | 'type' IDENTIFIER ';' ;                     // an associated type

instance     = 'instance' IDENTIFIER '<' type (',' type)* '>' constraints?
               '{' (define | 'type' IDENTIFIER '=' type ';')* '}' ';' ;

foreign      = 'foreign' TEXT IDENTIFIER ':' type ('from' TEXT)? ';'
             | 'foreign' TEXT '{' (TEXT IDENTIFIER ':' type ';')+ '}' ;

typeParams   = '<' IDENTIFIER (',' IDENTIFIER)* '>' ;
constraints  = 'where' constraint (',' constraint)* ;
constraint   = IDENTIFIER '<' type (',' type)* '>' | projection '~' type ;
projection   = IDENTIFIER '::' IDENTIFIER '<' type (',' type)* '>' ;
```

A type with a representation on the right of `=` is a newtype, such as
`type Meters = Int;`. A union with just one variant uses a leading `|`.
Constraints on a type declaration require type parameters.

A module's top-level statements run in order, and a function's body runs when
it is called. So a function may name a top-level definition that comes after
it -- which is how two functions call each other -- and a statement may use a
definition only once it, and everything the functions it reaches go on to
read, has run. A definition named before the checker reaches it needs its type
written down: a function's parameter and return types, a value's annotation.
Inside its own initializer a `def` may name itself only from within a function.

Types, classes and instances declare; nothing runs. A module's types may be
named, constructed and matched anywhere in it, above their declarations too,
and an instance serves the whole module -- which is how two types hold each
other. Two generic types that name each other do so with the same parameter
names, `Tree<T>` holding `Forest<T>` and `Forest<T>` holding `Tree<T>`: a type
cannot be built from one that holds it back with other arguments. A newtype
cannot contain itself.

In a `foreign` declaration the first TEXT is the symbol the library exports;
`from` names the library, and the block form names it once for several
functions. Without `from`, the symbol must be a runtime builtin.

## Types

```
type         = functionType | atomType ;
functionType = 'fn' '(' (type (',' type)*)? ')' '->' type ;
atomType     = 'Int' | 'Float' | 'Bool' | 'Byte' | 'Word' | 'Text' | 'Unit'
             | 'Never'
             | 'Array' '<' type '>'
             | 'Range' '<' type '>'
             | 'Channel' '<' type '>'
             | 'Worker' '<' type '>'
             | 'Ref' '<' type '>'                         // a reference, unless Ref is a declared type
             | qualifiedName ('<' type (',' type)* '>')? // a declared type or projection
             | '(' type (',' type)+ ')' ;                 // a tuple
qualifiedName = IDENTIFIER | moduleName '::' IDENTIFIER ('::' IDENTIFIER)? ;
```

## Expressions

Control-flow constructs are expressions. A block's value is its final
expression, or `Unit` when it ends with statements. A function returns the
value of its body.

Precedence, loosest first. Each level is left-associative unless noted.
Different levels may be mixed without parentheses: `a + b * c` means
`a + (b * c)`. Overloading changes the operation, not its precedence.

| Level | Form |
|---|---|
| 0 | `e := e` (reference write; right-associative) |
| 1 | `e as Type` |
| 2 | `e \|\| e` |
| 3 | `e && e` |
| 4 | `e \|> e` (pipe) |
| 5 | `e \| e` |
| 6 | `e ^ e` |
| 7 | `e & e` |
| 8 | `e == e`, `e != e` |
| 9 | `e < e`, `e <= e`, `e > e`, `e >= e` |
| 10 | `start..end`, `start..step..end` |
| 11 | `e << e`, `e >> e` |
| 12 | `e + e`, `e - e`, `e ++ e` (concatenation) |
| 13 | `e * e`, `e / e`, `e % e` |
| 14 | `!e`, `~e`, `#e` (length), `+e`, `-e`, `ref e`, `*e` (dereference) |
| 15 | `f(args)`, `e[index]`, `e.field`, `Module::name` |
| 16 | primary |

```
primary      = INTEGER | FLOAT | TEXT | 'true' | 'false' | '(' ')'
             | qualifiedName
             | '(' expression ')'
             | '(' expression (',' expression)+ ')'        // a tuple
             | array | comprehension | block | function
             | if | match | for | recordUpdate | construct ;

array        = '[' (expression (',' expression)* ','?)? ']' ;
comprehension= '[' expression 'for' binder 'in' expression ']' ;
block        = '{' item* expression? '}' ;
function     = 'fn' typeParams? '(' (param (',' param)*)? ')'
               ('->' type)? constraints? '=>' expression ;
param        = binder (':' type)? ;
if           = 'if' expression 'then' expression 'else' expression ;
for          = 'for' binder 'in' expression block ;
recordUpdate = '{' expression 'with' fieldSet (',' fieldSet)* '}' ;
fieldSet     = IDENTIFIER ('.' IDENTIFIER)* '=' expression ;
construct    = (IDENTIFIER ('.' IDENTIFIER)* '::')? IDENTIFIER       // a record type
               '{' fieldValue (',' fieldValue)* ','? '}' ;
fieldValue   = IDENTIFIER '=' expression ;
match        = 'match' expression 'with' case+ ;
case         = 'case' pattern ('if' expression)? '=>' expression ;
```

A record is built by calling its type's name, `Point(1, 2)`, or by naming its
fields, in any order and each exactly once, `Point { y = 2, x = 1 }`. The fields
are evaluated in the order the type declares them, as a record update's are. A
brace after a name is a construction only when the name is a record type and
`field =` follows it, so a block after a name is still a block. A union's
variants hold their fields by position, so they are built only by calling.
`for` evaluates its body for each element and returns `Unit`.

`ref e` creates a `Ref<T>` holding `e`, `*e` reads a reference's contents,
and `cell := value` replaces those contents and evaluates to `Unit`. These
forms and `Ref<T>` need no import. Bindings remain immutable. Group a
compound initializer, as in `ref (a + b)`; a write's right operand is a full
expression, as in `counter := (*counter) + 1`.

A `function` that is the right operand of `|>` is one stage of the pipeline: its
body is parsed as a pipe operand (level 5 and tighter), so it ends before the
next `|>`. `x |> fn(a) => f(a) |> g` is `g(f(x))`, and a later stage cannot name
`a`. Any other function's body is a whole `expression`.

## Patterns

```
pattern      = alternatives ('as' IDENTIFIER)* ;          // names the whole value
alternatives = primary ('|' primary)* ;                   // any one of them
primary      = '_' | '(' ')'                              // wildcard or Unit
             | number | TEXT | 'true' | 'false'
             | integer '..' integer ('..' integer)?       // the Ints a range visits
             | IDENTIFIER                                 // binds the value
             | qualifiedName '(' (pattern (',' pattern)*)? ')' // a variant or record, by position
             | qualifiedName '{' fieldPatterns '}'        // a record, by field name
             | '(' pattern ')'                            // grouping
             | '(' pattern (',' pattern)+ ')'             // a tuple
             | '[' (element (',' element)*)? ']' ;        // an array
element      = pattern | '..' IDENTIFIER? ;               // at most one '..'
fieldPatterns= fieldPattern (',' fieldPattern)* (',' '..')? | '..' ;
fieldPattern = IDENTIFIER '=' pattern ;
number       = ('-' | '+')? (INTEGER | FLOAT) ;
integer      = ('-' | '+')? INTEGER ;
```

A range pattern, `start..end` or `start..step..end`, matches the Ints the range
with those parts visits, the end excluded: `0..10` is 0 to 9, `90..1..101` is 90
to 100, `10..-1..0` is 10 down to 1. Its parts are Int literals and its step is 1
or -1, so a case always covers an interval; test other steps with a guard. A
range that visits nothing is an error.

An array pattern without `..` matches arrays of exactly its length. With one, it
matches every array at least as long as its other elements: those before `..`
are counted from the start, those after it from the end, and `..name` binds what
is between them, possibly nothing, as an array. `[head, ..tail]`,
`[..init, last]`, `[first, .., last]`.

A record pattern names the fields it matches, in any order, each as
`field = pattern`: `case Point { x = 0, y = y0 } => ...`. Every field is matched
unless the pattern ends in `..`, which leaves the ones it does not name out:
`case Point { x = 0, .. }`. Leaving a field out without `..`, naming one twice,
and a `..` that leaves nothing out are errors. A variant's fields have no
names, so only `Name(...)` matches them. Constructor patterns require parentheses
even with no fields: `case List::Nil() => ...`.

`pattern as name` matches what `pattern` matches and binds the whole value to
`name` as well: `case Option::Some(inner) as whole => ...`.

`first | second` matches what either alternative matches, trying them in order:
`case Shape::Dot() | Shape::Blob() => 0`. Every alternative binds the same names,
with the same types, and the case reads them from whichever one matched:
`case Shape::Line(n) | Shape::Box(n, _) => n`. `as` takes in every alternative
before it, so `A() | B() as x` names either; parenthesize one alternative to name
only it. A guard is tested once, with what the first matching alternative bound,
and when it fails the match goes on to the next case.

A `case` may carry a guard: `case n if n > 10 => ...`. The catch-all arm is
`case _ =>`; `_` is a pattern, so it nests inside the others.

A match's cases run to the first token that is not `case`, so a match in an
arm without braces takes the arm's following cases for its own. A case that
the cases above it already cover can never run, and is an error, as is an
alternative of `|` that the cases and alternatives before it cover. Coverage
counts the values patterns match, not how they are written, and a case with a
guard covers nothing.
