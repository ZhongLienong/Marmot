#include "Type.h"

#include <functional>
#include <numeric>
#include <unordered_set>
#include <utility>

#include "Common/Constant/Constant.h"

using namespace std::string_literals;

namespace
{
	using TypePtr = std::shared_ptr<MidoriType>;

	struct TypeConstPairHash
	{
		std::size_t operator()(const std::pair<const void*, const void*>& p) const noexcept
		{
			std::size_t h1 = std::hash<const void*>{}(p.first);
			std::size_t h2 = std::hash<const void*>{}(p.second);
			return h1 ^ (h2 << 32u | h2 >> 32u);
		}
	};

	using SubstitutionMap = std::unordered_map<std::string, TypePtr>;
	using TypeCache = std::unordered_map<const MidoriType*, TypePtr>;
	using JoinWithCommaFn = std::function<std::string(const std::string&, const std::string&)>;
	using ToStringFn = std::function<std::string(const MidoriType&)>;

	// Rebuilding a constraint through the class constructor would drop the operands
	// of an equality constraint and leave it looking like a class constraint with
	// no name, so both forms are rebuilt explicitly.
	template<typename SubstituteFn>
	MidoriType::ClassConstraint SubstituteConstraint(const MidoriType::ClassConstraint& constraint, SubstituteFn& substitute)
	{
		if (constraint.IsEquality())
		{
			return MidoriType::ClassConstraint(substitute(constraint.m_equality_lhs), substitute(constraint.m_equality_rhs));
		}

		std::vector<TypePtr> new_type_args;
		new_type_args.reserve(constraint.m_type_args.size());
		std::ranges::transform(constraint.m_type_args, std::back_inserter(new_type_args), substitute);
		return MidoriType::ClassConstraint(constraint.m_class_name, std::move(new_type_args));
	}

	template<typename SubstituteFn>
	struct SubstitutionVisitor
	{
		const SubstitutionMap& substitutions;
		SubstituteFn& substitute;
		TypeCache& cache;
		const TypePtr& current_type;

