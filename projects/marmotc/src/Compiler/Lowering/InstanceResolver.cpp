#include "InstanceResolver.h"
#include "Bytecode/Format/Format.h"
#include "Compiler/Constant/Constant.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <ranges>
#include <utility>

namespace
{
	using TypeRef = std::shared_ptr<MidoriType>;

	struct TypePairHash
	{
		size_t operator()(const std::pair<MidoriType*, MidoriType*>& pair) const
		{
			return std::hash<MidoriType*>{}(pair.first) ^ (std::hash<MidoriType*>{}(pair.second) << 1u);
		}
	};

	using VisitedPairs = std::unordered_set<std::pair<MidoriType*, MidoriType*>, TypePairHash>;

	bool MatchInstanceTypeArg(const TypeRef& pattern, const TypeRef& concrete, GenericTypes::TypeEnvironment& substitutions, VisitedPairs& visited);

	bool MatchAll(const std::vector<TypeRef>& patterns, const std::vector<TypeRef>& concretes, GenericTypes::TypeEnvironment& substitutions, VisitedPairs& visited)
	{
		if (patterns.size() != concretes.size())
		{
			return false;
		}
		for (size_t index = 0u; index < patterns.size(); index += 1u)
		{
			if (!MatchInstanceTypeArg(patterns[index], concretes[index], substitutions, visited))
			{
				return false;
			}
		}
		return true;
	}

	// A type argument no member mentions, `Taken<S, A>`'s A, is matched by
	// the arguments the type was instantiated with.
	bool MatchTypeArguments(const std::vector<TypeRef>& patterns, const std::vector<TypeRef>& concretes, GenericTypes::TypeEnvironment& substitutions, VisitedPairs& visited)
	{
		return patterns.size() != concretes.size() || MatchAll(patterns, concretes, substitutions, visited);
	}

	bool MatchInstanceTypeArg(const TypeRef& pattern, const TypeRef& concrete, GenericTypes::TypeEnvironment& substitutions, VisitedPairs& visited)
	{
		if (!visited.emplace(pattern.get(), concrete.get()).second)
		{
			return true;
		}

		if (pattern->IsType<MidoriType::GenericParam>())
		{
			const std::string& param_name = pattern->GetType<MidoriType::GenericParam>().m_name;
			const GenericTypes::TypeEnvironment::const_iterator bound = substitutions.find(param_name);
			if (bound != substitutions.cend())
			{
				return *bound->second == *concrete;
			}
			substitutions.emplace(param_name, concrete);
			return true;
		}

		if (pattern->IsType<MidoriType::ArrayType>())
		{
			return concrete->IsType<MidoriType::ArrayType>() && MatchInstanceTypeArg(pattern->GetType<MidoriType::ArrayType>().m_element_type, concrete->GetType<MidoriType::ArrayType>().m_element_type, substitutions, visited);
		}

		if (pattern->IsType<MidoriType::CellType>())
		{
			return concrete->IsType<MidoriType::CellType>() && MatchInstanceTypeArg(pattern->GetType<MidoriType::CellType>().m_element_type, concrete->GetType<MidoriType::CellType>().m_element_type, substitutions, visited);
		}

		if (pattern->IsType<MidoriType::RangeType>())
		{
			return concrete->IsType<MidoriType::RangeType>() && MatchInstanceTypeArg(pattern->GetType<MidoriType::RangeType>().m_element_type, concrete->GetType<MidoriType::RangeType>().m_element_type, substitutions, visited);
		}

		if (pattern->IsType<MidoriType::TupleType>())
		{
			return concrete->IsType<MidoriType::TupleType>() && MatchAll(pattern->GetType<MidoriType::TupleType>().m_element_types, concrete->GetType<MidoriType::TupleType>().m_element_types, substitutions, visited);
		}

		if (pattern->IsType<MidoriType::FunctionType>())
		{
			if (!concrete->IsType<MidoriType::FunctionType>())
			{
				return false;
			}
			const MidoriType::FunctionType& pattern_function = pattern->GetType<MidoriType::FunctionType>();
			const MidoriType::FunctionType& concrete_function = concrete->GetType<MidoriType::FunctionType>();
			return MatchAll(pattern_function.m_param_types, concrete_function.m_param_types, substitutions, visited) && MatchInstanceTypeArg(pattern_function.m_return_type, concrete_function.m_return_type, substitutions, visited);
		}

		if (pattern->IsType<MidoriType::StructType>())
		{
			if (!concrete->IsType<MidoriType::StructType>())
			{
				return false;
			}
			const MidoriType::StructType& pattern_struct = pattern->GetType<MidoriType::StructType>();
			const MidoriType::StructType& concrete_struct = concrete->GetType<MidoriType::StructType>();
			return pattern_struct.m_name == concrete_struct.m_name && pattern_struct.m_module_name == concrete_struct.m_module_name && MatchAll(pattern_struct.m_member_types, concrete_struct.m_member_types, substitutions, visited) && MatchTypeArguments(pattern_struct.m_type_arguments, concrete_struct.m_type_arguments, substitutions, visited);
		}

		if (pattern->IsType<MidoriType::UnionType>())
		{
			if (!concrete->IsType<MidoriType::UnionType>())
			{
				return false;
			}
			const MidoriType::UnionType& pattern_union = pattern->GetType<MidoriType::UnionType>();
			const MidoriType::UnionType& concrete_union = concrete->GetType<MidoriType::UnionType>();
			if (pattern_union.m_name != concrete_union.m_name || pattern_union.m_module_name != concrete_union.m_module_name || pattern_union.m_member_info.size() != concrete_union.m_member_info.size() || !MatchTypeArguments(pattern_union.m_type_arguments, concrete_union.m_type_arguments, substitutions, visited))
			{
				return false;
			}
			return std::ranges::all_of(pattern_union.m_member_info, [&](const std::pair<const std::string, MidoriType::UnionType::UnionMemberContext>& member)
			{
				const std::unordered_map<std::string, MidoriType::UnionType::UnionMemberContext>::const_iterator concrete_member = concrete_union.m_member_info.find(member.first);
				return concrete_member != concrete_union.m_member_info.cend() && MatchAll(member.second.m_member_types, concrete_member->second.m_member_types, substitutions, visited);
			});
		}

		if (pattern->IsType<MidoriType::AssociatedType>())
		{
			if (!concrete->IsType<MidoriType::AssociatedType>())
			{
				return false;
			}
			const MidoriType::AssociatedType& pattern_associated = pattern->GetType<MidoriType::AssociatedType>();
			const MidoriType::AssociatedType& concrete_associated = concrete->GetType<MidoriType::AssociatedType>();
			return pattern_associated.m_class_name == concrete_associated.m_class_name && pattern_associated.m_name == concrete_associated.m_name && MatchAll(pattern_associated.m_type_args, concrete_associated.m_type_args, substitutions, visited);
		}

		return *pattern == *concrete;
	}

