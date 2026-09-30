#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace MidoriTest
{
	class ScopedEnvVar
	{
	private:
		std::string m_name;
		std::optional<std::string> m_original_value;

	public:
		ScopedEnvVar(std::string name, std::optional<std::string> value);

		ScopedEnvVar(std::string name, std::string value);

		~ScopedEnvVar() noexcept;

		ScopedEnvVar(const ScopedEnvVar&) = delete;
		ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;
		ScopedEnvVar(ScopedEnvVar&&) = delete;
		ScopedEnvVar& operator=(ScopedEnvVar&&) = delete;

		[[nodiscard]] static std::optional<std::string> Read(std::string_view name);

	private:
		static bool ApplyValue(std::string_view name, const std::optional<std::string>& value) noexcept;
	};
}
