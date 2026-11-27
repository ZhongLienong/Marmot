#include "ModuleImports.h"
#include "Compiler/Names/SpecialNames.h"
#include "Support/Expected.h"
#include "Support/Optional.h"

#include <algorithm>
#include <utility>

namespace
{
	std::string DeclaringModuleName(const std::shared_ptr<MarmotType>& type)
	{
		if (type->IsType<MarmotType::StructType>())
		{
			return type->GetType<MarmotType::StructType>().m_module_name;
		}
		if (type->IsType<MarmotType::UnionType>())
		{
			return type->GetType<MarmotType::UnionType>().m_module_name;
		}
		if (type->IsType<MarmotType::NewType>())
		{
			return type->GetType<MarmotType::NewType>().m_module_name;
		}
		return {};
	}
}

ModuleImportConflict::ModuleImportConflict(std::filesystem::path dependency, std::string message)
	: m_dependency(std::move(dependency)),
	m_message(std::move(message))
{
}

MarmotCompiler::Expected<ModuleImports, ModuleImportConflict> ModuleImports::Collect(std::span<const std::shared_ptr<const ModuleExportMetadata>> dependencies)
{
	ModuleImports imports;
	TypeclassImports typeclass_imports;
	size_t type_count = 0u;
	size_t generic_count = 0u;
	for (const std::shared_ptr<const ModuleExportMetadata>& dependency : dependencies)
	{
		type_count += dependency->m_type_signatures.size() * 2u;
		generic_count += dependency->m_generic_functions.size() * 2u;
	}
	imports.m_symbols.reserve(dependencies.size());
	imports.m_types_by_module.reserve(dependencies.size());
	imports.m_types.reserve(type_count);
	imports.m_generics.reserve(generic_count);

	for (const std::shared_ptr<const ModuleExportMetadata>& dependency : dependencies)
	{
		const std::string& module_name = dependency->m_module_name;
		imports.m_symbols[module_name] = dependency->m_symbols;
		imports.m_types_by_module[module_name] = dependency->m_type_signatures;
		if (const MarmotCompiler::Optional<std::string> conflict = typeclass_imports.Import(module_name, dependency->m_typeclass_metadata))
		{
			return MarmotCompiler::Failure(ModuleImportConflict(dependency->m_file_path, *conflict));
		}

		// A facade does not change the declaring module of a nominal type or its constructors.
		std::unordered_map<std::string, std::string> declaring_modules;
		for (const auto& [name, type] : dependency->m_type_signatures)
		{
			const std::string declaring_module = DeclaringModuleName(type);
			if (!declaring_module.empty())
			{
				declaring_modules.emplace(name, declaring_module);
			}
		}
		for (const auto& [name, type] : dependency->m_type_signatures)
		{
			imports.m_types[name] = type;
			imports.m_types[module_name + NameSeparator.data() + name] = type;
			const std::unordered_map<std::string, std::string>::const_iterator declaring = declaring_modules.find(name.substr(0u, name.find(NameSeparator)));
			if (declaring != declaring_modules.cend() && declaring->second != module_name)
			{
				imports.m_types[declaring->second + NameSeparator.data() + name] = type;
			}
		}

		// Bare generic keys belong to the current module. Transitive keys are already qualified.
		for (const auto& [name, generic] : dependency->m_generic_functions)
		{
			if (name.find(NameSeparator) != std::string::npos)
			{
				imports.m_generics[name] = generic;
				continue;
			}
			imports.m_generics[module_name + NameSeparator.data() + name] = generic;
			if (!generic.m_defining_module.empty() && generic.m_defining_module != module_name)
			{
				imports.m_generics[generic.m_defining_module + NameSeparator.data() + name] = generic;
			}
		}
	}
	imports.m_typeclasses = std::move(typeclass_imports).TakeMetadata();
	return imports;
}

bool ModuleImports::ContainsModule(std::string_view module_name) const
{
	return m_symbols.contains(std::string(module_name));
}

ImportedSymbolAccess ModuleImports::AccessFrom(std::string_view importer, const std::string& module_name, const std::string& symbol_name) const
{
	const std::unordered_map<std::string, ModuleExportMetadata::SymbolTable>::const_iterator module = m_symbols.find(module_name);
	if (module == m_symbols.cend())
	{
		return ImportedSymbolAccess::ModuleNotFound;
	}
	const VisibilityLevel* visibility = module->second.FindExportVisibility(symbol_name);
	if (visibility == nullptr || *visibility == VisibilityLevel::Internal)
	{
		return ImportedSymbolAccess::SymbolNotExported;
	}
	if (*visibility == VisibilityLevel::Public || (!importer.empty() && SharesNamespace(importer, module_name)))
	{
		return ImportedSymbolAccess::Accessible;
	}
	return ImportedSymbolAccess::PrivateInaccessible;
}

std::vector<std::string> ModuleImports::ModulesExporting(std::string_view importer, const std::string& symbol_name) const
{
	std::vector<std::string> modules;
	for (const auto& [name, symbols] : m_symbols)
	{
		if (AccessFrom(importer, name, symbol_name) == ImportedSymbolAccess::Accessible)
		{
			modules.push_back(name);
		}
	}
	std::ranges::sort(modules);
	return modules;
}

std::vector<std::string> ModuleImports::ReexportOrigins(std::string_view importer, const std::string& symbol_name, std::span<const UseImport> use_imports) const
{
	std::vector<std::string> origins = ModulesExporting(importer, symbol_name);
	// `use` selects the origin when several imports export the same name.
	for (const UseImport& use_import : use_imports)
	{
		if (use_import.m_symbol_name == symbol_name && std::ranges::contains(origins, use_import.m_module_name))
		{
			return { use_import.m_module_name };
		}
	}
	return origins;
}

void ModuleImports::Reexport(const std::string& symbol_name, const std::string& origin, TypeEnvironment& types, GenericFunctions& generics) const
{
	const std::string member_prefix = symbol_name + NameSeparator.data();
	for (const auto& [name, type] : m_types_by_module.at(origin))
	{
		if (name == symbol_name || name.starts_with(member_prefix))
		{
			types[name] = type;
		}
	}
	const GenericFunctions::const_iterator generic = m_generics.find(origin + NameSeparator.data() + symbol_name);
	if (generic != m_generics.cend())
	{
		generics[symbol_name] = generic->second;
	}
}

const ModuleImports::ModuleTypes& ModuleImports::TypesByModule() const
{
	return m_types_by_module;
}

const ModuleImports::TypeEnvironment& ModuleImports::Types() const
{
	return m_types;
}

const TypeclassMetadataMap& ModuleImports::Typeclasses() const
{
	return m_typeclasses;
}

const GenericFunctions& ModuleImports::Generics() const
{
	return m_generics;
}
