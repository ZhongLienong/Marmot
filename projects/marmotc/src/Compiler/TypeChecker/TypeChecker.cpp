#include <algorithm>
#include <format>
#include <functional>
#include <iterator>
#include <ranges>
#include <unordered_set>
#include <type_traits>

#include "Compiler/Constant/Constant.h"
#include "Compiler/Names/SpecialNames.h"
#include "Compiler/Error/CompilerError.h"
#include "PatternCoverage.h"
#include "TypeChecker.h"
#include "Compiler/TypeMatching/InstanceTypeMatcher.h"
#include "Compiler/TypeTransform/TypeTransform.h"
#include <cassert>

using namespace std::string_literals;

namespace
{
	template <typename>
	inline constexpr bool AlwaysFalse = false;

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

	// A constructor belongs to the module that declared its type: two modules
	// may each declare a `Wrap`, and `Wrap(1)` in one is not the other's.
	std::string ConstructorKey(const std::string& module_name, const std::string& constructor_name)
	{
		return module_name.empty() ? constructor_name : module_name + std::string(NameSeparator) + constructor_name;
	}

	const Token& GetPatternToken(const MarmotPattern& pattern)
	{
		return std::visit
		(
			[]<typename T>(const T& node) -> const Token&
			{
				using Node = std::decay_t<T>;
				if constexpr (std::is_same_v<Node, MarmotPattern::Binding>)
				{
					return node.m_name;
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Wildcard>)
				{
					return node.m_token;
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Literal>)
				{
					return node.m_token;
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Tuple>)
				{
					return node.m_left_paren;
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Array>)
				{
					return node.m_left_bracket;
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Constructor>)
				{
					return node.m_name_token;
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::As>)
				{
					return GetPatternToken(*node.m_pattern);
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Or>)
				{
					return GetPatternToken(*node.m_alternatives.front());
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Range>)
				{
					return node.m_start;
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Record>)
				{
					return node.m_name_token;
				}
				else
				{
					static_assert(AlwaysFalse<Node>, "Unhandled pattern type.");
					return node.m_name_token;
				}
			},
			*pattern
		);
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

	bool HasTypeVariables(const std::shared_ptr<MarmotType>& type)
	{
		std::unordered_set<const MarmotType*> visited;
		return ContainsType(type, [](const MarmotType& node) { return node.IsType<MarmotType::TypeVariable>(); }, visited);
	}

	// A union's members' fields, in tag order: a member's tag is its place in the
	// declaration.
	std::vector<std::vector<std::shared_ptr<MarmotType>>> FieldsByTag(const MarmotType::UnionType& union_type)
	{
		std::vector<std::vector<std::shared_ptr<MarmotType>>> fields(union_type.m_member_info.size());
		for (const std::pair<const std::string, MarmotType::UnionType::UnionMemberContext>& member : union_type.m_member_info)
		{
			fields[static_cast<size_t>(member.second.m_tag)] = member.second.m_member_types;
		}
		return fields;
	}

	bool IsSpawnSignature(const std::shared_ptr<MarmotType>& argument, const std::shared_ptr<MarmotType>& function, const std::shared_ptr<MarmotType>& result)
	{
		if (!function->IsType<MarmotType::FunctionType>() || !result->IsType<MarmotType::WorkerType>())
		{
			return false;
		}
		const MarmotType::FunctionType& spawned = function->GetType<MarmotType::FunctionType>();
		return spawned.m_param_types.size() == 1u
			&& *spawned.m_param_types[0u] == *argument
			&& *spawned.m_return_type == *result->GetType<MarmotType::WorkerType>().m_result_type;
	}

	// What the VM builds when it joins a worker, by position rather than by name:
	// member 0 holds the worker's value and member 1 why there is none, cancelled
	// (member 0, no fields) or failed (member 1, the message).
	bool IsJoinOutcome(const std::shared_ptr<MarmotType>& outcome, const std::shared_ptr<MarmotType>& value)
	{
		if (!outcome->IsType<MarmotType::UnionType>())
		{
			return false;
		}
		const std::vector<std::vector<std::shared_ptr<MarmotType>>> outcome_fields = FieldsByTag(outcome->GetType<MarmotType::UnionType>());
		if (outcome_fields.size() != 2u || outcome_fields[0u].size() != 1u || *outcome_fields[0u][0u] != *value
			|| outcome_fields[1u].size() != 1u || !outcome_fields[1u][0u]->IsType<MarmotType::UnionType>())
		{
			return false;
		}
		const std::vector<std::vector<std::shared_ptr<MarmotType>>> error_fields = FieldsByTag(outcome_fields[1u][0u]->GetType<MarmotType::UnionType>());
		return error_fields.size() == 2u && error_fields[0u].empty()
			&& error_fields[1u].size() == 1u && error_fields[1u][0u]->IsType<MarmotType::TextType>();
	}

	std::unordered_set<int> CollectTypeVariableIds(const std::shared_ptr<MarmotType>& type)
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

	// A class constraint names its class directly; an equality constraint names it
	// through the projection on its left side. Both must refer to a declared class,
	// so validation sites ask for the name rather than reading m_class_name, which
	// is empty for an equality.
	const std::string& ConstrainedClassName(const MarmotType::ClassConstraint& constraint)
	{
		if (constraint.IsEquality() && constraint.m_equality_lhs->IsType<MarmotType::AssociatedType>())
		{
			return constraint.m_equality_lhs->GetType<MarmotType::AssociatedType>().m_class_name;
		}

		return constraint.m_class_name;
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

	void AppendUniqueConstraint(std::vector<MarmotType::ClassConstraint>& constraints, MarmotType::ClassConstraint&& constraint)
	{
		if (!ContainsConstraint(constraints, constraint))
		{
			constraints.push_back(std::move(constraint));
		}
	}

	std::vector<MarmotType::ClassConstraint> CollectSignatureConstraints(
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

	std::string JoinSortedNames(std::vector<std::string> names)
	{
		std::ranges::sort(names);
		return names | std::views::join_with(std::string_view(", ")) | std::ranges::to<std::string>();
	}


}

class TypeChecker::ScopeSession
{
public:
	explicit ScopeSession(TypeChecker& checker)
		: m_checker(&checker)
	{
		m_checker->BeginScope();
	}

	ScopeSession(const ScopeSession&) = delete;
	ScopeSession& operator=(const ScopeSession&) = delete;

	ScopeSession(ScopeSession&& other) noexcept
		: m_checker(other.m_checker)
	{
		other.m_checker = nullptr;
	}

	~ScopeSession()
	{
		if (m_checker)
		{
			m_checker->EndScope();
		}
	}

	template <typename Fn>
	std::invoke_result_t<Fn&> Then(Fn&& fn) && { return fn(); }

private:
	TypeChecker* m_checker;
};

class TypeChecker::FunctionBodyContext
{
public:
	FunctionBodyContext(TypeChecker& checker, std::shared_ptr<MarmotType> return_type, const std::vector<MarmotType::ClassConstraint>& constraints)
		: m_checker(checker),
		m_enclosing_return_type(std::exchange(checker.m_expected_return_type, std::move(return_type))),
		m_enclosing_constraint_count(checker.m_active_constraints.size())
	{
		for (const MarmotType::ClassConstraint& constraint : constraints)
		{
			if (!ContainsConstraint(m_checker.m_active_constraints, constraint))
			{
				m_checker.m_active_constraints.push_back(constraint);
			}
		}
	}

	~FunctionBodyContext()
	{
		m_checker.m_expected_return_type = std::move(m_enclosing_return_type);
		m_checker.m_active_constraints.resize(m_enclosing_constraint_count);
	}

	FunctionBodyContext(const FunctionBodyContext&) = delete;
	FunctionBodyContext& operator=(const FunctionBodyContext&) = delete;

private:
	TypeChecker& m_checker;
	std::shared_ptr<MarmotType> m_enclosing_return_type;
	size_t m_enclosing_constraint_count;
};

class DefiningGenericGuard
{
private:
	TypeChecker& m_type_checker;

public:
	DefiningGenericGuard(TypeChecker& tc, const std::string& name) : m_type_checker(tc)
	{
		m_type_checker.m_defining_generic_names.push_back(name);
	}

	~DefiningGenericGuard()
	{
		m_type_checker.m_defining_generic_names.pop_back();
	}

	DefiningGenericGuard(const DefiningGenericGuard&) = delete;
	DefiningGenericGuard& operator=(const DefiningGenericGuard&) = delete;
};

class RigidTypeVariablesGuard
{
private:
	TypeChecker& m_type_checker;
	std::vector<std::pair<int, std::string>> m_parameters;

public:
	RigidTypeVariablesGuard(TypeChecker& tc, const TypeChecker::TypeEnvironment& generic_params)
		: m_type_checker(tc)
	{
		for (const auto& [name, type] : generic_params)
		{
			if (type->IsType<MarmotType::TypeVariable>() && m_type_checker.m_rigid_type_vars.emplace(type->GetType<MarmotType::TypeVariable>().m_id, name).second)
			{
				m_parameters.emplace_back(type->GetType<MarmotType::TypeVariable>().m_id, name);
				m_type_checker.m_generic_parameter_ids.insert(type->GetType<MarmotType::TypeVariable>().m_id);
			}
		}
		std::ranges::sort(m_parameters, {}, &std::pair<int, std::string>::second);
	}

	~RigidTypeVariablesGuard()
	{
		for (const std::pair<int, std::string>& parameter : m_parameters)
		{
			m_type_checker.m_rigid_type_vars.erase(parameter.first);
		}
	}

	// Each parameter's variable, by id: binding a variable rewrites its node in
	// place, so the node itself no longer says which variable it was.
	const std::vector<std::pair<int, std::string>>& Parameters() const
	{
		return m_parameters;
	}

	RigidTypeVariablesGuard(const RigidTypeVariablesGuard&) = delete;
	RigidTypeVariablesGuard& operator=(const RigidTypeVariablesGuard&) = delete;
};

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

class ExpectedTypeGuard
{
private:
	TypeChecker& m_type_checker;
	std::shared_ptr<MarmotType> m_saved_type;

public:
	ExpectedTypeGuard(TypeChecker& tc, std::shared_ptr<MarmotType> new_expected)
		: m_type_checker(tc), m_saved_type(tc.m_expected_expr_type)
	{
		m_type_checker.m_expected_expr_type = std::move(new_expected);
	}

	~ExpectedTypeGuard()
	{
		m_type_checker.m_expected_expr_type = std::move(m_saved_type);
	}

	ExpectedTypeGuard(const ExpectedTypeGuard&) = delete;
	ExpectedTypeGuard& operator=(const ExpectedTypeGuard&) = delete;
};

std::size_t TypePairHash::operator()(const std::pair<MarmotType*, MarmotType*>& pair) const noexcept
{
	std::size_t h1 = std::hash<MarmotType*>{}(pair.first);
	std::size_t h2 = std::hash<MarmotType*>{}(pair.second);
	return h1 ^ (h2 * 0x9e3779b97f4a7c15 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
}

const std::array<Token::Name, 5u> TypeChecker::kBinaryArithmeticOperators{
	Token::Name::SINGLE_PLUS,
	Token::Name::SINGLE_MINUS,
	Token::Name::STAR,
	Token::Name::SLASH,
	Token::Name::PERCENT
};
const std::array<Token::Name, 1u> TypeChecker::kBinaryConcatenationOperators{ Token::Name::DOUBLE_PLUS };
const std::array<Token::Name, 4u> TypeChecker::kBinaryPartialOrderComparisonOperators{
	Token::Name::LEFT_ANGLE,
	Token::Name::LESS_EQUAL,
	Token::Name::RIGHT_ANGLE,
	Token::Name::GREATER_EQUAL
};
const std::array<Token::Name, 2u> TypeChecker::kBinaryEqualityOperators{
	Token::Name::DOUBLE_EQUAL,
	Token::Name::BANG_EQUAL
};
const std::array<Token::Name, 2u> TypeChecker::kBinaryLogicalOperators{
	Token::Name::DOUBLE_AMPERSAND,
	Token::Name::DOUBLE_BAR
};
const std::array<Token::Name, 5u> TypeChecker::kBinaryBitwiseOperators{
	Token::Name::CARET,
	Token::Name::SINGLE_AMPERSAND,
	Token::Name::SINGLE_BAR,
	Token::Name::RIGHT_SHIFT,
	Token::Name::LEFT_SHIFT
};

std::string TypeChecker::DescribeConstraint(const MarmotType::ClassConstraint& constraint) const
{
	if (constraint.IsEquality())
	{
		return constraint.m_equality_lhs->DisplayString(m_rigid_type_vars) + " ~ "s + constraint.m_equality_rhs->DisplayString(m_rigid_type_vars);
	}

	if (constraint.m_type_args.empty())
	{
		return constraint.m_class_name;
	}

	const std::string type_args = constraint.m_type_args
		| std::views::transform([this](const std::shared_ptr<MarmotType>& type_arg) { return type_arg->DisplayString(m_rigid_type_vars); })
		| std::views::join_with(std::string_view(", "))
		| std::ranges::to<std::string>();
	return std::format("{}<{}>", constraint.m_class_name, type_args);
}

CompilerError TypeChecker::MakeConstraintFailureError(const Token& token, const MarmotType::ClassConstraint& constraint, std::optional<std::string_view> suggestion) const
{
	std::string message;
	if (!constraint.m_type_args.empty())
	{
		message = std::format
		(
			"Type {} does not satisfy constraint {} - no matching instance found",
			constraint.m_type_args[0u]->DisplayString(m_rigid_type_vars),
			DescribeConstraint(constraint)
		);
	}
	else
	{
		message = std::format("Constraint {} is not satisfied - no matching instance found", DescribeConstraint(constraint));
	}

	return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnsatisfiedConstraint, message, token, m_file_name, m_source_lines, suggestion);
}

bool TypeChecker::HasActiveConstraint(const std::string& class_name, const std::shared_ptr<MarmotType>& type)
{
	std::shared_ptr<MarmotType> resolved_type = ApplySubstitution(type);

	for (const MarmotType::ClassConstraint& constraint : m_active_constraints)
	{
		if (constraint.m_class_name != class_name || constraint.m_type_args.size() != 1u)
		{
			continue;
		}

		std::shared_ptr<MarmotType> resolved_constraint_type = ApplySubstitution(constraint.m_type_args[0u]);
		if (*resolved_constraint_type == *resolved_type)
		{
			return true;
		}
	}

	return false;
}

std::optional<CompilerError> TypeChecker::EnsureTransferable(const Token& token, const std::shared_ptr<MarmotType>& type)
{
	std::unordered_set<const MarmotType*> visited;
	return EnsureTransferable(token, type, visited);
}

std::optional<CompilerError> TypeChecker::EnsureTransferable(const Token& token, const std::shared_ptr<MarmotType>& type, std::unordered_set<const MarmotType*>& visited)
{
	const std::shared_ptr<MarmotType> resolved_type = ApplySubstitution(type);
	if (!visited.insert(resolved_type.get()).second)
	{
		return std::nullopt;
	}

	const MarmotType::ClassConstraint transferable_constraint(std::string(TRANSFERABLE_CLASS_NAME), { resolved_type });

	const auto fail = [&](std::optional<std::string_view> suggestion = std::nullopt) -> std::optional<CompilerError>
	{
		return MakeConstraintFailureError(token, transferable_constraint, suggestion);
	};

	const auto has_transferable_marker = [&]() -> bool
	{
		return HasActiveConstraint(std::string(TRANSFERABLE_CLASS_NAME), resolved_type)
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
		return EnsureTransferable(token, resolved_type->GetType<MarmotType::ArrayType>().m_element_type, visited);
	}

	if (resolved_type->IsType<MarmotType::ChannelType>())
	{
		return EnsureTransferable(token, resolved_type->GetType<MarmotType::ChannelType>().m_element_type, visited);
	}

	// A cell reaching a worker is copied (the CellCrossesWorker warning says so),
	// so it is transferable exactly when what it holds is.
	if (resolved_type->IsType<MarmotType::CellType>())
	{
		return EnsureTransferable(token, resolved_type->GetType<MarmotType::CellType>().m_element_type, visited);
	}

	if (resolved_type->IsType<MarmotType::TupleType>())
	{
		for (const std::shared_ptr<MarmotType>& element_type : resolved_type->GetType<MarmotType::TupleType>().m_element_types)
		{
			if (std::optional<CompilerError> error = EnsureTransferable(token, element_type, visited))
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
		// A function value crosses as its procedure index plus a copy of its captured
		// cells, which is what lets `Concurrency::Spawn` take a function at all, and
		// what makes an in-language ParallelMap writable. What a closure captured is
		// not part of its type, so the captures themselves are not checked here: a
		// closure that captured a Worker crosses and the worker handle is meaningless
		// on the other side. That is the one place transferability is not decided at
		// compile time.
		return std::nullopt;
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
			if (std::optional<CompilerError> error = EnsureTransferable(token, member_type, visited))
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
				if (std::optional<CompilerError> error = EnsureTransferable(token, member_type, visited))
				{
					return error;
				}
			}
		}
		return std::nullopt;
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

std::optional<CompilerError> TypeChecker::DischargeEqualityConstraint(const Token& token, const MarmotType::ClassConstraint& constraint, const std::shared_ptr<MarmotType>& resolved_lhs, const std::shared_ptr<MarmotType>& resolved_rhs)
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
		if (Unify(token, lhs, rhs).has_value())
		{
			return std::nullopt;
		}
	}

	const std::pair<std::string, std::string> described = DescribeTypePair(*resolved_lhs, *resolved_rhs);
	return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnsatisfiedConstraint, std::format("Equality constraint is not satisfied: '{}' resolves to '{}', not '{}'", constraint.m_equality_lhs->DisplayString(), described.first, described.second), token, m_file_name, m_source_lines);
}

bool TypeChecker::IsSatisfiedByActiveConstraint(const MarmotType::ClassConstraint& resolved_constraint)
{
	return std::ranges::any_of
	(
		m_active_constraints,
		[this, &resolved_constraint](const MarmotType::ClassConstraint& active_constraint) -> bool
		{
			if (active_constraint.m_class_name != resolved_constraint.m_class_name || active_constraint.m_type_args.size() != resolved_constraint.m_type_args.size())
			{
				return false;
			}

			return std::ranges::all_of
			(
				std::views::iota(0u, resolved_constraint.m_type_args.size()),
				[this, &active_constraint, &resolved_constraint](size_t idx) -> bool
				{
					return *ApplySubstitution(active_constraint.m_type_args[idx]) == *resolved_constraint.m_type_args[idx];
				}
			);
		}
	);
}

// A class constraint holds when a where clause in force states it, or when
// exactly one instance matches and that instance's own where clauses hold.
// Two matching instances are not a missing one, and the error says which.
std::optional<CompilerError> TypeChecker::CheckConstraintHolds(const Token& token, const MarmotType::ClassConstraint& resolved_constraint, size_t depth, std::optional<std::string_view> suggestion)
{
	if (resolved_constraint.m_class_name == TRANSFERABLE_CLASS_NAME)
	{
		return EnsureTransferable(token, resolved_constraint.m_type_args.front());
	}

	if (IsSatisfiedByActiveConstraint(resolved_constraint))
	{
		return std::nullopt;
	}

	std::vector<ResolvedInstanceMatch> matches = FindMatchingInstances(resolved_constraint.m_class_name, resolved_constraint.m_type_args);
	if (matches.empty())
	{
		return MakeConstraintFailureError(token, resolved_constraint, suggestion);
	}
	if (matches.size() > 1u)
	{
		const std::vector<std::string> candidates = matches
			| std::views::transform([this](const ResolvedInstanceMatch& match) { return std::format("'{}'", DescribeConstraint(MarmotType::ClassConstraint(match.m_instance->m_class_name, std::vector<std::shared_ptr<MarmotType>>(match.m_instance->m_type_args)))); })
			| std::ranges::to<std::vector>();
		return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnsatisfiedConstraint, std::format("Constraint {} is ambiguous: {} instances match it ({})", DescribeConstraint(resolved_constraint), matches.size(), JoinSortedNames(candidates)), token, m_file_name, m_source_lines);
	}

	if (depth >= s_max_instance_constraint_depth)
	{
		return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnsatisfiedConstraint, std::format("Constraint {} could not be decided: its instances' where clauses nest more than {} deep", DescribeConstraint(resolved_constraint), s_max_instance_constraint_depth), token, m_file_name, m_source_lines);
	}

	MarmotResult::TypeResult nested_result = ValidateInstanceConstraints(token, *matches.front().m_instance, matches.front().m_substitutions, depth + 1u);
	if (!nested_result.has_value())
	{
		return std::move(nested_result.error());
	}
	return std::nullopt;
}

MarmotResult::TypeResult TypeChecker::ValidateFunctionConstraints(const Token& token, const MarmotType::FunctionType& function_type)
{
	for (const MarmotType::ClassConstraint& constraint : function_type.m_constraints)
	{
		// An equality constraint is discharged by reduction, not by instance
		// selection: resolve both sides at the call site and compare them.
		if (constraint.IsEquality())
		{
			std::shared_ptr<MarmotType> resolved_equality_lhs = ApplySubstitution(constraint.m_equality_lhs);
			std::shared_ptr<MarmotType> resolved_equality_rhs = ApplySubstitution(constraint.m_equality_rhs);
			if (std::optional<CompilerError> error = DischargeEqualityConstraint(token, constraint, resolved_equality_lhs, resolved_equality_rhs))
			{
				return std::unexpected(std::move(*error));
			}

			continue;
		}
		std::vector<std::shared_ptr<MarmotType>> resolved_type_args;
		resolved_type_args.reserve(constraint.m_type_args.size());
		for (const std::shared_ptr<MarmotType>& type_arg : constraint.m_type_args)
		{
			resolved_type_args.emplace_back(ApplySubstitution(type_arg));
		}

		// No instance is picked for a type nothing has decided yet.
		if (!std::ranges::all_of(resolved_type_args, [this](const std::shared_ptr<MarmotType>& type_arg) { return IsDecided(type_arg); }))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnsatisfiedConstraint, std::format("Call expression type error: could not infer the type this call's {} constraint is on; write the type on the definition that holds the result", constraint.m_class_name), token, m_file_name, m_source_lines));
		}

		if (std::optional<CompilerError> error = CheckConstraintHolds(token, MarmotType::ClassConstraint(constraint.m_class_name, std::move(resolved_type_args)), 0u))
		{
			return std::unexpected(std::move(*error));
		}
	}

	return MarmotType::MakeUndecidedType();
}

MarmotResult::TypeResult TypeChecker::ValidateInstanceConstraints(const Token& token, const InstanceInfo& instance_info, const TypeEnvironment& substitutions, size_t depth)
{
	for (const MarmotType::ClassConstraint& constraint : instance_info.m_constraints)
	{
		// An equality constraint is discharged by reduction, not by instance
		// selection: resolve both sides at the call site and compare them.
		if (constraint.IsEquality())
		{
			std::shared_ptr<MarmotType> resolved_equality_lhs = ApplySubstitution(TypeTransform::Instantiate(constraint.m_equality_lhs, substitutions));
			std::shared_ptr<MarmotType> resolved_equality_rhs = ApplySubstitution(TypeTransform::Instantiate(constraint.m_equality_rhs, substitutions));
			if (std::optional<CompilerError> error = DischargeEqualityConstraint(token, constraint, resolved_equality_lhs, resolved_equality_rhs))
			{
				return std::unexpected(std::move(*error));
			}

			continue;
		}
		std::vector<std::shared_ptr<MarmotType>> resolved_type_args;
		resolved_type_args.reserve(constraint.m_type_args.size());
		for (const std::shared_ptr<MarmotType>& type_arg : constraint.m_type_args)
		{
			resolved_type_args.emplace_back(ApplySubstitution(TypeTransform::Instantiate(type_arg, substitutions)));
		}

		if (std::optional<CompilerError> error = CheckConstraintHolds(token, MarmotType::ClassConstraint(constraint.m_class_name, std::move(resolved_type_args)), depth))
		{
			return std::unexpected(std::move(*error));
		}
	}

	return MarmotType::MakeUndecidedType();
}

std::shared_ptr<MarmotType> TypeChecker::ResolveAssociatedType(const MarmotType::AssociatedType& associated_type)
{
	std::vector<std::shared_ptr<MarmotType>> resolved_type_args;
	resolved_type_args.reserve(associated_type.m_type_args.size());
	for (const std::shared_ptr<MarmotType>& type_arg : associated_type.m_type_args)
	{
		resolved_type_args.emplace_back(ApplySubstitution(type_arg));
	}

	std::unordered_map<std::string, ClassInfo>::const_iterator class_it = m_classes.find(associated_type.m_class_name);
	if (class_it == m_classes.cend() || !class_it->second.m_associated_types.contains(associated_type.m_name))
	{
		std::shared_ptr<MarmotType> unresolved_projection = MarmotType::MakeAssociatedType(associated_type.m_class_name, associated_type.m_name, std::move(resolved_type_args));
		std::shared_ptr<MarmotType> equated_projection = ReduceProjectionByEqualityConstraint(unresolved_projection);
		return equated_projection != nullptr ? equated_projection : unresolved_projection;
	}

	std::optional<ResolvedInstanceMatch> resolved_match = FindMatchingInstance(associated_type.m_class_name, resolved_type_args);
	if (!resolved_match.has_value())
	{
		std::shared_ptr<MarmotType> unresolved_projection = MarmotType::MakeAssociatedType(associated_type.m_class_name, associated_type.m_name, std::move(resolved_type_args));
		std::shared_ptr<MarmotType> equated_projection = ReduceProjectionByEqualityConstraint(unresolved_projection);
		return equated_projection != nullptr ? equated_projection : unresolved_projection;
	}

	AssociatedTypeEnvironment::const_iterator binding_it = resolved_match->m_instance->m_associated_type_bindings.find(associated_type.m_name);
	if (binding_it == resolved_match->m_instance->m_associated_type_bindings.cend())
	{
		std::shared_ptr<MarmotType> unresolved_projection = MarmotType::MakeAssociatedType(associated_type.m_class_name, associated_type.m_name, std::move(resolved_type_args));
		std::shared_ptr<MarmotType> equated_projection = ReduceProjectionByEqualityConstraint(unresolved_projection);
		return equated_projection != nullptr ? equated_projection : unresolved_projection;
	}

	std::shared_ptr<MarmotType> concrete_binding = TypeTransform::Instantiate(binding_it->second, resolved_match->m_substitutions);
	return ApplySubstitution(concrete_binding);
}

// An associated-type projection over an abstract type argument cannot be reduced
// by instance selection, because there is no instance to select yet. A where
// clause of the form 'Class::Assoc<S> ~ Type' states the answer directly, so a
// projection that matches one is replaced by the type it was equated to.
std::shared_ptr<MarmotType> TypeChecker::ReduceProjectionByEqualityConstraint(const std::shared_ptr<MarmotType>& type) const
{
	if (!type->IsType<MarmotType::AssociatedType>())
	{
		return nullptr;
	}

	std::vector<MarmotType::ClassConstraint>::const_iterator match = std::ranges::find_if
	(
		m_active_constraints,
		[&type](const MarmotType::ClassConstraint& constraint)
		{
			return constraint.IsEquality() && *constraint.m_equality_lhs == *type;
		}
	);

	return match != m_active_constraints.cend() ? match->m_equality_rhs : nullptr;
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

MarmotResult::TypeResult TypeChecker::UnifyTypeArguments(const Token& token, std::vector<std::shared_ptr<MarmotType>>& left, std::vector<std::shared_ptr<MarmotType>>& right, UnifyDiagnosticMode diagnostic_mode)
{
	if (left.size() != right.size())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, std::format("Type mismatch: {} type argument(s) in one context but {} in another", left.size(), right.size()), token, m_file_name, m_source_lines));
	}

	for (size_t idx : std::views::iota(0u, left.size()))
	{
		MarmotResult::TypeResult result = Unify(token, left[idx], right[idx], diagnostic_mode);
		if (!result.has_value())
		{
			return result;
		}
	}

	return MarmotType::MakeLiteralType<MarmotType::UnitType>();
}

