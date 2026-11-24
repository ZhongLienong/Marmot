#pragma once

#include "Compiler/Module/GenericFunctionInfo.h"

#include <optional>
#include <string>

// Every generic function a module may specialize: its own, keyed by bare name,
// and every one its imports provide, keyed `Module::name`, so a name never
// reaches a generic of another module that happens to share it.
class GenericFunctionTable
{
private:
	std::string m_module_name;
	GenericFunctions m_functions;

public:
	GenericFunctionTable(std::string module_name, GenericFunctions imported);

	void Add(const std::string& name, GenericFunctionInfo info);
	bool Contains(const std::string& key) const;
	const GenericFunctionInfo& At(const std::string& key) const;
	GenericFunctions TakeAll() &&;

	std::optional<std::string> KeyIn(const std::string& module_name, const std::string& symbol_name) const;

	// The key of a source name: `Module::name`, or a bare name, which inside
	// a specialization of another module's generic is one of that module's.
	// Selected instances use KeyIn with their explicit declaring module.
	std::optional<std::string> FindKey(const std::string& resolved_name, const std::optional<std::string>& specialization_source_module) const;
};
