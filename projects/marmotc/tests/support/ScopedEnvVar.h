#pragma once

#include <string>
#include <string_view>

#include "Support/Optional.h"

namespace MarmotTest
{
	class ScopedEnvVar
	{
	private:
		std::string m_name;
		MarmotCompiler::Optional<std::string> m_original_value;

	public:
		ScopedEnvVar(std::string name, MarmotCompiler::Optional<std::string> value);

		ScopedEnvVar(std::string name, std::string value);

		~ScopedEnvVar() noexcept;

		ScopedEnvVar(const ScopedEnvVar&) = delete;
		ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;
		ScopedEnvVar(ScopedEnvVar&&) = delete;
		ScopedEnvVar& operator=(ScopedEnvVar&&) = delete;

		[[nodiscard]] static MarmotCompiler::Optional<std::string> Read(std::string_view name);

	private:
		static bool ApplyValue(std::string_view name, const MarmotCompiler::Optional<std::string>& value) noexcept;
	};
}
