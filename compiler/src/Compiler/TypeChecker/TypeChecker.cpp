#include <algorithm>
#include <algorithm>
#include <format>
#include <iterator>
#include <ranges>
#include <unordered_set>
#include <type_traits>

#include "Common/Constant/Constant.h"
#include "Common/Error/Error.h"
#include "TypeChecker.h"

using namespace std::string_literals;

namespace
{
	template <typename>
	inline constexpr bool AlwaysFalse = false;

	const Token& GetPatternToken(const MidoriPattern& pattern)
	{
		return std::visit
		(
			[]<typename T>(const T& node) -> const Token&
			{
				using Node = std::decay_t<T>;
				if constexpr (std::is_same_v<Node, MidoriPattern::Binding>)
				{
					return node.m_name;
				}
				else if constexpr (std::is_same_v<Node, MidoriPattern::Wildcard>)
				{
					return node.m_token;
				}
				else if constexpr (std::is_same_v<Node, MidoriPattern::Literal>)
				{
					return node.m_token;
				}
				else if constexpr (std::is_same_v<Node, MidoriPattern::Tuple>)
				{
					return node.m_left_paren;
				}
				else if constexpr (std::is_same_v<Node, MidoriPattern::Array>)
				{
					return node.m_left_bracket;
				}
				else if constexpr (std::is_same_v<Node, MidoriPattern::Constructor>)
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

	bool HasTypeVariables(const std::shared_ptr<MidoriType>& type, std::unordered_set<const MidoriType*>& visited)
	{
		if (visited.contains(type.get()))
		{
			return false;
		}
		visited.insert(type.get());

		if (type->IsType<MidoriType::TypeVariable>())
		{
			return true;
		}
		else if (type->IsType<MidoriType::ArrayType>())
		{
			return HasTypeVariables(type->GetType<MidoriType::ArrayType>().m_element_type, visited);
		}
		else if (type->IsType<MidoriType::RangeType>())
		{
			return HasTypeVariables(type->GetType<MidoriType::RangeType>().m_element_type, visited);
		}
		else if (type->IsType<MidoriType::WorkerType>())
		{
			return HasTypeVariables(type->GetType<MidoriType::WorkerType>().m_result_type, visited);
		}
		else if (type->IsType<MidoriType::ChannelType>())
		{
			return HasTypeVariables(type->GetType<MidoriType::ChannelType>().m_element_type, visited);
		}
		else if (type->IsType<MidoriType::CellType>())
		{
			return HasTypeVariables(type->GetType<MidoriType::CellType>().m_element_type, visited);
		}
		else if (type->IsType<MidoriType::FunctionType>())
		{
			MidoriType::FunctionType& func = type->GetType<MidoriType::FunctionType>();
			for (const std::shared_ptr<MidoriType>& param : func.m_param_types)
			{
				if (HasTypeVariables(param, visited)) return true;
			}
			if (HasTypeVariables(func.m_return_type, visited))
			{
				return true;
			}
			for (const MidoriType::ClassConstraint& constraint : func.m_constraints)
			{
				for (const std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
				{
					if (HasTypeVariables(type_arg, visited))
					{
						return true;
					}
				}
			}
			return false;
		}
		else if (type->IsType<MidoriType::StructType>())
		{
			for (const std::shared_ptr<MidoriType>& member : type->GetType<MidoriType::StructType>().m_member_types)
			{
				if (HasTypeVariables(member, visited)) return true;
			}
			for (const std::shared_ptr<MidoriType>& type_argument : type->GetType<MidoriType::StructType>().m_type_arguments)
			{
				if (HasTypeVariables(type_argument, visited))
				{
					return true;
				}
			}
			for (const MidoriType::ClassConstraint& constraint : type->GetType<MidoriType::StructType>().m_constraints)
			{
				for (const std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
				{
					if (HasTypeVariables(type_arg, visited))
					{
						return true;
					}
				}
			}
		}
		else if (type->IsType<MidoriType::UnionType>())
		{
			for (const auto& [name, ctx] : type->GetType<MidoriType::UnionType>().m_member_info)
			{
				for (const std::shared_ptr<MidoriType>& member : ctx.m_member_types)
				{
					if (HasTypeVariables(member, visited)) return true;
				}
			}
			for (const std::shared_ptr<MidoriType>& type_argument : type->GetType<MidoriType::UnionType>().m_type_arguments)
			{
				if (HasTypeVariables(type_argument, visited))
				{
					return true;
				}
			}
			for (const MidoriType::ClassConstraint& constraint : type->GetType<MidoriType::UnionType>().m_constraints)
			{
				for (const std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
				{
					if (HasTypeVariables(type_arg, visited))
					{
						return true;
					}
				}
			}
		}
		else if (type->IsType<MidoriType::TupleType>())
		{
			for (const std::shared_ptr<MidoriType>& elem : type->GetType<MidoriType::TupleType>().m_element_types)
			{
				if (HasTypeVariables(elem, visited)) return true;
			}
		}
		return false;
	}

	bool HasTypeVariables(const std::shared_ptr<MidoriType>& type)
	{
		std::unordered_set<const MidoriType*> visited;
		return HasTypeVariables(type, visited);
	}

	void CollectTypeVariableIds(const std::shared_ptr<MidoriType>& type, std::unordered_set<int>& type_variable_ids, std::unordered_set<const MidoriType*>& visited)
	{
		if (type == nullptr || !visited.insert(type.get()).second)
		{
			return;
		}

		if (type->IsType<MidoriType::TypeVariable>())
		{
			type_variable_ids.insert(type->GetType<MidoriType::TypeVariable>().m_id);
			return;
		}
		if (type->IsType<MidoriType::ArrayType>())
		{
			CollectTypeVariableIds(type->GetType<MidoriType::ArrayType>().m_element_type, type_variable_ids, visited);
			return;
		}
		if (type->IsType<MidoriType::RangeType>())
		{
			CollectTypeVariableIds(type->GetType<MidoriType::RangeType>().m_element_type, type_variable_ids, visited);
			return;
		}
		if (type->IsType<MidoriType::WorkerType>())
		{
			CollectTypeVariableIds(type->GetType<MidoriType::WorkerType>().m_result_type, type_variable_ids, visited);
			return;
		}
		if (type->IsType<MidoriType::ChannelType>())
		{
			CollectTypeVariableIds(type->GetType<MidoriType::ChannelType>().m_element_type, type_variable_ids, visited);
			return;
		}

		if (type->IsType<MidoriType::CellType>())
		{
			CollectTypeVariableIds(type->GetType<MidoriType::CellType>().m_element_type, type_variable_ids, visited);
			return;
		}
		if (type->IsType<MidoriType::TupleType>())
		{
			for (const std::shared_ptr<MidoriType>& element_type : type->GetType<MidoriType::TupleType>().m_element_types)
			{
				CollectTypeVariableIds(element_type, type_variable_ids, visited);
			}
			return;
		}
		if (type->IsType<MidoriType::FunctionType>())
		{
			const MidoriType::FunctionType& function_type = type->GetType<MidoriType::FunctionType>();
			for (const std::shared_ptr<MidoriType>& param_type : function_type.m_param_types)
			{
				CollectTypeVariableIds(param_type, type_variable_ids, visited);
			}
			CollectTypeVariableIds(function_type.m_return_type, type_variable_ids, visited);
			for (const MidoriType::ClassConstraint& constraint : function_type.m_constraints)
			{
				for (const std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
				{
					CollectTypeVariableIds(type_arg, type_variable_ids, visited);
				}
			}
			return;
		}
		if (type->IsType<MidoriType::StructType>())
		{
			const MidoriType::StructType& struct_type = type->GetType<MidoriType::StructType>();
			for (const std::shared_ptr<MidoriType>& member_type : struct_type.m_member_types)
			{
				CollectTypeVariableIds(member_type, type_variable_ids, visited);
			}
			for (const MidoriType::ClassConstraint& constraint : struct_type.m_constraints)
			{
				for (const std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
				{
					CollectTypeVariableIds(type_arg, type_variable_ids, visited);
				}
			}
			return;
		}
		if (type->IsType<MidoriType::UnionType>())
		{
			const MidoriType::UnionType& union_type = type->GetType<MidoriType::UnionType>();
			for (const auto& [_, member_ctx] : union_type.m_member_info)
			{
				for (const std::shared_ptr<MidoriType>& member_type : member_ctx.m_member_types)
				{
					CollectTypeVariableIds(member_type, type_variable_ids, visited);
				}
			}
			for (const MidoriType::ClassConstraint& constraint : union_type.m_constraints)
			{
				for (const std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
				{
					CollectTypeVariableIds(type_arg, type_variable_ids, visited);
				}
			}
			return;
		}
		if (type->IsType<MidoriType::AssociatedType>())
		{
			for (const std::shared_ptr<MidoriType>& type_arg : type->GetType<MidoriType::AssociatedType>().m_type_args)
			{
				CollectTypeVariableIds(type_arg, type_variable_ids, visited);
			}
			return;
		}
		if (type->IsType<MidoriType::ClassConstraint>())
		{
			for (const std::shared_ptr<MidoriType>& type_arg : type->GetType<MidoriType::ClassConstraint>().m_type_args)
			{
				CollectTypeVariableIds(type_arg, type_variable_ids, visited);
			}
		}
	}

	std::unordered_set<int> CollectTypeVariableIds(const std::shared_ptr<MidoriType>& type)
	{
		std::unordered_set<int> type_variable_ids;
		std::unordered_set<const MidoriType*> visited;
		CollectTypeVariableIds(type, type_variable_ids, visited);
		return type_variable_ids;
	}

	std::string FormatTypeVariableIds(const std::unordered_set<int>& type_variable_ids)
	{
		std::vector<int> sorted_ids(type_variable_ids.cbegin(), type_variable_ids.cend());
		std::ranges::sort(sorted_ids);

		std::string result = "{";
		for (size_t idx = 0; idx < sorted_ids.size(); idx += 1u)
		{
			if (idx != 0u)
			{
				result += ", ";
			}
			result += std::to_string(sorted_ids[idx]);
		}
		result += "}";
		return result;
	}

	bool ContainsAssociatedTypes(const std::shared_ptr<MidoriType>& type, std::unordered_set<const MidoriType*>& visited)
	{
		if (visited.contains(type.get()))
		{
			return false;
		}
		visited.insert(type.get());

		if (type->IsType<MidoriType::AssociatedType>())
		{
			return true;
		}
		if (type->IsType<MidoriType::ArrayType>())
		{
			return ContainsAssociatedTypes(type->GetType<MidoriType::ArrayType>().m_element_type, visited);
		}
		if (type->IsType<MidoriType::RangeType>())
		{
			return ContainsAssociatedTypes(type->GetType<MidoriType::RangeType>().m_element_type, visited);
		}
		if (type->IsType<MidoriType::WorkerType>())
		{
			return ContainsAssociatedTypes(type->GetType<MidoriType::WorkerType>().m_result_type, visited);
		}
		if (type->IsType<MidoriType::ChannelType>())
		{
			return ContainsAssociatedTypes(type->GetType<MidoriType::ChannelType>().m_element_type, visited);
		}

		if (type->IsType<MidoriType::CellType>())
		{
			return ContainsAssociatedTypes(type->GetType<MidoriType::CellType>().m_element_type, visited);
		}
		if (type->IsType<MidoriType::FunctionType>())
		{
			const MidoriType::FunctionType& func = type->GetType<MidoriType::FunctionType>();
			for (const std::shared_ptr<MidoriType>& param : func.m_param_types)
			{
				if (ContainsAssociatedTypes(param, visited))
				{
					return true;
				}
			}
			if (ContainsAssociatedTypes(func.m_return_type, visited))
			{
				return true;
			}
			for (const MidoriType::ClassConstraint& constraint : func.m_constraints)
			{
				for (const std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
				{
					if (ContainsAssociatedTypes(type_arg, visited))
					{
						return true;
					}
				}
			}
			return false;
		}
		if (type->IsType<MidoriType::StructType>())
		{
			for (const std::shared_ptr<MidoriType>& member : type->GetType<MidoriType::StructType>().m_member_types)
			{
				if (ContainsAssociatedTypes(member, visited))
				{
					return true;
				}
			}
			for (const MidoriType::ClassConstraint& constraint : type->GetType<MidoriType::StructType>().m_constraints)
			{
				for (const std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
				{
					if (ContainsAssociatedTypes(type_arg, visited))
					{
						return true;
					}
				}
			}
		}
		if (type->IsType<MidoriType::UnionType>())
		{
			for (const auto& [_, ctx] : type->GetType<MidoriType::UnionType>().m_member_info)
			{
				for (const std::shared_ptr<MidoriType>& member : ctx.m_member_types)
				{
					if (ContainsAssociatedTypes(member, visited))
					{
						return true;
					}
				}
			}
			for (const MidoriType::ClassConstraint& constraint : type->GetType<MidoriType::UnionType>().m_constraints)
			{
				for (const std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
				{
					if (ContainsAssociatedTypes(type_arg, visited))
					{
						return true;
					}
				}
			}
		}
		if (type->IsType<MidoriType::TupleType>())
		{
			for (const std::shared_ptr<MidoriType>& elem : type->GetType<MidoriType::TupleType>().m_element_types)
			{
				if (ContainsAssociatedTypes(elem, visited))
				{
					return true;
				}
			}
		}
		return false;
	}

	bool ContainsAssociatedTypes(const std::shared_ptr<MidoriType>& type)
	{
		std::unordered_set<const MidoriType*> visited;
		return ContainsAssociatedTypes(type, visited);
	}

	// A class constraint names its class directly; an equality constraint names it
	// through the projection on its left side. Both must refer to a declared class,
	// so validation sites ask for the name rather than reading m_class_name, which
	// is empty for an equality.
	const std::string& ConstrainedClassName(const MidoriType::ClassConstraint& constraint)
	{
		if (constraint.IsEquality() && constraint.m_equality_lhs->IsType<MidoriType::AssociatedType>())
		{
			return constraint.m_equality_lhs->GetType<MidoriType::AssociatedType>().m_class_name;
		}

		return constraint.m_class_name;
	}

	bool ContainsConstraint(const std::vector<MidoriType::ClassConstraint>& constraints, const MidoriType::ClassConstraint& constraint)
	{
		return std::ranges::any_of
		(
			constraints,
			[&constraint](const MidoriType::ClassConstraint& existing)
			{
				return existing == constraint;
			}
		);
	}

	void AppendUniqueConstraint(std::vector<MidoriType::ClassConstraint>& constraints, MidoriType::ClassConstraint&& constraint)
	{
		if (!ContainsConstraint(constraints, constraint))
		{
			constraints.push_back(std::move(constraint));
		}
	}

	void CollectTypeConstraints(
		const std::shared_ptr<MidoriType>& type,
		std::vector<MidoriType::ClassConstraint>& constraints,
		std::unordered_set<const MidoriType*>& visited
	)
	{
		if (type == nullptr || !visited.insert(type.get()).second)
		{
			return;
		}

		if (type->IsType<MidoriType::ArrayType>())
		{
			CollectTypeConstraints(type->GetType<MidoriType::ArrayType>().m_element_type, constraints, visited);
			return;
		}

		if (type->IsType<MidoriType::CellType>())
		{
			CollectTypeConstraints(type->GetType<MidoriType::CellType>().m_element_type, constraints, visited);
			return;
		}

		if (type->IsType<MidoriType::RangeType>())
		{
			CollectTypeConstraints(type->GetType<MidoriType::RangeType>().m_element_type, constraints, visited);
			return;
		}

		if (type->IsType<MidoriType::TupleType>())
		{
			for (const std::shared_ptr<MidoriType>& element_type : type->GetType<MidoriType::TupleType>().m_element_types)
			{
				CollectTypeConstraints(element_type, constraints, visited);
			}
			return;
		}

		if (type->IsType<MidoriType::FunctionType>())
		{
			const MidoriType::FunctionType& function_type = type->GetType<MidoriType::FunctionType>();
			for (const std::shared_ptr<MidoriType>& param_type : function_type.m_param_types)
			{
				CollectTypeConstraints(param_type, constraints, visited);
			}
			CollectTypeConstraints(function_type.m_return_type, constraints, visited);
			for (const MidoriType::ClassConstraint& constraint : function_type.m_constraints)
			{
				AppendUniqueConstraint(constraints, MidoriType::ClassConstraint(constraint));
			}
			return;
		}

		if (type->IsType<MidoriType::StructType>())
		{
			const MidoriType::StructType& struct_type = type->GetType<MidoriType::StructType>();
			for (const MidoriType::ClassConstraint& constraint : struct_type.m_constraints)
			{
				AppendUniqueConstraint(constraints, MidoriType::ClassConstraint(constraint));
			}
			for (const std::shared_ptr<MidoriType>& member_type : struct_type.m_member_types)
			{
				CollectTypeConstraints(member_type, constraints, visited);
			}
			return;
		}

		if (type->IsType<MidoriType::UnionType>())
		{
			const MidoriType::UnionType& union_type = type->GetType<MidoriType::UnionType>();
			for (const MidoriType::ClassConstraint& constraint : union_type.m_constraints)
			{
				AppendUniqueConstraint(constraints, MidoriType::ClassConstraint(constraint));
			}
			for (const auto& [_, member_ctx] : union_type.m_member_info)
			{
				for (const std::shared_ptr<MidoriType>& member_type : member_ctx.m_member_types)
				{
					CollectTypeConstraints(member_type, constraints, visited);
				}
			}
		}
	}

	std::vector<MidoriType::ClassConstraint> CollectSignatureConstraints(
		const std::vector<std::shared_ptr<MidoriType>>& param_types,
		const std::shared_ptr<MidoriType>& return_type
	)
	{
		std::vector<MidoriType::ClassConstraint> constraints;
		std::unordered_set<const MidoriType*> visited;
		for (const std::shared_ptr<MidoriType>& param_type : param_types)
		{
			CollectTypeConstraints(param_type, constraints, visited);
		}
		CollectTypeConstraints(return_type, constraints, visited);
		return constraints;
	}

	std::string JoinSortedNames(std::vector<std::string> names)
	{
		std::ranges::sort(names);

		std::string joined;
		for (size_t i = 0u; i < names.size(); i += 1u)
		{
			if (i != 0u)
			{
				joined.append(", ");
			}
			joined.append(names[i]);
		}

		return joined;
	}

	bool MatchInstanceTypeArg(const std::shared_ptr<MidoriType>& pattern, const std::shared_ptr<MidoriType>& concrete, std::unordered_map<std::string, std::shared_ptr<MidoriType>>& substitutions, std::unordered_set<std::pair<MidoriType*, MidoriType*>, TypePairHash>& visited);

	bool MatchInstanceTypeArguments(const std::vector<std::string>& pattern_generic_params, const std::vector<std::shared_ptr<MidoriType>>& pattern_type_arguments, const std::vector<std::shared_ptr<MidoriType>>& concrete_type_arguments, std::unordered_map<std::string, std::shared_ptr<MidoriType>>& substitutions, std::unordered_set<std::pair<MidoriType*, MidoriType*>, TypePairHash>& visited)
	{
		const std::vector<std::shared_ptr<MidoriType>> resolved_pattern_type_arguments = MidoriType::InstantiateTypeArguments
		(
			pattern_generic_params,
			pattern_type_arguments,
			[](const std::shared_ptr<MidoriType>& type_argument) { return type_argument; }
		);

		// Derive no bindings rather than rejecting the match, so a concrete type that
		// predates its arguments still resolves through its members.
		if (resolved_pattern_type_arguments.size() != concrete_type_arguments.size())
		{
			return true;
		}

		for (size_t i = 0u; i < resolved_pattern_type_arguments.size(); i += 1u)
		{
			if (!MatchInstanceTypeArg(resolved_pattern_type_arguments[i], concrete_type_arguments[i], substitutions, visited))
			{
				return false;
			}
		}

		return true;
	}

	bool MatchInstanceTypeArg(const std::shared_ptr<MidoriType>& pattern, const std::shared_ptr<MidoriType>& concrete, std::unordered_map<std::string, std::shared_ptr<MidoriType>>& substitutions, std::unordered_set<std::pair<MidoriType*, MidoriType*>, TypePairHash>& visited)
	{
		if (!pattern || !concrete)
		{
			return false;
		}

		std::pair<MidoriType*, MidoriType*> key{ pattern.get(), concrete.get() };
		if (visited.contains(key))
		{
			return true;
		}
		visited.emplace(key);

		if (pattern->IsType<MidoriType::GenericParam>())
		{
			const std::string& param_name = pattern->GetType<MidoriType::GenericParam>().m_name;
			std::unordered_map<std::string, std::shared_ptr<MidoriType>>::iterator it = substitutions.find(param_name);
			if (it != substitutions.end())
			{
				return *it->second == *concrete;
			}
			substitutions.emplace(param_name, concrete);
			return true;
		}

		if (pattern->IsType<MidoriType::ArrayType>())
		{
			if (!concrete->IsType<MidoriType::ArrayType>())
			{
				return false;
			}
			return MatchInstanceTypeArg(pattern->GetType<MidoriType::ArrayType>().m_element_type, concrete->GetType<MidoriType::ArrayType>().m_element_type, substitutions, visited);
		}

		if (pattern->IsType<MidoriType::CellType>())
		{
			if (!concrete->IsType<MidoriType::CellType>())
			{
				return false;
			}
			return MatchInstanceTypeArg(pattern->GetType<MidoriType::CellType>().m_element_type, concrete->GetType<MidoriType::CellType>().m_element_type, substitutions, visited);
		}

		if (pattern->IsType<MidoriType::RangeType>())
		{
			if (!concrete->IsType<MidoriType::RangeType>())
			{
				return false;
			}
			return MatchInstanceTypeArg(pattern->GetType<MidoriType::RangeType>().m_element_type, concrete->GetType<MidoriType::RangeType>().m_element_type, substitutions, visited);
		}

		if (pattern->IsType<MidoriType::TupleType>())
		{
			if (!concrete->IsType<MidoriType::TupleType>())
			{
				return false;
			}

			const MidoriType::TupleType& pattern_tuple = pattern->GetType<MidoriType::TupleType>();
			const MidoriType::TupleType& concrete_tuple = concrete->GetType<MidoriType::TupleType>();
			if (pattern_tuple.m_element_types.size() != concrete_tuple.m_element_types.size())
			{
				return false;
			}
			for (size_t i = 0u; i < pattern_tuple.m_element_types.size(); i += 1u)
			{
				if (!MatchInstanceTypeArg(pattern_tuple.m_element_types[i], concrete_tuple.m_element_types[i], substitutions, visited))
				{
					return false;
				}
			}
			return true;
		}

		if (pattern->IsType<MidoriType::FunctionType>())
		{
			if (!concrete->IsType<MidoriType::FunctionType>())
			{
				return false;
			}

			const MidoriType::FunctionType& pattern_func = pattern->GetType<MidoriType::FunctionType>();
			const MidoriType::FunctionType& concrete_func = concrete->GetType<MidoriType::FunctionType>();
			if (pattern_func.m_param_types.size() != concrete_func.m_param_types.size())
			{
				return false;
			}
			for (size_t i = 0u; i < pattern_func.m_param_types.size(); i += 1u)
			{
				if (!MatchInstanceTypeArg(pattern_func.m_param_types[i], concrete_func.m_param_types[i], substitutions, visited))
				{
					return false;
				}
			}
			return MatchInstanceTypeArg(pattern_func.m_return_type, concrete_func.m_return_type, substitutions, visited);
		}

		if (pattern->IsType<MidoriType::StructType>())
		{
			if (!concrete->IsType<MidoriType::StructType>())
			{
				return false;
			}

			const MidoriType::StructType& pattern_struct = pattern->GetType<MidoriType::StructType>();
			const MidoriType::StructType& concrete_struct = concrete->GetType<MidoriType::StructType>();
			if (pattern_struct.m_name != concrete_struct.m_name ||
				pattern_struct.m_member_types.size() != concrete_struct.m_member_types.size())
			{
				return false;
			}

			if (!MatchInstanceTypeArguments(pattern_struct.m_generic_params, pattern_struct.m_type_arguments, concrete_struct.m_type_arguments, substitutions, visited))
			{
				return false;
			}

			for (size_t i = 0u; i < pattern_struct.m_member_types.size(); i += 1u)
			{
				if (!MatchInstanceTypeArg(pattern_struct.m_member_types[i], concrete_struct.m_member_types[i], substitutions, visited))
				{
					return false;
				}
			}
			return true;
		}

		if (pattern->IsType<MidoriType::UnionType>())
		{
			if (!concrete->IsType<MidoriType::UnionType>())
			{
				return false;
			}

			const MidoriType::UnionType& pattern_union = pattern->GetType<MidoriType::UnionType>();
			const MidoriType::UnionType& concrete_union = concrete->GetType<MidoriType::UnionType>();
			if (pattern_union.m_name != concrete_union.m_name ||
				pattern_union.m_member_info.size() != concrete_union.m_member_info.size())
			{
				return false;
			}

			if (!MatchInstanceTypeArguments(pattern_union.m_generic_params, pattern_union.m_type_arguments, concrete_union.m_type_arguments, substitutions, visited))
			{
				return false;
			}

			for (const auto& [member_name, pattern_ctx] : pattern_union.m_member_info)
			{
				std::unordered_map<std::string, MidoriType::UnionType::UnionMemberContext>::const_iterator concrete_it =
					concrete_union.m_member_info.find(member_name);
				if (concrete_it == concrete_union.m_member_info.end())
				{
					return false;
				}
				const MidoriType::UnionType::UnionMemberContext& concrete_ctx = concrete_it->second;
				if (pattern_ctx.m_member_types.size() != concrete_ctx.m_member_types.size())
				{
					return false;
				}
				for (size_t i = 0u; i < pattern_ctx.m_member_types.size(); i += 1u)
				{
					if (!MatchInstanceTypeArg(pattern_ctx.m_member_types[i], concrete_ctx.m_member_types[i], substitutions, visited))
					{
						return false;
					}
				}
			}
			return true;
		}

		return *pattern == *concrete;
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
	std::shared_ptr<MidoriType> m_saved_type;

public:
	ExpectedTypeGuard(TypeChecker& tc, std::shared_ptr<MidoriType> new_expected)
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

std::size_t TypePairHash::operator()(const std::pair<MidoriType*, MidoriType*>& pair) const noexcept
{
	std::size_t h1 = std::hash<MidoriType*>{}(pair.first);
	std::size_t h2 = std::hash<MidoriType*>{}(pair.second);
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

std::string TypeChecker::DescribeConstraint(const MidoriType::ClassConstraint& constraint) const
{
	if (constraint.IsEquality())
	{
		return constraint.m_equality_lhs->ToString() + " ~ "s + constraint.m_equality_rhs->ToString();
	}

	if (constraint.m_type_args.empty())
	{
		return constraint.m_class_name;
	}

	std::string rendered = constraint.m_class_name + "<"s;
	for (size_t idx = 0u; idx < constraint.m_type_args.size(); idx += 1u)
	{
		if (idx > 0u)
		{
			rendered += ", "s;
		}
		rendered += constraint.m_type_args[idx]->ToString();
	}
	rendered += ">"s;
	return rendered;
}

CompilerError TypeChecker::MakeConstraintFailureError(const Token& token, const MidoriType::ClassConstraint& constraint, std::optional<std::string_view> suggestion) const
{
	std::string message;
	if (!constraint.m_type_args.empty())
	{
		message = std::format
		(
			"Type {} does not satisfy constraint {} - no matching instance found",
			constraint.m_type_args[0u]->ToString(),
			DescribeConstraint(constraint)
		);
	}
	else
	{
		message = std::format("Constraint {} is not satisfied - no matching instance found", DescribeConstraint(constraint));
	}

	return MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnsatisfiedConstraint, message, token, m_file_name, m_source_lines, suggestion);
}

bool TypeChecker::HasActiveConstraint(const std::string& class_name, const std::shared_ptr<MidoriType>& type)
{
	std::shared_ptr<MidoriType> resolved_type = ApplySubstitution(type);

	for (const MidoriType::ClassConstraint& constraint : m_active_constraints)
	{
		if (constraint.m_class_name != class_name || constraint.m_type_args.size() != 1u)
		{
			continue;
		}

		std::shared_ptr<MidoriType> resolved_constraint_type = ApplySubstitution(constraint.m_type_args[0u]);
		if (*resolved_constraint_type == *resolved_type)
		{
			return true;
		}
	}

	return false;
}

std::optional<CompilerError> TypeChecker::EnsureTransferable(const Token& token, const std::shared_ptr<MidoriType>& type)
{
	std::unordered_set<const MidoriType*> visited;
	return EnsureTransferable(token, type, visited);
}

std::optional<CompilerError> TypeChecker::EnsureTransferable(const Token& token, const std::shared_ptr<MidoriType>& type, std::unordered_set<const MidoriType*>& visited)
{
	const std::shared_ptr<MidoriType> resolved_type = ApplySubstitution(type);
	if (!visited.insert(resolved_type.get()).second)
	{
		return std::nullopt;
	}

	const MidoriType::ClassConstraint transferable_constraint("Transferable", { resolved_type });

	const auto fail = [&](std::optional<std::string_view> suggestion = std::nullopt) -> std::optional<CompilerError>
	{
		return MakeConstraintFailureError(token, transferable_constraint, suggestion);
	};

	const auto has_transferable_marker = [&]() -> bool
	{
		return HasActiveConstraint("Transferable", resolved_type)
			|| FindMatchingInstance("Transferable", { resolved_type }).has_value();
	};

	if (
		resolved_type->IsType<MidoriType::IntegerType>() ||
		resolved_type->IsType<MidoriType::FloatType>() ||
		resolved_type->IsType<MidoriType::ByteType>() ||
		resolved_type->IsType<MidoriType::WordType>() ||
		resolved_type->IsType<MidoriType::BoolType>() ||
		resolved_type->IsType<MidoriType::TextType>() ||
		resolved_type->IsType<MidoriType::UnitType>() ||
		resolved_type->IsType<MidoriType::NeverType>()
	)
	{
		return std::nullopt;
	}

	if (resolved_type->IsType<MidoriType::ArrayType>())
	{
		return EnsureTransferable(token, resolved_type->GetType<MidoriType::ArrayType>().m_element_type, visited);
	}

	if (resolved_type->IsType<MidoriType::ChannelType>())
	{
		return EnsureTransferable(token, resolved_type->GetType<MidoriType::ChannelType>().m_element_type, visited);
	}

	// A cell reaching a worker is copied (the CellCrossesWorker warning says so),
	// so it is transferable exactly when what it holds is.
	if (resolved_type->IsType<MidoriType::CellType>())
	{
		return EnsureTransferable(token, resolved_type->GetType<MidoriType::CellType>().m_element_type, visited);
	}

	if (resolved_type->IsType<MidoriType::TupleType>())
	{
		for (const std::shared_ptr<MidoriType>& element_type : resolved_type->GetType<MidoriType::TupleType>().m_element_types)
		{
			if (std::optional<CompilerError> error = EnsureTransferable(token, element_type, visited))
			{
				return error;
			}
		}
		return std::nullopt;
	}

	if (resolved_type->IsType<MidoriType::WorkerType>())
	{
		return fail("Worker values cannot cross worker boundaries.");
	}

	if (resolved_type->IsType<MidoriType::RangeType>())
	{
		return fail("Range values are not transferable.");
	}

	if (resolved_type->IsType<MidoriType::FunctionType>())
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

	if (resolved_type->IsType<MidoriType::GenericParam>() ||
		resolved_type->IsType<MidoriType::TypeVariable>() ||
		resolved_type->IsType<MidoriType::AssociatedType>())
	{
		return has_transferable_marker()
			? std::nullopt
			: fail("Add a 'where Transferable<T>' constraint or import an instance.");
	}

	if (resolved_type->IsType<MidoriType::StructType>())
	{
		if (!has_transferable_marker())
		{
			return fail("Add 'deriving (Transferable)' or define an instance.");
		}

		for (const std::shared_ptr<MidoriType>& member_type : resolved_type->GetType<MidoriType::StructType>().m_member_types)
		{
			if (std::optional<CompilerError> error = EnsureTransferable(token, member_type, visited))
			{
				return error;
			}
		}
		return std::nullopt;
	}

	if (resolved_type->IsType<MidoriType::UnionType>())
	{
		if (!has_transferable_marker())
		{
			return fail("Add 'deriving (Transferable)' or define an instance.");
		}

		for (const auto& [_, member_ctx] : resolved_type->GetType<MidoriType::UnionType>().m_member_info)
		{
			for (const std::shared_ptr<MidoriType>& member_type : member_ctx.m_member_types)
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

	return MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, message, token, m_file_name, m_source_lines);
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

	return MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, message, token, m_file_name, m_source_lines);
}

std::optional<std::vector<std::pair<std::string, std::shared_ptr<MidoriType>>>> TypeChecker::ResolveGenericTypeArguments(const std::shared_ptr<MidoriType>& prototype, const std::shared_ptr<MidoriType>& concrete_type) const
{
	std::vector<std::string> param_names;
	if (prototype->IsType<MidoriType::StructType>())
	{
		param_names = prototype->GetType<MidoriType::StructType>().m_generic_params;
	}
	else if (prototype->IsType<MidoriType::UnionType>())
	{
		param_names = prototype->GetType<MidoriType::UnionType>().m_generic_params;
	}
	else
	{
		return std::nullopt;
	}

	if (param_names.empty())
	{
		return std::nullopt;
	}

	std::unordered_map<std::string, std::shared_ptr<MidoriType>> substitutions;
	std::unordered_set<std::pair<MidoriType*, MidoriType*>, TypePairHash> visited;
	if (!MatchInstanceTypeArg(prototype, concrete_type, substitutions, visited))
	{
		return std::nullopt;
	}

	std::vector<std::pair<std::string, std::shared_ptr<MidoriType>>> resolved_args;
	resolved_args.reserve(param_names.size());
	for (const std::string& param_name : param_names)
	{
		std::unordered_map<std::string, std::shared_ptr<MidoriType>>::const_iterator it = substitutions.find(param_name);
		if (it == substitutions.cend())
		{
			return std::nullopt;
		}

		resolved_args.emplace_back(param_name, it->second);
	}

	return resolved_args;
}

std::optional<TypeChecker::ResolvedInstanceMatch> TypeChecker::FindMatchingInstance(const std::string& class_name, const std::vector<std::shared_ptr<MidoriType>>& type_args) const
{
	std::optional<ResolvedInstanceMatch> resolved_match;

	for (const auto& [_, instance_info] : m_instances)
	{
		if (instance_info.m_class_name != class_name || instance_info.m_type_args.size() != type_args.size())
		{
			continue;
		}

		TypeEnvironment substitutions;
		std::unordered_set<std::pair<MidoriType*, MidoriType*>, TypePairHash> visited;
		bool matched = true;
		for (size_t idx = 0u; idx < type_args.size(); idx += 1u)
		{
			if (!MatchInstanceTypeArg(instance_info.m_type_args[idx], type_args[idx], substitutions, visited))
			{
				matched = false;
				break;
			}
		}

		if (!matched)
		{
			continue;
		}

		if (resolved_match.has_value())
		{
			return std::nullopt;
		}

		resolved_match = ResolvedInstanceMatch{ .m_instance = &instance_info, .m_substitutions = std::move(substitutions) };
	}

	return resolved_match;
}

std::optional<CompilerError> TypeChecker::DischargeEqualityConstraint(const Token& token, const MidoriType::ClassConstraint& constraint, const std::shared_ptr<MidoriType>& resolved_lhs, const std::shared_ptr<MidoriType>& resolved_rhs)
{
	if (*resolved_lhs == *resolved_rhs)
	{
		return std::nullopt;
	}

	// An equality constraint also *solves*: `Iterable::Item<S> ~ A` with `A` still
	// unbound is what tells the call site what A is. `Iter::ToArray` has no other
	// argument mentioning A, so without this it could never be called.
	if (resolved_lhs->IsType<MidoriType::TypeVariable>() || resolved_rhs->IsType<MidoriType::TypeVariable>())
	{
		std::shared_ptr<MidoriType> lhs = resolved_lhs;
		std::shared_ptr<MidoriType> rhs = resolved_rhs;
		if (Unify(token, lhs, rhs).has_value())
		{
			return std::nullopt;
		}
	}

	return MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnsatisfiedConstraint, std::format("Equality constraint is not satisfied: '{}' resolves to '{}', not '{}'", constraint.m_equality_lhs->ToString(), resolved_lhs->ToString(), resolved_rhs->ToString()), token, m_file_name, m_source_lines);
}

bool TypeChecker::IsSatisfiedByActiveConstraint(const MidoriType::ClassConstraint& resolved_constraint)
{
	return std::ranges::any_of
	(
		m_active_constraints,
		[this, &resolved_constraint](const MidoriType::ClassConstraint& active_constraint) -> bool
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

MidoriResult::TypeResult TypeChecker::ValidateFunctionConstraints(const Token& token, const MidoriType::FunctionType& function_type)
{
	for (const MidoriType::ClassConstraint& constraint : function_type.m_constraints)
	{
		// An equality constraint is discharged by reduction, not by instance
		// selection: resolve both sides at the call site and compare them.
		if (constraint.IsEquality())
		{
			std::shared_ptr<MidoriType> resolved_equality_lhs = ApplySubstitution(constraint.m_equality_lhs);
			std::shared_ptr<MidoriType> resolved_equality_rhs = ApplySubstitution(constraint.m_equality_rhs);
			if (std::optional<CompilerError> error = DischargeEqualityConstraint(token, constraint, resolved_equality_lhs, resolved_equality_rhs))
			{
				return std::unexpected(std::move(*error));
			}

			continue;
		}
		std::vector<std::shared_ptr<MidoriType>> resolved_type_args;
		resolved_type_args.reserve(constraint.m_type_args.size());
		for (const std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
		{
			resolved_type_args.emplace_back(ApplySubstitution(type_arg));
		}

		const MidoriType::ClassConstraint resolved_constraint(constraint.m_class_name, std::move(resolved_type_args));
		if (IsSatisfiedByActiveConstraint(resolved_constraint))
		{
			continue;
		}

		if (FindMatchingInstance(resolved_constraint.m_class_name, resolved_constraint.m_type_args).has_value())
		{
			continue;
		}

		return std::unexpected(MakeConstraintFailureError(token, resolved_constraint));
	}

	return MidoriType::MakeUndecidedType();
}

MidoriResult::TypeResult TypeChecker::ValidateInstanceConstraints(const Token& token, const InstanceInfo& instance_info, const TypeEnvironment& substitutions, size_t depth)
{
	if (depth >= s_max_instance_constraint_depth)
	{
		return MidoriType::MakeUndecidedType();
	}

	for (const MidoriType::ClassConstraint& constraint : instance_info.m_constraints)
	{
		// An equality constraint is discharged by reduction, not by instance
		// selection: resolve both sides at the call site and compare them.
		if (constraint.IsEquality())
		{
			std::shared_ptr<MidoriType> resolved_equality_lhs = ApplySubstitution(MidoriType::SubstituteTypeParams(constraint.m_equality_lhs, substitutions));
			std::shared_ptr<MidoriType> resolved_equality_rhs = ApplySubstitution(MidoriType::SubstituteTypeParams(constraint.m_equality_rhs, substitutions));
			if (std::optional<CompilerError> error = DischargeEqualityConstraint(token, constraint, resolved_equality_lhs, resolved_equality_rhs))
			{
				return std::unexpected(std::move(*error));
			}

			continue;
		}
		std::vector<std::shared_ptr<MidoriType>> resolved_type_args;
		resolved_type_args.reserve(constraint.m_type_args.size());
		for (const std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
		{
			resolved_type_args.emplace_back(ApplySubstitution(MidoriType::SubstituteTypeParams(type_arg, substitutions)));
		}

		const MidoriType::ClassConstraint resolved_constraint(constraint.m_class_name, std::move(resolved_type_args));
		if (IsSatisfiedByActiveConstraint(resolved_constraint))
		{
			continue;
		}

		std::optional<ResolvedInstanceMatch> resolved_match = FindMatchingInstance(resolved_constraint.m_class_name, resolved_constraint.m_type_args);
		if (!resolved_match.has_value())
		{
			return std::unexpected(MakeConstraintFailureError(token, resolved_constraint));
		}

		MidoriResult::TypeResult nested_result = ValidateInstanceConstraints(token, *resolved_match->m_instance, resolved_match->m_substitutions, depth + 1u);
		if (!nested_result.has_value())
		{
			return nested_result;
		}
	}

	return MidoriType::MakeUndecidedType();
}

std::shared_ptr<MidoriType> TypeChecker::ResolveAssociatedType(const MidoriType::AssociatedType& associated_type)
{
	std::vector<std::shared_ptr<MidoriType>> resolved_type_args;
	resolved_type_args.reserve(associated_type.m_type_args.size());
	for (const std::shared_ptr<MidoriType>& type_arg : associated_type.m_type_args)
	{
		resolved_type_args.emplace_back(ApplySubstitution(type_arg));
	}

	std::unordered_map<std::string, ClassInfo>::const_iterator class_it = m_classes.find(associated_type.m_class_name);
	if (class_it == m_classes.cend() || !class_it->second.m_associated_types.contains(associated_type.m_name))
	{
		std::shared_ptr<MidoriType> unresolved_projection = MidoriType::MakeAssociatedType(associated_type.m_class_name, associated_type.m_name, std::move(resolved_type_args));
		std::shared_ptr<MidoriType> equated_projection = ReduceProjectionByEqualityConstraint(unresolved_projection);
		return equated_projection != nullptr ? equated_projection : unresolved_projection;
	}

	std::optional<ResolvedInstanceMatch> resolved_match = FindMatchingInstance(associated_type.m_class_name, resolved_type_args);
	if (!resolved_match.has_value())
	{
		std::shared_ptr<MidoriType> unresolved_projection = MidoriType::MakeAssociatedType(associated_type.m_class_name, associated_type.m_name, std::move(resolved_type_args));
		std::shared_ptr<MidoriType> equated_projection = ReduceProjectionByEqualityConstraint(unresolved_projection);
		return equated_projection != nullptr ? equated_projection : unresolved_projection;
	}

	AssociatedTypeEnvironment::const_iterator binding_it = resolved_match->m_instance->m_associated_type_bindings.find(associated_type.m_name);
	if (binding_it == resolved_match->m_instance->m_associated_type_bindings.cend())
	{
		std::shared_ptr<MidoriType> unresolved_projection = MidoriType::MakeAssociatedType(associated_type.m_class_name, associated_type.m_name, std::move(resolved_type_args));
		std::shared_ptr<MidoriType> equated_projection = ReduceProjectionByEqualityConstraint(unresolved_projection);
		return equated_projection != nullptr ? equated_projection : unresolved_projection;
	}

	std::shared_ptr<MidoriType> concrete_binding = MidoriType::SubstituteTypeParams(binding_it->second, resolved_match->m_substitutions);
	return ApplySubstitution(concrete_binding);
}

// An associated-type projection over an abstract type argument cannot be reduced
// by instance selection, because there is no instance to select yet. A where
// clause of the form 'Class::Assoc<S> ~ Type' states the answer directly, so a
// projection that matches one is replaced by the type it was equated to.
std::shared_ptr<MidoriType> TypeChecker::ReduceProjectionByEqualityConstraint(const std::shared_ptr<MidoriType>& type) const
{
	if (!type->IsType<MidoriType::AssociatedType>())
	{
		return nullptr;
	}

	std::vector<MidoriType::ClassConstraint>::const_iterator match = std::ranges::find_if
	(
		m_active_constraints,
		[&type](const MidoriType::ClassConstraint& constraint)
		{
			return constraint.IsEquality() && *constraint.m_equality_lhs == *type;
		}
	);

	return match != m_active_constraints.cend() ? match->m_equality_rhs : nullptr;
}

std::optional<CompilerError> TypeChecker::TryMakeGenericParameterMismatchError(const Token& token, const std::shared_ptr<MidoriType>& left, const std::shared_ptr<MidoriType>& right) const
{
	std::shared_ptr<MidoriType> prototype;
	std::string type_name;

	if (left->IsType<MidoriType::StructType>() && right->IsType<MidoriType::StructType>())
	{
		const MidoriType::StructType& left_struct = left->GetType<MidoriType::StructType>();
		const MidoriType::StructType& right_struct = right->GetType<MidoriType::StructType>();
		if (left_struct.m_name != right_struct.m_name)
		{
			return std::nullopt;
		}

		type_name = left_struct.m_name;
		TypeDefinitionMap::const_iterator it = m_struct_type_definitions.find(type_name);
		if (it == m_struct_type_definitions.cend())
		{
			return std::nullopt;
		}
		prototype = it->second;
	}
	else if (left->IsType<MidoriType::UnionType>() && right->IsType<MidoriType::UnionType>())
	{
		const MidoriType::UnionType& left_union = left->GetType<MidoriType::UnionType>();
		const MidoriType::UnionType& right_union = right->GetType<MidoriType::UnionType>();
		if (left_union.m_name != right_union.m_name)
		{
			return std::nullopt;
		}

		type_name = left_union.m_name;
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

	std::optional<std::vector<std::pair<std::string, std::shared_ptr<MidoriType>>>> left_args = ResolveGenericTypeArguments(prototype, left);
	std::optional<std::vector<std::pair<std::string, std::shared_ptr<MidoriType>>>> right_args = ResolveGenericTypeArguments(prototype, right);
	if (!left_args.has_value() || !right_args.has_value() || left_args->size() != right_args->size())
	{
		return std::nullopt;
	}

	std::vector<std::string> generic_param_names;
	generic_param_names.reserve(left_args->size());
	for (const auto& [param_name, _] : *left_args)
	{
		generic_param_names.emplace_back(param_name);
	}

	std::string type_signature = type_name;
	if (!generic_param_names.empty())
	{
		type_signature += "<"s;
		for (size_t idx = 0u; idx < generic_param_names.size(); idx += 1u)
		{
			if (idx > 0u)
			{
				type_signature += ", "s;
			}
			type_signature += generic_param_names[idx];
		}
		type_signature += ">"s;
	}

	for (size_t idx = 0u; idx < left_args->size(); idx += 1u)
	{
		const std::string& param_name = left_args->at(idx).first;
		const std::shared_ptr<MidoriType>& left_arg = left_args->at(idx).second;
		const std::shared_ptr<MidoriType>& right_arg = right_args->at(idx).second;
		if (*left_arg == *right_arg)
		{
			continue;
		}

		std::unordered_set<const MidoriType*> left_visited;
		std::unordered_set<const MidoriType*> right_visited;
		if (HasTypeVariables(left_arg, left_visited) || HasTypeVariables(right_arg, right_visited)
			|| left_arg->IsType<MidoriType::UndecidedType>() || right_arg->IsType<MidoriType::UndecidedType>())
		{
			continue;
		}

		const std::string message = std::format
		(
			"In type {}: parameter '{}' is {} in one context but {} in another",
			type_signature,
			param_name,
			left_arg->ToString(),
			right_arg->ToString()
		);
		return MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, message, token, m_file_name, m_source_lines);
	}

	return std::nullopt;
}

CompilerError TypeChecker::MakeUnificationError(const Token& token, const std::shared_ptr<MidoriType>& left, const std::shared_ptr<MidoriType>& right, UnifyDiagnosticMode diagnostic_mode) const
{
	if (std::optional<CompilerError> generic_mismatch = TryMakeGenericParameterMismatchError(token, left, right))
	{
		return std::move(*generic_mismatch);
	}

	auto build_expected_message = [](const std::shared_ptr<MidoriType>& expected, const std::shared_ptr<MidoriType>& actual)
	{
		return std::format("Expected type '{}' but got '{}'", expected->ToString(), actual->ToString());
	};

	if (diagnostic_mode == UnifyDiagnosticMode::ActualExpected)
	{
		return MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, build_expected_message(right, left), token, m_file_name, m_source_lines);
	}
	if (diagnostic_mode == UnifyDiagnosticMode::ExpectedActual)
	{
		return MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, build_expected_message(left, right), token, m_file_name, m_source_lines);
	}

	return MidoriError::GenerateTypeCheckerErrorWithContext
	(
		CompilerErrorCode::TypeMismatch,
		std::format("Type mismatch between '{}' and '{}'", left->ToString(), right->ToString()),
		token,
		m_file_name,
		m_source_lines
	);
}

MidoriResult::TypeResult TypeChecker::UnifyTypeArguments(const Token& token, std::vector<std::shared_ptr<MidoriType>>& left, std::vector<std::shared_ptr<MidoriType>>& right, UnifyDiagnosticMode diagnostic_mode)
{
	// Arity is equal for every same-named instantiation the parser admits, so this
	// only guards against a substitution site that failed to record its arguments.
	// Such a lapse is caught by the phantom type parameter tests rather than here.
	const size_t shared_count = std::min(left.size(), right.size());

	for (size_t idx : std::views::iota(0u, shared_count))
	{
		MidoriResult::TypeResult result = Unify(token, left[idx], right[idx], diagnostic_mode);
		if (!result.has_value())
		{
			return result;
		}
	}

	return MidoriType::MakeLiteralType<MidoriType::UnitType>();
}

MidoriResult::TypeResult TypeChecker::Unify(const Token& token, std::shared_ptr<MidoriType>& left, std::shared_ptr<MidoriType>& right, UnifyDiagnosticMode diagnostic_mode)
{
	// Apply current substitutions first
	std::shared_ptr<MidoriType> left_subst = ApplySubstitution(left);
	std::shared_ptr<MidoriType> right_subst = ApplySubstitution(right);

	// Recursion guard to prevent infinite loops when unifying recursive types
	std::pair<MidoriType*, MidoriType*> orig_pair{ left.get(), right.get() };
	std::pair<MidoriType*, MidoriType*> subst_pair{ left_subst.get(), right_subst.get() };
	if (m_unify_visited.contains(orig_pair) || m_unify_visited.contains(subst_pair))
	{
		return left_subst;
	}

	struct RecursionGuard
	{
		std::unordered_set<std::pair<MidoriType*, MidoriType*>, TypePairHash>& m_visited;
		std::vector<std::pair<MidoriType*, MidoriType*>> m_pairs;

		RecursionGuard(std::unordered_set<std::pair<MidoriType*, MidoriType*>, TypePairHash>& visited, std::pair<MidoriType*, MidoriType*> first, std::pair<MidoriType*, MidoriType*> second)
			: m_visited(visited)
		{
			m_pairs.push_back(first);
			if (second != first)
			{
				m_pairs.push_back(second);
			}

			for (const std::pair<MidoriType*, MidoriType*>& pair : m_pairs)
			{
				m_visited.emplace(pair);
			}
		}

		~RecursionGuard()
		{
			for (const std::pair<MidoriType*, MidoriType*>& pair : m_pairs)
			{
				m_visited.erase(pair);
			}
		}
	};

	RecursionGuard guard(m_unify_visited, orig_pair, subst_pair);

	bool is_complex_type = 
		left_subst->IsType<MidoriType::StructType>() || 
		left_subst->IsType<MidoriType::UnionType>() ||
		left_subst->IsType<MidoriType::ArrayType>() ||
		left_subst->IsType<MidoriType::RangeType>() ||
		left_subst->IsType<MidoriType::WorkerType>() ||
		left_subst->IsType<MidoriType::ChannelType>() ||
		left_subst->IsType<MidoriType::CellType>() ||
		left_subst->IsType<MidoriType::FunctionType>();

	if (!is_complex_type && *left_subst == *right_subst)
	{
		return left_subst;
	}
	// Never type unifies with any type (it's the bottom type)
	else if (left_subst->IsType<MidoriType::NeverType>())
	{
		return right_subst;
	}
	else if (right_subst->IsType<MidoriType::NeverType>())
	{
		return left_subst;
	}
	else if (left_subst->IsType<MidoriType::TypeVariable>())
	{
		int var_id = left_subst->GetType<MidoriType::TypeVariable>().m_id;
		if (OccursCheck(var_id, right_subst))
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Infinite type detected (occurs check failed)", token, m_file_name, m_source_lines, left_subst, right_subst));
		}
		m_type_substitution[var_id] = right_subst;
		*left = *right_subst;
		return left;
	}
	else if (right_subst->IsType<MidoriType::TypeVariable>())
	{
		int var_id = right_subst->GetType<MidoriType::TypeVariable>().m_id;
		if (OccursCheck(var_id, left_subst))
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Infinite type detected (occurs check failed)", token, m_file_name, m_source_lines, left_subst, right_subst));
		}
		m_type_substitution[var_id] = left_subst;
		*right = *left_subst;
		return left;
	}
	else if (left_subst->IsType<MidoriType::UndecidedType>() && !right_subst->IsType<MidoriType::UndecidedType>())
	{
		*left = *right_subst;
		return left;
	}
	else if (!left_subst->IsType<MidoriType::UndecidedType>() && right_subst->IsType<MidoriType::UndecidedType>())
	{
		*right = *left_subst;
		return left;
	}
	else if (left_subst->IsType<MidoriType::ArrayType>() && right_subst->IsType<MidoriType::ArrayType>())
	{
		MidoriResult::TypeResult result = Unify(token, left_subst->GetType<MidoriType::ArrayType>().m_element_type, right_subst->GetType<MidoriType::ArrayType>().m_element_type, diagnostic_mode);
		if (!result.has_value())
		{
			return result;
		}
		return left;
	}
	else if (left_subst->IsType<MidoriType::WorkerType>() && right_subst->IsType<MidoriType::WorkerType>())
	{
		MidoriResult::TypeResult result = Unify(token, left_subst->GetType<MidoriType::WorkerType>().m_result_type, right_subst->GetType<MidoriType::WorkerType>().m_result_type, diagnostic_mode);
		if (!result.has_value())
		{
			return result;
		}
		return left;
	}
	else if (left_subst->IsType<MidoriType::ChannelType>() && right_subst->IsType<MidoriType::ChannelType>())
	{
		MidoriResult::TypeResult result = Unify(token, left_subst->GetType<MidoriType::ChannelType>().m_element_type, right_subst->GetType<MidoriType::ChannelType>().m_element_type, diagnostic_mode);
		if (!result.has_value())
		{
			return result;
		}
		return left;
	}
	else if (left_subst->IsType<MidoriType::CellType>() && right_subst->IsType<MidoriType::CellType>())
	{
		MidoriResult::TypeResult result = Unify(token, left_subst->GetType<MidoriType::CellType>().m_element_type, right_subst->GetType<MidoriType::CellType>().m_element_type, diagnostic_mode);
		if (!result.has_value())
		{
			return result;
		}
		return left;
	}
	else if (left_subst->IsType<MidoriType::RangeType>() && right_subst->IsType<MidoriType::RangeType>())
	{
		MidoriResult::TypeResult result = Unify(token, left_subst->GetType<MidoriType::RangeType>().m_element_type, right_subst->GetType<MidoriType::RangeType>().m_element_type, diagnostic_mode);
		if (!result.has_value())
		{
			return result;
		}
		return left;
	}
	else if (left_subst->IsType<MidoriType::FunctionType>() && right_subst->IsType<MidoriType::FunctionType>())
	{
		MidoriType::FunctionType& left_func = left_subst->GetType<MidoriType::FunctionType>();
		MidoriType::FunctionType& right_func = right_subst->GetType<MidoriType::FunctionType>();

		// Function types must have the same number of parameters
		if (left_func.m_param_types.size() != right_func.m_param_types.size())
		{
			return std::unexpected(MakeFunctionArityError(token, left_func.m_param_types.size(), right_func.m_param_types.size(), diagnostic_mode));
		}

		MidoriResult::TypeResult result;

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
	else if (left_subst->IsType<MidoriType::StructType>() && right_subst->IsType<MidoriType::StructType>())
	{
		MidoriType::StructType& left_struct = left_subst->GetType<MidoriType::StructType>();
		MidoriType::StructType& right_struct = right_subst->GetType<MidoriType::StructType>();

		// Struct types must have the same name and same number of members
		if (left_struct.m_name != right_struct.m_name || left_struct.m_member_types.size() != right_struct.m_member_types.size())
		{
			return std::unexpected(MakeUnificationError(token, left_subst, right_subst, diagnostic_mode));
		}

		if (std::optional<CompilerError> generic_mismatch = TryMakeGenericParameterMismatchError(token, left_subst, right_subst))
		{
			return std::unexpected(std::move(*generic_mismatch));
		}

		MidoriResult::TypeResult type_argument_result = UnifyTypeArguments(token, left_struct.m_type_arguments, right_struct.m_type_arguments, diagnostic_mode);
		if (!type_argument_result.has_value())
		{
			return type_argument_result;
		}

		// Unify each member type
		for (size_t idx : std::views::iota(0u, left_struct.m_member_types.size()))
		{
			MidoriResult::TypeResult result = Unify(token, left_struct.m_member_types[idx], right_struct.m_member_types[idx], diagnostic_mode);
			if (!result.has_value())
			{
				return result;
			}
		}

		return left;
	}
	else if (left_subst->IsType<MidoriType::UnionType>() && right_subst->IsType<MidoriType::UnionType>())
	{
		MidoriType::UnionType& left_union = left_subst->GetType<MidoriType::UnionType>();
		MidoriType::UnionType& right_union = right_subst->GetType<MidoriType::UnionType>();

		// Union types must have the same name and same members
		if (left_union.m_name != right_union.m_name || left_union.m_member_info.size() != right_union.m_member_info.size())
		{
			return std::unexpected(MakeUnificationError(token, left_subst, right_subst, diagnostic_mode));
		}

		if (std::optional<CompilerError> generic_mismatch = TryMakeGenericParameterMismatchError(token, left_subst, right_subst))
		{
			return std::unexpected(std::move(*generic_mismatch));
		}

		MidoriResult::TypeResult type_argument_result = UnifyTypeArguments(token, left_union.m_type_arguments, right_union.m_type_arguments, diagnostic_mode);
		if (!type_argument_result.has_value())
		{
			return type_argument_result;
		}

		// The members of a nominal generic type are determined by its type
		// arguments, which have just been unified. Descending again is redundant,
		// and does not terminate when a member reaches the type through a fresh
		// node such as Array<Self>.
		NominalUnifyGuard nominal_guard(*this, left_union.m_name);
		if (!nominal_guard.Entered())
		{
			return left;
		}

		for (auto& [member_name, left_ctx] : left_union.m_member_info)
		{
			std::unordered_map<std::string, MidoriType::UnionType::UnionMemberContext>::iterator right_it = right_union.m_member_info.find(member_name);
			if (right_it == right_union.m_member_info.end())
			{
				return std::unexpected(MakeUnificationError(token, left_subst, right_subst, diagnostic_mode));
			}

			MidoriType::UnionType::UnionMemberContext& right_ctx = right_it->second;
			if (left_ctx.m_member_types.size() != right_ctx.m_member_types.size())
			{
				return std::unexpected(MakeUnificationError(token, left_subst, right_subst, diagnostic_mode));
			}

			for (size_t idx : std::views::iota(0u, left_ctx.m_member_types.size()))
			{
				MidoriResult::TypeResult result = Unify(token, left_ctx.m_member_types[idx], right_ctx.m_member_types[idx], diagnostic_mode);
				if (!result.has_value())
				{
					return result;
				}
			}
		}

		return left;
	}
	else if (left_subst->IsType<MidoriType::TupleType>() && right_subst->IsType<MidoriType::TupleType>())
	{
		MidoriType::TupleType& left_tuple = left_subst->GetType<MidoriType::TupleType>();
		MidoriType::TupleType& right_tuple = right_subst->GetType<MidoriType::TupleType>();

		// Tuple types must have the same number of elements
		if (left_tuple.m_element_types.size() != right_tuple.m_element_types.size())
		{
			return std::unexpected(MakeTupleArityError(token, left_tuple.m_element_types.size(), right_tuple.m_element_types.size(), diagnostic_mode));
		}

		// Unify each element type
		for (size_t idx : std::views::iota(0u, left_tuple.m_element_types.size()))
		{
			MidoriResult::TypeResult result = Unify(token, left_tuple.m_element_types[idx], right_tuple.m_element_types[idx], diagnostic_mode);
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

TypeChecker& TypeChecker::BeginScope()
{
	m_name_type_table.emplace_back();
	return *this;
}

TypeChecker& TypeChecker::EndScope()
{
	m_name_type_table.pop_back();
	return *this;
}

std::shared_ptr<MidoriType>* TypeChecker::FindNameType(const std::string& name)
{
	for (TypeChecker::TypeEnvironmentStack::reverse_iterator it = m_name_type_table.rbegin(); it != m_name_type_table.rend(); ++it)
	{
		TypeEnvironment::iterator found = it->find(name);
		if (found != it->end())
		{
			return &found->second;
		}
	}
	return nullptr;
}

const std::shared_ptr<MidoriType>* TypeChecker::FindNameType(const std::string& name) const
{
	for (TypeChecker::TypeEnvironmentStack::const_reverse_iterator it = m_name_type_table.crbegin(); it != m_name_type_table.crend(); ++it)
	{
		TypeEnvironment::const_iterator found = it->find(name);
		if (found != it->cend())
		{
			return &found->second;
		}
	}
	return nullptr;
}

MidoriResult::TypeResult TypeChecker::Evaluate(const std::unique_ptr<MidoriStatement>& statement)
{
	return VisitNode
	(
		[this]<typename T>(T& stmt) -> MidoriResult::TypeResult
		{
			return (*this)(stmt);
		},
		statement
	);
}

MidoriResult::TypeResult TypeChecker::Evaluate(const std::unique_ptr<MidoriExpression>& expression)
{
	return VisitNode
	(
		[this]<typename T>(T& expr) -> MidoriResult::TypeResult
		{
			return (*this)(expr);
		},
		expression
	);
}

MidoriExpression::ConditionOperandType TypeChecker::ResolveConditionOperandType(MidoriExpression::ConditionOperandType fallback, const std::unique_ptr<MidoriExpression>& expr)
{
	if (!expr->IsExpression<MidoriExpression::Binary>())
	{
		return fallback;
	}

	const MidoriExpression::Binary& binary = expr->GetExpression<MidoriExpression::Binary>();
	const std::shared_ptr<MidoriType>& left_type = binary.m_left->GetType();

	if (left_type->IsType<MidoriType::IntegerType>())
	{
		return MidoriExpression::ConditionOperandType::INTEGER;
	}
	if (left_type->IsType<MidoriType::FloatType>())
	{
		return MidoriExpression::ConditionOperandType::FLOAT;
	}
	return MidoriExpression::ConditionOperandType::OTHER;
}

MidoriResult::TypeResult TypeChecker::CheckPattern(MidoriPattern& pattern, const std::shared_ptr<MidoriType>& expected_type)
{
	std::shared_ptr<MidoriType> resolved_expected = ApplySubstitution(expected_type);

	return std::visit
	(
		[&]<typename T>(T&& node) -> MidoriResult::TypeResult
		{
			using Node = std::decay_t<T>;
			if constexpr (std::is_same_v<Node, MidoriPattern::Binding>)
			{
				node.m_type_data = resolved_expected;
				m_name_type_table.back()[node.m_name.m_lexeme] = resolved_expected;
				return node.m_type_data;
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Wildcard>)
			{
				node.m_type_data = resolved_expected;
				return node.m_type_data;
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Literal>)
			{
				switch (node.m_kind)
				{
				case MidoriPattern::LiteralKind::Bool:
					node.m_type_data = MidoriType::MakeLiteralType<MidoriType::BoolType>();
					break;
				case MidoriPattern::LiteralKind::Float:
					node.m_type_data = MidoriType::MakeLiteralType<MidoriType::FloatType>();
					break;
				case MidoriPattern::LiteralKind::Integer:
					node.m_type_data = MidoriType::MakeLiteralType<MidoriType::IntegerType>();
					break;
				case MidoriPattern::LiteralKind::Byte:
					node.m_type_data = MidoriType::MakeLiteralType<MidoriType::ByteType>();
					break;
				case MidoriPattern::LiteralKind::Word:
					node.m_type_data = MidoriType::MakeLiteralType<MidoriType::WordType>();
					break;
				case MidoriPattern::LiteralKind::Text:
					node.m_type_data = MidoriType::MakeLiteralType<MidoriType::TextType>();
					break;
				case MidoriPattern::LiteralKind::Unit:
					node.m_type_data = MidoriType::MakeLiteralType<MidoriType::UnitType>();
					break;
				}

				return Unify(node.m_token, node.m_type_data, resolved_expected, UnifyDiagnosticMode::ActualExpected)
					.and_then([&node](std::shared_ptr<MidoriType>&&) -> MidoriResult::TypeResult { return node.m_type_data; });
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Tuple>)
			{
				if (!resolved_expected->IsType<MidoriType::TupleType>())
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Pattern type error: expected tuple type", node.m_left_paren, m_file_name, m_source_lines, resolved_expected));
				}

				const MidoriType::TupleType& tuple_type = resolved_expected->GetType<MidoriType::TupleType>();
				if (tuple_type.m_element_types.size() != node.m_elements.size())
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Pattern type error: tuple arity mismatch", node.m_left_paren, m_file_name, m_source_lines));
				}

				for (size_t i = 0u; i < node.m_elements.size(); i += 1u)
				{
					MidoriResult::TypeResult elem_result = CheckPattern(*node.m_elements[i], tuple_type.m_element_types[i]);
					if (!elem_result.has_value())
					{
						return std::unexpected(std::move(elem_result.error()));
					}
				}

				node.m_type_data = resolved_expected;
				return node.m_type_data;
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Array>)
			{
				if (!resolved_expected->IsType<MidoriType::ArrayType>())
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Pattern type error: expected array type", node.m_left_bracket, m_file_name, m_source_lines, resolved_expected));
				}

				const MidoriType::ArrayType& array_type = resolved_expected->GetType<MidoriType::ArrayType>();
				for (const std::unique_ptr<MidoriPattern>& elem : node.m_elements)
				{
					MidoriResult::TypeResult elem_result = CheckPattern(*elem, array_type.m_element_type);
					if (!elem_result.has_value())
					{
						return std::unexpected(std::move(elem_result.error()));
					}
				}

				node.m_type_data = resolved_expected;
				return node.m_type_data;
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Constructor>)
			{
				if (node.m_is_union)
				{
					if (!resolved_expected->IsType<MidoriType::UnionType>())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Pattern type error: expected union type", node.m_name_token, m_file_name, m_source_lines, resolved_expected));
					}

					MidoriType::UnionType& union_type = resolved_expected->GetType<MidoriType::UnionType>();
					if (!union_type.m_member_info.contains(node.m_name))
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(std::format("Pattern type error: unrecognized union member '{}'", node.m_name), node.m_name_token, m_file_name, m_source_lines));
					}

					const MidoriType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(node.m_name);
					if (member_ctx.m_member_types.size() != node.m_args.size())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Pattern type error: incorrect union arity", node.m_name_token, m_file_name, m_source_lines));
					}

					node.m_tag = member_ctx.m_tag;
					for (size_t i = 0u; i < node.m_args.size(); i += 1u)
					{
						MidoriResult::TypeResult arg_result = CheckPattern(*node.m_args[i], member_ctx.m_member_types[i]);
						if (!arg_result.has_value())
						{
							return std::unexpected(std::move(arg_result.error()));
						}
					}

					node.m_type_data = resolved_expected;
					return node.m_type_data;
				}

				if (!resolved_expected->IsType<MidoriType::StructType>())
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Pattern type error: expected struct type", node.m_name_token, m_file_name, m_source_lines, resolved_expected));
				}

				const MidoriType::StructType& struct_type = resolved_expected->GetType<MidoriType::StructType>();
				if (struct_type.m_name != node.m_name)
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Pattern type error: struct name mismatch", node.m_name_token, m_file_name, m_source_lines));
				}

				if (struct_type.m_member_types.size() != node.m_args.size())
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Pattern type error: incorrect struct arity", node.m_name_token, m_file_name, m_source_lines));
				}

				for (size_t i = 0u; i < node.m_args.size(); i += 1u)
				{
					MidoriResult::TypeResult arg_result = CheckPattern(*node.m_args[i], struct_type.m_member_types[i]);
					if (!arg_result.has_value())
					{
						return std::unexpected(std::move(arg_result.error()));
					}
				}

				node.m_type_data = resolved_expected;
				return node.m_type_data;
			}
			else
			{
				return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Pattern type error: unsupported pattern", GetPatternToken(pattern), m_file_name, m_source_lines));
			}
		},
		*pattern
	);
}

bool TypeChecker::IsIrrefutablePattern(const MidoriPattern& pattern, const std::shared_ptr<MidoriType>& expected_type)
{
	std::shared_ptr<MidoriType> resolved_expected = ApplySubstitution(expected_type);

	return std::visit
	(
		[&]<typename T>(T&& node) -> bool
		{
			using Node = std::decay_t<T>;
			if constexpr (std::is_same_v<Node, MidoriPattern::Binding>)
			{
				return true;
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Wildcard>)
			{
				return true;
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Literal>)
			{
				return false;
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Tuple>)
			{
				if (!resolved_expected->IsType<MidoriType::TupleType>())
				{
					return false;
				}
				const MidoriType::TupleType& tuple_type = resolved_expected->GetType<MidoriType::TupleType>();
				if (tuple_type.m_element_types.size() != node.m_elements.size())
				{
					return false;
				}
				for (size_t i = 0u; i < node.m_elements.size(); i += 1u)
				{
					if (!IsIrrefutablePattern(*node.m_elements[i], tuple_type.m_element_types[i]))
					{
						return false;
					}
				}
				return true;
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Array>)
			{
				return false;
			}
			else if constexpr (std::is_same_v<Node, MidoriPattern::Constructor>)
			{
				if (node.m_is_union)
				{
					return false;
				}
				if (!resolved_expected->IsType<MidoriType::StructType>())
				{
					return false;
				}
				const MidoriType::StructType& struct_type = resolved_expected->GetType<MidoriType::StructType>();
				if (struct_type.m_member_types.size() != node.m_args.size())
				{
					return false;
				}
				for (size_t i = 0u; i < node.m_args.size(); i += 1u)
				{
					if (!IsIrrefutablePattern(*node.m_args[i], struct_type.m_member_types[i]))
					{
						return false;
					}
				}
				return true;
			}
			else
			{
				return false;
			}
		},
		*pattern
	);
}

std::shared_ptr<MidoriType> TypeChecker::FreshTypeVar()
{
	return MidoriType::MakeTypeVariable(m_next_type_var_id++);
}

std::shared_ptr<MidoriType> TypeChecker::Freshen(const std::shared_ptr<MidoriType>& type)
{
	FresheningContext context;
	return Freshen(type, context);
}

std::shared_ptr<MidoriType> TypeChecker::Freshen(const std::shared_ptr<MidoriType>& type, FresheningContext& context)
{
	// Check cache first to handle recursive types
	std::unordered_map<const MidoriType*, std::shared_ptr<MidoriType>>::iterator cache_it = context.m_type_cache.find(type.get());
	if (cache_it != context.m_type_cache.end())
	{
		return cache_it->second;
	}

	if (type->IsType<MidoriType::UndecidedType>() || type->IsType<MidoriType::TypeVariable>())
	{
		std::shared_ptr<MidoriType> fresh_var = FreshTypeVar();
		context.m_type_cache[type.get()] = fresh_var;
		return fresh_var;
	}
	else if (type->IsType<MidoriType::GenericParam>())
	{
		// For GenericParam, check if we've already freshened this parameter name
		const std::string& param_name = type->GetType<MidoriType::GenericParam>().m_name;
		TypeEnvironment::iterator it = context.m_generic_params.find(param_name);
		if (it != context.m_generic_params.end())
		{
			// We've already freshened this generic parameter - return the same type variable
			return it->second;
		}
		else
		{
			// First time seeing this generic parameter - create a fresh type variable and store it
			std::shared_ptr<MidoriType> fresh_var = FreshTypeVar();
			context.m_generic_params[param_name] = fresh_var;
			return fresh_var;
		}
	}
	else if (type->IsType<MidoriType::ArrayType>())
	{
		MidoriType::ArrayType& array_type = type->GetType<MidoriType::ArrayType>();
		return MidoriType::MakeArrayType(Freshen(array_type.m_element_type, context));
	}
	else if (type->IsType<MidoriType::RangeType>())
	{
		MidoriType::RangeType& range_type = type->GetType<MidoriType::RangeType>();
		return MidoriType::MakeRangeType(Freshen(range_type.m_element_type, context));
	}
	else if (type->IsType<MidoriType::WorkerType>())
	{
		MidoriType::WorkerType& worker_type = type->GetType<MidoriType::WorkerType>();
		return MidoriType::MakeWorkerType(Freshen(worker_type.m_result_type, context));
	}
	else if (type->IsType<MidoriType::ChannelType>())
	{
		MidoriType::ChannelType& channel_type = type->GetType<MidoriType::ChannelType>();
		return MidoriType::MakeChannelType(Freshen(channel_type.m_element_type, context));
	}
	else if (type->IsType<MidoriType::CellType>())
	{
		MidoriType::CellType& cell_type = type->GetType<MidoriType::CellType>();
		return MidoriType::MakeCellType(Freshen(cell_type.m_element_type, context));
	}
	else if (type->IsType<MidoriType::FunctionType>())
	{
		MidoriType::FunctionType& func_type = type->GetType<MidoriType::FunctionType>();
		std::vector<std::shared_ptr<MidoriType>> fresh_params;
		std::ranges::for_each
		(
			func_type.m_param_types,
			[&fresh_params, &context, this](const std::shared_ptr<MidoriType>& param_type)
			{
				fresh_params.emplace_back(Freshen(param_type, context));
			}
		);

		std::shared_ptr<MidoriType> fresh_return = Freshen(func_type.m_return_type, context);
		std::shared_ptr<MidoriType> fresh_function = MidoriType::MakeFunctionType(fresh_params, std::move(fresh_return), func_type.m_is_foreign);
		std::vector<MidoriType::ClassConstraint> fresh_constraints;
		fresh_constraints.reserve(func_type.m_constraints.size());
		for (const MidoriType::ClassConstraint& constraint : func_type.m_constraints)
		{
			MidoriType::ClassConstraint fresh_constraint(constraint);
			fresh_constraint.m_type_args.clear();
			for (const std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
			{
				fresh_constraint.m_type_args.emplace_back(Freshen(type_arg, context));
			}

			if (fresh_constraint.IsEquality())
			{
				fresh_constraint.m_equality_lhs = Freshen(constraint.m_equality_lhs, context);
				fresh_constraint.m_equality_rhs = Freshen(constraint.m_equality_rhs, context);
			}
			fresh_constraints.push_back(std::move(fresh_constraint));
		}
		fresh_function->GetType<MidoriType::FunctionType>().m_constraints = std::move(fresh_constraints);
		return fresh_function;
	}
	else if (type->IsType<MidoriType::StructType>())
	{
		MidoriType::StructType& struct_type = type->GetType<MidoriType::StructType>();

		// Create fresh struct and add to cache BEFORE recursing to handle cycles
		std::vector<std::shared_ptr<MidoriType>> empty_member_types;
		std::vector<std::string> member_names_copy = struct_type.m_member_names;
		std::vector<std::string> instantiated_generic_params;
		std::shared_ptr<MidoriType> fresh_struct = MidoriType::MakeStructType(struct_type.m_name, std::move(empty_member_types), std::move(member_names_copy), std::move(instantiated_generic_params));
		fresh_struct->GetType<MidoriType::StructType>().m_is_generic_instantiation = struct_type.m_is_generic_instantiation || !struct_type.m_generic_params.empty();
		context.m_type_cache[type.get()] = fresh_struct;

		if (fresh_struct->GetType<MidoriType::StructType>().m_is_generic_instantiation)
		{
			fresh_struct->GetType<MidoriType::StructType>().m_type_arguments = MidoriType::InstantiateTypeArguments
			(
				struct_type.m_generic_params,
				struct_type.m_type_arguments,
				[&context, this](const std::shared_ptr<MidoriType>& type_argument) { return Freshen(type_argument, context); }
			);
		}

		// Now freshen members
		std::vector<std::shared_ptr<MidoriType>> fresh_member_types;
		std::ranges::for_each
		(
			struct_type.m_member_types,
			[&fresh_member_types, &context, this](const std::shared_ptr<MidoriType>& member_type)
			{
				fresh_member_types.push_back(Freshen(member_type, context));
			}
		);

		// Update the fresh struct with freshened members
		fresh_struct->GetType<MidoriType::StructType>().m_member_types = std::move(fresh_member_types);
		std::vector<MidoriType::ClassConstraint> fresh_constraints;
		fresh_constraints.reserve(struct_type.m_constraints.size());
		for (const MidoriType::ClassConstraint& constraint : struct_type.m_constraints)
		{
			MidoriType::ClassConstraint fresh_constraint(constraint);
			fresh_constraint.m_type_args.clear();
			for (const std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
			{
				fresh_constraint.m_type_args.emplace_back(Freshen(type_arg, context));
			}

			if (fresh_constraint.IsEquality())
			{
				fresh_constraint.m_equality_lhs = Freshen(constraint.m_equality_lhs, context);
				fresh_constraint.m_equality_rhs = Freshen(constraint.m_equality_rhs, context);
			}
			fresh_constraints.push_back(std::move(fresh_constraint));
		}
		fresh_struct->GetType<MidoriType::StructType>().m_constraints = std::move(fresh_constraints);

		return fresh_struct;
	}
	else if (type->IsType<MidoriType::UnionType>())
	{
		MidoriType::UnionType& union_type = type->GetType<MidoriType::UnionType>();

		// Create fresh union and add to cache BEFORE recursing to handle cycles
		std::vector<std::string> instantiated_generic_params;
		std::shared_ptr<MidoriType> fresh_union = MidoriType::MakeUnionType(union_type.m_name, std::move(instantiated_generic_params));
		context.m_type_cache[type.get()] = fresh_union;
		MidoriType::UnionType& fresh_union_ref = fresh_union->GetType<MidoriType::UnionType>();
		fresh_union_ref.m_is_generic_instantiation = union_type.m_is_generic_instantiation || !union_type.m_generic_params.empty();

		if (fresh_union_ref.m_is_generic_instantiation)
		{
			fresh_union_ref.m_type_arguments = MidoriType::InstantiateTypeArguments
			(
				union_type.m_generic_params,
				union_type.m_type_arguments,
				[&context, this](const std::shared_ptr<MidoriType>& type_argument) { return Freshen(type_argument, context); }
			);
		}

		std::vector<MidoriType::ClassConstraint> fresh_constraints;
		fresh_constraints.reserve(union_type.m_constraints.size());
		for (const MidoriType::ClassConstraint& constraint : union_type.m_constraints)
		{
			MidoriType::ClassConstraint fresh_constraint(constraint);
			fresh_constraint.m_type_args.clear();
			for (const std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
			{
				fresh_constraint.m_type_args.emplace_back(Freshen(type_arg, context));
			}

			if (fresh_constraint.IsEquality())
			{
				fresh_constraint.m_equality_lhs = Freshen(constraint.m_equality_lhs, context);
				fresh_constraint.m_equality_rhs = Freshen(constraint.m_equality_rhs, context);
			}
			fresh_constraints.push_back(std::move(fresh_constraint));
		}
		fresh_union_ref.m_constraints = std::move(fresh_constraints);

		for (const auto& [member_name, member_ctx] : union_type.m_member_info)
		{
			std::vector<std::shared_ptr<MidoriType>> fresh_member_types;
			std::ranges::for_each
			(
				member_ctx.m_member_types,
				[&fresh_member_types, &context, this](const std::shared_ptr<MidoriType>& member_type)
				{
					fresh_member_types.emplace_back(Freshen(member_type, context));
				}
			);
			fresh_union_ref.m_member_info.emplace(member_name, MidoriType::UnionType::UnionMemberContext(std::move(fresh_member_types), member_ctx.m_tag));
		}

		return fresh_union;
	}
	else if (type->IsType<MidoriType::TupleType>())
	{
		MidoriType::TupleType& tuple_type = type->GetType<MidoriType::TupleType>();
		std::vector<std::shared_ptr<MidoriType>> fresh_element_types;
		std::ranges::for_each
		(
			tuple_type.m_element_types,
			[&fresh_element_types, &context, this](const std::shared_ptr<MidoriType>& element_type)
			{
				fresh_element_types.emplace_back(Freshen(element_type, context));
			}
		);
		return MidoriType::MakeTupleType(std::move(fresh_element_types));
	}
	else if (type->IsType<MidoriType::AssociatedType>())
	{
		MidoriType::AssociatedType& associated_type = type->GetType<MidoriType::AssociatedType>();
		std::vector<std::shared_ptr<MidoriType>> fresh_type_args;
		fresh_type_args.reserve(associated_type.m_type_args.size());
		for (const std::shared_ptr<MidoriType>& type_arg : associated_type.m_type_args)
		{
			fresh_type_args.emplace_back(Freshen(type_arg, context));
		}
		return MidoriType::MakeAssociatedType(associated_type.m_class_name, associated_type.m_name, std::move(fresh_type_args));
	}
	return type;
}

TypeChecker::FresheningContext TypeChecker::MakeLambdaFresheningContext()
{
	FresheningContext context;
	for (TypeEnvironmentStack::const_reverse_iterator scope_it = m_name_type_table.crbegin(); scope_it != m_name_type_table.crend(); ++scope_it)
	{
		for (const auto& [name, bound_type] : *scope_it)
		{
			if (context.m_generic_params.contains(name))
			{
				continue;
			}

			std::shared_ptr<MidoriType> resolved_type = ApplySubstitution(bound_type);
			if (!resolved_type->IsType<MidoriType::TypeVariable>())
			{
				continue;
			}

			context.m_generic_params.emplace(name, resolved_type);
			context.m_type_cache.emplace(resolved_type.get(), resolved_type);
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

	for (const TypeEnvironment& scope : m_name_type_table)
	{
		for (const auto& [_, bound_type] : scope)
		{
			std::unordered_set<int> scope_type_vars = CollectTypeVariableIds(ApplySubstitution(bound_type));
			enclosing_type_vars.insert(scope_type_vars.cbegin(), scope_type_vars.cend());
		}
	}

	for (const MidoriType::ClassConstraint& constraint : m_active_constraints)
	{
		for (const std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
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

std::shared_ptr<MidoriType> TypeChecker::ApplySubstitution(const std::shared_ptr<MidoriType>& type)
{
	std::unordered_map<const MidoriType*, std::shared_ptr<MidoriType>> cache;
	return ApplySubstitution(type, cache);
}

std::shared_ptr<MidoriType> TypeChecker::ApplySubstitution(const std::shared_ptr<MidoriType>& type, std::unordered_map<const MidoriType*, std::shared_ptr<MidoriType>>& cache)
{
	if (
			type->IsType<MidoriType::IntegerType>() || 
			type->IsType<MidoriType::FloatType>() || 
			type->IsType<MidoriType::BoolType>() || 
			type->IsType<MidoriType::UnitType>() || 
			type->IsType<MidoriType::TextType>() || 
			type->IsType<MidoriType::ByteType>() || 
			type->IsType<MidoriType::WordType>() || 
			type->IsType<MidoriType::NeverType>()
		)
	{
		return type;
	}

	std::unordered_set<const MidoriType*> visited;
	if (!HasTypeVariables(type, visited) && !ContainsAssociatedTypes(type))
	{
		return type;
	}

	// Check cache first to handle recursive types
	std::unordered_map<const MidoriType*, std::shared_ptr<MidoriType>>::iterator cache_it = cache.find(type.get());
	if (cache_it != cache.end())
	{
		return cache_it->second;
	}

	if (type->IsType<MidoriType::TypeVariable>())
	{
		int var_id = type->GetType<MidoriType::TypeVariable>().m_id;
		TypeSubstitution::iterator it = m_type_substitution.find(var_id);
		if (it != m_type_substitution.end())
		{
			// Recursively apply substitution in case the substitution itself contains type variables
			return ApplySubstitution(it->second, cache);
		}
		return type;
	}
	else if (type->IsType<MidoriType::AssociatedType>())
	{
		MidoriType::AssociatedType& associated_type = type->GetType<MidoriType::AssociatedType>();
		bool changed = false;
		std::vector<std::shared_ptr<MidoriType>> substituted_type_args;
		substituted_type_args.reserve(associated_type.m_type_args.size());

		for (const std::shared_ptr<MidoriType>& type_arg : associated_type.m_type_args)
		{
			std::shared_ptr<MidoriType> substituted_type_arg = ApplySubstitution(type_arg, cache);
			substituted_type_args.emplace_back(substituted_type_arg);
			if (substituted_type_arg != type_arg)
			{
				changed = true;
			}
		}

		std::shared_ptr<MidoriType> associated_type_ref = changed
			? MidoriType::MakeAssociatedType(associated_type.m_class_name, associated_type.m_name, std::move(substituted_type_args))
			: type;
		std::shared_ptr<MidoriType> resolved_type = ResolveAssociatedType(associated_type_ref->GetType<MidoriType::AssociatedType>());
		cache[type.get()] = resolved_type;
		return resolved_type;
	}
	else if (type->IsType<MidoriType::ArrayType>())
	{
		MidoriType::ArrayType& array_type = type->GetType<MidoriType::ArrayType>();
		std::shared_ptr<MidoriType> element_type = ApplySubstitution(array_type.m_element_type, cache);
		if (element_type != array_type.m_element_type)
		{
			return MidoriType::MakeArrayType(element_type);
		}
		return type;
	}
	else if (type->IsType<MidoriType::RangeType>())
	{
		MidoriType::RangeType& range_type = type->GetType<MidoriType::RangeType>();
		std::shared_ptr<MidoriType> element_type = ApplySubstitution(range_type.m_element_type, cache);
		if (element_type != range_type.m_element_type)
		{
			return MidoriType::MakeRangeType(element_type);
		}
		return type;
	}
	else if (type->IsType<MidoriType::WorkerType>())
	{
		MidoriType::WorkerType& worker_type = type->GetType<MidoriType::WorkerType>();
		std::shared_ptr<MidoriType> result_type = ApplySubstitution(worker_type.m_result_type, cache);
		if (result_type != worker_type.m_result_type)
		{
			return MidoriType::MakeWorkerType(result_type);
		}
		return type;
	}
	else if (type->IsType<MidoriType::ChannelType>())
	{
		MidoriType::ChannelType& channel_type = type->GetType<MidoriType::ChannelType>();
		std::shared_ptr<MidoriType> element_type = ApplySubstitution(channel_type.m_element_type, cache);
		if (element_type != channel_type.m_element_type)
		{
			return MidoriType::MakeChannelType(element_type);
		}
		return type;
	}
	else if (type->IsType<MidoriType::CellType>())
	{
		MidoriType::CellType& cell_type = type->GetType<MidoriType::CellType>();
		std::shared_ptr<MidoriType> element_type = ApplySubstitution(cell_type.m_element_type, cache);
		if (element_type != cell_type.m_element_type)
		{
			return MidoriType::MakeCellType(element_type);
		}
		return type;
	}
	else if (type->IsType<MidoriType::FunctionType>())
	{
		MidoriType::FunctionType& func_type = type->GetType<MidoriType::FunctionType>();
		bool changed = false;
		std::vector<std::shared_ptr<MidoriType>> new_param_types;

		for (const std::shared_ptr<MidoriType>& param_type : func_type.m_param_types)
		{
			std::shared_ptr<MidoriType> subst_param = ApplySubstitution(param_type, cache);
			new_param_types.push_back(subst_param);
			if (subst_param != param_type)
			{
				changed = true;
			}
		}

		std::shared_ptr<MidoriType> new_return_type = ApplySubstitution(func_type.m_return_type, cache);
		if (new_return_type != func_type.m_return_type)
		{
			changed = true;
		}

		std::vector<MidoriType::ClassConstraint> new_constraints;
		new_constraints.reserve(func_type.m_constraints.size());
		for (const MidoriType::ClassConstraint& constraint : func_type.m_constraints)
		{
			MidoriType::ClassConstraint substituted_constraint(constraint);
			substituted_constraint.m_type_args.clear();
			if (substituted_constraint.IsEquality())
			{
				substituted_constraint.m_equality_lhs = ApplySubstitution(constraint.m_equality_lhs, cache);
				substituted_constraint.m_equality_rhs = ApplySubstitution(constraint.m_equality_rhs, cache);
				if (substituted_constraint.m_equality_lhs != constraint.m_equality_lhs || substituted_constraint.m_equality_rhs != constraint.m_equality_rhs)
				{
					changed = true;
				}
			}

			for (const std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
			{
				std::shared_ptr<MidoriType> substituted_type_arg = ApplySubstitution(type_arg, cache);
				substituted_constraint.m_type_args.emplace_back(substituted_type_arg);
				if (substituted_type_arg != type_arg)
				{
					changed = true;
				}
			}
			new_constraints.push_back(std::move(substituted_constraint));
		}

		if (changed)
		{
			std::shared_ptr<MidoriType> new_function = MidoriType::MakeFunctionType(new_param_types, std::move(new_return_type), func_type.m_is_foreign);
			new_function->GetType<MidoriType::FunctionType>().m_constraints = std::move(new_constraints);
			return new_function;
		}
		return type;
	}
	else if (type->IsType<MidoriType::StructType>())
	{
		MidoriType::StructType& struct_type = type->GetType<MidoriType::StructType>();

		// Create new struct and add to cache BEFORE recursing to handle cycles
		std::vector<std::shared_ptr<MidoriType>> empty_member_types;
		std::vector<std::string> member_names_copy = struct_type.m_member_names;
		std::vector<std::string> instantiated_generic_params;
		std::shared_ptr<MidoriType> new_struct = MidoriType::MakeStructType(struct_type.m_name, std::move(empty_member_types), std::move(member_names_copy), std::move(instantiated_generic_params));
		cache[type.get()] = new_struct;

		bool changed = false;
		std::vector<std::shared_ptr<MidoriType>> new_member_types;

		for (const std::shared_ptr<MidoriType>& member_type : struct_type.m_member_types)
		{
			std::shared_ptr<MidoriType> subst_member = ApplySubstitution(member_type, cache);
			new_member_types.push_back(subst_member);
			if (subst_member != member_type)
			{
				changed = true;
			}
		}

		std::vector<MidoriType::ClassConstraint> new_constraints;
		new_constraints.reserve(struct_type.m_constraints.size());
		for (const MidoriType::ClassConstraint& constraint : struct_type.m_constraints)
		{
			MidoriType::ClassConstraint substituted_constraint(constraint);
			substituted_constraint.m_type_args.clear();
			if (substituted_constraint.IsEquality())
			{
				substituted_constraint.m_equality_lhs = ApplySubstitution(constraint.m_equality_lhs, cache);
				substituted_constraint.m_equality_rhs = ApplySubstitution(constraint.m_equality_rhs, cache);
				if (substituted_constraint.m_equality_lhs != constraint.m_equality_lhs || substituted_constraint.m_equality_rhs != constraint.m_equality_rhs)
				{
					changed = true;
				}
			}

			for (const std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
			{
				std::shared_ptr<MidoriType> substituted_type_arg = ApplySubstitution(type_arg, cache);
				substituted_constraint.m_type_args.emplace_back(substituted_type_arg);
				if (substituted_type_arg != type_arg)
				{
					changed = true;
				}
			}
			new_constraints.push_back(std::move(substituted_constraint));
		}

		std::vector<std::shared_ptr<MidoriType>> new_type_arguments;
		new_type_arguments.reserve(struct_type.m_type_arguments.size());
		for (const std::shared_ptr<MidoriType>& type_argument : struct_type.m_type_arguments)
		{
			std::shared_ptr<MidoriType> substituted_type_argument = ApplySubstitution(type_argument, cache);
			new_type_arguments.emplace_back(substituted_type_argument);
			if (substituted_type_argument != type_argument)
			{
				changed = true;
			}
		}

		if (changed)
		{
			new_struct->GetType<MidoriType::StructType>().m_member_types = std::move(new_member_types);
			new_struct->GetType<MidoriType::StructType>().m_constraints = std::move(new_constraints);
			new_struct->GetType<MidoriType::StructType>().m_type_arguments = std::move(new_type_arguments);
			// Mark this as a generic instantiation if the original had generic params
			if (!struct_type.m_generic_params.empty() || struct_type.m_is_generic_instantiation)
			{
				new_struct->GetType<MidoriType::StructType>().m_is_generic_instantiation = true;
			}
			return new_struct;
		}
		cache[type.get()] = type;
		return type;
	}
	else if (type->IsType<MidoriType::UnionType>())
	{
		MidoriType::UnionType& union_type = type->GetType<MidoriType::UnionType>();

		// Create new union and add to cache BEFORE recursing to handle cycles
		std::vector<std::string> instantiated_generic_params;
		std::shared_ptr<MidoriType> new_union = MidoriType::MakeUnionType(union_type.m_name, std::move(instantiated_generic_params));
		cache[type.get()] = new_union;
		MidoriType::UnionType& new_union_ref = new_union->GetType<MidoriType::UnionType>();

		bool changed = false;
		std::vector<MidoriType::ClassConstraint> new_constraints;
		new_constraints.reserve(union_type.m_constraints.size());
		for (const MidoriType::ClassConstraint& constraint : union_type.m_constraints)
		{
			MidoriType::ClassConstraint substituted_constraint(constraint);
			substituted_constraint.m_type_args.clear();
			if (substituted_constraint.IsEquality())
			{
				substituted_constraint.m_equality_lhs = ApplySubstitution(constraint.m_equality_lhs, cache);
				substituted_constraint.m_equality_rhs = ApplySubstitution(constraint.m_equality_rhs, cache);
				if (substituted_constraint.m_equality_lhs != constraint.m_equality_lhs || substituted_constraint.m_equality_rhs != constraint.m_equality_rhs)
				{
					changed = true;
				}
			}

			for (const std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
			{
				std::shared_ptr<MidoriType> substituted_type_arg = ApplySubstitution(type_arg, cache);
				substituted_constraint.m_type_args.emplace_back(substituted_type_arg);
				if (substituted_type_arg != type_arg)
				{
					changed = true;
				}
			}
			new_constraints.push_back(std::move(substituted_constraint));
		}
		new_union_ref.m_constraints = std::move(new_constraints);

		std::vector<std::shared_ptr<MidoriType>> new_type_arguments;
		new_type_arguments.reserve(union_type.m_type_arguments.size());
		for (const std::shared_ptr<MidoriType>& type_argument : union_type.m_type_arguments)
		{
			std::shared_ptr<MidoriType> substituted_type_argument = ApplySubstitution(type_argument, cache);
			new_type_arguments.emplace_back(substituted_type_argument);
			if (substituted_type_argument != type_argument)
			{
				changed = true;
			}
		}
		new_union_ref.m_type_arguments = std::move(new_type_arguments);

		for (const auto& [member_name, member_ctx] : union_type.m_member_info)
		{
			std::vector<std::shared_ptr<MidoriType>> new_member_types;
			for (const std::shared_ptr<MidoriType>& member_type : member_ctx.m_member_types)
			{
				std::shared_ptr<MidoriType> subst_member = ApplySubstitution(member_type, cache);
				new_member_types.push_back(subst_member);
				if (subst_member != member_type)
				{
					changed = true;
				}
			}
			new_union_ref.m_member_info.emplace(member_name, MidoriType::UnionType::UnionMemberContext(std::move(new_member_types), member_ctx.m_tag));
		}

		if (changed)
		{
			// Mark this as a generic instantiation if the original had generic params
			if (!union_type.m_generic_params.empty() || union_type.m_is_generic_instantiation)
			{
				new_union->GetType<MidoriType::UnionType>().m_is_generic_instantiation = true;
			}
			return new_union;
		}
		cache[type.get()] = type;
		return type;
	}
	else if (type->IsType<MidoriType::TupleType>())
	{
		MidoriType::TupleType& tuple_type = type->GetType<MidoriType::TupleType>();
		bool changed = false;
		std::vector<std::shared_ptr<MidoriType>> new_element_types;

		for (const std::shared_ptr<MidoriType>& element_type : tuple_type.m_element_types)
		{
			std::shared_ptr<MidoriType> subst_element = ApplySubstitution(element_type, cache);
			new_element_types.push_back(subst_element);
			if (subst_element != element_type)
			{
				changed = true;
			}
		}

		if (changed)
		{
			return MidoriType::MakeTupleType(std::move(new_element_types));
		}
		return type;
	}

	return type;
}

bool TypeChecker::OccursCheck(int var_id, const std::shared_ptr<MidoriType>& type)
{
	std::unordered_set<const MidoriType*> visited;
	return OccursCheck(var_id, type, visited);
}

bool TypeChecker::OccursCheck(int var_id, const std::shared_ptr<MidoriType>& type, std::unordered_set<const MidoriType*>& visited)
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

	std::shared_ptr<MidoriType> subst_type = ApplySubstitution(type);

	if (visited.contains(subst_type.get()))
	{
		return false;
	}
	visited.insert(subst_type.get());

	if (subst_type->IsType<MidoriType::TypeVariable>())
	{
		return subst_type->GetType<MidoriType::TypeVariable>().m_id == var_id;
	}
	else if (subst_type->IsType<MidoriType::ArrayType>())
	{
		return OccursCheck(var_id, subst_type->GetType<MidoriType::ArrayType>().m_element_type, visited);
	}
	else if (subst_type->IsType<MidoriType::RangeType>())
	{
		return OccursCheck(var_id, subst_type->GetType<MidoriType::RangeType>().m_element_type, visited);
	}
	else if (subst_type->IsType<MidoriType::WorkerType>())
	{
		return OccursCheck(var_id, subst_type->GetType<MidoriType::WorkerType>().m_result_type, visited);
	}
	else if (subst_type->IsType<MidoriType::ChannelType>())
	{
		return OccursCheck(var_id, subst_type->GetType<MidoriType::ChannelType>().m_element_type, visited);
	}
	else if (subst_type->IsType<MidoriType::CellType>())
	{
		return OccursCheck(var_id, subst_type->GetType<MidoriType::CellType>().m_element_type, visited);
	}
	else if (subst_type->IsType<MidoriType::FunctionType>())
	{
		MidoriType::FunctionType& func_type = subst_type->GetType<MidoriType::FunctionType>();
		for (const std::shared_ptr<MidoriType>& param_type : func_type.m_param_types)
		{
			if (OccursCheck(var_id, param_type, visited))
			{
				return true;
			}
		}
		return OccursCheck(var_id, func_type.m_return_type, visited);
	}
	else if (subst_type->IsType<MidoriType::StructType>())
	{
		MidoriType::StructType& struct_type = subst_type->GetType<MidoriType::StructType>();
		for (const std::shared_ptr<MidoriType>& member_type : struct_type.m_member_types)
		{
			if (OccursCheck(var_id, member_type, visited))
			{
				return true;
			}
		}
		for (const std::shared_ptr<MidoriType>& type_argument : struct_type.m_type_arguments)
		{
			if (OccursCheck(var_id, type_argument, visited))
			{
				return true;
			}
		}
		return false;
	}
	else if (subst_type->IsType<MidoriType::UnionType>())
	{
		MidoriType::UnionType& union_type = subst_type->GetType<MidoriType::UnionType>();
		// A variable can only reach a nominal type through its type arguments, which
		// are checked below. Walking the members again is redundant and does not
		// terminate when a member reaches the type through a fresh node.
		NominalUnifyGuard occurs_guard(*this, "occurs:" + union_type.m_name);
		if (occurs_guard.Entered())
		{
			for (const auto& [member_name, member_ctx] : union_type.m_member_info)
			{
				for (const std::shared_ptr<MidoriType>& member_type : member_ctx.m_member_types)
				{
					if (OccursCheck(var_id, member_type, visited))
					{
						return true;
					}
				}
			}
		}
		for (const std::shared_ptr<MidoriType>& type_argument : union_type.m_type_arguments)
		{
			if (OccursCheck(var_id, type_argument, visited))
			{
				return true;
			}
		}
		return false;
	}
	else if (subst_type->IsType<MidoriType::TupleType>())
	{
		MidoriType::TupleType& tuple_type = subst_type->GetType<MidoriType::TupleType>();
		for (const std::shared_ptr<MidoriType>& element_type : tuple_type.m_element_types)
		{
			if (OccursCheck(var_id, element_type, visited))
			{
				return true;
			}
		}
		return false;
	}
	else if (subst_type->IsType<MidoriType::AssociatedType>())
	{
		MidoriType::AssociatedType& associated_type = subst_type->GetType<MidoriType::AssociatedType>();
		for (const std::shared_ptr<MidoriType>& type_arg : associated_type.m_type_args)
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

TypeChecker::ClassInfo::ClassInfo(const std::string& name, std::vector<std::string>&& params, std::vector<MidoriType::ClassConstraint>&& supers, AssociatedTypeEnvironment&& associated_types, TypeEnvironment&& methods, std::unordered_set<std::string>&& defaults)
	: m_name(name),
	m_type_param_names(std::move(params)),
	m_superclasses(std::move(supers)),
	m_associated_types(std::move(associated_types)),
	m_method_types(std::move(methods)),
	m_methods_with_defaults(std::move(defaults))
{
}

TypeChecker::InstanceInfo::InstanceInfo(const std::string& tc_name, std::vector<std::shared_ptr<MidoriType>>&& args, std::vector<MidoriType::ClassConstraint>&& constraints, AssociatedTypeEnvironment&& associated_type_bindings, std::unordered_map<std::string, std::unique_ptr<MidoriStatement>>&& methods, bool is_derived)
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
	MidoriProgramTree&& parser_result,
	std::string_view file_name,
	const std::vector<std::string>& source_lines,
	TypeEnvironment imported_types,
	const std::unordered_map<std::string, ClassInfo>& imported_typeclasses,
	TypeclassInstanceTypeMap imported_instance_types,
	TypeclassInstanceAssociatedTypeBindingMap imported_instance_associated_type_bindings
)
	: m_program_tree(std::move(parser_result)),
	m_classes(imported_typeclasses),
	m_file_name(file_name),
	m_source_lines(source_lines),
	m_next_type_var_id(0)
{
	// Pre-populate type environment with imported types
	if (!imported_types.empty())
	{
		TypeEnvironment imported_names;
		for (const auto& [name, type] : imported_types)
		{
			if (type->IsType<MidoriType::StructType>())
			{
				const MidoriType::StructType& struct_type = type->GetType<MidoriType::StructType>();
				m_struct_type_definitions[struct_type.m_name] = type;
				if (!struct_type.m_generic_params.empty())
				{
					m_generic_structs.insert(struct_type.m_name);
				}

				// The declaring module binds a struct's name to its constructor's
				// function type, not to the struct type, so an importing module must
				// bind the same thing or there is nothing callable under the name.
				// The type itself is still reachable through m_struct_type_definitions,
				// registered just above.
				imported_names[name] = MidoriType::MakeFunctionType(struct_type.m_member_types, std::shared_ptr<MidoriType>(type));
				continue;
			}

			if (type->IsType<MidoriType::UnionType>())
			{
				const MidoriType::UnionType& union_type = type->GetType<MidoriType::UnionType>();
				m_union_type_definitions[union_type.m_name] = type;
				if (!union_type.m_generic_params.empty())
				{
					m_generic_unions.insert(union_type.m_name);
				}
			}

			imported_names[name] = type;
		}

		m_name_type_table.push_back(std::move(imported_names));
	}

	if (!imported_instance_types.empty())
	{
		for (const auto& [class_name, instances] : imported_instance_types)
		{
			for (size_t instance_idx = 0u; instance_idx < instances.size(); instance_idx += 1u)
			{
				const std::vector<std::shared_ptr<MidoriType>>& type_args = instances[instance_idx];
				if (type_args.empty())
				{
					continue;
				}

				std::vector<std::string> concrete_type_names;
				concrete_type_names.reserve(type_args.size());
				for (const std::shared_ptr<MidoriType>& type_arg : type_args)
				{
					concrete_type_names.emplace_back(type_arg->ToString());
				}

				InstanceKey instance_key{ class_name, std::move(concrete_type_names) };
				if (!m_instances.contains(instance_key))
				{
					std::vector<std::shared_ptr<MidoriType>> type_args_copy = type_args;
					AssociatedTypeEnvironment associated_type_bindings;
					TypeclassInstanceAssociatedTypeBindingMap::const_iterator imported_bindings_it = imported_instance_associated_type_bindings.find(class_name);
					if (imported_bindings_it != imported_instance_associated_type_bindings.cend() && instance_idx < imported_bindings_it->second.size())
					{
						associated_type_bindings = imported_bindings_it->second[instance_idx];
					}
					m_instances.emplace(std::move(instance_key), InstanceInfo(class_name, std::move(type_args_copy), std::vector<MidoriType::ClassConstraint>{}, std::move(associated_type_bindings), std::unordered_map<std::string, std::unique_ptr<MidoriStatement>>()));
				}
			}
		}
	}
}

MidoriResult::TypeCheckerResult TypeChecker::TypeCheck()
{
	return ScopeSession(*this).Then([&]() -> MidoriResult::TypeCheckerResult
	{
		std::vector<CompilerError> errors;

		std::ranges::for_each
		(
			m_program_tree,
			[&errors, this](std::unique_ptr<MidoriStatement>& statement)
			{
				MidoriResult::TypeResult result = Evaluate(statement);
				if (!result.has_value())
				{
					errors.emplace_back(std::move(result.error()));
				}
			}
		);

		if (errors.empty())
		{
			return std::move(m_program_tree);
		}
		return std::unexpected(MidoriResult::CompilerDiagnostics(std::move(errors)));
	});
}

// Extract type signatures from parsed AST without full type checking
// This allows parallel type checking of dependent modules
TypeChecker::TypeEnvironment TypeChecker::ExtractTypeSignatures(const MidoriProgramTree& ast, const std::unordered_set<std::string>* exported_symbols)
{
	TypeEnvironment signatures;

	for (const std::unique_ptr<MidoriStatement>& statement : ast)
	{
		std::visit(
			[&signatures, exported_symbols]<typename T>(const T& stmt) {
				using Node = std::decay_t<T>;

				if constexpr (std::is_same_v<Node, MidoriStatement::FunctionDefinition>)
				{
					if (exported_symbols == nullptr || exported_symbols->contains(stmt.m_name.m_lexeme))
					{
						std::shared_ptr<MidoriType> function_type = MidoriType::MakeFunctionType(
							stmt.m_param_types,
							std::shared_ptr<MidoriType>(stmt.m_return_type)
						);
						function_type->GetType<MidoriType::FunctionType>().m_constraints = stmt.m_constraints;
						signatures[stmt.m_name.m_lexeme] = function_type;
					}
				}
				else if constexpr (std::is_same_v<Node, MidoriStatement::Struct>)
				{
					if (exported_symbols == nullptr || exported_symbols->contains(stmt.m_name.m_lexeme))
					{
						signatures[stmt.m_name.m_lexeme] = stmt.m_self_type;
					}
				}
				else if constexpr (std::is_same_v<Node, MidoriStatement::Union>)
				{
					if (exported_symbols == nullptr || exported_symbols->contains(stmt.m_name.m_lexeme))
					{
						signatures[stmt.m_name.m_lexeme] = stmt.m_self_type;

						if (stmt.m_self_type->template IsType<MidoriType::UnionType>())
						{
							const MidoriType::UnionType& union_type = stmt.m_self_type->template GetType<MidoriType::UnionType>();
							for (const auto& [member_name, member_ctx] : union_type.m_member_info)
							{
								std::vector<std::shared_ptr<MidoriType>> member_types_copy = member_ctx.m_member_types;
								std::shared_ptr<MidoriType> union_constructor_type = MidoriType::MakeFunctionType(std::move(member_types_copy), std::shared_ptr<MidoriType>(stmt.m_self_type));
								union_constructor_type->GetType<MidoriType::FunctionType>().m_constraints = stmt.m_constraints;
								signatures[member_name] = union_constructor_type;
							}
						}
					}
				}
				else if constexpr (std::is_same_v<Node, MidoriStatement::ForeignDefinition>)
				{
					if (exported_symbols == nullptr || exported_symbols->contains(stmt.m_function_name.m_lexeme))
					{
						signatures[stmt.m_function_name.m_lexeme] = stmt.m_type;
					}
				}
				else if constexpr (std::is_same_v<Node, MidoriStatement::TypeAlias>)
				{
					if (exported_symbols == nullptr || exported_symbols->contains(stmt.m_name.m_lexeme))
					{
						signatures[stmt.m_name.m_lexeme] = stmt.m_aliased_type;
					}
				}
				else if constexpr (std::is_same_v<Node, MidoriStatement::VariableDefinition>)
				{
					if (exported_symbols == nullptr || exported_symbols->contains(stmt.m_name.m_lexeme))
					{
						if (stmt.m_annotated_type.has_value())
						{
							signatures[stmt.m_name.m_lexeme] = stmt.m_annotated_type.value();
						}
						else if (stmt.m_value != nullptr && stmt.m_value->template IsExpression<MidoriExpression::Function>())
						{
							const MidoriExpression::Function& function = stmt.m_value->template GetExpression<MidoriExpression::Function>();
							std::shared_ptr<MidoriType> function_type = MidoriType::MakeFunctionType(
								function.m_param_types,
								std::shared_ptr<MidoriType>(function.m_return_type)
							);
							function_type->GetType<MidoriType::FunctionType>().m_constraints = function.m_constraints;
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

MidoriResult::TypeResult TypeChecker::operator()(MidoriStatement::ExpressionStatement& simple)
{
	return Evaluate(simple.m_expr)
		.and_then
		(
			[](std::shared_ptr<MidoriType>&&) ->MidoriResult::TypeResult
			{
				return MidoriType::MakeUndecidedType();
			}
		);
}

MidoriResult::TypeResult TypeChecker::TypeCheckGenericLambdaDefinition(MidoriStatement::VariableDefinition& def, MidoriExpression::Function& function)
{
	// A generic lambda is specialized by name at each call site, so it must be a
	// named binding at the top level.
	if (def.m_local_index.has_value())
	{
		return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Function expression type error: a generic lambda must be bound at the top level, because it is specialized by name at each call site", function.m_function_keyword, m_file_name, m_source_lines));
	}

	if (def.m_annotated_type.has_value())
	{
		return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Function expression type error: a generic lambda cannot carry a type annotation, because its type differs at each call site", function.m_function_keyword, m_file_name, m_source_lines));
	}

	// Validate that generic parameter names are unique
	std::unordered_set<std::string> generic_param_names;
	for (const Token& generic_param : function.m_generic_params)
	{
		if (!generic_param_names.insert(generic_param.m_lexeme).second)
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Function expression type error: duplicate generic parameter name", generic_param, m_file_name, m_source_lines));
		}
	}

	// Validate that generic parameters don't conflict with function parameters
	for (const Token& param : function.m_params)
	{
		if (generic_param_names.contains(param.m_lexeme))
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Function expression type error: generic parameter conflicts with function parameter", param, m_file_name, m_source_lines));
		}
	}

	std::vector<MidoriType::ClassConstraint> propagated_constraints = CollectSignatureConstraints(function.m_param_types, function.m_return_type);
	for (MidoriType::ClassConstraint& propagated_constraint : propagated_constraints)
	{
		AppendUniqueConstraint(function.m_constraints, std::move(propagated_constraint));
	}

	for (const MidoriType::ClassConstraint& constraint : function.m_constraints)
	{
		if (!m_classes.contains(ConstrainedClassName(constraint)))
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Function expression type error: undefined class '" + ConstrainedClassName(constraint) + "' in constraint", function.m_function_keyword, m_file_name, m_source_lines));
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
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Function expression type error: class '" + constraint.m_class_name + "' expects " + std::to_string(tc_info.m_type_param_names.size()) + " type argument(s) but got " + std::to_string(constraint.m_type_args.size()), function.m_function_keyword, m_file_name, m_source_lines));
		}
	}

	FresheningContext freshening_context;
	for (std::shared_ptr<MidoriType>& param_type : function.m_param_types)
	{
		param_type = Freshen(param_type, freshening_context);
	}
	function.m_return_type = Freshen(function.m_return_type, freshening_context);
	for (MidoriType::ClassConstraint& constraint : function.m_constraints)
	{
		for (std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
		{
			type_arg = Freshen(type_arg, freshening_context);
		}

		// An equality constraint carries its types in its operands rather than in
		// m_type_args, and they must be freshened alongside the signature or the
		// projection will never match the one built at the use site.
		if (constraint.IsEquality())
		{
			constraint.m_equality_lhs = Freshen(constraint.m_equality_lhs, freshening_context);
			constraint.m_equality_rhs = Freshen(constraint.m_equality_rhs, freshening_context);
		}
	}

	std::shared_ptr<MidoriType> return_type_copy = function.m_return_type;
	std::shared_ptr<MidoriType> function_type = MidoriType::MakeFunctionType(function.m_param_types, std::move(return_type_copy));
	function_type->GetType<MidoriType::FunctionType>().m_constraints = function.m_constraints;
	function.m_type_data = function_type;
	def.m_value->GetType() = function_type;

	// Bind the name before evaluating the body so the lambda can recurse, and record
	// it as generic so each use site is freshened instead of pinned to one instantiation.
	m_name_type_table.back()[def.m_name.m_lexeme] = function_type;
	m_generic_functions.insert(def.m_name.m_lexeme);

	return ScopeSession(*this).Then([&]() -> MidoriResult::TypeResult
	{
		for (const Token& generic_param : function.m_generic_params)
		{
			const std::string& param_name = generic_param.m_lexeme;
			TypeEnvironment::iterator it = freshening_context.m_generic_params.find(param_name);
			if (it == freshening_context.m_generic_params.end())
			{
				std::shared_ptr<MidoriType> fresh_var = FreshTypeVar();
				it = freshening_context.m_generic_params.emplace(param_name, std::move(fresh_var)).first;
			}
			m_name_type_table.back().emplace(param_name, it->second);
		}

		std::ranges::for_each
		(
			std::views::iota(0u, function.m_params.size()),
			[&function, this](size_t idx)
			{
				m_name_type_table.back().emplace(function.m_params[idx].m_lexeme, function.m_param_types[idx]);
			}
		);

		std::shared_ptr<MidoriType> saved_expected_return_type = m_expected_return_type;
		m_expected_return_type = function.m_return_type;

		size_t prev_constraints_size = m_active_constraints.size();
		for (const MidoriType::ClassConstraint& constraint : function.m_constraints)
		{
			if (!ContainsConstraint(m_active_constraints, constraint))
			{
				m_active_constraints.push_back(constraint);
			}
		}

		// Inside its own body, a recursive call is not a fresh instantiation.
		DefiningGenericGuard defining_guard(*this, def.m_name.m_lexeme);
		ExpectedTypeGuard expected_expr_guard(*this, function.m_return_type);
		return Evaluate(function.m_body)
			.and_then
			(
				[&function, &saved_expected_return_type, prev_constraints_size, this](std::shared_ptr<MidoriType>&& body_type) -> MidoriResult::TypeResult
				{
					m_expected_return_type = saved_expected_return_type;
					m_active_constraints.resize(prev_constraints_size);

					return Unify(function.m_function_keyword, function.m_return_type, body_type, UnifyDiagnosticMode::ExpectedActual)
						.and_then
						(
							[](std::shared_ptr<MidoriType>&&) -> MidoriResult::TypeResult
							{
								return MidoriType::MakeUndecidedType();
							}
						);
				}
			).or_else
			(
				[&saved_expected_return_type, prev_constraints_size, this](CompilerError&& error) -> MidoriResult::TypeResult
				{
					m_expected_return_type = saved_expected_return_type;
					m_active_constraints.resize(prev_constraints_size);
					return std::unexpected(std::move(error));
				}
			);
	});
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriStatement::VariableDefinition& def)
{
	// Special handling for functions (scope management required)
	if (def.m_value->IsExpression<MidoriExpression::Function>())
	{
		MidoriExpression::Function& function = def.m_value->GetExpression<MidoriExpression::Function>();

		// A generic lambda is a definition, not an ordinary initializer: it must be
		// registered by name so each use site is instantiated separately.
		if (!function.m_generic_params.empty())
		{
			return TypeCheckGenericLambdaDefinition(def, function);
		}

		bool has_inferred_param_types = std::ranges::any_of
		(
			function.m_param_types,
			[](const std::shared_ptr<MidoriType>& param_type)
			{
				return param_type->IsType<MidoriType::UndecidedType>();
			}
		);

		if (!def.m_annotated_type.has_value() && has_inferred_param_types)
		{
			return std::unexpected
			(
				MidoriError::GenerateTypeCheckerErrorWithContext
				(
					"Function expression type error: could not infer all lambda parameter or return types",
					function.m_function_keyword,
					m_file_name,
					m_source_lines
				)
			);
		}

		// Freshen any UndecidedType parameters to TypeVariables
		for (std::shared_ptr<MidoriType>& param_type : function.m_param_types)
		{
			param_type = Freshen(param_type);
		}
		function.m_return_type = Freshen(function.m_return_type);

		std::shared_ptr<MidoriType> return_type_copy = function.m_return_type;
		std::shared_ptr<MidoriType> function_type = MidoriType::MakeFunctionType(function.m_param_types, std::move(return_type_copy));
		function.m_type_data = function_type;
		def.m_value->GetType() = function.m_type_data;

		if (def.m_annotated_type.has_value())
		{
			std::shared_ptr<MidoriType>& annotated_type = def.m_annotated_type.value();
			MidoriResult::TypeResult annotation_result = Unify(def.m_name, function_type, annotated_type, UnifyDiagnosticMode::ActualExpected);
			if (!annotation_result.has_value())
			{
				return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Define statement type error: function type annotation doesn't match function signature", def.m_name, m_file_name, m_source_lines, function_type, annotated_type));
			}

			function.m_type_data = ApplySubstitution(function_type);
			def.m_value->GetType() = function.m_type_data;
			const MidoriType::FunctionType& resolved_function_type = function.m_type_data->GetType<MidoriType::FunctionType>();
			function.m_param_types = resolved_function_type.m_param_types;
			function.m_return_type = resolved_function_type.m_return_type;
		}

		std::vector<MidoriType::ClassConstraint> function_constraints = CollectSignatureConstraints(function.m_param_types, function.m_return_type);
		def.m_value->GetType()->GetType<MidoriType::FunctionType>().m_constraints = function_constraints;

		m_name_type_table.back().emplace(def.m_name.m_lexeme, def.m_value->GetType());
		MidoriType::FunctionType& function_type_ref = def.m_value->GetType()->GetType<MidoriType::FunctionType>();

		return ScopeSession(*this).Then([&]() -> MidoriResult::TypeResult
		{
			std::ranges::for_each
			(
				std::views::iota(0u, function_type_ref.m_param_types.size()),
				[&function, &function_type_ref, this](size_t idx) { m_name_type_table.back().emplace(function.m_params[idx].m_lexeme, function_type_ref.m_param_types[idx]); }
			);

			return Evaluate(def.m_value)
				.and_then
				(
					[&def, &function, this](std::shared_ptr<MidoriType>&&) -> MidoriResult::TypeResult
					{
						std::shared_ptr<MidoriType> resolved_function_type = ApplySubstitution(def.m_value->GetType());
						def.m_value->GetType() = resolved_function_type;
						function.m_type_data = resolved_function_type;

						if (resolved_function_type->IsType<MidoriType::FunctionType>())
						{
							const MidoriType::FunctionType& resolved_signature = resolved_function_type->GetType<MidoriType::FunctionType>();
							function.m_param_types = resolved_signature.m_param_types;
							function.m_return_type = resolved_signature.m_return_type;
						}

						if (HasTypeVariables(resolved_function_type))
						{
							return std::unexpected
							(
								MidoriError::GenerateTypeCheckerErrorWithContext
								(
									"Function expression type error: could not infer all lambda parameter or return types",
									function.m_function_keyword,
									m_file_name,
									m_source_lines
								)
							);
						}

						if (std::shared_ptr<MidoriType>* binding = FindNameType(def.m_name.m_lexeme))
						{
							*binding = resolved_function_type;
						}

						return MidoriType::MakeUndecidedType();
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
			[&def, this](std::shared_ptr<MidoriType>&& type)->MidoriResult::TypeResult
			{
				if (def.m_annotated_type.has_value())
				{
					std::shared_ptr<MidoriType>& annotated_type = def.m_annotated_type.value();

					// Reject NeverType values with concrete type annotations
					// NeverType represents expressions that never return (infinite loops, unconditional returns/breaks)
					// They cannot be assigned to variables with concrete types
					if (type->IsType<MidoriType::NeverType>() && !annotated_type->IsType<MidoriType::NeverType>())
					{
						return std::unexpected(
							MidoriError::GenerateTypeCheckerErrorWithContext(
								"Cannot assign a never-returning expression to a variable with type " + annotated_type->ToString(),
								def.m_name,
								m_file_name,
								m_source_lines
							)
						);
					}

					return Unify(def.m_name, annotated_type, type, UnifyDiagnosticMode::ExpectedActual)
						.and_then
						(
							[&def, &annotated_type, this](std::shared_ptr<MidoriType>&& unified_type)->MidoriResult::TypeResult
							{
								m_name_type_table.back().emplace(def.m_name.m_lexeme, annotated_type);
								return unified_type;
							}
						);
				}

				// Check if this is an empty array without type annotation
				if (type->IsType<MidoriType::ArrayType>())
				{
					const MidoriType::ArrayType& array_type = type->GetType<MidoriType::ArrayType>();
					if (array_type.m_element_type->IsType<MidoriType::UndecidedType>())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Define statement type error: empty arrays require type annotations. Use 'def " + def.m_name.m_lexeme + " : Array<ElementType> = [];'", def.m_name, m_file_name, m_source_lines));
					}
				}

				m_name_type_table.back().emplace(def.m_name.m_lexeme, type);
				return MidoriType::MakeUndecidedType();
			}
		);
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriStatement::TupleDefinition& def_tuple)
{
	return Evaluate(def_tuple.m_value)
		.and_then
		(
			[&def_tuple, this](std::shared_ptr<MidoriType>&& type)->MidoriResult::TypeResult
			{
				if (!type->IsType<MidoriType::TupleType>())
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("DefineTuple statement type error: expected tuple expression on right-hand side", def_tuple.m_names[0], m_file_name, m_source_lines));
				}

				const MidoriType::TupleType& tuple_type = type->GetType<MidoriType::TupleType>();


				if (def_tuple.m_names.size() != tuple_type.m_element_types.size())
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("DefineTuple statement type error: tuple pattern has " + std::to_string(def_tuple.m_names.size()) + " bindings but tuple has " + std::to_string(tuple_type.m_element_types.size()) + " elements", def_tuple.m_names[0u], m_file_name, m_source_lines));
				}

				for (size_t i = 0u; i < def_tuple.m_names.size(); i += 1u)
				{
					m_name_type_table.back().emplace(def_tuple.m_names[i].m_lexeme, tuple_type.m_element_types[i]);
				}

				return MidoriType::MakeUndecidedType();
			}
		);
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriStatement::FunctionDefinition& defun)
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
				return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("DefineFunction type error: duplicate generic parameter name", generic_param, m_file_name, m_source_lines));
			}
		}

		// Validate that generic parameters don't conflict with function parameters
		for (const Token& param : defun.m_params)
		{
			if (generic_param_names.contains(param.m_lexeme))
			{
				return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("DefineFunction type error: generic parameter conflicts with function parameter", param, m_file_name, m_source_lines));
			}
		}
	}

	std::vector<MidoriType::ClassConstraint> propagated_constraints = CollectSignatureConstraints(defun.m_param_types, defun.m_return_type);
	for (MidoriType::ClassConstraint& propagated_constraint : propagated_constraints)
	{
		AppendUniqueConstraint(defun.m_constraints, std::move(propagated_constraint));
	}

	for (const MidoriType::ClassConstraint& constraint : defun.m_constraints)
	{
		if (!m_classes.contains(ConstrainedClassName(constraint)))
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("DefineFunction type error: undefined class '" + ConstrainedClassName(constraint) + "' in constraint", defun.m_name, m_file_name, m_source_lines));
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
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("DefineFunction type error: class '" + constraint.m_class_name + "' expects " + std::to_string(tc_info.m_type_param_names.size()) + " type argument(s) but got " + std::to_string(constraint.m_type_args.size()), defun.m_name, m_file_name, m_source_lines));
		}
	}

	FresheningContext freshening_context;
	for (std::shared_ptr<MidoriType>& param_type : defun.m_param_types)
	{
		param_type = Freshen(param_type, freshening_context);
	}
	defun.m_return_type = Freshen(defun.m_return_type, freshening_context);
	for (MidoriType::ClassConstraint& constraint : defun.m_constraints)
	{
		for (std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
		{
			type_arg = Freshen(type_arg, freshening_context);
		}

		// An equality constraint carries its types in its operands rather than in
		// m_type_args, and they must be freshened alongside the signature or the
		// projection will never match the one built at the use site.
		if (constraint.IsEquality())
		{
			constraint.m_equality_lhs = Freshen(constraint.m_equality_lhs, freshening_context);
			constraint.m_equality_rhs = Freshen(constraint.m_equality_rhs, freshening_context);
		}
	}

	std::shared_ptr<MidoriType> return_type_copy = defun.m_return_type;
	std::shared_ptr<MidoriType> function_type = MidoriType::MakeFunctionType(defun.m_param_types, std::move(return_type_copy));
	function_type->GetType<MidoriType::FunctionType>().m_constraints = defun.m_constraints;
	m_name_type_table.back()[defun.m_name.m_lexeme] = function_type;
	if (!defun.m_generic_params.empty())
	{
		m_generic_functions.insert(defun.m_name.m_lexeme);
	}

	return ScopeSession(*this).Then([&]() -> MidoriResult::TypeResult
	{
		for (const Token& generic_param : defun.m_generic_params)
		{
			const std::string& param_name = generic_param.m_lexeme;
			TypeEnvironment::iterator it = freshening_context.m_generic_params.find(param_name);
			if (it == freshening_context.m_generic_params.end())
			{
				std::shared_ptr<MidoriType> fresh_var = FreshTypeVar();
				it = freshening_context.m_generic_params.emplace(param_name, std::move(fresh_var)).first;
			}
			m_name_type_table.back().emplace(param_name, it->second);
		}
		std::ranges::for_each
		(
			std::views::iota(0u, defun.m_params.size()),
			[&defun, this](size_t idx) 
			{
				m_name_type_table.back().emplace(defun.m_params[idx].m_lexeme, defun.m_param_types[idx]);
			}
		);

		std::shared_ptr<MidoriType> saved_expected_return_type = m_expected_return_type;
		m_expected_return_type = defun.m_return_type;

		size_t prev_constraints_size = m_active_constraints.size();

		for (const MidoriType::ClassConstraint& constraint : defun.m_constraints)
		{
			if (!ContainsConstraint(m_active_constraints, constraint))
			{
				m_active_constraints.push_back(constraint);
			}
		}

		ExpectedTypeGuard expected_expr_guard(*this, defun.m_return_type);
		return Evaluate(defun.m_body)
			.and_then
			(
				[&defun, &saved_expected_return_type, prev_constraints_size, this](std::shared_ptr<MidoriType>&& function_return_value_type) ->MidoriResult::TypeResult
				{
					m_expected_return_type = saved_expected_return_type;
					m_active_constraints.resize(prev_constraints_size);

					return Unify(defun.m_name, defun.m_return_type, function_return_value_type, UnifyDiagnosticMode::ExpectedActual)
						.and_then
						(
							[&defun](std::shared_ptr<MidoriType>&&) -> MidoriResult::TypeResult
							{
								return defun.m_return_type;
							}
						);
				}
			).or_else
			(
				[&saved_expected_return_type, prev_constraints_size, this](CompilerError&& error) -> MidoriResult::TypeResult
				{
					m_expected_return_type = saved_expected_return_type; 
					m_active_constraints.resize(prev_constraints_size);
					return std::unexpected(std::move(error));
				}
			);
	});
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriStatement::ForeignDefinition& foreign)
{
	m_name_type_table.back()[foreign.m_function_name.m_lexeme] = foreign.m_type;
	return MidoriType::MakeUndecidedType();
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriStatement::Struct& struct_stmt)
{
	if (!struct_stmt.m_generic_params.empty())
	{
		std::unordered_set<std::string> generic_param_names;
		for (const Token& generic_param : struct_stmt.m_generic_params)
		{
			if (!generic_param_names.insert(generic_param.m_lexeme).second)
			{
				return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Struct declaration type error: duplicate generic parameter name", generic_param, m_file_name, m_source_lines));
			}
		}

		m_generic_structs.insert(struct_stmt.m_name.m_lexeme);
	}

	if (!struct_stmt.m_constraints.empty())
	{
		if (struct_stmt.m_generic_params.empty())
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Struct declaration error: constraints require at least one type parameter", struct_stmt.m_name, m_file_name, m_source_lines));
		}

		for (const MidoriType::ClassConstraint& constraint : struct_stmt.m_constraints)
		{
			if (!m_classes.contains(ConstrainedClassName(constraint)))
			{
				return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Struct declaration error: undefined class '" + ConstrainedClassName(constraint) + "' in constraint", struct_stmt.m_name, m_file_name, m_source_lines));
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
				return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Struct declaration error: class '" + constraint.m_class_name + "' expects " + std::to_string(tc_info.m_type_param_names.size()) + " type argument(s) but got " + std::to_string(constraint.m_type_args.size()), struct_stmt.m_name, m_file_name, m_source_lines));
			}
		}
	}

	struct_stmt.m_self_type->GetType<MidoriType::StructType>().m_constraints = struct_stmt.m_constraints;
	std::shared_ptr<MidoriType> struct_constructor_type = MidoriType::MakeFunctionType(struct_stmt.m_self_type->GetType<MidoriType::StructType>().m_member_types, std::move(struct_stmt.m_self_type));
	struct_constructor_type->GetType<MidoriType::FunctionType>().m_constraints = struct_stmt.m_constraints;
	m_name_type_table.back()[struct_stmt.m_name.m_lexeme] = struct_constructor_type;
	m_struct_type_definitions[struct_stmt.m_name.m_lexeme] = struct_constructor_type->GetType<MidoriType::FunctionType>().m_return_type;

	return MidoriType::MakeUndecidedType();
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriStatement::Union& union_stmt)
{
	if (!union_stmt.m_generic_params.empty())
	{
		std::unordered_set<std::string> generic_param_names;
		for (const Token& generic_param : union_stmt.m_generic_params)
		{
			if (!generic_param_names.insert(generic_param.m_lexeme).second)
			{
				return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Union declaration type error: duplicate generic parameter name", generic_param, m_file_name, m_source_lines));
			}
		}

		m_generic_unions.insert(union_stmt.m_name.m_lexeme);
	}

	if (!union_stmt.m_constraints.empty())
	{
		if (union_stmt.m_generic_params.empty())
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Union declaration error: constraints require at least one type parameter", union_stmt.m_name, m_file_name, m_source_lines));
		}

		for (const MidoriType::ClassConstraint& constraint : union_stmt.m_constraints)
		{
			if (!m_classes.contains(ConstrainedClassName(constraint)))
			{
				return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Union declaration error: undefined class '" + ConstrainedClassName(constraint) + "' in constraint", union_stmt.m_name, m_file_name, m_source_lines));
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
				return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Union declaration error: class '" + constraint.m_class_name + "' expects " + std::to_string(tc_info.m_type_param_names.size()) + " type argument(s) but got " + std::to_string(constraint.m_type_args.size()), union_stmt.m_name, m_file_name, m_source_lines));
			}
		}
	}

	MidoriType::UnionType& union_type = union_stmt.m_self_type->GetType<MidoriType::UnionType>();
	union_type.m_constraints = union_stmt.m_constraints;
	for (auto& [member_name, member_ctx] : union_type.m_member_info)
	{
		std::vector<std::shared_ptr<MidoriType>> member_types_copy = member_ctx.m_member_types;
		std::shared_ptr<MidoriType> union_constructor_type = MidoriType::MakeFunctionType(std::move(member_types_copy), std::shared_ptr(union_stmt.m_self_type));
		union_constructor_type->GetType<MidoriType::FunctionType>().m_constraints = union_stmt.m_constraints;
		m_name_type_table.back()[member_name] = union_constructor_type;
	}
	m_union_type_definitions[union_stmt.m_name.m_lexeme] = union_stmt.m_self_type;

	return MidoriType::MakeUndecidedType();
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriStatement::Class& class_stmt)
{
	std::unordered_set<std::string> type_param_names;
	for (const Token& type_param : class_stmt.m_type_params)
	{
		if (!type_param_names.insert(type_param.m_lexeme).second)
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Class declaration error: duplicate type parameter name", type_param, m_file_name, m_source_lines));
		}
	}

	if (m_classes.contains(class_stmt.m_name.m_lexeme))
	{
		return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Class declaration error: class already defined", class_stmt.m_name, m_file_name, m_source_lines));
	}

	AssociatedTypeEnvironment associated_types;
	for (const MidoriStatement::Class::AssociatedTypeDeclaration& associated_type_decl : class_stmt.m_associated_types)
	{
		if (type_param_names.contains(associated_type_decl.m_name.m_lexeme))
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Class declaration error: associated type name conflicts with class type parameter", associated_type_decl.m_name, m_file_name, m_source_lines));
		}
		if (associated_types.contains(associated_type_decl.m_name.m_lexeme))
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Class declaration error: duplicate associated type name", associated_type_decl.m_name, m_file_name, m_source_lines));
		}

		std::vector<std::shared_ptr<MidoriType>> associated_type_args;
		associated_type_args.reserve(class_stmt.m_type_params.size());
		for (const Token& type_param : class_stmt.m_type_params)
		{
			associated_type_args.emplace_back(MidoriType::MakeGenericType(type_param.m_lexeme));
		}

		associated_types.emplace
		(
			associated_type_decl.m_name.m_lexeme,
			MidoriType::MakeAssociatedType(class_stmt.m_name.m_lexeme, associated_type_decl.m_name.m_lexeme, std::move(associated_type_args))
		);
	}

	TypeEnvironment method_types;
	std::unordered_set<std::string> methods_with_defaults;
	for (std::unique_ptr<MidoriStatement>& method : class_stmt.m_methods)
	{
		if (!method->IsStatement<MidoriStatement::FunctionDefinition>())
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Class declaration error: methods must be function definitions", class_stmt.m_name, m_file_name, m_source_lines));
		}

		MidoriStatement::FunctionDefinition& defun = method->GetStatement<MidoriStatement::FunctionDefinition>();
		if (method_types.contains(defun.m_name.m_lexeme))
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Class declaration error: duplicate method name", defun.m_name, m_file_name, m_source_lines));
		}

		std::shared_ptr<MidoriType> method_type = MidoriType::MakeFunctionType(defun.m_param_types, std::shared_ptr<MidoriType>(defun.m_return_type));

		method_types[defun.m_name.m_lexeme] = method_type;
	}

	std::vector<std::string> param_names;
	for (const Token& param : class_stmt.m_type_params)
	{
		param_names.emplace_back(param.m_lexeme);
	}

	m_classes[class_stmt.m_name.m_lexeme] = ClassInfo(class_stmt.m_name.m_lexeme, std::move(param_names), std::vector<MidoriType::ClassConstraint>(class_stmt.m_superclasses), std::move(associated_types), std::move(method_types), std::move(methods_with_defaults));

	return MidoriType::MakeUndecidedType();
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriStatement::Instance& instance_stmt)
{
	std::unordered_map<std::string, ClassInfo>::iterator tc_it = m_classes.find(instance_stmt.m_class_name.m_lexeme);
	if (tc_it == m_classes.end())
	{
		return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Instance declaration error: unknown class '" + instance_stmt.m_class_name.m_lexeme + "'", instance_stmt.m_class_name, m_file_name, m_source_lines));
	}

	const ClassInfo& tc_info = tc_it->second;
	if (instance_stmt.m_type_args.size() != tc_info.m_type_param_names.size())
	{
		return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Instance declaration error: type argument count mismatch", instance_stmt.m_class_name, m_file_name, m_source_lines));
	}

	std::vector<std::string> concrete_type_names;
	for (const std::shared_ptr<MidoriType>& type_arg : instance_stmt.m_type_args)
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
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Instance declaration error: instance already defined for this type", instance_stmt.m_class_name, m_file_name, m_source_lines));
		}

		m_instances.erase(existing_instance_it);
	}

	AssociatedTypeEnvironment associated_type_bindings;
	for (const MidoriStatement::Instance::AssociatedTypeBinding& binding : instance_stmt.m_associated_types)
	{
		if (!tc_info.m_associated_types.contains(binding.m_name.m_lexeme))
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Instance declaration error: unknown associated type '" + binding.m_name.m_lexeme + "'", binding.m_name, m_file_name, m_source_lines));
		}
		if (associated_type_bindings.contains(binding.m_name.m_lexeme))
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Instance declaration error: duplicate associated type binding", binding.m_name, m_file_name, m_source_lines));
		}

		associated_type_bindings.emplace(binding.m_name.m_lexeme, binding.m_type);
	}

	for (const auto& [associated_type_name, _] : tc_info.m_associated_types)
	{
		if (!associated_type_bindings.contains(associated_type_name))
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Instance declaration error: missing binding for associated type '" + associated_type_name + "'", instance_stmt.m_class_name, m_file_name, m_source_lines));
		}
	}

	m_instances.emplace
	(
		instance_key,
		InstanceInfo
		(
			instance_stmt.m_class_name.m_lexeme,
			std::vector<std::shared_ptr<MidoriType>>(instance_stmt.m_type_args),
			std::vector<MidoriType::ClassConstraint>(instance_stmt.m_constraints),
			AssociatedTypeEnvironment(associated_type_bindings),
			std::unordered_map<std::string, std::unique_ptr<MidoriStatement>>()
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
	for (std::unique_ptr<MidoriStatement>& method : instance_stmt.m_methods)
	{
		if (!method->IsStatement<MidoriStatement::FunctionDefinition>())
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Instance declaration error: methods must be function definitions", instance_stmt.m_class_name, m_file_name, m_source_lines));
		}

		MidoriStatement::FunctionDefinition& defun = method->GetStatement<MidoriStatement::FunctionDefinition>();
		std::string mangled_name = defun.m_name.m_lexeme;
		std::string method_name = MidoriType::DemangleInstanceMethodName(mangled_name, instance_stmt.m_class_name.m_lexeme);

		if (!tc_info.m_method_types.contains(method_name))
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Instance declaration error: method '" + method_name + "' not defined in typeclass", defun.m_name, m_file_name, m_source_lines));
		}

		if (implemented_methods.contains(method_name))
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Instance declaration error: duplicate method implementation", defun.m_name, m_file_name, m_source_lines));
		}

		implemented_methods.emplace(method_name);
	}

	for (const auto& [method_name, method_type] : tc_info.m_method_types)
	{
		if (!implemented_methods.contains(method_name))
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Instance declaration error: missing implementation for method '" + method_name + "'", instance_stmt.m_class_name, m_file_name, m_source_lines));
		}
	}

