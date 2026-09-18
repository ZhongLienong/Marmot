#include "support/TempDir.h"

#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace
{
	std::atomic_uint64_t s_temp_dir_counter = 0u;
}

namespace MidoriTest
{
	TempDir::TempDir()
		: TempDir("marmot-test")
	{
	}

	TempDir::TempDir(std::string_view prefix)
		: m_path(MakeUniquePath(prefix))
	{
		std::filesystem::create_directories(m_path);
	}

	TempDir::~TempDir() noexcept
	{
		Cleanup();
	}

	TempDir::TempDir(TempDir&& other) noexcept
		: m_path(std::move(other.m_path))
	{
		other.m_path.clear();
	}

	TempDir& TempDir::operator=(TempDir&& other) noexcept
	{
		if (this != &other)
		{
			Cleanup();
			m_path = std::move(other.m_path);
			other.m_path.clear();
		}

		return *this;
	}

	const std::filesystem::path& TempDir::Path() const
	{
		return m_path;
	}

	std::filesystem::path TempDir::CreateDirectory(const std::filesystem::path& relative_path) const
	{
		const std::filesystem::path full_path = m_path / relative_path;
		std::filesystem::create_directories(full_path);
		return full_path;
	}

	std::filesystem::path TempDir::WriteTextFile(const std::filesystem::path& relative_path, std::string_view contents) const
	{
		const std::filesystem::path full_path = m_path / relative_path;
		if (full_path.has_parent_path())
		{
			std::filesystem::create_directories(full_path.parent_path());
		}

		std::ofstream output(full_path, std::ios::binary);
		if (!output.is_open())
		{
			throw std::runtime_error(std::format("Failed to open temporary file '{}'.", full_path.string()));
		}

		output << contents;
		if (!output.good())
		{
			throw std::runtime_error(std::format("Failed to write temporary file '{}'.", full_path.string()));
		}

		return full_path;
	}

	std::filesystem::path TempDir::MakeUniquePath(std::string_view prefix)
	{
		const std::filesystem::path temp_root = std::filesystem::temp_directory_path();
		const uint64_t counter_value = s_temp_dir_counter.fetch_add(1u) + 1u;
		const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
		const long long tick_count = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
		return temp_root / std::format("{}-{}-{}", prefix, tick_count, counter_value);
	}

	void TempDir::Cleanup() noexcept
	{
		if (m_path.empty())
		{
			return;
		}

		std::error_code error_code;
		std::filesystem::remove_all(m_path, error_code);
		m_path.clear();
	}
}