		template<typename T>
		TypePtr operator()(const T& type_variant) const
		{
			if constexpr (std::is_same_v<T, MidoriType::GenericParam>)
			{
				SubstitutionMap::const_iterator it = substitutions.find(type_variant.m_name);
				return (it != substitutions.end()) ? it->second : MidoriType::MakeGenericType(type_variant.m_name);
			}
			else if constexpr (std::is_same_v<T, MidoriType::ArrayType>)
			{
				return MidoriType::MakeArrayType(substitute(type_variant.m_element_type));
			}
			else if constexpr (std::is_same_v<T, MidoriType::WorkerType>)
			{
				return MidoriType::MakeWorkerType(substitute(type_variant.m_result_type));
			}
			else if constexpr (std::is_same_v<T, MidoriType::ChannelType>)
			{
				return MidoriType::MakeChannelType(substitute(type_variant.m_element_type));
			}
			else if constexpr (std::is_same_v<T, MidoriType::CellType>)
			{
				return MidoriType::MakeCellType(substitute(type_variant.m_element_type));
			}
			else if constexpr (std::is_same_v<T, MidoriType::RangeType>)
			{
				return MidoriType::MakeRangeType(substitute(type_variant.m_element_type));
			}
			else if constexpr (std::is_same_v<T, MidoriType::TupleType>)
			{
				std::vector<TypePtr> new_element_types;
				std::ranges::transform(type_variant.m_element_types, std::back_inserter(new_element_types), substitute);
				return MidoriType::MakeTupleType(std::move(new_element_types));
			}
			else if constexpr (std::is_same_v<T, MidoriType::FunctionType>)
			{
				std::vector<TypePtr> new_param_types;
				std::ranges::transform(type_variant.m_param_types, std::back_inserter(new_param_types), substitute);
				TypePtr new_function = MidoriType::MakeFunctionType(new_param_types, substitute(type_variant.m_return_type), type_variant.m_is_foreign);
				std::vector<MidoriType::ClassConstraint> new_constraints;
				new_constraints.reserve(type_variant.m_constraints.size());
				for (const MidoriType::ClassConstraint& constraint : type_variant.m_constraints)
				{
					new_constraints.emplace_back(SubstituteConstraint(constraint, substitute));
				}
				new_function->GetType<MidoriType::FunctionType>().m_constraints = std::move(new_constraints);
				return new_function;
			}
			else if constexpr (std::is_same_v<T, MidoriType::StructType>)
			{
				std::vector<TypePtr> empty_member_types;
				std::vector<std::string> member_names_copy = type_variant.m_member_names;
				std::vector<std::string> instantiated_generic_params;
				TypePtr new_struct = MidoriType::MakeStructType(type_variant.m_name, std::move(empty_member_types), std::move(member_names_copy), std::move(instantiated_generic_params));
				cache[current_type.get()] = new_struct;

				std::vector<TypePtr> new_member_types;
				std::ranges::transform(type_variant.m_member_types, std::back_inserter(new_member_types), substitute);
				new_struct->GetType<MidoriType::StructType>().m_member_types = std::move(new_member_types);
				std::vector<MidoriType::ClassConstraint> new_constraints;
				new_constraints.reserve(type_variant.m_constraints.size());
				for (const MidoriType::ClassConstraint& constraint : type_variant.m_constraints)
				{
					new_constraints.emplace_back(SubstituteConstraint(constraint, substitute));
				}
				new_struct->GetType<MidoriType::StructType>().m_constraints = std::move(new_constraints);
				if (!type_variant.m_generic_params.empty() || type_variant.m_is_generic_instantiation)
				{
					new_struct->GetType<MidoriType::StructType>().m_is_generic_instantiation = true;
					new_struct->GetType<MidoriType::StructType>().m_type_arguments = MidoriType::InstantiateTypeArguments(type_variant.m_generic_params, type_variant.m_type_arguments, substitute);
				}
				return new_struct;
			}
			else if constexpr (std::is_same_v<T, MidoriType::UnionType>)
			{
				std::vector<std::string> instantiated_generic_params;
				TypePtr new_union_type = MidoriType::MakeUnionType(type_variant.m_name, std::move(instantiated_generic_params));
				MidoriType::UnionType& new_union_ref = new_union_type->GetType<MidoriType::UnionType>();
				std::vector<MidoriType::ClassConstraint> new_constraints;
				new_constraints.reserve(type_variant.m_constraints.size());
				for (const MidoriType::ClassConstraint& constraint : type_variant.m_constraints)
				{
					new_constraints.emplace_back(SubstituteConstraint(constraint, substitute));
				}
				new_union_ref.m_constraints = std::move(new_constraints);
				if (!type_variant.m_generic_params.empty() || type_variant.m_is_generic_instantiation)
				{
					new_union_ref.m_is_generic_instantiation = true;
					new_union_ref.m_type_arguments = MidoriType::InstantiateTypeArguments(type_variant.m_generic_params, type_variant.m_type_arguments, substitute);
				}
				cache[current_type.get()] = new_union_type;

				std::ranges::for_each
				(
					type_variant.m_member_info,
					[this, &new_union_ref](const std::pair<const std::string, MidoriType::UnionType::UnionMemberContext>& member_pair)
					{
						const std::string& member_name = member_pair.first;
						const MidoriType::UnionType::UnionMemberContext& member_ctx = member_pair.second;

						std::vector<TypePtr> new_member_types;
						std::ranges::transform(member_ctx.m_member_types, std::back_inserter(new_member_types), this->substitute);

						new_union_ref.m_member_info.emplace(member_name, MidoriType::UnionType::UnionMemberContext{.m_member_types = std::move(new_member_types), .m_tag = member_ctx.m_tag});
					}
				);

				return new_union_type;
			}
			else if constexpr (std::is_same_v<T, MidoriType::NewType>)
			{
				std::vector<std::string> preserved_generic_params = type_variant.m_generic_params;
				TypePtr new_newtype = MidoriType::MakeNewType(type_variant.m_name, type_variant.m_representation, std::move(preserved_generic_params));
				cache[current_type.get()] = new_newtype;

				MidoriType::NewType& new_ref = new_newtype->GetType<MidoriType::NewType>();
				new_ref.m_representation = substitute(type_variant.m_representation);

				std::vector<MidoriType::ClassConstraint> new_constraints;
				new_constraints.reserve(type_variant.m_constraints.size());
				for (const MidoriType::ClassConstraint& constraint : type_variant.m_constraints)
				{
					new_constraints.emplace_back(SubstituteConstraint(constraint, substitute));
				}
				new_ref.m_constraints = std::move(new_constraints);

				if (!type_variant.m_generic_params.empty() || type_variant.m_is_generic_instantiation)
				{
					new_ref.m_is_generic_instantiation = true;
					new_ref.m_type_arguments = MidoriType::InstantiateTypeArguments(type_variant.m_generic_params, type_variant.m_type_arguments, substitute);
				}

				return new_newtype;
			}
			else if constexpr (std::is_same_v<T, MidoriType::AssociatedType>)
			{
				std::vector<TypePtr> new_type_args;
				std::ranges::transform(type_variant.m_type_args, std::back_inserter(new_type_args), substitute);
				return MidoriType::MakeAssociatedType(type_variant.m_class_name, type_variant.m_name, std::move(new_type_args));
			}
			else
			{
				return std::make_shared<MidoriType>(MidoriType::MidoriTypeUnion(type_variant));
			}
		}
	};

	template<typename ToStringCallback>
	struct ToStringVisitor
	{
		const JoinWithCommaFn& join_with_comma;
		const ToStringCallback& stringify;

