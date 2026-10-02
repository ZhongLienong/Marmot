#pragma once

#include "Bytecode/Executable/Executable.h"

#include <expected>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace MidoriStandalone
{
	inline constexpr int s_format_version = 1;

	struct NativeLibrary
	{
		std::string m_name;
		std::filesystem::path m_file_name;
		std::string m_bytes;

		NativeLibrary(std::string name, std::filesystem::path file_name, std::string bytes);
	};

	struct Package
	{
		VmExecutable m_program;
		std::vector<NativeLibrary> m_libraries;

		Package(VmExecutable program, std::vector<NativeLibrary> libraries);
	};

	[[nodiscard]] std::expected<std::optional<Package>, std::string> Read(std::istream& image);
	[[nodiscard]] std::expected<std::filesystem::path, std::string> CurrentExecutablePath();
	[[nodiscard]] std::expected<std::optional<Package>, std::string> ReadCurrentExecutable();
	[[nodiscard]] std::expected<int, std::string> Run(Package&& package);
}
