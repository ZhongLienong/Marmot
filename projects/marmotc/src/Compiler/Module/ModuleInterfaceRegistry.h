#pragma once

#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

struct ModuleInterface;

class ModuleInterfaceRegistry
{
public:
	explicit ModuleInterfaceRegistry(size_t module_count);

	void Publish(const std::string& file_path, std::shared_ptr<const ModuleInterface> interface);
	std::vector<std::shared_ptr<const ModuleInterface>> Snapshot(std::span<const std::string> file_paths) const;

private:
	mutable std::mutex m_mutex;
	std::unordered_map<std::string, std::shared_ptr<const ModuleInterface>> m_interfaces;
};
