#include "InstanceResolver.h"
#include "Compiler/Names/SpecialNames.h"
#include "Compiler/TypeMatching/InstanceTypeMatcher.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

namespace
{
	using TypeRef = std::shared_ptr<MarmotType>;

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

ResolvedInstanceName::ResolvedInstanceName(std::string symbol, std::string declaring_module)
	: m_symbol(std::move(symbol)),
	m_declaring_module(std::move(declaring_module))
{
}

ResolvedMethodCandidate::ResolvedMethodCandidate(std::string first_type_name, std::string second_type_name, std::optional<ResolvedInstanceName> resolved_name)
	: m_first_type_name(std::move(first_type_name)),
	m_second_type_name(std::move(second_type_name)),
	m_resolved_name(std::move(resolved_name))
{
}

InstanceResolver::InstanceResolver(TypeclassMetadataMap classes)
	: m_classes(std::move(classes))
{
}

std::optional<std::shared_ptr<MarmotType>> InstanceResolver::ResolveAssociatedType(const MarmotType::AssociatedType& projection) const
{
	const TypeclassMetadataMap::const_iterator metadata = m_classes.find(projection.m_class_name);
	if (metadata == m_classes.cend())
	{
		return std::nullopt;
	}
	for (const TypeclassInstance& instance : metadata->second.m_instances)
	{
		InstanceTypeMatcher matcher;
		const TypeclassInstance::AssociatedTypes::const_iterator binding = instance.m_associated_type_bindings.find(projection.m_name);
		if (binding != instance.m_associated_type_bindings.cend() && matcher.MatchAll(instance.m_type_args, projection.m_type_args))
		{
			return GenericTypes::Substitute(binding->second, matcher.Bindings());
		}
	}
	return std::nullopt;
}

void InstanceResolver::AddClass(const std::string& class_name, std::unordered_set<std::string> method_names)
{
	m_classes[class_name].m_method_names = std::move(method_names);
}

void InstanceResolver::AddInstance(const std::string& class_name, TypeclassInstance instance)
{
	m_classes[class_name].AddInstance(std::move(instance));
}

bool InstanceResolver::IsClassMethod(const std::string& qualified_name) const
{
	const size_t separator = qualified_name.rfind(NameSeparator);
	if (separator == std::string::npos)
	{
		return false;
	}
	const TypeclassMetadataMap::const_iterator methods = m_classes.find(qualified_name.substr(0u, separator));
	return methods != m_classes.cend() && methods->second.m_method_names.contains(qualified_name.substr(separator + NameSeparator.length()));
}

std::optional<ResolvedInstanceName> InstanceResolver::ResolveInstanceName(const std::string& class_name, const std::string& base_name) const
{
	const TypeclassMetadataMap::const_iterator metadata = m_classes.find(class_name);
	if (metadata != m_classes.cend())
	{
		for (const TypeclassInstance& instance : metadata->second.m_instances)
		{
			if (std::ranges::contains(instance.m_methods, base_name))
			{
				return ResolvedInstanceName(base_name, instance.m_declaring_module);
			}
		}
	}
	return std::nullopt;
}

std::optional<ResolvedInstanceName> InstanceResolver::ResolveInstanceNameForTypeArgs(const std::string& class_name, const std::string& method_name, const std::vector<std::shared_ptr<MarmotType>>& concrete_type_args) const
{
	const std::optional<ResolvedInstanceName> exact = ResolveInstanceName(class_name, MarmotType::MangleInstanceMethodName(method_name, class_name, concrete_type_args));
	if (exact.has_value())
	{
		return exact;
	}

	const TypeclassMetadataMap::const_iterator instance_args = m_classes.find(class_name);
	if (instance_args == m_classes.cend())
	{
		return std::nullopt;
	}

	for (const TypeclassInstance& instance : instance_args->second.m_instances)
	{
		const std::vector<TypeRef>& candidate_args = instance.m_type_args;
		InstanceTypeMatcher matcher;
		if (!matcher.MatchAll(candidate_args, concrete_type_args))
		{
			continue;
		}

		const std::optional<ResolvedInstanceName> candidate = ResolveInstanceName(class_name, MarmotType::MangleInstanceMethodName(method_name, class_name, candidate_args));
		if (candidate.has_value())
		{
			return candidate;
		}
	}
	return std::nullopt;
}

MethodResolutionMap InstanceResolver::ResolveConstraints(const std::vector<MarmotType::ClassConstraint>& constraints, const GenericTypes::TypeEnvironment& substitutions) const
{
	MethodResolutionMap resolutions;
	for (const MarmotType::ClassConstraint& constraint : constraints)
	{
		const TypeclassMetadataMap::const_iterator methods = m_classes.find(constraint.m_class_name);
		if (methods == m_classes.cend())
		{
			continue;
		}

		const std::vector<TypeRef> concrete_type_args = constraint.m_type_args
			| std::views::transform([&substitutions](const TypeRef& type_arg) { return GenericTypes::Substitute(type_arg, substitutions); })
			| std::ranges::to<std::vector>();
		const std::string first_type_name = concrete_type_args.empty() ? std::string() : concrete_type_args[0u]->ToString();
		const std::string second_type_name = concrete_type_args.size() > 1u ? concrete_type_args[1u]->ToString() : std::string();

		for (const std::string& method_name : methods->second.m_method_names)
		{
			const std::optional<ResolvedInstanceName> instance = ResolveInstanceNameForTypeArgs(constraint.m_class_name, method_name, concrete_type_args);
			resolutions[QualifiedMethod(constraint.m_class_name, method_name)].emplace_back(first_type_name, second_type_name, instance);
		}
	}
	return resolutions;
}

MethodResolution<std::optional<ResolvedInstanceName>> InstanceResolver::ResolveConstrainedCall(const MethodResolutionMap& resolutions, const std::string& callee_name, const std::shared_ptr<MarmotType>* first_argument_type, const std::shared_ptr<MarmotType>& return_type) const
{
	const std::vector<ResolvedMethodCandidate>& candidates = resolutions.at(callee_name);
	if (first_argument_type == nullptr)
	{
		if (candidates.size() == 1u && candidates[0u].m_resolved_name.has_value())
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
		return std::nullopt;
	}

	// This tiebreaker assumes a class's second type parameter is its return type,
	// which holds for Convertable<From, To> and not in general. Indexable<C, I>
	// violates it: the second parameter is the index type. Dormant for Indexable
	// today because the type checker rejects two constraints on one class that
	// differ only in the second parameter.
	if (matching.size() > 1u && GenericTypes::IsConcrete(return_type))
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
	if (!selected.m_resolved_name.has_value())
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

MethodResolution<ResolvedInstanceName> InstanceResolver::ResolveConstrainedValue(const MethodResolutionMap& resolutions, const std::string& name) const
{
	const std::vector<ResolvedMethodCandidate>& candidates = resolutions.at(name);
	if (candidates.size() != 1u)
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::LoweringAmbiguousMethodResolution, std::format("Ambiguous method '{}': cannot use method value when multiple class constraints are in scope.", name)));
	}
	if (!candidates[0u].m_resolved_name.has_value())
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::LoweringUnresolvedMethodResolution, std::format("Unresolved method '{}': no matching instance found.", name)));
	}
	return candidates[0u].m_resolved_name.value();
}

