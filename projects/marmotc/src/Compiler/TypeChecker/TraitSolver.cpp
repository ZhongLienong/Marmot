#include "TypeChecker.h"
#include "Compiler/Constant/Constant.h"
#include "Compiler/Names/SpecialNames.h"
#include "Compiler/TypeMatching/InstanceTypeMatcher.h"
#include <algorithm>
#include <cassert>
#include <format>
#include <iterator>
#include <ranges>

using namespace std::string_literals;

namespace
{
	// Two types as a person reads them. The module comes back only when the
	// short names are the same, which is the one case where a short name says
	// nothing.
	std::pair<std::string, std::string> DescribeTypePair(const MarmotType& first, const MarmotType& second)
	{
		std::string first_name = first.DisplayString();
		std::string second_name = second.DisplayString();
		if (first_name == second_name)
		{
			return { first.ToString(), second.ToString() };
		}

		return { std::move(first_name), std::move(second_name) };
	}

	void ForEachType(const std::shared_ptr<MarmotType>& type, const std::function<void(const MarmotType&)>& visit, std::unordered_set<const MarmotType*>& visited)
	{
		if (type == nullptr || !visited.insert(type.get()).second)
		{
			return;
		}

		visit(*type);
		TypeTransform::ForEachChild(*type, [&visit, &visited](const std::shared_ptr<MarmotType>& component) { ForEachType(component, visit, visited); });
	}

	bool ContainsType(const std::shared_ptr<MarmotType>& type, const std::function<bool(const MarmotType&)>& is_wanted, std::unordered_set<const MarmotType*>& visited)
	{
		if (type == nullptr || !visited.insert(type.get()).second)
		{
			return false;
		}

		if (is_wanted(*type))
		{
			return true;
		}

		bool found = false;
		TypeTransform::ForEachChild(*type, [&found, &is_wanted, &visited](const std::shared_ptr<MarmotType>& component) { found = found || ContainsType(component, is_wanted, visited); });
		return found;
	}

	std::unordered_set<std::string> CollectGenericParamNames(const std::vector<std::shared_ptr<MarmotType>>& types)
	{
		std::unordered_set<std::string> names;
		std::unordered_set<const MarmotType*> visited;
		for (const std::shared_ptr<MarmotType>& type : types)
		{
			ForEachType
			(
				type,
				[&names](const MarmotType& node)
				{
					if (node.IsType<MarmotType::GenericParam>())
					{
						names.insert(node.GetType<MarmotType::GenericParam>().m_name);
					}
				},
				visited
			);
		}
		return names;
	}

	bool ContainsConstraint(const std::vector<MarmotType::ClassConstraint>& constraints, const MarmotType::ClassConstraint& constraint)
	{
		return std::ranges::any_of
		(
			constraints,
			[&constraint](const MarmotType::ClassConstraint& existing)
			{
				return existing == constraint;
			}
		);
	}

	std::string JoinSortedNames(std::vector<std::string> names)
	{
		std::ranges::sort(names);
		return names | std::views::join_with(std::string_view(", ")) | std::ranges::to<std::string>();
	}
}

bool TypeChecker::HasTypeVariables(const std::shared_ptr<MarmotType>& type)
{
	std::unordered_set<const MarmotType*> visited;
	return ContainsType(type, [](const MarmotType& node) { return node.IsType<MarmotType::TypeVariable>(); }, visited);
}

std::unordered_set<int> TypeChecker::CollectTypeVariableIds(const std::shared_ptr<MarmotType>& type)
{
	std::unordered_set<int> type_variable_ids;
	std::unordered_set<const MarmotType*> visited;
	ForEachType
	(
		type,
		[&type_variable_ids](const MarmotType& node)
		{
			if (node.IsType<MarmotType::TypeVariable>())
			{
				type_variable_ids.insert(node.GetType<MarmotType::TypeVariable>().m_id);
			}
		},
		visited
	);
	return type_variable_ids;
}

// A class constraint names its class directly; an equality constraint names it
// through the projection on its left side. Both must refer to a declared class,
// so validation sites ask for the name rather than reading m_class_name, which
// is empty for an equality.
const std::string& TypeChecker::ConstrainedClassName(const MarmotType::ClassConstraint& constraint)
{
	if (constraint.IsEquality() && constraint.m_equality_lhs->IsType<MarmotType::AssociatedType>())
	{
		return constraint.m_equality_lhs->GetType<MarmotType::AssociatedType>().m_class_name;
	}

	return constraint.m_class_name;
}

void TypeChecker::AppendUniqueConstraint(std::vector<MarmotType::ClassConstraint>& constraints, MarmotType::ClassConstraint&& constraint)
{
	if (!ContainsConstraint(constraints, constraint))
	{
		constraints.push_back(std::move(constraint));
	}
}

std::vector<MarmotType::ClassConstraint> TypeChecker::CollectSignatureConstraints(
	const std::vector<std::shared_ptr<MarmotType>>& param_types,
	const std::shared_ptr<MarmotType>& return_type
)
{
	std::vector<MarmotType::ClassConstraint> constraints;
	std::unordered_set<const MarmotType*> visited;
	const std::function<void(const MarmotType&)> collect = [&constraints](const MarmotType& node)
	{
		const std::vector<MarmotType::ClassConstraint>* node_constraints = node.IsType<MarmotType::FunctionType>() ? &node.GetType<MarmotType::FunctionType>().m_constraints
			: node.IsType<MarmotType::StructType>() ? &node.GetType<MarmotType::StructType>().m_constraints
			: node.IsType<MarmotType::UnionType>() ? &node.GetType<MarmotType::UnionType>().m_constraints
			: nullptr;
		if (node_constraints == nullptr)
		{
			return;
		}

		for (const MarmotType::ClassConstraint& constraint : *node_constraints)
		{
			AppendUniqueConstraint(constraints, MarmotType::ClassConstraint(constraint));
		}
	};

	for (const std::shared_ptr<MarmotType>& param_type : param_types)
	{
		ForEachType(param_type, collect, visited);
	}
	ForEachType(return_type, collect, visited);
	return constraints;
}

class NominalUnifyGuard
{
private:
	TypeChecker& m_type_checker;
	std::string m_name;
	bool m_entered;

public:
	NominalUnifyGuard(TypeChecker& tc, const std::string& name)
		: m_type_checker(tc), m_name(name), m_entered(tc.m_unify_nominal_active.insert(name).second)
	{
	}

	~NominalUnifyGuard()
	{
		if (m_entered)
		{
			m_type_checker.m_unify_nominal_active.erase(m_name);
		}
	}

	bool Entered() const { return m_entered; }

	NominalUnifyGuard(const NominalUnifyGuard&) = delete;
	NominalUnifyGuard& operator=(const NominalUnifyGuard&) = delete;
};

std::size_t TypePairHash::operator()(const std::pair<MarmotType*, MarmotType*>& pair) const noexcept
{
	std::size_t h1 = std::hash<MarmotType*>{}(pair.first);
	std::size_t h2 = std::hash<MarmotType*>{}(pair.second);
	return h1 ^ (h2 * 0x9e3779b97f4a7c15 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
}

std::string TypeChecker::DescribeConstraint(const CheckingContext& context, const MarmotType::ClassConstraint& constraint) const
{
	if (constraint.IsEquality())
	{
		return constraint.m_equality_lhs->DisplayString(context.m_assumptions->m_rigid_type_vars) + " ~ "s + constraint.m_equality_rhs->DisplayString(context.m_assumptions->m_rigid_type_vars);
	}

	if (constraint.m_type_args.empty())
	{
		return constraint.m_class_name;
	}

	const std::string type_args = constraint.m_type_args
		| std::views::transform([&context](const std::shared_ptr<MarmotType>& type_arg) { return type_arg->DisplayString(context.m_assumptions->m_rigid_type_vars); })
		| std::views::join_with(std::string_view(", "))
		| std::ranges::to<std::string>();
	return std::format("{}<{}>", constraint.m_class_name, type_args);
}

CompilerError TypeChecker::MakeConstraintFailureError(const CheckingContext& context, const Token& token, const MarmotType::ClassConstraint& constraint, std::optional<std::string_view> suggestion) const
{
	std::string message;
	if (!constraint.m_type_args.empty())
	{
		message = std::format
		(
			"Type {} does not satisfy constraint {} - no matching instance found",
			constraint.m_type_args[0u]->DisplayString(context.m_assumptions->m_rigid_type_vars),
			DescribeConstraint(context, constraint)
		);
	}
	else
	{
		message = std::format("Constraint {} is not satisfied - no matching instance found", DescribeConstraint(context, constraint));
	}

	return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnsatisfiedConstraint, message, token, m_file_name, m_source_lines, suggestion);
}

bool TypeChecker::HasActiveConstraint(const CheckingContext& context, const std::string& class_name, const std::shared_ptr<MarmotType>& type)
{
	std::shared_ptr<MarmotType> resolved_type = ApplySubstitution(context, type);

	for (const MarmotType::ClassConstraint& constraint : context.m_assumptions->m_constraints)
	{
		if (constraint.m_class_name != class_name || constraint.m_type_args.size() != 1u)
		{
			continue;
		}

		std::shared_ptr<MarmotType> resolved_constraint_type = ApplySubstitution(context, constraint.m_type_args[0u]);
		if (*resolved_constraint_type == *resolved_type)
		{
			return true;
		}
	}

	return false;
}

std::optional<CompilerError> TypeChecker::EnsureTransferable(const CheckingContext& context, const Token& token, const std::shared_ptr<MarmotType>& type)
{
	std::unordered_set<std::shared_ptr<MarmotType>> visited;
	return EnsureTransferable(context, token, type, visited);
}

