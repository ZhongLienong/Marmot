#pragma once

#include "Compiler/AbstractSyntaxTree/Type.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct TypeclassInstance
{
	using TypeArguments = std::vector<std::shared_ptr<MarmotType>>;
	using AssociatedTypes = std::unordered_map<std::string, std::shared_ptr<MarmotType>>;

	TypeArguments m_type_args;
	AssociatedTypes m_associated_type_bindings;
	std::vector<MarmotType::ClassConstraint> m_constraints;
	// Bare emitted symbols; their origin is m_declaring_module.
	std::vector<std::string> m_methods;
	std::string m_declaring_module;

	TypeclassInstance(TypeArguments type_args, AssociatedTypes associated_type_bindings, std::vector<MarmotType::ClassConstraint> constraints, std::vector<std::string> methods, std::string declaring_module);

	bool HasTypeArguments(const TypeArguments& type_args) const;
};

struct TypeclassMetadata
{
	std::unordered_map<std::string, std::shared_ptr<MarmotType>> m_method_types;
	std::unordered_set<std::string> m_method_names;
	std::vector<std::string> m_type_param_names;
	std::vector<std::string> m_associated_type_names;
	std::vector<TypeclassInstance> m_instances;

	bool DefinitionMatches(const TypeclassMetadata& other) const;
	void AddInstance(TypeclassInstance instance);
	// Export names retain the existing symbol@Module spelling.
	std::vector<std::string> InstanceMethods() const;
};

using TypeclassMetadataMap = std::unordered_map<std::string, TypeclassMetadata>;

class TypeclassImports
{
private:
	TypeclassMetadataMap m_metadata;
	std::unordered_map<std::string, std::string> m_definition_sources;
	std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_instance_sources;

public:
	std::optional<std::string> Import(const std::string& module_name, const TypeclassMetadataMap& metadata);
	TypeclassMetadataMap TakeMetadata() &&;
};