	TypeEnvironment type_param_substitutions;
	for (size_t i = 0u; i < tc_info.m_type_param_names.size(); i += 1u)
	{
		type_param_substitutions.emplace(tc_info.m_type_param_names[i], instance_stmt.m_type_args[i]);
	}

	for (const std::unique_ptr<MidoriStatement>& method : instance_stmt.m_methods)
	{
		const MidoriStatement::FunctionDefinition& defun = method->GetStatement<MidoriStatement::FunctionDefinition>();
		const std::string mangled_name = defun.m_name.m_lexeme;
		const std::string method_name = MidoriType::DemangleInstanceMethodName(mangled_name, instance_stmt.m_class_name.m_lexeme);

		if (!defun.m_generic_params.empty())
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Instance declaration error: instance methods cannot declare generic parameters", defun.m_name, m_file_name, m_source_lines));
		}

		TypeEnvironment::const_iterator expected_it = tc_info.m_method_types.find(method_name);
		if (expected_it == tc_info.m_method_types.cend())
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Instance declaration error: method '" + method_name + "' not defined in class", defun.m_name, m_file_name, m_source_lines));
		}

		std::shared_ptr<MidoriType> expected_signature = ApplySubstitution(MidoriType::SubstituteTypeParams(expected_it->second, type_param_substitutions));
		std::shared_ptr<MidoriType> actual_signature = ApplySubstitution(MidoriType::MakeFunctionType(defun.m_param_types, std::shared_ptr<MidoriType>(defun.m_return_type)));

		if (*expected_signature != *actual_signature)
		{
			if (expected_signature->IsType<MidoriType::FunctionType>() && actual_signature->IsType<MidoriType::FunctionType>())
			{
				const MidoriType::FunctionType& expected_type = expected_signature->GetType<MidoriType::FunctionType>();
				const MidoriType::FunctionType& actual_type = actual_signature->GetType<MidoriType::FunctionType>();

				if (expected_type.m_param_types.size() != actual_type.m_param_types.size())
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Instance declaration error: method '" + method_name + "' parameter count does not match class declaration", defun.m_name, m_file_name, m_source_lines, actual_signature, expected_signature));
				}

				for (size_t i = 0u; i < expected_type.m_param_types.size(); i += 1u)
				{
					const std::shared_ptr<MidoriType>& expected_param = expected_type.m_param_types[i];
					const std::shared_ptr<MidoriType>& actual_param = actual_type.m_param_types[i];
					if (*expected_param != *actual_param)
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(std::format("Instance declaration error: method '{}' parameter {} type does not match class declaration", method_name, i + 1u), defun.m_name, m_file_name, m_source_lines, actual_param, expected_param));
					}
				}

				if (*expected_type.m_return_type != *actual_type.m_return_type)
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Instance declaration error: method '" + method_name + "' return type does not match class declaration", defun.m_name, m_file_name, m_source_lines, actual_type.m_return_type, expected_type.m_return_type));
				}
			}

			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Instance declaration error: method '" + method_name + "' signature does not match class declaration", defun.m_name, m_file_name, m_source_lines, actual_signature, expected_signature));
		}
	}

	// AppendUniqueConstraint does NOT make this append idempotent: on a second visit the stored
	// constraint has been freshened in place by the function-definition path while the incoming one has not,
	// so they compare unequal and a duplicate would accumulate. Safety comes instead from this
	// statement being visited at most once - TypeCheck traverses m_program_tree in a single pass,
	// the duplicate-instance guard above rejects a repeated instance key before reaching here,
	// and each module gets its own TypeChecker owning its own tree (imports carry instance
	// metadata, not Instance AST nodes). Preserve that property before reusing this loop.
	for (std::unique_ptr<MidoriStatement>& method : instance_stmt.m_methods)
	{
		MidoriStatement::FunctionDefinition& defun = method->GetStatement<MidoriStatement::FunctionDefinition>();
		for (const MidoriType::ClassConstraint& constraint : instance_stmt.m_constraints)
		{
			AppendUniqueConstraint(defun.m_constraints, MidoriType::ClassConstraint(constraint));
		}

		MidoriResult::TypeResult result = Evaluate(method);
		if (!result.has_value())
		{
			return result;
		}
	}

	registration_guard.m_keep = true;
	return MidoriType::MakeUndecidedType();
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriStatement::TypeAlias& type_alias)
{
	if (!type_alias.m_generic_params.empty())
	{
		std::unordered_set<std::string> generic_param_names;
		for (const Token& generic_param : type_alias.m_generic_params)
		{
			if (!generic_param_names.insert(generic_param.m_lexeme).second)
			{
				return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Type alias declaration error: duplicate generic parameter name", generic_param, m_file_name, m_source_lines));
			}
		}
	}

	if (type_alias.m_aliased_type->IsType<MidoriType::NewType>())
	{
		const MidoriType::NewType& new_type = type_alias.m_aliased_type->GetType<MidoriType::NewType>();
		RegisterIdentityConversion(type_alias.m_aliased_type, new_type.m_representation);
		RegisterIdentityConversion(new_type.m_representation, type_alias.m_aliased_type);
	}

	// Type alias is already registered in the parser's type table
	// Nothing more to do at type-checking time - the alias is resolved at parse time
	return MidoriType::MakeUndecidedType();
}