std::optional<CompilerError> TypeChecker::EnsureTransferable(const CheckingContext& context, const Token& token, const std::shared_ptr<MarmotType>& type, std::unordered_set<std::shared_ptr<MarmotType>>& visited)
{
	const std::shared_ptr<MarmotType> resolved_type = ApplySubstitution(context, type);
	if (!visited.insert(resolved_type).second)
	{
		return std::nullopt;
	}

	const MarmotType::ClassConstraint transferable_constraint(std::string(TRANSFERABLE_CLASS_NAME), { resolved_type });

	const auto fail = [&](std::optional<std::string_view> suggestion = std::nullopt) -> std::optional<CompilerError>
	{
		return MakeConstraintFailureError(context, token, transferable_constraint, suggestion);
	};

	const auto has_transferable_marker = [&]() -> bool
	{
		return HasActiveConstraint(context, std::string(TRANSFERABLE_CLASS_NAME), resolved_type)
			|| FindMatchingInstance(std::string(TRANSFERABLE_CLASS_NAME), { resolved_type }).has_value();
	};

	if (
		resolved_type->IsType<MarmotType::IntegerType>() ||
		resolved_type->IsType<MarmotType::FloatType>() ||
		resolved_type->IsType<MarmotType::ByteType>() ||
		resolved_type->IsType<MarmotType::WordType>() ||
		resolved_type->IsType<MarmotType::BoolType>() ||
		resolved_type->IsType<MarmotType::TextType>() ||
		resolved_type->IsType<MarmotType::UnitType>() ||
		resolved_type->IsType<MarmotType::NeverType>()
	)
	{
		return std::nullopt;
	}

	if (resolved_type->IsType<MarmotType::ArrayType>())
	{
		return EnsureTransferable(context, token, resolved_type->GetType<MarmotType::ArrayType>().m_element_type, visited);
	}

	if (resolved_type->IsType<MarmotType::ChannelType>())
	{
		return EnsureTransferable(context, token, resolved_type->GetType<MarmotType::ChannelType>().m_element_type, visited);
	}

	if (resolved_type->IsType<MarmotType::CellType>())
	{
		return fail("Send the referenced value or create a new reference in the worker.");
	}

	if (resolved_type->IsType<MarmotType::TupleType>())
	{
		for (const std::shared_ptr<MarmotType>& element_type : resolved_type->GetType<MarmotType::TupleType>().m_element_types)
		{
			if (std::optional<CompilerError> error = EnsureTransferable(context, token, element_type, visited))
			{
				return error;
			}
		}
		return std::nullopt;
	}

	if (resolved_type->IsType<MarmotType::WorkerType>())
	{
		return fail("Worker values cannot cross worker boundaries.");
	}

	if (resolved_type->IsType<MarmotType::RangeType>())
	{
		return fail("Range values are not transferable.");
	}

	if (resolved_type->IsType<MarmotType::FunctionType>())
	{
		return resolved_type->GetType<MarmotType::FunctionType>().m_is_transferable
			? std::nullopt
			: fail("A function crossing a worker boundary must have transferable captures. Use a Transferable constraint for callback parameters.");
	}

	if (resolved_type->IsType<MarmotType::GenericParam>() ||
		resolved_type->IsType<MarmotType::TypeVariable>() ||
		resolved_type->IsType<MarmotType::AssociatedType>())
	{
		return has_transferable_marker()
			? std::nullopt
			: fail("Add a 'where Transferable<T>' constraint or import an instance.");
	}

	if (resolved_type->IsType<MarmotType::StructType>())
	{
		if (!has_transferable_marker())
		{
			return fail("Add 'deriving (Transferable)' or define an instance.");
		}

		for (const std::shared_ptr<MarmotType>& member_type : resolved_type->GetType<MarmotType::StructType>().m_member_types)
		{
			if (std::optional<CompilerError> error = EnsureTransferable(context, token, member_type, visited))
			{
				return error;
			}
		}
		return std::nullopt;
	}

	if (resolved_type->IsType<MarmotType::UnionType>())
	{
		if (!has_transferable_marker())
		{
			return fail("Add 'deriving (Transferable)' or define an instance.");
		}

		for (const auto& [_, member_ctx] : resolved_type->GetType<MarmotType::UnionType>().m_member_info)
		{
			for (const std::shared_ptr<MarmotType>& member_type : member_ctx.m_member_types)
			{
				if (std::optional<CompilerError> error = EnsureTransferable(context, token, member_type, visited))
				{
					return error;
				}
			}
		}
		return std::nullopt;
	}

	if (resolved_type->IsType<MarmotType::NewType>())
	{
		return has_transferable_marker()
			? EnsureTransferable(context, token, resolved_type->GetType<MarmotType::NewType>().m_representation, visited)
			: fail();
	}

	return has_transferable_marker()
		? std::nullopt
		: fail();
}

CompilerError TypeChecker::MakeFunctionArityError(const Token& token, size_t left_count, size_t right_count, UnifyDiagnosticMode diagnostic_mode) const
{
	std::string message;
	if (diagnostic_mode == UnifyDiagnosticMode::ActualExpected)
	{
		message = std::format("Function expects {} argument(s) but got {}", right_count, left_count);
	}
	else if (diagnostic_mode == UnifyDiagnosticMode::ExpectedActual)
	{
		message = std::format("Function expects {} argument(s) but got {}", left_count, right_count);
	}
	else
	{
		message = std::format("Function type mismatch: {} argument(s) in one context but {} in another", left_count, right_count);
	}

	return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, message, token, m_file_name, m_source_lines);
}

CompilerError TypeChecker::MakeTupleArityError(const Token& token, size_t left_count, size_t right_count, UnifyDiagnosticMode diagnostic_mode) const
{
	std::string message;
	if (diagnostic_mode == UnifyDiagnosticMode::ActualExpected)
	{
		message = std::format("Tuple expects {} element(s) but got {}", right_count, left_count);
	}
	else if (diagnostic_mode == UnifyDiagnosticMode::ExpectedActual)
	{
		message = std::format("Tuple expects {} element(s) but got {}", left_count, right_count);
	}
	else
	{
		message = std::format("Tuple type mismatch: {} element(s) in one context but {} in another", left_count, right_count);
	}

	return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, message, token, m_file_name, m_source_lines);
}

std::optional<std::vector<std::pair<std::string, std::shared_ptr<MarmotType>>>> TypeChecker::ResolveGenericTypeArguments(const std::shared_ptr<MarmotType>& prototype, const std::shared_ptr<MarmotType>& concrete_type) const
{
	std::vector<std::string> param_names;
	if (prototype->IsType<MarmotType::StructType>())
	{
		param_names = prototype->GetType<MarmotType::StructType>().m_generic_params;
	}
	else if (prototype->IsType<MarmotType::UnionType>())
	{
		param_names = prototype->GetType<MarmotType::UnionType>().m_generic_params;
	}
	else
	{
		return std::nullopt;
	}

	if (param_names.empty())
	{
		return std::nullopt;
	}

	InstanceTypeMatcher matcher;
	if (!matcher.Match(prototype, concrete_type))
	{
		return std::nullopt;
	}

	const TypeEnvironment substitutions = std::move(matcher).TakeBindings();
	std::vector<std::pair<std::string, std::shared_ptr<MarmotType>>> resolved_args;
	resolved_args.reserve(param_names.size());
	for (const std::string& param_name : param_names)
	{
		std::unordered_map<std::string, std::shared_ptr<MarmotType>>::const_iterator it = substitutions.find(param_name);
		if (it == substitutions.cend())
		{
			return std::nullopt;
		}

		resolved_args.emplace_back(param_name, it->second);
	}

	return resolved_args;
}

std::optional<TypeChecker::ResolvedInstanceMatch> TypeChecker::FindMatchingInstance(const std::string& class_name, const std::vector<std::shared_ptr<MarmotType>>& type_args) const
{
	std::vector<ResolvedInstanceMatch> matches = FindMatchingInstances(class_name, type_args);
	if (matches.size() != 1u)
	{
		return std::nullopt;
	}
	return std::move(matches.front());
}

std::vector<TypeChecker::ResolvedInstanceMatch> TypeChecker::FindMatchingInstances(const std::string& class_name, const std::vector<std::shared_ptr<MarmotType>>& type_args) const
{
	std::vector<ResolvedInstanceMatch> matches;
	for (const auto& [_, instance_info] : m_instances)
	{
		if (instance_info.m_class_name != class_name)
		{
			continue;
		}
		InstanceTypeMatcher matcher;
		if (matcher.MatchAll(instance_info.m_type_args, type_args))
		{
			matches.push_back(ResolvedInstanceMatch{ .m_instance = &instance_info, .m_substitutions = std::move(matcher).TakeBindings() });
		}
	}
	return matches;
}

std::optional<CompilerError> TypeChecker::DischargeEqualityConstraint(const CheckingContext& context, const Token& token, const MarmotType::ClassConstraint& constraint, const std::shared_ptr<MarmotType>& resolved_lhs, const std::shared_ptr<MarmotType>& resolved_rhs)
{
	if (*resolved_lhs == *resolved_rhs)
	{
		return std::nullopt;
	}

	// An equality constraint also *solves*: `Iterable::Item<S> ~ A` with `A` still
	// unbound is what tells the call site what A is. `Iter::ToArray` has no other
	// argument mentioning A, so without this it could never be called.
	if (resolved_lhs->IsType<MarmotType::TypeVariable>() || resolved_rhs->IsType<MarmotType::TypeVariable>())
	{
		std::shared_ptr<MarmotType> lhs = resolved_lhs;
		std::shared_ptr<MarmotType> rhs = resolved_rhs;
		if (Unify(context, token, lhs, rhs).has_value())
		{
			return std::nullopt;
		}
	}

	const std::pair<std::string, std::string> described = DescribeTypePair(*resolved_lhs, *resolved_rhs);
	return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnsatisfiedConstraint, std::format("Equality constraint is not satisfied: '{}' resolves to '{}', not '{}'", constraint.m_equality_lhs->DisplayString(), described.first, described.second), token, m_file_name, m_source_lines);
}

bool TypeChecker::IsSatisfiedByActiveConstraint(const CheckingContext& context, const MarmotType::ClassConstraint& resolved_constraint)
{
	return std::ranges::any_of
	(
		context.m_assumptions->m_constraints,
		[this, &resolved_constraint, &context](const MarmotType::ClassConstraint& active_constraint) -> bool
		{
			if (active_constraint.m_class_name != resolved_constraint.m_class_name || active_constraint.m_type_args.size() != resolved_constraint.m_type_args.size())
			{
				return false;
			}

			return std::ranges::all_of
			(
				std::views::iota(0u, resolved_constraint.m_type_args.size()),
				[this, &active_constraint, &resolved_constraint, &context](size_t idx) -> bool
				{
					return *ApplySubstitution(context, active_constraint.m_type_args[idx]) == *resolved_constraint.m_type_args[idx];
				}
			);
		}
	);
}

