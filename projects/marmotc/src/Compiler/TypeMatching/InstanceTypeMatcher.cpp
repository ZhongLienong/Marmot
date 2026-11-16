#include "InstanceTypeMatcher.h"

#include <algorithm>

size_t InstanceTypeMatcher::TypePairHash::operator()(const std::pair<TypeRef, TypeRef>& pair) const noexcept
{
	return std::hash<MarmotType*>{}(pair.first.get()) ^ (std::hash<MarmotType*>{}(pair.second.get()) << 1u);
}

bool InstanceTypeMatcher::MatchAll(std::span<const TypeRef> patterns, std::span<const TypeRef> concretes)
{
	if (patterns.size() != concretes.size())
	{
		return false;
	}
	for (size_t index = 0u; index < patterns.size(); index += 1u)
	{
		if (!Match(patterns[index], concretes[index]))
		{
			return false;
		}
	}
	return true;
}

bool InstanceTypeMatcher::MatchArguments(const std::vector<std::string>& pattern_parameters, const std::vector<TypeRef>& pattern_arguments, const std::vector<std::string>& concrete_parameters, const std::vector<TypeRef>& concrete_arguments)
{
	// Prototypes keep parameter names; instantiations keep arguments. Both
	// spellings must bind phantom parameters even when no field mentions them.
	const auto identity = [](const TypeRef& type) { return type; };
	const std::vector<TypeRef> patterns = MarmotType::InstantiateTypeArguments(pattern_parameters, pattern_arguments, identity);
	const std::vector<TypeRef> concretes = MarmotType::InstantiateTypeArguments(concrete_parameters, concrete_arguments, identity);
	return MatchAll(patterns, concretes);
}

bool InstanceTypeMatcher::Match(const TypeRef& pattern, const TypeRef& concrete)
{
	// Synthesized prototype parameters are identified by their bindings, not addresses.
	if (pattern->IsType<MarmotType::GenericParam>())
	{
		const std::string& name = pattern->GetType<MarmotType::GenericParam>().m_name;
		const TypeEnvironment::const_iterator bound = m_bindings.find(name);
		if (bound != m_bindings.cend())
		{
			return *bound->second == *concrete;
		}
		m_bindings.emplace(name, concrete);
		return true;
	}
	if (!m_visited.emplace(pattern, concrete).second)
	{
		return true;
	}
	if (pattern->IsType<MarmotType::ArrayType>())
	{
		return concrete->IsType<MarmotType::ArrayType>() && Match(pattern->GetType<MarmotType::ArrayType>().m_element_type, concrete->GetType<MarmotType::ArrayType>().m_element_type);
	}
	if (pattern->IsType<MarmotType::CellType>())
	{
		return concrete->IsType<MarmotType::CellType>() && Match(pattern->GetType<MarmotType::CellType>().m_element_type, concrete->GetType<MarmotType::CellType>().m_element_type);
	}
	if (pattern->IsType<MarmotType::RangeType>())
	{
		return concrete->IsType<MarmotType::RangeType>() && Match(pattern->GetType<MarmotType::RangeType>().m_element_type, concrete->GetType<MarmotType::RangeType>().m_element_type);
	}
	if (pattern->IsType<MarmotType::TupleType>())
	{
		return concrete->IsType<MarmotType::TupleType>() && MatchAll(pattern->GetType<MarmotType::TupleType>().m_element_types, concrete->GetType<MarmotType::TupleType>().m_element_types);
	}
	if (pattern->IsType<MarmotType::FunctionType>())
	{
		if (!concrete->IsType<MarmotType::FunctionType>())
		{
			return false;
		}
		const MarmotType::FunctionType& pattern_function = pattern->GetType<MarmotType::FunctionType>();
		const MarmotType::FunctionType& concrete_function = concrete->GetType<MarmotType::FunctionType>();
		return MatchAll(pattern_function.m_param_types, concrete_function.m_param_types) && Match(pattern_function.m_return_type, concrete_function.m_return_type);
	}
	if (pattern->IsType<MarmotType::StructType>())
	{
		if (!concrete->IsType<MarmotType::StructType>())
		{
			return false;
		}
		const MarmotType::StructType& pattern_struct = pattern->GetType<MarmotType::StructType>();
		const MarmotType::StructType& concrete_struct = concrete->GetType<MarmotType::StructType>();
		return pattern_struct.m_name == concrete_struct.m_name
			&& pattern_struct.m_module_name == concrete_struct.m_module_name
			&& MatchArguments(pattern_struct.m_generic_params, pattern_struct.m_type_arguments, concrete_struct.m_generic_params, concrete_struct.m_type_arguments)
			&& MatchAll(pattern_struct.m_member_types, concrete_struct.m_member_types);
	}
	if (pattern->IsType<MarmotType::UnionType>())
	{
		if (!concrete->IsType<MarmotType::UnionType>())
		{
			return false;
		}
		const MarmotType::UnionType& pattern_union = pattern->GetType<MarmotType::UnionType>();
		const MarmotType::UnionType& concrete_union = concrete->GetType<MarmotType::UnionType>();
		if (pattern_union.m_name != concrete_union.m_name || pattern_union.m_module_name != concrete_union.m_module_name || pattern_union.m_member_info.size() != concrete_union.m_member_info.size()
			|| !MatchArguments(pattern_union.m_generic_params, pattern_union.m_type_arguments, concrete_union.m_generic_params, concrete_union.m_type_arguments))
		{
			return false;
		}
		return std::ranges::all_of(pattern_union.m_member_info, [&](const std::pair<const std::string, MarmotType::UnionType::UnionMemberContext>& member)
		{
			const std::unordered_map<std::string, MarmotType::UnionType::UnionMemberContext>::const_iterator concrete_member = concrete_union.m_member_info.find(member.first);
			return concrete_member != concrete_union.m_member_info.cend() && MatchAll(member.second.m_member_types, concrete_member->second.m_member_types);
		});
	}
	if (pattern->IsType<MarmotType::NewType>())
	{
		if (!concrete->IsType<MarmotType::NewType>())
		{
			return false;
		}
		const MarmotType::NewType& pattern_newtype = pattern->GetType<MarmotType::NewType>();
		const MarmotType::NewType& concrete_newtype = concrete->GetType<MarmotType::NewType>();
		return pattern_newtype.m_name == concrete_newtype.m_name
			&& pattern_newtype.m_module_name == concrete_newtype.m_module_name
			&& MatchArguments(pattern_newtype.m_generic_params, pattern_newtype.m_type_arguments, concrete_newtype.m_generic_params, concrete_newtype.m_type_arguments)
			&& Match(pattern_newtype.m_representation, concrete_newtype.m_representation);
	}
	if (pattern->IsType<MarmotType::AssociatedType>())
	{
		if (!concrete->IsType<MarmotType::AssociatedType>())
		{
			return false;
		}
		const MarmotType::AssociatedType& pattern_associated = pattern->GetType<MarmotType::AssociatedType>();
		const MarmotType::AssociatedType& concrete_associated = concrete->GetType<MarmotType::AssociatedType>();
		return pattern_associated.m_class_name == concrete_associated.m_class_name
			&& pattern_associated.m_name == concrete_associated.m_name
			&& MatchAll(pattern_associated.m_type_args, concrete_associated.m_type_args);
	}
	return *pattern == *concrete;
}

const InstanceTypeMatcher::TypeEnvironment& InstanceTypeMatcher::Bindings() const
{
	return m_bindings;
}

InstanceTypeMatcher::TypeEnvironment InstanceTypeMatcher::TakeBindings() &&
{
	return std::move(m_bindings);
}
