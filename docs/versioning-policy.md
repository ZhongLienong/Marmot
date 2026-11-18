# Marmot Versioning and Compatibility

Marmot is actively developed. No language, API, tooling, diagnostic, FFI, or
bytecode surface is currently frozen for backward compatibility. The
[Feature Matrix](feature-matrix.md) describes implementation maturity and
coverage; `Stable` does not promise that a feature will remain unchanged.

Syntax, type rules, runtime behavior, prelude APIs, diagnostic codes and JSON,
CLI behavior, manifests, FFI details, and `.mmc` encoding may change or be
removed as the project evolves. Source code, tooling, packages, and artifacts
may need updates after a Marmot release.

## Release Versions

Marmot release numbers use `MAJOR.MINOR.PATCH`, but their components do not
currently guarantee source, API, tooling, or artifact compatibility. The
`marmot` package resolver uses semantic versions to evaluate package constraints;
a version satisfying a constraint is a version-selection result, not a
compatibility guarantee made by this repository.

## `.mmc` Format Coordination

The `.mmc` header carries an internal format marker so the current compiler and
VM can agree on an artifact's encoding. The current VM rejects artifacts with a
different marker; this does not commit Marmot to reading older artifacts. When
a format change could make an artifact decode or execute differently, update
the marker so a mismatched reader rejects it instead of misinterpreting it.
Opcode IDs, builtin IDs, layouts, and semantics may change as the format
develops. There is no released or sealed `.mmc` format version; the current
registry and fixtures describe behavior in progress and may change with the
implementation.

## Change Notes

Update the relevant docs when behavior or an interface changes. Release notes
and migration guidance are useful when a change affects existing users, but
Marmot does not currently require staged deprecation, a waiting period, or a
particular release category for such changes.

A future compatibility commitment should identify the specific surface and
rules it covers. Until then, treat the docs and feature matrix as descriptions
of current behavior.