	std::string QualifiedMethod(std::string_view class_name, std::string_view method_name)
	{
		return std::format("{}{}{}", class_name, NameSeparator, method_name);
	}

	const std::vector<ResolvedMethodCandidate>* FindCandidates(const MethodResolutionMap& resolutions, std::string_view class_name, std::string_view method_name)
	{
		const MethodResolutionMap::const_iterator found = resolutions.find(QualifiedMethod(class_name, method_name));
		return found == resolutions.cend() ? nullptr : &found->second;
	}
}

MethodResolutionError::MethodResolutionError(CompilerErrorCode code, std::string message)
	: m_code(code),
	m_message(std::move(message))
{
}

ResolvedMethodCandidate::ResolvedMethodCandidate(std::string first_type_name, std::string second_type_name, std::string resolved_name, bool has_instance)
	: m_first_type_name(std::move(first_type_name)),
	m_second_type_name(std::move(second_type_name)),
	m_resolved_name(std::move(resolved_name)),
	m_has_instance(has_instance)
{
}

ResolvedInstanceName::ResolvedInstanceName(const std::string& resolved_name)
{
	const size_t at_pos = resolved_name.find(ModuleSeparator);
	m_symbol = resolved_name.substr(0u, at_pos);
	if (at_pos != std::string::npos)
	{
		m_module = resolved_name.substr(at_pos + 1u);
	}
}

InstanceResolver::InstanceResolver(ClassMethods class_methods, ClassInstances class_instances, ClassInstanceTypes class_instance_type_args, const ClassInstanceAssociatedTypes& associated_types, IsModuleGlobal is_module_global)
	: m_class_methods(std::move(class_methods)),
	m_class_instances(std::move(class_instances)),
	m_class_instance_type_args(std::move(class_instance_type_args)),
	m_is_module_global(std::move(is_module_global))
{
	for (const auto& [class_name, bindings] : associated_types)
	{
		const ClassInstanceTypes::const_iterator type_args = m_class_instance_type_args.find(class_name);
		for (size_t index = 0u; type_args != m_class_instance_type_args.cend() && index < bindings.size() && index < type_args->second.size(); index += 1u)
		{
			AddAssociatedTypes(class_name, type_args->second[index], bindings[index]);
		}
	}
}

