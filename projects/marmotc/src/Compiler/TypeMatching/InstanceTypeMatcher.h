#pragma once

#include "Compiler/AbstractSyntaxTree/Type.h"

#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// One candidate's instance-head bindings. A failed match rejects the candidate;
// copy the matcher when probing an alternative with the bindings already found.
class InstanceTypeMatcher
{
public:
	using TypeRef = std::shared_ptr<MarmotType>;
	using TypeEnvironment = std::unordered_map<std::string, TypeRef>;

	bool Match(const TypeRef& pattern, const TypeRef& concrete);
	bool MatchAll(std::span<const TypeRef> patterns, std::span<const TypeRef> concretes);
	const TypeEnvironment& Bindings() const;
	TypeEnvironment TakeBindings() &&;

private:
	struct TypePairHash
	{
		size_t operator()(const std::pair<TypeRef, TypeRef>& pair) const noexcept;
	};

	TypeEnvironment m_bindings;
	std::unordered_set<std::pair<TypeRef, TypeRef>, TypePairHash> m_visited;

	bool MatchArguments(const std::vector<std::string>& pattern_parameters, const std::vector<TypeRef>& pattern_arguments, const std::vector<std::string>& concrete_parameters, const std::vector<TypeRef>& concrete_arguments);
};