// A class constraint holds when a where clause in force states it, or when
// exactly one instance matches and that instance's own where clauses hold.
// Two matching instances are not a missing one, and the error says which.
std::optional<CompilerError> TypeChecker::CheckConstraintHolds(const CheckingContext& context, const Token& token, const MarmotType::ClassConstraint& resolved_constraint, size_t depth, std::optional<std::string_view> suggestion)
{
	if (resolved_constraint.m_class_name == TRANSFERABLE_CLASS_NAME)
	{
		return EnsureTransferable(context, token, resolved_constraint.m_type_args.front());
	}

	if (IsSatisfiedByActiveConstraint(context, resolved_constraint))
	{
		return std::nullopt;
	}

	std::vector<ResolvedInstanceMatch> matches = FindMatchingInstances(resolved_constraint.m_class_name, resolved_constraint.m_type_args);
	if (matches.empty())
	{
		return MakeConstraintFailureError(context, token, resolved_constraint, suggestion);
	}
	if (matches.size() > 1u)
	{
		const std::vector<std::string> candidates = matches
			| std::views::transform([this, &context](const ResolvedInstanceMatch& match) { return std::format("'{}'", DescribeConstraint(context, MarmotType::ClassConstraint(match.m_instance->m_class_name, std::vector<std::shared_ptr<MarmotType>>(match.m_instance->m_type_args)))); })
			| std::ranges::to<std::vector>();
		return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnsatisfiedConstraint, std::format("Constraint {} is ambiguous: {} instances match it ({})", DescribeConstraint(context, resolved_constraint), matches.size(), JoinSortedNames(candidates)), token, m_file_name, m_source_lines);
	}

	if (depth >= s_max_instance_constraint_depth)
	{
		return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnsatisfiedConstraint, std::format("Constraint {} could not be decided: its instances' where clauses nest more than {} deep", DescribeConstraint(context, resolved_constraint), s_max_instance_constraint_depth), token, m_file_name, m_source_lines);
	}

	MarmotResult::TypeResult nested_result = ValidateInstanceConstraints(context, token, *matches.front().m_instance, matches.front().m_substitutions, depth + 1u);
	if (!nested_result.has_value())
	{
		return std::move(nested_result.error());
	}
	return std::nullopt;
}

MarmotResult::TypeResult TypeChecker::ValidateFunctionConstraints(const CheckingContext& context, const Token& token, const MarmotType::FunctionType& function_type)
{
	for (const MarmotType::ClassConstraint& constraint : function_type.m_constraints)
	{
		// An equality constraint is discharged by reduction, not by instance
		// selection: resolve both sides at the call site and compare them.
		if (constraint.IsEquality())
		{
			std::shared_ptr<MarmotType> resolved_equality_lhs = ApplySubstitution(context, constraint.m_equality_lhs);
			std::shared_ptr<MarmotType> resolved_equality_rhs = ApplySubstitution(context, constraint.m_equality_rhs);
			if (std::optional<CompilerError> error = DischargeEqualityConstraint(context, token, constraint, resolved_equality_lhs, resolved_equality_rhs))
			{
				return std::unexpected(std::move(*error));
			}

			continue;
		}
		std::vector<std::shared_ptr<MarmotType>> resolved_type_args;
		resolved_type_args.reserve(constraint.m_type_args.size());
		for (const std::shared_ptr<MarmotType>& type_arg : constraint.m_type_args)
		{
			resolved_type_args.emplace_back(ApplySubstitution(context, type_arg));
		}

		// No instance is picked for a type nothing has decided yet.
		if (!std::ranges::all_of(resolved_type_args, [this, &context](const std::shared_ptr<MarmotType>& type_arg) { return IsDecided(context, type_arg); }))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnsatisfiedConstraint, std::format("Call expression type error: could not infer the type this call's {} constraint is on; write the type on the definition that holds the result", constraint.m_class_name), token, m_file_name, m_source_lines));
		}

		if (std::optional<CompilerError> error = CheckConstraintHolds(context, token, MarmotType::ClassConstraint(constraint.m_class_name, std::move(resolved_type_args)), 0u))
		{
			return std::unexpected(std::move(*error));
		}
	}

	return MarmotType::MakeUndecidedType();
}

MarmotResult::TypeResult TypeChecker::ValidateInstanceConstraints(const CheckingContext& context, const Token& token, const InstanceInfo& instance_info, const TypeEnvironment& substitutions, size_t depth)
{
	for (const MarmotType::ClassConstraint& constraint : instance_info.m_constraints)
	{
		// An equality constraint is discharged by reduction, not by instance
		// selection: resolve both sides at the call site and compare them.
		if (constraint.IsEquality())
		{
			std::shared_ptr<MarmotType> resolved_equality_lhs = ApplySubstitution(context, TypeTransform::Instantiate(constraint.m_equality_lhs, substitutions));
			std::shared_ptr<MarmotType> resolved_equality_rhs = ApplySubstitution(context, TypeTransform::Instantiate(constraint.m_equality_rhs, substitutions));
			if (std::optional<CompilerError> error = DischargeEqualityConstraint(context, token, constraint, resolved_equality_lhs, resolved_equality_rhs))
			{
				return std::unexpected(std::move(*error));
			}

			continue;
		}
		std::vector<std::shared_ptr<MarmotType>> resolved_type_args;
		resolved_type_args.reserve(constraint.m_type_args.size());
		for (const std::shared_ptr<MarmotType>& type_arg : constraint.m_type_args)
		{
			resolved_type_args.emplace_back(ApplySubstitution(context, TypeTransform::Instantiate(type_arg, substitutions)));
		}

		if (std::optional<CompilerError> error = CheckConstraintHolds(context, token, MarmotType::ClassConstraint(constraint.m_class_name, std::move(resolved_type_args)), depth))
		{
			return std::unexpected(std::move(*error));
		}
	}

	return MarmotType::MakeUndecidedType();
}

std::shared_ptr<MarmotType> TypeChecker::ResolveAssociatedType(const CheckingContext& context, const MarmotType::AssociatedType& associated_type)
{
	std::vector<std::shared_ptr<MarmotType>> resolved_type_args;
	resolved_type_args.reserve(associated_type.m_type_args.size());
	for (const std::shared_ptr<MarmotType>& type_arg : associated_type.m_type_args)
	{
		resolved_type_args.emplace_back(ApplySubstitution(context, type_arg));
	}

	std::unordered_map<std::string, ClassInfo>::const_iterator class_it = m_classes.find(associated_type.m_class_name);
	if (class_it == m_classes.cend() || !class_it->second.m_associated_types.contains(associated_type.m_name))
	{
		std::shared_ptr<MarmotType> unresolved_projection = MarmotType::MakeAssociatedType(associated_type.m_class_name, associated_type.m_name, std::move(resolved_type_args));
		std::shared_ptr<MarmotType> equated_projection = ReduceProjectionByEqualityConstraint(context, unresolved_projection);
		return equated_projection != nullptr ? equated_projection : unresolved_projection;
	}

	std::optional<ResolvedInstanceMatch> resolved_match = FindMatchingInstance(associated_type.m_class_name, resolved_type_args);
	if (!resolved_match.has_value())
	{
		std::shared_ptr<MarmotType> unresolved_projection = MarmotType::MakeAssociatedType(associated_type.m_class_name, associated_type.m_name, std::move(resolved_type_args));
		std::shared_ptr<MarmotType> equated_projection = ReduceProjectionByEqualityConstraint(context, unresolved_projection);
		return equated_projection != nullptr ? equated_projection : unresolved_projection;
	}

	AssociatedTypeEnvironment::const_iterator binding_it = resolved_match->m_instance->m_associated_type_bindings.find(associated_type.m_name);
	if (binding_it == resolved_match->m_instance->m_associated_type_bindings.cend())
	{
		std::shared_ptr<MarmotType> unresolved_projection = MarmotType::MakeAssociatedType(associated_type.m_class_name, associated_type.m_name, std::move(resolved_type_args));
		std::shared_ptr<MarmotType> equated_projection = ReduceProjectionByEqualityConstraint(context, unresolved_projection);
		return equated_projection != nullptr ? equated_projection : unresolved_projection;
	}

	std::shared_ptr<MarmotType> concrete_binding = TypeTransform::Instantiate(binding_it->second, resolved_match->m_substitutions);
	return ApplySubstitution(context, concrete_binding);
}

// An associated-type projection over an abstract type argument cannot be reduced
// by instance selection, because there is no instance to select yet. A where
// clause of the form 'Class::Assoc<S> ~ Type' states the answer directly, so a
// projection that matches one is replaced by the type it was equated to.
std::shared_ptr<MarmotType> TypeChecker::ReduceProjectionByEqualityConstraint(const CheckingContext& context, const std::shared_ptr<MarmotType>& type) const
{
	if (!type->IsType<MarmotType::AssociatedType>())
	{
		return nullptr;
	}

	std::vector<MarmotType::ClassConstraint>::const_iterator match = std::ranges::find_if
	(
		context.m_assumptions->m_constraints,
		[&type](const MarmotType::ClassConstraint& constraint)
		{
			return constraint.IsEquality() && *constraint.m_equality_lhs == *type;
		}
	);

	return match != context.m_assumptions->m_constraints.cend() ? match->m_equality_rhs : nullptr;
}