MarmotResult::TypeResult TypeChecker::Unify(const Token& token, std::shared_ptr<MarmotType>& left, std::shared_ptr<MarmotType>& right, UnifyDiagnosticMode diagnostic_mode)
{
	// Apply current substitutions first
	std::shared_ptr<MarmotType> left_subst = ApplySubstitution(left);
	std::shared_ptr<MarmotType> right_subst = ApplySubstitution(right);

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
	const std::shared_ptr<MarmotType>* expected_side = diagnostic_mode == UnifyDiagnosticMode::ActualExpected ? &right_subst
		: diagnostic_mode == UnifyDiagnosticMode::ExpectedActual ? &left_subst
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
		if (OccursCheck(var_id, right_subst))
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
		if (OccursCheck(var_id, left_subst))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Infinite type detected (occurs check failed)", token, m_file_name, m_source_lines, left_subst, right_subst));
		}
		m_type_substitution[var_id] = left_subst;
		*right = *left_subst;
		return left;
	}
	else if (left_subst->IsType<MarmotType::ArrayType>() && right_subst->IsType<MarmotType::ArrayType>())
	{
		MarmotResult::TypeResult result = Unify(token, left_subst->GetType<MarmotType::ArrayType>().m_element_type, right_subst->GetType<MarmotType::ArrayType>().m_element_type, diagnostic_mode);
		if (!result.has_value())
		{
			return result;
		}
		return left;
	}
	else if (left_subst->IsType<MarmotType::WorkerType>() && right_subst->IsType<MarmotType::WorkerType>())
	{
		MarmotResult::TypeResult result = Unify(token, left_subst->GetType<MarmotType::WorkerType>().m_result_type, right_subst->GetType<MarmotType::WorkerType>().m_result_type, diagnostic_mode);
		if (!result.has_value())
		{
			return result;
		}
		return left;
	}
	else if (left_subst->IsType<MarmotType::ChannelType>() && right_subst->IsType<MarmotType::ChannelType>())
	{
		MarmotResult::TypeResult result = Unify(token, left_subst->GetType<MarmotType::ChannelType>().m_element_type, right_subst->GetType<MarmotType::ChannelType>().m_element_type, diagnostic_mode);
		if (!result.has_value())
		{
			return result;
		}
		return left;
	}
	else if (left_subst->IsType<MarmotType::CellType>() && right_subst->IsType<MarmotType::CellType>())
	{
		MarmotResult::TypeResult result = Unify(token, left_subst->GetType<MarmotType::CellType>().m_element_type, right_subst->GetType<MarmotType::CellType>().m_element_type, diagnostic_mode);
		if (!result.has_value())
		{
			return result;
		}
		return left;
	}
	else if (left_subst->IsType<MarmotType::RangeType>() && right_subst->IsType<MarmotType::RangeType>())
	{
		MarmotResult::TypeResult result = Unify(token, left_subst->GetType<MarmotType::RangeType>().m_element_type, right_subst->GetType<MarmotType::RangeType>().m_element_type, diagnostic_mode);
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

		// Function types must have the same number of parameters
		if (left_func.m_param_types.size() != right_func.m_param_types.size())
		{
			return std::unexpected(MakeFunctionArityError(token, left_func.m_param_types.size(), right_func.m_param_types.size(), diagnostic_mode));
		}

		MarmotResult::TypeResult result;

		for (size_t idx : std::views::iota(0u, left_func.m_param_types.size()))
		{
			result = Unify(token, left_func.m_param_types[idx], right_func.m_param_types[idx], diagnostic_mode);
			if (!result.has_value())
			{
				return result;
			}
		}

		result = Unify(token, left_func.m_return_type, right_func.m_return_type, diagnostic_mode);
		if (!result.has_value())
		{
			return result;
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
		MarmotResult::TypeResult type_argument_result = UnifyTypeArguments(token, left_new_type.m_type_arguments, right_new_type.m_type_arguments, diagnostic_mode);
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

		MarmotResult::TypeResult type_argument_result = UnifyTypeArguments(token, left_struct.m_type_arguments, right_struct.m_type_arguments, diagnostic_mode);
		if (!type_argument_result.has_value())
		{
			return type_argument_result;
		}

		// Unify each member type
		for (size_t idx : std::views::iota(0u, left_struct.m_member_types.size()))
		{
			MarmotResult::TypeResult result = Unify(token, left_struct.m_member_types[idx], right_struct.m_member_types[idx], diagnostic_mode);
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

		MarmotResult::TypeResult type_argument_result = UnifyTypeArguments(token, left_union.m_type_arguments, right_union.m_type_arguments, diagnostic_mode);
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
				MarmotResult::TypeResult result = Unify(token, left_ctx.m_member_types[idx], right_ctx.m_member_types[idx], diagnostic_mode);
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
		for (size_t idx : std::views::iota(0u, left_tuple.m_element_types.size()))
		{
			MarmotResult::TypeResult result = Unify(token, left_tuple.m_element_types[idx], right_tuple.m_element_types[idx], diagnostic_mode);
			if (!result.has_value())
			{
				return result;
			}
		}

		return left;
	}
	else
	{
		return std::unexpected(MakeUnificationError(token, left_subst, right_subst, diagnostic_mode));
	}
}

TypeChecker::TypeScope::TypeScope(TypeEnvironment names)
	: m_names(std::move(names))
{
}

void TypeChecker::BindParameters(const FunctionParameters& parameters, const std::vector<std::shared_ptr<MarmotType>>& types)
{
	for (size_t index = 0u; index < parameters.Size(); index += 1u)
	{
		if (const GeneratedBinding* binding = parameters.GeneratedBindingAt(index))
		{
			m_type_scopes.back().m_generated.emplace(binding->m_index, types[index]);
		}
		else
		{
			m_type_scopes.back().m_names.emplace(parameters.SourceToken(index).m_lexeme, types[index]);
		}
	}
}

TypeChecker& TypeChecker::BeginScope()
{
	m_type_scopes.emplace_back();
	return *this;
}

TypeChecker& TypeChecker::EndScope()
{
	std::erase_if(m_local_generic_functions, [this](const LocalGenericFunction& local) { return local.m_depth == m_type_scopes.size(); });
	m_type_scopes.pop_back();
	return *this;
}

std::shared_ptr<MarmotType>* TypeChecker::FindNameType(const std::string& name)
{
	for (TypeChecker::TypeEnvironmentStack::reverse_iterator it = m_type_scopes.rbegin(); it != m_type_scopes.rend(); ++it)
	{
		TypeEnvironment::iterator found = it->m_names.find(name);
		if (found != it->m_names.end())
		{
			return &found->second;
		}
	}
	return nullptr;
}

const std::shared_ptr<MarmotType>* TypeChecker::FindNameType(const std::string& name) const
{
	for (TypeChecker::TypeEnvironmentStack::const_reverse_iterator it = m_type_scopes.crbegin(); it != m_type_scopes.crend(); ++it)
	{
		TypeEnvironment::const_iterator found = it->m_names.find(name);
		if (found != it->m_names.cend())
		{
			return &found->second;
		}
	}
	return nullptr;
}

MarmotResult::TypeResult TypeChecker::Evaluate(const std::unique_ptr<MarmotStatement>& statement)
{
	return VisitNode
	(
		[this]<typename T>(T& stmt) -> MarmotResult::TypeResult
		{
			return (*this)(stmt);
		},
		statement
	);
}

MarmotResult::TypeResult TypeChecker::Evaluate(const std::unique_ptr<MarmotExpression>& expression)
{
	return VisitNode
	(
		[this]<typename T>(T& expr) -> MarmotResult::TypeResult
		{
			return (*this)(expr);
		},
		expression
	);
}

MarmotResult::TypeResult TypeChecker::CheckPattern(MarmotPattern& pattern, const std::shared_ptr<MarmotType>& expected_type)
{
	std::shared_ptr<MarmotType> resolved_expected = ApplySubstitution(expected_type);

	return std::visit
	(
		[&]<typename T>(T&& node) -> MarmotResult::TypeResult
		{
			using Node = std::decay_t<T>;
			if constexpr (std::is_same_v<Node, MarmotPattern::Binding>)
			{
				node.m_type_data = resolved_expected;
				if (node.m_is_generated)
				{
					m_type_scopes.back().m_generated[node.m_local_index.value()] = resolved_expected;
				}
				else
				{
					m_type_scopes.back().m_names[node.m_name.m_lexeme] = resolved_expected;
				}
				return node.m_type_data;
			}
			else if constexpr (std::is_same_v<Node, MarmotPattern::Wildcard>)
			{
				node.m_type_data = resolved_expected;
				return node.m_type_data;
			}
			else if constexpr (std::is_same_v<Node, MarmotPattern::Literal>)
			{
				switch (node.m_kind)
				{
				case MarmotPattern::LiteralKind::Bool:
					node.m_type_data = MarmotType::MakeLiteralType<MarmotType::BoolType>();
					break;
				case MarmotPattern::LiteralKind::Float:
					node.m_type_data = MarmotType::MakeLiteralType<MarmotType::FloatType>();
					break;
				case MarmotPattern::LiteralKind::Integer:
					node.m_type_data = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
					break;
				case MarmotPattern::LiteralKind::Byte:
					node.m_type_data = MarmotType::MakeLiteralType<MarmotType::ByteType>();
					break;
				case MarmotPattern::LiteralKind::Word:
					node.m_type_data = MarmotType::MakeLiteralType<MarmotType::WordType>();
					break;
				case MarmotPattern::LiteralKind::Text:
					node.m_type_data = MarmotType::MakeLiteralType<MarmotType::TextType>();
					break;
				case MarmotPattern::LiteralKind::Unit:
					node.m_type_data = MarmotType::MakeLiteralType<MarmotType::UnitType>();
					break;
				}

				// Literals do not take their type from where they are used, so a hex or
				// binary literal up to 0xFF is a Byte even against an Int.
				if (node.m_kind == MarmotPattern::LiteralKind::Byte && resolved_expected->IsType<MarmotType::IntegerType>())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, std::format("Pattern type error: '{}' is a Byte, since a hex or binary literal up to 0xFF is one; write {} to match an Int", node.m_token.m_lexeme, ParseUnsignedLiteral(node.m_token.m_lexeme).value()), node.m_token, m_file_name, m_source_lines));
				}
				return Unify(node.m_token, node.m_type_data, resolved_expected, UnifyDiagnosticMode::ActualExpected)
					.and_then([&node](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult { return node.m_type_data; });
			}
			else if constexpr (std::is_same_v<Node, MarmotPattern::Tuple>)
			{
				if (!resolved_expected->IsType<MarmotType::TupleType>())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Pattern type error: expected tuple type", node.m_left_paren, m_file_name, m_source_lines, resolved_expected));
				}

				const MarmotType::TupleType& tuple_type = resolved_expected->GetType<MarmotType::TupleType>();
				if (tuple_type.m_element_types.size() != node.m_elements.size())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Pattern type error: tuple arity mismatch", node.m_left_paren, m_file_name, m_source_lines));
				}

				for (size_t i = 0u; i < node.m_elements.size(); i += 1u)
				{
					MarmotResult::TypeResult elem_result = CheckPattern(*node.m_elements[i], tuple_type.m_element_types[i]);
					if (!elem_result.has_value())
					{
						return std::unexpected(std::move(elem_result.error()));
					}
				}

				node.m_type_data = resolved_expected;
				return node.m_type_data;
			}
			else if constexpr (std::is_same_v<Node, MarmotPattern::Array>)
			{
				if (!resolved_expected->IsType<MarmotType::ArrayType>())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Pattern type error: expected array type", node.m_left_bracket, m_file_name, m_source_lines, resolved_expected));
				}

				const MarmotType::ArrayType& array_type = resolved_expected->GetType<MarmotType::ArrayType>();
				for (const std::unique_ptr<MarmotPattern>& elem : node.m_elements)
				{
					MarmotResult::TypeResult elem_result = CheckPattern(*elem, array_type.m_element_type);
					if (!elem_result.has_value())
					{
						return std::unexpected(std::move(elem_result.error()));
					}
				}

				if (node.m_rest.has_value())
				{
					MarmotResult::TypeResult rest_result = CheckPattern(*node.m_rest->m_pattern, resolved_expected);
					if (!rest_result.has_value())
					{
						return std::unexpected(std::move(rest_result.error()));
					}
				}

				node.m_type_data = resolved_expected;
				return node.m_type_data;
			}
			else if constexpr (std::is_same_v<Node, MarmotPattern::Constructor>)
			{
				if (node.m_is_union)
				{
					if (!resolved_expected->IsType<MarmotType::UnionType>())
					{
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Pattern type error: expected union type", node.m_name_token, m_file_name, m_source_lines, resolved_expected));
					}

					MarmotType::UnionType& union_type = resolved_expected->GetType<MarmotType::UnionType>();
					if (!union_type.m_member_info.contains(node.m_name))
					{
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(std::format("Pattern type error: unrecognized union member '{}'", node.m_name), node.m_name_token, m_file_name, m_source_lines));
					}

					const MarmotType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(node.m_name);
					if (member_ctx.m_member_types.size() != node.m_args.size())
					{
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Pattern type error: incorrect union arity", node.m_name_token, m_file_name, m_source_lines));
					}

					node.m_tag = member_ctx.m_tag;
					for (size_t i = 0u; i < node.m_args.size(); i += 1u)
					{
						MarmotResult::TypeResult arg_result = CheckPattern(*node.m_args[i], member_ctx.m_member_types[i]);
						if (!arg_result.has_value())
						{
							return std::unexpected(std::move(arg_result.error()));
						}
					}

					node.m_type_data = resolved_expected;
					return node.m_type_data;
				}

				if (!resolved_expected->IsType<MarmotType::StructType>())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Pattern type error: expected struct type", node.m_name_token, m_file_name, m_source_lines, resolved_expected));
				}

				const MarmotType::StructType& struct_type = resolved_expected->GetType<MarmotType::StructType>();
				if (struct_type.m_name != node.m_name)
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Pattern type error: struct name mismatch", node.m_name_token, m_file_name, m_source_lines));
				}

				if (struct_type.m_member_types.size() != node.m_args.size())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Pattern type error: incorrect struct arity", node.m_name_token, m_file_name, m_source_lines));
				}

				for (size_t i = 0u; i < node.m_args.size(); i += 1u)
				{
					MarmotResult::TypeResult arg_result = CheckPattern(*node.m_args[i], struct_type.m_member_types[i]);
					if (!arg_result.has_value())
					{
						return std::unexpected(std::move(arg_result.error()));
					}
				}

				node.m_type_data = resolved_expected;
				return node.m_type_data;
			}
			else if constexpr (std::is_same_v<Node, MarmotPattern::As>)
			{
				return CheckPattern(*node.m_pattern, resolved_expected)
					.and_then([&](std::shared_ptr<MarmotType>&&) { return CheckPattern(*node.m_binding, resolved_expected); })
					.transform([&node, &resolved_expected](std::shared_ptr<MarmotType>&&)
					{
						node.m_type_data = resolved_expected;
						return node.m_type_data;
					});
			}
			else if constexpr (std::is_same_v<Node, MarmotPattern::Record>)
			{
				if (!resolved_expected->IsType<MarmotType::StructType>() || resolved_expected->GetType<MarmotType::StructType>().m_name != node.m_name)
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Pattern type error: expected struct type", node.m_name_token, m_file_name, m_source_lines, resolved_expected));
				}

				const MarmotType::StructType& struct_type = resolved_expected->GetType<MarmotType::StructType>();
				std::vector<bool> is_matched(struct_type.m_member_names.size(), false);
				for (MarmotPattern::Record::Field& field : node.m_fields)
				{
					const std::vector<std::string>::const_iterator found = std::ranges::find(struct_type.m_member_names, field.m_name.m_lexeme);
					if (found == struct_type.m_member_names.cend())
					{
						const std::string suggestion = std::format("Struct '{}' does not have a member named '{}'", struct_type.m_name, field.m_name.m_lexeme);
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Unknown struct member in record pattern", field.m_name, m_file_name, m_source_lines, std::optional<std::string_view>(suggestion)));
					}

					field.m_index = static_cast<int>(std::distance(struct_type.m_member_names.cbegin(), found));
					if (is_matched[static_cast<size_t>(field.m_index)])
					{
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(std::format("Pattern type error: '{}' is matched twice in this record pattern", field.m_name.m_lexeme), field.m_name, m_file_name, m_source_lines));
					}
					is_matched[static_cast<size_t>(field.m_index)] = true;

					MarmotResult::TypeResult field_result = CheckPattern(*field.m_pattern, struct_type.m_member_types[static_cast<size_t>(field.m_index)]);
					if (!field_result.has_value())
					{
						return field_result;
					}
				}

				const std::vector<std::string> left_out = std::views::zip(struct_type.m_member_names, is_matched)
					| std::views::filter([](const std::tuple<const std::string&, bool>& member) { return !std::get<1>(member); })
					| std::views::transform([](const std::tuple<const std::string&, bool>& member) { return std::format("'{}'", std::get<0>(member)); })
					| std::ranges::to<std::vector>();
				if (!node.m_rest.has_value() && !left_out.empty())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(std::format("Pattern type error: this record pattern leaves out {}; match each, as in 'field = _', or end with '..' to leave them out", left_out | std::views::join_with(std::string_view(", ")) | std::ranges::to<std::string>()), node.m_name_token, m_file_name, m_source_lines));
				}
				if (node.m_rest.has_value() && left_out.empty())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Pattern type error: every field is matched already, so '..' leaves nothing out", node.m_rest.value(), m_file_name, m_source_lines));
				}

				node.m_type_data = resolved_expected;
				return node.m_type_data;
			}
			else if constexpr (std::is_same_v<Node, MarmotPattern::Range>)
			{
				node.m_type_data = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
				return Unify(node.m_start, node.m_type_data, resolved_expected, UnifyDiagnosticMode::ActualExpected)
					.and_then([&node](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult { return node.m_type_data; });
			}
			else if constexpr (std::is_same_v<Node, MarmotPattern::Or>)
			{
				// Each alternative binds the same names; the first decides their types and
				// every later one must agree with it.
				MarmotResult::TypeResult first = CheckPattern(*node.m_alternatives.front(), resolved_expected);
				if (!first.has_value())
				{
					return first;
				}
				std::unordered_map<std::string, std::shared_ptr<MarmotType>>& names = m_type_scopes.back().m_names;
				const std::vector<std::pair<std::string, std::shared_ptr<MarmotType>>> bound = node.m_alternatives.front()->Bindings(false)
					| std::views::transform([&names](const MarmotPattern::Binding* binding) { return std::pair{ binding->m_name.m_lexeme, names.at(binding->m_name.m_lexeme) }; })
					| std::ranges::to<std::vector>();

				for (std::unique_ptr<MarmotPattern>& alternative : node.m_alternatives | std::views::drop(1))
				{
					MarmotResult::TypeResult checked = CheckPattern(*alternative, resolved_expected);
					if (!checked.has_value())
					{
						return checked;
					}
					for (const auto& [name, type] : bound)
					{
						std::shared_ptr<MarmotType> resolved_type = ApplySubstitution(type);
						std::shared_ptr<MarmotType> alternative_type = ApplySubstitution(names.at(name));
						if (!Unify(GetPatternToken(*alternative), resolved_type, alternative_type).has_value())
						{
							return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, std::format("Pattern type error: '{}' has a different type in this alternative than in the first", name), GetPatternToken(*alternative), m_file_name, m_source_lines, alternative_type, resolved_type));
						}
						names[name] = type;
					}
				}

				node.m_type_data = resolved_expected;
				return node.m_type_data;
			}
			else
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Pattern type error: unsupported pattern", GetPatternToken(pattern), m_file_name, m_source_lines));
			}
		},
		*pattern
	);
}

TypeChecker::LocalGenericFunction::LocalGenericFunction(size_t depth, std::string name, std::vector<int> parameters)
	: m_depth(depth),
	m_name(std::move(name)),
	m_parameters(std::move(parameters))
{
}

TypeChecker::PendingInstance::PendingInstance(const Token& token, std::shared_ptr<MarmotType> instance, std::vector<MarmotType::ClassConstraint> active_constraints, std::string undecided_message)
	: m_token(token),
	m_instance(std::move(instance)),
	m_active_constraints(std::move(active_constraints)),
	m_undecided_message(std::move(undecided_message))
{
}

std::shared_ptr<MarmotType> TypeChecker::Instantiate(const std::shared_ptr<MarmotType>& type, const std::vector<int>& parameters)
{
	std::ranges::for_each(parameters, [this](int parameter) { m_type_substitution.emplace(parameter, FreshTypeVar()); });
	std::shared_ptr<MarmotType> instance = ApplySubstitution(type);
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
std::optional<CompilerError> TypeChecker::CheckGenericParametersStayGeneric(const std::vector<std::pair<int, std::string>>& generic_params, const std::vector<Token>& declared_params, const Token& definition_token)
{
	const std::function<const Token&(const std::string&)> token_of = [&declared_params, &definition_token](const std::string& name) -> const Token&
	{
		std::vector<Token>::const_iterator declared = std::ranges::find_if(declared_params, [&name](const Token& param) { return param.m_lexeme == name; });
		return declared != declared_params.cend() ? *declared : definition_token;
	};

	std::unordered_map<int, std::string> owners;
	for (const auto& [id, name] : generic_params)
	{
		const std::shared_ptr<MarmotType> resolved = ApplySubstitution(MarmotType::MakeTypeVariable(id));
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
std::shared_ptr<MarmotType> TypeChecker::InstantiateWrittenType(const std::shared_ptr<MarmotType>& written_type) const
{
	if (m_rigid_type_vars.empty())
	{
		return written_type;
	}

	TypeEnvironment parameters;
	for (const auto& [id, name] : m_rigid_type_vars)
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

std::optional<std::string> TypeChecker::RigidParameterName(const std::shared_ptr<MarmotType>& resolved_type) const
{
	if (!resolved_type->IsType<MarmotType::TypeVariable>())
	{
		return std::nullopt;
	}

	const std::unordered_map<int, std::string>::const_iterator rigid = m_rigid_type_vars.find(resolved_type->GetType<MarmotType::TypeVariable>().m_id);
	return rigid != m_rigid_type_vars.cend() ? std::optional<std::string>(rigid->second) : std::nullopt;
}

TypeTransform::Freshener TypeChecker::MakeLambdaFresheningContext()
{
	TypeTransform::Freshener context([this]() { return FreshTypeVar(); });
	for (TypeEnvironmentStack::const_reverse_iterator scope_it = m_type_scopes.crbegin(); scope_it != m_type_scopes.crend(); ++scope_it)
	{
		for (const auto& [name, bound_type] : scope_it->m_names)
		{
			if (context.Bindings().contains(name))
			{
				continue;
			}

			std::shared_ptr<MarmotType> resolved_type = ApplySubstitution(bound_type);
			if (!resolved_type->IsType<MarmotType::TypeVariable>())
			{
				continue;
			}

			context.KeepParameter(name, std::move(resolved_type));
		}
	}
	return context;
}

// The type variables an enclosing generic definition owns: everything reachable from the
// names currently in scope (its generic parameter bindings and its own parameters), from
// the class constraints in force, and from the return type being checked against. Each of
// those is pinned when that definition is monomorphised at a call site, so a variable left
// over in one of them is not an inference failure. A variable that appears in none of them
// was invented locally and nothing will ever decide it.
//
// Deliberately excludes m_expected_expr_type: that is the type demanded of the expression
// being checked, and it has already been unified into that expression's own type, so
// feeding it back in would let an expression vouch for itself.
std::unordered_set<int> TypeChecker::CollectEnclosingTypeVariableIds()
{
	std::unordered_set<int> enclosing_type_vars;

	const auto remember = [&](const std::shared_ptr<MarmotType>& type)
	{
		const std::unordered_set<int> variables = CollectTypeVariableIds(ApplySubstitution(type));
		enclosing_type_vars.insert(variables.cbegin(), variables.cend());
	};
	for (const TypeScope& scope : m_type_scopes)
	{
		for (const auto& [_, type] : scope.m_names)
		{
			remember(type);
		}
		for (const auto& [_, type] : scope.m_generated)
		{
			remember(type);
		}
	}

	for (const MarmotType::ClassConstraint& constraint : m_active_constraints)
	{
		for (const std::shared_ptr<MarmotType>& type_arg : constraint.m_type_args)
		{
			std::unordered_set<int> constraint_type_vars = CollectTypeVariableIds(ApplySubstitution(type_arg));
			enclosing_type_vars.insert(constraint_type_vars.cbegin(), constraint_type_vars.cend());
		}
	}

	if (m_expected_return_type != nullptr)
	{
		std::unordered_set<int> expected_return_type_vars = CollectTypeVariableIds(ApplySubstitution(m_expected_return_type));
		enclosing_type_vars.insert(expected_return_type_vars.cbegin(), expected_return_type_vars.cend());
	}

	return enclosing_type_vars;
}

std::shared_ptr<MarmotType> TypeChecker::ApplySubstitution(const std::shared_ptr<MarmotType>& type)
{
	return TypeTransform::Apply(type, m_type_substitution, [this](const MarmotType::AssociatedType& projection) { return ResolveAssociatedType(projection); });
}

bool TypeChecker::OccursCheck(int var_id, const std::shared_ptr<MarmotType>& type)
{
	std::unordered_set<const MarmotType*> visited;
	return OccursCheck(var_id, type, visited);
}

bool TypeChecker::OccursCheck(int var_id, const std::shared_ptr<MarmotType>& type, std::unordered_set<const MarmotType*>& visited)
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

	std::shared_ptr<MarmotType> subst_type = ApplySubstitution(type);

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
		return OccursCheck(var_id, subst_type->GetType<MarmotType::ArrayType>().m_element_type, visited);
	}
	else if (subst_type->IsType<MarmotType::RangeType>())
	{
		return OccursCheck(var_id, subst_type->GetType<MarmotType::RangeType>().m_element_type, visited);
	}
	else if (subst_type->IsType<MarmotType::WorkerType>())
	{
		return OccursCheck(var_id, subst_type->GetType<MarmotType::WorkerType>().m_result_type, visited);
	}
	else if (subst_type->IsType<MarmotType::ChannelType>())
	{
		return OccursCheck(var_id, subst_type->GetType<MarmotType::ChannelType>().m_element_type, visited);
	}
	else if (subst_type->IsType<MarmotType::CellType>())
	{
		return OccursCheck(var_id, subst_type->GetType<MarmotType::CellType>().m_element_type, visited);
	}
	else if (subst_type->IsType<MarmotType::FunctionType>())
	{
		MarmotType::FunctionType& func_type = subst_type->GetType<MarmotType::FunctionType>();
		for (const std::shared_ptr<MarmotType>& param_type : func_type.m_param_types)
		{
			if (OccursCheck(var_id, param_type, visited))
			{
				return true;
			}
		}
		return OccursCheck(var_id, func_type.m_return_type, visited);
	}
	else if (subst_type->IsType<MarmotType::StructType>())
	{
		MarmotType::StructType& struct_type = subst_type->GetType<MarmotType::StructType>();
		for (const std::shared_ptr<MarmotType>& member_type : struct_type.m_member_types)
		{
			if (OccursCheck(var_id, member_type, visited))
			{
				return true;
			}
		}
		for (const std::shared_ptr<MarmotType>& type_argument : struct_type.m_type_arguments)
		{
			if (OccursCheck(var_id, type_argument, visited))
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
					if (OccursCheck(var_id, member_type, visited))
					{
						return true;
					}
				}
			}
		}
		for (const std::shared_ptr<MarmotType>& type_argument : union_type.m_type_arguments)
		{
			if (OccursCheck(var_id, type_argument, visited))
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
			if (OccursCheck(var_id, element_type, visited))
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
			if (OccursCheck(var_id, type_arg, visited))
			{
				return true;
			}
		}
		return false;
	}

	return false;
}

TypeChecker::ClassInfo::ClassInfo(const std::string& name, std::vector<std::string>&& params, AssociatedTypeEnvironment&& associated_types, TypeEnvironment&& methods)
	: m_name(name),
	m_type_param_names(std::move(params)),
	m_associated_types(std::move(associated_types)),
	m_method_types(std::move(methods))
{
}