void TypeChecker::RegisterIdentityConversion(const std::shared_ptr<MidoriType>& from_type, const std::shared_ptr<MidoriType>& to_type)
{
	InstanceKey conversion_key{ std::string(CONVERTABLE_CLASS_NAME), { from_type->ToString(), to_type->ToString() } };
	if (m_instances.contains(conversion_key))
	{
		return;
	}

	std::vector<std::shared_ptr<MidoriType>> type_args{ from_type, to_type };
	m_instances.emplace
	(
		std::move(conversion_key),
		InstanceInfo(std::string(CONVERTABLE_CLASS_NAME), std::move(type_args), {}, {}, {}, true)
	);
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::Match& match)
{
	return Evaluate(match.m_arg_expr)
		.and_then
		(
			[&match, this](std::shared_ptr<MidoriType>&& arg_type) -> MidoriResult::TypeResult
			{
				std::shared_ptr<MidoriType> resolved_arg_type = ApplySubstitution(arg_type);
				bool is_union = resolved_arg_type->IsType<MidoriType::UnionType>();
				bool is_bool = resolved_arg_type->IsType<MidoriType::BoolType>();

				std::unordered_set<std::string> missing_cases;
				if (is_union)
				{
					const MidoriType::UnionType& union_type = resolved_arg_type->GetType<MidoriType::UnionType>();
					for (const auto& [member_name, member_ctx] : union_type.m_member_info)
					{
						missing_cases.insert(member_name);
					}
				}
				else if (is_bool)
				{
					missing_cases.emplace("true");
					missing_cases.emplace("false");
				}

				bool has_catch_all_case = false;
				std::shared_ptr<MidoriType> prev_case_type = nullptr;

				for (const std::unique_ptr<MidoriExpression>& case_expr : match.m_cases)
				{
					ScopeSession scope(*this);
					std::optional<CompilerError> error;
					MidoriResult::TypeResult case_result;

					if (case_expr->IsExpression<MidoriExpression::Case>())
					{
						MidoriExpression::Case& match_case = case_expr->GetExpression<MidoriExpression::Case>();
						MidoriResult::TypeResult pattern_result = CheckPattern(*match_case.m_pattern, resolved_arg_type);
						if (!pattern_result.has_value())
						{
							error = std::move(pattern_result.error());
						}
						else if (match_case.HasGuard())
						{
							error = CheckCaseGuard(match_case);
						}

						if (!error.has_value() && !match_case.HasGuard())
						{
							if (is_union && match_case.m_pattern->IsPattern<MidoriPattern::Constructor>())
							{
								const MidoriPattern::Constructor& ctor = match_case.m_pattern->GetPattern<MidoriPattern::Constructor>();
								if (ctor.m_is_union)
								{
									missing_cases.erase(ctor.m_name);
								}
							}
							else if (is_bool && match_case.m_pattern->IsPattern<MidoriPattern::Literal>())
							{
								const MidoriPattern::Literal& literal = match_case.m_pattern->GetPattern<MidoriPattern::Literal>();
								if (literal.m_kind == MidoriPattern::LiteralKind::Bool)
								{
									if (literal.m_token.m_token_name == Token::Name::TRUE)
									{
										missing_cases.erase("true");
									}
									else if (literal.m_token.m_token_name == Token::Name::FALSE)
									{
										missing_cases.erase("false");
									}
								}
							}

							if (IsIrrefutablePattern(*match_case.m_pattern, resolved_arg_type))
							{
								has_catch_all_case = true;
								missing_cases.clear();
							}
						}

						if (!error.has_value())
						{
							case_result = Evaluate(match_case.m_expr);
						}
					}

					if (error.has_value())
					{
						return std::unexpected(std::move(error.value()));
					}
					else if (!case_result.has_value())
					{
						return std::unexpected(std::move(case_result.error()));
					}
					else
					{
						std::shared_ptr<MidoriType> resolved_case_type = ApplySubstitution(case_result.value());
						if (prev_case_type == nullptr)
						{
							prev_case_type = resolved_case_type;
						}
						else
						{
							std::shared_ptr<MidoriType> resolved_prev_case_type = ApplySubstitution(prev_case_type);
							MidoriResult::TypeResult unify_result = Unify(match.m_match_keyword, resolved_prev_case_type, resolved_case_type);
							if (!unify_result.has_value())
							{
								return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, "Match expression type error: case types do not match", match.m_match_keyword, m_file_name, m_source_lines, resolved_prev_case_type, resolved_case_type));
							}

							prev_case_type = ApplySubstitution(resolved_prev_case_type);
						}
					}
				}

				if (is_union || is_bool)
				{
					if (!missing_cases.empty())
					{
						std::vector<std::string> missing_names(missing_cases.begin(), missing_cases.end());
						const std::string missing_label = is_union ? "variants" : "cases";
						const std::string message = std::format("Match expression type error: non-exhaustive match: missing {}: {}", missing_label, JoinSortedNames(std::move(missing_names)));
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeNonExhaustiveMatch, message, match.m_match_keyword, m_file_name, m_source_lines));
					}
				}
				else
				{
					if (!has_catch_all_case)
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeNonExhaustiveMatch, "Match expression type error: non-union matches need a catch-all arm such as 'case _ =>'",match.m_match_keyword, m_file_name, m_source_lines));
					}
				}

				match.m_type_data = prev_case_type;
				return match.m_type_data;
			}
		);
}

