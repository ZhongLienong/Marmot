# Formatting

Marmot ships a first-party formatter through:

```powershell
marmotc.exe fmt <file|dir>
```

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
| `if`       | `if(cond) then { ... } else { ... };`                           |
| `for`      | `for x in iter { ... };`                                        |
| `match`    | `match value with case <pattern> => expr case _ => expr;`      |
| `type` (record) | `type Name = { field: Type, ... };`                        |
| `type` (sum) | `type Name = Variant \| Variant(Type) \| ...;`                |
| `class`    | `class Name<T> { method: fn(value: T) -> Type; ... };`           |
| `instance` | `instance Name<Type> { def method = fn(value: Type) -> Type => ...; };` |

`import { ... }` and `export { ... }` use a single-line *inline brace*: the
opening and closing braces stay on the same line as the directive even when the
list grows. Inline braces never trigger indentation.

### Line width

- target line width is **100 characters**
- the formatter does **not** automatically wrap or reflow expressions; long lines
  remain as the author wrote them
- authors are expected to break long lines manually, e.g. by introducing
  intermediate `def` bindings or by splitting list literals across multiple lines

The line width is a guideline, not a hard limit. CI does not fail on long lines.

### Trailing commas

- multi-line argument, parameter, record field, and array literal lists may end
  with a trailing comma
- the formatter preserves an existing trailing comma and does not add one when
  it is missing
- single-line lists never receive a trailing comma

### Import ordering and grouping

Imports inside a single `import { ... }` block are grouped by category in this
order, and ordered alphabetically within each group:

1. `<Module>` brace-style references to package-resolved modules
2. `"path/to/Module.mmt"` quoted relative paths

A file may contain multiple `import` blocks; the formatter does not merge them,
but it preserves blank-line separation between top-level directives so authors
can group related imports manually.

### Match arm alignment

- each `case` arm, including a catch-all `case _`, appears on its own line
- arms inside a `match ... with` are indented one level beyond the surrounding
  block
- exactly one space appears between the pattern and `=>`, and one space between
  `=>` and the arm body
- the trailing `;` that closes the `match` expression appears on its own
  position immediately after the last arm

```marmot
def name = match value with
    case (Some(x), 0) => x
    case (Some(x), _) => x + 1
    case _ => 0;
```

The formatter does **not** vertically align the `=>` arrows. Authors who want
column alignment can add spaces by hand; the formatter will preserve them as
long as they sit between the pattern and the arrow.

### Spacing

- one space after `,`
- one space around `=`, `=>`, `->`, `::=`, and infix operators
- no space inside `(`, `[`, or after `.`, `..`, `::`, `#`
- no space before `,`, `;`, `)`, `]`, `.`, `..`, `::`
- no space between a function name and its `(` argument list
- one space between `}` and a following `else` or other continuation keyword

### Blank lines

- exactly one blank line between top-level declarations (`def`, `class`,
  `instance`, `type`, `alias`, `foreign`)
- consecutive `module`, `import`, `use`, `public`, and `private` directives
  receive **no** blank line between them; they form a header block
- the formatter collapses runs of blank lines down to at most one

### String literals

- text literals always use double quotes
- escape sequences are normalized to `\\`, `\"`, `\n`, `\r`, `\t`, `\b`, `\f`

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