TypeChecker::InstanceInfo::InstanceInfo(const std::string& tc_name, std::vector<std::shared_ptr<MarmotType>>&& args, std::vector<MarmotType::ClassConstraint>&& constraints, AssociatedTypeEnvironment&& associated_type_bindings, std::unordered_map<std::string, std::unique_ptr<MarmotStatement>>&& methods, bool is_derived)
	: m_class_name(tc_name),
	m_type_args(std::move(args)),
	m_constraints(std::move(constraints)),
	m_associated_type_bindings(std::move(associated_type_bindings)),
	m_method_impls(std::move(methods)),
	m_is_derived(is_derived)
{
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

TypeChecker::TypeChecker(
	MarmotProgramTree&& parser_result,
	std::string_view file_name,
	const std::vector<std::string>& source_lines,
	TypeEnvironment imported_types,
	const TypeclassMetadataMap& imported_typeclasses
)
	: m_program_tree(std::move(parser_result)),
	m_file_name(file_name),
	m_source_lines(source_lines),
	m_next_type_var_id(0)
{
	for (const auto& [class_name, metadata] : imported_typeclasses)
	{
		AssociatedTypeEnvironment associated_types;
		for (const std::string& name : metadata.m_associated_type_names)
		{
			std::vector<std::shared_ptr<MarmotType>> type_args;
			for (const std::string& param : metadata.m_type_param_names)
			{
				type_args.emplace_back(MarmotType::MakeGenericType(param));
			}
			associated_types.emplace(name, MarmotType::MakeAssociatedType(class_name, name, std::move(type_args)));
		}
		m_classes.emplace(class_name, ClassInfo(class_name, std::vector<std::string>(metadata.m_type_param_names), std::move(associated_types), TypeEnvironment(metadata.m_method_types)));
	}

	// Pre-populate type environment with imported types
	if (!imported_types.empty())
	{
		TypeEnvironment imported_names;
		for (const auto& [name, type] : imported_types)
		{
			// An exported value can have a struct type too, `def Origin = P(0, 0)`,
			// and is bound to its type like any value. Only the struct's own name
			// is its constructor.
			const size_t separator_pos = name.rfind(NameSeparator);
			const std::string_view bare_name = separator_pos == std::string::npos ? std::string_view(name) : std::string_view(name).substr(separator_pos + NameSeparator.length());
			if (type->IsType<MarmotType::StructType>() && type->GetType<MarmotType::StructType>().m_name == bare_name)
			{
				const MarmotType::StructType& struct_type = type->GetType<MarmotType::StructType>();
				m_struct_type_definitions[ConstructorKey(struct_type.m_module_name, struct_type.m_name)] = type;
				if (!struct_type.m_generic_params.empty())
				{
					m_generic_structs.insert(ConstructorKey(struct_type.m_module_name, struct_type.m_name));
				}

				// The declaring module binds a struct's name to its constructor's
				// function type, not to the struct type, so an importing module must
				// bind the same thing or there is nothing callable under the name.
				// The type itself is still reachable through m_struct_type_definitions,
				// registered just above.
				// It carries the struct's where clause, as the declaring module's does,
				// so constructing one here is held to the same constraints.
				std::shared_ptr<MarmotType> constructor_type = MarmotType::MakeFunctionType(struct_type.m_member_types, std::shared_ptr<MarmotType>(type));
				constructor_type->GetType<MarmotType::FunctionType>().m_constraints = struct_type.m_constraints;
				imported_names[name] = std::move(constructor_type);
				continue;
			}

			// The declaring module derives a newtype's conversions to and from its
			// representation itself, so they are not among the instances it
			// exports; without them `4 as Module::Meters` found no instance.
			if (type->IsType<MarmotType::NewType>())
			{
				RegisterIdentityConversion(type, type->GetType<MarmotType::NewType>().m_representation);
				RegisterIdentityConversion(type->GetType<MarmotType::NewType>().m_representation, type);
			}

			if (type->IsType<MarmotType::UnionType>())
			{
				const MarmotType::UnionType& union_type = type->GetType<MarmotType::UnionType>();
				m_union_type_definitions[ConstructorKey(union_type.m_module_name, union_type.m_name)] = type;
				if (!union_type.m_generic_params.empty())
				{
					m_generic_unions.insert(ConstructorKey(union_type.m_module_name, union_type.m_name));
				}
			}

			imported_names[name] = type;
		}

		m_type_scopes.emplace_back(std::move(imported_names));
	}

	for (const auto& [class_name, metadata] : imported_typeclasses)
	{
		for (const TypeclassInstance& instance : metadata.m_instances)
		{
			std::vector<std::string> concrete_type_names;
			concrete_type_names.reserve(instance.m_type_args.size());
			for (const std::shared_ptr<MarmotType>& type_arg : instance.m_type_args)
			{
				concrete_type_names.emplace_back(type_arg->ToString());
			}
			InstanceKey instance_key{ class_name, std::move(concrete_type_names) };
			if (!m_instances.contains(instance_key))
			{
				m_instances.emplace(std::move(instance_key), InstanceInfo(class_name, TypeclassInstance::TypeArguments(instance.m_type_args), std::vector<MarmotType::ClassConstraint>(instance.m_constraints), AssociatedTypeEnvironment(instance.m_associated_type_bindings), std::unordered_map<std::string, std::unique_ptr<MarmotStatement>>()));
			}
		}
	}
}

const TypeChecker::TypeEnvironment& TypeChecker::ModuleTypes() const noexcept
{
	return m_module_types;
}

namespace
{
	bool IsWrittenDown(const std::shared_ptr<MarmotType>& type)
	{
		return !type->IsType<MarmotType::UndecidedType>();
	}
}

void TypeChecker::CollectLaterDefinitions()
{
	for (const std::unique_ptr<MarmotStatement>& statement : m_program_tree)
	{
		if (statement->IsStatement<MarmotStatement::VariableDefinition>())
		{
			const MarmotStatement::VariableDefinition& def = statement->GetStatement<MarmotStatement::VariableDefinition>();
			if (def.m_annotated_type.has_value())
			{
				m_later_definitions[def.m_name.m_lexeme] = def.m_annotated_type.value();
				continue;
			}

			if ((def.m_value != nullptr) && def.m_value->IsExpression<MarmotExpression::Function>())
			{
				const MarmotExpression::Function& function = def.m_value->GetExpression<MarmotExpression::Function>();
				if (std::ranges::all_of(function.m_param_types, IsWrittenDown) && IsWrittenDown(function.m_return_type))
				{
					std::shared_ptr<MarmotType> function_type = MarmotType::MakeFunctionType(function.m_param_types, std::shared_ptr<MarmotType>(function.m_return_type));
					function_type->GetType<MarmotType::FunctionType>().m_constraints = function.m_constraints;
					m_later_definitions[def.m_name.m_lexeme] = std::move(function_type);
					continue;
				}
			}

			m_later_unannotated.insert(def.m_name.m_lexeme);
		}
		else if (statement->IsStatement<MarmotStatement::FunctionDefinition>())
		{
			const MarmotStatement::FunctionDefinition& defun = statement->GetStatement<MarmotStatement::FunctionDefinition>();
			std::shared_ptr<MarmotType> function_type = MarmotType::MakeFunctionType(defun.m_param_types, std::shared_ptr<MarmotType>(defun.m_return_type));
			function_type->GetType<MarmotType::FunctionType>().m_constraints = defun.m_constraints;
			m_later_definitions[defun.m_name.m_lexeme] = std::move(function_type);
		}
		else if (statement->IsStatement<MarmotStatement::ForeignDefinition>())
		{
			const MarmotStatement::ForeignDefinition& foreign = statement->GetStatement<MarmotStatement::ForeignDefinition>();
			m_later_definitions[foreign.m_function_name.m_lexeme] = foreign.m_type;
		}
		else if (statement->IsStatement<MarmotStatement::TupleDefinition>())
		{
			for (const Token& name : statement->GetStatement<MarmotStatement::TupleDefinition>().m_names)
			{
				m_later_unannotated.insert(name.m_lexeme);
			}
		}
	}

	// The only scope yet is the imported names. A bare name this module
	// defines is its own, so an import's entry under it would bind a use
	// above the definition to the import instead.
	if (!m_type_scopes.empty())
	{
		TypeEnvironment& imported_names = m_type_scopes.front().m_names;
		for (const std::string& name : m_later_definitions | std::views::keys)
		{
			imported_names.erase(name);
		}
		for (const std::string& name : m_later_unannotated)
		{
			imported_names.erase(name);
		}
	}
}

void TypeChecker::RecordFailedDefinition(const MarmotStatement& statement)
{
	if (statement.IsStatement<MarmotStatement::VariableDefinition>())
	{
		m_failed_definitions.insert(statement.GetStatement<MarmotStatement::VariableDefinition>().m_name.m_lexeme);
	}
	else if (statement.IsStatement<MarmotStatement::TupleDefinition>())
	{
		for (const Token& name : statement.GetStatement<MarmotStatement::TupleDefinition>().m_names)
		{
			m_failed_definitions.insert(name.m_lexeme);
		}
	}
}

MarmotResult::TypeCheckerResult TypeChecker::TypeCheck()
{
	CollectLaterDefinitions();

	return ScopeSession(*this).Then([&]() -> MarmotResult::TypeCheckerResult
	{
		std::vector<CompilerError> errors;

		// Declarations first: a type may be named and constructed, and an
		// instance selected, anywhere in its module, and declaring one runs
		// nothing. An instance's method bodies are code, checked in order.
		const std::function<bool(const std::unique_ptr<MarmotStatement>&)> is_declaration = [](const std::unique_ptr<MarmotStatement>& statement)
			{
				return statement->IsStatement<MarmotStatement::Class>()
					|| statement->IsStatement<MarmotStatement::Struct>()
					|| statement->IsStatement<MarmotStatement::Union>()
					|| statement->IsStatement<MarmotStatement::TypeAlias>();
			};
		const std::function<void(MarmotResult::TypeResult&&)> collect = [&errors](MarmotResult::TypeResult&& result)
			{
				if (!result.has_value())
				{
					errors.emplace_back(std::move(result.error()));
				}
			};

		for (std::unique_ptr<MarmotStatement>& statement : m_program_tree | std::views::filter(is_declaration))
		{
			collect(Evaluate(statement));
		}
		for (std::unique_ptr<MarmotStatement>& statement : m_program_tree)
		{
			if (statement->IsStatement<MarmotStatement::Instance>())
			{
				collect(RegisterInstance(statement->GetStatement<MarmotStatement::Instance>()));
			}
		}
		if (!errors.empty())
		{
			return std::unexpected(MarmotResult::CompilerDiagnostics(std::move(errors)));
		}

		for (std::unique_ptr<MarmotStatement>& statement : m_program_tree | std::views::filter([&is_declaration](const std::unique_ptr<MarmotStatement>& statement) { return !is_declaration(statement); }))
		{
			m_used_failed_definition = false;
			MarmotResult::TypeResult result = Evaluate(statement);
			if (!result.has_value())
			{
				RecordFailedDefinition(*statement);
				if (m_used_failed_definition)
				{
					continue;
				}
			}
			collect(std::move(result));
		}

		if (errors.empty())
		{
			if (std::optional<CompilerError> error = CheckDecidedPendingInstances(0u))
			{
				errors.push_back(std::move(*error));
			}
			for (const PendingInstance& undecided : m_pending_instances)
			{
				errors.push_back(MarmotError::GenerateTypeCheckerErrorWithContext(undecided.m_undecided_message, undecided.m_token, m_file_name, m_source_lines));
			}
		}
		if (errors.empty())
		{
			for (std::unique_ptr<MarmotStatement>& statement : m_program_tree)
			{
				ResolveRecordedTypes(*statement);
			}
			errors = std::move(m_undecided_call_errors);
		}
		if (errors.empty())
		{
			m_module_types = m_type_scopes.back().m_names;
			return std::move(m_program_tree);
		}
		return std::unexpected(MarmotResult::CompilerDiagnostics(std::move(errors)));
	});
}

void TypeChecker::ResolveRecordedTypes(MarmotStatement& statement)
{
	struct StatementResolver
	{
		TypeChecker& m_self;

		void operator()(MarmotStatement::ExpressionStatement& expression_statement) const
		{
			m_self.ResolveRecordedTypes(*expression_statement.m_expr);
		}

		void operator()(MarmotStatement::VariableDefinition& definition) const
		{
			if (definition.m_value != nullptr)
			{
				m_self.ResolveRecordedTypes(*definition.m_value);
			}
		}

		void operator()(MarmotStatement::TupleDefinition& definition) const
		{
			m_self.ResolveRecordedTypes(*definition.m_value);
		}

		void operator()(MarmotStatement::FunctionDefinition& definition) const
		{
			if (definition.m_body != nullptr)
			{
				m_self.ResolveRecordedTypes(*definition.m_body);
			}
		}

		void operator()(MarmotStatement::Instance& instance) const
		{
			for (MarmotStatement::Instance::Method& method : instance.m_methods)
			{
				(*this)(method.m_definition);
			}
		}

		void operator()(MarmotStatement::ForeignDefinition&) const {}
		void operator()(MarmotStatement::Struct&) const {}
		void operator()(MarmotStatement::Union&) const {}
		void operator()(MarmotStatement::Class&) const {}
		void operator()(MarmotStatement::TypeAlias&) const {}
	};

	VisitNode(StatementResolver{ *this }, statement);
}

void TypeChecker::ResolveRecordedTypes(MarmotExpression& expression)
{
	struct ExpressionResolver
	{
		TypeChecker& m_self;

		void Resolve(std::unique_ptr<MarmotExpression>& child) const
		{
			if (child != nullptr)
			{
				m_self.ResolveRecordedTypes(*child);
			}
		}

		void Resolve(std::vector<std::unique_ptr<MarmotExpression>>& children) const
		{
			std::ranges::for_each(children, [this](std::unique_ptr<MarmotExpression>& child) { Resolve(child); });
		}

		void Resolve(std::shared_ptr<MarmotType>& type) const
		{
			if (type != nullptr)
			{
				type = m_self.ApplySubstitution(type);
			}
		}

		void operator()(MarmotExpression::As& node) const { Resolve(node.m_to_type); Resolve(node.m_expr); }
		void operator()(MarmotExpression::Binary& node) const { Resolve(node.m_left); Resolve(node.m_right); }
		void operator()(MarmotExpression::Group& node) const { Resolve(node.m_expr_in); }
		void operator()(MarmotExpression::Tuple& node) const { Resolve(node.m_elements); }
		void operator()(MarmotExpression::Literal&) const {}
		void operator()(MarmotExpression::UnaryPrefix& node) const { Resolve(node.m_expr); }
		void operator()(MarmotExpression::Send& node) const { Resolve(node.m_channel); Resolve(node.m_value); }
		void operator()(MarmotExpression::Receive& node) const { Resolve(node.m_channel); }
		void operator()(MarmotExpression::NameAccess&) const {}

		void operator()(MarmotExpression::Call& node) const
		{
			Resolve(node.m_callee);
			Resolve(node.m_arguments);
			m_self.CheckCallTypeDecided(node);
		}

		void operator()(MarmotExpression::Function& node) const { Resolve(node.m_body); }
		void operator()(MarmotExpression::Construct& node) const { Resolve(node.m_params); }
		void operator()(MarmotExpression::IfElse& node) const { Resolve(node.m_condition); Resolve(node.m_true_branch); Resolve(node.m_else_branch); }
		void operator()(MarmotExpression::MemberAccess& node) const { Resolve(node.m_struct); }
		void operator()(MarmotExpression::Array& node) const { Resolve(node.m_elems); }
		void operator()(MarmotExpression::IndexAccess& node) const { Resolve(node.m_arr_var); Resolve(node.m_index); }
		void operator()(MarmotExpression::RangeBinary& node) const { Resolve(node.m_start); Resolve(node.m_end); }
		void operator()(MarmotExpression::RangeTernary& node) const { Resolve(node.m_start); Resolve(node.m_step); Resolve(node.m_end); }
		void operator()(MarmotExpression::Match& node) const { Resolve(node.m_arg_expr); Resolve(node.m_cases); }

		void operator()(MarmotExpression::RecordUpdate& node) const
		{
			Resolve(node.m_source);
			std::ranges::for_each(node.m_updates, [this](MarmotExpression::RecordUpdate::FieldUpdate& update) { Resolve(update.m_value); });
		}

		void operator()(MarmotExpression::ArrayComprehension& node) const
		{
			Resolve(node.m_iterable_item_type);
			Resolve(node.m_iterable_next_type);
			Resolve(node.m_transform_expr);
			Resolve(node.m_range);
		}

		void operator()(MarmotExpression::Block& node) const
		{
			std::ranges::for_each(node.m_stmts, [this](std::unique_ptr<MarmotStatement>& statement) { m_self.ResolveRecordedTypes(*statement); });
			if (node.m_final_expr.has_value())
			{
				Resolve(node.m_final_expr.value());
			}
		}

		void operator()(MarmotExpression::Case& node) const
		{
			m_self.ResolveRecordedTypes(*node.m_pattern);
			Resolve(node.m_expr);
			if (node.m_guard.has_value())
			{
				Resolve(node.m_guard.value());
			}
		}

		void operator()(MarmotExpression::For& node) const
		{
			Resolve(node.m_iterable_item_type);
			Resolve(node.m_iterable_next_type);
			Resolve(node.m_range);
			Resolve(node.m_body);
		}
	};

	expression.GetType() = ApplySubstitution(expression.GetType());
	VisitNode(ExpressionResolver{ *this }, expression);
}

void TypeChecker::ResolveRecordedTypes(MarmotPattern& pattern)
{
	pattern.GetType() = ApplySubstitution(pattern.GetType());
	const auto resolve_all = [this](std::vector<std::unique_ptr<MarmotPattern>>& children)
	{
		std::ranges::for_each(children, [this](std::unique_ptr<MarmotPattern>& child) { ResolveRecordedTypes(*child); });
	};
	if (pattern.IsPattern<MarmotPattern::Tuple>())
	{
		resolve_all(pattern.GetPattern<MarmotPattern::Tuple>().m_elements);
	}
	else if (pattern.IsPattern<MarmotPattern::Array>())
	{
		MarmotPattern::Array& array = pattern.GetPattern<MarmotPattern::Array>();
		resolve_all(array.m_elements);
		if (array.m_rest.has_value())
		{
			ResolveRecordedTypes(*array.m_rest->m_pattern);
		}
	}
	else if (pattern.IsPattern<MarmotPattern::Constructor>())
	{
		resolve_all(pattern.GetPattern<MarmotPattern::Constructor>().m_args);
	}
	else if (pattern.IsPattern<MarmotPattern::As>())
	{
		MarmotPattern::As& as = pattern.GetPattern<MarmotPattern::As>();
		ResolveRecordedTypes(*as.m_pattern);
		ResolveRecordedTypes(*as.m_binding);
	}
	else if (pattern.IsPattern<MarmotPattern::Or>())
	{
		resolve_all(pattern.GetPattern<MarmotPattern::Or>().m_alternatives);
	}
	else if (pattern.IsPattern<MarmotPattern::Record>())
	{
		std::ranges::for_each(pattern.GetPattern<MarmotPattern::Record>().m_fields, [this](MarmotPattern::Record::Field& field) { ResolveRecordedTypes(*field.m_pattern); });
	}
}

// Only the call that brings a variable in reports it: one whose argument already
// holds it, as `Id(Make(3))` does, is not where the type went missing.
void TypeChecker::CheckCallTypeDecided(const MarmotExpression::Call& call)
{
	std::unordered_set<int> argument_variables;
	for (const std::unique_ptr<MarmotExpression>& argument : call.m_arguments)
	{
		argument_variables.merge(CollectTypeVariableIds(argument->GetType()));
	}
	const bool is_undecided = std::ranges::any_of(CollectTypeVariableIds(call.m_type_data), [this, &argument_variables](int id)
	{
		return !m_generic_parameter_ids.contains(id) && !argument_variables.contains(id);
	});
	if (!is_undecided)
	{
		return;
	}

	const bool names_callee = call.m_callee->IsExpression<MarmotExpression::NameAccess>();
	const Token& token = names_callee ? call.m_callee->GetExpression<MarmotExpression::NameAccess>().m_name : call.m_paren;
	const std::string what = names_callee ? std::format("'{}' returns here", token.m_lexeme) : std::string("this call returns");
	m_undecided_call_errors.push_back(MarmotError::GenerateTypeCheckerErrorWithContext(std::format("Call expression type error: could not infer the type {}; write the type on the definition that holds the result", what), token, m_file_name, m_source_lines));
}

// Extract type signatures from parsed AST without full type checking
// This allows parallel type checking of dependent modules
TypeChecker::TypeEnvironment TypeChecker::ExtractTypeSignatures(const MarmotProgramTree& ast, const std::unordered_set<std::string>* exported_symbols)
{
	TypeEnvironment signatures;

	for (const std::unique_ptr<MarmotStatement>& statement : ast)
	{
		std::visit(
			[&signatures, exported_symbols]<typename T>(const T& stmt) {
				using Node = std::decay_t<T>;

				if constexpr (std::is_same_v<Node, MarmotStatement::FunctionDefinition>)
				{
					if (exported_symbols == nullptr || exported_symbols->contains(stmt.m_name.m_lexeme))
					{
						std::shared_ptr<MarmotType> function_type = MarmotType::MakeFunctionType(
							stmt.m_param_types,
							std::shared_ptr<MarmotType>(stmt.m_return_type)
						);
						function_type->GetType<MarmotType::FunctionType>().m_constraints = stmt.m_constraints;
						signatures[stmt.m_name.m_lexeme] = function_type;
					}
				}
				else if constexpr (std::is_same_v<Node, MarmotStatement::Struct>)
				{
					if (exported_symbols == nullptr || exported_symbols->contains(stmt.m_name.m_lexeme))
					{
						signatures[stmt.m_name.m_lexeme] = stmt.m_self_type;
					}
				}
				else if constexpr (std::is_same_v<Node, MarmotStatement::Union>)
				{
					if (exported_symbols == nullptr || exported_symbols->contains(stmt.m_name.m_lexeme))
					{
						signatures[stmt.m_name.m_lexeme] = stmt.m_self_type;

						if (stmt.m_self_type->template IsType<MarmotType::UnionType>())
						{
							const MarmotType::UnionType& union_type = stmt.m_self_type->template GetType<MarmotType::UnionType>();
							for (const auto& [member_name, member_ctx] : union_type.m_member_info)
							{
								std::vector<std::shared_ptr<MarmotType>> member_types_copy = member_ctx.m_member_types;
								std::shared_ptr<MarmotType> union_constructor_type = MarmotType::MakeFunctionType(std::move(member_types_copy), std::shared_ptr<MarmotType>(stmt.m_self_type));
								union_constructor_type->GetType<MarmotType::FunctionType>().m_constraints = stmt.m_constraints;
								signatures[member_name] = union_constructor_type;
							}
						}
					}
				}
				else if constexpr (std::is_same_v<Node, MarmotStatement::ForeignDefinition>)
				{
					if (exported_symbols == nullptr || exported_symbols->contains(stmt.m_function_name.m_lexeme))
					{
						signatures[stmt.m_function_name.m_lexeme] = stmt.m_type;
					}
				}
				else if constexpr (std::is_same_v<Node, MarmotStatement::TypeAlias>)
				{
					if (exported_symbols == nullptr || exported_symbols->contains(stmt.m_name.m_lexeme))
					{
						signatures[stmt.m_name.m_lexeme] = stmt.m_aliased_type;
					}
				}
				else if constexpr (std::is_same_v<Node, MarmotStatement::VariableDefinition>)
				{
					if (exported_symbols == nullptr || exported_symbols->contains(stmt.m_name.m_lexeme))
					{
						if (stmt.m_annotated_type.has_value())
						{
							signatures[stmt.m_name.m_lexeme] = stmt.m_annotated_type.value();
						}
						else if (stmt.m_value != nullptr && stmt.m_value->template IsExpression<MarmotExpression::Function>())
						{
							const MarmotExpression::Function& function = stmt.m_value->template GetExpression<MarmotExpression::Function>();
							std::shared_ptr<MarmotType> function_type = MarmotType::MakeFunctionType(
								function.m_param_types,
								std::shared_ptr<MarmotType>(function.m_return_type)
							);
							function_type->GetType<MarmotType::FunctionType>().m_constraints = function.m_constraints;
							signatures[stmt.m_name.m_lexeme] = function_type;
						}
					}
				}
			},
			**statement
		);
	}

	return signatures;
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotStatement::ExpressionStatement& simple)
{
	return Evaluate(simple.m_expr)
		.and_then
		(
			[](std::shared_ptr<MarmotType>&&) ->MarmotResult::TypeResult
			{
				return MarmotType::MakeUndecidedType();
			}
		);
}

MarmotResult::TypeResult TypeChecker::CheckFunctionBody(const std::unique_ptr<MarmotExpression>& body, std::shared_ptr<MarmotType>& return_type, const std::vector<MarmotType::ClassConstraint>& constraints, const Token& token)
{
	ExpectedTypeGuard expected_expr_guard(*this, return_type);
	const auto check_body = [&]() -> MarmotResult::TypeResult
	{
		FunctionBodyContext context(*this, return_type, constraints);
		return Evaluate(body);
	};
	// Unification uses the enclosing constraints, after the body's assumptions
	// have gone out of scope, so the body cannot justify its own result type.
	return check_body().and_then([&](std::shared_ptr<MarmotType>&& body_type)
	{
		return Unify(token, return_type, body_type, UnifyDiagnosticMode::ExpectedActual);
	});
}

MarmotResult::TypeResult TypeChecker::TypeCheckGenericLambdaDefinition(MarmotStatement::VariableDefinition& def, MarmotExpression::Function& function)
{
	if (def.m_annotated_type.has_value())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Function expression type error: a generic lambda cannot carry a type annotation, because its type differs at each call site", function.m_function_keyword, m_file_name, m_source_lines));
	}

	// Bound before the body so the lambda can recurse, and recorded as generic so
	// each use is instantiated instead of pinned to one.
	const std::string& name = def.m_name.m_lexeme;
	return CheckGenericLambda(function, def.m_name, name, [this, &def, &name](const std::shared_ptr<MarmotType>& function_type, const std::vector<int>& parameters)
	{
		def.m_value->GetType() = function_type;
		m_type_scopes.back().m_names[name] = function_type;
		if (def.m_local_index.has_value())
		{
			m_local_generic_functions.emplace_back(m_type_scopes.size(), name, parameters);
		}
		else
		{
			m_generic_functions.insert(name);
		}
	});
}