void InstanceResolver::AddAssociatedTypes(const std::string& class_name, const std::vector<std::shared_ptr<MidoriType>>& type_args, AssociatedTypeBindings bindings)
{
	if (!bindings.empty())
	{
		m_associated_types[class_name].emplace_back(type_args, std::move(bindings));
	}
}

std::optional<std::shared_ptr<MidoriType>> InstanceResolver::ResolveAssociatedType(const MidoriType::AssociatedType& projection) const
{
	const std::unordered_map<std::string, std::vector<std::pair<std::vector<TypeRef>, AssociatedTypeBindings>>>::const_iterator instances = m_associated_types.find(projection.m_class_name);
	if (instances == m_associated_types.cend())
	{
		return std::nullopt;
	}

	for (const auto& [type_args, bindings] : instances->second)
	{
		GenericTypes::TypeEnvironment substitutions;
		VisitedPairs visited;
		const AssociatedTypeBindings::const_iterator binding = bindings.find(projection.m_name);
		if (binding != bindings.cend() && MatchAll(type_args, projection.m_type_args, substitutions, visited))
		{
			return GenericTypes::Substitute(binding->second, substitutions);
		}
	}
	return std::nullopt;
}

void InstanceResolver::AddClass(const std::string& class_name, std::unordered_set<std::string> method_names)
{
	m_class_methods[class_name] = std::move(method_names);
}

void InstanceResolver::AddInstanceTypeArgs(const std::string& class_name, const std::vector<std::shared_ptr<MidoriType>>& type_args)
{
	std::vector<std::vector<TypeRef>>& existing = m_class_instance_type_args[class_name];
	const bool is_present = std::ranges::any_of(existing, [&type_args](const std::vector<TypeRef>& candidate)
	{
		return std::ranges::equal(candidate, type_args, [](const TypeRef& left, const TypeRef& right) { return *left == *right; });
	});
	if (!is_present)
	{
		existing.push_back(type_args);
	}
}

void InstanceResolver::AddInstanceMethod(const std::string& class_name, const std::string& method_name)
{
	std::vector<std::string>& methods = m_class_instances[class_name];
	if (!std::ranges::contains(methods, method_name))
	{
		methods.push_back(method_name);
	}
}

bool InstanceResolver::IsClassMethod(const std::string& qualified_name) const
{
	const size_t separator = qualified_name.rfind(NameSeparator);
	if (separator == std::string::npos)
	{
		return false;
	}
	const ClassMethods::const_iterator methods = m_class_methods.find(qualified_name.substr(0u, separator));
	return methods != m_class_methods.cend() && methods->second.contains(qualified_name.substr(separator + NameSeparator.length()));
}

std::optional<std::string> InstanceResolver::ResolveInstanceName(const std::string& class_name, const std::string& base_name) const
{
	if (m_is_module_global(base_name))
	{
		return base_name;
	}

	const ClassInstances::const_iterator instances = m_class_instances.find(class_name);
	if (instances == m_class_instances.cend())
	{
		return std::nullopt;
	}

	const std::string with_module = base_name + ModuleSeparator;
	const std::vector<std::string>::const_iterator found = std::ranges::find_if(instances->second, [&](const std::string& method)
	{
		return method == base_name || method.starts_with(with_module);
	});
	return found == instances->second.cend() ? std::nullopt : std::optional<std::string>(*found);
}

std::optional<std::string> InstanceResolver::ResolveInstanceNameForTypeArgs(const std::string& class_name, const std::string& method_name, const std::vector<std::shared_ptr<MidoriType>>& concrete_type_args) const
{
	const std::optional<std::string> exact = ResolveInstanceName(class_name, MidoriType::MangleInstanceMethodName(method_name, class_name, concrete_type_args));
	if (exact.has_value())
	{
		return exact;
	}

	const ClassInstanceTypes::const_iterator instance_args = m_class_instance_type_args.find(class_name);
	if (instance_args == m_class_instance_type_args.cend())
	{
		return std::nullopt;
	}

	for (const std::vector<TypeRef>& candidate_args : instance_args->second)
	{
		GenericTypes::TypeEnvironment substitutions;
		VisitedPairs visited;
		if (!MatchAll(candidate_args, concrete_type_args, substitutions, visited))
		{
			continue;
		}

		const std::optional<std::string> candidate = ResolveInstanceName(class_name, MidoriType::MangleInstanceMethodName(method_name, class_name, candidate_args));
		if (candidate.has_value())
		{
			return candidate;
		}
	}
	return std::nullopt;
}