std::optional<CompilerError> TypeChecker::TryMakeGenericParameterMismatchError(const Token& token, const std::shared_ptr<MarmotType>& left, const std::shared_ptr<MarmotType>& right) const
{
	std::shared_ptr<MarmotType> prototype;
	std::string type_name;

	if (left->IsType<MarmotType::StructType>() && right->IsType<MarmotType::StructType>())
	{
		const MarmotType::StructType& left_struct = left->GetType<MarmotType::StructType>();
		const MarmotType::StructType& right_struct = right->GetType<MarmotType::StructType>();
		if (left_struct.m_name != right_struct.m_name || left_struct.m_module_name != right_struct.m_module_name)
		{
			return std::nullopt;
		}

		type_name = ConstructorKey(left_struct.m_module_name, left_struct.m_name);
		TypeDefinitionMap::const_iterator it = m_struct_type_definitions.find(type_name);
		if (it == m_struct_type_definitions.cend())
		{
			return std::nullopt;
		}
		prototype = it->second;
	}
	else if (left->IsType<MarmotType::UnionType>() && right->IsType<MarmotType::UnionType>())
	{
		const MarmotType::UnionType& left_union = left->GetType<MarmotType::UnionType>();
		const MarmotType::UnionType& right_union = right->GetType<MarmotType::UnionType>();
		if (left_union.m_name != right_union.m_name || left_union.m_module_name != right_union.m_module_name)
		{
			return std::nullopt;
		}

		type_name = ConstructorKey(left_union.m_module_name, left_union.m_name);
		TypeDefinitionMap::const_iterator it = m_union_type_definitions.find(type_name);
		if (it == m_union_type_definitions.cend())
		{
			return std::nullopt;
		}
		prototype = it->second;
	}
	else
	{
		return std::nullopt;
	}

	std::optional<std::vector<std::pair<std::string, std::shared_ptr<MarmotType>>>> left_args = ResolveGenericTypeArguments(prototype, left);
	std::optional<std::vector<std::pair<std::string, std::shared_ptr<MarmotType>>>> right_args = ResolveGenericTypeArguments(prototype, right);
	if (!left_args.has_value() || !right_args.has_value() || left_args->size() != right_args->size())
	{
		return std::nullopt;
	}

	std::string type_signature = type_name;
	if (!left_args->empty())
	{
		const std::string generic_param_names = *left_args
			| std::views::keys
			| std::views::join_with(std::string_view(", "))
			| std::ranges::to<std::string>();
		type_signature += std::format("<{}>", generic_param_names);
	}

	for (size_t idx = 0u; idx < left_args->size(); idx += 1u)
	{
		const std::string& param_name = left_args->at(idx).first;
		const std::shared_ptr<MarmotType>& left_arg = left_args->at(idx).second;
		const std::shared_ptr<MarmotType>& right_arg = right_args->at(idx).second;
		if (*left_arg == *right_arg)
		{
			continue;
		}

		if (HasTypeVariables(left_arg) || HasTypeVariables(right_arg)
			|| left_arg->IsType<MarmotType::UndecidedType>() || right_arg->IsType<MarmotType::UndecidedType>())
		{
			continue;
		}

		const std::string message = std::format
		(
			"In type {}: parameter '{}' is {} in one context but {} in another",
			type_signature,
			param_name,
			DescribeTypePair(*left_arg, *right_arg).first,
			DescribeTypePair(*left_arg, *right_arg).second
		);
		return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, message, token, m_file_name, m_source_lines);
	}

	return std::nullopt;
}

CompilerError TypeChecker::MakeUnificationError(const Token& token, const std::shared_ptr<MarmotType>& left, const std::shared_ptr<MarmotType>& right, UnifyDiagnosticMode diagnostic_mode) const
{
	if (std::optional<CompilerError> generic_mismatch = TryMakeGenericParameterMismatchError(token, left, right))
	{
		return std::move(*generic_mismatch);
	}

	auto build_expected_message = [](const std::shared_ptr<MarmotType>& expected, const std::shared_ptr<MarmotType>& actual)
	{
		const std::pair<std::string, std::string> described = DescribeTypePair(*expected, *actual);
		return std::format("Expected type '{}' but got '{}'", described.first, described.second);
	};

	if (diagnostic_mode == UnifyDiagnosticMode::ActualExpected)
	{
		return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, build_expected_message(right, left), token, m_file_name, m_source_lines);
	}
	if (diagnostic_mode == UnifyDiagnosticMode::ExpectedActual)
	{
		return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, build_expected_message(left, right), token, m_file_name, m_source_lines);
	}

	return MarmotError::GenerateTypeCheckerErrorWithContext
	(
		CompilerErrorCode::TypeMismatch,
		std::format("Type mismatch between '{}' and '{}'", DescribeTypePair(*left, *right).first, DescribeTypePair(*left, *right).second),
		token,
		m_file_name,
		m_source_lines
	);
}

MarmotResult::TypeResult TypeChecker::UnifyTypeArguments(const CheckingContext& context, const Token& token, std::vector<std::shared_ptr<MarmotType>>& left, std::vector<std::shared_ptr<MarmotType>>& right)
{
	if (left.size() != right.size())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, std::format("Type mismatch: {} type argument(s) in one context but {} in another", left.size(), right.size()), token, m_file_name, m_source_lines));
	}

	for (size_t idx : std::views::iota(0u, left.size()))
	{
		// A nominal parameter may occur anywhere in the representation, including
		// within a writable field or a function's parameter.
		MarmotResult::TypeResult result = Unify(context, token, left[idx], right[idx]);
		if (!result.has_value())
		{
			return result;
		}
	}

	return MarmotType::MakeLiteralType<MarmotType::UnitType>();
}