MarmotResult::TypeResult TypeChecker::CheckGenericLambda(MarmotExpression::Function& function, const Token& anchor, const std::string& name, const std::function<void(const std::shared_ptr<MarmotType>&, const std::vector<int>&)>& bind)
{
	// Validate that generic parameter names are unique
	std::unordered_set<std::string> generic_param_names;
	for (const Token& generic_param : function.m_generic_params)
	{
		if (!generic_param_names.insert(generic_param.m_lexeme).second)
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Function expression type error: duplicate generic parameter name", generic_param, m_file_name, m_source_lines));
		}
	}

	// Validate that generic parameters don't conflict with function parameters
	for (size_t index = 0u; index < function.m_params.Size(); index += 1u)
	{
		if (function.m_params.GeneratedBindingAt(index) != nullptr)
		{
			continue;
		}
		const Token& param = function.m_params.SourceToken(index);
		if (generic_param_names.contains(param.m_lexeme))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Function expression type error: generic parameter conflicts with function parameter", param, m_file_name, m_source_lines));
		}
	}

	std::vector<MarmotType::ClassConstraint> propagated_constraints = CollectSignatureConstraints(function.m_param_types, function.m_return_type);
	for (MarmotType::ClassConstraint& propagated_constraint : propagated_constraints)
	{
		AppendUniqueConstraint(function.m_constraints, std::move(propagated_constraint));
	}

	for (const MarmotType::ClassConstraint& constraint : function.m_constraints)
	{
		if (!m_classes.contains(ConstrainedClassName(constraint)))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Function expression type error: undefined class '" + ConstrainedClassName(constraint) + "' in constraint", function.m_function_keyword, m_file_name, m_source_lines));
		}

		// An equality constraint names no class of its own, so the arity check
		// below does not apply to it.
		if (constraint.IsEquality())
		{
			continue;
		}

		const ClassInfo& tc_info = m_classes.at(constraint.m_class_name);

		if (constraint.m_type_args.size() != tc_info.m_type_param_names.size())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Function expression type error: class '" + constraint.m_class_name + "' expects " + std::to_string(tc_info.m_type_param_names.size()) + " type argument(s) but got " + std::to_string(constraint.m_type_args.size()), function.m_function_keyword, m_file_name, m_source_lines));
		}
	}

	TypeTransform::Freshener freshening_context([this]() { return FreshTypeVar(); });
	for (std::shared_ptr<MarmotType>& param_type : function.m_param_types)
	{
		param_type = freshening_context.Apply(param_type);
	}
	function.m_return_type = freshening_context.Apply(function.m_return_type);
	// An equality constraint's types are its operands, which Map freshens with the
	// signature: otherwise the projection would never match the one built at the
	// use site.
	function.m_constraints = MarmotType::MapConstraints(function.m_constraints, [&freshening_context](const std::shared_ptr<MarmotType>& constraint_type) { return freshening_context.Apply(constraint_type); });

	std::shared_ptr<MarmotType> return_type_copy = function.m_return_type;
	std::shared_ptr<MarmotType> function_type = MarmotType::MakeFunctionType(function.m_param_types, std::move(return_type_copy));
	function_type->GetType<MarmotType::FunctionType>().m_constraints = function.m_constraints;
	function.m_type_data = function_type;
	bind(function_type, freshening_context.Bindings()
		| std::views::values
		| std::views::filter([](const std::shared_ptr<MarmotType>& type) { return type->IsType<MarmotType::TypeVariable>(); })
		| std::views::transform([](const std::shared_ptr<MarmotType>& type) { return type->GetType<MarmotType::TypeVariable>().m_id; })
		| std::ranges::to<std::vector>());

	return ScopeSession(*this).Then([&]() -> MarmotResult::TypeResult
	{
		for (const Token& generic_param : function.m_generic_params)
		{
			const std::string& param_name = generic_param.m_lexeme;
			m_type_scopes.back().m_names.emplace(param_name, freshening_context.Parameter(param_name));
		}

		BindParameters(function.m_params, function.m_param_types);

		// Inside its own body, a recursive call is not a fresh instantiation.
		DefiningGenericGuard defining_guard(*this, name);
		RigidTypeVariablesGuard rigid_guard(*this, freshening_context.Bindings());
		const size_t pending_start = m_pending_instances.size();
		return CheckFunctionBody(function.m_body, function.m_return_type, function.m_constraints, function.m_function_keyword)
			.and_then([&function, &rigid_guard, pending_start, &anchor, this](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
			{
				if (std::optional<CompilerError> error = CheckGenericParametersStayGeneric(rigid_guard.Parameters(), function.m_generic_params, anchor))
				{
					return std::unexpected(std::move(*error));
				}
				if (std::optional<CompilerError> error = CheckDecidedPendingInstances(pending_start))
				{
					return std::unexpected(std::move(*error));
				}
				return MarmotType::MakeUndecidedType();
			});
	});
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotStatement::VariableDefinition& def)
{
	if (def.m_annotated_type.has_value())
	{
		def.m_annotated_type = InstantiateWrittenType(def.m_annotated_type.value());
	}

	// Special handling for functions (scope management required)
	if (def.m_value->IsExpression<MarmotExpression::Function>())
	{
		MarmotExpression::Function& function = def.m_value->GetExpression<MarmotExpression::Function>();

		// A generic lambda is a definition, not an ordinary initializer: it must be
		// registered by name so each use site is instantiated separately.
		if (!function.m_generic_params.empty())
		{
			return TypeCheckGenericLambdaDefinition(def, function);
		}

		bool has_inferred_param_types = std::ranges::any_of
		(
			function.m_param_types,
			[](const std::shared_ptr<MarmotType>& param_type)
			{
				return param_type->IsType<MarmotType::UndecidedType>();
			}
		);

		if (!def.m_annotated_type.has_value() && has_inferred_param_types)
		{
			return std::unexpected
			(
				MarmotError::GenerateTypeCheckerErrorWithContext
				(
					"Function expression type error: could not infer all lambda parameter or return types",
					function.m_function_keyword,
					m_file_name,
					m_source_lines
				)
			);
		}

		// Freshen any UndecidedType parameters to TypeVariables
		for (std::shared_ptr<MarmotType>& param_type : function.m_param_types)
		{
			param_type = Freshen(param_type);
		}
		function.m_return_type = Freshen(function.m_return_type);

		std::shared_ptr<MarmotType> return_type_copy = function.m_return_type;
		std::shared_ptr<MarmotType> function_type = MarmotType::MakeFunctionType(function.m_param_types, std::move(return_type_copy));
		function.m_type_data = function_type;
		def.m_value->GetType() = function.m_type_data;

		if (def.m_annotated_type.has_value())
		{
			std::shared_ptr<MarmotType>& annotated_type = def.m_annotated_type.value();
			MarmotResult::TypeResult annotation_result = Unify(def.m_name, function_type, annotated_type, UnifyDiagnosticMode::ActualExpected);
			if (!annotation_result.has_value())
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Define statement type error: function type annotation doesn't match function signature", def.m_name, m_file_name, m_source_lines, function_type, annotated_type));
			}

			function.m_type_data = ApplySubstitution(function_type);
			def.m_value->GetType() = function.m_type_data;
			const MarmotType::FunctionType& resolved_function_type = function.m_type_data->GetType<MarmotType::FunctionType>();
			function.m_param_types = resolved_function_type.m_param_types;
			function.m_return_type = resolved_function_type.m_return_type;
		}

		std::vector<MarmotType::ClassConstraint> function_constraints = CollectSignatureConstraints(function.m_param_types, function.m_return_type);
		def.m_value->GetType()->GetType<MarmotType::FunctionType>().m_constraints = function_constraints;

		m_type_scopes.back().m_names.emplace(def.m_name.m_lexeme, def.m_value->GetType());
		MarmotType::FunctionType& function_type_ref = def.m_value->GetType()->GetType<MarmotType::FunctionType>();

		return ScopeSession(*this).Then([&]() -> MarmotResult::TypeResult
		{
			BindParameters(function.m_params, function_type_ref.m_param_types);

			return Evaluate(def.m_value)
				.and_then
				(
					[&def, &function, this](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
					{
						std::shared_ptr<MarmotType> resolved_function_type = ApplySubstitution(def.m_value->GetType());
						def.m_value->GetType() = resolved_function_type;
						function.m_type_data = resolved_function_type;

						if (resolved_function_type->IsType<MarmotType::FunctionType>())
						{
							const MarmotType::FunctionType& resolved_signature = resolved_function_type->GetType<MarmotType::FunctionType>();
							function.m_param_types = resolved_signature.m_param_types;
							function.m_return_type = resolved_signature.m_return_type;
						}

						if (HasTypeVariables(resolved_function_type))
						{
							return std::unexpected
							(
								MarmotError::GenerateTypeCheckerErrorWithContext
								(
									"Function expression type error: could not infer all lambda parameter or return types",
									function.m_function_keyword,
									m_file_name,
									m_source_lines
								)
							);
						}

						if (std::shared_ptr<MarmotType>* binding = FindNameType(def.m_name.m_lexeme))
						{
							*binding = resolved_function_type;
						}

						return MarmotType::MakeUndecidedType();
					}
				);
		});
	}

	// Set expected type from annotation if present
	std::unique_ptr<ExpectedTypeGuard> expected_type_guard;
	if (def.m_annotated_type.has_value())
	{
		expected_type_guard = std::make_unique<ExpectedTypeGuard>(*this, def.m_annotated_type.value());
	}

	// General case
	return Evaluate(def.m_value)
		.and_then
		(
			[&def, this](std::shared_ptr<MarmotType>&& type)->MarmotResult::TypeResult
			{
				if (def.m_annotated_type.has_value())
				{
					std::shared_ptr<MarmotType>& annotated_type = def.m_annotated_type.value();

					// Reject NeverType values with concrete type annotations
					// NeverType represents expressions that never return (infinite loops, unconditional returns/breaks)
					// They cannot be assigned to variables with concrete types
					if (type->IsType<MarmotType::NeverType>() && !annotated_type->IsType<MarmotType::NeverType>())
					{
						return std::unexpected(
							MarmotError::GenerateTypeCheckerErrorWithContext(
								"Cannot assign a never-returning expression to a variable with type " + annotated_type->DisplayString(),
								def.m_name,
								m_file_name,
								m_source_lines
							)
						);
					}

					return Unify(def.m_name, annotated_type, type, UnifyDiagnosticMode::ExpectedActual)
						.and_then
						(
							[&def, &annotated_type, this](std::shared_ptr<MarmotType>&& unified_type)->MarmotResult::TypeResult
							{
								m_type_scopes.back().m_names.emplace(def.m_name.m_lexeme, annotated_type);
								return unified_type;
							}
						);
				}

				// Check if this is an empty array without type annotation
				if (type->IsType<MarmotType::ArrayType>())
				{
					const MarmotType::ArrayType& array_type = type->GetType<MarmotType::ArrayType>();
					if (array_type.m_element_type->IsType<MarmotType::UndecidedType>())
					{
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Define statement type error: empty arrays require type annotations. Use 'def " + def.m_name.m_lexeme + " : Array<ElementType> = [];'", def.m_name, m_file_name, m_source_lines));
					}
				}

				m_type_scopes.back().m_names.emplace(def.m_name.m_lexeme, type);
				return MarmotType::MakeUndecidedType();
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotStatement::TupleDefinition& def_tuple)
{
	return Evaluate(def_tuple.m_value)
		.and_then
		(
			[&def_tuple, this](std::shared_ptr<MarmotType>&& type)->MarmotResult::TypeResult
			{
				if (!type->IsType<MarmotType::TupleType>())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("DefineTuple statement type error: expected tuple expression on right-hand side", def_tuple.m_names[0], m_file_name, m_source_lines));
				}

				const MarmotType::TupleType& tuple_type = type->GetType<MarmotType::TupleType>();


				if (def_tuple.m_names.size() != tuple_type.m_element_types.size())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("DefineTuple statement type error: tuple pattern has " + std::to_string(def_tuple.m_names.size()) + " bindings but tuple has " + std::to_string(tuple_type.m_element_types.size()) + " elements", def_tuple.m_names[0u], m_file_name, m_source_lines));
				}

				for (size_t i = 0u; i < def_tuple.m_names.size(); i += 1u)
				{
					m_type_scopes.back().m_names.emplace(def_tuple.m_names[i].m_lexeme, tuple_type.m_element_types[i]);
				}

				return MarmotType::MakeUndecidedType();
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotStatement::FunctionDefinition& defun)
{
	// If function has generic parameters, check for lambda syntax error
	if (!defun.m_generic_params.empty())
	{
		// Validate that generic parameter names are unique
		std::unordered_set<std::string> generic_param_names;
		for (const Token& generic_param : defun.m_generic_params)
		{
			if (!generic_param_names.insert(generic_param.m_lexeme).second)
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("DefineFunction type error: duplicate generic parameter name", generic_param, m_file_name, m_source_lines));
			}
		}

		// Validate that generic parameters don't conflict with function parameters
		for (size_t index = 0u; index < defun.m_params.Size(); index += 1u)
		{
			if (defun.m_params.GeneratedBindingAt(index) != nullptr)
			{
				continue;
			}
			const Token& param = defun.m_params.SourceToken(index);
			if (generic_param_names.contains(param.m_lexeme))
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("DefineFunction type error: generic parameter conflicts with function parameter", param, m_file_name, m_source_lines));
			}
		}
	}

	std::vector<MarmotType::ClassConstraint> propagated_constraints = CollectSignatureConstraints(defun.m_param_types, defun.m_return_type);
	for (MarmotType::ClassConstraint& propagated_constraint : propagated_constraints)
	{
		AppendUniqueConstraint(defun.m_constraints, std::move(propagated_constraint));
	}

	for (const MarmotType::ClassConstraint& constraint : defun.m_constraints)
	{
		if (!m_classes.contains(ConstrainedClassName(constraint)))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("DefineFunction type error: undefined class '" + ConstrainedClassName(constraint) + "' in constraint", defun.m_name, m_file_name, m_source_lines));
		}

		// An equality constraint names no class of its own, so the arity check
		// below does not apply to it.
		if (constraint.IsEquality())
		{
			continue;
		}

		const ClassInfo& tc_info = m_classes.at(constraint.m_class_name);

		if (constraint.m_type_args.size() != tc_info.m_type_param_names.size())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("DefineFunction type error: class '" + constraint.m_class_name + "' expects " + std::to_string(tc_info.m_type_param_names.size()) + " type argument(s) but got " + std::to_string(constraint.m_type_args.size()), defun.m_name, m_file_name, m_source_lines));
		}
	}

	TypeTransform::Freshener freshening_context([this]() { return FreshTypeVar(); });
	for (std::shared_ptr<MarmotType>& param_type : defun.m_param_types)
	{
		param_type = freshening_context.Apply(param_type);
	}
	defun.m_return_type = freshening_context.Apply(defun.m_return_type);
	// An equality constraint's types are its operands, which Map freshens with the
	// signature: otherwise the projection would never match the one built at the
	// use site.
	defun.m_constraints = MarmotType::MapConstraints(defun.m_constraints, [&freshening_context](const std::shared_ptr<MarmotType>& constraint_type) { return freshening_context.Apply(constraint_type); });

	std::shared_ptr<MarmotType> return_type_copy = defun.m_return_type;
	std::shared_ptr<MarmotType> function_type = MarmotType::MakeFunctionType(defun.m_param_types, std::move(return_type_copy));
	function_type->GetType<MarmotType::FunctionType>().m_constraints = defun.m_constraints;
	m_type_scopes.back().m_names[defun.m_name.m_lexeme] = function_type;
	if (!defun.m_generic_params.empty())
	{
		m_generic_functions.insert(defun.m_name.m_lexeme);
	}

	return ScopeSession(*this).Then([&]() -> MarmotResult::TypeResult
	{
		for (const Token& generic_param : defun.m_generic_params)
		{
			const std::string& param_name = generic_param.m_lexeme;
			m_type_scopes.back().m_names.emplace(param_name, freshening_context.Parameter(param_name));
		}
		BindParameters(defun.m_params, defun.m_param_types);

		RigidTypeVariablesGuard rigid_guard(*this, freshening_context.Bindings());
		const size_t pending_start = m_pending_instances.size();
		return CheckFunctionBody(defun.m_body, defun.m_return_type, defun.m_constraints, defun.m_name)
			.and_then([&defun, &rigid_guard, pending_start, this](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
			{
				if (std::optional<CompilerError> error = CheckGenericParametersStayGeneric(rigid_guard.Parameters(), defun.m_generic_params, defun.m_name))
				{
					return std::unexpected(std::move(*error));
				}
				if (std::optional<CompilerError> error = CheckDecidedPendingInstances(pending_start))
				{
					return std::unexpected(std::move(*error));
				}
				return defun.m_return_type;
			});
	});
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotStatement::ForeignDefinition& foreign)
{
	if (foreign.DeclaredPrimitive().has_value())
	{
		if (std::optional<CompilerError> error = CheckPrimitiveDeclaration(foreign))
		{
			return std::unexpected(std::move(*error));
		}
	}

	m_type_scopes.back().m_names[foreign.m_function_name.m_lexeme] = foreign.m_type;
	return MarmotType::MakeUndecidedType();
}

// The VM gives a primitive's instruction only what its declaration says, so the
// declaration must say what the instruction takes and gives.
std::optional<CompilerError> TypeChecker::CheckPrimitiveDeclaration(const MarmotStatement::ForeignDefinition& foreign) const
{
	const MarmotType::FunctionType& signature = foreign.m_type->GetType<MarmotType::FunctionType>();
	const std::vector<std::shared_ptr<MarmotType>>& params = signature.m_param_types;
	const std::shared_ptr<MarmotType>& result = signature.m_return_type;
	const bool takes_one = params.size() == 1u;
	struct DeclaredShape
	{
		bool m_matches;
		std::string_view m_expected;
	};
	const DeclaredShape shape = [&]() -> DeclaredShape
	{
		switch (foreign.DeclaredPrimitive().value())
		{
		case MarmotPrimitive::Spawn:
			return { params.size() == 2u && IsSpawnSignature(params[0u], params[1u], result), "fn(A, fn(A) -> R) -> Worker<R>" };
		case MarmotPrimitive::Join:
			return { takes_one && params[0u]->IsType<MarmotType::WorkerType>() && IsJoinOutcome(result, params[0u]->GetType<MarmotType::WorkerType>().m_result_type), "fn(Worker<T>) -> R, a union whose member 0 holds T and member 1 an error, itself a union whose member 0 holds nothing and member 1 a Text" };
		case MarmotPrimitive::ChannelNew:
			return { takes_one && params[0u]->IsType<MarmotType::IntegerType>() && result->IsType<MarmotType::ChannelType>(), "fn(Int) -> Channel<T>" };
		case MarmotPrimitive::ChannelClose:
			return { takes_one && params[0u]->IsType<MarmotType::ChannelType>() && result->IsType<MarmotType::UnitType>(), "fn(Channel<T>) -> Unit" };
		case MarmotPrimitive::WorkerIsDone:
		case MarmotPrimitive::WorkerCancel:
			return { takes_one && params[0u]->IsType<MarmotType::WorkerType>() && result->IsType<MarmotType::BoolType>(), "fn(Worker<T>) -> Bool" };
		}
		std::unreachable();
	}();

	if (shape.m_matches)
	{
		return std::nullopt;
	}
	return MarmotError::GenerateTypeCheckerErrorWithContext(std::format("Foreign declaration error: '{}' declares the primitive \"{}\", whose type must be {}", foreign.m_function_name.m_lexeme, foreign.m_foreign_name, shape.m_expected), foreign.m_function_name, m_file_name, m_source_lines);
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotStatement::Struct& struct_stmt)
{
	if (!struct_stmt.m_generic_params.empty())
	{
		std::unordered_set<std::string> generic_param_names;
		for (const Token& generic_param : struct_stmt.m_generic_params)
		{
			if (!generic_param_names.insert(generic_param.m_lexeme).second)
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Struct declaration type error: duplicate generic parameter name", generic_param, m_file_name, m_source_lines));
			}
		}

		m_generic_structs.insert(ConstructorKey(struct_stmt.m_self_type->GetType<MarmotType::StructType>().m_module_name, struct_stmt.m_name.m_lexeme));
	}

	if (!struct_stmt.m_constraints.empty())
	{
		if (struct_stmt.m_generic_params.empty())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Struct declaration error: constraints require at least one type parameter", struct_stmt.m_name, m_file_name, m_source_lines));
		}

		for (const MarmotType::ClassConstraint& constraint : struct_stmt.m_constraints)
		{
			if (!m_classes.contains(ConstrainedClassName(constraint)))
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Struct declaration error: undefined class '" + ConstrainedClassName(constraint) + "' in constraint", struct_stmt.m_name, m_file_name, m_source_lines));
			}

			// An equality constraint names no class of its own, so the arity check
			// below does not apply to it.
			if (constraint.IsEquality())
			{
				continue;
			}

			const ClassInfo& tc_info = m_classes.at(constraint.m_class_name);
			if (constraint.m_type_args.size() != tc_info.m_type_param_names.size())
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Struct declaration error: class '" + constraint.m_class_name + "' expects " + std::to_string(tc_info.m_type_param_names.size()) + " type argument(s) but got " + std::to_string(constraint.m_type_args.size()), struct_stmt.m_name, m_file_name, m_source_lines));
			}
		}
	}

	struct_stmt.m_self_type->GetType<MarmotType::StructType>().m_constraints = struct_stmt.m_constraints;
	const std::string struct_module_name = struct_stmt.m_self_type->GetType<MarmotType::StructType>().m_module_name;
	std::shared_ptr<MarmotType> struct_constructor_type = MarmotType::MakeFunctionType(struct_stmt.m_self_type->GetType<MarmotType::StructType>().m_member_types, std::move(struct_stmt.m_self_type));
	struct_constructor_type->GetType<MarmotType::FunctionType>().m_constraints = struct_stmt.m_constraints;
	m_type_scopes.back().m_names[struct_stmt.m_name.m_lexeme] = struct_constructor_type;
	m_type_scopes.back().m_names[ConstructorKey(struct_module_name, struct_stmt.m_name.m_lexeme)] = struct_constructor_type;
	m_struct_type_definitions[ConstructorKey(struct_module_name, struct_stmt.m_name.m_lexeme)] = struct_constructor_type->GetType<MarmotType::FunctionType>().m_return_type;

	return MarmotType::MakeUndecidedType();
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotStatement::Union& union_stmt)
{
	if (!union_stmt.m_generic_params.empty())
	{
		std::unordered_set<std::string> generic_param_names;
		for (const Token& generic_param : union_stmt.m_generic_params)
		{
			if (!generic_param_names.insert(generic_param.m_lexeme).second)
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Union declaration type error: duplicate generic parameter name", generic_param, m_file_name, m_source_lines));
			}
		}

		m_generic_unions.insert(ConstructorKey(union_stmt.m_self_type->GetType<MarmotType::UnionType>().m_module_name, union_stmt.m_name.m_lexeme));
	}

	if (!union_stmt.m_constraints.empty())
	{
		if (union_stmt.m_generic_params.empty())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Union declaration error: constraints require at least one type parameter", union_stmt.m_name, m_file_name, m_source_lines));
		}

		for (const MarmotType::ClassConstraint& constraint : union_stmt.m_constraints)
		{
			if (!m_classes.contains(ConstrainedClassName(constraint)))
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Union declaration error: undefined class '" + ConstrainedClassName(constraint) + "' in constraint", union_stmt.m_name, m_file_name, m_source_lines));
			}

			// An equality constraint names no class of its own, so the arity check
			// below does not apply to it.
			if (constraint.IsEquality())
			{
				continue;
			}

			const ClassInfo& tc_info = m_classes.at(constraint.m_class_name);
			if (constraint.m_type_args.size() != tc_info.m_type_param_names.size())
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Union declaration error: class '" + constraint.m_class_name + "' expects " + std::to_string(tc_info.m_type_param_names.size()) + " type argument(s) but got " + std::to_string(constraint.m_type_args.size()), union_stmt.m_name, m_file_name, m_source_lines));
			}
		}
	}

	MarmotType::UnionType& union_type = union_stmt.m_self_type->GetType<MarmotType::UnionType>();
	union_type.m_constraints = union_stmt.m_constraints;
	for (auto& [member_name, member_ctx] : union_type.m_member_info)
	{
		std::vector<std::shared_ptr<MarmotType>> member_types_copy = member_ctx.m_member_types;
		std::shared_ptr<MarmotType> union_constructor_type = MarmotType::MakeFunctionType(std::move(member_types_copy), std::shared_ptr(union_stmt.m_self_type));
		union_constructor_type->GetType<MarmotType::FunctionType>().m_constraints = union_stmt.m_constraints;
		m_type_scopes.back().m_names[member_name] = union_constructor_type;
		m_type_scopes.back().m_names[ConstructorKey(union_type.m_module_name, member_name)] = union_constructor_type;
	}
	m_union_type_definitions[ConstructorKey(union_type.m_module_name, union_stmt.m_name.m_lexeme)] = union_stmt.m_self_type;

	return MarmotType::MakeUndecidedType();
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotStatement::Class& class_stmt)
{
	std::unordered_set<std::string> type_param_names;
	for (const Token& type_param : class_stmt.m_type_params)
	{
		if (!type_param_names.insert(type_param.m_lexeme).second)
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Class declaration error: duplicate type parameter name", type_param, m_file_name, m_source_lines));
		}
	}

	if (m_classes.contains(class_stmt.m_name.m_lexeme))
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Class declaration error: class already defined", class_stmt.m_name, m_file_name, m_source_lines));
	}

	AssociatedTypeEnvironment associated_types;
	for (const MarmotStatement::Class::AssociatedTypeDeclaration& associated_type_decl : class_stmt.m_associated_types)
	{
		if (type_param_names.contains(associated_type_decl.m_name.m_lexeme))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Class declaration error: associated type name conflicts with class type parameter", associated_type_decl.m_name, m_file_name, m_source_lines));
		}
		if (associated_types.contains(associated_type_decl.m_name.m_lexeme))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Class declaration error: duplicate associated type name", associated_type_decl.m_name, m_file_name, m_source_lines));
		}

		std::vector<std::shared_ptr<MarmotType>> associated_type_args;
		associated_type_args.reserve(class_stmt.m_type_params.size());
		for (const Token& type_param : class_stmt.m_type_params)
		{
			associated_type_args.emplace_back(MarmotType::MakeGenericType(type_param.m_lexeme));
		}

		associated_types.emplace
		(
			associated_type_decl.m_name.m_lexeme,
			MarmotType::MakeAssociatedType(class_stmt.m_name.m_lexeme, associated_type_decl.m_name.m_lexeme, std::move(associated_type_args))
		);
	}

	TypeEnvironment method_types;
	for (std::unique_ptr<MarmotStatement>& method : class_stmt.m_methods)
	{
		if (!method->IsStatement<MarmotStatement::FunctionDefinition>())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Class declaration error: methods must be function definitions", class_stmt.m_name, m_file_name, m_source_lines));
		}

		MarmotStatement::FunctionDefinition& defun = method->GetStatement<MarmotStatement::FunctionDefinition>();
		if (method_types.contains(defun.m_name.m_lexeme))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Class declaration error: duplicate method name", defun.m_name, m_file_name, m_source_lines));
		}

		std::shared_ptr<MarmotType> method_type = MarmotType::MakeFunctionType(defun.m_param_types, std::shared_ptr<MarmotType>(defun.m_return_type));

		method_types[defun.m_name.m_lexeme] = method_type;
	}

	std::vector<std::string> param_names;
	for (const Token& param : class_stmt.m_type_params)
	{
		param_names.emplace_back(param.m_lexeme);
	}

	m_classes[class_stmt.m_name.m_lexeme] = ClassInfo(class_stmt.m_name.m_lexeme, std::move(param_names), std::move(associated_types), std::move(method_types));

	return MarmotType::MakeUndecidedType();
}

