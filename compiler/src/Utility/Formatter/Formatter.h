#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "Common/Error/Error.h"

namespace MidoriFormatter
{
	struct Options
	{
		bool m_write_in_place = false;
		bool m_check_only = false;
	};

	struct FileResult
	{
		std::filesystem::path m_path;
		bool m_changed = false;
		bool m_written = false;
		std::string m_original_text;
		std::string m_formatted_text;
		std::optional<std::string> m_error;
	};

	struct RunResult
	{
		std::vector<FileResult> m_files;

		[[nodiscard]] bool HasErrors() const;
		[[nodiscard]] bool HasChanges() const;
		[[nodiscard]] int ErrorCount() const;
		[[nodiscard]] int ChangedCount() const;
	};

	[[nodiscard]] std::expected<std::string, CompilerError> FormatSource(std::string_view source_code, std::string_view file_name);

	[[nodiscard]] RunResult FormatPath(const std::filesystem::path& target_path, const Options& options);
}
