#include "GenericTypes.h"

#include <algorithm>
#include <format>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace
{
	struct TypePairHash
	{
		size_t operator()(const std::pair<MarmotType*, MarmotType*>& pair) const
		{
			return std::hash<MarmotType*>{}(pair.first) ^ (std::hash<MarmotType*>{}(pair.second) << 1u);
		}
	};

	using VisitedPairs = std::unordered_set<std::pair<MarmotType*, MarmotType*>, TypePairHash>;

	bool ContainsFreeTypeParameter(const std::shared_ptr<MarmotType>& type, std::unordered_set<const MarmotType*>& visited)
	{
		if (!type)
		{
			return false;
		}
		if (!visited.insert(type.get()).second)
		{
			return false;
		}

		if (type->IsType<MarmotType::GenericParam>() || type->IsType<MarmotType::TypeVariable>() || type->IsType<MarmotType::UndecidedType>())
		{
			return true;
		}
		if (type->IsType<MarmotType::ArrayType>())
		{
			return ContainsFreeTypeParameter(type->GetType<MarmotType::ArrayType>().m_element_type, visited);
		}
		if (type->IsType<MarmotType::RangeType>())
		{
			return ContainsFreeTypeParameter(type->GetType<MarmotType::RangeType>().m_element_type, visited);
		}
		if (type->IsType<MarmotType::WorkerType>())
		{
			return ContainsFreeTypeParameter(type->GetType<MarmotType::WorkerType>().m_result_type, visited);
		}
		if (type->IsType<MarmotType::ChannelType>())
		{
			return ContainsFreeTypeParameter(type->GetType<MarmotType::ChannelType>().m_element_type, visited);
		}

		if (type->IsType<MarmotType::CellType>())
		{
			return ContainsFreeTypeParameter(type->GetType<MarmotType::CellType>().m_element_type, visited);
		}
		if (type->IsType<MarmotType::TupleType>())
		{
			const MarmotType::TupleType& tuple_type = type->GetType<MarmotType::TupleType>();
			return std::ranges::any_of
			(
				tuple_type.m_element_types,
				[&visited](const std::shared_ptr<MarmotType>& element_type) -> bool
				{
					return ContainsFreeTypeParameter(element_type, visited);
				}
			);
		}
		if (type->IsType<MarmotType::FunctionType>())
		{
			const MarmotType::FunctionType& function_type = type->GetType<MarmotType::FunctionType>();
			if (ContainsFreeTypeParameter(function_type.m_return_type, visited))
			{
				return true;
			}
			return std::ranges::any_of
			(
				function_type.m_param_types,
				[&visited](const std::shared_ptr<MarmotType>& param_type) -> bool
				{
					return ContainsFreeTypeParameter(param_type, visited);
				}
			);
		}
		if (type->IsType<MarmotType::StructType>())
		{
			const MarmotType::StructType& struct_type = type->GetType<MarmotType::StructType>();
			return std::ranges::any_of
			(
				struct_type.m_member_types,
				[&visited](const std::shared_ptr<MarmotType>& member_type) -> bool
				{
					return ContainsFreeTypeParameter(member_type, visited);
				}
			);
		}
		if (type->IsType<MarmotType::UnionType>())
		{
			const MarmotType::UnionType& union_type = type->GetType<MarmotType::UnionType>();
			return std::ranges::any_of
			(
				union_type.m_member_info,
				[&visited](const std::pair<const std::string, MarmotType::UnionType::UnionMemberContext>& member) -> bool
				{
					return std::ranges::any_of
					(
						member.second.m_member_types,
						[&visited](const std::shared_ptr<MarmotType>& member_type) -> bool
						{
							return ContainsFreeTypeParameter(member_type, visited);
						}
					);
				}
			);
		}
		if (type->IsType<MarmotType::AssociatedType>())
		{
			const MarmotType::AssociatedType& associated_type = type->GetType<MarmotType::AssociatedType>();
			return std::ranges::any_of
			(
				associated_type.m_type_args,
				[&visited](const std::shared_ptr<MarmotType>& type_arg) -> bool
				{
					return ContainsFreeTypeParameter(type_arg, visited);
				}
			);
		}
		if (type->IsType<MarmotType::NewType>())
		{
			return ContainsFreeTypeParameter(type->GetType<MarmotType::NewType>().m_representation, visited);
		}

		return false;
	}

	void DeduceRecursive(const std::shared_ptr<MarmotType>& param_type, const std::shared_ptr<MarmotType>& concrete_type, GenericTypes::TypeEnvironment& map, VisitedPairs& visited)
	{
		if (!param_type || !concrete_type)
		{
			return;
		}
		if (param_type.get() == concrete_type.get())
		{
			return;
		}
		if (visited.contains({param_type.get(), concrete_type.get()}))
		{
			return;
		}
		visited.insert({ param_type.get(), concrete_type.get() });

		struct DeduceGenericVisitor
		{
			const std::shared_ptr<MarmotType>& m_param_type;
			const std::shared_ptr<MarmotType>& m_concrete_type;
			std::unordered_map<std::string, std::shared_ptr<MarmotType>>& m_map;
			VisitedPairs& m_visited;

			void operator()(const MarmotType::GenericParam& p_var) const
			{
				m_map[p_var.m_name] = m_concrete_type;
			}

			void operator()(const MarmotType::TypeVariable&) const
			{
				m_map[m_param_type->ToString()] = m_concrete_type;
			}

			void operator()(const MarmotType::ArrayType& p_var) const
			{
				if (m_concrete_type->IsType<MarmotType::ArrayType>())
				{
					DeduceRecursive(p_var.m_element_type, m_concrete_type->GetType<MarmotType::ArrayType>().m_element_type, m_map, m_visited);
				}
			}

			void operator()(const MarmotType::WorkerType& p_var) const
			{
				if (m_concrete_type->IsType<MarmotType::WorkerType>())
				{
					DeduceRecursive(p_var.m_result_type, m_concrete_type->GetType<MarmotType::WorkerType>().m_result_type, m_map, m_visited);
				}
			}

			void operator()(const MarmotType::ChannelType& p_var) const
			{
				if (m_concrete_type->IsType<MarmotType::ChannelType>())
				{
					DeduceRecursive(p_var.m_element_type, m_concrete_type->GetType<MarmotType::ChannelType>().m_element_type, m_map, m_visited);
				}
			}

			void operator()(const MarmotType::CellType& p_var) const
			{
				if (m_concrete_type->IsType<MarmotType::CellType>())
				{
					DeduceRecursive(p_var.m_element_type, m_concrete_type->GetType<MarmotType::CellType>().m_element_type, m_map, m_visited);
				}
			}

			void operator()(const MarmotType::StructType& p_var) const
			{
				if (m_concrete_type->IsType<MarmotType::StructType>())
				{
					const MarmotType::StructType& c_struct = m_concrete_type->GetType<MarmotType::StructType>();
					if (p_var.m_member_types.size() == c_struct.m_member_types.size())
					{
						for (size_t i = 0uz; i < p_var.m_member_types.size(); i += 1uz)
						{
							DeduceRecursive(p_var.m_member_types[i], c_struct.m_member_types[i], m_map, m_visited);
						}
					}
				}
			}

			void operator()(const MarmotType::FunctionType& p_var) const
			{
				if (m_concrete_type->IsType<MarmotType::FunctionType>())
				{
					const MarmotType::FunctionType& c_func = m_concrete_type->GetType<MarmotType::FunctionType>();
					DeduceRecursive(p_var.m_return_type, c_func.m_return_type, m_map, m_visited);
					if (p_var.m_param_types.size() == c_func.m_param_types.size())
					{
						for (size_t i = 0uz; i < p_var.m_param_types.size(); i += 1uz)
						{
							DeduceRecursive(p_var.m_param_types[i], c_func.m_param_types[i], m_map, m_visited);
						}
					}
				}
			}

			void operator()(const MarmotType::TupleType& p_var) const
			{
				if (m_concrete_type->IsType<MarmotType::TupleType>())
				{
					const MarmotType::TupleType& c_tuple = m_concrete_type->GetType<MarmotType::TupleType>();
					if (p_var.m_element_types.size() == c_tuple.m_element_types.size())
					{
						for (size_t i = 0uz; i < p_var.m_element_types.size(); i += 1uz)
						{
							DeduceRecursive(p_var.m_element_types[i], c_tuple.m_element_types[i], m_map, m_visited);
						}
					}
				}
			}

			void operator()(const MarmotType::UnionType& p_var) const
			{
				if (m_concrete_type->IsType<MarmotType::UnionType>())
				{
					const MarmotType::UnionType& c_union = m_concrete_type->GetType<MarmotType::UnionType>();
					for (const auto& [name, ctx] : p_var.m_member_info)
					{
						if (c_union.m_member_info.contains(name))
						{
							const MarmotType::UnionType::UnionMemberContext& c_ctx = c_union.m_member_info.at(name);
							if (ctx.m_member_types.size() == c_ctx.m_member_types.size())
							{
								for (size_t i = 0uz; i < ctx.m_member_types.size(); i += 1uz)
								{
									DeduceRecursive(ctx.m_member_types[i], c_ctx.m_member_types[i], m_map, m_visited);
								}
							}
						}
					}
				}
			}
			void operator()(const MarmotType::AssociatedType& p_var) const
			{
				if (m_concrete_type->IsType<MarmotType::AssociatedType>())
				{
					const MarmotType::AssociatedType& c_associated = m_concrete_type->GetType<MarmotType::AssociatedType>();
					if (p_var.m_class_name == c_associated.m_class_name &&
						p_var.m_name == c_associated.m_name &&
						p_var.m_type_args.size() == c_associated.m_type_args.size())
					{
						for (size_t i = 0uz; i < p_var.m_type_args.size(); i += 1uz)
						{
							DeduceRecursive(p_var.m_type_args[i], c_associated.m_type_args[i], m_map, m_visited);
						}
					}
				}
			}

			void operator()(const MarmotType::UndecidedType&) const {}
			void operator()(const MarmotType::FloatType&) const {}
			void operator()(const MarmotType::IntegerType&) const {}
			void operator()(const MarmotType::ByteType&) const {}
			void operator()(const MarmotType::WordType&) const {}
			void operator()(const MarmotType::TextType&) const {}
			void operator()(const MarmotType::BoolType&) const {}
			void operator()(const MarmotType::UnitType&) const {}
			void operator()(const MarmotType::NeverType&) const {}
			void operator()(const MarmotType::RangeType& p_var) const
			{
				if (m_concrete_type->IsType<MarmotType::RangeType>())
				{
					DeduceRecursive(p_var.m_element_type, m_concrete_type->GetType<MarmotType::RangeType>().m_element_type, m_map, m_visited);
				}
			}
			void operator()(const MarmotType::ClassConstraint&) const {}
			void operator()(const MarmotType::NewType& p_var) const
			{
				if (m_concrete_type->IsType<MarmotType::NewType>())
				{
					const MarmotType::NewType& c_newtype = m_concrete_type->GetType<MarmotType::NewType>();
					DeduceRecursive(p_var.m_representation, c_newtype.m_representation, m_map, m_visited);
					if (p_var.m_type_arguments.size() == c_newtype.m_type_arguments.size())
					{
						for (size_t i = 0uz; i < p_var.m_type_arguments.size(); i += 1uz)
						{
							DeduceRecursive(p_var.m_type_arguments[i], c_newtype.m_type_arguments[i], m_map, m_visited);
						}
					}
				}
			}
		};

		std::visit(DeduceGenericVisitor{ param_type, concrete_type, map, visited }, param_type->m_type);
	}
}