MethodResolutionMap InstanceResolver::ResolveConstraints(const std::vector<MidoriType::ClassConstraint>& constraints, const GenericTypes::TypeEnvironment& substitutions) const
{
	MethodResolutionMap resolutions;
	for (const MidoriType::ClassConstraint& constraint : constraints)
	{
		const ClassMethods::const_iterator methods = m_class_methods.find(constraint.m_class_name);
		if (methods == m_class_methods.cend())
		{
			continue;
		}

		const std::vector<TypeRef> concrete_type_args = constraint.m_type_args
			| std::views::transform([&substitutions](const TypeRef& type_arg) { return GenericTypes::Substitute(type_arg, substitutions); })
			| std::ranges::to<std::vector>();
		const std::string first_type_name = concrete_type_args.empty() ? std::string() : concrete_type_args[0u]->ToString();
		const std::string second_type_name = concrete_type_args.size() > 1u ? concrete_type_args[1u]->ToString() : std::string();

		for (const std::string& method_name : methods->second)
		{
			const std::optional<std::string> instance = ResolveInstanceNameForTypeArgs(constraint.m_class_name, method_name, concrete_type_args);
			const std::string resolved = instance.value_or(MidoriType::MangleInstanceMethodName(method_name, constraint.m_class_name, concrete_type_args));
			resolutions[QualifiedMethod(constraint.m_class_name, method_name)].emplace_back(first_type_name, second_type_name, resolved, instance.has_value());
		}
	}
	return resolutions;
}

MethodResolution<std::string> InstanceResolver::ResolveConstrainedCall(const MethodResolutionMap& resolutions, const std::string& callee_name, const std::shared_ptr<MidoriType>* first_argument_type, const std::shared_ptr<MidoriType>& return_type) const
{
	const std::vector<ResolvedMethodCandidate>& candidates = resolutions.at(callee_name);
	if (first_argument_type == nullptr)
	{
		if (candidates.size() == 1u && candidates[0u].m_has_instance)
		{
			return candidates[0u].m_resolved_name;
		}
		return std::unexpected(MethodResolutionError(CompilerErrorCode::LoweringAmbiguousMethodResolution, std::format("Ambiguous method '{}': cannot resolve a method call with no arguments.", callee_name)));
	}

	const std::string first_arg_type_name = (*first_argument_type)->ToString();
	const std::string return_type_name = return_type->ToString();

	std::vector<const ResolvedMethodCandidate*> matching = candidates
		| std::views::filter([&](const ResolvedMethodCandidate& candidate) { return candidate.m_first_type_name == first_arg_type_name; })
		| std::views::transform([](const ResolvedMethodCandidate& candidate) { return &candidate; })
		| std::ranges::to<std::vector>();

	if (matching.empty())
	{
		const std::string candidates_info = candidates
			| std::views::transform([](const ResolvedMethodCandidate& candidate) { return std::format("\nCandidate: {} (First: '{}', Instance: {})", candidate.m_resolved_name, candidate.m_first_type_name, candidate.m_has_instance); })
			| std::views::join
			| std::ranges::to<std::string>();
		return std::unexpected(MethodResolutionError(CompilerErrorCode::LoweringAmbiguousMethodResolution, std::format("Ambiguous method '{}': no constraint matches argument type '{}'. Candidates:{}", callee_name, first_arg_type_name, candidates_info)));
	}

	const bool return_type_is_concrete =
		!return_type_name.empty() &&
		return_type_name != "Undecided" &&
		!(return_type_name.size() > 1u && return_type_name[0u] == 'T' && std::isdigit(static_cast<unsigned char>(return_type_name[1u])) != 0);

	// This tiebreaker assumes a class's second type parameter is its return type,
	// which holds for Convertable<From, To> and not in general. Indexable<C, I>
	// violates it: the second parameter is the index type. Dormant for Indexable
	// today because the type checker rejects two constraints on one class that
	// differ only in the second parameter.
	if (matching.size() > 1u && return_type_is_concrete)
	{
		std::vector<const ResolvedMethodCandidate*> matching_by_return = matching
			| std::views::filter([&](const ResolvedMethodCandidate* candidate) { return !candidate->m_second_type_name.empty() && candidate->m_second_type_name == return_type_name; })
			| std::ranges::to<std::vector>();
		if (!matching_by_return.empty())
		{
			matching = std::move(matching_by_return);
		}
	}

	if (matching.size() != 1u)
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::LoweringAmbiguousMethodResolution, std::format("Ambiguous method '{}': multiple constraints match argument type '{}'. Make constraints more specific.", callee_name, first_arg_type_name)));
	}

	const ResolvedMethodCandidate& selected = *matching[0u];
	if (!selected.m_has_instance)
	{
		std::string suffix;
		if (!selected.m_first_type_name.empty())
		{
			suffix = std::format(" (constraint types: {}", selected.m_first_type_name);
			if (!selected.m_second_type_name.empty())
			{
				suffix.append(", ").append(selected.m_second_type_name);
			}
			suffix.append(")");
		}
		return std::unexpected(MethodResolutionError(CompilerErrorCode::LoweringUnresolvedMethodResolution, std::format("Unresolved method '{}': no matching instance found{}.", callee_name, suffix)));
	}
	return selected.m_resolved_name;
}

