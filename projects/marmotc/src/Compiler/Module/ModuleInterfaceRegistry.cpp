#include "ModuleInterfaceRegistry.h"

#include <utility>

ModuleInterfaceRegistry::ModuleInterfaceRegistry(size_t module_count)
{
	m_interfaces.reserve(module_count);
}

void ModuleInterfaceRegistry::Publish(const std::string& file_path, std::shared_ptr<const ModuleInterface> interface)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_interfaces.emplace(file_path, std::move(interface));
}

std::vector<std::shared_ptr<const ModuleInterface>> ModuleInterfaceRegistry::Snapshot(std::span<const std::string> file_paths) const
{
	std::vector<std::shared_ptr<const ModuleInterface>> interfaces;
	interfaces.reserve(file_paths.size());
	std::lock_guard<std::mutex> lock(m_mutex);
	for (const std::string& file_path : file_paths)
	{
		interfaces.push_back(m_interfaces.at(file_path));
	}
	return interfaces;
}
