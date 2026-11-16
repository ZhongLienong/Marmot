# Marmot language

Terms for Marmot's declarations, their visibility between modules, and type transformations.

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
