#pragma once

#include "Compiler/Lowering/GenericFunctionInfo.h"

#include <optional>
#include <string>
#include <unordered_map>

// Every generic function a module may specialize: its own, keyed by bare name,
// and every one its imports provide, keyed `Module::name`, so a name never
// reaches a generic of another module that happens to share it.
class GenericFunctionTable
{
public:
	using Functions = std::unordered_map<std::string, GenericFunctionInfo>;

private:
	std::string m_module_name;
	Functions m_functions;

public:
	GenericFunctionTable(std::string module_name, Functions imported);

	void Add(const std::string& name, GenericFunctionInfo info);
	bool Contains(const std::string& key) const;
	const GenericFunctionInfo& At(const std::string& key) const;
	const Functions& All() const;
	Functions TakeAll() &&;

	std::optional<std::string> KeyIn(const std::string& module_name, const std::string& symbol_name) const;

	// The key of the generic a resolved name refers to: `name@Module`,
	// `Module::name`, or a bare name, which inside a specialization of another
	// module's generic is one of that module's.
	std::optional<std::string> FindKey(const std::string& resolved_name, const std::optional<std::string>& specialization_source_module) const;
};