MethodResolution<std::string> InstanceResolver::ResolveConstrainedValue(const MethodResolutionMap& resolutions, const std::string& name) const
{
	const std::vector<ResolvedMethodCandidate>& candidates = resolutions.at(name);
	if (candidates.size() != 1u)
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::LoweringAmbiguousMethodResolution, std::format("Ambiguous method '{}': cannot use method value when multiple class constraints are in scope.", name)));
	}
	if (!candidates[0u].m_has_instance)
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::LoweringUnresolvedMethodResolution, std::format("Unresolved method '{}': no matching instance found.", name)));
	}
	return candidates[0u].m_resolved_name;
}

MethodResolution<std::optional<std::string>> InstanceResolver::ResolveConcreteCall(const std::string& callee_name, const std::vector<std::shared_ptr<MidoriType>>& argument_types, const std::shared_ptr<MidoriType>& return_type) const
{
	if (!IsClassMethod(callee_name))
	{
		return std::nullopt;
	}

	const size_t separator = callee_name.rfind(NameSeparator);
	const std::string qualifier = callee_name.substr(0u, separator);
	const std::string method_name = callee_name.substr(separator + NameSeparator.length());

	const ClassInstanceTypes::const_iterator instance_args = m_class_instance_type_args.find(qualifier);
	if (instance_args == m_class_instance_type_args.cend())
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::LoweringUnresolvedMethodResolution, std::format("Unresolved method '{}': no instance metadata found.", callee_name)));
	}

	std::vector<ResolvedMethodCandidate> candidates;
	for (const std::vector<TypeRef>& candidate_args : instance_args->second)
	{
		if (candidate_args.empty())
		{
			continue;
		}

		GenericTypes::TypeEnvironment substitutions;
		VisitedPairs visited;
		bool matched = true;
		if (candidate_args.size() == 1u)
		{
			matched = std::ranges::all_of(argument_types, [&](const TypeRef& argument_type) { return MatchInstanceTypeArg(candidate_args[0u], argument_type, substitutions, visited); });
		}
		else
		{
			const size_t positional_count = std::min(candidate_args.size(), argument_types.size());
			for (size_t index = 0u; index < positional_count && matched; index += 1u)
			{
				matched = MatchInstanceTypeArg(candidate_args[index], argument_types[index], substitutions, visited);
			}

			if (matched && argument_types.size() + 1u == candidate_args.size())
			{
				matched = MatchInstanceTypeArg(candidate_args.back(), return_type, substitutions, visited);
			}
			else if (matched && argument_types.size() < candidate_args.size())
			{
				matched = false;
			}
		}

		if (!matched)
		{
			continue;
		}

		const std::string mangled_name = MidoriType::MangleInstanceMethodName(method_name, qualifier, candidate_args);
		const std::optional<std::string> resolved_name = ResolveInstanceName(qualifier, mangled_name);
		candidates.emplace_back(candidate_args[0u]->ToString(), candidate_args.size() > 1u ? candidate_args[1u]->ToString() : std::string(), resolved_name.value_or(mangled_name), resolved_name.has_value());
	}

	if (candidates.empty())
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::LoweringUnresolvedMethodResolution, std::format("Unresolved method '{}': no matching concrete instance found.", callee_name)));
	}
	if (candidates.size() != 1u)
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::LoweringAmbiguousMethodResolution, std::format("Ambiguous method '{}': multiple concrete instances match this call.", callee_name)));
	}
	if (!candidates[0u].m_has_instance)
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::LoweringUnresolvedMethodResolution, std::format("Unresolved method '{}': no emitted instance method was found.", callee_name)));
	}
	return std::optional<std::string>(candidates[0u].m_resolved_name);
}