// An instance is registered with the declarations, so code anywhere in the
// module can select it; its method bodies are checked where it stands.
MarmotResult::TypeResult TypeChecker::RegisterInstance(MarmotStatement::Instance& instance_stmt)
{
	std::unordered_map<std::string, ClassInfo>::iterator tc_it = m_classes.find(instance_stmt.m_class_name.m_lexeme);
	if (tc_it == m_classes.end())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: unknown class '" + instance_stmt.m_class_name.m_lexeme + "'", instance_stmt.m_class_name, m_file_name, m_source_lines));
	}

	const ClassInfo& tc_info = tc_it->second;
	if (instance_stmt.m_type_args.size() != tc_info.m_type_param_names.size())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: type argument count mismatch", instance_stmt.m_class_name, m_file_name, m_source_lines));
	}

	// The compiler, not an instance, does arithmetic and bit operations, so an
	// instance for a type it cannot do them on would let a generic body claim
	// what no specialization can do.
	const std::shared_ptr<MarmotType>& first_type_arg = instance_stmt.m_type_args.front();
	if (instance_stmt.m_class_name.m_lexeme == NUMERIC_CLASS_NAME && !first_type_arg->IsNumericType())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnsatisfiedConstraint, std::format("Instance declaration error: the compiler does arithmetic itself, on Int, Float, Byte and Word only, so there is no Numeric<{}>", first_type_arg->DisplayString()), instance_stmt.m_class_name, m_file_name, m_source_lines));
	}
	if (instance_stmt.m_class_name.m_lexeme == BITWISE_CLASS_NAME && !first_type_arg->IsType<MarmotType::IntegerType>() && !first_type_arg->IsType<MarmotType::ByteType>() && !first_type_arg->IsType<MarmotType::WordType>())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnsatisfiedConstraint, std::format("Instance declaration error: the compiler does bit operations itself, on Int, Byte and Word only, so there is no Bitwise<{}>", first_type_arg->DisplayString()), instance_stmt.m_class_name, m_file_name, m_source_lines));
	}

	std::vector<std::string> concrete_type_names;
	for (const std::shared_ptr<MarmotType>& type_arg : instance_stmt.m_type_args)
	{
		concrete_type_names.push_back(type_arg->ToString());
	}

	InstanceKey instance_key{instance_stmt.m_class_name.m_lexeme, concrete_type_names};

	std::unordered_map<InstanceKey, InstanceInfo, InstanceKeyHash>::iterator existing_instance_it = m_instances.find(instance_key);
	if (existing_instance_it != m_instances.end())
	{
		// A derived identity conversion is only a stand-in for the instance the user did
		// not write. An explicit declaration with real behaviour replaces it rather than
		// colliding with it; the type alias that derived it is always checked first, so
		// this is the only order in which the two can meet.
		if (!existing_instance_it->second.m_is_derived)
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: instance already defined for this type", instance_stmt.m_class_name, m_file_name, m_source_lines));
		}

		m_instances.erase(existing_instance_it);
	}

	AssociatedTypeEnvironment associated_type_bindings;
	for (const MarmotStatement::Instance::AssociatedTypeBinding& binding : instance_stmt.m_associated_types)
	{
		if (!tc_info.m_associated_types.contains(binding.m_name.m_lexeme))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: unknown associated type '" + binding.m_name.m_lexeme + "'", binding.m_name, m_file_name, m_source_lines));
		}
		if (associated_type_bindings.contains(binding.m_name.m_lexeme))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: duplicate associated type binding", binding.m_name, m_file_name, m_source_lines));
		}

		associated_type_bindings.emplace(binding.m_name.m_lexeme, binding.m_type);
	}

	for (const auto& [associated_type_name, _] : tc_info.m_associated_types)
	{
		if (!associated_type_bindings.contains(associated_type_name))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: missing binding for associated type '" + associated_type_name + "'", instance_stmt.m_class_name, m_file_name, m_source_lines));
		}
	}

	m_instances.emplace
	(
		instance_key,
		InstanceInfo
		(
			instance_stmt.m_class_name.m_lexeme,
			std::vector<std::shared_ptr<MarmotType>>(instance_stmt.m_type_args),
			std::vector<MarmotType::ClassConstraint>(instance_stmt.m_constraints),
			AssociatedTypeEnvironment(associated_type_bindings),
			std::unordered_map<std::string, std::unique_ptr<MarmotStatement>>()
		)
	);

	struct InstanceRegistrationGuard
	{
		std::unordered_map<InstanceKey, InstanceInfo, InstanceKeyHash>* m_instances = nullptr;
		InstanceKey m_key;
		bool m_keep = false;

		~InstanceRegistrationGuard()
		{
			if (m_instances != nullptr && !m_keep)
			{
				m_instances->erase(m_key);
			}
		}
	} registration_guard{ &m_instances, instance_key, false };

	std::unordered_set<std::string> implemented_methods;
	for (const MarmotStatement::Instance::Method& method : instance_stmt.m_methods)
	{
		const std::string& method_name = method.m_name.m_lexeme;

		if (!tc_info.m_method_types.contains(method_name))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: method '" + method_name + "' not defined in typeclass", method.m_name, m_file_name, m_source_lines));
		}

		if (implemented_methods.contains(method_name))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: duplicate method implementation", method.m_name, m_file_name, m_source_lines));
		}

		implemented_methods.emplace(method_name);
	}

	for (const auto& [method_name, method_type] : tc_info.m_method_types)
	{
		if (!implemented_methods.contains(method_name))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: missing implementation for method '" + method_name + "'", instance_stmt.m_class_name, m_file_name, m_source_lines));
		}
	}

	TypeEnvironment type_param_substitutions;
	for (size_t i = 0u; i < tc_info.m_type_param_names.size(); i += 1u)
	{
		type_param_substitutions.emplace(tc_info.m_type_param_names[i], instance_stmt.m_type_args[i]);
	}

	for (const MarmotStatement::Instance::Method& method : instance_stmt.m_methods)
	{
		const MarmotStatement::FunctionDefinition& defun = method.m_definition;
		const std::string& method_name = method.m_name.m_lexeme;

		if (!defun.m_generic_params.empty())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: instance methods cannot declare generic parameters", method.m_name, m_file_name, m_source_lines));
		}

		TypeEnvironment::const_iterator expected_it = tc_info.m_method_types.find(method_name);
		if (expected_it == tc_info.m_method_types.cend())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: method '" + method_name + "' not defined in class", method.m_name, m_file_name, m_source_lines));
		}

		std::shared_ptr<MarmotType> expected_signature = ApplySubstitution(TypeTransform::Instantiate(expected_it->second, type_param_substitutions));
		std::shared_ptr<MarmotType> actual_signature = ApplySubstitution(MarmotType::MakeFunctionType(defun.m_param_types, std::shared_ptr<MarmotType>(defun.m_return_type)));

		if (*expected_signature != *actual_signature)
		{
			if (expected_signature->IsType<MarmotType::FunctionType>() && actual_signature->IsType<MarmotType::FunctionType>())
			{
				const MarmotType::FunctionType& expected_type = expected_signature->GetType<MarmotType::FunctionType>();
				const MarmotType::FunctionType& actual_type = actual_signature->GetType<MarmotType::FunctionType>();

				if (expected_type.m_param_types.size() != actual_type.m_param_types.size())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: method '" + method_name + "' parameter count does not match class declaration", method.m_name, m_file_name, m_source_lines, actual_signature, expected_signature));
				}

				for (size_t i = 0u; i < expected_type.m_param_types.size(); i += 1u)
				{
					const std::shared_ptr<MarmotType>& expected_param = expected_type.m_param_types[i];
					const std::shared_ptr<MarmotType>& actual_param = actual_type.m_param_types[i];
					if (*expected_param != *actual_param)
					{
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(std::format("Instance declaration error: method '{}' parameter {} type does not match class declaration", method_name, i + 1u), method.m_name, m_file_name, m_source_lines, actual_param, expected_param));
					}
				}

				if (*expected_type.m_return_type != *actual_type.m_return_type)
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: method '" + method_name + "' return type does not match class declaration", method.m_name, m_file_name, m_source_lines, actual_type.m_return_type, expected_type.m_return_type));
				}
			}

			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: method '" + method_name + "' signature does not match class declaration", method.m_name, m_file_name, m_source_lines, actual_signature, expected_signature));
		}
	}

	registration_guard.m_keep = true;
	return MarmotType::MakeUndecidedType();
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotStatement::Instance& instance_stmt)
{
	// AppendUniqueConstraint does NOT make this append idempotent: on a second visit the stored
	// constraint has been freshened in place by the function-definition path while the incoming one has not,
	// so they compare unequal and a duplicate would accumulate. Safety comes instead from this
	// statement being visited at most once - TypeCheck traverses m_program_tree in a single pass,
	// the duplicate-instance guard above rejects a repeated instance key before reaching here,
	// and each module gets its own TypeChecker owning its own tree (imports carry instance
	// metadata, not Instance AST nodes). Preserve that property before reusing this loop.
	for (MarmotStatement::Instance::Method& method : instance_stmt.m_methods)
	{
		MarmotStatement::FunctionDefinition& defun = method.m_definition;
		for (const MarmotType::ClassConstraint& constraint : instance_stmt.m_constraints)
		{
			AppendUniqueConstraint(defun.m_constraints, MarmotType::ClassConstraint(constraint));
		}

		MarmotResult::TypeResult result = (*this)(defun);
		if (!result.has_value())
		{
			return result;
		}
	}

	return MarmotType::MakeUndecidedType();
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotStatement::TypeAlias& type_alias)
{
	if (!type_alias.m_generic_params.empty())
	{
		std::unordered_set<std::string> generic_param_names;
		for (const Token& generic_param : type_alias.m_generic_params)
		{
			if (!generic_param_names.insert(generic_param.m_lexeme).second)
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Type alias declaration error: duplicate generic parameter name", generic_param, m_file_name, m_source_lines));
			}
		}
	}

	if (type_alias.m_aliased_type->IsType<MarmotType::NewType>())
	{
		const MarmotType::NewType& new_type = type_alias.m_aliased_type->GetType<MarmotType::NewType>();
		RegisterIdentityConversion(type_alias.m_aliased_type, new_type.m_representation);
		RegisterIdentityConversion(new_type.m_representation, type_alias.m_aliased_type);
	}

	// Type alias is already registered in the parser's type table
	// Nothing more to do at type-checking time - the alias is resolved at parse time
	return MarmotType::MakeUndecidedType();
}

void TypeChecker::RegisterIdentityConversion(const std::shared_ptr<MarmotType>& from_type, const std::shared_ptr<MarmotType>& to_type)
{
	InstanceKey conversion_key{ std::string(CONVERTABLE_CLASS_NAME), { from_type->ToString(), to_type->ToString() } };
	if (m_instances.contains(conversion_key))
	{
		return;
	}

	std::vector<std::shared_ptr<MarmotType>> type_args{ from_type, to_type };
	m_instances.emplace
	(
		std::move(conversion_key),
		InstanceInfo(std::string(CONVERTABLE_CLASS_NAME), std::move(type_args), {}, {}, {}, true)
	);
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::Match& match)
{
	// The type demanded of the match is its cases' type, not the scrutinee's.
	const auto evaluate_scrutinee = [&match, this]() -> MarmotResult::TypeResult
	{
		ExpectedTypeGuard guard(*this, nullptr);
		return Evaluate(match.m_arg_expr);
	};

	return evaluate_scrutinee()
		.and_then
		(
			[&match, this](std::shared_ptr<MarmotType>&& arg_type) -> MarmotResult::TypeResult
			{
				std::shared_ptr<MarmotType> resolved_arg_type = ApplySubstitution(arg_type);
				std::shared_ptr<MarmotType> prev_case_type = nullptr;
				PatternCoverage coverage;

				for (const std::unique_ptr<MarmotExpression>& case_expr : match.m_cases)
				{
					ScopeSession scope(*this);
					MarmotExpression::Case& match_case = case_expr->GetExpression<MarmotExpression::Case>();

					// Before the case's own pattern is checked: a match in an arm without
					// braces takes the enclosing match's later cases, whose patterns are
					// likely of another type, and this is the error that says why.
					if (&case_expr != &match.m_cases.front() && coverage.IsExhaustive())
					{
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnreachableCase, "The cases above already match every value, so this one can never run. If it belongs to an enclosing match, put the match above it in braces.", match_case.m_keyword, m_file_name, m_source_lines));
					}

					MarmotResult::TypeResult case_result = CheckPattern(*match_case.m_pattern, resolved_arg_type)
						.and_then([&](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
						{
							const std::optional<CompilerError> error = CheckCaseReachable(coverage, match_case);
							if (error.has_value())
							{
								return std::unexpected(error.value());
							}
							return {};
						})
						.and_then([&](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
						{
							const std::optional<CompilerError> error = match_case.HasGuard() ? CheckCaseGuard(match_case) : std::nullopt;
							if (error.has_value())
							{
								return std::unexpected(error.value());
							}
							return Evaluate(match_case.m_expr);
						});

					if (!case_result.has_value())
					{
						return std::unexpected(std::move(case_result.error()));
					}

					// A guarded case may fail its guard, so it covers nothing.
					if (!match_case.HasGuard())
					{
						coverage.Add(*match_case.m_pattern);
					}

					std::shared_ptr<MarmotType> resolved_case_type = ApplySubstitution(case_result.value());
					if (prev_case_type == nullptr)
					{
						prev_case_type = resolved_case_type;
					}
					else
					{
						std::shared_ptr<MarmotType> resolved_prev_case_type = ApplySubstitution(prev_case_type);
						MarmotResult::TypeResult unify_result = Unify(match.m_match_keyword, resolved_prev_case_type, resolved_case_type);
						if (!unify_result.has_value())
						{
							return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, "Match expression type error: case types do not match", match.m_match_keyword, m_file_name, m_source_lines, resolved_prev_case_type, resolved_case_type));
						}

						// What unification gives back, not the previous arm's type: an arm
						// that never finishes takes the type of the arms that do.
						prev_case_type = ApplySubstitution(unify_result.value());
					}
				}

				const std::vector<std::string> unmatched = coverage.FindUnmatched();

				if (unmatched == std::vector<std::string>{ "_" })
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeNonExhaustiveMatch, "Match expression type error: non-union matches need a catch-all arm such as 'case _ =>'", match.m_match_keyword, m_file_name, m_source_lines));
				}
				if (!unmatched.empty())
				{
					const std::string message = std::format("Match expression type error: non-exhaustive match: no case matches {}", unmatched | std::views::join_with(std::string_view(", ")) | std::ranges::to<std::string>());
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeNonExhaustiveMatch, message, match.m_match_keyword, m_file_name, m_source_lines));
				}

				match.m_type_data = prev_case_type;
				return match.m_type_data;
			}
		);
}

// A case, and each alternative of every or-pattern in it, must match some value
// nothing before it does.
std::optional<CompilerError> TypeChecker::CheckCaseReachable(const PatternCoverage& coverage, const MarmotExpression::Case& match_case)
{
	if (!coverage.IsUseful(*match_case.m_pattern))
	{
		return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnreachableCase, "The cases above already match every value this one does, so it can never run.", match_case.m_keyword, m_file_name, m_source_lines);
	}

	const MarmotPattern* alternative = coverage.FindUnreachableAlternative(*match_case.m_pattern);
	if (alternative != nullptr)
	{
		return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnreachableCase, "The cases and alternatives before this one already match every value it does, so it can never match.", GetPatternToken(*alternative), m_file_name, m_source_lines);
	}
	return std::nullopt;
}

std::optional<CompilerError> TypeChecker::CheckCaseGuard(MarmotExpression::Case& case_expr)
{
	MarmotResult::TypeResult guard_result = Evaluate(case_expr.m_guard.value());
	if (!guard_result.has_value())
	{
		return std::move(guard_result.error());
	}

	std::shared_ptr<MarmotType> bool_type = MarmotType::MakeLiteralType<MarmotType::BoolType>();
	std::shared_ptr<MarmotType> resolved_guard_type = ApplySubstitution(guard_result.value());
	if (!Unify(case_expr.m_keyword, bool_type, resolved_guard_type).has_value())
	{
		return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, "Match expression type error: case guard must be of type Bool", case_expr.m_keyword, m_file_name, m_source_lines, resolved_guard_type, bool_type);
	}

	return std::nullopt;
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::Case& case_expr)
{
	if (case_expr.HasGuard())
	{
		std::optional<CompilerError> guard_error = CheckCaseGuard(case_expr);
		if (guard_error.has_value())
		{
			return std::unexpected(std::move(guard_error.value()));
		}
	}

	return Evaluate(case_expr.m_expr)
		.and_then
		(
			[&case_expr, this](std::shared_ptr<MarmotType>&& expr_type) ->MarmotResult::TypeResult
			{
				return Unify(case_expr.m_keyword, case_expr.m_type_data, expr_type);
			}
		);
}

