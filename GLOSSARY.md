# Marmot declarations

Terms for Marmot's typeclass and derived declarations and their visibility between modules.

## Language

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
