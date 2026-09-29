#include "GenericFunctionTable.h"
#include "Bytecode/Format/Format.h"
#include "Compiler/Constant/Constant.h"

GenericFunctionTable::GenericFunctionTable(std::string module_name, Functions imported)
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

const GenericFunctionTable::Functions& GenericFunctionTable::All() const
{
	return m_functions;
}

GenericFunctionTable::Functions GenericFunctionTable::TakeAll() &&
{
	return std::move(m_functions);
}

std::optional<std::string> GenericFunctionTable::KeyIn(const std::string& module_name, const std::string& symbol_name) const
{
	const std::string key = module_name == m_module_name ? symbol_name : module_name + std::string(NameSeparator) + symbol_name;
	return m_functions.contains(key) ? std::optional<std::string>(key) : std::nullopt;
}

std::optional<std::string> GenericFunctionTable::FindKey(const std::string& resolved_name, const std::optional<std::string>& specialization_source_module) const
{
	const size_t at_pos = resolved_name.find(ModuleSeparator);
	if (at_pos != std::string::npos)
	{
		return KeyIn(resolved_name.substr(at_pos + 1u), resolved_name.substr(0u, at_pos));
	}

	const size_t separator_pos = resolved_name.rfind(NameSeparator);
	if (separator_pos != std::string::npos)
	{
		return KeyIn(resolved_name.substr(0u, separator_pos), resolved_name.substr(separator_pos + NameSeparator.length()));
	}

	if (specialization_source_module.has_value())
	{
		return KeyIn(specialization_source_module.value(), resolved_name);
	}

	return m_functions.contains(resolved_name) ? std::optional<std::string>(resolved_name) : std::nullopt;
}
