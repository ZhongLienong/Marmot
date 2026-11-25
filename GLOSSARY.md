# Marmot

Terms for Marmot's language, type transformations, and toolchain.

## Language

**Module**:
A named `.mmt` source unit with its own namespace. Export declarations control
which names other modules can access.

**Nominal type**:
A record, union, or newtype identified by its declaring module, name, and any
type arguments.

**Type alias**:
A transparent name for another type. The alias and the named type share
the same type identity.

**Typeclass**:
A named set of method and associated-type requirements parameterized by types.

**Instance**:
A declaration supplying a typeclass's methods and associated types for particular
type arguments, optionally subject to constraints. Its declaring module is the
module whose source contains that declaration.

**Typeclass metadata**:
A typeclass's methods, associated types and type parameters, together with the
instances visible to a module. Visibility through an import does not make that
module the declaring module of an instance.

**Derived declaration**:
A typeclass instance or container function generated from the shape of a record
or union through its `deriving` clause.

**Lexical binding**:
A parameter, local definition, pattern binding or loop variable belonging to its
declaring function. The alternatives of an or-pattern share one set of case
bindings.

**Binding identity**:
The identity shared by a lexical binding and every reference to it, independent
of its spelling or runtime storage. Specializing a generic body preserves its
source bindings while giving their values separate execution contexts.

## Type transformations

**Type transformation**:
An operation producing a type from another type while preserving its declaring
identity, with distinct rules for instantiation, freshening and substitution.

**Type instantiation**:
A use of a generic type with arguments for its declared parameters, including
parameters that appear in no field or representation.

**Type freshening**:
A use of a generic signature with fresh inference variables, keeping repeated
occurrences of each parameter tied to the same choice of type.

**Inference substitution**:
The interpretation of a type using the inference variables already decided and
the associated types known in the current checking context.

**Type specialization**:
The interpretation of a generic body's types for the arguments and typeclass
instances chosen at a use of that body.

## Projects and packages

**Project manifest**:
The `project.marmot` file that describes a project's entry source, layout, and
dependency requirements for the `marmot` project tool.

**Package**:
A versioned unit described by `package.marmot` that can supply modules and native
libraries to a project.

**Build plan**:
The JSON input that gives `marmotc` the resolved import search paths, native
library metadata, and optionally an entry source.

## Compiler and VM

**MarmotIR**:
The compiler's typed SSA representation of checked code before bytecode emission.

**`.mmc` artifact**:
The serialized program containing linked bytecode and its runtime metadata.
`marmotc` writes it and `marmotvm` loads it.

**VM intrinsic**:
A VM-provided operation with a registered signature. A Marmot `foreign`
declaration without `from` names a VM intrinsic.

**Standalone executable**:
A platform-native executable that bundles the VM, a `.mmc` artifact, and the
native libraries imported by that artifact.