void GenericTypes::Deduce(const std::shared_ptr<MarmotType>& pattern, const std::shared_ptr<MarmotType>& concrete, TypeEnvironment& map)
{
	VisitedPairs visited;
	DeduceRecursive(pattern, concrete, map, visited);
}

std::shared_ptr<MarmotType> GenericTypes::RepresentationOf(const std::shared_ptr<MarmotType>& type)
{
	std::shared_ptr<MarmotType> current = type;
	while (current != nullptr && current->IsType<MarmotType::NewType>())
	{
		current = current->GetType<MarmotType::NewType>().m_representation;
	}

	return current;
}

bool GenericTypes::IsPointer(const std::shared_ptr<MarmotType>& type)
{
	const std::shared_ptr<MarmotType> representation = RepresentationOf(type);
	if (representation->IsType<MarmotType::TextType>() || representation->IsType<MarmotType::ArrayType>() || representation->IsType<MarmotType::TupleType>()
		|| representation->IsType<MarmotType::StructType>() || representation->IsType<MarmotType::UnionType>() || representation->IsType<MarmotType::FunctionType>()
		|| representation->IsType<MarmotType::CellType>() || representation->IsType<MarmotType::RangeType>())
	{
		return true;
	}
	if (representation->IsType<MarmotType::IntegerType>() || representation->IsType<MarmotType::FloatType>() || representation->IsType<MarmotType::ByteType>()
		|| representation->IsType<MarmotType::WordType>() || representation->IsType<MarmotType::BoolType>() || representation->IsType<MarmotType::UnitType>()
		|| representation->IsType<MarmotType::NeverType>() || representation->IsType<MarmotType::WorkerType>() || representation->IsType<MarmotType::ChannelType>())
	{
		return false;
	}
	throw std::logic_error(std::format("cannot tell whether a {} is a pointer", type->ToString()));
}

bool GenericTypes::IsNewTypeErasedConversion(const std::shared_ptr<MarmotType>& from_type, const std::shared_ptr<MarmotType>& target_type)
{
	if (from_type == nullptr || target_type == nullptr)
	{
		return false;
	}

	if (!from_type->IsType<MarmotType::NewType>() && !target_type->IsType<MarmotType::NewType>())
	{
		return false;
	}

	std::shared_ptr<MarmotType> from_representation = RepresentationOf(from_type);
	std::shared_ptr<MarmotType> target_representation = RepresentationOf(target_type);
	if (from_representation == nullptr || target_representation == nullptr)
	{
		return false;
	}

	return *from_representation == *target_representation;
}

bool GenericTypes::IsGenericInstanceHead(const std::vector<std::shared_ptr<MarmotType>>& type_args)
{
	std::unordered_set<const MarmotType*> visited;
	return std::ranges::any_of(type_args, [&visited](const std::shared_ptr<MarmotType>& type_arg) { return ContainsFreeTypeParameter(type_arg, visited); });
}

bool GenericTypes::IsConcrete(const std::shared_ptr<MarmotType>& type)
{
	std::unordered_set<const MarmotType*> visited;
	return !ContainsFreeTypeParameter(type, visited);
}