MarmotResult::TypeResult TypeChecker::Unify(const CheckingContext& context, const Token& token, const std::shared_ptr<MarmotType>& left, const std::shared_ptr<MarmotType>& right, UnifyDiagnosticMode diagnostic_mode)
{
	// Apply current substitutions first
	std::shared_ptr<MarmotType> left_subst = ApplySubstitution(context, left);
	std::shared_ptr<MarmotType> right_subst = ApplySubstitution(context, right);

	// Recursion guard to prevent infinite loops when unifying recursive types
	std::pair<MarmotType*, MarmotType*> orig_pair{ left.get(), right.get() };
	std::pair<MarmotType*, MarmotType*> subst_pair{ left_subst.get(), right_subst.get() };
	if (m_unify_visited.contains(orig_pair) || m_unify_visited.contains(subst_pair))
	{
		return left_subst;
	}

	struct RecursionGuard
	{
		std::unordered_set<std::pair<MarmotType*, MarmotType*>, TypePairHash>& m_visited;
		std::vector<std::pair<MarmotType*, MarmotType*>> m_pairs;

		RecursionGuard(std::unordered_set<std::pair<MarmotType*, MarmotType*>, TypePairHash>& visited, std::pair<MarmotType*, MarmotType*> first, std::pair<MarmotType*, MarmotType*> second)
			: m_visited(visited)
		{
			m_pairs.push_back(first);
			if (second != first)
			{
				m_pairs.push_back(second);
			}

			for (const std::pair<MarmotType*, MarmotType*>& pair : m_pairs)
			{
				m_visited.emplace(pair);
			}
		}

		~RecursionGuard()
		{
			for (const std::pair<MarmotType*, MarmotType*>& pair : m_pairs)
			{
				m_visited.erase(pair);
			}
		}
	};

	RecursionGuard guard(m_unify_visited, orig_pair, subst_pair);

	bool is_complex_type = 
		left_subst->IsType<MarmotType::StructType>() ||
		left_subst->IsType<MarmotType::UnionType>() ||
		left_subst->IsType<MarmotType::NewType>() ||
		left_subst->IsType<MarmotType::ArrayType>() ||
		left_subst->IsType<MarmotType::RangeType>() ||
		left_subst->IsType<MarmotType::WorkerType>() ||
		left_subst->IsType<MarmotType::ChannelType>() ||
		left_subst->IsType<MarmotType::CellType>() ||
		left_subst->IsType<MarmotType::FunctionType>();

	// Never is the type of an expression that does not finish, so it may stand
	// where any type is expected. Where Never itself is expected, as in the body
	// of a function declared to return it, nothing else fits.
	const bool left_is_expected = diagnostic_mode == UnifyDiagnosticMode::ExpectedActual || diagnostic_mode == UnifyDiagnosticMode::ReferenceWrite;
	const std::shared_ptr<MarmotType>* expected_side = diagnostic_mode == UnifyDiagnosticMode::ActualExpected ? &right_subst
		: left_is_expected ? &left_subst
		: nullptr;
	const std::shared_ptr<MarmotType>* actual_side = expected_side == &right_subst ? &left_subst : &right_subst;
	const bool actual_is_open = (*actual_side)->IsType<MarmotType::NeverType>() || (*actual_side)->IsType<MarmotType::TypeVariable>() || (*actual_side)->IsType<MarmotType::UndecidedType>();

	// Two Undecided types that meet must end up as one type: `[[], []]` is
	// told its element type later, by one use, and both elements need it.
	// Undecided is refined by overwriting it in place, which reaches only one
	// of the two, so both become the same fresh variable instead, and whatever
	// that variable is bound to reaches every node that holds either.
	if (left_subst->IsType<MarmotType::UndecidedType>() && right_subst->IsType<MarmotType::UndecidedType>() && left.get() != right.get())
	{
		const std::shared_ptr<MarmotType> shared = FreshTypeVar();
		*left = *shared;
		*right = *shared;
		return left;
	}

	if (!is_complex_type && *left_subst == *right_subst)
	{
		return left_subst;
	}
	else if (expected_side != nullptr && (*expected_side)->IsType<MarmotType::NeverType>() && !actual_is_open)
	{
		return std::unexpected(MakeUnificationError(token, left_subst, right_subst, diagnostic_mode));
	}
	// Never type unifies with any type (it's the bottom type)
	else if (left_subst->IsType<MarmotType::NeverType>())
	{
		return right_subst;
	}
	else if (right_subst->IsType<MarmotType::NeverType>())
	{
		return left_subst;
	}
	// Undecided holds no information, so it takes the other side. It is never
	// bound into a variable: a variable bound to Undecided has lost what it was.
	else if (left_subst->IsType<MarmotType::UndecidedType>() && !right_subst->IsType<MarmotType::UndecidedType>())
	{
		*left = *right_subst;
		return left;
	}
	else if (!left_subst->IsType<MarmotType::UndecidedType>() && right_subst->IsType<MarmotType::UndecidedType>())
	{
		*right = *left_subst;
		return left;
	}
	// Of two variables, the newer is bound to the older. A generic function's own
	// parameters are the oldest variables in its body, so a call's freshened ones
	// resolve to them; binding the other way round left the result of a nested
	// generic call typed by a variable the specialization had never heard of.
	else if (left_subst->IsType<MarmotType::TypeVariable>() && !(right_subst->IsType<MarmotType::TypeVariable>() && right_subst->GetType<MarmotType::TypeVariable>().m_id > left_subst->GetType<MarmotType::TypeVariable>().m_id))
	{
		int var_id = left_subst->GetType<MarmotType::TypeVariable>().m_id;
		if (OccursCheck(context, var_id, right_subst))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Infinite type detected (occurs check failed)", token, m_file_name, m_source_lines, left_subst, right_subst));
		}
		m_type_substitution[var_id] = right_subst;
		*left = *right_subst;
		return left;
	}
	else if (right_subst->IsType<MarmotType::TypeVariable>())
	{
		int var_id = right_subst->GetType<MarmotType::TypeVariable>().m_id;
		if (OccursCheck(context, var_id, left_subst))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Infinite type detected (occurs check failed)", token, m_file_name, m_source_lines, left_subst, right_subst));
		}
		m_type_substitution[var_id] = left_subst;
		*right = *left_subst;
		return left;
	}
	else if (left_subst->IsType<MarmotType::ArrayType>() && right_subst->IsType<MarmotType::ArrayType>())
	{
		MarmotResult::TypeResult result = Unify(context, token, left_subst->GetType<MarmotType::ArrayType>().m_element_type, right_subst->GetType<MarmotType::ArrayType>().m_element_type, diagnostic_mode);
		if (!result.has_value())
		{
			return result;
		}
		if (diagnostic_mode == UnifyDiagnosticMode::CommonType)
		{
			return MarmotType::MakeArrayType(*result);
		}
		return left;
	}
	else if (left_subst->IsType<MarmotType::WorkerType>() && right_subst->IsType<MarmotType::WorkerType>())
	{
		MarmotResult::TypeResult result = Unify(context, token, left_subst->GetType<MarmotType::WorkerType>().m_result_type, right_subst->GetType<MarmotType::WorkerType>().m_result_type, diagnostic_mode);
		if (!result.has_value())
		{
			return result;
		}
		if (diagnostic_mode == UnifyDiagnosticMode::CommonType)
		{
			return MarmotType::MakeWorkerType(*result);
		}
		return left;
	}
	else if (left_subst->IsType<MarmotType::ChannelType>() && right_subst->IsType<MarmotType::ChannelType>())
	{
		MarmotResult::TypeResult result = Unify(context, token, left_subst->GetType<MarmotType::ChannelType>().m_element_type, right_subst->GetType<MarmotType::ChannelType>().m_element_type);
		if (!result.has_value())
		{
			return result;
		}
		return left;
	}
	else if (left_subst->IsType<MarmotType::CellType>() && right_subst->IsType<MarmotType::CellType>())
	{
		MarmotResult::TypeResult result = Unify(context, token, left_subst->GetType<MarmotType::CellType>().m_element_type, right_subst->GetType<MarmotType::CellType>().m_element_type);
		if (!result.has_value())
		{
			return result;
		}
		return left;
	}
	else if (left_subst->IsType<MarmotType::RangeType>() && right_subst->IsType<MarmotType::RangeType>())
	{
		MarmotResult::TypeResult result = Unify(context, token, left_subst->GetType<MarmotType::RangeType>().m_element_type, right_subst->GetType<MarmotType::RangeType>().m_element_type, diagnostic_mode);
		if (!result.has_value())
		{
			return result;
		}
		return left;
	}
	else if (left_subst->IsType<MarmotType::FunctionType>() && right_subst->IsType<MarmotType::FunctionType>())
	{
		MarmotType::FunctionType& left_func = left_subst->GetType<MarmotType::FunctionType>();
		MarmotType::FunctionType& right_func = right_subst->GetType<MarmotType::FunctionType>();

		const bool join_types = diagnostic_mode == UnifyDiagnosticMode::CommonType;
		if (!join_types && ((left_func.m_is_transferable && !right_func.m_is_transferable && diagnostic_mode != UnifyDiagnosticMode::ActualExpected) ||
		    (right_func.m_is_transferable && !left_func.m_is_transferable && !left_is_expected)))
		{
			return std::unexpected(MakeUnificationError(token, left_subst, right_subst, diagnostic_mode));
		}

		// Function types must have the same number of parameters
		if (left_func.m_param_types.size() != right_func.m_param_types.size())
		{
			return std::unexpected(MakeFunctionArityError(token, left_func.m_param_types.size(), right_func.m_param_types.size(), diagnostic_mode));
		}

		MarmotResult::TypeResult result;

		for (size_t idx : std::views::iota(0u, left_func.m_param_types.size()))
		{
			// A function supplied by the caller must accept every argument the
			// expected signature permits, including callbacks without a guarantee.
			result = Unify(context, token, right_func.m_param_types[idx], left_func.m_param_types[idx], join_types ? UnifyDiagnosticMode::Symmetric : diagnostic_mode);
			if (!result.has_value())
			{
				return result;
			}
		}

		result = Unify(context, token, left_func.m_return_type, right_func.m_return_type, diagnostic_mode);
		if (!result.has_value())
		{
			return result;
		}
		if (join_types)
		{
			MarmotType::FunctionType common = left_func;
			common.m_return_type = std::move(*result);
			common.m_is_transferable = left_func.m_is_transferable && right_func.m_is_transferable;
			return std::make_shared<MarmotType>(std::move(common));
		}
		return left;
	}
	else if (left_subst->IsType<MarmotType::NewType>() && right_subst->IsType<MarmotType::NewType>())
	{
		MarmotType::NewType& left_new_type = left_subst->GetType<MarmotType::NewType>();
		MarmotType::NewType& right_new_type = right_subst->GetType<MarmotType::NewType>();
		if (left_new_type.m_name != right_new_type.m_name || left_new_type.m_module_name != right_new_type.m_module_name)
		{
			return std::unexpected(MakeUnificationError(token, left_subst, right_subst, diagnostic_mode));
		}

		// The type arguments decide the representation, so they are all there is to unify.
		MarmotResult::TypeResult type_argument_result = UnifyTypeArguments(context, token, left_new_type.m_type_arguments, right_new_type.m_type_arguments);
		if (!type_argument_result.has_value())
		{
			return type_argument_result;
		}
		return left;
	}
	else if (left_subst->IsType<MarmotType::StructType>() && right_subst->IsType<MarmotType::StructType>())
	{
		MarmotType::StructType& left_struct = left_subst->GetType<MarmotType::StructType>();
		MarmotType::StructType& right_struct = right_subst->GetType<MarmotType::StructType>();

		// Struct types must have the same name and same number of members
		if (left_struct.m_name != right_struct.m_name || left_struct.m_module_name != right_struct.m_module_name || left_struct.m_member_types.size() != right_struct.m_member_types.size())
		{
			return std::unexpected(MakeUnificationError(token, left_subst, right_subst, diagnostic_mode));
		}

		if (std::optional<CompilerError> generic_mismatch = TryMakeGenericParameterMismatchError(token, left_subst, right_subst))
		{
			return std::unexpected(std::move(*generic_mismatch));
		}

		MarmotResult::TypeResult type_argument_result = UnifyTypeArguments(context, token, left_struct.m_type_arguments, right_struct.m_type_arguments);
		if (!type_argument_result.has_value())
		{
			return type_argument_result;
		}

		// Unify each member type
		for (size_t idx : std::views::iota(0u, left_struct.m_member_types.size()))
		{
			MarmotResult::TypeResult result = Unify(context, token, left_struct.m_member_types[idx], right_struct.m_member_types[idx], diagnostic_mode);
			if (!result.has_value())
			{
				return result;
			}
		}

		return left;
	}
	else if (left_subst->IsType<MarmotType::UnionType>() && right_subst->IsType<MarmotType::UnionType>())
	{
		MarmotType::UnionType& left_union = left_subst->GetType<MarmotType::UnionType>();
		MarmotType::UnionType& right_union = right_subst->GetType<MarmotType::UnionType>();

		// Union types must have the same name and same members
		if (left_union.m_name != right_union.m_name || left_union.m_module_name != right_union.m_module_name || left_union.m_member_info.size() != right_union.m_member_info.size())
		{
			return std::unexpected(MakeUnificationError(token, left_subst, right_subst, diagnostic_mode));
		}

		if (std::optional<CompilerError> generic_mismatch = TryMakeGenericParameterMismatchError(token, left_subst, right_subst))
		{
			return std::unexpected(std::move(*generic_mismatch));
		}

		MarmotResult::TypeResult type_argument_result = UnifyTypeArguments(context, token, left_union.m_type_arguments, right_union.m_type_arguments);
		if (!type_argument_result.has_value())
		{
			return type_argument_result;
		}

		// The members of a nominal generic type are determined by its type
		// arguments, which have just been unified. Descending again is redundant,
		// and does not terminate when a member reaches the type through a fresh
		// node such as Array<Self>.
		NominalUnifyGuard nominal_guard(*this, ConstructorKey(left_union.m_module_name, left_union.m_name));
		if (!nominal_guard.Entered())
		{
			return left;
		}

		for (auto& [member_name, left_ctx] : left_union.m_member_info)
		{
			std::unordered_map<std::string, MarmotType::UnionType::UnionMemberContext>::iterator right_it = right_union.m_member_info.find(member_name);
			if (right_it == right_union.m_member_info.end())
			{
				return std::unexpected(MakeUnificationError(token, left_subst, right_subst, diagnostic_mode));
			}

			MarmotType::UnionType::UnionMemberContext& right_ctx = right_it->second;
			if (left_ctx.m_member_types.size() != right_ctx.m_member_types.size())
			{
				return std::unexpected(MakeUnificationError(token, left_subst, right_subst, diagnostic_mode));
			}

			for (size_t idx : std::views::iota(0u, left_ctx.m_member_types.size()))
			{
				MarmotResult::TypeResult result = Unify(context, token, left_ctx.m_member_types[idx], right_ctx.m_member_types[idx], diagnostic_mode);
				if (!result.has_value())
				{
					return result;
				}
			}
		}

		return left;
	}
	else if (left_subst->IsType<MarmotType::TupleType>() && right_subst->IsType<MarmotType::TupleType>())
	{
		MarmotType::TupleType& left_tuple = left_subst->GetType<MarmotType::TupleType>();
		MarmotType::TupleType& right_tuple = right_subst->GetType<MarmotType::TupleType>();

		// Tuple types must have the same number of elements
		if (left_tuple.m_element_types.size() != right_tuple.m_element_types.size())
		{
			return std::unexpected(MakeTupleArityError(token, left_tuple.m_element_types.size(), right_tuple.m_element_types.size(), diagnostic_mode));
		}

		// Unify each element type
		std::vector<std::shared_ptr<MarmotType>> common_elements;
		for (size_t idx : std::views::iota(0u, left_tuple.m_element_types.size()))
		{
			MarmotResult::TypeResult result = Unify(context, token, left_tuple.m_element_types[idx], right_tuple.m_element_types[idx], diagnostic_mode);
			if (!result.has_value())
			{
				return result;
			}
			if (diagnostic_mode == UnifyDiagnosticMode::CommonType)
			{
				common_elements.push_back(std::move(*result));
			}
		}
		if (diagnostic_mode == UnifyDiagnosticMode::CommonType)
		{
			return MarmotType::MakeTupleType(std::move(common_elements));
		}

		return left;
	}
	else
	{
		return std::unexpected(MakeUnificationError(token, left_subst, right_subst, diagnostic_mode));
	}
}