// A `for` loop and an array comprehension iterate the same things the same way:
// a Range, an Array, or anything with an Iterable instance or constraint. This
// decides which, records it on the node, and gives the element type.
template <typename IterationNode>
MarmotResult::TypeResult TypeChecker::ResolveIteration(IterationNode& node, const std::shared_ptr<MarmotType>& range_type, std::string_view error_prefix)
{
	const std::shared_ptr<MarmotType> resolved_range = ApplySubstitution(range_type);
	const Token& token = node.m_in_keyword;
	const std::function<CompilerError(std::string_view, const std::shared_ptr<MarmotType>&)> fail = [this, &token, error_prefix](std::string_view message, const std::shared_ptr<MarmotType>& shown_type)
	{
		return MarmotError::GenerateTypeCheckerErrorWithContext(std::format("{} type error: {}", error_prefix, message), token, m_file_name, m_source_lines, shown_type);
	};

	if (resolved_range->IsType<MarmotType::RangeType>())
	{
		node.m_is_array_iteration = false;
		node.m_is_iterable_iteration = false;
		return resolved_range->GetType<MarmotType::RangeType>().m_element_type;
	}
	if (resolved_range->IsType<MarmotType::ArrayType>())
	{
		node.m_is_array_iteration = true;
		node.m_is_iterable_iteration = false;
		return resolved_range->GetType<MarmotType::ArrayType>().m_element_type;
	}

	std::unordered_map<std::string, ClassInfo>::iterator class_it = m_classes.find(std::string(ITERABLE_CLASS_NAME));
	if (class_it == m_classes.end())
	{
		return std::unexpected(fail("Iterable class not found", resolved_range));
	}

	const bool uses_associated_item = class_it->second.m_associated_types.contains(std::string(ITEM_ASSOCIATED_TYPE_NAME));
	std::shared_ptr<MarmotType> iterable_item_type;
	bool is_iterable = false;

	// A bare variable stands for a type nothing is known about yet, so only a
	// where clause can say it is iterable; any other type may match an instance,
	// generic or not.
	if (!resolved_range->IsType<MarmotType::TypeVariable>())
	{
		if (uses_associated_item)
		{
			std::optional<ResolvedInstanceMatch> resolved_match = FindMatchingInstance(std::string(ITERABLE_CLASS_NAME), { resolved_range });
			if (resolved_match.has_value())
			{
				AssociatedTypeEnvironment::const_iterator binding_it = resolved_match->m_instance->m_associated_type_bindings.find(std::string(ITEM_ASSOCIATED_TYPE_NAME));
				if (binding_it != resolved_match->m_instance->m_associated_type_bindings.cend())
				{
					iterable_item_type = ApplySubstitution(TypeTransform::Instantiate(binding_it->second, resolved_match->m_substitutions));
					is_iterable = true;
				}
			}
		}
		else
		{
			for (const auto& [key, info] : m_instances)
			{
				if (info.m_class_name != ITERABLE_CLASS_NAME || info.m_type_args.size() != 2u)
				{
					continue;
				}

				InstanceTypeMatcher matcher;
				if (!matcher.Match(info.m_type_args[0u], resolved_range))
				{
					continue;
				}

				if (is_iterable)
				{
					return std::unexpected(fail("ambiguous Iterable instance for iterator type", resolved_range));
				}
				iterable_item_type = TypeTransform::Instantiate(info.m_type_args[1u], matcher.Bindings());
				is_iterable = true;
			}
		}
	}

	if (!is_iterable)
	{
		for (const MarmotType::ClassConstraint& constraint : m_active_constraints)
		{
			if (constraint.m_class_name == ITERABLE_CLASS_NAME && constraint.m_type_args.size() == class_it->second.m_type_param_names.size() && !constraint.m_type_args.empty() && *ApplySubstitution(constraint.m_type_args[0u]) == *resolved_range)
			{
				if (uses_associated_item)
				{
					iterable_item_type = ResolveAssociatedType(MarmotType::AssociatedType(std::string(ITERABLE_CLASS_NAME), std::string(ITEM_ASSOCIATED_TYPE_NAME), std::vector<std::shared_ptr<MarmotType>>(constraint.m_type_args)));
				}
				else if (constraint.m_type_args.size() >= 2u)
				{
					iterable_item_type = constraint.m_type_args[1u];
				}
				is_iterable = true;
				break;
			}
		}
	}

	if (!is_iterable)
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(std::format("{} type error: expected Range, Array, or Iterable type for iteration", error_prefix), token, m_file_name, m_source_lines, resolved_range, MarmotType::MakeRangeType(MarmotType::MakeLiteralType<MarmotType::IntegerType>())));
	}

	std::unordered_map<std::string, std::shared_ptr<MarmotType>>::iterator method_it = class_it->second.m_method_types.find(std::string(NEXT_METHOD_NAME));
	if (method_it == class_it->second.m_method_types.end())
	{
		return std::unexpected(fail("Iterable::Next not found", resolved_range));
	}

	TypeEnvironment substitutions;
	const std::vector<std::string>& type_params = class_it->second.m_type_param_names;
	if (!type_params.empty())
	{
		substitutions[type_params[0u]] = resolved_range;
		if (!uses_associated_item && type_params.size() >= 2u && iterable_item_type != nullptr)
		{
			substitutions[type_params[1u]] = iterable_item_type;
		}
	}

	std::shared_ptr<MarmotType> next_type = ApplySubstitution(TypeTransform::Instantiate(method_it->second, substitutions));
	if (!next_type->IsType<MarmotType::FunctionType>())
	{
		return std::unexpected(fail("Iterable::Next must be a function", resolved_range));
	}

	std::shared_ptr<MarmotType> next_return = ApplySubstitution(next_type->GetType<MarmotType::FunctionType>().m_return_type);
	if (!next_return->IsType<MarmotType::UnionType>())
	{
		return std::unexpected(fail("Iterable::Next must return an Option-like union", next_return));
	}

	const MarmotType::UnionType& option_union = next_return->GetType<MarmotType::UnionType>();
	const std::string some_name = option_union.m_name + std::string(NameSeparator) + std::string(ITERABLE_SOME_CONSTRUCTOR_NAME);
	if (!option_union.m_member_info.contains(some_name))
	{
		return std::unexpected(fail("Iterable::Next return type is missing 'Some' constructor", next_return));
	}

	const MarmotType::UnionType::UnionMemberContext& some_ctx = option_union.m_member_info.at(some_name);
	if (some_ctx.m_member_types.size() != 1u)
	{
		return std::unexpected(fail("Iterable::Next 'Some' constructor must contain exactly one value", next_return));
	}

	// Next returns Option<(Item, Iter)>. The payload is a pair whose first
	// element is the item and whose second is the advanced iterator, so the
	// loop variable takes element 0 rather than the payload itself.
	std::shared_ptr<MarmotType> some_payload = ApplySubstitution(some_ctx.m_member_types[0u]);
	if (!some_payload->IsType<MarmotType::TupleType>() || some_payload->GetType<MarmotType::TupleType>().m_element_types.size() != 2u)
	{
		return std::unexpected(fail("Iterable::Next must return Option<(Item, Iter)>", some_payload));
	}

	iterable_item_type = ApplySubstitution(some_payload->GetType<MarmotType::TupleType>().m_element_types[0u]);
	node.m_is_array_iteration = false;
	node.m_is_iterable_iteration = true;
	node.m_iterable_item_type = iterable_item_type;
	node.m_iterable_some_tag = some_ctx.m_tag;
	node.m_iterable_next_type = next_return;
	return iterable_item_type;
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::For& for_expr)
{
	return Evaluate(for_expr.m_range)
		.and_then([&for_expr, this](std::shared_ptr<MarmotType>&& range_type) { return ResolveIteration(for_expr, range_type, "For loop expression"); })
		.and_then
		(
			[&for_expr, this](std::shared_ptr<MarmotType>&& element_type) -> MarmotResult::TypeResult
			{
				ScopeSession scope(*this);
				m_type_scopes.back().m_names[for_expr.m_loop_variable.m_lexeme] = element_type;

				return Evaluate(for_expr.m_body)
					.and_then
					(
						[&for_expr](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
						{
							// A `for` loop consumes an iterable for its effects, so it is Unit.
							// It used to take its type from whatever `break` carried out of it.
							for_expr.m_type_data = MarmotType::MakeLiteralType<MarmotType::UnitType>();
							return for_expr.m_type_data;
						}
					);
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::ArrayComprehension& comp)
{
	return Evaluate(comp.m_range)
		.and_then([&comp, this](std::shared_ptr<MarmotType>&& range_type) { return ResolveIteration(comp, range_type, "Array comprehension"); })
		.and_then
		(
			[&comp, this](std::shared_ptr<MarmotType>&& element_type) -> MarmotResult::TypeResult
			{
				ScopeSession scope(*this);
				m_type_scopes.back().m_names[comp.m_loop_variable.m_lexeme] = element_type;

				// The transform expression produces one element, so its expected type is
				// the expected array's element type, never the array type itself. Leaving
				// the ambient type in place pushes Array<T> into a construction here,
				// which then cannot infer its own arguments.
				const std::shared_ptr<MarmotType> ambient_expected_type = m_expected_expr_type;
				ExpectedTypeGuard transform_guard(*this, (ambient_expected_type != nullptr && ambient_expected_type->IsType<MarmotType::ArrayType>()) ? ambient_expected_type->GetType<MarmotType::ArrayType>().m_element_type : std::shared_ptr<MarmotType>{});

				return Evaluate(comp.m_transform_expr)
					.and_then
					(
						[&comp, this](std::shared_ptr<MarmotType>&& transform_type) -> MarmotResult::TypeResult
						{
							comp.m_type_data = MarmotType::MakeArrayType(ApplySubstitution(transform_type));
							return comp.m_type_data;
						}
					);
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::As& as)
{
	// A cast's operand is typed on its own terms. The ambient expected type
	// describes the result of the cast, not its source, so leaving it in place
	// pushes the target type into a construction on the left of 'as'.
	ExpectedTypeGuard operand_guard(*this, std::shared_ptr<MarmotType>{});
	as.m_to_type = InstantiateWrittenType(as.m_to_type);

	return Evaluate(as.m_expr)
		.and_then
		(
			[&as, this](std::shared_ptr<MarmotType>&& expr_type) ->MarmotResult::TypeResult
			{
				// Instance selection matches the resolved types, so a variable inference
				// has bound casts as its type, and a generic instance is found.
				const std::shared_ptr<MarmotType> from_type = ApplySubstitution(expr_type);
				const std::shared_ptr<MarmotType> to_type = ApplySubstitution(as.m_to_type);
				const MarmotType::ClassConstraint conversion(std::string(CONVERTABLE_CLASS_NAME), { from_type, to_type });
				const bool has_convertable_instance = !FindMatchingInstances(conversion.m_class_name, conversion.m_type_args).empty();
				const bool has_convertable_constraint = !has_convertable_instance && IsSatisfiedByActiveConstraint(conversion);

				// Check if this is a built-in conversion
				bool is_builtin_conversion = false;
				if (to_type->IsType<MarmotType::StructType>() && from_type->IsType<MarmotType::StructType>())
				{
					if (!has_convertable_instance && !has_convertable_constraint)
					{
						const MarmotType::StructType& from_struct_type = from_type->GetType<MarmotType::StructType>();
						const MarmotType::StructType& to_struct_type = to_type->GetType<MarmotType::StructType>();
						if (to_struct_type.m_member_types.size() != from_struct_type.m_member_types.size())
						{
							return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Type cast expression type error: struct member count mismatch", as.m_as_keyword, m_file_name, m_source_lines, from_type, to_type));
						}

						for (size_t i : std::views::iota(0u, to_struct_type.m_member_types.size()))
						{
							if (*from_struct_type.m_member_types[i] != *to_struct_type.m_member_types[i])
							{
								return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Type cast expression type error: struct member type mismatch", as.m_as_keyword, m_file_name, m_source_lines, from_struct_type.m_member_types[i], to_struct_type.m_member_types[i]));
							}
						}
						is_builtin_conversion = true;
					}
				}
				else if (to_type->IsType<MarmotType::StructType>() && !has_convertable_instance && !has_convertable_constraint)
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Type cast expression type error: cannot cast to struct type", as.m_as_keyword, m_file_name, m_source_lines, from_type, to_type));
				}

				// Check for built-in primitive conversions
				bool const is_from_int = from_type->IsType<MarmotType::IntegerType>();
				bool const is_from_float = from_type->IsType<MarmotType::FloatType>();
				bool const is_from_text = from_type->IsType<MarmotType::TextType>();
				bool const is_from_byte = from_type->IsType<MarmotType::ByteType>();
				bool const is_from_word = from_type->IsType<MarmotType::WordType>();
				bool const is_from_bool = from_type->IsType<MarmotType::BoolType>();

				bool const is_to_int = to_type->IsType<MarmotType::IntegerType>();
				bool const is_to_float = to_type->IsType<MarmotType::FloatType>();
				bool const is_to_text = to_type->IsType<MarmotType::TextType>();
				bool const is_to_byte = to_type->IsType<MarmotType::ByteType>();
				bool const is_to_word = to_type->IsType<MarmotType::WordType>();

				if ((is_from_int && (is_to_float || is_to_text || is_to_byte || is_to_word)) ||
					(is_from_float && (is_to_int || is_to_text || is_to_byte || is_to_word)) ||
					(is_from_text && (is_to_int || is_to_float)) ||
					(is_from_byte && (is_to_int || is_to_float || is_to_word || is_to_text)) ||
					(is_from_word && (is_to_int || is_to_float || is_to_byte || is_to_text)) ||
					(is_from_bool && is_to_text))
				{
					is_builtin_conversion = true;
				}

				// `x as T` where x already has type T is a no-op, so it needs no Convertable instance.
				const bool is_identity_conversion = *from_type == *to_type;

				// Verify that either Convertable instance exists, constraint exists, or it's a built-in conversion
				if (!has_convertable_instance && !has_convertable_constraint && !is_builtin_conversion && !is_identity_conversion)
				{
					MarmotType::ClassConstraint constraint(std::string(CONVERTABLE_CLASS_NAME), { from_type, to_type });
					const std::string suggestion = std::format("Define 'instance Convertable<{}, {}>' to enable this conversion.", from_type->DisplayString(), to_type->DisplayString());
					return std::unexpected(MakeConstraintFailureError(as.m_as_keyword, constraint, suggestion));
				}

				as.m_type_data = to_type;

				// Prefer built-in code generation for concrete built-in casts so Convertable
				// instances can implement Convert via `as` without recursing back into themselves.
				// Constraints still require Convertable dispatch because the concrete conversion
				// is only known during specialization.
				as.m_uses_convertable = has_convertable_constraint || (has_convertable_instance && !is_builtin_conversion);
				if (has_convertable_instance && !is_builtin_conversion)
				{
					if (std::optional<CompilerError> error = CheckConstraintHolds(as.m_as_keyword, conversion, 0u))
					{
						return std::unexpected(std::move(*error));
					}
				}
				return as.m_type_data;
			}
		);
}

// A hex or binary literal up to 0xFF is a Byte: it is how a Byte is written.
// Beside an Int, the plain mismatch reads as a mistake in the other operand.
std::optional<CompilerError> TypeChecker::ByteLiteralBesideInt(const MarmotExpression::Binary& binary, const std::shared_ptr<MarmotType>& left_type, const std::shared_ptr<MarmotType>& right_type)
{
	const std::shared_ptr<MarmotType> resolved_left = ApplySubstitution(left_type);
	const std::shared_ptr<MarmotType> resolved_right = ApplySubstitution(right_type);
	const std::function<const MarmotExpression::Literal*(const std::unique_ptr<MarmotExpression>&, const std::shared_ptr<MarmotType>&)> byte_literal_facing_int =
		[](const std::unique_ptr<MarmotExpression>& operand, const std::shared_ptr<MarmotType>& other_type) -> const MarmotExpression::Literal*
		{
			if (!other_type->IsType<MarmotType::IntegerType>() || !operand->IsExpression<MarmotExpression::Literal>())
			{
				return nullptr;
			}

			const MarmotExpression::Literal& literal = operand->GetExpression<MarmotExpression::Literal>();
			return literal.m_kind == MarmotExpression::LiteralKind::Byte ? &literal : nullptr;
		};

	const MarmotExpression::Literal* literal = byte_literal_facing_int(binary.m_left, resolved_right);
	if (literal == nullptr)
	{
		literal = byte_literal_facing_int(binary.m_right, resolved_left);
	}
	if (literal == nullptr)
	{
		return std::nullopt;
	}

	const std::string& lexeme = literal->m_token.m_lexeme;
	return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, std::format("'{0}' is a Byte: a hex or binary literal up to 0xFF is how a Byte is written. For an Int here, write {1}, or {0} as Int.", lexeme, ParseUnsignedLiteral(lexeme).value()), literal->m_token, m_file_name, m_source_lines);
}

MarmotResult::TypeResult TypeChecker::CheckRefNew(const std::unique_ptr<MarmotExpression>& value)
{
	std::shared_ptr<MarmotType> expected_element = nullptr;
	const std::shared_ptr<MarmotType> expected_type = m_expected_expr_type != nullptr ? ApplySubstitution(m_expected_expr_type) : nullptr;
	if (expected_type != nullptr && expected_type->IsType<MarmotType::CellType>())
	{
		expected_element = expected_type->GetType<MarmotType::CellType>().m_element_type;
	}

	ExpectedTypeGuard guard(*this, expected_element);
	return Evaluate(value).transform([this](const std::shared_ptr<MarmotType>& value_type)
	{
		return MarmotType::MakeCellType(ApplySubstitution(value_type));
	});
}

MarmotResult::TypeResult TypeChecker::CheckRefAccess(const std::unique_ptr<MarmotExpression>& cell, const std::unique_ptr<MarmotExpression>* value, const Token& token, std::string_view error_message)
{
	MarmotResult::TypeResult cell_result = [this, &cell]() -> MarmotResult::TypeResult
	{
		ExpectedTypeGuard guard(*this, nullptr);
		return Evaluate(cell);
	}();
	if (!cell_result.has_value())
	{
		return cell_result;
	}

	std::shared_ptr<MarmotType> cell_type = ApplySubstitution(cell_result.value());
	if (cell_type->IsType<MarmotType::TypeVariable>())
	{
		std::shared_ptr<MarmotType> expected_cell = MarmotType::MakeCellType(FreshTypeVar());
		MarmotResult::TypeResult unified = Unify(token, cell_type, expected_cell);
		if (!unified.has_value())
		{
			return unified;
		}
		cell_type = ApplySubstitution(cell_type);
	}
	if (!cell_type->IsType<MarmotType::CellType>())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(std::string(error_message), token, m_file_name, m_source_lines, cell_type));
	}

	std::shared_ptr<MarmotType> element_type = cell_type->GetType<MarmotType::CellType>().m_element_type;
	if (value != nullptr)
	{
		ExpectedTypeGuard guard(*this, element_type);
		MarmotResult::TypeResult value_result = Evaluate(*value);
		if (!value_result.has_value())
		{
			return value_result;
		}
		MarmotResult::TypeResult unified = Unify(token, element_type, value_result.value());
		if (!unified.has_value())
		{
			return unified;
		}
	}
	return ApplySubstitution(element_type);
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::Binary& binary)
{
	if (binary.m_op.m_token_name == Token::Name::COLON_EQUAL)
	{
		return CheckRefAccess(binary.m_left, &binary.m_right, binary.m_op, "Reference write operator requires a Ref<T>")
			.transform([&binary](const std::shared_ptr<MarmotType>&)
			{
				binary.m_type_data = MarmotType::MakeLiteralType<MarmotType::UnitType>();
				return binary.m_type_data;
			});
	}

	// A comparison's result is Bool whatever its operands are, so the type
	// demanded of it says nothing about them: `C(1) == C(2)` where a Bool is
	// expected was checked as if C(1) had to be a Bool.
	const Token::Name op_name = binary.m_op.m_token_name;
	const bool compares = op_name == Token::Name::DOUBLE_EQUAL || op_name == Token::Name::BANG_EQUAL
		|| op_name == Token::Name::LEFT_ANGLE || op_name == Token::Name::LESS_EQUAL
		|| op_name == Token::Name::RIGHT_ANGLE || op_name == Token::Name::GREATER_EQUAL;
	std::optional<ExpectedTypeGuard> operand_guard;
	if (compares)
	{
		operand_guard.emplace(*this, nullptr);
	}

	return Evaluate(binary.m_left)
		.and_then
		(
			[&binary, this](std::shared_ptr<MarmotType>&& left_type) -> MarmotResult::TypeResult
			{
				return Evaluate(binary.m_right)
					.and_then
					(
						[&left_type, &binary, this](std::shared_ptr<MarmotType>&& right_type) -> MarmotResult::TypeResult
						{
							return CheckBinaryOperator(binary, left_type, right_type);
						}
					);
			}
		);
}

// The class an operand must belong to for an operator to apply. A generic
// parameter belongs to one only where its definition says so, and it is shown
// by the name it was written as, with the where clause that would say so.
std::optional<CompilerError> TypeChecker::RequireOperandClass(const Token& op, std::string_view class_name, const std::shared_ptr<MarmotType>& resolved_operand)
{
	std::optional<CompilerError> error = CheckConstraintHolds(op, MarmotType::ClassConstraint(std::string(class_name), { resolved_operand }), 0u);
	const std::optional<std::string> param_name = RigidParameterName(resolved_operand);
	if (!error.has_value() || !param_name.has_value())
	{
		return error;
	}

	const std::string_view declaring_file = class_name == BITWISE_CLASS_NAME ? NUMERIC_CLASS_NAME : class_name;
	const std::string hint = m_classes.contains(std::string(class_name))
		? std::format("'{0}' is a generic parameter, so it is only known to be one when the definition says 'where {1}<{0}>'.", param_name.value(), class_name)
		: std::format("'{0}' is a generic parameter. Import MarmotPrelude/{2}.mmt, which declares {1}, and say 'where {1}<{0}>' on the definition.", param_name.value(), class_name, declaring_file);
	return MakeConstraintFailureError(op, MarmotType::ClassConstraint(std::string(class_name), { MarmotType::MakeGenericType(param_name.value()) }), hint);
}

// An operand whose type is still a variable is one inference has not decided
// yet, and an operator may decide it. A generic parameter is a variable too, but
// it stands for whatever type a caller picks, so an operator applies to it only
// where a where clause names the class that provides the operator: Numeric for
// arithmetic, Bitwise for the bit operators, Equatable, Orderable and
// Concatenable for the rest.
MarmotResult::TypeResult TypeChecker::CheckBinaryOperator(MarmotExpression::Binary& binary, std::shared_ptr<MarmotType>& left_type, std::shared_ptr<MarmotType>& right_type)
{
	const Token& op = binary.m_op;
	if (op.m_token_name == Token::Name::LEFT_SHIFT || op.m_token_name == Token::Name::RIGHT_SHIFT)
	{
		const std::shared_ptr<MarmotType> resolved_left = ApplySubstitution(left_type);
		const std::shared_ptr<MarmotType> resolved_right = ApplySubstitution(right_type);

		if (resolved_right->IsType<MarmotType::TypeVariable>() && !RigidParameterName(resolved_right).has_value())
		{
			std::shared_ptr<MarmotType> int_type = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
			MarmotResult::TypeResult unified = Unify(op, right_type, int_type, UnifyDiagnosticMode::ActualExpected);
			if (!unified.has_value())
			{
				return unified;
			}
		}
		else if (const std::optional<std::string> amount_param = RigidParameterName(resolved_right))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Shift operator type error: shift amount must be Int", op, m_file_name, m_source_lines, MarmotType::MakeGenericType(amount_param.value()), MarmotType::MakeLiteralType<MarmotType::IntegerType>()));
		}
		else if (!resolved_right->IsType<MarmotType::IntegerType>())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Shift operator type error: shift amount must be Int", op, m_file_name, m_source_lines, resolved_right, MarmotType::MakeLiteralType<MarmotType::IntegerType>()));
		}

		if (RigidParameterName(resolved_left).has_value())
		{
			if (std::optional<CompilerError> error = RequireOperandClass(op, BITWISE_CLASS_NAME, resolved_left))
			{
				return std::unexpected(std::move(*error));
			}
		}
		else if (!resolved_left->IsType<MarmotType::IntegerType>() && !resolved_left->IsType<MarmotType::ByteType>() && !resolved_left->IsType<MarmotType::WordType>() && !resolved_left->IsType<MarmotType::TypeVariable>())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Shift operator type error: can only shift Int, Byte, or Word types", op, m_file_name, m_source_lines, resolved_left, MarmotType::MakeLiteralType<MarmotType::IntegerType>(), MarmotType::MakeLiteralType<MarmotType::ByteType>(), MarmotType::MakeLiteralType<MarmotType::WordType>()));
		}

		binary.m_type_data = left_type;
		return binary.m_type_data;
	}

	if (std::optional<CompilerError> byte_literal_error = ByteLiteralBesideInt(binary, left_type, right_type))
	{
		return std::unexpected(std::move(byte_literal_error.value()));
	}

	MarmotResult::TypeResult unified = Unify(op, left_type, right_type);
	if (!unified.has_value())
	{
		return unified;
	}

	const std::shared_ptr<MarmotType> resolved = ApplySubstitution(left_type);
	const std::optional<std::string> param_name = RigidParameterName(resolved);
	const bool is_undecided = resolved->IsType<MarmotType::TypeVariable>() && !param_name.has_value();
	binary.m_type_data = left_type;

	if (std::ranges::contains(kBinaryPartialOrderComparisonOperators, op.m_token_name))
	{
		if (!resolved->IsNumericType() && !is_undecided)
		{
			if (std::optional<CompilerError> error = RequireOperandClass(op, ORDERABLE_CLASS_NAME, resolved))
			{
				return std::unexpected(std::move(*error));
			}
			binary.m_uses_orderable = true;
		}

		binary.m_type_data = MarmotType::MakeLiteralType<MarmotType::BoolType>();
		return binary.m_type_data;
	}

	if (std::ranges::contains(kBinaryArithmeticOperators, op.m_token_name))
	{
		if (param_name.has_value())
		{
			if (std::optional<CompilerError> error = RequireOperandClass(op, NUMERIC_CLASS_NAME, resolved))
			{
				return std::unexpected(std::move(*error));
			}
		}
		else if (!resolved->IsNumericType() && !is_undecided)
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Binary expression type error: expected numeric type", op, m_file_name, m_source_lines, resolved, MarmotType::MakeLiteralType<MarmotType::IntegerType>(), MarmotType::MakeLiteralType<MarmotType::FloatType>()));
		}
		return binary.m_type_data;
	}

	if (std::ranges::contains(kBinaryBitwiseOperators, op.m_token_name))
	{
		if (param_name.has_value())
		{
			if (std::optional<CompilerError> error = RequireOperandClass(op, BITWISE_CLASS_NAME, resolved))
			{
				return std::unexpected(std::move(*error));
			}
		}
		else if (!resolved->IsType<MarmotType::IntegerType>() && !resolved->IsType<MarmotType::ByteType>() && !resolved->IsType<MarmotType::WordType>() && !is_undecided)
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Binary expression type error: expected integer, byte, or word type", op, m_file_name, m_source_lines, resolved, MarmotType::MakeLiteralType<MarmotType::IntegerType>(), MarmotType::MakeLiteralType<MarmotType::ByteType>(), MarmotType::MakeLiteralType<MarmotType::WordType>()));
		}
		return binary.m_type_data;
	}

	if (std::ranges::contains(kBinaryEqualityOperators, op.m_token_name))
	{
		const bool is_builtin = resolved->IsNumericType() || resolved->IsType<MarmotType::TextType>() || resolved->IsType<MarmotType::BoolType>() || is_undecided;
		if (!is_builtin)
		{
			if (std::optional<CompilerError> error = RequireOperandClass(op, EQUATABLE_CLASS_NAME, resolved))
			{
				return std::unexpected(std::move(*error));
			}
			binary.m_uses_equatable = true;
		}

		binary.m_type_data = MarmotType::MakeLiteralType<MarmotType::BoolType>();
		return binary.m_type_data;
	}

	if (std::ranges::contains(kBinaryLogicalOperators, op.m_token_name))
	{
		std::shared_ptr<MarmotType> bool_type = MarmotType::MakeLiteralType<MarmotType::BoolType>();
		if (is_undecided)
		{
			MarmotResult::TypeResult bool_result = Unify(op, left_type, bool_type, UnifyDiagnosticMode::ActualExpected);
			if (!bool_result.has_value())
			{
				return bool_result;
			}
		}
		else if (!resolved->IsType<MarmotType::BoolType>())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Binary expression type error: expected boolean type", op, m_file_name, m_source_lines, resolved, bool_type));
		}

		binary.m_type_data = bool_type;
		return binary.m_type_data;
	}

	if (std::ranges::contains(kBinaryConcatenationOperators, op.m_token_name))
	{
		if (!resolved->IsType<MarmotType::TextType>() && !resolved->IsType<MarmotType::ArrayType>())
		{
			if (std::optional<CompilerError> error = RequireOperandClass(op, CONCATENABLE_CLASS_NAME, resolved))
			{
				return std::unexpected(std::move(*error));
			}
			binary.m_uses_concatenable = true;
		}
	}

	return binary.m_type_data;
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::Group& group)
{
	return Evaluate(group.m_expr_in)
		.and_then
		(
			[&group](std::shared_ptr<MarmotType>&& actual_type) ->MarmotResult::TypeResult
			{
				group.m_type_data = std::move(actual_type);
				return group.m_type_data;
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::Tuple& tuple)
{
	if (tuple.m_elements.empty())
	{
		// Empty tuple is Unit type
		tuple.m_type_data = MarmotType::MakeLiteralType<MarmotType::UnitType>();
		return tuple.m_type_data;
	}

	std::vector<std::shared_ptr<MarmotType>> element_types;
	element_types.reserve(tuple.m_elements.size());

	const std::shared_ptr<MarmotType> ambient_expected_type = m_expected_expr_type;
	const bool expected_tuple_matches = ambient_expected_type != nullptr
		&& ambient_expected_type->IsType<MarmotType::TupleType>()
		&& ambient_expected_type->GetType<MarmotType::TupleType>().m_element_types.size() == tuple.m_elements.size();

	for (size_t index : std::views::iota(0u, tuple.m_elements.size()))
	{
		// An element's expected type is its own slot in the expected tuple, never
		// the whole tuple. Leaving the ambient type in place leaks it into the
		// element, so a construction there would infer against the tuple type.
		ExpectedTypeGuard element_guard(*this, expected_tuple_matches ? ambient_expected_type->GetType<MarmotType::TupleType>().m_element_types[index] : std::shared_ptr<MarmotType>{});

		MarmotResult::TypeResult result = Evaluate(tuple.m_elements[index]);
		if (!result.has_value())
		{
			return result;
		}

		element_types.emplace_back(std::move(result.value()));
	}

	tuple.m_type_data = MarmotType::MakeTupleType(std::move(element_types));
	return tuple.m_type_data;
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::UnaryPrefix& unary)
{
	if (unary.m_op.m_token_name == Token::Name::REF || unary.m_op.m_token_name == Token::Name::STAR)
	{
		MarmotResult::TypeResult result = unary.m_op.m_token_name == Token::Name::REF
			? CheckRefNew(unary.m_expr)
			: CheckRefAccess(unary.m_expr, nullptr, unary.m_op, "Dereference operator requires a Ref<T>");
		return result.transform([&unary](std::shared_ptr<MarmotType> type)
		{
			unary.m_type_data = std::move(type);
			return unary.m_type_data;
		});
	}

	return Evaluate(unary.m_expr)
		.and_then
		(
			[this, &unary](std::shared_ptr<MarmotType>&& operand_type) -> MarmotResult::TypeResult
			{
				std::shared_ptr<MarmotType> actual_type = ApplySubstitution(operand_type);
				if (unary.m_op.m_token_name == Token::Name::SINGLE_MINUS || unary.m_op.m_token_name == Token::Name::SINGLE_PLUS)
				{
					if (RigidParameterName(actual_type).has_value())
					{
						if (std::optional<CompilerError> error = RequireOperandClass(unary.m_op, NUMERIC_CLASS_NAME, actual_type))
						{
							return std::unexpected(std::move(*error));
						}
					}
					else if (!actual_type->IsNumericType())
					{
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Unary operator requires numeric type", unary.m_op, m_file_name, m_source_lines, actual_type, MarmotType::MakeLiteralType<MarmotType::IntegerType>(), MarmotType::MakeLiteralType<MarmotType::FloatType>()));
					}
				}
				else if (unary.m_op.m_token_name == Token::Name::BANG)
				{
					if (!actual_type->IsType<MarmotType::BoolType>())
					{
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Logical NOT operator requires boolean type", unary.m_op, m_file_name, m_source_lines, actual_type, MarmotType::MakeLiteralType<MarmotType::BoolType>()));
					}
				}
				else if (unary.m_op.m_token_name == Token::Name::TILDE)
				{
					if (RigidParameterName(actual_type).has_value())
					{
						if (std::optional<CompilerError> error = RequireOperandClass(unary.m_op, BITWISE_CLASS_NAME, actual_type))
						{
							return std::unexpected(std::move(*error));
						}
					}
					else if (!actual_type->IsType<MarmotType::IntegerType>() && !actual_type->IsType<MarmotType::ByteType>() && !actual_type->IsType<MarmotType::WordType>())
					{
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Bitwise NOT operator requires an integer, byte, or word type", unary.m_op, m_file_name, m_source_lines, actual_type, MarmotType::MakeLiteralType<MarmotType::IntegerType>(), MarmotType::MakeLiteralType<MarmotType::ByteType>(), MarmotType::MakeLiteralType<MarmotType::WordType>()));
					}
				}
				else if (unary.m_op.m_token_name == Token::Name::HASH)
				{
					if (actual_type->IsType<MarmotType::ArrayType>())
					{
						unary.m_type_data = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
						return unary.m_type_data;
					}

					const std::string suggestion = m_classes.contains(std::string(COUNTABLE_CLASS_NAME))
						? std::format("Define 'instance Countable<{}>' to give it a length.", actual_type->DisplayString())
						: actual_type->IsType<MarmotType::TextType>()
						? std::string("Import MarmotPrelude/Countable.mmt: it declares Countable and its instance for Text.")
						: std::format("Import MarmotPrelude/Countable.mmt, which declares Countable, and define 'instance Countable<{}>'.", actual_type->DisplayString());
					if (std::optional<CompilerError> error = CheckConstraintHolds(unary.m_op, MarmotType::ClassConstraint(std::string(COUNTABLE_CLASS_NAME), { actual_type }), 0u, suggestion))
					{
						return std::unexpected(std::move(*error));
					}

					unary.m_uses_countable = true;
					unary.m_type_data = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
					return unary.m_type_data;
				}

				unary.m_type_data = std::move(actual_type);
				return unary.m_type_data;
			}
		);
}

// Spawn(argument, F) runs F in a new worker. Its one argument stands for all of
// F's parameters, which no signature can say, and both what F takes and what it
// gives back must be able to cross.
MarmotResult::TypeResult TypeChecker::CheckSpawnCall(MarmotExpression::Call& call)
{
	const MarmotExpression::NameAccess& spawn_name = call.m_callee->GetExpression<MarmotExpression::NameAccess>();
	const Token& spawn_token = spawn_name.m_name;
	if (call.m_arguments.size() != 2u)
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, std::format("Call expression type error: incorrect arity: expected 2 argument(s), got {}", call.m_arguments.size()), call.m_paren, m_file_name, m_source_lines));
	}

	// The function is an ordinary value: it is evaluated here and transferred to the
	// worker, captured cells and all, so a lambda closing over the enclosing scope
	// works and a function held in a parameter can be spawned.
	// The ambient expected type belongs to the spawn's own result, Worker<T>, and
	// would otherwise be pushed onto a lambda written in callee position.
	std::unique_ptr<MarmotExpression>& function = call.m_arguments[1u];
	MarmotResult::TypeResult callee_result;
	{
		ExpectedTypeGuard guard(*this, nullptr);
		m_callee_name = &spawn_name;
		callee_result = Evaluate(call.m_callee);
		m_callee_name = function->IsExpression<MarmotExpression::NameAccess>() ? &function->GetExpression<MarmotExpression::NameAccess>() : nullptr;
		callee_result = std::move(callee_result).and_then([this, &function](std::shared_ptr<MarmotType>&&) { return Evaluate(function); });
		m_callee_name = nullptr;
	}

	if (!callee_result.has_value())
	{
		return callee_result;
	}

	std::shared_ptr<MarmotType> callee_type = ApplySubstitution(callee_result.value());
	if (!callee_type->IsType<MarmotType::FunctionType>())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeNotCallable, std::format("Spawn expression type error: the second argument to {} must be a function", spawn_token.m_lexeme), spawn_token, m_file_name, m_source_lines, callee_type));
	}

	MarmotType::FunctionType& function_type = callee_type->GetType<MarmotType::FunctionType>();
	if (function_type.m_is_foreign)
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Spawn expression type error: foreign functions cannot be spawned", spawn_token, m_file_name, m_source_lines));
	}

	if (std::optional<CompilerError> error = CheckSpawnArgument(call, function_type))
	{
		return std::unexpected(std::move(*error));
	}

	std::shared_ptr<MarmotType> resolved_return_type = ApplySubstitution(function_type.m_return_type);
	if (std::optional<CompilerError> error = EnsureTransferable(spawn_token, resolved_return_type))
	{
		return std::unexpected(std::move(*error));
	}

	call.m_type_data = MarmotType::MakeWorkerType(resolved_return_type);
	return call.m_type_data;
}

std::optional<CompilerError> TypeChecker::CheckSpawnArgument(MarmotExpression::Call& call, const MarmotType::FunctionType& function_type)
{
	const Token& spawn_token = call.m_callee->GetExpression<MarmotExpression::NameAccess>().m_name;
	const size_t arity = function_type.m_param_types.size();
	std::shared_ptr<MarmotType> expected_type;
	if (arity == 0u)
	{
		expected_type = MarmotType::MakeLiteralType<MarmotType::UnitType>();
	}
	else if (arity == 1u)
	{
		expected_type = function_type.m_param_types[0u];
	}
	else
	{
		std::vector<std::shared_ptr<MarmotType>> element_types(function_type.m_param_types.begin(), function_type.m_param_types.end());
		expected_type = MarmotType::MakeTupleType(std::move(element_types));
	}

	ExpectedTypeGuard guard(*this, expected_type);
	MarmotResult::TypeResult argument_result = Evaluate(call.m_arguments[0u]);
	if (!argument_result.has_value())
	{
		return std::move(argument_result.error());
	}

	std::shared_ptr<MarmotType> actual_type = ApplySubstitution(argument_result.value());
	if (arity >= 2u)
	{
		const bool is_matching_tuple = actual_type->IsType<MarmotType::TupleType>() && actual_type->GetType<MarmotType::TupleType>().m_element_types.size() == arity;
		if (!is_matching_tuple)
		{
			return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, std::format("Spawn expression type error: the spawned function takes {} parameters, so pass them to {} as one {}-element tuple", arity, spawn_token.m_lexeme, arity), spawn_token, m_file_name, m_source_lines, actual_type, expected_type);
		}
	}

	MarmotResult::TypeResult unify_result = Unify(spawn_token, actual_type, expected_type, UnifyDiagnosticMode::ActualExpected);
	if (!unify_result.has_value())
	{
		return std::move(unify_result.error());
	}

	for (const std::shared_ptr<MarmotType>& param_type : function_type.m_param_types)
	{
		if (std::optional<CompilerError> error = EnsureTransferable(spawn_token, ApplySubstitution(param_type)))
		{
			return error;
		}
	}

	return std::nullopt;
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::Send& send)
{
	return Evaluate(send.m_channel)
		.and_then
		(
			[this, &send](std::shared_ptr<MarmotType>&& channel_type) -> MarmotResult::TypeResult
			{
				std::shared_ptr<MarmotType> resolved_channel_type = ApplySubstitution(channel_type);
				if (!resolved_channel_type->IsType<MarmotType::ChannelType>())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Send expression type error: left operand must be Channel<T>", send.m_arrow, m_file_name, m_source_lines, resolved_channel_type));
				}

				std::shared_ptr<MarmotType> element_type = resolved_channel_type->GetType<MarmotType::ChannelType>().m_element_type;
				ExpectedTypeGuard guard(*this, element_type);
				return Evaluate(send.m_value)
					.and_then
					(
						[this, &send, element_type](std::shared_ptr<MarmotType>&& value_type) mutable -> MarmotResult::TypeResult
						{
							std::shared_ptr<MarmotType> actual_value_type = std::move(value_type);
							MarmotResult::TypeResult unify_result = Unify(send.m_arrow, actual_value_type, element_type, UnifyDiagnosticMode::ActualExpected);
							if (!unify_result.has_value())
							{
								return unify_result;
							}

							std::shared_ptr<MarmotType> resolved_element_type = ApplySubstitution(element_type);
							if (std::optional<CompilerError> error = EnsureTransferable(send.m_arrow, resolved_element_type))
							{
								return std::unexpected(std::move(*error));
							}

							send.m_type_data = MarmotType::MakeLiteralType<MarmotType::BoolType>();
							return send.m_type_data;
						}
					);
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::Receive& receive)
{
	return Evaluate(receive.m_channel)
		.and_then
		(
			[this, &receive](std::shared_ptr<MarmotType>&& channel_type) -> MarmotResult::TypeResult
			{
				std::shared_ptr<MarmotType> resolved_channel_type = ApplySubstitution(channel_type);
				if (!resolved_channel_type->IsType<MarmotType::ChannelType>())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Receive expression type error: expected Channel<T>", receive.m_arrow, m_file_name, m_source_lines, resolved_channel_type));
				}

				receive.m_type_data = ApplySubstitution(resolved_channel_type->GetType<MarmotType::ChannelType>().m_element_type);
				return receive.m_type_data;
			}
		);
}