MethodResolution<std::optional<ResolvedInstanceName>> InstanceResolver::ResolveConcreteCall(const std::string& callee_name, const std::vector<std::shared_ptr<MarmotType>>& argument_types, const std::shared_ptr<MarmotType>& return_type) const
{
	if (!IsClassMethod(callee_name))
	{
		return std::nullopt;
	}

	const size_t separator = callee_name.rfind(NameSeparator);
	const std::string qualifier = callee_name.substr(0u, separator);
	const std::string method_name = callee_name.substr(separator + NameSeparator.length());

	const TypeclassMetadataMap::const_iterator instance_args = m_classes.find(qualifier);
	if (instance_args == m_classes.cend())
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::LoweringUnresolvedMethodResolution, std::format("Unresolved method '{}': no instance metadata found.", callee_name)));
	}

	std::vector<ResolvedMethodCandidate> candidates;
	for (const TypeclassInstance& instance : instance_args->second.m_instances)
	{
		const std::vector<TypeRef>& candidate_args = instance.m_type_args;
		if (candidate_args.empty())
		{
			continue;
		}

		InstanceTypeMatcher matcher;
		bool matched = true;
		if (candidate_args.size() == 1u)
		{
			matched = std::ranges::all_of(argument_types, [&](const TypeRef& argument_type) { return matcher.Match(candidate_args[0u], argument_type); });
		}
		else
		{
			const size_t positional_count = std::min(candidate_args.size(), argument_types.size());
			for (size_t index = 0u; index < positional_count && matched; index += 1u)
			{
				matched = matcher.Match(candidate_args[index], argument_types[index]);
			}

			if (matched && argument_types.size() + 1u == candidate_args.size())
			{
				matched = matcher.Match(candidate_args.back(), return_type);
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

		const std::string mangled_name = MarmotType::MangleInstanceMethodName(method_name, qualifier, candidate_args);
		const std::optional<ResolvedInstanceName> resolved_name = ResolveInstanceName(qualifier, mangled_name);
		candidates.emplace_back(candidate_args[0u]->ToString(), candidate_args.size() > 1u ? candidate_args[1u]->ToString() : std::string(), resolved_name);
	}

	if (candidates.empty())
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::LoweringUnresolvedMethodResolution, std::format("Unresolved method '{}': no matching concrete instance found.", callee_name)));
	}
	if (candidates.size() != 1u)
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::LoweringAmbiguousMethodResolution, std::format("Ambiguous method '{}': multiple concrete instances match this call.", callee_name)));
	}
	if (!candidates[0u].m_resolved_name.has_value())
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::LoweringUnresolvedMethodResolution, std::format("Unresolved method '{}': no emitted instance method was found.", callee_name)));
	}
	return candidates[0u].m_resolved_name;
}