TypeChecker::TypeScope::TypeScope(TypeScope* parent)
	: m_parent(parent)
{
}

TypeChecker::TypeScope::TypeScope(TypeEnvironment names)
	: m_parent(nullptr),
	m_names(std::move(names))
{
}

TypeChecker::PendingInstance::PendingInstance(const Token& token, std::shared_ptr<MarmotType> instance, std::shared_ptr<const CheckingAssumptions> assumptions, std::string undecided_message)
	: m_token(token),
	m_instance(std::move(instance)),
	m_assumptions(std::move(assumptions)),
	m_undecided_message(std::move(undecided_message))
{
}

std::shared_ptr<MarmotType> TypeChecker::Instantiate(const CheckingContext& context, const std::shared_ptr<MarmotType>& type, const std::vector<int>& parameters)
{
	std::ranges::for_each(parameters, [this](int parameter) { m_type_substitution.emplace(parameter, FreshTypeVar()); });
	std::shared_ptr<MarmotType> instance = ApplySubstitution(context, type);
	std::ranges::for_each(parameters, [this](int parameter) { m_type_substitution.erase(parameter); });
	return instance;
}

std::shared_ptr<MarmotType> TypeChecker::FreshTypeVar()
{
	return MarmotType::MakeTypeVariable(m_next_type_var_id++);
}

std::shared_ptr<MarmotType> TypeChecker::Freshen(const std::shared_ptr<MarmotType>& type)
{
	return TypeTransform::Freshener([this]() { return FreshTypeVar(); }).Apply(type);
}

// A generic parameter is checked in its body as a type variable, so nothing but
// this stops the body from deciding it. Once the body is checked, each one must
// still be a variable of its own: bound to a type, the body works only for that
// type; bound to another parameter, only when the two are the same.
std::optional<CompilerError> TypeChecker::CheckGenericParametersStayGeneric(const CheckingContext& context, const std::vector<std::pair<int, std::string>>& generic_params, const std::vector<Token>& declared_params, const Token& definition_token)
{
	const std::function<const Token&(const std::string&)> token_of = [&declared_params, &definition_token](const std::string& name) -> const Token&
	{
		std::vector<Token>::const_iterator declared = std::ranges::find_if(declared_params, [&name](const Token& param) { return param.m_lexeme == name; });
		return declared != declared_params.cend() ? *declared : definition_token;
	};

	std::unordered_map<int, std::string> owners;
	for (const auto& [id, name] : generic_params)
	{
		const std::shared_ptr<MarmotType> resolved = ApplySubstitution(context, MarmotType::MakeTypeVariable(id));
		if (!resolved->IsType<MarmotType::TypeVariable>())
		{
			return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, std::format("Generic parameter '{}' stands for any type, but the body uses it as '{}'", name, resolved->DisplayString()), token_of(name), m_file_name, m_source_lines);
		}

		const int resolved_id = resolved->GetType<MarmotType::TypeVariable>().m_id;
		const std::unordered_map<int, std::string>::const_iterator owner = owners.find(resolved_id);
		if (owner != owners.cend())
		{
			return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, std::format("Generic parameters '{}' and '{}' stand for two types a caller picks, but the body uses them as one", owner->second, name), token_of(name), m_file_name, m_source_lines);
		}
		owners.emplace(resolved_id, name);
	}

	return std::nullopt;
}

// A type written inside a generic body names the body's parameters as the parser
// read them, by name. The body checks them as its own variables, so a written
// type is put in those terms before anything is unified with it.
std::shared_ptr<MarmotType> TypeChecker::InstantiateWrittenType(const CheckingContext& context, const std::shared_ptr<MarmotType>& written_type) const
{
	if (context.m_assumptions->m_rigid_type_vars.empty())
	{
		return written_type;
	}

	TypeEnvironment parameters;
	for (const auto& [id, name] : context.m_assumptions->m_rigid_type_vars)
	{
		parameters.emplace(name, MarmotType::MakeTypeVariable(id));
	}

	const std::unordered_set<std::string> written_names = CollectGenericParamNames({ written_type });
	if (std::ranges::none_of(written_names, [&parameters](const std::string& name) { return parameters.contains(name); }))
	{
		return written_type;
	}
	return TypeTransform::Instantiate(written_type, parameters);
}

std::optional<std::string> TypeChecker::RigidParameterName(const CheckingContext& context, const std::shared_ptr<MarmotType>& resolved_type) const
{
	if (!resolved_type->IsType<MarmotType::TypeVariable>())
	{
		return std::nullopt;
	}

	const std::unordered_map<int, std::string>::const_iterator rigid = context.m_assumptions->m_rigid_type_vars.find(resolved_type->GetType<MarmotType::TypeVariable>().m_id);
	return rigid != context.m_assumptions->m_rigid_type_vars.cend() ? std::optional<std::string>(rigid->second) : std::nullopt;
}

TypeTransform::Freshener TypeChecker::MakeLambdaFresheningContext(const CheckingContext& context)
{
	TypeTransform::Freshener freshening([this]() { return FreshTypeVar(); });
	for (const TypeScope* scope_it = context.m_scope; scope_it != nullptr; scope_it = scope_it->m_parent)
	{
		for (const auto& [name, bound_type] : scope_it->m_names)
		{
			if (freshening.Bindings().contains(name))
			{
				continue;
			}

			std::shared_ptr<MarmotType> resolved_type = ApplySubstitution(context, bound_type);
			if (!resolved_type->IsType<MarmotType::TypeVariable>())
			{
				continue;
			}

			freshening.KeepParameter(name, std::move(resolved_type));
		}
	}
	return freshening;
}

// The type variables an enclosing generic definition owns: everything reachable from the
// names currently in scope (its generic parameter bindings and its own parameters), from
// the class constraints in force, and from the return type being checked against. Each of
// those is pinned when that definition is monomorphised at a call site, so a variable left
// over in one of them is not an inference failure. A variable that appears in none of them
// was invented locally and nothing will ever decide it.
//
// Deliberately excludes context.m_expected_expr_type: that is the type demanded of the expression
// being checked, and it has already been unified into that expression's own type, so
// feeding it back in would let an expression vouch for itself.
std::unordered_set<int> TypeChecker::CollectEnclosingTypeVariableIds(const CheckingContext& context)
{
	std::unordered_set<int> enclosing_type_vars;

	const auto remember = [&](const std::shared_ptr<MarmotType>& type)
	{
		const std::unordered_set<int> variables = CollectTypeVariableIds(ApplySubstitution(context, type));
		enclosing_type_vars.insert(variables.cbegin(), variables.cend());
	};
	for (const TypeScope* scope = context.m_scope; scope != nullptr; scope = scope->m_parent)
	{
		for (const auto& [_, type] : scope->m_names)
		{
			remember(type);
		}
		for (const auto& [_, type] : scope->m_bindings)
		{
			remember(type);
		}
	}

	for (const MarmotType::ClassConstraint& constraint : context.m_assumptions->m_constraints)
	{
		for (const std::shared_ptr<MarmotType>& type_arg : constraint.m_type_args)
		{
			std::unordered_set<int> constraint_type_vars = CollectTypeVariableIds(ApplySubstitution(context, type_arg));
			enclosing_type_vars.insert(constraint_type_vars.cbegin(), constraint_type_vars.cend());
		}
	}

	if (context.m_expected_return_type != nullptr)
	{
		std::unordered_set<int> expected_return_type_vars = CollectTypeVariableIds(ApplySubstitution(context, context.m_expected_return_type));
		enclosing_type_vars.insert(expected_return_type_vars.cbegin(), expected_return_type_vars.cend());
	}

	return enclosing_type_vars;
}

std::shared_ptr<MarmotType> TypeChecker::ApplySubstitution(const CheckingContext& context, const std::shared_ptr<MarmotType>& type)
{
	return TypeTransform::Apply(type, m_type_substitution, [this, &context](const MarmotType::AssociatedType& projection) { return ResolveAssociatedType(context, projection); });
}

bool TypeChecker::OccursCheck(const CheckingContext& context, int var_id, const std::shared_ptr<MarmotType>& type)
{
	std::unordered_set<const MarmotType*> visited;
	return OccursCheck(context, var_id, type, visited);
}

