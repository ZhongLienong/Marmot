#include "ModuleExports.h"
#include "Compiler/Lowering/Lowering.h"

#include <format>
#include <utility>

ModuleExports::ModuleExports(const ModuleDeclaration* declaration, std::filesystem::path file_path, TypeEnvironment types, TypeclassMetadataMap typeclasses)
	: m_module_name(declaration != nullptr ? declaration->ModuleName() : file_path.stem().string()),
	m_file_path(std::move(file_path)),
	m_declarations(declaration != nullptr ? declaration->Exports() : ModuleDeclaration::ExportList()),
	m_types(std::move(types)),
	m_typeclasses(std::move(typeclasses))
{
	for (const ModuleExport& exported : m_declarations)
	{
		m_symbols = std::move(m_symbols).WithExport(exported.m_symbol_name, exported.m_visibility);
		m_names.insert(exported.m_symbol_name);
		const TypeclassMetadataMap::const_iterator typeclass = m_typeclasses.find(exported.m_symbol_name);
		if (typeclass != m_typeclasses.cend())
		{
			for (const std::string& method : typeclass->second.InstanceMethods())
			{
				m_symbols = std::move(m_symbols).WithExport(method, exported.m_visibility);
				m_names.insert(method);
			}
		}
	}
}

const std::string& ModuleExports::ModuleName() const
{
	return m_module_name;
}

const ModuleInterface::ExportSet& ModuleExports::Names() const
{
	return m_names;
}

void ModuleExports::RecordInferredTypes(const TypeEnvironment& inferred)
{
	// Unannotated definitions only acquire their exported types after checking.
	for (const ModuleExport& exported : m_declarations)
	{
		if (m_types.contains(exported.m_symbol_name))
		{
			continue;
		}
		const TypeEnvironment::const_iterator type = inferred.find(exported.m_symbol_name);
		if (type != inferred.cend())
		{
			m_types[exported.m_symbol_name] = type->second;
		}
	}
}

std::expected<PublishedModuleInterface, CompilerError> ModuleExports::Publish(LoweredModule& lowered, const ModuleImports& imports, std::span<const UseImport> use_imports, const std::vector<std::string>& source_lines) &&
{
	std::unordered_set<std::string> defined;
	for (const LoweredExport& exported : lowered.m_exports)
	{
		defined.insert(exported.m_name);
	}
	for (const auto& [name, metadata] : m_typeclasses)
	{
		defined.insert(name);
		for (const std::string& method : metadata.InstanceMethods())
		{
			defined.insert(method);
		}
	}
	for (const auto& [name, type] : m_types)
	{
		defined.insert(name);
	}

	PublishedModuleInterface published;
	// Source order fixes which invalid export is diagnosed first.
	for (const ModuleExport& exported : m_declarations)
	{
		const std::string& name = exported.m_symbol_name;
		if (defined.contains(name))
		{
			continue;
		}
		const std::vector<std::string> origins = imports.ReexportOrigins(m_module_name, name, use_imports);
		if (origins.size() == 1u)
		{
			published.m_reexports[name] = origins.front();
			imports.Reexport(name, origins.front(), m_types, lowered.m_generic_functions);
			continue;
		}
		const std::string message = origins.empty()
			? std::format("Symbol '{}' is exported but not defined in module '{}'", name, m_module_name)
			: std::format("Symbol '{}' is re-exported by module '{}', but '{}' and '{}' both export it. Write 'use {}.{{{}}}' to say which one.", name, m_module_name, origins.front(), origins[1u], origins.front(), name);
		return std::unexpected(CompilerError::WithToken(CompilerStage::Module, message, exported.m_token, m_file_path.string(), source_lines, std::nullopt, CompilerErrorCode::ModuleMissingExportedSymbol));
	}
	published.m_interface = std::make_shared<const ModuleInterface>(m_module_name, m_file_path, std::move(m_symbols), std::move(m_types), std::move(m_typeclasses), std::move(lowered.m_generic_functions));
	return published;
}
