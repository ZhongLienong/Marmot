# Grammar

This is Marmot's grammar as of v1, and it is a contract: **nothing here is
removed after v1.** Later versions may add rules, and a program written against
this document keeps compiling.

Additions under consideration — an error-propagation operator, waiting on
several channels — would be new rules, not changes to these. The syntax that
earlier versions dropped (`return`, `loop`, `break`, `continue`, assignment and
its compound forms, `new`, `defun`, `struct`, `union`, `default`, the `spawn`,
`join` and `channel` keywords) is gone for good; the compiler carries no trace
of it, so those words are now ordinary identifiers.

The notation: `X?` optional, `X*` zero or more, `X+` one or more,
`X (',' X)*` a separated list, `'x'` a literal, `UPPERCASE` a token class.

## Lexical

```
IDENTIFIER   letter or '_', then letters, digits or '_'
INTEGER      decimal digits, or 0x / 0X hex, or 0b / 0B binary
FLOAT        digits '.' digits
TEXT         '"' characters '"', with \n \t \r \\ \" \0 escapes
COMMENT      '//' to end of line, or '/*' to the first '*/' (no nesting)
```

A file may start with a UTF-8 byte order mark, which is not part of the program.
Whitespace separates tokens and is otherwise insignificant.

**Keywords.** `alias` `as` `case` `class` `def` `deriving` `else` `export`
`false` `fn` `for` `foreign` `if` `import` `in` `instance` `match` `module`
`private` `public` `then` `true` `type` `use` `where` `with`

**Type names.** `Array` `Bool` `Byte` `Channel` `Float` `Int` `Never` `Range`
`Text` `Unit` `Word` `Worker`

**Symbols.** `-> <- => ( ) { } [ ] , . .. ; + ++ - << >> % * / | || |> ^ & && !
!= = == > >= < <= : :: ~ #`

## Program

```
program      = module? item* ;
module       = 'module' IDENTIFIER ('.' IDENTIFIER)* ;
item         = import | use | export | declaration | expression ';' ;
import       = 'import' '{' importPath (',' importPath)* ','? '}' ;
importPath   = TEXT | systemName ;         // "<Name>", <Name>, or "./path.mmt"
systemName   = '<' IDENTIFIER ('.' IDENTIFIER)* '>' ;
use          = 'use' IDENTIFIER ('.' IDENTIFIER)* '.' '{' IDENTIFIER (',' IDENTIFIER)* '}' ;
export       = ('public' | 'private') 'export' '{' IDENTIFIER (',' IDENTIFIER)* '}' ;
```

A program is its top-level items, run in order; there is no entry function, and
a definition named `main` is an ordinary one.

An import names either a module to find on the search paths, written
`"<Name>"`, or a file, written as a path. Importing a module lets you write
`Module::name`; `use` is what puts a name in scope on its own.

## Declarations

```
declaration  = define | typeDecl | alias | class | instance | foreign ;

define       = 'def' IDENTIFIER (':' type)? '=' expression ';' ;

typeDecl     = 'type' IDENTIFIER typeParams? '=' (record | variants) deriving? ';' ;
record       = '{' field (',' field)* ','? '}' ;
field        = IDENTIFIER ':' type ;
variants     = variant ('|' variant)* ;
variant      = IDENTIFIER ('(' type (',' type)* ')')? ;
deriving     = 'deriving' '(' IDENTIFIER (',' IDENTIFIER)* ')' ;
             // a record derives Equatable, Hashable or Transferable;
             // a union those, plus Map, Bind and Unwrap

alias        = 'alias' IDENTIFIER typeParams? '=' type ';' ;

class        = 'class' IDENTIFIER typeParams constraints? '{' classMember* '}' ';' ;
classMember  = IDENTIFIER ':' type ';'                     // a method's signature
             | 'type' IDENTIFIER ';' ;                     // an associated type

instance     = 'instance' IDENTIFIER '<' type (',' type)* '>' constraints?
               '{' (define | 'type' IDENTIFIER '=' type ';')* '}' ';' ;

foreign      = 'foreign' TEXT IDENTIFIER ':' type ('from' TEXT)? ';'
             | 'foreign' TEXT '{' (TEXT IDENTIFIER ':' type ';')+ '}' ;

typeParams   = '<' IDENTIFIER (',' IDENTIFIER)* '>' ;
constraints  = 'where' constraint (',' constraint)* ;
constraint   = IDENTIFIER '<' type (',' type)* '>' ;
```

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
             | IDENTIFIER ('<' type (',' type)* '>')?     // a declared type
             | '(' type (',' type)+ ')' ;                 // a tuple
```

## Expressions

Everything is an expression. A block's value is its last expression; a
function's value is its body.

Precedence, loosest first. Each level is left-associative unless noted.

| Level | Form |
|---|---|
| 1 | `e as Type` |
| 2 | `e \|\| e` |
| 3 | `e && e` |
| 4 | `ch -> e` (send on a channel) |
| 5 | `e \|> e` (pipe; `\|> match ...` is allowed) |
| 6 | `e \| e` |
| 7 | `e ^ e` |
| 8 | `e & e` |
| 9 | `e == e`, `e != e` |
| 10 | `e < e`, `e <= e`, `e > e`, `e >= e` |
| 11 | `start..end`, `start..step..end` |
| 12 | `e << e`, `e >> e` |
| 13 | `e + e`, `e - e`, `e ++ e` (concatenation) |
| 14 | `e * e`, `e / e`, `e % e` |
| 15 | `!e`, `~e`, `#e` (length), `-e`, `<- ch` (receive) |
| 16 | `f(args)`, `e[index]`, `e.field`, `Module::name` |
| 17 | primary |

```
primary      = INTEGER | FLOAT | TEXT | 'true' | 'false'
             | IDENTIFIER
             | IDENTIFIER ('.' IDENTIFIER)* '::' IDENTIFIER   // a module's name
             | '(' expression ')'
             | '(' expression (',' expression)+ ')'        // a tuple
             | array | comprehension | block | function
             | if | match | for | recordUpdate ;

array        = '[' (expression (',' expression)* ','?)? ']' ;
comprehension= '[' expression 'for' IDENTIFIER 'in' expression ']' ;
block        = '{' item* expression? '}' ;
function     = 'fn' typeParams? '(' (param (',' param)*)? ')'
               ('->' type)? constraints? '=>' expression ;
param        = IDENTIFIER (':' type)? ;
if           = 'if' expression 'then' expression 'else' expression ;
for          = 'for' IDENTIFIER 'in' expression block ;
recordUpdate = '{' expression 'with' fieldSet (',' fieldSet)* '}' ;
fieldSet     = IDENTIFIER '=' expression ;
match        = 'match' expression 'with' case+ ;
case         = 'case' pattern ('if' expression)? '=>' expression ;
```

A record is built by calling its type's name: `Point(1, 2)`. `for` evaluates
its body for each element and has no value of its own.

## Patterns

```
pattern      = '_'                                        // wildcard
             | INTEGER | FLOAT | TEXT | 'true' | 'false'
             | IDENTIFIER                                 // binds the value
             | IDENTIFIER '(' pattern (',' pattern)* ')'  // a variant
             | '(' pattern (',' pattern)+ ')'             // a tuple
             | '[' (pattern (',' pattern)*)? ']' ;        // an array
```

A `case` may carry a guard: `case n if n > 10 => ...`. The catch-all arm is
`case _ =>`; `_` is a pattern, so it nests inside the others.

A match's cases run to the first token that is not `case`, so a match in an
arm without braces takes the arm's following cases for its own. A case that
the cases above it already cover can never run, and is an error.
