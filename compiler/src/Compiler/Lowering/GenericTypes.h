#pragma once

#include "Compiler/AbstractSyntaxTree/Type.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// What specializing a generic needs to know about types: which concrete type
// each type parameter stands for, and the type a generic one becomes. Lowering
// and the code generator share these, so a specialization is typed one way.
namespace GenericTypes
{
	using TypeEnvironment = std::unordered_map<std::string, std::shared_ptr<MidoriType>>;

	// Records in `map` what each type parameter and type variable in `pattern`
	// stands for in `concrete`.
	void Deduce(const std::shared_ptr<MidoriType>& pattern, const std::shared_ptr<MidoriType>& concrete, TypeEnvironment& map);

	// What an associated type projection with concrete arguments is, or
	// nothing when no instance says.
	using AssociatedTypeResolver = std::function<std::optional<std::shared_ptr<MidoriType>>(const MidoriType::AssociatedType&)>;

	std::shared_ptr<MidoriType> Substitute(const std::shared_ptr<MidoriType>& type, const TypeEnvironment& map);
	// Also replaces each projection `resolve` knows with what it stands for.
	std::shared_ptr<MidoriType> Substitute(const std::shared_ptr<MidoriType>& type, const TypeEnvironment& map, const AssociatedTypeResolver& resolve);

	// Opcode selection only. Never use it for instance selection or generic
	// deduction: a newtype must stay nominal there, or instance Foo<Int>
	// starts matching Meters.
	std::shared_ptr<MidoriType> RepresentationOf(const std::shared_ptr<MidoriType>& type);

	// A conversion between a newtype and its representation, which changes
	// nothing at run time.
	bool IsNewTypeErasedConversion(const std::shared_ptr<MidoriType>& from_type, const std::shared_ptr<MidoriType>& target_type);

	// No type parameter, type variable or undecided type anywhere in it.
	bool IsConcrete(const std::shared_ptr<MidoriType>& type);

	// An instance head with a type parameter left free, `Iterable<Mapped<S, A, B>>`.
	bool IsGenericInstanceHead(const std::vector<std::shared_ptr<MidoriType>>& type_args);
}
