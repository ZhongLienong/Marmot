#include "GenericTypes.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <unordered_set>
#include <utility>

namespace
{
	struct TypePairHash
	{
		size_t operator()(const std::pair<MidoriType*, MidoriType*>& pair) const
		{
			return std::hash<MidoriType*>{}(pair.first) ^ (std::hash<MidoriType*>{}(pair.second) << 1u);
		}
	};

	using VisitedPairs = std::unordered_set<std::pair<MidoriType*, MidoriType*>, TypePairHash>;

	bool ContainsFreeTypeParameter(const std::shared_ptr<MidoriType>& type, std::unordered_set<const MidoriType*>& visited)
	{
		if (!type)
		{
			return false;
		}
		if (!visited.insert(type.get()).second)
		{
			return false;
		}

		if (type->IsType<MidoriType::GenericParam>() || type->IsType<MidoriType::TypeVariable>() || type->IsType<MidoriType::UndecidedType>())
		{
			return true;
		}
		if (type->IsType<MidoriType::ArrayType>())
		{
			return ContainsFreeTypeParameter(type->GetType<MidoriType::ArrayType>().m_element_type, visited);
		}
		if (type->IsType<MidoriType::RangeType>())
		{
			return ContainsFreeTypeParameter(type->GetType<MidoriType::RangeType>().m_element_type, visited);
		}
		if (type->IsType<MidoriType::WorkerType>())
		{
			return ContainsFreeTypeParameter(type->GetType<MidoriType::WorkerType>().m_result_type, visited);
		}
		if (type->IsType<MidoriType::ChannelType>())
		{
			return ContainsFreeTypeParameter(type->GetType<MidoriType::ChannelType>().m_element_type, visited);
		}

		if (type->IsType<MidoriType::CellType>())
		{
			return ContainsFreeTypeParameter(type->GetType<MidoriType::CellType>().m_element_type, visited);
		}
		if (type->IsType<MidoriType::TupleType>())
		{
			const MidoriType::TupleType& tuple_type = type->GetType<MidoriType::TupleType>();
			return std::ranges::any_of
			(
				tuple_type.m_element_types,
				[&visited](const std::shared_ptr<MidoriType>& element_type) -> bool
				{
					return ContainsFreeTypeParameter(element_type, visited);
				}
			);
		}
		if (type->IsType<MidoriType::FunctionType>())
		{
			const MidoriType::FunctionType& function_type = type->GetType<MidoriType::FunctionType>();
			if (ContainsFreeTypeParameter(function_type.m_return_type, visited))
			{
				return true;
			}
			return std::ranges::any_of
			(
				function_type.m_param_types,
				[&visited](const std::shared_ptr<MidoriType>& param_type) -> bool
				{
					return ContainsFreeTypeParameter(param_type, visited);
				}
			);
		}
		if (type->IsType<MidoriType::StructType>())
		{
			const MidoriType::StructType& struct_type = type->GetType<MidoriType::StructType>();
			return std::ranges::any_of
			(
				struct_type.m_member_types,
				[&visited](const std::shared_ptr<MidoriType>& member_type) -> bool
				{
					return ContainsFreeTypeParameter(member_type, visited);
				}
			);
		}
		if (type->IsType<MidoriType::UnionType>())
		{
			const MidoriType::UnionType& union_type = type->GetType<MidoriType::UnionType>();
			return std::ranges::any_of
			(
				union_type.m_member_info,
				[&visited](const std::pair<const std::string, MidoriType::UnionType::UnionMemberContext>& member) -> bool
				{
					return std::ranges::any_of
					(
						member.second.m_member_types,
						[&visited](const std::shared_ptr<MidoriType>& member_type) -> bool
						{
							return ContainsFreeTypeParameter(member_type, visited);
						}
					);
				}
			);
		}
		if (type->IsType<MidoriType::AssociatedType>())
		{
			const MidoriType::AssociatedType& associated_type = type->GetType<MidoriType::AssociatedType>();
			return std::ranges::any_of
			(
				associated_type.m_type_args,
				[&visited](const std::shared_ptr<MidoriType>& type_arg) -> bool
				{
					return ContainsFreeTypeParameter(type_arg, visited);
				}
			);
		}

		return false;
	}