		std::string StringifyTypeArguments(const std::string& name, const std::vector<TypePtr>& type_arguments) const
		{
			std::vector<std::string> type_argument_strings;
			std::ranges::transform
			(
				type_arguments,
				std::back_inserter(type_argument_strings),
				[this](const TypePtr& type_argument) { return stringify(*type_argument); }
			);

			return name + "<"s + std::accumulate(std::next(type_argument_strings.begin()), type_argument_strings.end(), type_argument_strings.front(), join_with_comma) + ">"s;
		}

		template<typename Type>
		std::string operator()(const Type& type_variant) const
		{
			if constexpr (std::is_same_v<Type, MidoriType::UndecidedType>)
			{
				return "Undecided"s;
			}
			else if constexpr (std::is_same_v<Type, MidoriType::GenericParam>)
			{
				return type_variant.m_name;
			}
			else if constexpr (std::is_same_v<Type, MidoriType::TypeVariable>)
			{
				return "T"s + std::to_string(type_variant.m_id);
			}
			else if constexpr (std::is_same_v<Type, MidoriType::FloatType>)
			{
				return "Float"s;
			}
			else if constexpr (std::is_same_v<Type, MidoriType::IntegerType>)
			{
				return "Int"s;
			}
			else if constexpr (std::is_same_v<Type, MidoriType::ByteType>)
			{
				return "Byte"s;
			}
			else if constexpr (std::is_same_v<Type, MidoriType::WordType>)
			{
				return "Word"s;
			}
			else if constexpr (std::is_same_v<Type, MidoriType::TextType>)
			{
				return "Text"s;
			}
			else if constexpr (std::is_same_v<Type, MidoriType::BoolType>)
			{
				return "Bool"s;
			}
			else if constexpr (std::is_same_v<Type, MidoriType::UnitType>)
			{
				return "Unit"s;
			}
			else if constexpr (std::is_same_v<Type, MidoriType::NeverType>)
			{
				return "Never"s;
			}
			else if constexpr (std::is_same_v<Type, MidoriType::ArrayType>)
			{
				return "Array<"s + stringify(*type_variant.m_element_type) + ">"s;
			}
			else if constexpr (std::is_same_v<Type, MidoriType::WorkerType>)
			{
				return "Worker<"s + stringify(*type_variant.m_result_type) + ">"s;
			}
			else if constexpr (std::is_same_v<Type, MidoriType::ChannelType>)
			{
				return "Channel<"s + stringify(*type_variant.m_element_type) + ">"s;
			}
			else if constexpr (std::is_same_v<Type, MidoriType::CellType>)
			{
				return "Cell<"s + stringify(*type_variant.m_element_type) + ">"s;
			}
			else if constexpr (std::is_same_v<Type, MidoriType::RangeType>)
			{
				return "Range<"s + stringify(*type_variant.m_element_type) + ">"s;
			}
			else if constexpr (std::is_same_v<Type, MidoriType::TupleType>)
			{
				if (type_variant.m_element_types.empty())
				{
					return "Unit"s;
				}

				std::vector<std::string> type_strings;
				std::ranges::transform
				(
					type_variant.m_element_types,
					std::back_inserter(type_strings),
					[this](const TypePtr& elem_type) { return stringify(*elem_type); }
				);

				return "("s + std::accumulate(std::next(type_strings.begin()), type_strings.end(), type_strings.front(), join_with_comma) + ")"s;
			}
			else if constexpr (std::is_same_v<Type, MidoriType::FunctionType>)
			{
				if (type_variant.m_param_types.empty())
				{
					return "fn() -> "s + stringify(*type_variant.m_return_type);
				}

				std::vector<std::string> param_strings;
				std::ranges::transform
				(
					type_variant.m_param_types,
					std::back_inserter(param_strings),
					[this](const TypePtr& param_type) { return stringify(*param_type); }
				);

				return "fn("s + std::accumulate(std::next(param_strings.begin()), param_strings.end(), param_strings.front(), join_with_comma) + ") -> "s + stringify(*type_variant.m_return_type);
			}
			else if constexpr (std::is_same_v<Type, MidoriType::StructType>)
			{
				if (!type_variant.m_generic_params.empty())
				{
					return type_variant.m_name + "<"s + std::accumulate(std::next(type_variant.m_generic_params.begin()), type_variant.m_generic_params.end(), type_variant.m_generic_params.front(), join_with_comma) + ">"s;
				}
				else if (type_variant.m_is_generic_instantiation && !type_variant.m_type_arguments.empty())
				{
					return StringifyTypeArguments(type_variant.m_name, type_variant.m_type_arguments);
				}
				else if (type_variant.m_is_generic_instantiation && !type_variant.m_member_types.empty())
				{
					std::vector<std::string> member_type_strings;
					std::ranges::transform
					(
						type_variant.m_member_types,
						std::back_inserter(member_type_strings),
						[this](const TypePtr& member_type) { return stringify(*member_type); }
					);

					return type_variant.m_name + "<"s + std::accumulate(std::next(member_type_strings.begin()), member_type_strings.end(), member_type_strings.front(), join_with_comma) + ">"s;
				}
				else
				{
					return type_variant.m_name;
				}
			}
			else if constexpr (std::is_same_v<Type, MidoriType::UnionType>)
			{
				if (!type_variant.m_generic_params.empty())
				{
					return type_variant.m_name + "<"s + std::accumulate(std::next(type_variant.m_generic_params.begin()), type_variant.m_generic_params.end(), type_variant.m_generic_params.front(), join_with_comma) + ">"s;
				}
				else if (type_variant.m_is_generic_instantiation && !type_variant.m_type_arguments.empty())
				{
					return StringifyTypeArguments(type_variant.m_name, type_variant.m_type_arguments);
				}
				else if (type_variant.m_is_generic_instantiation)
				{
					std::vector<std::string> member_keys;
					member_keys.reserve(type_variant.m_member_info.size());
					for (const auto& [member_name, member_ctx] : type_variant.m_member_info)
					{
						member_keys.emplace_back(member_name);
					}
					std::sort(member_keys.begin(), member_keys.end());
					
					std::string sig = type_variant.m_name + "<"s;
					bool first = true;
					for (const std::string& key : member_keys)
					{
						if (!first)
						{
							sig += ", ";
						}
						first = false;
						
						const MidoriType::UnionType::UnionMemberContext& ctx = type_variant.m_member_info.at(key);
						
						sig += key;
						if (!ctx.m_member_types.empty())
						{
							sig += "(";
							std::vector<std::string> type_strs;
							std::ranges::transform
							(
								ctx.m_member_types,
								std::back_inserter(type_strs),
								[this](const TypePtr& member_type) { return stringify(*member_type); }
							);
							sig += std::accumulate(std::next(type_strs.begin()), type_strs.end(), type_strs.front(), join_with_comma);
							sig += ")";
						}
					}
					sig += ">"s;
					return sig;
				}
				else
				{
					return type_variant.m_name;
				}
			}
			else if constexpr (std::is_same_v<Type, MidoriType::NewType>)
			{
				// Never renders its representation. InstanceKey and
				// MangleInstanceMethodName key on this string, so rendering the
				// representation here would collapse Hashable<Meters> into
				// Hashable<Int>.
				if (type_variant.m_is_generic_instantiation && !type_variant.m_type_arguments.empty())
				{
					return StringifyTypeArguments(type_variant.m_name, type_variant.m_type_arguments);
				}

				if (!type_variant.m_generic_params.empty())
				{
					return type_variant.m_name + "<"s + std::accumulate(std::next(type_variant.m_generic_params.begin()), type_variant.m_generic_params.end(), type_variant.m_generic_params.front(), join_with_comma) + ">"s;
				}

				return type_variant.m_name;
			}
			else if constexpr (std::is_same_v<Type, MidoriType::AssociatedType>)
			{
				if (type_variant.m_type_args.empty())
				{
					return type_variant.m_class_name + "::"s + type_variant.m_name;
				}

				std::vector<std::string> type_arg_strings;
				std::ranges::transform
				(
					type_variant.m_type_args,
					std::back_inserter(type_arg_strings),
					[this](const TypePtr& type_arg) { return stringify(*type_arg); }
				);

				return type_variant.m_class_name + "::"s + type_variant.m_name + "<"s +
					std::accumulate(std::next(type_arg_strings.begin()), type_arg_strings.end(), type_arg_strings.front(), join_with_comma) +
					">"s;
			}
			else
			{
				return ""s;
			}
		}
	};

}