MethodResolution<std::optional<std::string>> InstanceResolver::MatchSingleInstance(const std::string& class_name, const std::string& method_name, const std::function<bool(const std::vector<std::shared_ptr<MidoriType>>&, GenericTypes::TypeEnvironment&)>& matches, const std::string& ambiguity_message) const
{
	const ClassInstanceTypes::const_iterator instance_args = m_class_instance_type_args.find(class_name);
	if (instance_args == m_class_instance_type_args.cend())
	{
		return std::nullopt;
	}

	std::optional<std::string> resolved;
	for (const std::vector<TypeRef>& candidate_args : instance_args->second)
	{
		GenericTypes::TypeEnvironment substitutions;
		if (!matches(candidate_args, substitutions))
		{
			continue;
		}

		std::optional<std::string> candidate = ResolveInstanceName(class_name, MidoriType::MangleInstanceMethodName(method_name, class_name, candidate_args));
		if (!candidate.has_value())
		{
			continue;
		}
		if (resolved.has_value() && resolved.value() != candidate.value())
		{
			return std::unexpected(MethodResolutionError(CompilerErrorCode::None, ambiguity_message));
		}
		resolved = std::move(candidate);
	}
	return resolved;
}

MethodResolution<std::string> InstanceResolver::ResolveConcat(const MethodResolutionMap& resolutions, const std::shared_ptr<MidoriType>& operand_type) const
{
	std::optional<std::string> resolved;
	const std::vector<ResolvedMethodCandidate>* candidates = FindCandidates(resolutions, CONCATENABLE_CLASS_NAME, CONCAT_METHOD_NAME);
	if (candidates != nullptr)
	{
		const std::string operand_type_name = operand_type->ToString();
		for (const ResolvedMethodCandidate& candidate : *candidates)
		{
			if (candidate.m_first_type_name != operand_type_name || !candidate.m_has_instance)
			{
				continue;
			}
			if (resolved.has_value() && resolved.value() != candidate.m_resolved_name)
			{
				return std::unexpected(MethodResolutionError(CompilerErrorCode::None, std::format("Concatenable instance method resolution is ambiguous for type '{}'", operand_type_name)));
			}
			resolved = candidate.m_resolved_name;
		}
	}

	if (!resolved.has_value())
	{
		const MethodResolution<std::optional<std::string>> matched = MatchSingleInstance(std::string(CONCATENABLE_CLASS_NAME), std::string(CONCAT_METHOD_NAME), [&operand_type](const std::vector<TypeRef>& candidate_args, GenericTypes::TypeEnvironment& substitutions)
		{
			VisitedPairs visited;
			return candidate_args.size() == 1u && MatchInstanceTypeArg(candidate_args[0u], operand_type, substitutions, visited);
		}, std::format("Concatenable instance method resolution is ambiguous for type '{}'", operand_type->DisplayString()));
		if (!matched.has_value())
		{
			return std::unexpected(matched.error());
		}
		resolved = matched.value();
	}

	if (!resolved.has_value())
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::None, std::format("Concatenable instance method '{}' not found", MidoriType::MangleInstanceMethodName(std::string(CONCAT_METHOD_NAME), std::string(CONCATENABLE_CLASS_NAME), { operand_type }))));
	}
	return resolved.value();
}

MethodResolution<std::string> InstanceResolver::ResolveEquals(const std::shared_ptr<MidoriType>& operand_type) const
{
	return ResolveOperandInstance(std::string(EQUATABLE_CLASS_NAME), std::string(EQUALS_METHOD_NAME), operand_type);
}

MethodResolution<std::string> InstanceResolver::ResolveCompare(const std::shared_ptr<MidoriType>& operand_type) const
{
	return ResolveOperandInstance(std::string(ORDERABLE_CLASS_NAME), std::string(COMPARE_METHOD_NAME), operand_type);
}

// The instance an operator on one operand type calls: the one declared for
// exactly that type, or else the single generic instance that matches it.
MethodResolution<std::string> InstanceResolver::ResolveOperandInstance(const std::string& class_name, const std::string& method_name, const std::shared_ptr<MidoriType>& operand_type) const
{
	const std::string mangled_name = MidoriType::MangleInstanceMethodName(method_name, class_name, { operand_type });
	if (m_is_module_global(mangled_name))
	{
		return mangled_name;
	}

	const MethodResolution<std::optional<std::string>> matched = MatchSingleInstance(class_name, method_name, [&operand_type](const std::vector<TypeRef>& candidate_args, GenericTypes::TypeEnvironment& substitutions)
	{
		VisitedPairs visited;
		return candidate_args.size() == 1u && MatchInstanceTypeArg(candidate_args[0u], operand_type, substitutions, visited);
	}, std::format("{} instance method resolution is ambiguous for type '{}'", class_name, operand_type->DisplayString()));
	if (!matched.has_value())
	{
		return std::unexpected(matched.error());
	}
	if (!matched.value().has_value())
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::None, std::format("{} instance method '{}' not found", class_name, mangled_name)));
	}
	return matched.value().value();
}