MethodResolution<std::optional<ResolvedInstanceName>> InstanceResolver::MatchSingleInstance(const std::string& class_name, const std::string& method_name, const std::function<bool(const std::vector<std::shared_ptr<MarmotType>>&)>& matches, const std::string& ambiguity_message) const
{
	const TypeclassMetadataMap::const_iterator instance_args = m_classes.find(class_name);
	if (instance_args == m_classes.cend())
	{
		return std::nullopt;
	}

	std::optional<ResolvedInstanceName> resolved;
	for (const TypeclassInstance& instance : instance_args->second.m_instances)
	{
		const std::vector<TypeRef>& candidate_args = instance.m_type_args;
		if (!matches(candidate_args))
		{
			continue;
		}

		std::optional<ResolvedInstanceName> candidate = ResolveInstanceName(class_name, MarmotType::MangleInstanceMethodName(method_name, class_name, candidate_args));
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

MethodResolution<ResolvedInstanceName> InstanceResolver::ResolveConcat(const MethodResolutionMap& resolutions, const std::shared_ptr<MarmotType>& operand_type) const
{
	std::optional<ResolvedInstanceName> resolved;
	const std::vector<ResolvedMethodCandidate>* candidates = FindCandidates(resolutions, CONCATENABLE_CLASS_NAME, CONCAT_METHOD_NAME);
	if (candidates != nullptr)
	{
		const std::string operand_type_name = operand_type->ToString();
		for (const ResolvedMethodCandidate& candidate : *candidates)
		{
			if (candidate.m_first_type_name != operand_type_name || !candidate.m_resolved_name.has_value())
			{
				continue;
			}
			if (resolved.has_value() && resolved.value() != candidate.m_resolved_name.value())
			{
				return std::unexpected(MethodResolutionError(CompilerErrorCode::None, std::format("Concatenable instance method resolution is ambiguous for type '{}'", operand_type_name)));
			}
			resolved = candidate.m_resolved_name;
		}
	}

	if (!resolved.has_value())
	{
		const MethodResolution<std::optional<ResolvedInstanceName>> matched = MatchSingleInstance(std::string(CONCATENABLE_CLASS_NAME), std::string(CONCAT_METHOD_NAME), [&operand_type](const std::vector<TypeRef>& candidate_args)
		{
			InstanceTypeMatcher matcher;
			return candidate_args.size() == 1u && matcher.Match(candidate_args[0u], operand_type);
		}, std::format("Concatenable instance method resolution is ambiguous for type '{}'", operand_type->DisplayString()));
		if (!matched.has_value())
		{
			return std::unexpected(matched.error());
		}
		resolved = matched.value();
	}

	if (!resolved.has_value())
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::None, std::format("Concatenable instance method '{}' not found", MarmotType::MangleInstanceMethodName(std::string(CONCAT_METHOD_NAME), std::string(CONCATENABLE_CLASS_NAME), { operand_type }))));
	}
	return resolved.value();
}