	void DeduceRecursive(const std::shared_ptr<MidoriType>& param_type, const std::shared_ptr<MidoriType>& concrete_type, GenericTypes::TypeEnvironment& map, VisitedPairs& visited)
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
			const std::shared_ptr<MidoriType>& m_param_type;
			const std::shared_ptr<MidoriType>& m_concrete_type;
			std::unordered_map<std::string, std::shared_ptr<MidoriType>>& m_map;
			VisitedPairs& m_visited;

			void operator()(const MidoriType::GenericParam& p_var) const
			{
				m_map[p_var.m_name] = m_concrete_type;
			}

			void operator()(const MidoriType::TypeVariable&) const
			{
				m_map[m_param_type->ToString()] = m_concrete_type;
			}

			void operator()(const MidoriType::ArrayType& p_var) const
			{
				if (m_concrete_type->IsType<MidoriType::ArrayType>())
				{
					DeduceRecursive(p_var.m_element_type, m_concrete_type->GetType<MidoriType::ArrayType>().m_element_type, m_map, m_visited);
				}
			}

			void operator()(const MidoriType::WorkerType& p_var) const
			{
				if (m_concrete_type->IsType<MidoriType::WorkerType>())
				{
					DeduceRecursive(p_var.m_result_type, m_concrete_type->GetType<MidoriType::WorkerType>().m_result_type, m_map, m_visited);
				}
			}

			void operator()(const MidoriType::ChannelType& p_var) const
			{
				if (m_concrete_type->IsType<MidoriType::ChannelType>())
				{
					DeduceRecursive(p_var.m_element_type, m_concrete_type->GetType<MidoriType::ChannelType>().m_element_type, m_map, m_visited);
				}
			}

			void operator()(const MidoriType::CellType& p_var) const
			{
				if (m_concrete_type->IsType<MidoriType::CellType>())
				{
					DeduceRecursive(p_var.m_element_type, m_concrete_type->GetType<MidoriType::CellType>().m_element_type, m_map, m_visited);
				}
			}

			void operator()(const MidoriType::StructType& p_var) const
			{
				if (m_concrete_type->IsType<MidoriType::StructType>())
				{
					const MidoriType::StructType& c_struct = m_concrete_type->GetType<MidoriType::StructType>();
					if (p_var.m_member_types.size() == c_struct.m_member_types.size())
					{
						for (size_t i = 0uz; i < p_var.m_member_types.size(); i += 1uz)
						{
							DeduceRecursive(p_var.m_member_types[i], c_struct.m_member_types[i], m_map, m_visited);
						}
					}
				}
			}

