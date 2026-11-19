# Formatting

Marmot ships a first-party formatter through:

```powershell
marmotc.exe fmt <file|dir>
```

`marmot fmt` delegates to the same formatter and uses the same style.

## Commands

Format a single file to stdout:

```powershell
marmotc.exe fmt src/Main.mmt
```

Rewrite files in place:

```powershell
marmotc.exe fmt src -w
```

Check whether formatting would change anything:

```powershell
marmotc.exe fmt test --check
```

Machine-readable summary:

```powershell
marmotc.exe fmt src --check --format json
```

## Canonical Style

The canonical Marmot style is opinionated and not configurable. The formatter is
the source of truth; the rules below document what `marmotc.exe fmt` produces.

### Indentation

- 4 spaces per level
- never tabs

### Brace placement

All control-flow and declaration constructs use K&R style: the opening brace
appears on the same line as the construct, and the closing brace is aligned
with the line that owns the brace.

| Construct  | Form                                                           |
|------------|----------------------------------------------------------------|
| `def` (function) | `def name = fn(args) -> Type => { ... };`                  |
| `def`      | `def name = expr;` or `def name = { ... };`                     |
| `if`       | `if cond then { ... } else { ... };`                            |
| `for`      | `for x in iter { ... };`                                        |
| `match`    | `match value with case <pattern> => expr case _ => expr;`      |
| `type` (record) | `type Name = { field: Type, ... };`                        |
| `type` (sum) | `type Name = Variant \| Variant(Type) \| ...;`                |
| `class`    | `class Name<T> { method: fn(value: T) -> Type; ... };`           |
| `instance` | `instance Name<Type> { def method = fn(value: Type) -> Type => ...; };` |

Module directives can use inline braces, as in `import { <IO> }`, or span
multiple lines. The formatter keeps the layout you chose.

### Line width

- target line width is **100 characters**
- calls that exceed the target, or have arguments that require multiple lines,
  use separate, aligned opening and closing parentheses; each argument is
  indented one level and starts on its own line
- small calls and short expression-bodied lambdas stay compact
- other expressions are not automatically wrapped; use intermediate `def`
  bindings or split list literals when that improves readability

The line width is a guideline, not a hard limit. CI does not fail on long lines.

For example, a call containing a block-bodied lambda is formatted as:

```marmot
def result = Apply
(
    fn(value: Int) -> Int => {
        def doubled = value * 2;
        doubled + 1
    },
    2
);
```

Function parameter lists and parenthesized expressions keep their existing layout.

### Trailing commas

The formatter keeps trailing commas even when it collapses an array literal
to one line. The grammar determines where you can add one.

### Import ordering and grouping

The formatter keeps imports in source order, even in a block that mixes
`<Module>` imports and quoted paths. It keeps separate import blocks and
preserves blank lines between directives.

### Match arm alignment

- each `case` arm, including a catch-all `case _`, appears on its own line
- arms inside a `match ... with` are indented one level beyond the surrounding
  block
- exactly one space appears between the pattern and `=>`, and one space between
  `=>` and the arm body
- a terminating `;` normally follows the last arm on the same line

```marmot
def name = match value with
    case (Some(x), 0) => x
    case (Some(x), _) => x + 1
    case _ => 0;
```

The formatter uses one space before `=>`, even if you aligned the arrows by
hand.

### Spacing

- one space after `,`
- one space around `=`, `=>`, `->`, `:=`, and infix operators
- no space inside `(`, `[`, or after `.`, `..`, `::`, `#`
- no space before `,`, `;`, `)`, `]`, `.`, `..`, `::`
- compact calls have no space between a function name and its `(` argument list;
  large calls use the multiline layout above; a `(` or `[`
  that opens an operand after an infix operator, `def`, `if`, `case` or
  `match` is spaced like any operand: `a / (b - c)`, `def (x, y) = pair`
- no space around the angle brackets of type arguments: `Array<Text>`,
  `fn<T>(value: T)`, `Map::Map<Text, Int>`
- no space after a symbolic unary operator: `-x`, `!done`, `*counter`, `-(a - b)`
- reference creation may be written `ref value` or `ref(value)`; the formatter
  preserves either form
- one space between `}` and a following `else` or other continuation keyword

### Blank lines

- exactly one blank line between top-level declarations (`def`, `class`,
  `instance`, `type`, `alias`, `foreign`)
- blank lines between `module`, `import`, `use`, and export directives are
  preserved, up to one blank line per gap
- the formatter collapses runs of blank lines down to at most one

### String literals

- text literals always use double quotes
- an unrecognized escape keeps its backslash. The formatter writes `\r` as
  `\\r` so the two spellings decode to the same text

## Comment Rules

The formatter preserves both `//` line comments and `/* ... */` block comments.

Attachment rules:

- **leading comments** stay attached to the next declaration or statement, with
  the comment placed on its own line at the same indentation as the following
  token
- **trailing comments** stay on the line they originally followed; they are
  separated from preceding code by **two spaces** for line comments and by one
  space for inline block comments
- **inline block comments** that sit between tokens on the same line stay
  inline, surrounded by single spaces

`marmotc.exe fmt` is intended to be idempotent for both comment-free and
comment-bearing source files. Any non-idempotent output is treated as a bug.

## CI Integration

To enforce formatting in CI, run the formatter in `--check` mode against the
source tree:

```powershell
marmotc.exe fmt src --check
marmotc.exe fmt test --check
```

`--check` exits with a non-zero status when any file would change. Combine with
`--format json` to consume a machine-readable summary in CI logs.