struct MidoriType::TypeEqualityVisitor
{
	template<typename TypeA, typename TypeB>
	bool operator()(const TypeA& a, const TypeB& b) const
	{
		if constexpr (!std::is_same_v<TypeA, TypeB>)
		{
			return false;
		}
		else if constexpr (std::is_same_v<TypeA, MidoriType::GenericParam>)
		{
			return a.m_name == b.m_name;
		}
		else if constexpr (std::is_same_v<TypeA, MidoriType::TypeVariable>)
		{
			return a.m_id == b.m_id;
		}
		else if constexpr (std::is_same_v<TypeA, MidoriType::ArrayType>)
		{
			return *a.m_element_type == *b.m_element_type;
		}
		else if constexpr (std::is_same_v<TypeA, MidoriType::WorkerType>)
		{
			return *a.m_result_type == *b.m_result_type;
		}
		else if constexpr (std::is_same_v<TypeA, MidoriType::ChannelType>)
		{
			return *a.m_element_type == *b.m_element_type;
		}
		else if constexpr (std::is_same_v<TypeA, MidoriType::CellType>)
		{
			return *a.m_element_type == *b.m_element_type;
		}
		else if constexpr (std::is_same_v<TypeA, MidoriType::RangeType>)
		{
			return *a.m_element_type == *b.m_element_type;
		}
		else if constexpr (std::is_same_v<TypeA, MidoriType::TupleType>)
		{
			return a.m_element_types.size() == b.m_element_types.size()
				&& std::ranges::equal
				(
					a.m_element_types,
					b.m_element_types,
					[](const std::shared_ptr<MidoriType>& t1, const std::shared_ptr<MidoriType>& t2)
					{
						return *t1 == *t2;
					}
				);
		}
		else if constexpr (std::is_same_v<TypeA, MidoriType::FunctionType>)
		{
			return std::ranges::equal
			(
				a.m_param_types,
				b.m_param_types,
				[](const std::shared_ptr<MidoriType>& t1, const std::shared_ptr<MidoriType>& t2)
				{
					return *t1 == *t2;
				}
			) && *a.m_return_type == *b.m_return_type;
		}
		else if constexpr (std::is_same_v<TypeA, MidoriType::StructType>)
		{
			thread_local std::unordered_set<std::pair<const void*, const void*>, TypeConstPairHash> s_visiting;
			std::pair<const void*, const void*> key{&a, &b};
			if (s_visiting.contains(key))
			{
				return true;
			}
			s_visiting.insert(key);
			bool result = MidoriType::CompareStructTypes(a, b);
			s_visiting.erase(key);
			return result;
		}
		else if constexpr (std::is_same_v<TypeA, MidoriType::UnionType>)
		{
			thread_local std::unordered_set<std::pair<const void*, const void*>, TypeConstPairHash> s_visiting;
			std::pair<const void*, const void*> key{&a, &b};
			if (s_visiting.contains(key))
			{
				return true;
			}
			s_visiting.insert(key);
			bool result = MidoriType::CompareUnionTypes(a, b);
			s_visiting.erase(key);
			return result;
		}
		else if constexpr (std::is_same_v<TypeA, MidoriType::NewType>)
		{
			// Name and type arguments only. Comparing representations would make
			// Meters equal to Int, which is the whole thing this prevents.
			thread_local std::unordered_set<std::pair<const void*, const void*>, TypeConstPairHash> s_visiting;
			std::pair<const void*, const void*> key{&a, &b};
			if (s_visiting.contains(key))
			{
				return true;
			}
			s_visiting.insert(key);
			bool result = a.m_name == b.m_name && MidoriType::CompareTypeArguments(a.m_type_arguments, b.m_type_arguments);
			s_visiting.erase(key);
			return result;
		}
		else if constexpr (std::is_same_v<TypeA, MidoriType::AssociatedType>)
		{
			return a == b;
		}
		else if constexpr (std::is_same_v<TypeA, MidoriType::ClassConstraint>)
		{
			return a == b;
		}
		else
		{
			return true;
		}
	}
};