MethodResolution<std::optional<std::string>> InstanceResolver::ResolveCount(const MethodResolutionMap& resolutions, const std::shared_ptr<MidoriType>& operand_type, bool uses_countable) const
{
	const std::vector<ResolvedMethodCandidate>* candidates = FindCandidates(resolutions, COUNTABLE_CLASS_NAME, COUNT_METHOD_NAME);
	if (candidates != nullptr)
	{
		const std::string type_name = operand_type->ToString();
		const std::vector<ResolvedMethodCandidate>::const_iterator found = std::ranges::find_if(*candidates, [&](const ResolvedMethodCandidate& candidate) { return candidate.m_first_type_name == type_name && candidate.m_has_instance; });
		if (found != candidates->cend())
		{
			return std::optional<std::string>(found->m_resolved_name);
		}
	}

	if (operand_type->IsType<MidoriType::TypeVariable>())
	{
		if (uses_countable)
		{
			return std::unexpected(MethodResolutionError(CompilerErrorCode::None, "Cannot resolve Countable instance for type variables outside of specialization context"));
		}
		return std::nullopt;
	}

	const std::optional<std::string> resolved = ResolveInstanceNameForTypeArgs(std::string(COUNTABLE_CLASS_NAME), std::string(COUNT_METHOD_NAME), { operand_type });
	if (resolved.has_value())
	{
		return resolved;
	}
	if (uses_countable)
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::None, std::format("Countable instance method '{}' not found", MidoriType::MangleInstanceMethodName(std::string(COUNT_METHOD_NAME), std::string(COUNTABLE_CLASS_NAME), { operand_type }))));
	}
	return std::nullopt;
}

MethodResolution<std::string> InstanceResolver::ResolveIndex(const MethodResolutionMap& resolutions, const std::shared_ptr<MidoriType>& container_type, const std::shared_ptr<MidoriType>& index_type) const
{
	const std::vector<ResolvedMethodCandidate>* candidates = FindCandidates(resolutions, INDEXABLE_CLASS_NAME, GET_METHOD_NAME);
	if (candidates != nullptr)
	{
		const std::string container_type_name = container_type->ToString();
		const std::string index_type_name = index_type->ToString();
		const std::vector<ResolvedMethodCandidate>::const_iterator found = std::ranges::find_if(*candidates, [&](const ResolvedMethodCandidate& candidate)
		{
			return candidate.m_first_type_name == container_type_name && candidate.m_second_type_name == index_type_name && candidate.m_has_instance;
		});
		if (found != candidates->cend())
		{
			return found->m_resolved_name;
		}
	}

	if (container_type->IsType<MidoriType::TypeVariable>())
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::None, "Cannot resolve Indexable instance for type variables outside of specialization context"));
	}

	const std::optional<std::string> resolved = ResolveInstanceNameForTypeArgs(std::string(INDEXABLE_CLASS_NAME), std::string(GET_METHOD_NAME), { container_type, index_type });
	if (resolved.has_value())
	{
		return resolved.value();
	}
	return std::unexpected(MethodResolutionError(CompilerErrorCode::None, std::format("Indexable instance method '{}' not found", MidoriType::MangleInstanceMethodName(std::string(GET_METHOD_NAME), std::string(INDEXABLE_CLASS_NAME), { container_type }))));
}

