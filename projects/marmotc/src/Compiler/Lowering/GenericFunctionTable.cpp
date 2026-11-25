#include "GenericFunctionTable.h"
#include "Compiler/Names/SpecialNames.h"
#include "Support/Optional.h"

GenericFunctionTable::GenericFunctionTable(std::string module_name, GenericFunctions imported)
	: m_module_name(std::move(module_name)),
	m_functions(std::move(imported))
{
}

void GenericFunctionTable::Add(const std::string& name, GenericFunctionInfo info)
{
	m_functions.emplace(name, std::move(info));
}

bool GenericFunctionTable::Contains(const std::string& key) const
{
	return m_functions.contains(key);
}

const GenericFunctionInfo& GenericFunctionTable::At(const std::string& key) const
{
	return m_functions.at(key);
}

GenericFunctions GenericFunctionTable::TakeAll() &&
{
	return std::move(m_functions);
}

MarmotCompiler::Optional<std::string> GenericFunctionTable::KeyIn(const std::string& module_name, const std::string& symbol_name) const
{
	const std::string key = module_name == m_module_name ? symbol_name : module_name + std::string(NameSeparator) + symbol_name;
	return m_functions.contains(key) ? MarmotCompiler::Optional<std::string>(key) : MarmotCompiler::None;
}

MarmotCompiler::Optional<std::string> GenericFunctionTable::FindKey(const std::string& resolved_name, const MarmotCompiler::Optional<std::string>& specialization_source_module) const
{
	const size_t separator_pos = resolved_name.rfind(NameSeparator);
	if (separator_pos != std::string::npos)
	{
		return KeyIn(resolved_name.substr(0u, separator_pos), resolved_name.substr(separator_pos + NameSeparator.length()));
	}

	if (specialization_source_module.HasValue())
	{
		return KeyIn(specialization_source_module.Value(), resolved_name);
	}

	return m_functions.contains(resolved_name) ? MarmotCompiler::Optional<std::string>(resolved_name) : MarmotCompiler::None;
}