MidoriType::MidoriType(MidoriType::MidoriTypeUnion&& actual_type) : m_type(std::move(actual_type))
{
}

MidoriType::GenericParam::GenericParam(const std::string& name) : m_name(name)
{
}

bool MidoriType::GenericParam::operator==(const GenericParam& other) const
{
	return m_name == other.m_name;
}

MidoriType::TypeVariable::TypeVariable(int id) : m_id(id)
{
}

bool MidoriType::TypeVariable::operator==(const TypeVariable& other) const
{
	return m_id == other.m_id;
}

MidoriType::UnionType::UnionType(const std::string& name) : m_name(name)
{
}

MidoriType::ClassConstraint::ClassConstraint(const std::string& typeclass_name, std::vector<std::shared_ptr<MidoriType>>&& type_args)
	: m_class_name(typeclass_name), 
	m_type_args(std::move(type_args))
{
}

MidoriType::ClassConstraint::ClassConstraint(std::shared_ptr<MidoriType>&& equality_lhs, std::shared_ptr<MidoriType>&& equality_rhs)
	: m_kind(Kind::Equality),
	m_equality_lhs(std::move(equality_lhs)),
	m_equality_rhs(std::move(equality_rhs))
{
}

bool MidoriType::ClassConstraint::IsEquality() const
{
	return m_kind == Kind::Equality;
}

bool MidoriType::ClassConstraint::operator==(const ClassConstraint& other) const
{
	if (m_kind != other.m_kind)
	{
		return false;
	}

	if (m_kind == Kind::Equality)
	{
		return *m_equality_lhs == *other.m_equality_lhs
			&& *m_equality_rhs == *other.m_equality_rhs;
	}

	if (m_class_name != other.m_class_name || m_type_args.size() != other.m_type_args.size())
	{
		return false;
	}

	return std::ranges::equal
	(
		m_type_args,
		other.m_type_args,
		[](const std::shared_ptr<MidoriType>& left, const std::shared_ptr<MidoriType>& right)
		{
			return *left == *right;
		}
	);
}

