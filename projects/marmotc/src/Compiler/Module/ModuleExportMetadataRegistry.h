#pragma once

#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

struct ModuleExportMetadata;

class ModuleExportMetadataRegistry
{
public:
	explicit ModuleExportMetadataRegistry(size_t module_count);

	void Publish(const std::string& file_path, std::shared_ptr<const ModuleExportMetadata> metadata);
	std::vector<std::shared_ptr<const ModuleExportMetadata>> Snapshot(std::span<const std::string> file_paths) const;

private:
	mutable std::mutex m_mutex;
	std::unordered_map<std::string, std::shared_ptr<const ModuleExportMetadata>> m_metadata_by_file;
};