std::shared_ptr<MarmotType> TypeChecker::NarrowClassMethodType(const std::string& class_name, const ClassInfo& class_info, const std::shared_ptr<MarmotType>& declared_method_type, const std::vector<std::shared_ptr<MarmotType>>& known_arg_types, size_t arity)
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

		std::shared_ptr<MarmotType> candidate_method_type = ApplySubstitution(TypeTransform::Instantiate(declared_method_type, class_substitutions));
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
			[&candidate_function_type, &known_arg_types, &matcher, this](size_t idx) -> bool
			{
				return matcher.Match(candidate_function_type.m_param_types[idx], ApplySubstitution(known_arg_types[idx]));
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
		narrowed_method_type = substitutions.empty() ? candidate_method_type : ApplySubstitution(TypeTransform::Instantiate(candidate_method_type, substitutions));
	}

	return narrowed_method_type;
}

// A class method called on the type a where clause in force names: the
// constraint, not an instance, says what the method's type is.
MarmotResult::TypeResult TypeChecker::CheckConstrainedMethodCall(MarmotExpression::Call& call, const ClassInfo& tc_info, const std::shared_ptr<MarmotType>& method_type, const MarmotType::ClassConstraint& constraint, std::vector<std::shared_ptr<MarmotType>>& arg_results)
{
	TypeEnvironment class_substitutions;
	for (size_t i = 0u; i < tc_info.m_type_param_names.size(); i += 1u)
	{
		class_substitutions.emplace(tc_info.m_type_param_names[i], ApplySubstitution(constraint.m_type_args[i]));
	}

	std::shared_ptr<MarmotType> resolved_method_type = ApplySubstitution(TypeTransform::Instantiate(method_type, class_substitutions));
	if (!resolved_method_type->IsType<MarmotType::FunctionType>())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeNotCallable, "Call expression type error: not a callable", call.m_paren, m_file_name, m_source_lines, resolved_method_type));
	}

	MarmotType::FunctionType& function_type = resolved_method_type->GetType<MarmotType::FunctionType>();
	if (function_type.m_param_types.size() != arg_results.size())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, std::format("Call expression type error: incorrect arity: expected {} argument(s), got {}", function_type.m_param_types.size(), arg_results.size()), call.m_paren, m_file_name, m_source_lines));
	}

	for (size_t idx : std::views::iota(0u, arg_results.size()))
	{
		MarmotResult::TypeResult result = Unify(call.m_paren, arg_results[idx], function_type.m_param_types[idx], UnifyDiagnosticMode::ActualExpected);
		if (!result.has_value())
		{
			return result;
		}
	}

	call.m_is_foreign = function_type.m_is_foreign;
	call.m_type_data = ApplySubstitution(function_type.m_return_type);
	return call.m_type_data;
}

// A class method selected from the instances by its arguments. `arg_results`
// holds the arguments already checked, which may be none; the rest are checked
// here, each against what the instances still matching say it must be.
MarmotResult::TypeResult TypeChecker::CheckInstanceMethodCall(MarmotExpression::Call& call, const std::string& qualifier, const std::string& method_name, const ClassInfo& tc_info, const std::shared_ptr<MarmotType>& method_type, std::vector<std::shared_ptr<MarmotType>>&& arg_results)
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
		std::shared_ptr<MarmotType> narrowed_method_type = NarrowClassMethodType(qualifier, tc_info, method_type, arg_results, call.m_arguments.size());
		std::shared_ptr<MarmotType> expected_param_type = narrowed_method_type != nullptr ? narrowed_method_type->GetType<MarmotType::FunctionType>().m_param_types[idx] : std::shared_ptr<MarmotType>{};
		ExpectedTypeGuard guard(*this, std::move(expected_param_type));

		MarmotResult::TypeResult arg_result = Evaluate(call.m_arguments[idx]);
		if (!arg_result.has_value())
		{
			return arg_result;
		}
		arg_results.emplace_back(std::move(arg_result.value()));
	}

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

		std::shared_ptr<MarmotType> candidate_method_type = ApplySubstitution(TypeTransform::Instantiate(method_type, class_substitutions));
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
			std::shared_ptr<MarmotType> resolved_arg = ApplySubstitution(arg_results[i]);
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
		// Convertable need. Applying it as a filter rejected every instance of
		// a class whose method returns a fixed type.
		bool matches_expected_type = false;
		if (m_expected_expr_type != nullptr)
		{
			InstanceTypeMatcher expected_matcher = matcher;
			std::shared_ptr<MarmotType> expected_type = ApplySubstitution(m_expected_expr_type);
			if (expected_matcher.Match(candidate_function_type.m_return_type, expected_type))
			{
				matches_expected_type = true;
				matcher = std::move(expected_matcher);
			}
		}

		TypeEnvironment substitutions = std::move(matcher).TakeBindings();
		std::shared_ptr<MarmotType> resolved_method_type = ApplySubstitution(TypeTransform::Instantiate(candidate_method_type, substitutions));
		candidates.emplace_back(std::move(resolved_method_type), &instance_info, std::move(substitutions), matches_expected_type);
	}

	if (candidates.empty())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Call expression type error: no matching concrete instance for '" + qualifier + NameSeparator.data() + method_name + "'", call.m_paren, m_file_name, m_source_lines));
	}
	if (candidates.size() != 1u)
	{
		if (std::ranges::count_if(candidates, &ConcreteMethodCandidate::m_matches_expected_type) != 1)
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Call expression type error: ambiguous concrete instance for '" + qualifier + NameSeparator.data() + method_name + "'", call.m_paren, m_file_name, m_source_lines));
		}

		std::erase_if(candidates, [](const ConcreteMethodCandidate& candidate) -> bool { return !candidate.m_matches_expected_type; });
	}

	MarmotType::FunctionType& function_type = candidates[0u].m_method_type->GetType<MarmotType::FunctionType>();
	for (size_t idx : std::views::iota(0u, arg_results.size()))
	{
		MarmotResult::TypeResult result = Unify(call.m_paren, arg_results[idx], function_type.m_param_types[idx], UnifyDiagnosticMode::ActualExpected);
		if (!result.has_value())
		{
			return result;
		}
	}

	MarmotResult::TypeResult constraint_result = ValidateFunctionConstraints(call.m_paren, function_type);
	if (!constraint_result.has_value())
	{
		return constraint_result;
	}

	// function_type comes from the class declaration, so it never carries the
	// selected instance's where-clause - that lives on InstanceInfo instead.
	MarmotResult::TypeResult instance_constraint_result = ValidateInstanceConstraints(call.m_paren, *candidates[0u].m_instance, candidates[0u].m_substitutions, 0u);
	if (!instance_constraint_result.has_value())
	{
		return instance_constraint_result;
	}

	call.m_is_foreign = function_type.m_is_foreign;
	call.m_type_data = ApplySubstitution(function_type.m_return_type);
	return call.m_type_data;
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::Call& call)
{
	const size_t pending = m_pending_instances.size();
	call.m_primitive = CalledPrimitive(call);
	MarmotResult::TypeResult checked = call.m_primitive == MarmotPrimitive::Spawn ? CheckSpawnCall(call) : CheckCall(call);
	return std::move(checked)
		.and_then([this, pending](std::shared_ptr<MarmotType>&& type) -> MarmotResult::TypeResult
		{
			if (std::optional<CompilerError> error = CheckDecidedPendingInstances(pending))
			{
				return std::unexpected(std::move(*error));
			}
			return type;
		});
}

// The primitive whose declaration a call's callee names. Only a name can name
// one, since a primitive is never a value.
std::optional<MarmotPrimitive> TypeChecker::CalledPrimitive(const MarmotExpression::Call& call) const
{
	if (!call.m_callee->IsExpression<MarmotExpression::NameAccess>())
	{
		return std::nullopt;
	}

	const std::shared_ptr<MarmotType>* binding = FindNameType(call.m_callee->GetExpression<MarmotExpression::NameAccess>().m_name.m_lexeme);
	return binding != nullptr && (*binding)->IsType<MarmotType::FunctionType>() ? (*binding)->GetType<MarmotType::FunctionType>().m_primitive : std::nullopt;
}

// The waiting instances from `from` on whose types are now decided, each checked
// with the constraints that were in force where it was used. The rest wait on.
std::optional<CompilerError> TypeChecker::CheckDecidedPendingInstances(size_t from)
{
	std::vector<PendingInstance> waiting;
	std::optional<CompilerError> error;
	for (PendingInstance& pending : m_pending_instances | std::views::drop(from))
	{
		const std::shared_ptr<MarmotType> instance = ApplySubstitution(pending.m_instance);
		if (!IsDecided(instance))
		{
			waiting.push_back(std::move(pending));
			continue;
		}
		if (!error.has_value())
		{
			std::vector<MarmotType::ClassConstraint> in_force = std::exchange(m_active_constraints, pending.m_active_constraints);
			MarmotResult::TypeResult validated = ValidateFunctionConstraints(pending.m_token, instance->GetType<MarmotType::FunctionType>());
			m_active_constraints = std::move(in_force);
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

MarmotResult::TypeResult TypeChecker::UseGenericInstance(const Token& token, std::shared_ptr<MarmotType> instance, std::string_view undecided_message)
{
	std::unordered_set<int> visible_type_vars = CollectEnclosingTypeVariableIds();
	if (m_expected_expr_type != nullptr)
	{
		MarmotResult::TypeResult expected_result = Unify(token, instance, m_expected_expr_type, UnifyDiagnosticMode::ActualExpected);
		if (!expected_result.has_value())
		{
			return expected_result;
		}
		std::unordered_set<int> expected_type_vars = CollectTypeVariableIds(ApplySubstitution(m_expected_expr_type));
		visible_type_vars.insert(expected_type_vars.cbegin(), expected_type_vars.cend());
	}

	instance = ApplySubstitution(instance);
	if (!std::ranges::all_of(CollectTypeVariableIds(instance), [&visible_type_vars](int id) { return visible_type_vars.contains(id); }))
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(std::string(undecided_message), token, m_file_name, m_source_lines));
	}
	if (!IsDecided(instance))
	{
		m_pending_instances.emplace_back(token, instance, m_active_constraints, std::string(undecided_message));
		return instance;
	}
	return ValidateFunctionConstraints(token, instance->GetType<MarmotType::FunctionType>())
		.transform([&instance](std::shared_ptr<MarmotType>&&) { return instance; });
}

// Decided: every variable left is a generic parameter of a definition being checked.
bool TypeChecker::IsDecided(const std::shared_ptr<MarmotType>& type) const
{
	return std::ranges::all_of(CollectTypeVariableIds(type), [this](int id) { return m_rigid_type_vars.contains(id); });
}

MarmotResult::TypeResult TypeChecker::CheckCall(MarmotExpression::Call& call)
{
	if (call.m_callee->IsExpression<MarmotExpression::NameAccess>())
	{
		MarmotExpression::NameAccess& callee_name = call.m_callee->GetExpression<MarmotExpression::NameAccess>();
		const std::string& full_name = callee_name.m_name.m_lexeme;
		size_t separator_pos = full_name.rfind(NameSeparator.data());
		if (separator_pos != std::string::npos)
		{
			const std::string qualifier = full_name.substr(0u, separator_pos);
			const std::string method_name = full_name.substr(separator_pos + NameSeparator.length());

			std::unordered_map<std::string, ClassInfo>::const_iterator tc_it = m_classes.find(qualifier);
			if (tc_it != m_classes.cend())
			{
				const ClassInfo& tc_info = tc_it->second;
				TypeEnvironment::const_iterator method_it = tc_info.m_method_types.find(method_name);
				if (method_it != tc_info.m_method_types.cend())
				{
					std::vector<std::shared_ptr<MarmotType>> arg_results;
					const std::vector<const MarmotType::ClassConstraint*> matching_constraints = m_active_constraints
						| std::views::filter([&qualifier](const MarmotType::ClassConstraint& constraint) { return constraint.m_class_name == qualifier; })
						| std::views::transform([](const MarmotType::ClassConstraint& constraint) { return &constraint; })
						| std::ranges::to<std::vector>();

					if (!matching_constraints.empty())
					{
						arg_results.reserve(call.m_arguments.size());
						for (std::unique_ptr<MarmotExpression>& call_arg : call.m_arguments)
						{
							MarmotResult::TypeResult arg_result = Evaluate(call_arg);
							if (!arg_result.has_value())
							{
								return arg_result;
							}
							arg_results.emplace_back(std::move(arg_result.value()));
						}

						// A where clause in force answers for the type it names. The method is
						// picked by its first argument, so a call on any other type - a concrete
						// one with an instance of its own - is selected from the instances.
						std::vector<const MarmotType::ClassConstraint*> selected_constraints = matching_constraints;
						if (!arg_results.empty())
						{
							const std::shared_ptr<MarmotType> first_arg_type = ApplySubstitution(arg_results[0u]);
							std::erase_if
							(
								selected_constraints,
								[this, &first_arg_type](const MarmotType::ClassConstraint* constraint)
								{
									return constraint->m_type_args.empty() || *ApplySubstitution(constraint->m_type_args[0u]) != *first_arg_type;
								}
							);
						}

						if (selected_constraints.size() > 1u)
						{
							const std::string first_arg_name = arg_results.empty() ? std::string("no arguments") : ApplySubstitution(arg_results[0u])->DisplayString();
							return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Call expression type error: ambiguous class method '" + qualifier + NameSeparator.data() + method_name + "' for argument type '" + first_arg_name + "'", call.m_paren, m_file_name, m_source_lines));
						}
						if (selected_constraints.size() == 1u)
						{
							return CheckConstrainedMethodCall(call, tc_info, method_it->second, *selected_constraints.front(), arg_results);
						}
					}

					return CheckInstanceMethodCall(call, qualifier, method_name, tc_info, method_it->second, std::move(arg_results));
				}
			}
		}
	}

	// A lambda is called directly only from inside parentheses: `fn(x) => x(3)`
	// would put the call in its body.
	const MarmotExpression* callee_inside_groups = call.m_callee.get();
	while (callee_inside_groups->IsExpression<MarmotExpression::Group>())
	{
		callee_inside_groups = callee_inside_groups->GetExpression<MarmotExpression::Group>().m_expr_in.get();
	}

	if (callee_inside_groups->IsExpression<MarmotExpression::Function>())
	{
		std::vector<std::shared_ptr<MarmotType>> arg_results;
		arg_results.reserve(call.m_arguments.size());
		for (std::unique_ptr<MarmotExpression>& call_arg : call.m_arguments)
		{
			MarmotResult::TypeResult arg_result = Evaluate(call_arg);
			if (!arg_result.has_value())
			{
				return arg_result;
			}
			arg_results.emplace_back(std::move(arg_result.value()));
		}

		std::shared_ptr<MarmotType> expected_return_type = m_expected_expr_type != nullptr ? std::shared_ptr<MarmotType>(m_expected_expr_type) : FreshTypeVar();
		std::shared_ptr<MarmotType> expected_callee_type = MarmotType::MakeFunctionType(arg_results, std::move(expected_return_type));
		ExpectedTypeGuard guard(*this, expected_callee_type);

		return Evaluate(call.m_callee)
			.and_then
			(
				[&call, &arg_results, this](std::shared_ptr<MarmotType>&& actual_type) -> MarmotResult::TypeResult
				{
					std::shared_ptr<MarmotType> resolved_type = ApplySubstitution(actual_type);
					if (!resolved_type->IsType<MarmotType::FunctionType>())
					{
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeNotCallable, "Call expression type error: not a callable", call.m_paren, m_file_name, m_source_lines, resolved_type));
					}

					MarmotType::FunctionType& function_type = resolved_type->GetType<MarmotType::FunctionType>();
					if (function_type.m_param_types.size() != call.m_arguments.size())
					{
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, std::format("Call expression type error: incorrect arity: expected {} argument(s), got {}", function_type.m_param_types.size(), call.m_arguments.size()), call.m_paren, m_file_name, m_source_lines));
					}

					for (size_t idx : std::views::iota(0u, arg_results.size()))
					{
						std::shared_ptr<MarmotType>& actual_param_type = arg_results[idx];
						std::shared_ptr<MarmotType>& param_type = function_type.m_param_types[idx];
						MarmotResult::TypeResult result = Unify(call.m_paren, actual_param_type, param_type, UnifyDiagnosticMode::ActualExpected);
						if (!result.has_value())
						{
							return result;
						}
					}

					call.m_is_foreign = function_type.m_is_foreign;
					call.m_type_data = ApplySubstitution(function_type.m_return_type);
					return call.m_type_data;
				}
			);
	}

	// The type expected of the call is its result's, not the callee's.
	MarmotResult::TypeResult callee_result = [&call, this]() -> MarmotResult::TypeResult
		{
			ExpectedTypeGuard guard(*this, nullptr);
			m_callee_name = call.m_callee->IsExpression<MarmotExpression::NameAccess>() ? &call.m_callee->GetExpression<MarmotExpression::NameAccess>() : nullptr;
			MarmotResult::TypeResult result = Evaluate(call.m_callee);
			m_callee_name = nullptr;
			return result;
		}();

	return std::move(callee_result)
		.and_then
		(
			[&call, this](std::shared_ptr<MarmotType>&& actual_type) ->MarmotResult::TypeResult
			{
				std::shared_ptr<MarmotType> resolved_type = ApplySubstitution(actual_type);

				if (!resolved_type->IsType<MarmotType::FunctionType>())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeNotCallable, "Call expression type error: not a callable", call.m_paren, m_file_name, m_source_lines, resolved_type));
				}

				MarmotType::FunctionType& function_type = resolved_type->GetType<MarmotType::FunctionType>();
				if (function_type.m_param_types.size() != call.m_arguments.size())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, std::format("Call expression type error: incorrect arity: expected {} argument(s), got {}", function_type.m_param_types.size(), call.m_arguments.size()), call.m_paren, m_file_name, m_source_lines));
				}

				for (size_t idx : std::views::iota(0u, call.m_arguments.size()))
				{
					// Each argument is unified against its parameter before the next one is checked, so a
					// parameter type that an earlier argument decides is already resolved by the time it
					// becomes the expected type. Deferring every unification to a second pass left the
					// callee's freshened variables unbound, so `Append(buckets, Slot::Empty())` on
					// `fn<T>(Array<T>, T) -> Unit` checked the construction against a bare T and rejected it.
					ExpectedTypeGuard guard(*this, function_type.m_param_types[idx]);

					MarmotResult::TypeResult arg_result = Evaluate(call.m_arguments[idx]);
					if (!arg_result.has_value())
					{
						return arg_result;
					}

					MarmotResult::TypeResult unify_result = Unify(call.m_paren, arg_result.value(), function_type.m_param_types[idx], UnifyDiagnosticMode::ActualExpected);
					if (!unify_result.has_value())
					{
						return unify_result;
					}
				}

				// A where clause may be on a type only the result names, which where the
				// result goes decides, as it does for a generic used as a value.
				if (!function_type.m_constraints.empty() && m_expected_expr_type != nullptr && !IsDecided(ApplySubstitution(function_type.m_return_type)))
				{
					MarmotResult::TypeResult expected_result = Unify(call.m_paren, function_type.m_return_type, m_expected_expr_type, UnifyDiagnosticMode::ActualExpected);
					if (!expected_result.has_value())
					{
						return expected_result;
					}
				}

				MarmotResult::TypeResult constraint_result = ValidateFunctionConstraints(call.m_paren, function_type);
				if (!constraint_result.has_value())
				{
					return constraint_result;
				}

				call.m_is_foreign = function_type.m_is_foreign;
				call.m_type_data = ApplySubstitution(function_type.m_return_type);

				return call.m_type_data;
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::MemberAccess& get)
{
	// The type the context demands of `a.b` describes the member, not the object: `a`'s type is
	// settled by `a` alone, and the member name then picks a field out of it. Carrying the
	// demand into the object let a construction there unify its own struct type with whatever
	// the surrounding call wanted, so `IO::PrintLine((new Point(7, 8)).x as Text)` was rejected
	// for building a Point where Text was expected.
	ExpectedTypeGuard object_guard(*this, std::shared_ptr<MarmotType>{});

	return Evaluate(get.m_struct)
		.and_then
		(
			[this, &get](std::shared_ptr<MarmotType>&& object_type) -> MarmotResult::TypeResult
			{
				const std::shared_ptr<MarmotType> actual_type = ApplySubstitution(object_type);
				if (!actual_type->IsType<MarmotType::StructType>())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Cannot access member on non-struct type", get.m_member_name, m_file_name, m_source_lines));
				}

				const MarmotType::StructType& struct_type = actual_type->GetType<MarmotType::StructType>();
				std::vector<std::string>::const_iterator find_result = std::find(struct_type.m_member_names.cbegin(), struct_type.m_member_names.cend(), get.m_member_name.m_lexeme);
				if (find_result == struct_type.m_member_names.cend())
				{
					std::string suggestion = std::format("Struct '{}' does not have a member named '{}'", struct_type.m_name, get.m_member_name.m_lexeme);
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Unknown struct member", get.m_member_name, m_file_name, m_source_lines, suggestion));
				}

				get.m_index = static_cast<int>(find_result - struct_type.m_member_names.cbegin());

				get.m_type_data = struct_type.m_member_types[static_cast<size_t>(get.m_index)];
				return get.m_type_data;
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::NameAccess& variable)
{
	const MarmotExpression::NameContext::Local* local = std::get_if<MarmotExpression::NameContext::Local>(&variable.m_name_ctx);
	if (local != nullptr && local->m_is_generated)
	{
		const auto scopes = m_type_scopes | std::views::reverse;
		const auto scope = std::ranges::find_if(scopes, [local](const TypeScope& candidate) { return candidate.m_generated.contains(local->m_index); });
		assert(scope != scopes.end());
		variable.m_type_data = ApplySubstitution(scope->m_generated.at(local->m_index));
		return variable.m_type_data;
	}
	size_t separator_pos = variable.m_name.m_lexeme.rfind(NameSeparator.data());
	if (separator_pos != std::string::npos)
	{
		std::string qualifier = variable.m_name.m_lexeme.substr(0u, separator_pos);
		std::string symbol_name = variable.m_name.m_lexeme.substr(separator_pos + NameSeparator.length());

		for (const MarmotType::ClassConstraint& constraint : m_active_constraints)
		{
			if (constraint.m_class_name != qualifier)
			{
				continue;
			}

			std::unordered_map<std::string, ClassInfo>::iterator tc_it = m_classes.find(constraint.m_class_name);
			if (tc_it == m_classes.end())
			{
				continue;
			}

			const ClassInfo& tc_info = tc_it->second;
			TypeEnvironment::const_iterator method_it = tc_info.m_method_types.find(symbol_name);
			if (method_it != tc_info.m_method_types.cend())
			{
				variable.m_type_data = Freshen(method_it->second);
				return variable.m_type_data;
			}
		}

		if (m_classes.contains(qualifier))
		{
			std::vector<std::string> active_classes;
			active_classes.reserve(m_active_constraints.size());
			for (const MarmotType::ClassConstraint& constraint : m_active_constraints)
			{
				active_classes.push_back(constraint.m_class_name);
			}

			std::string suggestion = active_classes.empty()
				? "No matching class constraints are active in this scope."
				: "Active constraints: " + JoinSortedNames(std::move(active_classes));
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Name access expression type error: class method is not available in the current constraint scope", variable.m_name, m_file_name, m_source_lines, suggestion));
		}
	}

	const std::shared_ptr<MarmotType>* binding = FindNameType(variable.m_name.m_lexeme);
	const bool is_defining = std::ranges::contains(m_defining_generic_names, variable.m_name.m_lexeme);
	const std::vector<LocalGenericFunction>::const_reverse_iterator local_generic = std::ranges::find(m_local_generic_functions | std::views::reverse, variable.m_name.m_lexeme, &LocalGenericFunction::m_name);
	const bool is_value = &variable != m_callee_name;
	if (is_value && binding != nullptr && (*binding)->IsType<MarmotType::FunctionType>() && (*binding)->GetType<MarmotType::FunctionType>().m_primitive.has_value())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(std::format("Name access expression type error: '{}' is a primitive, an instruction rather than a function, so it can only be called", variable.m_name.m_lexeme), variable.m_name, m_file_name, m_source_lines));
	}
	const std::function<MarmotResult::TypeResult()> use_as_value = [this, &variable]()
	{
		return UseGenericInstance(variable.m_name, variable.m_type_data, std::format("Name access expression type error: could not infer which types '{}' is used at; pass it where a function type is expected, or write the type on the definition that holds it", variable.m_name.m_lexeme))
			.transform([&variable](std::shared_ptr<MarmotType>&& instance) { return variable.m_type_data = std::move(instance); });
	};
	if (binding != nullptr && !is_defining && local_generic != m_local_generic_functions.crend())
	{
		variable.m_type_data = Instantiate(*binding, local_generic->m_parameters);
		return is_value ? use_as_value() : variable.m_type_data;
	}
	if (binding != nullptr)
	{
		const bool binding_is_generic_function =
			m_generic_functions.contains(variable.m_name.m_lexeme)
			&& std::ranges::find(m_defining_generic_names, variable.m_name.m_lexeme) == m_defining_generic_names.cend()
			&& binding->get()->IsType<MarmotType::FunctionType>();
		if (binding_is_generic_function
			|| variable.m_name.m_lexeme.find(NameSeparator) != std::string::npos
			|| (binding->get()->IsType<MarmotType::FunctionType>() && binding->get()->GetType<MarmotType::FunctionType>().m_is_foreign))
		{
			variable.m_type_data = Freshen(*binding);
			if (is_value && variable.m_type_data->IsType<MarmotType::FunctionType>() && !CollectTypeVariableIds(variable.m_type_data).empty())
			{
				return use_as_value();
			}
		}
		else
		{
			// Apply substitution to get the most up-to-date type
			// This handles cases where the type contains type variables that have been unified
			variable.m_type_data = ApplySubstitution(*binding);
		}
		return variable.m_type_data;
	}

	for (const MarmotType::ClassConstraint& constraint : m_active_constraints)
	{
		std::unordered_map<std::string, ClassInfo>::iterator tc_it = m_classes.find(constraint.m_class_name);
		if (tc_it != m_classes.end())
		{
			const ClassInfo& tc_info = tc_it->second;
			TypeEnvironment::const_iterator method_it = tc_info.m_method_types.find(variable.m_name.m_lexeme);
			if (method_it != tc_info.m_method_types.cend())
			{
				// This is a valid class method - return its type
				// The actual name resolution to the mangled method happens in code generation
				variable.m_type_data = Freshen(method_it->second);
				return variable.m_type_data;
			}
		}
	}

	// A top-level definition the checker has not reached yet: its type is the
	// one written on it.
	const TypeEnvironment::const_iterator later = m_later_definitions.find(variable.m_name.m_lexeme);
	if (later != m_later_definitions.cend())
	{
		variable.m_type_data = Freshen(later->second);
		return variable.m_type_data;
	}

	if (m_failed_definitions.contains(variable.m_name.m_lexeme))
	{
		m_used_failed_definition = true;
		variable.m_type_data = FreshTypeVar();
		return variable.m_type_data;
	}

	if (m_later_unannotated.contains(variable.m_name.m_lexeme))
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUndefinedName, std::format("'{}' is defined further down, and its type is not written on it, so it is not known here. Give it a type -- its parameter and return types, for a function -- or move it above this.", variable.m_name.m_lexeme), variable.m_name, m_file_name, m_source_lines));
	}

	return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUndefinedName, "Name access expression type error: variable not found", variable.m_name, m_file_name, m_source_lines));
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::Literal& literal)
{
	switch (literal.m_kind)
	{
	case MarmotExpression::LiteralKind::Text:
		literal.m_type_data = MarmotType::MakeLiteralType<MarmotType::TextType>();
		break;
	case MarmotExpression::LiteralKind::Bool:
		literal.m_type_data = MarmotType::MakeLiteralType<MarmotType::BoolType>();
		break;
	case MarmotExpression::LiteralKind::Float:
		literal.m_type_data = MarmotType::MakeLiteralType<MarmotType::FloatType>();
		break;
	case MarmotExpression::LiteralKind::Integer:
		literal.m_type_data = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
		break;
	case MarmotExpression::LiteralKind::Byte:
		literal.m_type_data = MarmotType::MakeLiteralType<MarmotType::ByteType>();
		break;
	case MarmotExpression::LiteralKind::Word:
		literal.m_type_data = MarmotType::MakeLiteralType<MarmotType::WordType>();
		break;
	case MarmotExpression::LiteralKind::Unit:
		literal.m_type_data = MarmotType::MakeLiteralType<MarmotType::UnitType>();
		break;
	}
	return literal.m_type_data;
}