std::optional<CompilerError> TypeChecker::CheckCaseGuard(MidoriExpression::Case& case_expr)
{
	MidoriResult::TypeResult guard_result = Evaluate(case_expr.m_guard.value());
	if (!guard_result.has_value())
	{
		return std::move(guard_result.error());
	}

	std::shared_ptr<MidoriType> bool_type = MidoriType::MakeLiteralType<MidoriType::BoolType>();
	std::shared_ptr<MidoriType> resolved_guard_type = ApplySubstitution(guard_result.value());
	if (!Unify(case_expr.m_keyword, bool_type, resolved_guard_type).has_value())
	{
		return MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, "Match expression type error: case guard must be of type Bool", case_expr.m_keyword, m_file_name, m_source_lines, resolved_guard_type, bool_type);
	}

	return std::nullopt;
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::Case& case_expr)
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
			[&case_expr, this](std::shared_ptr<MidoriType>&& expr_type) ->MidoriResult::TypeResult
			{
				return Unify(case_expr.m_keyword, case_expr.m_type_data, expr_type);
			}
		);
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::For& for_expr)
{
	return Evaluate(for_expr.m_range)
		.and_then
		(
			[&for_expr, this](std::shared_ptr<MidoriType>&& range_type)->MidoriResult::TypeResult
			{
				std::shared_ptr<MidoriType> resolved_range = ApplySubstitution(range_type);

				std::shared_ptr<MidoriType> element_type;

				if (resolved_range->IsType<MidoriType::RangeType>())
				{
					element_type = resolved_range->GetType<MidoriType::RangeType>().m_element_type;
					for_expr.m_is_array_iteration = false;
				}
				else if (resolved_range->IsType<MidoriType::ArrayType>())
				{
					element_type = resolved_range->GetType<MidoriType::ArrayType>().m_element_type;
					for_expr.m_is_array_iteration = true;
					for_expr.m_is_iterable_iteration = false;
				}
				else
				{
					std::shared_ptr<MidoriType> iterable_item_type;
					int iterable_some_tag = -1;
					bool has_iterable_instance = false;
					bool has_iterable_constraint = false;
					bool range_has_type_vars = false;

					{
						std::unordered_set<const MidoriType*> visited;
						range_has_type_vars = HasTypeVariables(resolved_range, visited);
					}

					std::unordered_map<std::string, ClassInfo>::iterator class_it = m_classes.find(std::string(ITERABLE_CLASS_NAME));
					if (class_it == m_classes.end())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("For loop expression type error: Iterable class not found", for_expr.m_in_keyword, m_file_name, m_source_lines, resolved_range));
					}

					const bool uses_associated_item = class_it->second.m_associated_types.contains("Item");

					if (!range_has_type_vars)
					{
						if (uses_associated_item)
						{
							std::optional<ResolvedInstanceMatch> resolved_match = FindMatchingInstance(std::string(ITERABLE_CLASS_NAME), { resolved_range });
							if (resolved_match.has_value())
							{
								AssociatedTypeEnvironment::const_iterator binding_it = resolved_match->m_instance->m_associated_type_bindings.find("Item");
								if (binding_it != resolved_match->m_instance->m_associated_type_bindings.cend())
								{
									iterable_item_type = ApplySubstitution(MidoriType::SubstituteTypeParams(binding_it->second, resolved_match->m_substitutions));
									has_iterable_instance = true;
								}
							}
						}
						else
						{
							for (const auto& [key, info] : m_instances)
							{
								if (info.m_class_name == ITERABLE_CLASS_NAME && info.m_type_args.size() == 2u)
								{
									std::unordered_map<std::string, std::shared_ptr<MidoriType>> substitutions;
									std::unordered_set<std::pair<MidoriType*, MidoriType*>, TypePairHash> visited;
									if (!MatchInstanceTypeArg(info.m_type_args[0u], resolved_range, substitutions, visited))
									{
										continue;
									}

									std::shared_ptr<MidoriType> candidate_item_type = MidoriType::SubstituteTypeParams(info.m_type_args[1u], substitutions);
									if (has_iterable_instance)
									{
										return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("For loop expression type error: ambiguous Iterable instance for iterator type", for_expr.m_in_keyword, m_file_name, m_source_lines, resolved_range));
									}
									iterable_item_type = candidate_item_type;
									has_iterable_instance = true;
								}
							}
						}
					}

					if (!has_iterable_instance)
					{
						for (const MidoriType::ClassConstraint& constraint : m_active_constraints)
						{
							if (constraint.m_class_name == ITERABLE_CLASS_NAME && constraint.m_type_args.size() == class_it->second.m_type_param_names.size() && !constraint.m_type_args.empty() && *constraint.m_type_args[0u] == *resolved_range)
							{
								if (uses_associated_item)
								{
									iterable_item_type = ResolveAssociatedType(MidoriType::AssociatedType(std::string(ITERABLE_CLASS_NAME), "Item", std::vector<std::shared_ptr<MidoriType>>(constraint.m_type_args)));
								}
								else if (constraint.m_type_args.size() >= 2u)
								{
									iterable_item_type = constraint.m_type_args[1u];
								}
								has_iterable_constraint = true;
								break;
							}
						}
					}

					if (!has_iterable_instance && !has_iterable_constraint)
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("For loop expression type error: expected Range, Array, or Iterable type for iteration", for_expr.m_in_keyword, m_file_name, m_source_lines, resolved_range, MidoriType::MakeRangeType(MidoriType::MakeLiteralType<MidoriType::IntegerType>())));
					}

					std::unordered_map<std::string, std::shared_ptr<MidoriType>>::iterator method_it = class_it->second.m_method_types.find(std::string(NEXT_METHOD_NAME));
					if (method_it == class_it->second.m_method_types.end())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("For loop expression type error: Iterable::Next not found", for_expr.m_in_keyword, m_file_name, m_source_lines, resolved_range));
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

					std::shared_ptr<MidoriType> next_type = ApplySubstitution(MidoriType::SubstituteTypeParams(method_it->second, substitutions));
					if (!next_type->IsType<MidoriType::FunctionType>())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("For loop expression type error: Iterable::Next must be a function", for_expr.m_in_keyword, m_file_name, m_source_lines, resolved_range));
					}

					std::shared_ptr<MidoriType> next_return = ApplySubstitution(next_type->GetType<MidoriType::FunctionType>().m_return_type);
					if (!next_return->IsType<MidoriType::UnionType>())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("For loop expression type error: Iterable::Next must return an Option-like union", for_expr.m_in_keyword, m_file_name, m_source_lines, next_return));
					}

					const MidoriType::UnionType& option_union = next_return->GetType<MidoriType::UnionType>();
					std::string some_name = option_union.m_name + std::string(NameSeparator) + "Some";
					if (!option_union.m_member_info.contains(some_name))
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("For loop expression type error: Iterable::Next return type is missing 'Some' constructor", for_expr.m_in_keyword, m_file_name, m_source_lines, next_return));
					}

					const MidoriType::UnionType::UnionMemberContext& some_ctx = option_union.m_member_info.at(some_name);
					iterable_some_tag = some_ctx.m_tag;
					if (some_ctx.m_member_types.size() != 1u)
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("For loop expression type error: Iterable::Next 'Some' constructor must contain exactly one value", for_expr.m_in_keyword, m_file_name, m_source_lines, next_return));
					}

					// Next returns Option<(Item, Iter)>. The payload is a pair whose first
					// element is the item and whose second is the advanced iterator, so the
					// loop variable takes element 0 rather than the payload itself.
					std::shared_ptr<MidoriType> some_payload = ApplySubstitution(some_ctx.m_member_types[0u]);
					if (!some_payload->IsType<MidoriType::TupleType>() || some_payload->GetType<MidoriType::TupleType>().m_element_types.size() != 2u)
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("For loop expression type error: Iterable::Next must return Option<(Item, Iter)>", for_expr.m_in_keyword, m_file_name, m_source_lines, some_payload));
					}

					iterable_item_type = ApplySubstitution(some_payload->GetType<MidoriType::TupleType>().m_element_types[0u]);
					element_type = iterable_item_type;
					for_expr.m_is_array_iteration = false;
					for_expr.m_is_iterable_iteration = true;
					for_expr.m_iterable_item_type = iterable_item_type;
					for_expr.m_iterable_some_tag = iterable_some_tag;
				}

				ScopeSession scope(*this);

				std::string var_name(for_expr.m_loop_variable.m_lexeme);
				m_name_type_table.back()[var_name] = element_type;

				return Evaluate(for_expr.m_body)
					.and_then
					(
						[&for_expr](std::shared_ptr<MidoriType>&&) -> MidoriResult::TypeResult
						{
							// A `for` loop consumes an iterable for its effects, so it is Unit.
							// It used to take its type from whatever `break` carried out of it.
							for_expr.m_type_data = MidoriType::MakeLiteralType<MidoriType::UnitType>();
							return for_expr.m_type_data;
						}
					);
			}
		);
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::ArrayComprehension& comp)
{
	return Evaluate(comp.m_range)
		.and_then
		(
			[&comp, this](std::shared_ptr<MidoriType>&& range_type) -> MidoriResult::TypeResult
			{
				std::shared_ptr<MidoriType> resolved_range = ApplySubstitution(range_type);

				std::shared_ptr<MidoriType> element_type;

				if (resolved_range->IsType<MidoriType::RangeType>())
				{
					element_type = resolved_range->GetType<MidoriType::RangeType>().m_element_type;
					comp.m_is_array_iteration = false;
				}
				else if (resolved_range->IsType<MidoriType::ArrayType>())
				{
					element_type = resolved_range->GetType<MidoriType::ArrayType>().m_element_type;
					comp.m_is_array_iteration = true;
					comp.m_is_iterable_iteration = false;
				}
				else
				{
					std::shared_ptr<MidoriType> iterable_item_type;
					int iterable_some_tag = -1;
					bool has_iterable_instance = false;
					bool has_iterable_constraint = false;
					bool range_has_type_vars = false;

					{
						std::unordered_set<const MidoriType*> visited;
						range_has_type_vars = HasTypeVariables(resolved_range, visited);
					}

					std::unordered_map<std::string, ClassInfo>::iterator class_it = m_classes.find(std::string(ITERABLE_CLASS_NAME));
					if (class_it == m_classes.end())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Array comprehension type error: Iterable class not found", comp.m_in_keyword, m_file_name, m_source_lines, resolved_range));
					}

					const bool uses_associated_item = class_it->second.m_associated_types.contains("Item");

					if (!range_has_type_vars)
					{
						if (uses_associated_item)
						{
							std::optional<ResolvedInstanceMatch> resolved_match = FindMatchingInstance(std::string(ITERABLE_CLASS_NAME), { resolved_range });
							if (resolved_match.has_value())
							{
								AssociatedTypeEnvironment::const_iterator binding_it = resolved_match->m_instance->m_associated_type_bindings.find("Item");
								if (binding_it != resolved_match->m_instance->m_associated_type_bindings.cend())
								{
									iterable_item_type = ApplySubstitution(MidoriType::SubstituteTypeParams(binding_it->second, resolved_match->m_substitutions));
									has_iterable_instance = true;
								}
							}
						}
						else
						{
							for (const auto& [key, info] : m_instances)
							{
								if (info.m_class_name == ITERABLE_CLASS_NAME && info.m_type_args.size() == 2u)
								{
									std::unordered_map<std::string, std::shared_ptr<MidoriType>> substitutions;
									std::unordered_set<std::pair<MidoriType*, MidoriType*>, TypePairHash> visited;
									if (!MatchInstanceTypeArg(info.m_type_args[0u], resolved_range, substitutions, visited))
									{
										continue;
									}

									std::shared_ptr<MidoriType> candidate_item_type = MidoriType::SubstituteTypeParams(info.m_type_args[1u], substitutions);
									if (has_iterable_instance)
									{
										return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Array comprehension type error: ambiguous Iterable instance for iterator type", comp.m_in_keyword, m_file_name, m_source_lines, resolved_range));
									}
									iterable_item_type = candidate_item_type;
									has_iterable_instance = true;
								}
							}
						}
					}

					if (!has_iterable_instance)
					{
						for (const MidoriType::ClassConstraint& constraint : m_active_constraints)
						{
							if (constraint.m_class_name == ITERABLE_CLASS_NAME && constraint.m_type_args.size() == class_it->second.m_type_param_names.size() && !constraint.m_type_args.empty() && *constraint.m_type_args[0u] == *resolved_range)
							{
								if (uses_associated_item)
								{
									iterable_item_type = ResolveAssociatedType(MidoriType::AssociatedType(std::string(ITERABLE_CLASS_NAME), "Item", std::vector<std::shared_ptr<MidoriType>>(constraint.m_type_args)));
								}
								else if (constraint.m_type_args.size() >= 2u)
								{
									iterable_item_type = constraint.m_type_args[1u];
								}
								has_iterable_constraint = true;
								break;
							}
						}
					}

					if (!has_iterable_instance && !has_iterable_constraint)
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Array comprehension type error: expected Range, Array, or Iterable type for iteration", comp.m_in_keyword, m_file_name, m_source_lines, resolved_range, MidoriType::MakeRangeType(MidoriType::MakeLiteralType<MidoriType::IntegerType>())));
					}

					std::unordered_map<std::string, std::shared_ptr<MidoriType>>::iterator method_it = class_it->second.m_method_types.find(std::string(NEXT_METHOD_NAME));
					if (method_it == class_it->second.m_method_types.end())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Array comprehension type error: Iterable::Next not found", comp.m_in_keyword, m_file_name, m_source_lines, resolved_range));
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

					std::shared_ptr<MidoriType> next_type = ApplySubstitution(MidoriType::SubstituteTypeParams(method_it->second, substitutions));
					if (!next_type->IsType<MidoriType::FunctionType>())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Array comprehension type error: Iterable::Next must be a function", comp.m_in_keyword, m_file_name, m_source_lines, resolved_range));
					}

					std::shared_ptr<MidoriType> next_return = ApplySubstitution(next_type->GetType<MidoriType::FunctionType>().m_return_type);
					if (!next_return->IsType<MidoriType::UnionType>())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Array comprehension type error: Iterable::Next must return an Option-like union", comp.m_in_keyword, m_file_name, m_source_lines, next_return));
					}

					const MidoriType::UnionType& option_union = next_return->GetType<MidoriType::UnionType>();
					std::string some_name = option_union.m_name + std::string(NameSeparator) + "Some";
					if (!option_union.m_member_info.contains(some_name))
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Array comprehension type error: Iterable::Next return type is missing 'Some' constructor", comp.m_in_keyword, m_file_name, m_source_lines, next_return));
					}

					const MidoriType::UnionType::UnionMemberContext& some_ctx = option_union.m_member_info.at(some_name);
					iterable_some_tag = some_ctx.m_tag;
					if (some_ctx.m_member_types.size() != 1u)
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Array comprehension type error: Iterable::Next 'Some' constructor must contain exactly one value", comp.m_in_keyword, m_file_name, m_source_lines, next_return));
					}

					// Next returns Option<(Item, Iter)>. The payload is a pair whose first
					// element is the item and whose second is the advanced iterator, so the
					// loop variable takes element 0 rather than the payload itself.
					std::shared_ptr<MidoriType> some_payload = ApplySubstitution(some_ctx.m_member_types[0u]);
					if (!some_payload->IsType<MidoriType::TupleType>() || some_payload->GetType<MidoriType::TupleType>().m_element_types.size() != 2u)
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Array comprehension type error: Iterable::Next must return Option<(Item, Iter)>", comp.m_in_keyword, m_file_name, m_source_lines, some_payload));
					}

					iterable_item_type = ApplySubstitution(some_payload->GetType<MidoriType::TupleType>().m_element_types[0u]);
					element_type = iterable_item_type;
					comp.m_is_array_iteration = false;
					comp.m_is_iterable_iteration = true;
					comp.m_iterable_item_type = iterable_item_type;
					comp.m_iterable_some_tag = iterable_some_tag;
				}

				ScopeSession scope(*this);

				// Add loop variable to scope with element type
				std::string var_name(comp.m_loop_variable.m_lexeme);
				m_name_type_table.back()[var_name] = element_type;

				// The transform expression produces one element, so its expected type is
				// the expected array's element type, never the array type itself. Leaving
				// the ambient type in place pushes Array<T> into a construction here,
				// which then cannot infer its own arguments.
				const std::shared_ptr<MidoriType> ambient_expected_type = m_expected_expr_type;
				ExpectedTypeGuard transform_guard(*this, (ambient_expected_type != nullptr && ambient_expected_type->IsType<MidoriType::ArrayType>()) ? ambient_expected_type->GetType<MidoriType::ArrayType>().m_element_type : std::shared_ptr<MidoriType>{});

				// Type check the transform expression
				return std::visit
				(
					[this]<typename T>(T&& arg) -> MidoriResult::TypeResult { return (*this)(arg); }, **comp.m_transform_expr)
					.and_then
					(
						[&comp, this](std::shared_ptr<MidoriType>&& transform_type) -> MidoriResult::TypeResult
						{
							// Result type is Array<T> where T is the type of the transform expression
							comp.m_type_data = MidoriType::MakeArrayType(ApplySubstitution(transform_type));
							return comp.m_type_data;
						}
					);
			}
		);
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::As& as)
{
	// A cast's operand is typed on its own terms. The ambient expected type
	// describes the result of the cast, not its source, so leaving it in place
	// pushes the target type into a construction on the left of 'as'.
	ExpectedTypeGuard operand_guard(*this, std::shared_ptr<MidoriType>{});

	return Evaluate(as.m_expr)
		.and_then
		(
			[&as, this](std::shared_ptr<MidoriType>&& expr_type) ->MidoriResult::TypeResult
			{
				// Check for Convertable<From, To> instance
				InstanceKey conversion_key{
					"Convertable",
					{expr_type->ToString(), as.m_to_type->ToString()}
				};

				std::unordered_map<InstanceKey, InstanceInfo, InstanceKeyHash>::iterator instance_it =
					m_instances.find(conversion_key);
				bool has_convertable_instance = (instance_it != m_instances.end());
				bool has_convertable_constraint = false;
				if (!has_convertable_instance)
				{
					for (const MidoriType::ClassConstraint& constraint : m_active_constraints)
					{
						if (constraint.m_class_name == "Convertable"s && constraint.m_type_args.size() == 2 && *constraint.m_type_args[0] == *expr_type && *constraint.m_type_args[1] == *as.m_to_type)
						{
							has_convertable_constraint = true;
							break;
						}
					}
				}

				// Check if this is a built-in conversion
				bool is_builtin_conversion = false;
				if (as.m_to_type->IsType<MidoriType::StructType>() && expr_type->IsType<MidoriType::StructType>())
				{
					if (!has_convertable_instance && !has_convertable_constraint)
					{
						const MidoriType::StructType& from_struct_type = expr_type->GetType<MidoriType::StructType>();
						const MidoriType::StructType& to_struct_type = as.m_to_type->GetType<MidoriType::StructType>();
						if (to_struct_type.m_member_types.size() != from_struct_type.m_member_types.size())
						{
							return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Type cast expression type error: struct member count mismatch", as.m_as_keyword, m_file_name, m_source_lines, expr_type, as.m_to_type));
						}

						for (size_t i : std::views::iota(0u, to_struct_type.m_member_types.size()))
						{
							if (*from_struct_type.m_member_types[i] != *to_struct_type.m_member_types[i])
							{
								return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Type cast expression type error: struct member type mismatch", as.m_as_keyword, m_file_name, m_source_lines, from_struct_type.m_member_types[i], to_struct_type.m_member_types[i]));
							}
						}
						is_builtin_conversion = true;
					}
				}
				else if (as.m_to_type->IsType<MidoriType::StructType>() && !has_convertable_instance && !has_convertable_constraint)
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Type cast expression type error: cannot cast to struct type", as.m_as_keyword, m_file_name, m_source_lines, expr_type, as.m_to_type));
				}

				// Check for built-in primitive conversions
				bool const is_from_int = expr_type->IsType<MidoriType::IntegerType>();
				bool const is_from_float = expr_type->IsType<MidoriType::FloatType>();
				bool const is_from_text = expr_type->IsType<MidoriType::TextType>();
				bool const is_from_byte = expr_type->IsType<MidoriType::ByteType>();
				bool const is_from_word = expr_type->IsType<MidoriType::WordType>();

				bool const is_to_int = as.m_to_type->IsType<MidoriType::IntegerType>();
				bool const is_to_float = as.m_to_type->IsType<MidoriType::FloatType>();
				bool const is_to_text = as.m_to_type->IsType<MidoriType::TextType>();
				bool const is_to_byte = as.m_to_type->IsType<MidoriType::ByteType>();
				bool const is_to_word = as.m_to_type->IsType<MidoriType::WordType>();

				if ((is_from_int && (is_to_float || is_to_text || is_to_byte || is_to_word)) ||
					(is_from_float && (is_to_int || is_to_text || is_to_byte || is_to_word)) ||
					(is_from_text && (is_to_int || is_to_float)) ||
					(is_from_byte && (is_to_int || is_to_float || is_to_word || is_to_text)) ||
					(is_from_word && (is_to_int || is_to_float || is_to_byte || is_to_text)))
				{
					is_builtin_conversion = true;
				}

				// `x as T` where x already has type T is a no-op, so it needs no Convertable instance.
				const bool is_identity_conversion = *ApplySubstitution(expr_type) == *ApplySubstitution(as.m_to_type);

				// Verify that either Convertable instance exists, constraint exists, or it's a built-in conversion
				if (!has_convertable_instance && !has_convertable_constraint && !is_builtin_conversion && !is_identity_conversion)
				{
					MidoriType::ClassConstraint constraint("Convertable", { expr_type, as.m_to_type });
					const std::string suggestion = std::format("Define 'instance Convertable<{}, {}>' to enable this conversion.", expr_type->ToString(), as.m_to_type->ToString());
					return std::unexpected(MakeConstraintFailureError(as.m_as_keyword, constraint, suggestion));
				}

				as.m_from_type = expr_type;
				as.m_type_data = as.m_to_type;

				// Prefer built-in code generation for concrete built-in casts so Convertable
				// instances can implement Convert via `as` without recursing back into themselves.
				// Constraints still require Convertable dispatch because the concrete conversion
				// is only known during specialization.
				as.m_uses_convertable = has_convertable_constraint || (has_convertable_instance && !is_builtin_conversion);
				return as.m_type_data;
			}
		);
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::Binary& binary)
{
	return Evaluate(binary.m_left)
		.and_then
		(
			[&binary, this](std::shared_ptr<MidoriType>&& left_type) ->MidoriResult::TypeResult
			{
				return Evaluate(binary.m_right)
					.and_then
					(
						[&left_type, &binary, this](std::shared_ptr<MidoriType>&& right_type) ->MidoriResult::TypeResult
						{
							// Special handling for shift operators: right operand must be Int, left can be Int/Byte/Word
							if (binary.m_op.m_token_name == Token::Name::LEFT_SHIFT || binary.m_op.m_token_name == Token::Name::RIGHT_SHIFT)
							{
								std::shared_ptr<MidoriType> resolved_left = ApplySubstitution(left_type);
								std::shared_ptr<MidoriType> resolved_right = ApplySubstitution(right_type);

								// Right operand (shift amount) must be Int
								if (!resolved_right->IsType<MidoriType::IntegerType>() && !resolved_right->IsType<MidoriType::TypeVariable>())
								{
									return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Shift operator type error: shift amount must be Int", binary.m_op, m_file_name, m_source_lines, resolved_right, MidoriType::MakeLiteralType<MidoriType::IntegerType>()));
								}

								// Left operand must be Int, Byte, or Word
								if (!resolved_left->IsType<MidoriType::IntegerType>() && !resolved_left->IsType<MidoriType::ByteType>() && !resolved_left->IsType<MidoriType::WordType>() && !resolved_left->IsType<MidoriType::TypeVariable>())
								{
									return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Shift operator type error: can only shift Int, Byte, or Word types", binary.m_op, m_file_name, m_source_lines, resolved_left, MidoriType::MakeLiteralType<MidoriType::IntegerType>(), MidoriType::MakeLiteralType<MidoriType::ByteType>(), MidoriType::MakeLiteralType<MidoriType::WordType>()));
								}

								// Result type is the same as left operand type
								binary.m_type_data = left_type;
								return binary.m_type_data;
							}

							return Unify(binary.m_op, left_type, right_type)
								.and_then
								(
									[&binary, &left_type, &right_type, this](std::shared_ptr<MidoriType>&&)->MidoriResult::TypeResult
									{
										std::shared_ptr<MidoriType>& self_type = binary.m_type_data;

										// Apply substitution to get concrete types if available
										std::shared_ptr<MidoriType> resolved_left = ApplySubstitution(left_type);
										std::shared_ptr<MidoriType> resolved_right = ApplySubstitution(right_type);

										self_type = left_type;

										if (std::ranges::contains(kBinaryPartialOrderComparisonOperators.cbegin(), kBinaryPartialOrderComparisonOperators.cend(), binary.m_op.m_token_name))
										{
											std::shared_ptr<MidoriType> resolved_self = ApplySubstitution(self_type);
											bool is_builtin = resolved_self->IsNumericType() || resolved_self->IsType<MidoriType::TypeVariable>();

											if (!is_builtin)
											{
												// Check for Orderable<T> instance
												InstanceKey orderable_key{std::string(ORDERABLE_CLASS_NAME),{resolved_self->ToString()}};
												std::unordered_map<InstanceKey, InstanceInfo, InstanceKeyHash>::iterator instance_it = m_instances.find(orderable_key);
												bool has_orderable_instance = (instance_it != m_instances.end());

												// Check for Orderable<T> constraint
												bool has_orderable_constraint = false;
												if (!has_orderable_instance)
												{
													for (const MidoriType::ClassConstraint& constraint : m_active_constraints)
													{
														if (constraint.m_class_name == ORDERABLE_CLASS_NAME && constraint.m_type_args.size() == 1 && *constraint.m_type_args[0] == *resolved_self)
														{
															has_orderable_constraint = true;
															break;
														}
													}
												}

												if (!has_orderable_instance && !has_orderable_constraint)
												{
													MidoriType::ClassConstraint constraint(std::string(ORDERABLE_CLASS_NAME), { resolved_self });
													return std::unexpected(MakeConstraintFailureError(binary.m_op, constraint));
												}

												binary.m_uses_orderable = true;
											}

											self_type = MidoriType::MakeLiteralType<MidoriType::BoolType>();
										}
										else if (std::ranges::contains(kBinaryArithmeticOperators.cbegin(), kBinaryArithmeticOperators.cend(), binary.m_op.m_token_name))
										{
											// Allow type variables (will be constrained by usage) or concrete numeric types
											std::shared_ptr<MidoriType> resolved_self = ApplySubstitution(self_type);
											if (!resolved_self->IsNumericType() && !resolved_self->IsType<MidoriType::TypeVariable>())
											{
												return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Binary expression type error: expected numeric type", binary.m_op, m_file_name, m_source_lines, resolved_self, MidoriType::MakeLiteralType<MidoriType::IntegerType>(), MidoriType::MakeLiteralType<MidoriType::FloatType>()));
											}
										}
										else if (std::ranges::contains(kBinaryBitwiseOperators.cbegin(), kBinaryBitwiseOperators.cend(), binary.m_op.m_token_name))
										{
											std::shared_ptr<MidoriType> resolved_self = ApplySubstitution(self_type);
											if (!resolved_self->IsType<MidoriType::IntegerType>() && !resolved_self->IsType<MidoriType::ByteType>() && !resolved_self->IsType<MidoriType::WordType>() && !resolved_self->IsType<MidoriType::TypeVariable>())
											{
												return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Binary expression type error: expected integer, byte, or word type", binary.m_op, m_file_name, m_source_lines, resolved_self, MidoriType::MakeLiteralType<MidoriType::IntegerType>(), MidoriType::MakeLiteralType<MidoriType::ByteType>(), MidoriType::MakeLiteralType<MidoriType::WordType>()));
											}
										}
										else if (std::ranges::contains(kBinaryEqualityOperators.cbegin(), kBinaryEqualityOperators.cend(), binary.m_op.m_token_name))
										{
											std::shared_ptr<MidoriType> resolved_self = ApplySubstitution(self_type);
											bool is_builtin = resolved_self->IsNumericType() || resolved_self->IsType<MidoriType::TextType>() || resolved_self->IsType<MidoriType::BoolType>() || resolved_self->IsType<MidoriType::TypeVariable>();

											if (!is_builtin)
											{
												// Check for Equatable<T> instance
												InstanceKey equatable_key{std::string(EQUATABLE_CLASS_NAME),{resolved_self->ToString()}};
												std::unordered_map<InstanceKey, InstanceInfo, InstanceKeyHash>::iterator instance_it = m_instances.find(equatable_key);
												bool has_equatable_instance = (instance_it != m_instances.end());

												// Check for Equatable<T> constraint
												bool has_equatable_constraint = false;
												if (!has_equatable_instance)
												{
													for (const MidoriType::ClassConstraint& constraint : m_active_constraints)
													{
														if (constraint.m_class_name == EQUATABLE_CLASS_NAME && constraint.m_type_args.size() == 1 && *constraint.m_type_args[0] == *resolved_self)
														{
															has_equatable_constraint = true;
															break;
														}
													}
												}

												if (!has_equatable_instance && !has_equatable_constraint)
												{
													MidoriType::ClassConstraint constraint(std::string(EQUATABLE_CLASS_NAME), { resolved_self });
													return std::unexpected(MakeConstraintFailureError(binary.m_op, constraint));
												}

												binary.m_uses_equatable = true;
											}

											self_type = MidoriType::MakeLiteralType<MidoriType::BoolType>();
										}
										else if (std::ranges::contains(kBinaryLogicalOperators.cbegin(), kBinaryLogicalOperators.cend(), binary.m_op.m_token_name))
										{
											std::shared_ptr<MidoriType> resolved_left_logical = ApplySubstitution(left_type);
											if (!resolved_left_logical->IsType<MidoriType::BoolType>() && !resolved_left_logical->IsType<MidoriType::TypeVariable>())
											{
												return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Binary expression type error: expected boolean type", binary.m_op, m_file_name, m_source_lines, resolved_left_logical, MidoriType::MakeLiteralType<MidoriType::BoolType>()));
											}

											self_type = MidoriType::MakeLiteralType<MidoriType::BoolType>();
										}
										else if (std::ranges::contains(kBinaryConcatenationOperators.cbegin(), kBinaryConcatenationOperators.cend(), binary.m_op.m_token_name))
										{
											bool is_builtin_concat = resolved_left->IsType<MidoriType::TextType>() || resolved_left->IsType<MidoriType::ArrayType>();
											if (!is_builtin_concat)
											{
												bool has_concatenable_instance = FindMatchingInstance(std::string(CONCATENABLE_CLASS_NAME), { resolved_left }).has_value();
												bool has_concatenable_constraint = false;
												if (!has_concatenable_instance)
												{
													for (const MidoriType::ClassConstraint& constraint : m_active_constraints)
													{
														if (constraint.m_class_name == CONCATENABLE_CLASS_NAME && constraint.m_type_args.size() == 1u && *constraint.m_type_args[0] == *resolved_left)
														{
															has_concatenable_constraint = true;
															break;
														}
													}
												}

												if (!has_concatenable_instance && !has_concatenable_constraint)
												{
													MidoriType::ClassConstraint constraint(std::string(CONCATENABLE_CLASS_NAME), { resolved_left });
													return std::unexpected(MakeConstraintFailureError(binary.m_op, constraint));
												}

												binary.m_uses_concatenable = true;
											}
										}

										return self_type;
									}
								);
						}
					);
			}
		);
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::Group& group)
{
	return Evaluate(group.m_expr_in)
		.and_then
		(
			[&group](std::shared_ptr<MidoriType>&& actual_type) ->MidoriResult::TypeResult
			{
				group.m_type_data = std::move(actual_type);
				return group.m_type_data;
			}
		);
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::Tuple& tuple)
{
	if (tuple.m_elements.empty())
	{
		// Empty tuple is Unit type
		tuple.m_type_data = MidoriType::MakeLiteralType<MidoriType::UnitType>();
		return tuple.m_type_data;
	}

	std::vector<std::shared_ptr<MidoriType>> element_types;
	element_types.reserve(tuple.m_elements.size());

	const std::shared_ptr<MidoriType> ambient_expected_type = m_expected_expr_type;
	const bool expected_tuple_matches = ambient_expected_type != nullptr
		&& ambient_expected_type->IsType<MidoriType::TupleType>()
		&& ambient_expected_type->GetType<MidoriType::TupleType>().m_element_types.size() == tuple.m_elements.size();

	for (size_t index : std::views::iota(0u, tuple.m_elements.size()))
	{
		// An element's expected type is its own slot in the expected tuple, never
		// the whole tuple. Leaving the ambient type in place leaks it into the
		// element, so a construction there would infer against the tuple type.
		ExpectedTypeGuard element_guard(*this, expected_tuple_matches ? ambient_expected_type->GetType<MidoriType::TupleType>().m_element_types[index] : std::shared_ptr<MidoriType>{});

		MidoriResult::TypeResult result = Evaluate(tuple.m_elements[index]);
		if (!result.has_value())
		{
			return result;
		}

		element_types.emplace_back(std::move(result.value()));
	}

	tuple.m_type_data = MidoriType::MakeTupleType(std::move(element_types));
	return tuple.m_type_data;
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::UnaryPrefix& unary)
{
	return Evaluate(unary.m_expr)
		.and_then
		(
			[this, &unary](std::shared_ptr<MidoriType>&& actual_type) -> MidoriResult::TypeResult
			{
				if (unary.m_op.m_token_name == Token::Name::SINGLE_MINUS || unary.m_op.m_token_name == Token::Name::SINGLE_PLUS)
				{
					if (!actual_type->IsNumericType())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Unary operator requires numeric type", unary.m_op, m_file_name, m_source_lines, actual_type, MidoriType::MakeLiteralType<MidoriType::IntegerType>(), MidoriType::MakeLiteralType<MidoriType::FloatType>()));
					}
				}
				else if (unary.m_op.m_token_name == Token::Name::BANG)
				{
					if (!actual_type->IsType<MidoriType::BoolType>())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Logical NOT operator requires boolean type", unary.m_op, m_file_name, m_source_lines, actual_type, MidoriType::MakeLiteralType<MidoriType::BoolType>()));
					}
				}
				else if (unary.m_op.m_token_name == Token::Name::TILDE)
				{
					if (!actual_type->IsType<MidoriType::IntegerType>())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Bitwise NOT operator requires integer type", unary.m_op, m_file_name, m_source_lines, actual_type, MidoriType::MakeLiteralType<MidoriType::IntegerType>()));
					}
				}
				else if (unary.m_op.m_token_name == Token::Name::HASH)
				{
					std::shared_ptr<MidoriType> resolved_type = ApplySubstitution(actual_type);

					if (resolved_type->IsType<MidoriType::ArrayType>())
					{
						unary.m_type_data = MidoriType::MakeLiteralType<MidoriType::IntegerType>();
						return unary.m_type_data;
					}

					bool has_countable_instance = FindMatchingInstance(std::string(COUNTABLE_CLASS_NAME), { resolved_type }).has_value();
					bool has_countable_constraint = false;
					if (!has_countable_instance)
					{
						for (const MidoriType::ClassConstraint& constraint : m_active_constraints)
						{
							if (constraint.m_class_name == COUNTABLE_CLASS_NAME && constraint.m_type_args.size() == 1u && *constraint.m_type_args[0] == *resolved_type)
							{
								has_countable_constraint = true;
								break;
							}
						}
					}

					if (!has_countable_instance && !has_countable_constraint)
					{
						MidoriType::ClassConstraint constraint(std::string(COUNTABLE_CLASS_NAME), { resolved_type });
						return std::unexpected(MakeConstraintFailureError(unary.m_op, constraint));
					}

					unary.m_uses_countable = has_countable_instance || has_countable_constraint;
					unary.m_type_data = MidoriType::MakeLiteralType<MidoriType::IntegerType>();
					return unary.m_type_data;
				}

				unary.m_type_data = std::move(actual_type);
				return unary.m_type_data;
			}
		);
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::UnarySuffix&)
{
	// TODO: Not yet implemented, no suffix operators at the moment
	return {};
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::Spawn& spawn)
{
	// The function is an ordinary value: it is evaluated here and transferred to the
	// worker, captured cells and all, so a lambda closing over the enclosing scope
	// works and a function held in a parameter can be spawned.
	// The ambient expected type belongs to the spawn's own result, Worker<T>, and
	// would otherwise be pushed onto a lambda written in callee position.
	MidoriResult::TypeResult callee_result;
	{
		ExpectedTypeGuard guard(*this, nullptr);
		callee_result = Evaluate(spawn.m_callee);
	}

	if (!callee_result.has_value())
	{
		return callee_result;
	}

	std::shared_ptr<MidoriType> callee_type = ApplySubstitution(callee_result.value());
	if (!callee_type->IsType<MidoriType::FunctionType>())
	{
		return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeNotCallable, "Spawn expression type error: the second argument to Concurrency::Spawn must be a function", spawn.m_spawn_keyword, m_file_name, m_source_lines, callee_type));
	}

	MidoriType::FunctionType& function_type = callee_type->GetType<MidoriType::FunctionType>();
	if (function_type.m_is_foreign)
	{
		return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Spawn expression type error: foreign functions cannot be spawned", spawn.m_spawn_keyword, m_file_name, m_source_lines));
	}

	if (std::optional<CompilerError> error = CheckSpawnArgument(spawn, function_type))
	{
		return std::unexpected(std::move(*error));
	}

	std::shared_ptr<MidoriType> resolved_return_type = ApplySubstitution(function_type.m_return_type);
	if (std::optional<CompilerError> error = EnsureTransferable(spawn.m_spawn_keyword, resolved_return_type))
	{
		return std::unexpected(std::move(*error));
	}

	spawn.m_type_data = MidoriType::MakeWorkerType(resolved_return_type);
	return spawn.m_type_data;
}