MidoriType::AssociatedType::AssociatedType(const std::string& class_name, const std::string& name, std::vector<std::shared_ptr<MidoriType>>&& type_args)
	: m_class_name(class_name),
	m_name(name),
	m_type_args(std::move(type_args))
{
}

bool MidoriType::AssociatedType::operator==(const AssociatedType& other) const
{
	if (m_class_name != other.m_class_name || m_name != other.m_name || m_type_args.size() != other.m_type_args.size())
	{
		return false;
	}

	return std::ranges::equal
	(
		m_type_args,
		other.m_type_args,
		[](const std::shared_ptr<MidoriType>& left, const std::shared_ptr<MidoriType>& right)
		{
			return *left == *right;
		}
	);
}

const std::shared_ptr<MidoriType> MidoriType::MakeUndecidedType()
{
	return std::make_shared<MidoriType>(MidoriTypeUnion(UndecidedType{}));
}

std::shared_ptr<MidoriType> MidoriType::MakeGenericType(const std::string& name)
{
	return std::make_shared<MidoriType>(MidoriTypeUnion(GenericParam(name)));
}

std::shared_ptr<MidoriType> MidoriType::MakeTypeVariable(int id)
{
	return std::make_shared<MidoriType>(MidoriTypeUnion(TypeVariable(id)));
}

std::shared_ptr<MidoriType> MidoriType::MakeAssociatedType(const std::string& class_name, const std::string& name, std::vector<std::shared_ptr<MidoriType>>&& type_args)
{
	return std::make_shared<MidoriType>(MidoriTypeUnion(AssociatedType(class_name, name, std::move(type_args))));
}

std::shared_ptr<MidoriType> MidoriType::MakeArrayType(const std::shared_ptr<MidoriType>& element_type)
{
	return std::make_shared<MidoriType>(MidoriTypeUnion(ArrayType{.m_element_type = element_type}));
}

std::shared_ptr<MidoriType> MidoriType::MakeWorkerType(const std::shared_ptr<MidoriType>& result_type)
{
	return std::make_shared<MidoriType>(MidoriTypeUnion(WorkerType{.m_result_type = result_type}));
}

std::shared_ptr<MidoriType> MidoriType::MakeChannelType(const std::shared_ptr<MidoriType>& element_type)
{
	return std::make_shared<MidoriType>(MidoriTypeUnion(ChannelType{.m_element_type = element_type}));
}

std::shared_ptr<MidoriType> MidoriType::MakeCellType(const std::shared_ptr<MidoriType>& element_type)
{
	return std::make_shared<MidoriType>(MidoriTypeUnion(CellType{.m_element_type = element_type}));
}

std::shared_ptr<MidoriType> MidoriType::MakeRangeType(const std::shared_ptr<MidoriType>& element_type)
{
	return std::make_shared<MidoriType>(MidoriTypeUnion(RangeType{.m_element_type = element_type}));
}

std::shared_ptr<MidoriType> MidoriType::MakeTupleType(std::vector<std::shared_ptr<MidoriType>>&& element_types)
{
	return std::make_shared<MidoriType>(MidoriTypeUnion(TupleType{.m_element_types = std::move(element_types)}));
}

std::shared_ptr<MidoriType> MidoriType::MakeFunctionType(const std::vector<std::shared_ptr<MidoriType>>& param_types, std::shared_ptr<MidoriType>&& return_type, bool is_foreign)
{
	return std::make_shared<MidoriType>(MidoriTypeUnion(FunctionType{.m_param_types = param_types, .m_return_type = return_type, .m_constraints = {}, .m_is_foreign = is_foreign}));
}

std::shared_ptr<MidoriType> MidoriType::MakeStructType(const std::string& name, std::vector<std::shared_ptr<MidoriType>>&& member_types, std::vector<std::string>&& member_names, std::vector<std::string>&& generic_params)
{
	return std::make_shared<MidoriType>(MidoriTypeUnion(StructType{.m_member_types = std::move(member_types), .m_member_names = std::move(member_names), .m_name = name, .m_generic_params = std::move(generic_params), .m_constraints = {}}));
}

std::shared_ptr<MidoriType> MidoriType::MakeUnionType(const std::string& name, std::vector<std::string>&& generic_params)
{
	UnionType union_type(name);
	union_type.m_generic_params = std::move(generic_params);
	union_type.m_constraints = {};
	return std::make_shared<MidoriType>(MidoriTypeUnion(std::move(union_type)));
}