MarmotResult::TypeResult TypeChecker::ResolveFunctionExpressionSignature(MarmotExpression::Function& function, const std::unordered_set<int>& outer_visible_type_vars)
{
	function.m_type_data = ApplySubstitution(function.m_type_data);
	const MarmotType::FunctionType& resolved_type = function.m_type_data->GetType<MarmotType::FunctionType>();
	function.m_param_types = resolved_type.m_param_types;
	function.m_return_type = resolved_type.m_return_type;

	if (HasTypeVariables(function.m_type_data))
	{
		std::unordered_set<int> unresolved_type_vars = CollectTypeVariableIds(function.m_type_data);
		bool only_outer_type_vars_remain = std::ranges::all_of
		(
			unresolved_type_vars,
			[&outer_visible_type_vars](int type_var_id) { return outer_visible_type_vars.contains(type_var_id); }
		);

		if (!only_outer_type_vars_remain)
		{
			return std::unexpected
			(
				MarmotError::GenerateTypeCheckerErrorWithContext
				(
					"Function expression type error: could not infer all lambda parameter or return types",
					function.m_function_keyword,
					m_file_name,
					m_source_lines
				)
			);
		}
	}

	return function.m_type_data;
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::Function& function)
{
	// An anonymous generic lambda is one value, so it takes one instance of its
	// generic type, which the context decides. Its parameter and return types
	// stay generic for lowering to read the instance against.
	if (!function.m_generic_params.empty())
	{
		std::vector<int> parameters;
		return CheckGenericLambda(function, function.m_function_keyword, std::string(), [&parameters](const std::shared_ptr<MarmotType>&, const std::vector<int>& own) { parameters = own; })
			.and_then([&](std::shared_ptr<MarmotType>&&) { return UseGenericInstance(function.m_function_keyword, Instantiate(function.m_type_data, parameters), "Function expression type error: could not infer which types this generic lambda is used at"); })
			.transform([&function](std::shared_ptr<MarmotType>&& instance) { return function.m_type_data = std::move(instance); });
	}

	// Preserve visible generic type variables when a lambda annotation refers to them.
	TypeTransform::Freshener freshening_context = MakeLambdaFresheningContext();

	// Freshen any UndecidedType parameters to TypeVariables
	for (std::shared_ptr<MarmotType>& param_type : function.m_param_types)
	{
		param_type = freshening_context.Apply(param_type);
	}
	function.m_return_type = freshening_context.Apply(function.m_return_type);

	// Create the function type and store it so it can be returned
	std::shared_ptr<MarmotType> return_type_copy = function.m_return_type;
	function.m_type_data = MarmotType::MakeFunctionType(function.m_param_types, std::move(return_type_copy));

	if (m_expected_expr_type != nullptr)
	{
		std::shared_ptr<MarmotType> expected_type = m_expected_expr_type;
		MarmotResult::TypeResult expected_result = Unify(function.m_function_keyword, function.m_type_data, expected_type, UnifyDiagnosticMode::ActualExpected);
		if (!expected_result.has_value())
		{
			return expected_result;
		}

		function.m_type_data = ApplySubstitution(function.m_type_data);
		const MarmotType::FunctionType& inferred_type = function.m_type_data->GetType<MarmotType::FunctionType>();
		function.m_param_types = inferred_type.m_param_types;
		function.m_return_type = inferred_type.m_return_type;
	}

	// Collected before the lambda's own scope is pushed, so its parameters - the very
	// things this visitor has to infer - are not in the set.
	std::unordered_set<int> outer_visible_type_vars = CollectEnclosingTypeVariableIds();
	if (m_expected_expr_type != nullptr)
	{
		std::unordered_set<int> expected_type_vars = CollectTypeVariableIds(ApplySubstitution(m_expected_expr_type));
		outer_visible_type_vars.insert(expected_type_vars.cbegin(), expected_type_vars.cend());
	}

	std::vector<MarmotType::ClassConstraint> function_constraints = CollectSignatureConstraints(function.m_param_types, function.m_return_type);
	function.m_type_data->GetType<MarmotType::FunctionType>().m_constraints = function_constraints;

	return ScopeSession(*this).Then([&]() -> MarmotResult::TypeResult
	{
		BindParameters(function.m_params, function.m_param_types);

		return CheckFunctionBody(function.m_body, function.m_return_type, function_constraints, function.m_function_keyword)
			.and_then([&function, &outer_visible_type_vars, this](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
			{
				return ResolveFunctionExpressionSignature(function, outer_visible_type_vars);
			});
	});
}

// Each value moved to its field's position, which makes a named construction
// the positional one.
std::optional<CompilerError> TypeChecker::PlaceNamedFields(MarmotExpression::Construct& construct, const MarmotType::StructType& struct_type)
{
	std::vector<std::unique_ptr<MarmotExpression>> placed(struct_type.m_member_names.size());
	for (size_t written = 0u; written < construct.m_field_names.size(); written += 1u)
	{
		const Token& field = construct.m_field_names[written];
		const std::vector<std::string>::const_iterator found = std::ranges::find(struct_type.m_member_names, field.m_lexeme);
		if (found == struct_type.m_member_names.cend())
		{
			return MarmotError::GenerateTypeCheckerErrorWithContext(std::format("Construct expression type error: '{}' has no field named '{}'", struct_type.m_name, field.m_lexeme), field, m_file_name, m_source_lines);
		}
		placed[static_cast<size_t>(std::distance(struct_type.m_member_names.cbegin(), found))] = std::move(construct.m_params[written]);
	}

	const std::vector<std::string> missing = std::views::iota(0u, placed.size())
		| std::views::filter([&placed](size_t index) { return placed[index] == nullptr; })
		| std::views::transform([&struct_type](size_t index) { return std::format("'{}'", struct_type.m_member_names[index]); })
		| std::ranges::to<std::vector>();
	if (!missing.empty())
	{
		return MarmotError::GenerateTypeCheckerErrorWithContext(std::format("Construct expression type error: '{}' needs a value for {}", struct_type.m_name, missing | std::views::join_with(std::string_view(", ")) | std::ranges::to<std::string>()), construct.m_data_name, m_file_name, m_source_lines);
	}

	construct.m_params = std::move(placed);
	construct.m_field_names.clear();
	return std::nullopt;
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::Construct& construct)
{
	const std::shared_ptr<MarmotType>& return_type = construct.m_return_type;
	std::string constructor_name;
	std::string actual_type_name;
	std::string declaring_module_name;

	if (construct.IsConstructTypeOf<MarmotExpression::Construct::Struct>())
	{
		const MarmotType::StructType& struct_type = return_type->GetType<MarmotType::StructType>();
		actual_type_name = struct_type.m_name;
		constructor_name = actual_type_name;
		declaring_module_name = struct_type.m_module_name;
	}
	else
	{
		const MarmotType::UnionType& union_type = return_type->GetType<MarmotType::UnionType>();
		constructor_name = construct.m_data_name.m_lexeme;
		actual_type_name = union_type.m_name;
		declaring_module_name = union_type.m_module_name;
	}

	const std::shared_ptr<MarmotType>* constructor_type_ptr = FindNameType(ConstructorKey(declaring_module_name, constructor_name));
	if (constructor_type_ptr == nullptr)
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Construct expression type error: struct or union not found", construct.m_data_name, m_file_name, m_source_lines));
	}

	if (!construct.m_field_names.empty())
	{
		const TypeDefinitionMap::const_iterator definition = m_struct_type_definitions.find(ConstructorKey(declaring_module_name, actual_type_name));
		if (std::optional<CompilerError> error = PlaceNamedFields(construct, definition->second->GetType<MarmotType::StructType>()))
		{
			return std::unexpected(std::move(*error));
		}
	}

	bool is_generic = false;
	if (construct.IsConstructTypeOf<MarmotExpression::Construct::Struct>())
	{
		is_generic = m_generic_structs.contains(ConstructorKey(declaring_module_name, actual_type_name));
	}
	else
	{
		is_generic = m_generic_unions.contains(ConstructorKey(declaring_module_name, actual_type_name));
	}

	std::shared_ptr<MarmotType> constructor_type_shared = is_generic ? Freshen(*constructor_type_ptr) : *constructor_type_ptr;

	// The name must resolve to the constructor's function type. It does not when a
	// struct is imported into another module, where the name binds to the type
	// itself, and reading it as a function would be undefined rather than an error.
	if (!constructor_type_shared->IsType<MarmotType::FunctionType>())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Construct expression type error: '" + constructor_name + "' does not name a constructor here (it resolved to '" + constructor_type_shared->DisplayString() + "')", construct.m_data_name, m_file_name, m_source_lines));
	}

	MarmotType::FunctionType& constructor_type = constructor_type_shared->GetType<MarmotType::FunctionType>();

	if (construct.m_has_explicit_type_args)
	{
		construct.m_return_type = InstantiateWrittenType(construct.m_return_type);
		MarmotResult::TypeResult explicit_type_result = Unify(construct.m_data_name, constructor_type.m_return_type, construct.m_return_type, UnifyDiagnosticMode::ActualExpected);
		if (!explicit_type_result.has_value())
		{
			return explicit_type_result;
		}
	}
	else if (m_expected_expr_type != nullptr)
	{
		std::shared_ptr<MarmotType> expected_type = m_expected_expr_type;
		MarmotResult::TypeResult expected_type_result = Unify(construct.m_data_name, constructor_type.m_return_type, expected_type, UnifyDiagnosticMode::ActualExpected);
		if (!expected_type_result.has_value())
		{
			return expected_type_result;
		}
	}

	if (constructor_type.m_param_types.size() != construct.m_params.size())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, std::format("Construct expression type error: incorrect arity: expected {} argument(s), got {}", constructor_type.m_param_types.size(), construct.m_params.size()), construct.m_data_name, m_file_name, m_source_lines));
	}

	for (size_t idx : std::views::iota(0u, construct.m_params.size()))
	{
		std::unique_ptr<MarmotExpression>& param = construct.m_params[idx];
		ExpectedTypeGuard guard(*this, constructor_type.m_param_types[idx]);
		MarmotResult::TypeResult param_result = Evaluate(param);
		if (!param_result.has_value())
		{
			return param_result;
		}

		std::shared_ptr<MarmotType> param_type = constructor_type.m_param_types[idx];
		MarmotResult::TypeResult unify_result = Unify(construct.m_data_name, param_result.value(), param_type, UnifyDiagnosticMode::ActualExpected);
		if (!unify_result.has_value())
		{
			return unify_result;
		}
	}

	MarmotResult::TypeResult constraint_result = ValidateFunctionConstraints(construct.m_data_name, constructor_type);
	if (!constraint_result.has_value())
	{
		return constraint_result;
	}

	// For generic structs, apply substitution to the return type to get the monomorphized type
	// After unifying parameters, type variables have been substituted with concrete types
	construct.m_type_data = ApplySubstitution(constructor_type.m_return_type);

	if (HasTypeVariables(construct.m_type_data))
	{
		// Inside a generic definition the declared types have already been freshened, so
		// `new Bag(xs, t)` in `def MakeBag = fn<T>(xs : Array<T>, t : Int) -> Bag<T>` infers
		// Bag<T0> for a type variable T0 that stands for T. That is fully inferred, not
		// ambiguous - the enclosing definition owns T0 and monomorphisation decides it at
		// each call site. Only a variable that no enclosing definition owns is ambiguous.
		std::unordered_set<int> enclosing_type_vars = CollectEnclosingTypeVariableIds();
		std::unordered_set<int> unresolved_type_vars = CollectTypeVariableIds(construct.m_type_data);

		bool only_enclosing_type_vars_remain = std::ranges::all_of
		(
			unresolved_type_vars,
			[&enclosing_type_vars](int type_var_id) { return enclosing_type_vars.contains(type_var_id); }
		);

		if (!only_enclosing_type_vars_remain)
		{
			return std::unexpected(
				MarmotError::GenerateTypeCheckerErrorWithContext(
					std::format("Construct expression type error: could not infer all type arguments for '{}'", actual_type_name),
					construct.m_data_name,
					m_file_name,
					m_source_lines
				)
			);
		}
	}

	// Mark as generic instantiation if this was a generic struct/union
	if (construct.IsConstructTypeOf<MarmotExpression::Construct::Struct>())
	{
		if (is_generic && construct.m_type_data->IsType<MarmotType::StructType>())
		{
			construct.m_type_data->GetType<MarmotType::StructType>().m_is_generic_instantiation = true;
		}
	}
	else if (is_generic && construct.m_type_data->IsType<MarmotType::UnionType>())
	{
		construct.m_type_data->GetType<MarmotType::UnionType>().m_is_generic_instantiation = true;
	}

	return construct.m_type_data;
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::RecordUpdate& record_update)
{
	MarmotResult::TypeResult source_result = Evaluate(record_update.m_source);
	if (!source_result.has_value())
	{
		return source_result;
	}

	std::shared_ptr<MarmotType> source_type = ApplySubstitution(source_result.value());

	for (MarmotExpression::RecordUpdate::FieldUpdate& update : record_update.m_updates)
	{
		std::shared_ptr<MarmotType> member_type = source_type;
		update.m_indices.clear();
		for (const Token& step : update.m_path)
		{
			// A union's variants hold their fields by position, so there is no name to set.
			if (member_type->IsType<MarmotType::UnionType>())
			{
				std::string suggestion = std::format("'{}' is a union, whose variants' fields have no names; match on it and construct the variant instead", member_type->GetType<MarmotType::UnionType>().m_name);
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Record update expression type error: not a record", step, m_file_name, m_source_lines, std::optional<std::string_view>(suggestion)));
			}

			if (!member_type->IsType<MarmotType::StructType>())
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Record update expression type error: not a struct", step, m_file_name, m_source_lines, member_type));
			}

			const MarmotType::StructType& struct_type = member_type->GetType<MarmotType::StructType>();
			std::vector<std::string>::const_iterator find_result = std::ranges::find(struct_type.m_member_names, step.m_lexeme);
			if (find_result == struct_type.m_member_names.cend())
			{
				std::string suggestion = std::format("Struct '{}' does not have a member named '{}'", struct_type.m_name, step.m_lexeme);
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Unknown struct member in record update", step, m_file_name, m_source_lines, std::optional<std::string_view>(suggestion)));
			}

			const size_t index = static_cast<size_t>(find_result - struct_type.m_member_names.cbegin());
			update.m_indices.push_back(static_cast<int>(index));
			member_type = ApplySubstitution(struct_type.m_member_types[index]);
		}

		ExpectedTypeGuard guard(*this, member_type);
		MarmotResult::TypeResult value_result = Evaluate(update.m_value);
		if (!value_result.has_value())
		{
			return value_result;
		}

		MarmotResult::TypeResult unify_result = Unify(update.m_path.back(), member_type, value_result.value(), UnifyDiagnosticMode::ExpectedActual);
		if (!unify_result.has_value())
		{
			return unify_result;
		}
	}

	// The result is the source's own already-resolved type, so no inference is needed here
	// at all. Construct instead looks the constructor function up by name, freshens it and
	// infers the type arguments from the supplied members; that also works inside a generic
	// function - `new Bag(b.items, t)` in `def Retag = fn<T>(b : Bag<T>, t : Int) -> Bag<T>`
	// infers Bag<T0>, a type variable the enclosing function owns. See
	// test/generics/success/generic_construction_in_generic_function.mmt.
	record_update.m_type_data = source_type;
	return record_update.m_type_data;
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::Array& array)
{
	std::shared_ptr<MarmotType> expected_array_type;
	std::shared_ptr<MarmotType> expected_element_type;
	if (m_expected_expr_type != nullptr)
	{
		std::shared_ptr<MarmotType> resolved_expected_type = ApplySubstitution(m_expected_expr_type);
		if (resolved_expected_type->IsType<MarmotType::ArrayType>())
		{
			expected_array_type = resolved_expected_type;
			expected_element_type = resolved_expected_type->GetType<MarmotType::ArrayType>().m_element_type;
		}
	}

	if (array.m_elems.empty())
	{
		array.m_type_data = expected_array_type != nullptr ? expected_array_type : MarmotType::MakeArrayType(MarmotType::MakeUndecidedType());
		return array.m_type_data;
	}

	ExpectedTypeGuard element_guard(*this, expected_element_type);

	std::vector<std::shared_ptr<MarmotType>> element_results;
	element_results.reserve(array.m_elems.size());

	for (std::unique_ptr<MarmotExpression>& element : array.m_elems)
	{
		MarmotResult::TypeResult result = Evaluate(element);
		if (!result.has_value())
		{
			return result;
		}

		element_results.emplace_back(std::move(result.value()));
	}

	// The elements share one type, which each may tell more of: `[[], [1]]` is
	// Array<Array<Int>>, and an element that never finishes takes the others' type.
	std::shared_ptr<MarmotType> element_type = element_results[0u];
	for (std::shared_ptr<MarmotType>& element_result : element_results | std::views::drop(1))
	{
		std::shared_ptr<MarmotType> resolved_element_type = ApplySubstitution(element_type);
		MarmotResult::TypeResult unified = Unify(array.m_op, resolved_element_type, element_result);
		if (!unified.has_value())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Array expression type error: inconsistent element types", array.m_op, m_file_name, m_source_lines, ApplySubstitution(element_result), ApplySubstitution(element_type)));
		}
		element_type = unified.value();
	}

	array.m_type_data = MarmotType::MakeArrayType(ApplySubstitution(element_type));
	return array.m_type_data;
}

MarmotResult::TypeResult TypeChecker::ResolveIndexableElementType(const Token& op, const std::shared_ptr<MarmotType>& container_type, const std::shared_ptr<MarmotType>& index_type)
{
	const MarmotType::ClassConstraint constraint(std::string(INDEXABLE_CLASS_NAME), { container_type, index_type });

	std::optional<ResolvedInstanceMatch> resolved_match = FindMatchingInstance(std::string(INDEXABLE_CLASS_NAME), constraint.m_type_args);
	if (resolved_match.has_value())
	{
		AssociatedTypeEnvironment::const_iterator binding_it = resolved_match->m_instance->m_associated_type_bindings.find(std::string(ELEMENT_ASSOCIATED_TYPE_NAME));
		if (binding_it == resolved_match->m_instance->m_associated_type_bindings.cend())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Index expression type error: the matching Indexable instance does not bind Element", op, m_file_name, m_source_lines, container_type));
		}

		return ApplySubstitution(TypeTransform::Instantiate(binding_it->second, resolved_match->m_substitutions));
	}

	if (IsSatisfiedByActiveConstraint(constraint))
	{
		return ApplySubstitution(MarmotType::MakeAssociatedType(std::string(INDEXABLE_CLASS_NAME), std::string(ELEMENT_ASSOCIATED_TYPE_NAME), { container_type, index_type }));
	}

	return std::unexpected(MakeConstraintFailureError(op, constraint));
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::IndexAccess& array_get)
{
	return Evaluate(array_get.m_arr_var)
		.and_then
		(
			[&array_get, this](std::shared_ptr<MarmotType>&& container_type) ->MarmotResult::TypeResult
			{
				return Evaluate(array_get.m_index)
					.and_then
					(
						[&array_get, &container_type, this](std::shared_ptr<MarmotType>&& raw_index_type) ->MarmotResult::TypeResult
						{
							std::shared_ptr<MarmotType> index_type = ApplySubstitution(raw_index_type);
							container_type = ApplySubstitution(container_type);

							if (container_type->IsType<MarmotType::ArrayType>())
							{
								if (!index_type->IsType<MarmotType::IntegerType>())
								{
									return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Array get expression type error: index must be integer", array_get.m_op, m_file_name, m_source_lines, index_type, MarmotType::MakeLiteralType<MarmotType::IntegerType>()));
								}

								array_get.m_type_data = ApplySubstitution(container_type->GetType<MarmotType::ArrayType>().m_element_type);
								return array_get.m_type_data;
							}

							return ResolveIndexableElementType(array_get.m_op, container_type, index_type)
								.and_then
								(
									[&array_get, this](std::shared_ptr<MarmotType>&& element_type) ->MarmotResult::TypeResult
									{
										array_get.m_uses_indexable = true;
										array_get.m_type_data = ApplySubstitution(element_type);
										return array_get.m_type_data;
									}
								);
						}
					);
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::RangeBinary& range_binary)
{
	return Evaluate(range_binary.m_start)
		.and_then
		(
			[&range_binary, this](std::shared_ptr<MarmotType>&& start_type) -> MarmotResult::TypeResult
			{
				return Evaluate(range_binary.m_end)
					.and_then
					(
						[&start_type, &range_binary, this](std::shared_ptr<MarmotType>&& end_type) -> MarmotResult::TypeResult
						{
							return Unify(range_binary.m_range_op, start_type, end_type)
								.and_then
								(
									[&range_binary, &start_type, this](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
									{
										std::shared_ptr<MarmotType> resolved_type = ApplySubstitution(start_type);

										if (RigidParameterName(resolved_type).has_value())
										{
											if (std::optional<CompilerError> error = RequireOperandClass(range_binary.m_range_op, NUMERIC_CLASS_NAME, resolved_type))
											{
												return std::unexpected(std::move(*error));
											}
										}
										else if (!resolved_type->IsNumericType() && !resolved_type->IsType<MarmotType::TypeVariable>())
										{
											return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Range expression type error: expected numeric type", range_binary.m_range_op, m_file_name, m_source_lines, resolved_type, MarmotType::MakeLiteralType<MarmotType::IntegerType>(), MarmotType::MakeLiteralType<MarmotType::FloatType>()));
										}

										range_binary.m_type_data = MarmotType::MakeRangeType(start_type);
										return range_binary.m_type_data;
									}
								);
						}
					);
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::RangeTernary& range_ternary)
{
	return Evaluate(range_ternary.m_start)
		.and_then
		(
			[&range_ternary, this](std::shared_ptr<MarmotType>&& start_type) -> MarmotResult::TypeResult
			{
				return Evaluate(range_ternary.m_step)
					.and_then
					(
						[&start_type, &range_ternary, this](std::shared_ptr<MarmotType>&& step_type) -> MarmotResult::TypeResult
						{
							return Unify(range_ternary.m_first_range_op, start_type, step_type)
								.and_then
								(
									[&start_type, &range_ternary, this](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
									{
										return Evaluate(range_ternary.m_end)
											.and_then
											(
												[&start_type, &range_ternary, this](std::shared_ptr<MarmotType>&& end_type) -> MarmotResult::TypeResult
												{
													return Unify(range_ternary.m_second_range_op, start_type, end_type)
														.and_then
														(
															[&range_ternary, &start_type, this](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
															{
																std::shared_ptr<MarmotType> resolved_type = ApplySubstitution(start_type);

																if (RigidParameterName(resolved_type).has_value())
																{
																	if (std::optional<CompilerError> error = RequireOperandClass(range_ternary.m_first_range_op, NUMERIC_CLASS_NAME, resolved_type))
																	{
																		return std::unexpected(std::move(*error));
																	}
																}
																else if (!resolved_type->IsNumericType() && !resolved_type->IsType<MarmotType::TypeVariable>())
																{
																	return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Range expression type error: expected numeric type", range_ternary.m_first_range_op, m_file_name, m_source_lines, resolved_type, MarmotType::MakeLiteralType<MarmotType::IntegerType>(), MarmotType::MakeLiteralType<MarmotType::FloatType>()));
																}

																range_ternary.m_type_data = MarmotType::MakeRangeType(start_type);
																return range_ternary.m_type_data;
															}
														);
												}
											);
									}
								);
						}
					);
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::IfElse& if_else)
{
	// The type demanded of the if is the type of its branches, not of its
	// condition: `C(x) == C(1)` as a condition was checked against Option<C>.
	const auto evaluate_condition = [&if_else, this]() -> MarmotResult::TypeResult
	{
		ExpectedTypeGuard guard(*this, nullptr);
		return Evaluate(if_else.m_condition);
	};

	return evaluate_condition()
		.and_then
		(
			[&if_else, this](std::shared_ptr<MarmotType>&& actual_type) ->MarmotResult::TypeResult
			{
				std::shared_ptr<MarmotType> bool_type = MarmotType::MakeLiteralType<MarmotType::BoolType>();
				return Unify(if_else.m_if_token, bool_type, actual_type, UnifyDiagnosticMode::ExpectedActual)
					.and_then
					(
						[&if_else, this](std::shared_ptr<MarmotType>&& type)->MarmotResult::TypeResult
						{
							if (!type->IsType<MarmotType::BoolType>())
							{
								return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("IfElse expression type error: condition must be boolean", if_else.m_if_token, m_file_name, m_source_lines, type, MarmotType::MakeLiteralType<MarmotType::BoolType>()));
							}

							auto evaluate_branch_with_expected = [this](const std::unique_ptr<MarmotExpression>& branch) -> MarmotResult::TypeResult
							{
								if (m_expected_expr_type != nullptr)
								{
									ExpectedTypeGuard guard(*this, m_expected_expr_type);
									return Evaluate(branch);
								}

								return Evaluate(branch);
							};

							return evaluate_branch_with_expected(if_else.m_true_branch)
								.and_then
								(
									[&if_else, &evaluate_branch_with_expected, this](std::shared_ptr<MarmotType>&& true_branch_type) ->MarmotResult::TypeResult
									{
										// With nothing demanded of the whole if, the else branch is
										// still known to need the then branch's type, which lets
										// `if c then Some(x) else None()` decide None's type.
										const auto evaluate_else = [&if_else, &true_branch_type, &evaluate_branch_with_expected, this]() -> MarmotResult::TypeResult
										{
											if (m_expected_expr_type == nullptr)
											{
												ExpectedTypeGuard guard(*this, ApplySubstitution(true_branch_type));
												return Evaluate(if_else.m_else_branch);
											}
											return evaluate_branch_with_expected(if_else.m_else_branch);
										};
										return evaluate_else()
											.and_then
											(
												[&true_branch_type, &if_else, this](std::shared_ptr<MarmotType>&& else_branch_type)->MarmotResult::TypeResult
												{
													return Unify(if_else.m_else_token, true_branch_type, else_branch_type)
														.and_then
														(
															[&if_else](std::shared_ptr<MarmotType>&& type)->MarmotResult::TypeResult
															{
																if_else.m_type_data = type;
																return if_else.m_type_data;
															}
														);
												}
											);
									}
								);
						}
					);
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(MarmotExpression::Block& block)
{
	return ScopeSession(*this).Then([&]() -> MarmotResult::TypeResult
	{
		for (const std::unique_ptr<MarmotStatement>& stmt : block.m_stmts)
		{
			ExpectedTypeGuard statement_guard(*this, std::shared_ptr<MarmotType>{});
			MarmotResult::TypeResult result = Evaluate(stmt);
			if (!result.has_value())
			{
				return result;
			}
			// Note: Statement types are checked but do NOT contribute to the block's type
			// Only the final expression (if present) determines the block type
		}

		if (block.m_final_expr.has_value())
		{
			return Evaluate(*block.m_final_expr)
				.and_then
				(
					[&block, this](std::shared_ptr<MarmotType>&& final_value) -> MarmotResult::TypeResult
					{
						return Unify(block.m_right_brace, block.m_type_data, final_value, UnifyDiagnosticMode::ExpectedActual);
					}
				);
		}

		// Blocks without final expressions have Unit type
		if (block.m_type_data->IsType<MarmotType::UndecidedType>())
		{
			block.m_type_data = MarmotType::MakeLiteralType<MarmotType::UnitType>();
		}
		return block.m_type_data;
	});
}