std::optional<CompilerError> TypeChecker::CheckSpawnArgument(MidoriExpression::Spawn& spawn, const MidoriType::FunctionType& function_type)
{
	const size_t arity = function_type.m_param_types.size();
	spawn.m_callee_arity = static_cast<int>(arity);

	std::shared_ptr<MidoriType> expected_type;
	if (arity == 0u)
	{
		expected_type = MidoriType::MakeLiteralType<MidoriType::UnitType>();
	}
	else if (arity == 1u)
	{
		expected_type = function_type.m_param_types[0u];
	}
	else
	{
		std::vector<std::shared_ptr<MidoriType>> element_types(function_type.m_param_types.begin(), function_type.m_param_types.end());
		expected_type = MidoriType::MakeTupleType(std::move(element_types));
	}

	ExpectedTypeGuard guard(*this, expected_type);
	MidoriResult::TypeResult argument_result = Evaluate(spawn.m_arguments[0u]);
	if (!argument_result.has_value())
	{
		return std::move(argument_result.error());
	}

	std::shared_ptr<MidoriType> actual_type = ApplySubstitution(argument_result.value());
	if (arity >= 2u)
	{
		const bool is_matching_tuple = actual_type->IsType<MidoriType::TupleType>() && actual_type->GetType<MidoriType::TupleType>().m_element_types.size() == arity;
		if (!is_matching_tuple)
		{
			return MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, std::format("Spawn expression type error: the spawned function takes {} parameters, so pass them to Concurrency::Spawn as one {}-element tuple", arity, arity), spawn.m_spawn_keyword, m_file_name, m_source_lines, actual_type, expected_type);
		}
	}

	MidoriResult::TypeResult unify_result = Unify(spawn.m_spawn_keyword, actual_type, expected_type, UnifyDiagnosticMode::ActualExpected);
	if (!unify_result.has_value())
	{
		return std::move(unify_result.error());
	}

	for (const std::shared_ptr<MidoriType>& param_type : function_type.m_param_types)
	{
		if (std::optional<CompilerError> error = EnsureTransferable(spawn.m_spawn_keyword, ApplySubstitution(param_type)))
		{
			return error;
		}
	}

	return std::nullopt;
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::Join& join)
{
	return Evaluate(join.m_worker)
		.and_then
		(
			[this, &join](std::shared_ptr<MidoriType>&& worker_type) -> MidoriResult::TypeResult
			{
				std::shared_ptr<MidoriType> resolved_worker_type = ApplySubstitution(worker_type);
				if (!resolved_worker_type->IsType<MidoriType::WorkerType>())
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Join expression type error: expected Worker<T>", join.m_join_keyword, m_file_name, m_source_lines, resolved_worker_type));
				}

				std::shared_ptr<MidoriType> worker_result_type = ApplySubstitution(resolved_worker_type->GetType<MidoriType::WorkerType>().m_result_type);

				// The VM builds these values itself, so the declarations it builds them
				// from must have exactly the shape it assumes.
				const std::shared_ptr<MidoriType>& result_type = join.m_result_type;
				if (result_type == nullptr
					|| !result_type->IsType<MidoriType::UnionType>()
					|| result_type->GetType<MidoriType::UnionType>().m_generic_params.size() != 2u)
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Join expression type error: 'Result' must be declared as a union with two type parameters, Ok(T) | Err(E)", join.m_join_keyword, m_file_name, m_source_lines));
				}

				const MidoriType::UnionType& result_union = result_type->GetType<MidoriType::UnionType>();
				const std::string ok_name = result_union.m_name + std::string(NameSeparator) + "Ok";
				const std::string err_name = result_union.m_name + std::string(NameSeparator) + "Err";
				if (!result_union.m_member_info.contains(ok_name) || result_union.m_member_info.at(ok_name).m_member_types.size() != 1u
					|| !result_union.m_member_info.contains(err_name) || result_union.m_member_info.at(err_name).m_member_types.size() != 1u)
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Join expression type error: 'Result' must be declared as Ok(T) | Err(E)", join.m_join_keyword, m_file_name, m_source_lines, result_type));
				}

				const std::shared_ptr<MidoriType>& worker_error_type = join.m_worker_error_type;
				if (worker_error_type == nullptr || !worker_error_type->IsType<MidoriType::UnionType>())
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Join expression type error: 'WorkerError' must be declared as a union, Cancelled | Failed(Text)", join.m_join_keyword, m_file_name, m_source_lines));
				}

				const MidoriType::UnionType& error_union = worker_error_type->GetType<MidoriType::UnionType>();
				const std::string cancelled_name = error_union.m_name + std::string(NameSeparator) + "Cancelled";
				const std::string failed_name = error_union.m_name + std::string(NameSeparator) + "Failed";
				if (!error_union.m_generic_params.empty()
					|| !error_union.m_member_info.contains(cancelled_name) || !error_union.m_member_info.at(cancelled_name).m_member_types.empty()
					|| !error_union.m_member_info.contains(failed_name) || error_union.m_member_info.at(failed_name).m_member_types.size() != 1u
					|| !error_union.m_member_info.at(failed_name).m_member_types[0u]->IsType<MidoriType::TextType>())
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Join expression type error: 'WorkerError' must be declared as Cancelled | Failed(Text)", join.m_join_keyword, m_file_name, m_source_lines, worker_error_type));
				}

				join.m_ok_tag = result_union.m_member_info.at(ok_name).m_tag;
				join.m_err_tag = result_union.m_member_info.at(err_name).m_tag;
				join.m_cancelled_tag = error_union.m_member_info.at(cancelled_name).m_tag;
				join.m_failed_tag = error_union.m_member_info.at(failed_name).m_tag;

				TypeEnvironment substitutions;
				substitutions[result_union.m_generic_params[0u]] = worker_result_type;
				substitutions[result_union.m_generic_params[1u]] = worker_error_type;
				join.m_type_data = ApplySubstitution(MidoriType::SubstituteTypeParams(result_type, substitutions));
				return join.m_type_data;
			}
		);
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::ChannelCreate& channel_create)
{
	if (channel_create.m_element_type == nullptr)
	{
		// Concurrency::MakeChannel(capacity) names no element type, so it takes the
		// one its context expects, as a generic constructor does.
		std::shared_ptr<MidoriType> expected_type = m_expected_expr_type != nullptr ? ApplySubstitution(m_expected_expr_type) : nullptr;
		if (expected_type == nullptr || !expected_type->IsType<MidoriType::ChannelType>() || expected_type->GetType<MidoriType::ChannelType>().m_element_type->IsType<MidoriType::TypeVariable>())
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Channel creation type error: Concurrency::MakeChannel cannot tell what the channel carries. Annotate where it is bound, as in 'def ch : Channel<Int> = Concurrency::MakeChannel(4);'", channel_create.m_channel_keyword, m_file_name, m_source_lines));
		}

		channel_create.m_element_type = expected_type->GetType<MidoriType::ChannelType>().m_element_type;
	}

	if (std::optional<CompilerError> error = EnsureTransferable(channel_create.m_channel_keyword, channel_create.m_element_type))
	{
		return std::unexpected(std::move(*error));
	}

	std::shared_ptr<MidoriType> expected_capacity_type = MidoriType::MakeLiteralType<MidoriType::IntegerType>();
	ExpectedTypeGuard guard(*this, expected_capacity_type);
	return Evaluate(channel_create.m_capacity)
		.and_then
		(
			[this, &channel_create, expected_capacity_type](std::shared_ptr<MidoriType>&& capacity_type) mutable -> MidoriResult::TypeResult
			{
				std::shared_ptr<MidoriType> actual_capacity_type = std::move(capacity_type);
				MidoriResult::TypeResult unify_result = Unify(channel_create.m_channel_keyword, actual_capacity_type, expected_capacity_type, UnifyDiagnosticMode::ActualExpected);
				if (!unify_result.has_value())
				{
					return unify_result;
				}

				channel_create.m_element_type = ApplySubstitution(channel_create.m_element_type);
				channel_create.m_type_data = MidoriType::MakeChannelType(channel_create.m_element_type);
				return channel_create.m_type_data;
			}
		);
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::Send& send)
{
	return Evaluate(send.m_channel)
		.and_then
		(
			[this, &send](std::shared_ptr<MidoriType>&& channel_type) -> MidoriResult::TypeResult
			{
				std::shared_ptr<MidoriType> resolved_channel_type = ApplySubstitution(channel_type);
				if (!resolved_channel_type->IsType<MidoriType::ChannelType>())
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Send expression type error: left operand must be Channel<T>", send.m_arrow, m_file_name, m_source_lines, resolved_channel_type));
				}

				std::shared_ptr<MidoriType> element_type = resolved_channel_type->GetType<MidoriType::ChannelType>().m_element_type;
				ExpectedTypeGuard guard(*this, element_type);
				return Evaluate(send.m_value)
					.and_then
					(
						[this, &send, element_type](std::shared_ptr<MidoriType>&& value_type) mutable -> MidoriResult::TypeResult
						{
							std::shared_ptr<MidoriType> actual_value_type = std::move(value_type);
							MidoriResult::TypeResult unify_result = Unify(send.m_arrow, actual_value_type, element_type, UnifyDiagnosticMode::ActualExpected);
							if (!unify_result.has_value())
							{
								return unify_result;
							}

							std::shared_ptr<MidoriType> resolved_element_type = ApplySubstitution(element_type);
							if (std::optional<CompilerError> error = EnsureTransferable(send.m_arrow, resolved_element_type))
							{
								return std::unexpected(std::move(*error));
							}

							send.m_type_data = MidoriType::MakeLiteralType<MidoriType::BoolType>();
							return send.m_type_data;
						}
					);
			}
		);
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::Receive& receive)
{
	return Evaluate(receive.m_channel)
		.and_then
		(
			[this, &receive](std::shared_ptr<MidoriType>&& channel_type) -> MidoriResult::TypeResult
			{
				std::shared_ptr<MidoriType> resolved_channel_type = ApplySubstitution(channel_type);
				if (!resolved_channel_type->IsType<MidoriType::ChannelType>())
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Receive expression type error: expected Channel<T>", receive.m_arrow, m_file_name, m_source_lines, resolved_channel_type));
				}

				receive.m_type_data = ApplySubstitution(resolved_channel_type->GetType<MidoriType::ChannelType>().m_element_type);
				return receive.m_type_data;
			}
		);
}

