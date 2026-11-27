#include "ModuleExportMetadataRegistry.h"

#include <utility>

ModuleExportMetadataRegistry::ModuleExportMetadataRegistry(size_t module_count)
{
	m_metadata_by_file.reserve(module_count);
}

void ModuleExportMetadataRegistry::Publish(const std::string& file_path, std::shared_ptr<const ModuleExportMetadata> metadata)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_metadata_by_file.emplace(file_path, std::move(metadata));
}

std::vector<std::shared_ptr<const ModuleExportMetadata>> ModuleExportMetadataRegistry::Snapshot(std::span<const std::string> file_paths) const
{
	std::vector<std::shared_ptr<const ModuleExportMetadata>> snapshot;
	snapshot.reserve(file_paths.size());
	std::lock_guard<std::mutex> lock(m_mutex);
	for (const std::string& file_path : file_paths)
	{
		snapshot.push_back(m_metadata_by_file.at(file_path));
	}
	return snapshot;
}