MethodResolution<ResolvedInstanceName> InstanceResolver::ResolveEquals(const std::shared_ptr<MarmotType>& operand_type) const
{
	return ResolveOperandInstance(std::string(EQUATABLE_CLASS_NAME), std::string(EQUALS_METHOD_NAME), operand_type);
}

MethodResolution<ResolvedInstanceName> InstanceResolver::ResolveCompare(const std::shared_ptr<MarmotType>& operand_type) const
{
	return ResolveOperandInstance(std::string(ORDERABLE_CLASS_NAME), std::string(COMPARE_METHOD_NAME), operand_type);
}

// The instance an operator on one operand type calls: the one declared for
// exactly that type, or else the single generic instance that matches it.
MethodResolution<ResolvedInstanceName> InstanceResolver::ResolveOperandInstance(const std::string& class_name, const std::string& method_name, const std::shared_ptr<MarmotType>& operand_type) const
{
	const std::string mangled_name = MarmotType::MangleInstanceMethodName(method_name, class_name, { operand_type });
	const std::optional<ResolvedInstanceName> exact = ResolveInstanceName(class_name, mangled_name);
	if (exact.has_value())
	{
		return exact.value();
	}

	const MethodResolution<std::optional<ResolvedInstanceName>> matched = MatchSingleInstance(class_name, method_name, [&operand_type](const std::vector<TypeRef>& candidate_args)
	{
		InstanceTypeMatcher matcher;
		return candidate_args.size() == 1u && matcher.Match(candidate_args[0u], operand_type);
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

MethodResolution<std::optional<ResolvedInstanceName>> InstanceResolver::ResolveCount(const MethodResolutionMap& resolutions, const std::shared_ptr<MarmotType>& operand_type, bool uses_countable) const
{
	const std::vector<ResolvedMethodCandidate>* candidates = FindCandidates(resolutions, COUNTABLE_CLASS_NAME, COUNT_METHOD_NAME);
	if (candidates != nullptr)
	{
		const std::string type_name = operand_type->ToString();
		const std::vector<ResolvedMethodCandidate>::const_iterator found = std::ranges::find_if(*candidates, [&](const ResolvedMethodCandidate& candidate) { return candidate.m_first_type_name == type_name && candidate.m_resolved_name.has_value(); });
		if (found != candidates->cend())
		{
			return found->m_resolved_name;
		}
	}

	if (operand_type->IsType<MarmotType::TypeVariable>())
	{
		if (uses_countable)
		{
			return std::unexpected(MethodResolutionError(CompilerErrorCode::None, "Cannot resolve Countable instance for type variables outside of specialization context"));
		}
		return std::nullopt;
	}

	const std::optional<ResolvedInstanceName> resolved = ResolveInstanceNameForTypeArgs(std::string(COUNTABLE_CLASS_NAME), std::string(COUNT_METHOD_NAME), { operand_type });
	if (resolved.has_value())
	{
		return resolved;
	}
	if (uses_countable)
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::None, std::format("Countable instance method '{}' not found", MarmotType::MangleInstanceMethodName(std::string(COUNT_METHOD_NAME), std::string(COUNTABLE_CLASS_NAME), { operand_type }))));
	}
	return std::nullopt;
}