std::shared_ptr<MidoriType> TypeChecker::NarrowClassMethodType(const std::string& class_name, const ClassInfo& class_info, const std::shared_ptr<MidoriType>& declared_method_type, const std::vector<std::shared_ptr<MidoriType>>& known_arg_types, size_t arity)
{
	std::shared_ptr<MidoriType> narrowed_method_type;

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

		std::shared_ptr<MidoriType> candidate_method_type = ApplySubstitution(MidoriType::SubstituteTypeParams(declared_method_type, class_substitutions));
		if (!candidate_method_type->IsType<MidoriType::FunctionType>())
		{
			continue;
		}

		const MidoriType::FunctionType& candidate_function_type = candidate_method_type->GetType<MidoriType::FunctionType>();
		if (candidate_function_type.m_param_types.size() != arity)
		{
			continue;
		}

		std::unordered_map<std::string, std::shared_ptr<MidoriType>> substitutions;
		std::unordered_set<std::pair<MidoriType*, MidoriType*>, TypePairHash> visited;
		bool matched = std::ranges::all_of
		(
			std::views::iota(0u, known_arg_types.size()),
			[&candidate_function_type, &known_arg_types, &substitutions, &visited, this](size_t idx) -> bool
			{
				return MatchInstanceTypeArg(candidate_function_type.m_param_types[idx], ApplySubstitution(known_arg_types[idx]), substitutions, visited);
			}
		);

		if (!matched)
		{
			continue;
		}

		if (narrowed_method_type != nullptr)
		{
			return std::shared_ptr<MidoriType>{};
		}

		// SubstituteTypeParams with an empty map is not the identity - it rebuilds a struct with
		// its generic parameters cleared - so a match that derived no bindings keeps the candidate.
		narrowed_method_type = substitutions.empty() ? candidate_method_type : ApplySubstitution(MidoriType::SubstituteTypeParams(candidate_method_type, substitutions));
	}

	return narrowed_method_type;
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::Call& call)
{
	if (call.m_callee->IsExpression<MidoriExpression::NameAccess>())
	{
		MidoriExpression::NameAccess& callee_name = call.m_callee->GetExpression<MidoriExpression::NameAccess>();
		const std::string& full_name = callee_name.m_name.m_lexeme;
		// The parser turns a complete call to one of these into its own node, so a
		// Call reaching here has the wrong number of arguments.
		if (full_name == "Concurrency::Spawn")
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, "Call expression type error: Concurrency::Spawn takes two arguments, the value to pass and the function to run, as in 'Concurrency::Spawn(21, Double)'. Pass several values as a tuple.", call.m_paren, m_file_name, m_source_lines));
		}
		if (full_name == "Concurrency::Join" || full_name == "Concurrency::MakeChannel")
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, std::format("Call expression type error: {} takes exactly one argument", full_name), call.m_paren, m_file_name, m_source_lines));
		}
		if (full_name == "Concurrency::Close")
		{
			if (call.m_arguments.size() != 1u)
			{
				return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, "Call expression type error: Concurrency::Close takes exactly one argument", call.m_paren, m_file_name, m_source_lines));
			}

			MidoriResult::TypeResult channel_result = Evaluate(call.m_arguments[0u]);
			if (!channel_result.has_value())
			{
				return channel_result;
			}

			std::shared_ptr<MidoriType> resolved_channel_type = ApplySubstitution(channel_result.value());
			if (!resolved_channel_type->IsType<MidoriType::ChannelType>())
			{
				return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Call expression type error: Concurrency::Close takes a Channel<T>", call.m_paren, m_file_name, m_source_lines, resolved_channel_type));
			}

			call.m_is_foreign = false;
			call.m_type_data = MidoriType::MakeLiteralType<MidoriType::UnitType>();
			return call.m_type_data;
		}
		if (full_name == "Concurrency::IsDone" || full_name == "Concurrency::Cancel")
		{
			if (call.m_arguments.size() != 1u)
			{
				return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, std::format("Call expression type error: {} takes exactly one argument", full_name), call.m_paren, m_file_name, m_source_lines));
			}

			MidoriResult::TypeResult worker_result = Evaluate(call.m_arguments[0u]);
			if (!worker_result.has_value())
			{
				return worker_result;
			}

			std::shared_ptr<MidoriType> resolved_worker_type = ApplySubstitution(worker_result.value());
			if (!resolved_worker_type->IsType<MidoriType::WorkerType>())
			{
				return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(std::format("Call expression type error: {} takes a Worker<T>", full_name), call.m_paren, m_file_name, m_source_lines, resolved_worker_type));
			}

			call.m_is_foreign = false;
			call.m_type_data = MidoriType::MakeLiteralType<MidoriType::BoolType>();
			return call.m_type_data;
		}

		if (full_name == "Cell::New")
		{
			if (call.m_arguments.size() != 1u)
			{
				return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, "Call expression type error: Cell::New takes exactly one argument, the value the cell starts with", call.m_paren, m_file_name, m_source_lines));
			}

			// An annotated binding such as 'def c : Cell<Array<Int>> = Cell::New([])' tells
			// the initial value what it is, the way a constructor's context does.
			std::shared_ptr<MidoriType> expected_element = nullptr;
			std::shared_ptr<MidoriType> expected_type = m_expected_expr_type != nullptr ? ApplySubstitution(m_expected_expr_type) : nullptr;
			if (expected_type != nullptr && expected_type->IsType<MidoriType::CellType>())
			{
				expected_element = expected_type->GetType<MidoriType::CellType>().m_element_type;
			}

			MidoriResult::TypeResult value_result = [this, &call, &expected_element]() -> MidoriResult::TypeResult
			{
				ExpectedTypeGuard guard(*this, expected_element);
				return Evaluate(call.m_arguments[0u]);
			}();
			if (!value_result.has_value())
			{
				return value_result;
			}

			call.m_is_foreign = false;
			call.m_type_data = MidoriType::MakeCellType(ApplySubstitution(value_result.value()));
			return call.m_type_data;
		}
		if (full_name == "Cell::Get" || full_name == "Cell::Set")
		{
			const bool is_set = full_name == "Cell::Set";
			const size_t expected_arity = is_set ? 2u : 1u;
			if (call.m_arguments.size() != expected_arity)
			{
				return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, std::format("Call expression type error: {} takes {}", full_name, is_set ? "two arguments, the cell and the new value" : "exactly one argument, the cell"), call.m_paren, m_file_name, m_source_lines));
			}

			MidoriResult::TypeResult cell_result = [this, &call]() -> MidoriResult::TypeResult
			{
				ExpectedTypeGuard guard(*this, nullptr);
				return Evaluate(call.m_arguments[0u]);
			}();
			if (!cell_result.has_value())
			{
				return cell_result;
			}

			std::shared_ptr<MidoriType> resolved_cell_type = ApplySubstitution(cell_result.value());
			if (!resolved_cell_type->IsType<MidoriType::CellType>())
			{
				return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(std::format("Call expression type error: {} takes a Cell<T>", full_name), call.m_paren, m_file_name, m_source_lines, resolved_cell_type));
			}

			std::shared_ptr<MidoriType> element_type = resolved_cell_type->GetType<MidoriType::CellType>().m_element_type;
			if (is_set)
			{
				MidoriResult::TypeResult value_result = [this, &call, &element_type]() -> MidoriResult::TypeResult
				{
					ExpectedTypeGuard guard(*this, element_type);
					return Evaluate(call.m_arguments[1u]);
				}();
				if (!value_result.has_value())
				{
					return value_result;
				}

				std::shared_ptr<MidoriType> value_type = value_result.value();
				MidoriResult::TypeResult unified = Unify(call.m_paren, element_type, value_type);
				if (!unified.has_value())
				{
					return unified;
				}
			}

			call.m_is_foreign = false;
			call.m_type_data = ApplySubstitution(element_type);
			return call.m_type_data;
		}

		size_t separator_pos = full_name.rfind(NameSeparator.data());
		if (separator_pos != std::string::npos)
		{
			std::string qualifier = full_name.substr(0u, separator_pos);
			std::string method_name = full_name.substr(separator_pos + NameSeparator.length());

			std::vector<const MidoriType::ClassConstraint*> matching_constraints;
			for (const MidoriType::ClassConstraint& constraint : m_active_constraints)
			{
				if (constraint.m_class_name == qualifier)
				{
					matching_constraints.emplace_back(&constraint);
				}
			}

			if (!matching_constraints.empty())
			{
				std::unordered_map<std::string, ClassInfo>::iterator tc_it = m_classes.find(qualifier);
				if (tc_it == m_classes.end())
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Call expression type error: unknown class '" + qualifier + "'", call.m_paren, m_file_name, m_source_lines));
				}

				const ClassInfo& tc_info = tc_it->second;
				TypeEnvironment::const_iterator method_it = tc_info.m_method_types.find(method_name);
				if (method_it != tc_info.m_method_types.cend())
				{
					std::vector<std::shared_ptr<MidoriType>> arg_results;
					arg_results.reserve(call.m_arguments.size());
					for (std::unique_ptr<MidoriExpression>& call_arg : call.m_arguments)
					{
						MidoriResult::TypeResult arg_result = Evaluate(call_arg);
						if (!arg_result.has_value())
						{
							return arg_result;
						}
						arg_results.emplace_back(std::move(arg_result.value()));
					}

					TypeEnvironment env_substitutions;
					for (TypeChecker::TypeEnvironmentStack::reverse_iterator it = m_name_type_table.rbegin(); it != m_name_type_table.rend(); ++it)
					{
						for (const TypeEnvironment::value_type& entry : *it)
						{
							const std::string& name = entry.first;
							const std::shared_ptr<MidoriType>& type = entry.second;

							if (!env_substitutions.contains(name))
							{
								env_substitutions.emplace(name, type);
							}
						}
					}

					std::vector<const MidoriType::ClassConstraint*> selected_constraints;
					if (!arg_results.empty())
					{
						std::shared_ptr<MidoriType> first_arg_type = ApplySubstitution(arg_results[0u]);
						for (const MidoriType::ClassConstraint* constraint : matching_constraints)
						{
							if (constraint->m_type_args.empty())
							{
								continue;
							}

							std::shared_ptr<MidoriType> substituted_first = MidoriType::SubstituteTypeParams(constraint->m_type_args[0u], env_substitutions);
							std::shared_ptr<MidoriType> resolved_first = ApplySubstitution(substituted_first);

							if (*resolved_first == *first_arg_type)
							{
								selected_constraints.emplace_back(constraint);
							}
						}
					}
					else
					{
						selected_constraints = matching_constraints;
					}

					if (selected_constraints.empty())
					{
						std::string first_arg_name = arg_results.empty() ? std::string("no arguments") : ApplySubstitution(arg_results[0u])->ToString();
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Call expression type error: no matching class constraint for '" + qualifier + NameSeparator.data() + method_name + "' and argument type '" + first_arg_name + "'", call.m_paren, m_file_name, m_source_lines));
					}
					if (selected_constraints.size() != 1u)
					{
						std::string first_arg_name = arg_results.empty() ? std::string("no arguments") : ApplySubstitution(arg_results[0u])->ToString();
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Call expression type error: ambiguous class method '" + qualifier + NameSeparator.data() + method_name + "' for argument type '" + first_arg_name + "'", call.m_paren, m_file_name, m_source_lines));
					}

					const MidoriType::ClassConstraint& selected_constraint = *selected_constraints[0u];
					if (selected_constraint.m_type_args.size() != tc_info.m_type_param_names.size())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Call expression type error: constraint type argument count mismatch for class '" + qualifier + "'", call.m_paren, m_file_name, m_source_lines));
					}

					TypeEnvironment class_substitutions;
					for (size_t i = 0u; i < tc_info.m_type_param_names.size(); i += 1u)
					{
						std::shared_ptr<MidoriType> resolved_type_arg = MidoriType::SubstituteTypeParams(selected_constraint.m_type_args[i], env_substitutions);
						class_substitutions.emplace(tc_info.m_type_param_names[i], ApplySubstitution(resolved_type_arg));
					}

					std::shared_ptr<MidoriType> substituted_method_type = MidoriType::SubstituteTypeParams(method_it->second, class_substitutions);
					std::shared_ptr<MidoriType> resolved_method_type = ApplySubstitution(substituted_method_type);
					if (!resolved_method_type->IsType<MidoriType::FunctionType>())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeNotCallable, "Call expression type error: not a callable", call.m_paren, m_file_name, m_source_lines, resolved_method_type));
					}

					MidoriType::FunctionType& function_type = resolved_method_type->GetType<MidoriType::FunctionType>();
					if (function_type.m_param_types.size() != arg_results.size())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, "Call expression type error: incorrect arity", call.m_paren, m_file_name, m_source_lines));
					}

					std::vector<std::shared_ptr<MidoriType>>& param_types = function_type.m_param_types;
					for (size_t idx : std::views::iota(0u, arg_results.size()))
					{
						std::shared_ptr<MidoriType>& actual_param_type = arg_results[idx];
						std::shared_ptr<MidoriType>& param_type = param_types[idx];
						MidoriResult::TypeResult result = Unify(call.m_paren, actual_param_type, param_type, UnifyDiagnosticMode::ActualExpected);
						if (!result.has_value())
						{
							return result;
						}
					}

					call.m_is_foreign = function_type.m_is_foreign;
					call.m_type_data = ApplySubstitution(function_type.m_return_type);
					return call.m_type_data;
				}
			}

			std::unordered_map<std::string, ClassInfo>::iterator tc_it = m_classes.find(qualifier);
			if (tc_it != m_classes.end())
			{
				const ClassInfo& tc_info = tc_it->second;
				TypeEnvironment::const_iterator method_it = tc_info.m_method_types.find(method_name);
				if (method_it != tc_info.m_method_types.cend())
				{
					std::vector<std::shared_ptr<MidoriType>> arg_results;
					arg_results.reserve(call.m_arguments.size());
					for (size_t idx : std::views::iota(0u, call.m_arguments.size()))
					{
						// Instance selection is argument-directed, so no parameter type is known before the
						// arguments are checked. Narrowing against the arguments already settled recovers one
						// as soon as a single instance still matches, which is what lets
						// `Appendable::Append(buckets, Slot::Empty())` infer the construction from the
						// container it is appended to. A prefix that picks out no single instance leaves the
						// argument checked with no expected type, as before. Selection below is unchanged and
						// still consults every argument.
						std::shared_ptr<MidoriType> narrowed_method_type = NarrowClassMethodType(qualifier, tc_info, method_it->second, arg_results, call.m_arguments.size());
						std::shared_ptr<MidoriType> expected_param_type = narrowed_method_type != nullptr ? narrowed_method_type->GetType<MidoriType::FunctionType>().m_param_types[idx] : std::shared_ptr<MidoriType>{};
						ExpectedTypeGuard guard(*this, std::move(expected_param_type));

						MidoriResult::TypeResult arg_result = Evaluate(call.m_arguments[idx]);
						if (!arg_result.has_value())
						{
							return arg_result;
						}
						arg_results.emplace_back(std::move(arg_result.value()));
					}

					struct ConcreteMethodCandidate
					{
						std::shared_ptr<MidoriType> m_method_type;
						const InstanceInfo* m_instance;
						TypeEnvironment m_substitutions;
						bool m_matches_expected_type;

						ConcreteMethodCandidate(std::shared_ptr<MidoriType>&& method_type, const InstanceInfo* instance, TypeEnvironment&& substitutions, bool matches_expected_type)
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

						std::shared_ptr<MidoriType> candidate_method_type = ApplySubstitution(MidoriType::SubstituteTypeParams(method_it->second, class_substitutions));
						if (!candidate_method_type->IsType<MidoriType::FunctionType>())
						{
							continue;
						}

						const MidoriType::FunctionType& candidate_function_type = candidate_method_type->GetType<MidoriType::FunctionType>();
						if (candidate_function_type.m_param_types.size() != arg_results.size())
						{
							continue;
						}

						std::unordered_map<std::string, std::shared_ptr<MidoriType>> substitutions;
						std::unordered_set<std::pair<MidoriType*, MidoriType*>, TypePairHash> visited;
						bool matched = true;

						for (size_t i = 0u; i < arg_results.size(); i += 1u)
						{
							std::shared_ptr<MidoriType> resolved_arg = ApplySubstitution(arg_results[i]);
							if (!MatchInstanceTypeArg(candidate_function_type.m_param_types[i], resolved_arg, substitutions, visited))
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
							std::unordered_map<std::string, std::shared_ptr<MidoriType>> expected_substitutions = substitutions;
							std::unordered_set<std::pair<MidoriType*, MidoriType*>, TypePairHash> expected_visited = visited;
							std::shared_ptr<MidoriType> expected_type = ApplySubstitution(m_expected_expr_type);
							if (MatchInstanceTypeArg(candidate_function_type.m_return_type, expected_type, expected_substitutions, expected_visited))
							{
								matches_expected_type = true;
								substitutions = std::move(expected_substitutions);
								visited = std::move(expected_visited);
							}
						}

						std::shared_ptr<MidoriType> resolved_method_type = ApplySubstitution(MidoriType::SubstituteTypeParams(candidate_method_type, substitutions));
						candidates.emplace_back(std::move(resolved_method_type), &instance_info, std::move(substitutions), matches_expected_type);
					}

					if (candidates.empty())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Call expression type error: no matching concrete instance for '" + qualifier + NameSeparator.data() + method_name + "'", call.m_paren, m_file_name, m_source_lines));
					}
					if (candidates.size() != 1u)
					{
						if (std::ranges::count_if(candidates, &ConcreteMethodCandidate::m_matches_expected_type) != 1)
						{
							return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Call expression type error: ambiguous concrete instance for '" + qualifier + NameSeparator.data() + method_name + "'", call.m_paren, m_file_name, m_source_lines));
						}

						std::erase_if(candidates, [](const ConcreteMethodCandidate& candidate) -> bool { return !candidate.m_matches_expected_type; });
					}

					MidoriType::FunctionType& function_type = candidates[0u].m_method_type->GetType<MidoriType::FunctionType>();
					for (size_t idx : std::views::iota(0u, arg_results.size()))
					{
						std::shared_ptr<MidoriType>& actual_param_type = arg_results[idx];
						std::shared_ptr<MidoriType>& param_type = function_type.m_param_types[idx];
						MidoriResult::TypeResult result = Unify(call.m_paren, actual_param_type, param_type, UnifyDiagnosticMode::ActualExpected);
						if (!result.has_value())
						{
							return result;
						}
					}

					MidoriResult::TypeResult constraint_result = ValidateFunctionConstraints(call.m_paren, function_type);
					if (!constraint_result.has_value())
					{
						return constraint_result;
					}

					// function_type comes from the class declaration, so it never carries the
					// selected instance's where-clause - that lives on InstanceInfo instead.
					MidoriResult::TypeResult instance_constraint_result = ValidateInstanceConstraints(call.m_paren, *candidates[0u].m_instance, candidates[0u].m_substitutions, 0u);
					if (!instance_constraint_result.has_value())
					{
						return instance_constraint_result;
					}

					call.m_is_foreign = function_type.m_is_foreign;
					call.m_type_data = ApplySubstitution(function_type.m_return_type);
					return call.m_type_data;
				}
			}
		}
	}

	if (call.m_callee->IsExpression<MidoriExpression::Function>())
	{
		std::vector<std::shared_ptr<MidoriType>> arg_results;
		arg_results.reserve(call.m_arguments.size());
		for (std::unique_ptr<MidoriExpression>& call_arg : call.m_arguments)
		{
			MidoriResult::TypeResult arg_result = Evaluate(call_arg);
			if (!arg_result.has_value())
			{
				return arg_result;
			}
			arg_results.emplace_back(std::move(arg_result.value()));
		}

		std::shared_ptr<MidoriType> expected_return_type = m_expected_expr_type != nullptr ? std::shared_ptr<MidoriType>(m_expected_expr_type) : FreshTypeVar();
		std::shared_ptr<MidoriType> expected_callee_type = MidoriType::MakeFunctionType(arg_results, std::move(expected_return_type));
		ExpectedTypeGuard guard(*this, expected_callee_type);

		return Evaluate(call.m_callee)
			.and_then
			(
				[&call, &arg_results, this](std::shared_ptr<MidoriType>&& actual_type) -> MidoriResult::TypeResult
				{
					std::shared_ptr<MidoriType> resolved_type = ApplySubstitution(actual_type);
					if (!resolved_type->IsType<MidoriType::FunctionType>())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeNotCallable, "Call expression type error: not a callable", call.m_paren, m_file_name, m_source_lines, resolved_type));
					}

					MidoriType::FunctionType& function_type = resolved_type->GetType<MidoriType::FunctionType>();
					if (function_type.m_param_types.size() != call.m_arguments.size())
					{
						return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, "Call expression type error: incorrect arity", call.m_paren, m_file_name, m_source_lines));
					}

					for (size_t idx : std::views::iota(0u, arg_results.size()))
					{
						std::shared_ptr<MidoriType>& actual_param_type = arg_results[idx];
						std::shared_ptr<MidoriType>& param_type = function_type.m_param_types[idx];
						MidoriResult::TypeResult result = Unify(call.m_paren, actual_param_type, param_type, UnifyDiagnosticMode::ActualExpected);
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

	return Evaluate(call.m_callee)
		.and_then
		(
			[&call, this](std::shared_ptr<MidoriType>&& actual_type) ->MidoriResult::TypeResult
			{
				std::shared_ptr<MidoriType> resolved_type = ApplySubstitution(actual_type);

				if (!resolved_type->IsType<MidoriType::FunctionType>())
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeNotCallable, "Call expression type error: not a callable", call.m_paren, m_file_name, m_source_lines, resolved_type));
				}

				MidoriType::FunctionType& function_type = resolved_type->GetType<MidoriType::FunctionType>();
				if (function_type.m_param_types.size() != call.m_arguments.size())
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, "Call expression type error: incorrect arity", call.m_paren, m_file_name, m_source_lines));
				}

				for (size_t idx : std::views::iota(0u, call.m_arguments.size()))
				{
					// Each argument is unified against its parameter before the next one is checked, so a
					// parameter type that an earlier argument decides is already resolved by the time it
					// becomes the expected type. Deferring every unification to a second pass left the
					// callee's freshened variables unbound, so `Append(buckets, Slot::Empty())` on
					// `fn<T>(Array<T>, T) -> Unit` checked the construction against a bare T and rejected it.
					ExpectedTypeGuard guard(*this, function_type.m_param_types[idx]);

					MidoriResult::TypeResult arg_result = Evaluate(call.m_arguments[idx]);
					if (!arg_result.has_value())
					{
						return arg_result;
					}

					MidoriResult::TypeResult unify_result = Unify(call.m_paren, arg_result.value(), function_type.m_param_types[idx], UnifyDiagnosticMode::ActualExpected);
					if (!unify_result.has_value())
					{
						return unify_result;
					}
				}

				MidoriResult::TypeResult constraint_result = ValidateFunctionConstraints(call.m_paren, function_type);
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

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::MemberAccess& get)
{
	// The type the context demands of `a.b` describes the member, not the object: `a`'s type is
	// settled by `a` alone, and the member name then picks a field out of it. Carrying the
	// demand into the object let a construction there unify its own struct type with whatever
	// the surrounding call wanted, so `IO::PrintLine((new Point(7, 8)).x as Text)` was rejected
	// for building a Point where Text was expected.
	ExpectedTypeGuard object_guard(*this, std::shared_ptr<MidoriType>{});

	return Evaluate(get.m_struct)
		.and_then
		(
			[this, &get](std::shared_ptr<MidoriType>&& actual_type) -> MidoriResult::TypeResult
			{
				if (!actual_type->IsType<MidoriType::StructType>())
				{
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Cannot access member on non-struct type", get.m_member_name, m_file_name, m_source_lines));
				}

				const MidoriType::StructType& struct_type = actual_type->GetType<MidoriType::StructType>();
				std::vector<std::string>::const_iterator find_result = std::find(struct_type.m_member_names.cbegin(), struct_type.m_member_names.cend(), get.m_member_name.m_lexeme);
				if (find_result == struct_type.m_member_names.cend())
				{
					std::string suggestion = std::format("Struct '{}' does not have a member named '{}'", struct_type.m_name, get.m_member_name.m_lexeme);
					return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Unknown struct member", get.m_member_name, m_file_name, m_source_lines, suggestion));
				}

				get.m_index = static_cast<int>(find_result - struct_type.m_member_names.cbegin());

				get.m_type_data = struct_type.m_member_types[static_cast<size_t>(get.m_index)];
				return get.m_type_data;
			}
		);
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::NameAccess& variable)
{
	size_t separator_pos = variable.m_name.m_lexeme.rfind(NameSeparator.data());
	if (separator_pos != std::string::npos)
	{
		std::string qualifier = variable.m_name.m_lexeme.substr(0u, separator_pos);
		std::string symbol_name = variable.m_name.m_lexeme.substr(separator_pos + NameSeparator.length());

		for (const MidoriType::ClassConstraint& constraint : m_active_constraints)
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
			for (const MidoriType::ClassConstraint& constraint : m_active_constraints)
			{
				active_classes.push_back(constraint.m_class_name);
			}

			std::string suggestion = active_classes.empty()
				? "No matching class constraints are active in this scope."
				: "Active constraints: " + JoinSortedNames(std::move(active_classes));
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Name access expression type error: class method is not available in the current constraint scope", variable.m_name, m_file_name, m_source_lines, suggestion));
		}
	}

	const std::shared_ptr<MidoriType>* binding = FindNameType(variable.m_name.m_lexeme);
	if (binding != nullptr)
	{
		const bool binding_is_generic_function =
			m_generic_functions.contains(variable.m_name.m_lexeme)
			&& std::ranges::find(m_defining_generic_names, variable.m_name.m_lexeme) == m_defining_generic_names.cend()
			&& binding->get()->IsType<MidoriType::FunctionType>();
		if (binding_is_generic_function
			|| variable.m_name.m_lexeme.find("::") != std::string::npos
			|| (binding->get()->IsType<MidoriType::FunctionType>() && binding->get()->GetType<MidoriType::FunctionType>().m_is_foreign))
		{
			variable.m_type_data = Freshen(*binding);
		}
		else
		{
			// Apply substitution to get the most up-to-date type
			// This handles cases where the type contains type variables that have been unified
			variable.m_type_data = ApplySubstitution(*binding);
		}
		return variable.m_type_data;
	}

	for (const MidoriType::ClassConstraint& constraint : m_active_constraints)
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

	return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUndefinedName, "Name access expression type error: variable not found", variable.m_name, m_file_name, m_source_lines));
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::Literal& literal)
{
	switch (literal.m_kind)
	{
	case MidoriExpression::LiteralKind::Text:
		literal.m_type_data = MidoriType::MakeLiteralType<MidoriType::TextType>();
		break;
	case MidoriExpression::LiteralKind::Bool:
		literal.m_type_data = MidoriType::MakeLiteralType<MidoriType::BoolType>();
		break;
	case MidoriExpression::LiteralKind::Float:
		literal.m_type_data = MidoriType::MakeLiteralType<MidoriType::FloatType>();
		break;
	case MidoriExpression::LiteralKind::Integer:
		literal.m_type_data = MidoriType::MakeLiteralType<MidoriType::IntegerType>();
		break;
	case MidoriExpression::LiteralKind::Byte:
		literal.m_type_data = MidoriType::MakeLiteralType<MidoriType::ByteType>();
		break;
	case MidoriExpression::LiteralKind::Word:
		literal.m_type_data = MidoriType::MakeLiteralType<MidoriType::WordType>();
		break;
	case MidoriExpression::LiteralKind::Unit:
		literal.m_type_data = MidoriType::MakeLiteralType<MidoriType::UnitType>();
		break;
	}
	return literal.m_type_data;
}

