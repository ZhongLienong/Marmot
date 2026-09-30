#pragma once

#include "support/TempDir.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace MidoriTest
{
	struct TempProjectFile
	{
		std::filesystem::path m_relative_path;
		std::string m_contents;

		TempProjectFile(std::filesystem::path relative_path, std::string contents);
	};

	class TempProject
	{
	private:
		TempDir m_root;

	public:
		TempProject();

		explicit TempProject(std::vector<TempProjectFile> files);

		[[nodiscard]] const std::filesystem::path& Root() const;

		[[nodiscard]] std::filesystem::path Path(const std::filesystem::path& relative_path) const;

		[[nodiscard]] std::filesystem::path WriteFile(const std::filesystem::path& relative_path, std::string_view contents) const;

		[[nodiscard]] std::filesystem::path WriteSourceFile(const std::filesystem::path& relative_path, std::string_view contents) const;
	};
}