MethodResolution<std::optional<std::string>> InstanceResolver::ResolveConvert(const MethodResolutionMap& resolutions, const std::shared_ptr<MidoriType>& concrete_from_type, const std::shared_ptr<MidoriType>& from_type, const std::shared_ptr<MidoriType>& to_type, bool uses_convertable) const
{
	const bool has_type_variable = from_type->IsType<MidoriType::TypeVariable>() || to_type->IsType<MidoriType::TypeVariable>();
	if (!uses_convertable && !has_type_variable)
	{
		return std::nullopt;
	}

	const std::vector<ResolvedMethodCandidate>* candidates = FindCandidates(resolutions, CONVERTABLE_CLASS_NAME, CONVERT_METHOD_NAME);
	if (candidates != nullptr)
	{
		const std::string from_type_name = concrete_from_type->ToString();
		const std::string to_type_name = to_type->ToString();
		const std::vector<ResolvedMethodCandidate>::const_iterator found = std::ranges::find_if(*candidates, [&](const ResolvedMethodCandidate& candidate)
		{
			return candidate.m_first_type_name == from_type_name && candidate.m_second_type_name == to_type_name && candidate.m_has_instance;
		});
		if (found != candidates->cend())
		{
			return std::optional<std::string>(found->m_resolved_name);
		}
	}

	// A newtype and its representation share a runtime representation. A
	// hand-written instance still wins; this only spares the derived one,
	// which has no method body, from being reported missing.
	const bool is_newtype_erased = GenericTypes::IsNewTypeErasedConversion(from_type, to_type);
	if (has_type_variable)
	{
		if (uses_convertable && !is_newtype_erased)
		{
			return std::unexpected(MethodResolutionError(CompilerErrorCode::None, "Cannot resolve Convertable instance for type variables outside of specialization context"));
		}
		return std::nullopt;
	}

	const std::string mangled_name = MidoriType::MangleInstanceMethodName(std::string(CONVERT_METHOD_NAME), std::string(CONVERTABLE_CLASS_NAME), { from_type, to_type });
	if (m_is_module_global(mangled_name))
	{
		return std::optional<std::string>(mangled_name);
	}

	const MethodResolution<std::optional<std::string>> matched = MatchSingleInstance(std::string(CONVERTABLE_CLASS_NAME), std::string(CONVERT_METHOD_NAME), [&](const std::vector<TypeRef>& candidate_args, GenericTypes::TypeEnvironment& substitutions)
	{
		VisitedPairs visited;
		return candidate_args.size() == 2u && MatchInstanceTypeArg(candidate_args[0u], from_type, substitutions, visited) && MatchInstanceTypeArg(candidate_args[1u], to_type, substitutions, visited);
	}, std::format("Convertable instance method resolution is ambiguous for types '{}' -> '{}'", from_type->DisplayString(), to_type->DisplayString()));
	if (!matched.has_value() || matched.value().has_value())
	{
		return matched;
	}
	if (uses_convertable && !is_newtype_erased)
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::None, std::format("Convertable instance method '{}' not found", mangled_name)));
	}
	return std::nullopt;
}

MethodResolution<std::string> InstanceResolver::ResolveIterableNext(const MethodResolutionMap& resolutions, const std::shared_ptr<MidoriType>& iterator_type, const std::shared_ptr<MidoriType>& item_type) const
{
	const std::vector<ResolvedMethodCandidate>* candidates = FindCandidates(resolutions, ITERABLE_CLASS_NAME, NEXT_METHOD_NAME);
	if (candidates != nullptr)
	{
		const std::string iterator_name = iterator_type->ToString();
		const std::string item_name = item_type->ToString();
		const std::vector<ResolvedMethodCandidate>::const_iterator found = std::ranges::find_if(*candidates, [&](const ResolvedMethodCandidate& candidate)
		{
			return candidate.m_first_type_name == iterator_name && (candidate.m_second_type_name.empty() || candidate.m_second_type_name == item_name);
		});
		if (found != candidates->cend())
		{
			if (!found->m_has_instance)
			{
				return std::unexpected(MethodResolutionError(CompilerErrorCode::None, std::format("Unresolved Iterable::Next instance for iterator type '{}'", iterator_name)));
			}
			return found->m_resolved_name;
		}
	}

	if (iterator_type->IsType<MidoriType::TypeVariable>() || item_type->IsType<MidoriType::TypeVariable>())
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::None, "Cannot resolve Iterable instance for type variables outside of specialization context"));
	}

	const MethodResolution<std::optional<std::string>> matched = MatchSingleInstance(std::string(ITERABLE_CLASS_NAME), std::string(NEXT_METHOD_NAME), [&](const std::vector<TypeRef>& candidate_args, GenericTypes::TypeEnvironment& substitutions)
	{
		VisitedPairs visited;
		return !candidate_args.empty() && MatchInstanceTypeArg(candidate_args[0u], iterator_type, substitutions, visited) && (candidate_args.size() == 1u || MatchInstanceTypeArg(candidate_args[1u], item_type, substitutions, visited));
	}, std::format("Iterable instance method resolution is ambiguous for iterator type '{}'", iterator_type->DisplayString()));
	if (!matched.has_value())
	{
		return std::unexpected(matched.error());
	}
	if (!matched.value().has_value())
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::None, std::format("Iterable::Next instance for iterator type '{}' not found", iterator_type->DisplayString())));
	}
	return matched.value().value();
}