bool TypeChecker::OccursCheck(const CheckingContext& context, int var_id, const std::shared_ptr<MarmotType>& type, std::unordered_set<const MarmotType*>& visited)
{
	// Guard on the incoming node before substituting. ApplySubstitution rebuilds any
	// type that still carries type variables, so it hands back a fresh pointer on
	// every call; keying the guard only on that result means a cyclic generic type
	// is never recognised as already visited and the walk does not terminate.
	if (visited.contains(type.get()))
	{
		return false;
	}
	visited.insert(type.get());

	std::shared_ptr<MarmotType> subst_type = ApplySubstitution(context, type);

	if (visited.contains(subst_type.get()))
	{
		return false;
	}
	visited.insert(subst_type.get());

	if (subst_type->IsType<MarmotType::TypeVariable>())
	{
		return subst_type->GetType<MarmotType::TypeVariable>().m_id == var_id;
	}
	else if (subst_type->IsType<MarmotType::ArrayType>())
	{
		return OccursCheck(context, var_id, subst_type->GetType<MarmotType::ArrayType>().m_element_type, visited);
	}
	else if (subst_type->IsType<MarmotType::RangeType>())
	{
		return OccursCheck(context, var_id, subst_type->GetType<MarmotType::RangeType>().m_element_type, visited);
	}
	else if (subst_type->IsType<MarmotType::WorkerType>())
	{
		return OccursCheck(context, var_id, subst_type->GetType<MarmotType::WorkerType>().m_result_type, visited);
	}
	else if (subst_type->IsType<MarmotType::ChannelType>())
	{
		return OccursCheck(context, var_id, subst_type->GetType<MarmotType::ChannelType>().m_element_type, visited);
	}
	else if (subst_type->IsType<MarmotType::CellType>())
	{
		return OccursCheck(context, var_id, subst_type->GetType<MarmotType::CellType>().m_element_type, visited);
	}
	else if (subst_type->IsType<MarmotType::FunctionType>())
	{
		MarmotType::FunctionType& func_type = subst_type->GetType<MarmotType::FunctionType>();
		for (const std::shared_ptr<MarmotType>& param_type : func_type.m_param_types)
		{
			if (OccursCheck(context, var_id, param_type, visited))
			{
				return true;
			}
		}
		return OccursCheck(context, var_id, func_type.m_return_type, visited);
	}
	else if (subst_type->IsType<MarmotType::StructType>())
	{
		MarmotType::StructType& struct_type = subst_type->GetType<MarmotType::StructType>();
		for (const std::shared_ptr<MarmotType>& member_type : struct_type.m_member_types)
		{
			if (OccursCheck(context, var_id, member_type, visited))
			{
				return true;
			}
		}
		for (const std::shared_ptr<MarmotType>& type_argument : struct_type.m_type_arguments)
		{
			if (OccursCheck(context, var_id, type_argument, visited))
			{
				return true;
			}
		}
		return false;
	}
	else if (subst_type->IsType<MarmotType::UnionType>())
	{
		MarmotType::UnionType& union_type = subst_type->GetType<MarmotType::UnionType>();
		// A variable can only reach a nominal type through its type arguments, which
		// are checked below. Walking the members again is redundant and does not
		// terminate when a member reaches the type through a fresh node.
		NominalUnifyGuard occurs_guard(*this, "occurs:" + ConstructorKey(union_type.m_module_name, union_type.m_name));
		if (occurs_guard.Entered())
		{
			for (const auto& [member_name, member_ctx] : union_type.m_member_info)
			{
				for (const std::shared_ptr<MarmotType>& member_type : member_ctx.m_member_types)
				{
					if (OccursCheck(context, var_id, member_type, visited))
					{
						return true;
					}
				}
			}
		}
		for (const std::shared_ptr<MarmotType>& type_argument : union_type.m_type_arguments)
		{
			if (OccursCheck(context, var_id, type_argument, visited))
			{
				return true;
			}
		}
		return false;
	}
	else if (subst_type->IsType<MarmotType::TupleType>())
	{
		MarmotType::TupleType& tuple_type = subst_type->GetType<MarmotType::TupleType>();
		for (const std::shared_ptr<MarmotType>& element_type : tuple_type.m_element_types)
		{
			if (OccursCheck(context, var_id, element_type, visited))
			{
				return true;
			}
		}
		return false;
	}
	else if (subst_type->IsType<MarmotType::AssociatedType>())
	{
		MarmotType::AssociatedType& associated_type = subst_type->GetType<MarmotType::AssociatedType>();
		for (const std::shared_ptr<MarmotType>& type_arg : associated_type.m_type_args)
		{
			if (OccursCheck(context, var_id, type_arg, visited))
			{
				return true;
			}
		}
		return false;
	}

	return false;
}

bool TypeChecker::InstanceKey::operator==(const InstanceKey& other) const
{
	return m_class_name == other.m_class_name && m_concrete_types == other.m_concrete_types;
}

// InstanceKey hash function
std::size_t TypeChecker::InstanceKeyHash::operator()(const InstanceKey& key) const
{
	std::size_t hash = std::hash<std::string>{}(key.m_class_name);
	for (const std::string& type : key.m_concrete_types)
	{
		hash ^= std::hash<std::string>{}(type) + HASH_OFFSET_BASIS + (hash << HASH_LEFT_SHIFT) + (hash >> HASH_RIGHT_SHIFT);
	}
	return hash;
}

std::shared_ptr<MarmotType> TypeChecker::NarrowClassMethodType(const CheckingContext& context, const std::string& class_name, const ClassInfo& class_info, const std::shared_ptr<MarmotType>& declared_method_type, const std::vector<std::shared_ptr<MarmotType>>& known_arg_types, size_t arity)
{
	std::shared_ptr<MarmotType> narrowed_method_type;

	for (const std::pair<const InstanceKey, InstanceInfo>& instance_entry : m_instances)
	{
		const InstanceInfo& instance_info = instance_entry.second;
		if (instance_info.m_class_name != class_name || instance_info.m_type_args.size() != class_info.m_type_param_names.size())
		{
			continue;
		}

		TypeEnvironment class_substitutions;
		for (size_t idx : std::views::iota(0u, class_info.m_type_param_names.size()))
		{
			class_substitutions.emplace(class_info.m_type_param_names[idx], instance_info.m_type_args[idx]);
		}

		std::shared_ptr<MarmotType> candidate_method_type = ApplySubstitution(context, TypeTransform::Instantiate(declared_method_type, class_substitutions));
		if (!candidate_method_type->IsType<MarmotType::FunctionType>())
		{
			continue;
		}

		const MarmotType::FunctionType& candidate_function_type = candidate_method_type->GetType<MarmotType::FunctionType>();
		if (candidate_function_type.m_param_types.size() != arity)
		{
			continue;
		}

		InstanceTypeMatcher matcher;
		bool matched = std::ranges::all_of
		(
			std::views::iota(0u, known_arg_types.size()),
			[&candidate_function_type, &known_arg_types, &matcher, this, &context](size_t idx) -> bool
			{
				return matcher.Match(candidate_function_type.m_param_types[idx], ApplySubstitution(context, known_arg_types[idx]));
			}
		);

		if (!matched)
		{
			continue;
		}

		if (narrowed_method_type != nullptr)
		{
			return std::shared_ptr<MarmotType>{};
		}

		TypeEnvironment substitutions = std::move(matcher).TakeBindings();

		// The narrowed type becomes an argument's expected type, so a parameter of the
		// instance that the arguments so far have not decided must be left open: as
		// the instance's own GenericParam it would pin the argument to that name.
		for (const std::string& param_name : CollectGenericParamNames(instance_info.m_type_args))
		{
			substitutions.try_emplace(param_name, FreshTypeVar());
		}

		// A match with no instance parameters needs no further instantiation.
		narrowed_method_type = substitutions.empty() ? candidate_method_type : ApplySubstitution(context, TypeTransform::Instantiate(candidate_method_type, substitutions));
	}

	return narrowed_method_type;
}

// A class method called on the type a where clause in force names: the
// constraint, not an instance, says what the method's type is.
std::vector<const MarmotType::ClassConstraint*> TypeChecker::MatchingMethodConstraints(const CheckingContext& context, const std::string& qualifier, const ClassInfo& info, const std::shared_ptr<MarmotType>& method, const std::vector<std::shared_ptr<MarmotType>>& arguments)
{
	std::vector<const MarmotType::ClassConstraint*> matches;
	for (const MarmotType::ClassConstraint& constraint : context.m_assumptions->m_constraints)
	{
		if (constraint.m_class_name != qualifier)
		{
			continue;
		}
		TypeEnvironment substitutions;
		for (size_t index = 0u; index < info.m_type_param_names.size(); index += 1u)
		{
			substitutions.emplace(info.m_type_param_names[index], constraint.m_type_args[index]);
		}
		const std::shared_ptr<MarmotType> signature = ApplySubstitution(context, TypeTransform::Instantiate(method, substitutions));
		const std::vector<std::shared_ptr<MarmotType>>& parameters = signature->GetType<MarmotType::FunctionType>().m_param_types;
		if (std::ranges::equal(parameters, arguments, [this, &context](const auto& parameter, const auto& argument) { return *ApplySubstitution(context, parameter) == *ApplySubstitution(context, argument); }))
		{
			matches.push_back(&constraint);
		}
	}
	return matches;
}

MarmotResult::TypeResult TypeChecker::CheckConstrainedMethod(const CheckingContext& context, const Token& token, const ClassInfo& tc_info, const std::shared_ptr<MarmotType>& method_type, const MarmotType::ClassConstraint& constraint, std::vector<std::shared_ptr<MarmotType>>& arg_results)
{
	TypeEnvironment class_substitutions;
	for (size_t i = 0u; i < tc_info.m_type_param_names.size(); i += 1u)
	{
		class_substitutions.emplace(tc_info.m_type_param_names[i], ApplySubstitution(context, constraint.m_type_args[i]));
	}

	std::shared_ptr<MarmotType> resolved_method_type = ApplySubstitution(context, TypeTransform::Instantiate(method_type, class_substitutions));
	if (!resolved_method_type->IsType<MarmotType::FunctionType>())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeNotCallable, "Call expression type error: not a callable", token, m_file_name, m_source_lines, resolved_method_type));
	}

	MarmotType::FunctionType& function_type = resolved_method_type->GetType<MarmotType::FunctionType>();
	if (function_type.m_param_types.size() != arg_results.size())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, std::format("Call expression type error: incorrect arity: expected {} argument(s), got {}", function_type.m_param_types.size(), arg_results.size()), token, m_file_name, m_source_lines));
	}

	for (size_t idx : std::views::iota(0u, arg_results.size()))
	{
		MarmotResult::TypeResult result = Unify(context, token, arg_results[idx], function_type.m_param_types[idx], UnifyDiagnosticMode::ActualExpected);
		if (!result.has_value())
		{
			return result;
		}
	}

	return ApplySubstitution(context, resolved_method_type);
}