MethodResolution<ResolvedInstanceName> InstanceResolver::ResolveIndex(const MethodResolutionMap& resolutions, const std::shared_ptr<MarmotType>& container_type, const std::shared_ptr<MarmotType>& index_type) const
{
	const std::vector<ResolvedMethodCandidate>* candidates = FindCandidates(resolutions, INDEXABLE_CLASS_NAME, GET_METHOD_NAME);
	if (candidates != nullptr)
	{
		const std::string container_type_name = container_type->ToString();
		const std::string index_type_name = index_type->ToString();
		const std::vector<ResolvedMethodCandidate>::const_iterator found = std::ranges::find_if(*candidates, [&](const ResolvedMethodCandidate& candidate)
		{
			return candidate.m_first_type_name == container_type_name && candidate.m_second_type_name == index_type_name && candidate.m_resolved_name.has_value();
		});
		if (found != candidates->cend())
		{
			return found->m_resolved_name.value();
		}
	}

	if (container_type->IsType<MarmotType::TypeVariable>())
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::None, "Cannot resolve Indexable instance for type variables outside of specialization context"));
	}

	const std::optional<ResolvedInstanceName> resolved = ResolveInstanceNameForTypeArgs(std::string(INDEXABLE_CLASS_NAME), std::string(GET_METHOD_NAME), { container_type, index_type });
	if (resolved.has_value())
	{
		return resolved.value();
	}
	return std::unexpected(MethodResolutionError(CompilerErrorCode::None, std::format("Indexable instance method '{}' not found", MarmotType::MangleInstanceMethodName(std::string(GET_METHOD_NAME), std::string(INDEXABLE_CLASS_NAME), { container_type }))));
}

MethodResolution<std::optional<ResolvedInstanceName>> InstanceResolver::ResolveConvert(const MethodResolutionMap& resolutions, const std::shared_ptr<MarmotType>& concrete_from_type, const std::shared_ptr<MarmotType>& from_type, const std::shared_ptr<MarmotType>& to_type, bool uses_convertable) const
{
	const bool has_type_variable = from_type->IsType<MarmotType::TypeVariable>() || to_type->IsType<MarmotType::TypeVariable>();
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
			return candidate.m_first_type_name == from_type_name && candidate.m_second_type_name == to_type_name && candidate.m_resolved_name.has_value();
		});
		if (found != candidates->cend())
		{
			return found->m_resolved_name;
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

	const std::string mangled_name = MarmotType::MangleInstanceMethodName(std::string(CONVERT_METHOD_NAME), std::string(CONVERTABLE_CLASS_NAME), { from_type, to_type });
	const std::optional<ResolvedInstanceName> exact = ResolveInstanceName(std::string(CONVERTABLE_CLASS_NAME), mangled_name);
	if (exact.has_value())
	{
		return exact;
	}

	const MethodResolution<std::optional<ResolvedInstanceName>> matched = MatchSingleInstance(std::string(CONVERTABLE_CLASS_NAME), std::string(CONVERT_METHOD_NAME), [&](const std::vector<TypeRef>& candidate_args)
	{
		InstanceTypeMatcher matcher;
		return candidate_args.size() == 2u && matcher.Match(candidate_args[0u], from_type) && matcher.Match(candidate_args[1u], to_type);
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

MethodResolution<ResolvedInstanceName> InstanceResolver::ResolveIterableNext(const MethodResolutionMap& resolutions, const std::shared_ptr<MarmotType>& iterator_type, const std::shared_ptr<MarmotType>& item_type) const
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
			if (!found->m_resolved_name.has_value())
			{
				return std::unexpected(MethodResolutionError(CompilerErrorCode::None, std::format("Unresolved Iterable::Next instance for iterator type '{}'", iterator_name)));
			}
			return found->m_resolved_name.value();
		}
	}

	if (iterator_type->IsType<MarmotType::TypeVariable>() || item_type->IsType<MarmotType::TypeVariable>())
	{
		return std::unexpected(MethodResolutionError(CompilerErrorCode::None, "Cannot resolve Iterable instance for type variables outside of specialization context"));
	}

	const MethodResolution<std::optional<ResolvedInstanceName>> matched = MatchSingleInstance(std::string(ITERABLE_CLASS_NAME), std::string(NEXT_METHOD_NAME), [&](const std::vector<TypeRef>& candidate_args)
	{
		InstanceTypeMatcher matcher;
		return !candidate_args.empty() && matcher.Match(candidate_args[0u], iterator_type) && (candidate_args.size() == 1u || matcher.Match(candidate_args[1u], item_type));
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
