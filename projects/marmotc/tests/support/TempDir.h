#pragma once

#include <filesystem>
#include <string_view>

namespace MidoriTest
{
	class TempDir
	{
	private:
		std::filesystem::path m_path;

	public:
		TempDir();

		explicit TempDir(std::string_view prefix);

		~TempDir() noexcept;

		TempDir(const TempDir&) = delete;
		TempDir& operator=(const TempDir&) = delete;
		TempDir(TempDir&& other) noexcept;
		TempDir& operator=(TempDir&& other) noexcept;

		[[nodiscard]] const std::filesystem::path& Path() const;

		[[nodiscard]] std::filesystem::path CreateDirectory(const std::filesystem::path& relative_path) const;

		[[nodiscard]] std::filesystem::path WriteTextFile(const std::filesystem::path& relative_path, std::string_view contents) const;

	private:
		static std::filesystem::path MakeUniquePath(std::string_view prefix);

		void Cleanup() noexcept;
	};
}