std::shared_ptr<MidoriType> MidoriType::MakeNewType(const std::string& name, const std::shared_ptr<MidoriType>& representation, std::vector<std::string>&& generic_params)
{
	return std::make_shared<MidoriType>(MidoriTypeUnion(NewType{.m_name = name, .m_representation = representation, .m_generic_params = std::move(generic_params), .m_type_arguments = {}, .m_constraints = {}}));
}

std::shared_ptr<MidoriType> MidoriType::SubstituteTypeParams(const std::shared_ptr<MidoriType>& type, const std::unordered_map<std::string, std::shared_ptr<MidoriType>>& substitutions)
{
	using SubstituteFn = std::function<TypePtr(const TypePtr&)>;
	TypeCache cache;

	SubstituteFn substitute = [&substitutions, &substitute, &cache](const TypePtr& t) -> TypePtr
		{
			TypeCache::iterator cache_it = cache.find(t.get());
			if (cache_it != cache.end())
			{
				return cache_it->second;
			}

			const SubstitutionVisitor<SubstituteFn> visitor{substitutions, substitute, cache, t};
			return std::visit(visitor, t->m_type);
		};

	return substitute(type);
}

std::vector<std::shared_ptr<MidoriType>> MidoriType::InstantiateTypeArguments(const std::vector<std::string>& generic_params, const std::vector<std::shared_ptr<MidoriType>>& type_arguments, const TypeArgumentSubstituteFn& substitute)
{
	std::vector<std::shared_ptr<MidoriType>> instantiated;

	if (!type_arguments.empty())
	{
		instantiated.reserve(type_arguments.size());
		std::ranges::transform(type_arguments, std::back_inserter(instantiated), substitute);
		return instantiated;
	}

	instantiated.reserve(generic_params.size());
	std::ranges::transform
	(
		generic_params,
		std::back_inserter(instantiated),
		[&substitute](const std::string& param_name) { return substitute(MakeGenericType(param_name)); }
	);

	return instantiated;
}

std::string MidoriType::ToString() const
{
	JoinWithCommaFn join_with_comma = [](const std::string& acc, const std::string& elem)
		{
			return acc.empty() ? elem : acc + ", "s + elem;
		};

	std::unordered_set<const MidoriType*> visited;

	ToStringFn stringify;
	stringify = [&visited, &join_with_comma, &stringify](const MidoriType& type) -> std::string
	{
		if (visited.contains(&type))
		{
			if (type.IsType<StructType>())
			{
					return type.GetType<StructType>().m_name;
				}
				else if (type.IsType<UnionType>())
				{
					return type.GetType<UnionType>().m_name;
				}
				else if (type.IsType<NewType>())
				{
					return type.GetType<NewType>().m_name;
				}
				return "Recursive"s;
			}

			visited.insert(&type);

			const std::string result = std::visit(ToStringVisitor<ToStringFn>{join_with_comma, stringify}, type.m_type);

			visited.erase(&type);
			return result;
		};

	return stringify(*this);
}

std::string MidoriType::MangleInstanceMethodName(const std::string& method_name, const std::string& typeclass_name, const std::vector<std::shared_ptr<MidoriType>>& type_args)
{
	std::string mangled = INTERNAL_NAME_PREFIX + method_name + "_"s + typeclass_name;

	for (const std::shared_ptr<MidoriType>& type_arg : type_args)
	{
		std::string type_str = type_arg->ToString();
		std::ranges::replace_if(type_str, [](char c) { return !std::isalnum(c); }, '_');	// Replace special characters with underscores for valid identifier
		mangled += "_"s + type_str;
	}

	return mangled;
}

std::string MidoriType::DemangleInstanceMethodName(const std::string& mangled_name, const std::string& typeclass_name)
{
	// mangled format: $methodName_TypeclassName_Type1_Type2
	// Skip the INTERNAL_NAME_PREFIX if present
	size_t start_pos = 0u;
	if (!mangled_name.empty() && mangled_name[0u] == INTERNAL_NAME_PREFIX)
	{
		start_pos = 1u;
	}

	// Find the position of "_TypeclassName"
	std::string suffix = "_"s + typeclass_name;
	size_t pos = mangled_name.find(suffix, start_pos);

	if (pos != std::string::npos)
	{
		return mangled_name.substr(start_pos, pos - start_pos);
	}

	return mangled_name.substr(start_pos);
}

bool MidoriType::IsNumericType() const
{
	return IsType<FloatType>() || IsType<IntegerType>() || IsType<ByteType>() || IsType<WordType>();
}

bool MidoriType::CompareTypeArguments(const std::vector<std::shared_ptr<MidoriType>>& a, const std::vector<std::shared_ptr<MidoriType>>& b)
{
	return std::ranges::equal
	(
		a,
		b,
		[](const std::shared_ptr<MidoriType>& left, const std::shared_ptr<MidoriType>& right)
		{
			return *left == *right;
		}
	);
}

