# Marmot Versioning Policy

This document defines how Marmot versions its public surface.

The authoritative status of any user-visible feature lives in
[Feature Matrix](feature-matrix.md). This policy explains what those statuses
mean for compatibility, deprecation, and releases.

## The Grammar

[Grammar](grammar.md) is the language's syntax as of v1, and it is a contract:
no rule in it is removed. Later versions add rules; a program written against
that document keeps compiling. The syntax earlier versions dropped is gone from
the compiler entirely, so those words are ordinary identifiers now.

## Public Surface

Marmot treats the following as public surface when they are documented and
marked `Stable` in the feature matrix:

- language syntax, type rules, and runtime semantics
- documented prelude modules and APIs
- documented compiler warning/error codes and machine-readable report shapes
- documented manifest formats and CLI behavior meant for normal user workflows
- documented FFI ABI behavior

Undocumented internals are not versioned contracts. A feature appearing in the
implementation is not automatically part of the stable public surface unless the
docs and feature matrix say so.

## Version Scheme

Marmot uses semantic versioning in `MAJOR.MINOR.PATCH` form.

- `PATCH` releases are for backwards-compatible fixes, documentation updates,
  implementation refactors, and performance work. They should not intentionally
  break documented stable surface.
- `MINOR` releases are for backwards-compatible feature additions, experimental
  changes, deprecations, and experimental-to-stable promotions.
- `MAJOR` releases are for intentional breaking changes to stable surface after
  `1.0.0`.

Before `1.0.0`, Marmot uses `0.MINOR.PATCH` releases.

- During `0.x`, experimental surface may change between minor releases.
- During `0.x`, stable surface should still not change silently.
- If a stable-surface break is unavoidable before `1.0.0`, it must ship in a
  new minor release, not a patch release, and it must be called out explicitly
  as a breaking change with migration notes.

## What Counts as a Breaking Change

The following count as breaking changes when they affect `Stable` surface:

- previously valid source no longer lexes, parses, or type-checks
- the meaning of valid code changes in a way that can change results, control
  flow, imports, or visible runtime behavior
- a documented prelude API is removed, renamed, narrowed, or given an
  incompatible signature or contract
- a documented warning/error code is removed or renamed
- the machine-readable warning or compiler-report JSON shape changes in an
  incompatible way
- a documented manifest field or documented FFI ABI contract changes in an
  incompatible way
- a documented CLI workflow or environment-variable contract used by normal
  users changes incompatibly

The following do not count as breaking by themselves:

- bug fixes that move behavior toward the documented semantics
- documentation clarifications that do not change the implementation contract
- new warnings, unless they change the default outcome from success to failure
- additive APIs, syntax, or diagnostics that do not invalidate older code

## Release Communication

Every release that changes user-visible behavior should update the changelog or
release notes with clear sections for:

- `Breaking Changes`
- `Deprecated`
- `Removed`
- `Promoted to Stable`
- `Migration Notes`

When a stable feature changes:

- the feature matrix should be updated in the same change set
- the relevant user-facing docs should be updated in the same change set
- migration guidance should be included when user code may need edits

When a deprecation is machine-detectable, Marmot should prefer a compiler
warning with a stable warning code and a concrete suggestion.

If a breaking change must ship immediately for correctness, soundness, security,
or data-integrity reasons, the release notes should say that directly.

## Deprecating Stable Features

Stable features should be deprecated in stages:

1. Mark the feature as deprecated in the docs and note the preferred
   replacement.
2. Call out the deprecation in release notes.
3. Emit a deprecation warning when technically feasible.
4. Keep the deprecated behavior available for at least one minor release after
   the first public deprecation notice.
5. Remove the feature only in the next major release after `1.0.0`, or in a
   later `0.x` minor release if Marmot is still pre-1.0 and the removal is
   clearly announced as breaking.

Stable features should not be silently demoted back to `Experimental`. If the
project no longer wants to support a stable contract, that is itself a breaking
change.

## Experimental Surface

Experimental features are implemented but do not yet carry the full compatibility
guarantees of stable surface.

- They may change or be removed in minor releases.
- They should still be mentioned in release notes when behavior changes.
- Patch releases should avoid experimental breakage unless the change is a bug
  fix or a correctness/security response.
- Migration notes are still preferred when an experimental feature changes
  shape.

## Experimental-to-Stable Graduation

A feature should move from `Experimental` to `Stable` only when all of the
following are true:

1. The relevant implementation path is complete for normal use.
2. User-facing docs exist and match current behavior.
3. Automated coverage exists for the normal path and failure path when failure
   behavior is part of the contract.
4. The feature has been available for at least one minor release without
   breaking changes.
5. There are no known semantic bugs that block normal use.

Promotion should be recorded in two places:

- the feature matrix row changes from `Experimental` to `Stable`
- the release notes call out the promotion explicitly

Once promoted, the normal stable deprecation and breaking-change rules start
from that release onward.
