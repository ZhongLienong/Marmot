#include "TypeclassMetadata.h"
#include "Bytecode/Format/Format.h"
#include "Support/Optional.h"

#include <algorithm>
#include <format>
#include <utility>

TypeclassInstance::TypeclassInstance(TypeArguments type_args, AssociatedTypes associated_type_bindings, std::vector<MarmotType::ClassConstraint> constraints, std::vector<std::string> methods, std::string declaring_module)
	: m_type_args(std::move(type_args)),
	m_associated_type_bindings(std::move(associated_type_bindings)),
	m_constraints(std::move(constraints)),
	m_methods(std::move(methods)),
	m_declaring_module(std::move(declaring_module))
{
}

bool TypeclassInstance::HasTypeArguments(const TypeArguments& type_args) const
{
	return std::ranges::equal(m_type_args, type_args, [](const std::shared_ptr<MarmotType>& left, const std::shared_ptr<MarmotType>& right) { return *left == *right; });
}

bool TypeclassMetadata::DefinitionMatches(const TypeclassMetadata& other) const
{
	if (m_method_names != other.m_method_names || m_type_param_names != other.m_type_param_names || m_associated_type_names != other.m_associated_type_names || m_method_types.size() != other.m_method_types.size())
	{
		return false;
	}
	for (const auto& [name, type] : m_method_types)
	{
		const std::unordered_map<std::string, std::shared_ptr<MarmotType>>::const_iterator found = other.m_method_types.find(name);
		if (found == other.m_method_types.cend() || *type != *found->second)
		{
			return false;
		}
	}
	return true;
}

void TypeclassMetadata::AddInstance(TypeclassInstance instance)
{
	const std::vector<TypeclassInstance>::iterator existing = std::ranges::find_if(m_instances, [&instance](const TypeclassInstance& candidate) { return candidate.HasTypeArguments(instance.m_type_args); });
	if (existing == m_instances.end())
	{
		m_instances.push_back(std::move(instance));
		return;
	}
	// A shared dependency contributes its instance through every import path.
	// Keep the first payload while retaining all visible emitted symbols.
	for (std::string& method : instance.m_methods)
	{
		if (!std::ranges::contains(existing->m_methods, method))
		{
			existing->m_methods.push_back(std::move(method));
		}
	}
}

std::vector<std::string> TypeclassMetadata::InstanceMethods() const
{
	std::vector<std::string> names;
	for (const TypeclassInstance& instance : m_instances)
	{
		for (const std::string& method : instance.m_methods)
		{
			names.push_back(instance.m_declaring_module.empty() ? method : method + MmcWire::MODULE_SEPARATOR + instance.m_declaring_module);
		}
	}
	return names;
}

MarmotCompiler::Optional<std::string> TypeclassImports::Import(const std::string& module_name, const TypeclassMetadataMap& metadata)
{
	for (const auto& [class_name, incoming] : metadata)
	{
		const TypeclassMetadataMap::iterator existing = m_metadata.find(class_name);
		if (existing != m_metadata.end())
		{
			if (!existing->second.DefinitionMatches(incoming))
			{
				return std::format("Typeclass '{}' is defined in multiple imported modules ('{}' and '{}')", class_name, m_definition_sources.at(class_name), module_name);
			}
			for (const TypeclassInstance& instance : incoming.m_instances)
			{
				if (instance.m_declaring_module != module_name)
				{
					continue;
				}
				const std::string signature = MarmotType::JoinToStrings(instance.m_type_args);
				const std::unordered_map<std::string, std::string>::const_iterator previous = m_instance_sources[class_name].find(signature);
				if (previous != m_instance_sources[class_name].cend() && previous->second != module_name)
				{
					return std::format("Typeclass '{}' has two instances for '{}': one in module '{}', one in module '{}'. An instance belongs in the module that declares the class or the module that declares the type.", class_name, signature, previous->second, module_name);
				}
			}
			for (const TypeclassInstance& instance : incoming.m_instances)
			{
				existing->second.AddInstance(instance);
			}
		}
		else
		{
			m_metadata.emplace(class_name, incoming);
			m_definition_sources.emplace(class_name, module_name);
		}
		for (const TypeclassInstance& instance : incoming.m_instances)
		{
			if (instance.m_declaring_module == module_name)
			{
				m_instance_sources[class_name].emplace(MarmotType::JoinToStrings(instance.m_type_args), module_name);
			}
		}
	}
	return MarmotCompiler::None;
}

TypeclassMetadataMap TypeclassImports::TakeMetadata() &&
{
	return std::move(m_metadata);
}