MidoriResult::TypeResult TypeChecker::ResolveFunctionExpressionSignature(MidoriExpression::Function& function, const std::unordered_set<int>& outer_visible_type_vars)
{
	function.m_type_data = ApplySubstitution(function.m_type_data);
	const MidoriType::FunctionType& resolved_type = function.m_type_data->GetType<MidoriType::FunctionType>();
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
				MidoriError::GenerateTypeCheckerErrorWithContext
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

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::Function& function)
{
	// A generic lambda is specialized by name at each call site, so it is only
	// reachable as the value of a top-level 'def'. That case is handled by
	// TypeCheckGenericLambdaDefinition and never reaches this visitor; anything
	// arriving here is anonymous (for example passed directly as an argument).
	if (!function.m_generic_params.empty())
	{
		return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Function expression type error: an anonymous lambda cannot have generic parameters; bind it to a top-level name with 'def' so it can be specialized at each call site", function.m_function_keyword, m_file_name, m_source_lines));
	}

	// Preserve visible generic type variables when a lambda annotation refers to them.
	FresheningContext freshening_context = MakeLambdaFresheningContext();

	// Freshen any UndecidedType parameters to TypeVariables
	for (std::shared_ptr<MidoriType>& param_type : function.m_param_types)
	{
		param_type = Freshen(param_type, freshening_context);
	}
	function.m_return_type = Freshen(function.m_return_type, freshening_context);

	// Create the function type and store it so it can be returned
	std::shared_ptr<MidoriType> return_type_copy = function.m_return_type;
	function.m_type_data = MidoriType::MakeFunctionType(function.m_param_types, std::move(return_type_copy));

	if (m_expected_expr_type != nullptr)
	{
		std::shared_ptr<MidoriType> expected_type = m_expected_expr_type;
		MidoriResult::TypeResult expected_result = Unify(function.m_function_keyword, function.m_type_data, expected_type, UnifyDiagnosticMode::ActualExpected);
		if (!expected_result.has_value())
		{
			return expected_result;
		}

		function.m_type_data = ApplySubstitution(function.m_type_data);
		const MidoriType::FunctionType& inferred_type = function.m_type_data->GetType<MidoriType::FunctionType>();
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

	std::vector<MidoriType::ClassConstraint> function_constraints = CollectSignatureConstraints(function.m_param_types, function.m_return_type);
	function.m_type_data->GetType<MidoriType::FunctionType>().m_constraints = function_constraints;

	return ScopeSession(*this).Then([&]() -> MidoriResult::TypeResult
	{
		std::ranges::for_each
		(
			std::views::iota(0u, function.m_params.size()),
			[&function, this](size_t idx) {m_name_type_table.back().emplace(function.m_params[idx].m_lexeme, function.m_param_types[idx]); }
		);

		size_t prev_constraints_size = m_active_constraints.size();
		for (const MidoriType::ClassConstraint& constraint : function_constraints)
		{
			if (!ContainsConstraint(m_active_constraints, constraint))
			{
				m_active_constraints.push_back(constraint);
			}
		}

		// A return inside the body is validated against this, and only types itself as
		// Never once it is set. Leaving it unset made `{ return 1; }` type as Int, which
		// broke every enclosing unification the function-definition path handles correctly.
		std::shared_ptr<MidoriType> saved_expected_return_type = m_expected_return_type;
		m_expected_return_type = function.m_return_type;

		ExpectedTypeGuard expected_expr_guard(*this, function.m_return_type);
		return Evaluate(function.m_body)
			.and_then
			(
				[&function, &outer_visible_type_vars, &saved_expected_return_type, prev_constraints_size, this](std::shared_ptr<MidoriType>&& function_return_value_type) ->MidoriResult::TypeResult
				{
					m_expected_return_type = saved_expected_return_type;
					m_active_constraints.resize(prev_constraints_size);

					return Unify(function.m_function_keyword, function.m_return_type, function_return_value_type, UnifyDiagnosticMode::ExpectedActual)
						.and_then
						(
							[&function, &outer_visible_type_vars, this](std::shared_ptr<MidoriType>&&) -> MidoriResult::TypeResult
							{
								return ResolveFunctionExpressionSignature(function, outer_visible_type_vars);
							}
						);
				}
			).or_else
			(
				[&saved_expected_return_type, prev_constraints_size, this](CompilerError&& error) -> MidoriResult::TypeResult
				{
					m_expected_return_type = saved_expected_return_type;
					m_active_constraints.resize(prev_constraints_size);
					return std::unexpected(std::move(error));
				}
			);
	});
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::Construct& construct)
{
	const std::shared_ptr<MidoriType>& return_type = construct.m_return_type;
	std::string constructor_name;
	std::string actual_type_name; 

	if (construct.IsConstructTypeOf<MidoriExpression::Construct::Struct>())
	{
		actual_type_name = return_type->GetType<MidoriType::StructType>().m_name;
		constructor_name = actual_type_name;
	}
	else
	{
		constructor_name = construct.m_data_name.m_lexeme;
		actual_type_name = return_type->GetType<MidoriType::UnionType>().m_name;
	}

	const std::shared_ptr<MidoriType>* constructor_type_ptr = FindNameType(constructor_name);
	if (constructor_type_ptr == nullptr)
	{
		return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Construct expression type error: struct or union not found", construct.m_data_name, m_file_name, m_source_lines));
	}

	bool is_generic = false;
	if (construct.IsConstructTypeOf<MidoriExpression::Construct::Struct>())
	{
		is_generic = m_generic_structs.contains(actual_type_name);
	}
	else
	{
		is_generic = m_generic_unions.contains(actual_type_name);
	}

	std::shared_ptr<MidoriType> constructor_type_shared = is_generic ? Freshen(*constructor_type_ptr) : *constructor_type_ptr;

	// The name must resolve to the constructor's function type. It does not when a
	// struct is imported into another module, where the name binds to the type
	// itself, and reading it as a function would be undefined rather than an error.
	if (!constructor_type_shared->IsType<MidoriType::FunctionType>())
	{
		return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Construct expression type error: '" + constructor_name + "' does not name a constructor here (it resolved to '" + constructor_type_shared->ToString() + "')", construct.m_data_name, m_file_name, m_source_lines));
	}

	MidoriType::FunctionType& constructor_type = constructor_type_shared->GetType<MidoriType::FunctionType>();

	if (construct.m_has_explicit_type_args)
	{
		MidoriResult::TypeResult explicit_type_result = Unify(construct.m_data_name, constructor_type.m_return_type, construct.m_return_type, UnifyDiagnosticMode::ActualExpected);
		if (!explicit_type_result.has_value())
		{
			return explicit_type_result;
		}
	}
	else if (m_expected_expr_type != nullptr)
	{
		std::shared_ptr<MidoriType> expected_type = m_expected_expr_type;
		MidoriResult::TypeResult expected_type_result = Unify(construct.m_data_name, constructor_type.m_return_type, expected_type, UnifyDiagnosticMode::ActualExpected);
		if (!expected_type_result.has_value())
		{
			return expected_type_result;
		}
	}

	if (constructor_type.m_param_types.size() != construct.m_params.size())
	{
		return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, "Construct expression type error: incorrect arity", construct.m_data_name, m_file_name, m_source_lines));
	}

	for (size_t idx : std::views::iota(0u, construct.m_params.size()))
	{
		std::unique_ptr<MidoriExpression>& param = construct.m_params[idx];
		ExpectedTypeGuard guard(*this, constructor_type.m_param_types[idx]);
		MidoriResult::TypeResult param_result = Evaluate(param);
		if (!param_result.has_value())
		{
			return param_result;
		}

		std::shared_ptr<MidoriType> param_type = constructor_type.m_param_types[idx];
		MidoriResult::TypeResult unify_result = Unify(construct.m_data_name, param_result.value(), param_type, UnifyDiagnosticMode::ActualExpected);
		if (!unify_result.has_value())
		{
			return unify_result;
		}
	}

	MidoriResult::TypeResult constraint_result = ValidateFunctionConstraints(construct.m_data_name, constructor_type);
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
				MidoriError::GenerateTypeCheckerErrorWithContext(
					std::format("Construct expression type error: could not infer all type arguments for '{}'", actual_type_name),
					construct.m_data_name,
					m_file_name,
					m_source_lines
				)
			);
		}
	}

	// Mark as generic instantiation if this was a generic struct/union
	if (construct.IsConstructTypeOf<MidoriExpression::Construct::Struct>())
	{
		if (m_generic_structs.contains(actual_type_name) && construct.m_type_data->IsType<MidoriType::StructType>())
		{
			construct.m_type_data->GetType<MidoriType::StructType>().m_is_generic_instantiation = true;
		}
	}
	else if (m_generic_unions.contains(actual_type_name) && construct.m_type_data->IsType<MidoriType::UnionType>())
	{
		construct.m_type_data->GetType<MidoriType::UnionType>().m_is_generic_instantiation = true;
	}

	return construct.m_type_data;
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::RecordUpdate& record_update)
{
	MidoriResult::TypeResult source_result = Evaluate(record_update.m_source);
	if (!source_result.has_value())
	{
		return source_result;
	}

	std::shared_ptr<MidoriType> source_type = source_result.value();

	// Multi-variant types are out of scope: `{ u with ... }` on a union would require the
	// variant to be statically known. A union is always a UnionType even when it has a
	// single variant, so the two are trivially distinguishable here.
	if (source_type->IsType<MidoriType::UnionType>())
	{
		std::string suggestion = std::format("'{}' is a union; record update needs the variant to be statically known and is not yet supported on multi-variant types", source_type->GetType<MidoriType::UnionType>().m_name);
		return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Record update expression type error: multi-variant type", record_update.m_with_keyword, m_file_name, m_source_lines, std::optional<std::string_view>(suggestion)));
	}

	if (!source_type->IsType<MidoriType::StructType>())
	{
		return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Record update expression type error: not a struct", record_update.m_with_keyword, m_file_name, m_source_lines, source_type));
	}

	const MidoriType::StructType& struct_type = source_type->GetType<MidoriType::StructType>();

	// One slot per declared member, in declared order; -1 means "copy from the source".
	record_update.m_slot_sources.assign(struct_type.m_member_names.size(), -1);

	for (size_t update_index : std::views::iota(0u, record_update.m_updates.size()))
	{
		MidoriExpression::RecordUpdate::FieldUpdate& update = record_update.m_updates[update_index];

		std::vector<std::string>::const_iterator find_result = std::find(struct_type.m_member_names.cbegin(), struct_type.m_member_names.cend(), update.m_name.m_lexeme);
		if (find_result == struct_type.m_member_names.cend())
		{
			std::string suggestion = std::format("Struct '{}' does not have a member named '{}'", struct_type.m_name, update.m_name.m_lexeme);
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Unknown struct member in record update", update.m_name, m_file_name, m_source_lines, std::optional<std::string_view>(suggestion)));
		}

		update.m_index = static_cast<int>(find_result - struct_type.m_member_names.cbegin());
		record_update.m_slot_sources[static_cast<size_t>(update.m_index)] = static_cast<int>(update_index);

		std::shared_ptr<MidoriType> member_type = struct_type.m_member_types[static_cast<size_t>(update.m_index)];

		ExpectedTypeGuard guard(*this, member_type);
		MidoriResult::TypeResult value_result = Evaluate(update.m_value);
		if (!value_result.has_value())
		{
			return value_result;
		}

		MidoriResult::TypeResult unify_result = Unify(update.m_name, member_type, value_result.value(), UnifyDiagnosticMode::ExpectedActual);
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

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::Array& array)
{
	std::shared_ptr<MidoriType> expected_array_type;
	std::shared_ptr<MidoriType> expected_element_type;
	if (m_expected_expr_type != nullptr)
	{
		std::shared_ptr<MidoriType> resolved_expected_type = ApplySubstitution(m_expected_expr_type);
		if (resolved_expected_type->IsType<MidoriType::ArrayType>())
		{
			expected_array_type = resolved_expected_type;
			expected_element_type = resolved_expected_type->GetType<MidoriType::ArrayType>().m_element_type;
		}
	}

	if (array.m_elems.empty())
	{
		array.m_type_data = expected_array_type != nullptr ? expected_array_type : MidoriType::MakeArrayType(MidoriType::MakeUndecidedType());
		return array.m_type_data;
	}

	ExpectedTypeGuard element_guard(*this, expected_element_type);

	std::vector<std::shared_ptr<MidoriType>> element_results;
	element_results.reserve(array.m_elems.size());

	for (std::unique_ptr<MidoriExpression>& element : array.m_elems)
	{
		MidoriResult::TypeResult result = Evaluate(element);
		if (!result.has_value())
		{
			return result;
		}

		element_results.emplace_back(std::move(result.value()));
	}

	for (size_t idx : std::views::iota(0u, element_results.size()))
	{
		if (*element_results[0u] != *element_results[idx])
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Array expression type error: inconsistent element types", array.m_op, m_file_name, m_source_lines, element_results[idx], element_results[0u]));
		}
	}

	array.m_type_data = MidoriType::MakeArrayType(element_results[0u]);
	return array.m_type_data;
}

