#pragma once

#include "Compiler/AbstractSyntaxTree/Type.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// What specializing a generic needs to know about types: which concrete type
// each type parameter stands for, and the type a generic one becomes.
namespace GenericTypes
{
	using TypeEnvironment = std::unordered_map<std::string, std::shared_ptr<MarmotType>>;

	// Records in `map` what each type parameter and type variable in `pattern`
	// stands for in `concrete`.
	void Deduce(const std::shared_ptr<MarmotType>& pattern, const std::shared_ptr<MarmotType>& concrete, TypeEnvironment& map);

	// Opcode selection only. Never use it for instance selection or generic
	// deduction: a newtype must stay nominal there, or instance Foo<Int>
	// starts matching Meters.
	std::shared_ptr<MarmotType> RepresentationOf(const std::shared_ptr<MarmotType>& type);

	// Whether a value of this type is a heap object at run time, which is the one
	// thing the VM needs to know of it and never works out from its bits. A newtype
	// is what it wraps. A type variable left this late is a compiler bug.
	bool IsPointer(const std::shared_ptr<MarmotType>& type);

	// A conversion between a newtype and its representation, which changes
	// nothing at run time.
	bool IsNewTypeErasedConversion(const std::shared_ptr<MarmotType>& from_type, const std::shared_ptr<MarmotType>& target_type);

	// No type parameter, type variable or undecided type anywhere in it.
	bool IsConcrete(const std::shared_ptr<MarmotType>& type);

	// An instance head with a type parameter left free, `Iterable<Mapped<S, A, B>>`.
	bool IsGenericInstanceHead(const std::vector<std::shared_ptr<MarmotType>>& type_args);
}