// A class method selected from the instances by its arguments. `arg_results`
// holds the arguments already checked, which may be none; the rest are checked
// here, each against what the instances still matching say it must be.
MarmotResult::TypeResult TypeChecker::CheckInstanceMethodCall(const CheckingContext& context, MarmotExpression::Call& call, const std::string& qualifier, const std::string& method_name, const ClassInfo& tc_info, const std::shared_ptr<MarmotType>& method_type, std::vector<std::shared_ptr<MarmotType>>&& arg_results)
{
	arg_results.reserve(call.m_arguments.size());
	for (size_t idx : std::views::iota(arg_results.size(), call.m_arguments.size()))
	{
		// Instance selection is argument-directed, so no parameter type is known before the
		// arguments are checked. Narrowing against the arguments already settled recovers one
		// as soon as a single instance still matches, which is what lets
		// `Appendable::Append(buckets, Slot::Empty())` infer the construction from the
		// container it is appended to. A prefix that picks out no single instance leaves the
		// argument checked with no expected type, as before. Selection below is unchanged and
		// still consults every argument.
		std::shared_ptr<MarmotType> narrowed_method_type = NarrowClassMethodType(context, qualifier, tc_info, method_type, arg_results, call.m_arguments.size());
		std::shared_ptr<MarmotType> expected_param_type = narrowed_method_type != nullptr ? narrowed_method_type->GetType<MarmotType::FunctionType>().m_param_types[idx] : std::shared_ptr<MarmotType>{};
		const CheckingContext expected_context = context.WithExpectedType(std::move(expected_param_type));

		MarmotResult::TypeResult arg_result = Evaluate(expected_context, call.m_arguments[idx]);
		if (!arg_result.has_value())
		{
			return arg_result;
		}
		arg_results.emplace_back(std::move(arg_result.value()));
	}

	return CheckInstanceMethod(context, call.m_paren, qualifier, method_name, tc_info, method_type, std::move(arg_results)).transform
	(
		[&call, this, &context](std::shared_ptr<MarmotType> signature)
		{
			const MarmotType::FunctionType& function = signature->GetType<MarmotType::FunctionType>();
			call.m_is_foreign = function.m_is_foreign;
			call.m_type_data = ApplySubstitution(context, function.m_return_type);
			return call.m_type_data;
		}
	);
}

MarmotResult::TypeResult TypeChecker::CheckInstanceMethod(const CheckingContext& context, const Token& token, const std::string& qualifier, const std::string& method_name, const ClassInfo& tc_info, const std::shared_ptr<MarmotType>& method_type, std::vector<std::shared_ptr<MarmotType>>&& arg_results)
{
	struct ConcreteMethodCandidate
	{
		std::shared_ptr<MarmotType> m_method_type;
		const InstanceInfo* m_instance;
		TypeEnvironment m_substitutions;
		bool m_matches_expected_type;

		ConcreteMethodCandidate(std::shared_ptr<MarmotType>&& method_type, const InstanceInfo* instance, TypeEnvironment&& substitutions, bool matches_expected_type)
			: m_method_type(std::move(method_type)), m_instance(instance), m_substitutions(std::move(substitutions)), m_matches_expected_type(matches_expected_type)
		{
		}
	};

	std::vector<ConcreteMethodCandidate> candidates;
	for (const auto& [instance_key, instance_info] : m_instances)
	{
		if (instance_info.m_class_name != qualifier || instance_info.m_type_args.size() != tc_info.m_type_param_names.size())
		{
			continue;
		}

		TypeEnvironment class_substitutions;
		for (size_t i = 0u; i < tc_info.m_type_param_names.size(); i += 1u)
		{
			class_substitutions.emplace(tc_info.m_type_param_names[i], instance_info.m_type_args[i]);
		}

		std::shared_ptr<MarmotType> candidate_method_type = ApplySubstitution(context, TypeTransform::Instantiate(method_type, class_substitutions));
		if (!candidate_method_type->IsType<MarmotType::FunctionType>())
		{
			continue;
		}

		const MarmotType::FunctionType& candidate_function_type = candidate_method_type->GetType<MarmotType::FunctionType>();
		if (candidate_function_type.m_param_types.size() != arg_results.size())
		{
			continue;
		}

		InstanceTypeMatcher matcher;
		bool matched = true;

		for (size_t i = 0u; i < arg_results.size(); i += 1u)
		{
			std::shared_ptr<MarmotType> resolved_arg = ApplySubstitution(context, arg_results[i]);
			if (!matcher.Match(candidate_function_type.m_param_types[i], resolved_arg))
			{
				matched = false;
				break;
			}
		}

		if (!matched)
		{
			continue;
		}

		// Selection is argument-directed, exactly as the operator spellings are.
		// The expected type only breaks ties between instances the arguments
		// already accept, which is what return-type-directed classes such as
		// Convertible need. Applying it as a filter rejected every instance of
		// a class whose method returns a fixed type.
		bool matches_expected_type = false;
		if (context.m_expected_expr_type != nullptr)
		{
			InstanceTypeMatcher expected_matcher = matcher;
			std::shared_ptr<MarmotType> expected_type = ApplySubstitution(context, context.m_expected_expr_type);
			if (expected_matcher.Match(candidate_function_type.m_return_type, expected_type))
			{
				matches_expected_type = true;
				matcher = std::move(expected_matcher);
			}
		}

		TypeEnvironment substitutions = std::move(matcher).TakeBindings();
		std::shared_ptr<MarmotType> resolved_method_type = ApplySubstitution(context, TypeTransform::Instantiate(candidate_method_type, substitutions));
		candidates.emplace_back(std::move(resolved_method_type), &instance_info, std::move(substitutions), matches_expected_type);
	}

	if (candidates.empty())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Call expression type error: no matching concrete instance for '" + qualifier + NameSeparator.data() + method_name + "'", token, m_file_name, m_source_lines));
	}
	if (candidates.size() != 1u)
	{
		if (std::ranges::count_if(candidates, &ConcreteMethodCandidate::m_matches_expected_type) != 1)
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Call expression type error: ambiguous concrete instance for '" + qualifier + NameSeparator.data() + method_name + "'", token, m_file_name, m_source_lines));
		}

		std::erase_if(candidates, [](const ConcreteMethodCandidate& candidate) -> bool { return !candidate.m_matches_expected_type; });
	}

	MarmotType::FunctionType& function_type = candidates[0u].m_method_type->GetType<MarmotType::FunctionType>();
	for (size_t idx : std::views::iota(0u, arg_results.size()))
	{
		MarmotResult::TypeResult result = Unify(context, token, arg_results[idx], function_type.m_param_types[idx], UnifyDiagnosticMode::ActualExpected);
		if (!result.has_value())
		{
			return result;
		}
	}

	MarmotResult::TypeResult constraint_result = ValidateFunctionConstraints(context, token, function_type);
	if (!constraint_result.has_value())
	{
		return constraint_result;
	}

	// function_type comes from the class declaration, so it never carries the
	// selected instance's where-clause - that lives on InstanceInfo instead.
	MarmotResult::TypeResult instance_constraint_result = ValidateInstanceConstraints(context, token, *candidates[0u].m_instance, candidates[0u].m_substitutions, 0u);
	if (!instance_constraint_result.has_value())
	{
		return instance_constraint_result;
	}

	return ApplySubstitution(context, candidates[0u].m_method_type);
}

// The waiting instances from `from` on whose types are now decided, each checked
// with the constraints that were in force where it was used. The rest wait on.
std::optional<CompilerError> TypeChecker::CheckDecidedPendingInstances(const CheckingContext& context, size_t from)
{
	std::vector<PendingInstance> waiting;
	std::optional<CompilerError> error;
	for (PendingInstance& pending : m_pending_instances | std::views::drop(from))
	{
		const CheckingContext pending_context = context.WithAssumptions(pending.m_assumptions);
		const std::shared_ptr<MarmotType> instance = ApplySubstitution(pending_context, pending.m_instance);
		if (!IsDecided(pending_context, instance))
		{
			waiting.push_back(std::move(pending));
			continue;
		}
		if (!error.has_value())
		{
			MarmotResult::TypeResult validated = ValidateFunctionConstraints(pending_context, pending.m_token, instance->GetType<MarmotType::FunctionType>());
			if (!validated.has_value())
			{
				error = std::move(validated.error());
			}
		}
	}
	m_pending_instances.erase(m_pending_instances.begin() + static_cast<std::ptrdiff_t>(from), m_pending_instances.end());
	m_pending_instances.insert(m_pending_instances.end(), std::make_move_iterator(waiting.begin()), std::make_move_iterator(waiting.end()));
	return error;
}

MarmotResult::TypeResult TypeChecker::UseGenericInstance(const CheckingContext& context, const Token& token, std::shared_ptr<MarmotType> instance, std::string_view undecided_message)
{
	std::unordered_set<int> visible_type_vars = CollectEnclosingTypeVariableIds(context);
	if (context.m_expected_expr_type != nullptr)
	{
		MarmotResult::TypeResult expected_result = Unify(context, token, instance, context.m_expected_expr_type, UnifyDiagnosticMode::ActualExpected);
		if (!expected_result.has_value())
		{
			return expected_result;
		}
		std::unordered_set<int> expected_type_vars = CollectTypeVariableIds(ApplySubstitution(context, context.m_expected_expr_type));
		visible_type_vars.insert(expected_type_vars.cbegin(), expected_type_vars.cend());
	}

	instance = ApplySubstitution(context, instance);
	if (!std::ranges::all_of(CollectTypeVariableIds(instance), [&visible_type_vars](int id) { return visible_type_vars.contains(id); }))
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(std::string(undecided_message), token, m_file_name, m_source_lines));
	}
	if (!IsDecided(context, instance))
	{
		m_pending_instances.emplace_back(token, instance, context.m_assumptions, std::string(undecided_message));
		return instance;
	}
	return ValidateFunctionConstraints(context, token, instance->GetType<MarmotType::FunctionType>())
		.transform([&instance](std::shared_ptr<MarmotType>&&) { return instance; });
}

// Decided: every variable left is a generic parameter of a definition being checked.
bool TypeChecker::IsDecided(const CheckingContext& context, const std::shared_ptr<MarmotType>& type) const
{
	return std::ranges::all_of(CollectTypeVariableIds(type), [&context](int id) { return context.m_assumptions->m_rigid_type_vars.contains(id); });
}