MidoriResult::TypeResult TypeChecker::ResolveIndexableElementType(const Token& op, const std::shared_ptr<MidoriType>& container_type, const std::shared_ptr<MidoriType>& index_type)
{
	const MidoriType::ClassConstraint constraint(std::string(INDEXABLE_CLASS_NAME), { container_type, index_type });

	std::optional<ResolvedInstanceMatch> resolved_match = FindMatchingInstance(std::string(INDEXABLE_CLASS_NAME), constraint.m_type_args);
	if (resolved_match.has_value())
	{
		AssociatedTypeEnvironment::const_iterator binding_it = resolved_match->m_instance->m_associated_type_bindings.find(std::string(ELEMENT_ASSOCIATED_TYPE_NAME));
		if (binding_it == resolved_match->m_instance->m_associated_type_bindings.cend())
		{
			return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Index expression type error: the matching Indexable instance does not bind Element", op, m_file_name, m_source_lines, container_type));
		}

		return ApplySubstitution(MidoriType::SubstituteTypeParams(binding_it->second, resolved_match->m_substitutions));
	}

	if (IsSatisfiedByActiveConstraint(constraint))
	{
		return ApplySubstitution(MidoriType::MakeAssociatedType(std::string(INDEXABLE_CLASS_NAME), std::string(ELEMENT_ASSOCIATED_TYPE_NAME), { container_type, index_type }));
	}

	return std::unexpected(MakeConstraintFailureError(op, constraint));
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::IndexAccess& array_get)
{
	return Evaluate(array_get.m_arr_var)
		.and_then
		(
			[&array_get, this](std::shared_ptr<MidoriType>&& container_type) ->MidoriResult::TypeResult
			{
				return Evaluate(array_get.m_index)
					.and_then
					(
						[&array_get, &container_type, this](std::shared_ptr<MidoriType>&& raw_index_type) ->MidoriResult::TypeResult
						{
							std::shared_ptr<MidoriType> index_type = ApplySubstitution(raw_index_type);
							container_type = ApplySubstitution(container_type);

							if (container_type->IsType<MidoriType::ArrayType>())
							{
								if (!index_type->IsType<MidoriType::IntegerType>())
								{
									return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Array get expression type error: index must be integer", array_get.m_op, m_file_name, m_source_lines, index_type, MidoriType::MakeLiteralType<MidoriType::IntegerType>()));
								}

								array_get.m_type_data = ApplySubstitution(container_type->GetType<MidoriType::ArrayType>().m_element_type);
								return array_get.m_type_data;
							}

							return ResolveIndexableElementType(array_get.m_op, container_type, index_type)
								.and_then
								(
									[&array_get, this](std::shared_ptr<MidoriType>&& element_type) ->MidoriResult::TypeResult
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

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::RangeBinary& range_binary)
{
	return Evaluate(range_binary.m_start)
		.and_then
		(
			[&range_binary, this](std::shared_ptr<MidoriType>&& start_type) -> MidoriResult::TypeResult
			{
				return Evaluate(range_binary.m_end)
					.and_then
					(
						[&start_type, &range_binary, this](std::shared_ptr<MidoriType>&& end_type) -> MidoriResult::TypeResult
						{
							return Unify(range_binary.m_range_op, start_type, end_type)
								.and_then
								(
									[&range_binary, &start_type, this](std::shared_ptr<MidoriType>&&) -> MidoriResult::TypeResult
									{
										std::shared_ptr<MidoriType> resolved_type = ApplySubstitution(start_type);

										if (!resolved_type->IsNumericType() && !resolved_type->IsType<MidoriType::TypeVariable>())
										{
											return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Range expression type error: expected numeric type", range_binary.m_range_op, m_file_name, m_source_lines, resolved_type, MidoriType::MakeLiteralType<MidoriType::IntegerType>(), MidoriType::MakeLiteralType<MidoriType::FloatType>()));
										}

										range_binary.m_type_data = MidoriType::MakeRangeType(start_type);
										return range_binary.m_type_data;
									}
								);
						}
					);
			}
		);
}

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::RangeTernary& range_ternary)
{
	return Evaluate(range_ternary.m_start)
		.and_then
		(
			[&range_ternary, this](std::shared_ptr<MidoriType>&& start_type) -> MidoriResult::TypeResult
			{
				return Evaluate(range_ternary.m_step)
					.and_then
					(
						[&start_type, &range_ternary, this](std::shared_ptr<MidoriType>&& step_type) -> MidoriResult::TypeResult
						{
							return Unify(range_ternary.m_first_range_op, start_type, step_type)
								.and_then
								(
									[&start_type, &range_ternary, this](std::shared_ptr<MidoriType>&&) -> MidoriResult::TypeResult
									{
										return Evaluate(range_ternary.m_end)
											.and_then
											(
												[&start_type, &range_ternary, this](std::shared_ptr<MidoriType>&& end_type) -> MidoriResult::TypeResult
												{
													return Unify(range_ternary.m_second_range_op, start_type, end_type)
														.and_then
														(
															[&range_ternary, &start_type, this](std::shared_ptr<MidoriType>&&) -> MidoriResult::TypeResult
															{
																std::shared_ptr<MidoriType> resolved_type = ApplySubstitution(start_type);

																if (!resolved_type->IsNumericType() && !resolved_type->IsType<MidoriType::TypeVariable>())
																{
																	return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("Range expression type error: expected numeric type", range_ternary.m_first_range_op, m_file_name, m_source_lines, resolved_type, MidoriType::MakeLiteralType<MidoriType::IntegerType>(), MidoriType::MakeLiteralType<MidoriType::FloatType>()));
																}

																range_ternary.m_type_data = MidoriType::MakeRangeType(start_type);
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

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::IfElse& if_else)
{
	return Evaluate(if_else.m_condition)
		.and_then
		(
			[&if_else, this](std::shared_ptr<MidoriType>&& actual_type) ->MidoriResult::TypeResult
			{
				std::shared_ptr<MidoriType> bool_type = MidoriType::MakeLiteralType<MidoriType::BoolType>();
				return Unify(if_else.m_if_token, bool_type, actual_type, UnifyDiagnosticMode::ExpectedActual)
					.and_then
					(
						[&if_else, this](std::shared_ptr<MidoriType>&& type)->MidoriResult::TypeResult
						{
							if (!type->IsType<MidoriType::BoolType>())
							{
								return std::unexpected(MidoriError::GenerateTypeCheckerErrorWithContext("IfElse expression type error: condition must be boolean", if_else.m_if_token, m_file_name, m_source_lines, type, MidoriType::MakeLiteralType<MidoriType::BoolType>()));
							}

							if_else.m_condition_operand_type = ResolveConditionOperandType(if_else.m_condition_operand_type, if_else.m_condition);

							auto evaluate_branch_with_expected = [this](const std::unique_ptr<MidoriExpression>& branch) -> MidoriResult::TypeResult
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
									[&if_else, &evaluate_branch_with_expected, this](std::shared_ptr<MidoriType>&& true_branch_type) ->MidoriResult::TypeResult
									{
										return evaluate_branch_with_expected(if_else.m_else_branch)
											.and_then
											(
												[&true_branch_type, &if_else, this](std::shared_ptr<MidoriType>&& else_branch_type)->MidoriResult::TypeResult
												{
													return Unify(if_else.m_else_token, true_branch_type, else_branch_type)
														.and_then
														(
															[&if_else](std::shared_ptr<MidoriType>&& type)->MidoriResult::TypeResult
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

MidoriResult::TypeResult TypeChecker::operator()(MidoriExpression::Block& block)
{
	return ScopeSession(*this).Then([&]() -> MidoriResult::TypeResult
	{
		for (const std::unique_ptr<MidoriStatement>& stmt : block.m_stmts)
		{
			ExpectedTypeGuard statement_guard(*this, std::shared_ptr<MidoriType>{});
			MidoriResult::TypeResult result = Evaluate(stmt);
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
					[&block, this](std::shared_ptr<MidoriType>&& final_value) -> MidoriResult::TypeResult
					{
						return Unify(block.m_right_brace, block.m_type_data, final_value, UnifyDiagnosticMode::ExpectedActual);
					}
				);
		}

		// Blocks without final expressions have Unit type
		if (block.m_type_data->IsType<MidoriType::UndecidedType>())
		{
			block.m_type_data = MidoriType::MakeLiteralType<MidoriType::UnitType>();
		}
		return block.m_type_data;
	});
}