			void operator()(const MidoriType::FunctionType& p_var) const
			{
				if (m_concrete_type->IsType<MidoriType::FunctionType>())
				{
					const MidoriType::FunctionType& c_func = m_concrete_type->GetType<MidoriType::FunctionType>();
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

			void operator()(const MidoriType::TupleType& p_var) const
			{
				if (m_concrete_type->IsType<MidoriType::TupleType>())
				{
					const MidoriType::TupleType& c_tuple = m_concrete_type->GetType<MidoriType::TupleType>();
					if (p_var.m_element_types.size() == c_tuple.m_element_types.size())
					{
						for (size_t i = 0uz; i < p_var.m_element_types.size(); i += 1uz)
						{
							DeduceRecursive(p_var.m_element_types[i], c_tuple.m_element_types[i], m_map, m_visited);
						}
					}
				}
			}

			void operator()(const MidoriType::UnionType& p_var) const
			{
				if (m_concrete_type->IsType<MidoriType::UnionType>())
				{
					const MidoriType::UnionType& c_union = m_concrete_type->GetType<MidoriType::UnionType>();
					for (const auto& [name, ctx] : p_var.m_member_info)
					{
						if (c_union.m_member_info.contains(name))
						{
							const MidoriType::UnionType::UnionMemberContext& c_ctx = c_union.m_member_info.at(name);
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
			void operator()(const MidoriType::AssociatedType& p_var) const
			{
				if (m_concrete_type->IsType<MidoriType::AssociatedType>())
				{
					const MidoriType::AssociatedType& c_associated = m_concrete_type->GetType<MidoriType::AssociatedType>();
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

			void operator()(const MidoriType::UndecidedType&) const {}
			void operator()(const MidoriType::FloatType&) const {}
			void operator()(const MidoriType::IntegerType&) const {}
			void operator()(const MidoriType::ByteType&) const {}
			void operator()(const MidoriType::WordType&) const {}
			void operator()(const MidoriType::TextType&) const {}
			void operator()(const MidoriType::BoolType&) const {}
			void operator()(const MidoriType::UnitType&) const {}
			void operator()(const MidoriType::NeverType&) const {}
			void operator()(const MidoriType::RangeType& p_var) const
			{
				if (m_concrete_type->IsType<MidoriType::RangeType>())
				{
					DeduceRecursive(p_var.m_element_type, m_concrete_type->GetType<MidoriType::RangeType>().m_element_type, m_map, m_visited);
				}
			}
			void operator()(const MidoriType::ClassConstraint&) const {}
			void operator()(const MidoriType::NewType& p_var) const
			{
				if (m_concrete_type->IsType<MidoriType::NewType>())
				{
					const MidoriType::NewType& c_newtype = m_concrete_type->GetType<MidoriType::NewType>();
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

void GenericTypes::Deduce(const std::shared_ptr<MidoriType>& pattern, const std::shared_ptr<MidoriType>& concrete, TypeEnvironment& map)
{
	VisitedPairs visited;
	DeduceRecursive(pattern, concrete, map, visited);
}

std::shared_ptr<MidoriType> GenericTypes::Substitute(const std::shared_ptr<MidoriType>& type, const TypeEnvironment& generic_type_map)
{
	return Substitute(type, generic_type_map, AssociatedTypeResolver());
}

std::shared_ptr<MidoriType> GenericTypes::Substitute(const std::shared_ptr<MidoriType>& type, const TypeEnvironment& generic_type_map, const AssociatedTypeResolver& resolve)
{
	using SubstituteFn = std::function<std::shared_ptr<MidoriType>(const std::shared_ptr<MidoriType>&)>;

	std::unordered_map<const MidoriType*, std::shared_ptr<MidoriType>> cache;
	std::unordered_set<const MidoriType*> visiting;

	SubstituteFn substitute;

	struct SubstituteVisitor
	{
		const TypeEnvironment& m_generic_type_map;
		std::unordered_map<const MidoriType*, std::shared_ptr<MidoriType>>& m_cache;
		SubstituteFn& m_substitute;
		const std::shared_ptr<MidoriType>& m_current;
		const AssociatedTypeResolver& m_resolve;

		std::shared_ptr<MidoriType> operator()(const MidoriType::GenericParam& type_variant) const
		{
			TypeEnvironment::const_iterator it = m_generic_type_map.find(type_variant.m_name);
			if (it != m_generic_type_map.end())
			{
				return it->second;
			}
			return m_current;
		}

		std::shared_ptr<MidoriType> operator()(const MidoriType::TypeVariable&) const
		{
			TypeEnvironment::const_iterator it = m_generic_type_map.find(m_current->ToString());
			if (it != m_generic_type_map.end())
			{
				return it->second;
			}
			return m_current;
		}

		std::shared_ptr<MidoriType> operator()(const MidoriType::ArrayType& type_variant) const
		{
			std::shared_ptr<MidoriType> substituted_element = m_substitute(type_variant.m_element_type);
			if (substituted_element != type_variant.m_element_type)
			{
				return std::make_shared<MidoriType>(MidoriType::ArrayType{ substituted_element });
			}
			return m_current;
		}

		std::shared_ptr<MidoriType> operator()(const MidoriType::WorkerType& type_variant) const
		{
			std::shared_ptr<MidoriType> substituted_result = m_substitute(type_variant.m_result_type);
			if (substituted_result != type_variant.m_result_type)
			{
				return MidoriType::MakeWorkerType(substituted_result);
			}
			return m_current;
		}

		std::shared_ptr<MidoriType> operator()(const MidoriType::ChannelType& type_variant) const
		{
			std::shared_ptr<MidoriType> substituted_element = m_substitute(type_variant.m_element_type);
			if (substituted_element != type_variant.m_element_type)
			{
				return MidoriType::MakeChannelType(substituted_element);
			}
			return m_current;
		}

		std::shared_ptr<MidoriType> operator()(const MidoriType::CellType& type_variant) const
		{
			std::shared_ptr<MidoriType> substituted_element = m_substitute(type_variant.m_element_type);
			if (substituted_element != type_variant.m_element_type)
			{
				return MidoriType::MakeCellType(substituted_element);
			}
			return m_current;
		}

		std::shared_ptr<MidoriType> operator()(const MidoriType::TupleType& type_variant) const
		{
			std::vector<std::shared_ptr<MidoriType>> substituted_elements;
			bool changed = false;
			for (const std::shared_ptr<MidoriType>& elem_type : type_variant.m_element_types)
			{
				std::shared_ptr<MidoriType> substituted = m_substitute(elem_type);
				substituted_elements.push_back(substituted);
				if (substituted != elem_type)
				{
					changed = true;
				}
			}
			if (changed)
			{
				return std::make_shared<MidoriType>(MidoriType::TupleType{ std::move(substituted_elements) });
			}
			return m_current;
		}

		std::shared_ptr<MidoriType> operator()(const MidoriType::FunctionType& type_variant) const
		{
			std::vector<std::shared_ptr<MidoriType>> substituted_params;
			bool changed = false;
			for (const std::shared_ptr<MidoriType>& param_type : type_variant.m_param_types)
			{
				std::shared_ptr<MidoriType> substituted = m_substitute(param_type);
				substituted_params.push_back(substituted);
				if (substituted != param_type)
				{
					changed = true;
				}
			}
			std::shared_ptr<MidoriType> substituted_return = m_substitute(type_variant.m_return_type);
			if (substituted_return != type_variant.m_return_type)
			{
				changed = true;
			}
			std::vector<MidoriType::ClassConstraint> substituted_constraints;
			substituted_constraints.reserve(type_variant.m_constraints.size());
			for (const MidoriType::ClassConstraint& constraint : type_variant.m_constraints)
			{
				std::vector<std::shared_ptr<MidoriType>> substituted_type_args;
				substituted_type_args.reserve(constraint.m_type_args.size());
				for (const std::shared_ptr<MidoriType>& type_arg : constraint.m_type_args)
				{
					std::shared_ptr<MidoriType> substituted = m_substitute(type_arg);
					substituted_type_args.push_back(substituted);
					if (substituted != type_arg)
					{
						changed = true;
					}
				}
				substituted_constraints.emplace_back(constraint.m_class_name, std::move(substituted_type_args));
			}
			if (changed)
			{
				return std::make_shared<MidoriType>
				(
					MidoriType::FunctionType
					{
						.m_param_types = std::move(substituted_params),
						.m_return_type = substituted_return,
						.m_constraints = std::move(substituted_constraints),
						.m_is_foreign = type_variant.m_is_foreign
					}
				);
			}
			return m_current;
		}

		std::shared_ptr<MidoriType> operator()(const MidoriType::StructType& type_variant) const
		{
			std::vector<std::shared_ptr<MidoriType>> empty_member_types;
			std::vector<std::string> member_names_copy = type_variant.m_member_names;
			std::vector<std::string> instantiated_generic_params;
			std::shared_ptr<MidoriType> new_struct = MidoriType::MakeStructType(type_variant.m_name, type_variant.m_module_name, std::move(empty_member_types), std::move(member_names_copy), std::move(instantiated_generic_params));
			m_cache[m_current.get()] = new_struct;

			std::vector<std::shared_ptr<MidoriType>> substituted_members;
			std::ranges::transform(type_variant.m_member_types, std::back_inserter(substituted_members), m_substitute);
			new_struct->GetType<MidoriType::StructType>().m_member_types = std::move(substituted_members);
			std::vector<MidoriType::ClassConstraint> substituted_constraints;
			substituted_constraints.reserve(type_variant.m_constraints.size());
			for (const MidoriType::ClassConstraint& constraint : type_variant.m_constraints)
			{
				std::vector<std::shared_ptr<MidoriType>> substituted_type_args;
				substituted_type_args.reserve(constraint.m_type_args.size());
				std::ranges::transform(constraint.m_type_args, std::back_inserter(substituted_type_args), m_substitute);
				substituted_constraints.emplace_back(constraint.m_class_name, std::move(substituted_type_args));
			}
			new_struct->GetType<MidoriType::StructType>().m_constraints = std::move(substituted_constraints);
			if (!type_variant.m_generic_params.empty() || type_variant.m_is_generic_instantiation)
			{
				new_struct->GetType<MidoriType::StructType>().m_is_generic_instantiation = true;
				new_struct->GetType<MidoriType::StructType>().m_type_arguments = MidoriType::InstantiateTypeArguments(type_variant.m_generic_params, type_variant.m_type_arguments, m_substitute);
			}
			return new_struct;
		}

		std::shared_ptr<MidoriType> operator()(const MidoriType::UnionType& type_variant) const
		{
			std::vector<std::string> instantiated_generic_params;
			std::shared_ptr<MidoriType> new_union = MidoriType::MakeUnionType(type_variant.m_name, type_variant.m_module_name, std::move(instantiated_generic_params));
			m_cache[m_current.get()] = new_union;
			MidoriType::UnionType& new_union_ref = new_union->GetType<MidoriType::UnionType>();
			std::vector<MidoriType::ClassConstraint> substituted_constraints;
			substituted_constraints.reserve(type_variant.m_constraints.size());
			for (const MidoriType::ClassConstraint& constraint : type_variant.m_constraints)
			{
				std::vector<std::shared_ptr<MidoriType>> substituted_type_args;
				substituted_type_args.reserve(constraint.m_type_args.size());
				std::ranges::transform(constraint.m_type_args, std::back_inserter(substituted_type_args), m_substitute);
				substituted_constraints.emplace_back(constraint.m_class_name, std::move(substituted_type_args));
			}
			new_union_ref.m_constraints = std::move(substituted_constraints);
			if (!type_variant.m_generic_params.empty() || type_variant.m_is_generic_instantiation)
			{
				new_union_ref.m_is_generic_instantiation = true;
				new_union_ref.m_type_arguments = MidoriType::InstantiateTypeArguments(type_variant.m_generic_params, type_variant.m_type_arguments, m_substitute);
			}

			for (const auto& [member_name, member_ctx] : type_variant.m_member_info)
			{
				std::vector<std::shared_ptr<MidoriType>> substituted_members;
				std::ranges::transform(member_ctx.m_member_types, std::back_inserter(substituted_members), m_substitute);
				new_union_ref.m_member_info.emplace(member_name, MidoriType::UnionType::UnionMemberContext{ std::move(substituted_members), member_ctx.m_tag });
			}
			return new_union;
		}
		std::shared_ptr<MidoriType> operator()(const MidoriType::AssociatedType& type_variant) const
		{
			std::vector<std::shared_ptr<MidoriType>> substituted_type_args;
			bool changed = false;
			for (const std::shared_ptr<MidoriType>& type_arg : type_variant.m_type_args)
			{
				std::shared_ptr<MidoriType> substituted = m_substitute(type_arg);
				substituted_type_args.push_back(substituted);
				if (substituted != type_arg)
				{
					changed = true;
				}
			}
			const std::shared_ptr<MidoriType> projection = changed ? MidoriType::MakeAssociatedType(type_variant.m_class_name, type_variant.m_name, std::move(substituted_type_args)) : m_current;
			if (!m_resolve)
			{
				return projection;
			}
			return m_resolve(projection->GetType<MidoriType::AssociatedType>()).value_or(projection);
		}

		std::shared_ptr<MidoriType> operator()(const MidoriType::NewType& type_variant) const
		{
			std::shared_ptr<MidoriType> substituted_representation = m_substitute(type_variant.m_representation);
			std::vector<std::string> instantiated_generic_params;
			std::shared_ptr<MidoriType> new_newtype = MidoriType::MakeNewType(type_variant.m_name, type_variant.m_module_name, substituted_representation, std::move(instantiated_generic_params));
			m_cache[m_current.get()] = new_newtype;
			MidoriType::NewType& new_newtype_ref = new_newtype->GetType<MidoriType::NewType>();
			std::vector<MidoriType::ClassConstraint> substituted_constraints;
			substituted_constraints.reserve(type_variant.m_constraints.size());
			for (const MidoriType::ClassConstraint& constraint : type_variant.m_constraints)
			{
				std::vector<std::shared_ptr<MidoriType>> substituted_type_args;
				substituted_type_args.reserve(constraint.m_type_args.size());
				std::ranges::transform(constraint.m_type_args, std::back_inserter(substituted_type_args), m_substitute);
				substituted_constraints.emplace_back(constraint.m_class_name, std::move(substituted_type_args));
			}
			new_newtype_ref.m_constraints = std::move(substituted_constraints);
			if (!type_variant.m_generic_params.empty() || type_variant.m_is_generic_instantiation)
			{
				new_newtype_ref.m_is_generic_instantiation = true;
				new_newtype_ref.m_type_arguments = MidoriType::InstantiateTypeArguments(type_variant.m_generic_params, type_variant.m_type_arguments, m_substitute);
			}
			return new_newtype;
		}

		std::shared_ptr<MidoriType> operator()(const MidoriType::UndecidedType&) const { return m_current; }
		std::shared_ptr<MidoriType> operator()(const MidoriType::FloatType&) const { return m_current; }
		std::shared_ptr<MidoriType> operator()(const MidoriType::IntegerType&) const { return m_current; }
		std::shared_ptr<MidoriType> operator()(const MidoriType::ByteType&) const { return m_current; }
		std::shared_ptr<MidoriType> operator()(const MidoriType::WordType&) const { return m_current; }
		std::shared_ptr<MidoriType> operator()(const MidoriType::TextType&) const { return m_current; }
		std::shared_ptr<MidoriType> operator()(const MidoriType::BoolType&) const { return m_current; }
		std::shared_ptr<MidoriType> operator()(const MidoriType::UnitType&) const { return m_current; }
		std::shared_ptr<MidoriType> operator()(const MidoriType::NeverType&) const { return m_current; }
		std::shared_ptr<MidoriType> operator()(const MidoriType::RangeType& type_variant) const
		{
			std::shared_ptr<MidoriType> substituted_element = m_substitute(type_variant.m_element_type);
			if (substituted_element != type_variant.m_element_type)
			{
				return MidoriType::MakeRangeType(substituted_element);
			}
			return m_current;
		}
		std::shared_ptr<MidoriType> operator()(const MidoriType::ClassConstraint&) const { return m_current; }
	};

	substitute = [&generic_type_map, &cache, &visiting, &substitute, &resolve](const std::shared_ptr<MidoriType>& current) -> std::shared_ptr<MidoriType>
	{
		if (!current)
		{
			return current;
		}

		std::unordered_map<const MidoriType*, std::shared_ptr<MidoriType>>::iterator cache_it = cache.find(current.get());
		if (cache_it != cache.end())
		{
			return cache_it->second;
		}

		if (visiting.contains(current.get()))
		{
			return current;
		}

		visiting.insert(current.get());

		SubstituteVisitor visitor{ generic_type_map, cache, substitute, current, resolve };
		std::shared_ptr<MidoriType> result = std::visit(visitor, current->m_type);

		visiting.erase(current.get());
		return result;
	};

	return substitute(type);
}

std::shared_ptr<MidoriType> GenericTypes::RepresentationOf(const std::shared_ptr<MidoriType>& type)
{
	std::shared_ptr<MidoriType> current = type;
	while (current != nullptr && current->IsType<MidoriType::NewType>())
	{
		current = current->GetType<MidoriType::NewType>().m_representation;
	}

	return current;
}

bool GenericTypes::IsNewTypeErasedConversion(const std::shared_ptr<MidoriType>& from_type, const std::shared_ptr<MidoriType>& target_type)
{
	if (from_type == nullptr || target_type == nullptr)
	{
		return false;
	}

	if (!from_type->IsType<MidoriType::NewType>() && !target_type->IsType<MidoriType::NewType>())
	{
		return false;
	}

	std::shared_ptr<MidoriType> from_representation = RepresentationOf(from_type);
	std::shared_ptr<MidoriType> target_representation = RepresentationOf(target_type);
	if (from_representation == nullptr || target_representation == nullptr)
	{
		return false;
	}

	return *from_representation == *target_representation;
}

bool GenericTypes::IsGenericInstanceHead(const std::vector<std::shared_ptr<MidoriType>>& type_args)
{
	std::unordered_set<const MidoriType*> visited;
	return std::ranges::any_of(type_args, [&visited](const std::shared_ptr<MidoriType>& type_arg) { return ContainsFreeTypeParameter(type_arg, visited); });
}

bool GenericTypes::IsConcrete(const std::shared_ptr<MidoriType>& type)
{
	std::unordered_set<const MidoriType*> visited;
	return !ContainsFreeTypeParameter(type, visited);
}
