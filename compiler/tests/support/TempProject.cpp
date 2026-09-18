#include "support/TempProject.h"

#include <utility>

namespace MidoriTest
{
	TempProjectFile::TempProjectFile(std::filesystem::path relative_path, std::string contents)
		: m_relative_path(std::move(relative_path)),
		m_contents(std::move(contents))
	{
	}

	TempProject::TempProject()
		: m_root("marmot-project")
	{
	}

	TempProject::TempProject(std::vector<TempProjectFile> files)
		: TempProject()
	{
		for (const TempProjectFile& file : files)
		{
			static_cast<void>(WriteFile(file.m_relative_path, file.m_contents));
		}
	}

	const std::filesystem::path& TempProject::Root() const
	{
		return m_root.Path();
	}

	std::filesystem::path TempProject::Path(const std::filesystem::path& relative_path) const
	{
		return m_root.Path() / relative_path;
	}

	std::filesystem::path TempProject::WriteFile(const std::filesystem::path& relative_path, std::string_view contents) const
	{
		return m_root.WriteTextFile(relative_path, contents);
	}

	std::filesystem::path TempProject::WriteSourceFile(const std::filesystem::path& relative_path, std::string_view contents) const
	{
		return WriteFile(relative_path, contents);
	}
}