bool MidoriType::CompareGenericStructs(const StructType& a, const StructType& b)
{
	if (a.m_member_types.size() != b.m_member_types.size())
	{
		return false;
	}

	return std::ranges::equal
	(
		a.m_member_types,
		b.m_member_types,
		[](const std::shared_ptr<MidoriType>& left, const std::shared_ptr<MidoriType>& right)
		{
			return *left == *right;
		}
	);
}

bool MidoriType::CompareInstantiatedStructs(const StructType& a, const StructType& b)
{
	if (a.m_member_types.size() != b.m_member_types.size() || !std::ranges::equal(a.m_member_names, b.m_member_names))
	{
		return false;
	}

	if (!CompareTypeArguments(a.m_type_arguments, b.m_type_arguments))
	{
		return false;
	}

	return std::ranges::equal
	(
		a.m_member_types,
		b.m_member_types,
		[](const std::shared_ptr<MidoriType>& left, const std::shared_ptr<MidoriType>& right)
		{
			return *left == *right;
		}
	);
}

bool MidoriType::CompareStructTypes(const StructType& a, const StructType& b)
{
	if (a.m_name != b.m_name)
	{
		return false;
	}

	bool a_has_generic_params = !a.m_generic_params.empty();
	bool b_has_generic_params = !b.m_generic_params.empty();

	if (a_has_generic_params || b_has_generic_params)
	{
		return CompareGenericStructs(a, b);
	}

	return CompareInstantiatedStructs(a, b);
}

bool MidoriType::CompareGenericUnions(const UnionType& a, const UnionType& b)
{
	using VariantPairCheckFn = std::function<bool(const std::pair<const std::string, UnionType::UnionMemberContext>&)>;

	if (a.m_member_info.size() != b.m_member_info.size())
	{
		return false;
	}

	VariantPairCheckFn check_variant = [&b](const std::pair<const std::string, UnionType::UnionMemberContext>& a_variant)
		{
			std::unordered_map<std::string, UnionType::UnionMemberContext>::const_iterator b_it = b.m_member_info.find(a_variant.first);

			if (b_it == b.m_member_info.end())
			{
				return false;
			}

			const UnionType::UnionMemberContext& a_ctx = a_variant.second;
			const UnionType::UnionMemberContext& b_ctx = b_it->second;

			return a_ctx.m_member_types.size() == b_ctx.m_member_types.size() 
				&& std::ranges::equal
				(
					a_ctx.m_member_types,
					b_ctx.m_member_types,
					[](const std::shared_ptr<MidoriType>& t1, const std::shared_ptr<MidoriType>& t2)
					{
						return *t1 == *t2;
					}
				);
		};

	return std::ranges::all_of(a.m_member_info, check_variant);
}

bool MidoriType::CompareInstantiatedUnions(const UnionType& a, const UnionType& b)
{
	using VariantPairCheckFn = std::function<bool(const std::pair<const std::string, UnionType::UnionMemberContext>&)>;

	if (a.m_member_info.size() != b.m_member_info.size())
	{
		return false;
	}

	if (!CompareTypeArguments(a.m_type_arguments, b.m_type_arguments))
	{
		return false;
	}

	VariantPairCheckFn check_variant = [&b](const std::pair<const std::string, UnionType::UnionMemberContext>& a_variant)
		{
			std::unordered_map<std::string, UnionType::UnionMemberContext>::const_iterator b_it = b.m_member_info.find(a_variant.first);

			if (b_it == b.m_member_info.end())
			{
				return false;
			}

			const UnionType::UnionMemberContext& a_ctx = a_variant.second;
			const UnionType::UnionMemberContext& b_ctx = b_it->second;

			if (a_ctx.m_member_types.size() != b_ctx.m_member_types.size())
			{
				return false;
			}

			return std::ranges::equal
			(
				a_ctx.m_member_types,
				b_ctx.m_member_types,
				[](const std::shared_ptr<MidoriType>& left, const std::shared_ptr<MidoriType>& right)
				{
					return *left == *right;
				}
			);
		};

	return std::ranges::all_of(a.m_member_info, check_variant);
}

bool MidoriType::CompareUnionTypes(const UnionType& a, const UnionType& b)
{
	if (a.m_name != b.m_name)
	{
		return false;
	}

	bool a_has_generic_params = !a.m_generic_params.empty();
	bool b_has_generic_params = !b.m_generic_params.empty();

	if (a_has_generic_params || b_has_generic_params)
	{
		return CompareGenericUnions(a, b);
	}

	return CompareInstantiatedUnions(a, b);
}

bool operator==(const MidoriType& lhs, const MidoriType& rhs)
{
	return std::visit(MidoriType::TypeEqualityVisitor{}, lhs.m_type, rhs.m_type);
}
