Read `AGENTS.md` before anything larger than a one-line edit: it has the build
and test commands, the repo layout, the rules that are not style (no defensive
programming, the grammar is sealed at v1), how to write Marmot for tests and the
prelude, the compiler invariants that were each a bug once, and the gotchas.

- Try not to use `auto`
- Use `c++23` features
- use prefix `m_` for class members, `s_` for static values
- favor function chaining and the functional programming paradigm
- define and use constructor
- Dont write comments for self-explanatory code
- Dont write docs unless told so
- Try to put implementations inside `cpp` files
- Try to use `{}` for `for`, `if` etc, never do oneline.
- Use Pascal style for class/struct/function names
