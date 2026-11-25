#include "support/ScopedEnvVar.h"
#include "Support/Optional.h"

#include <cstdlib>
#include <format>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <stdlib.h>
#endif

namespace MarmotTest
{
	ScopedEnvVar::ScopedEnvVar(std::string name, MarmotCompiler::Optional<std::string> value)
		: m_name(std::move(name)),
		m_original_value(Read(m_name))
	{
		if (!ApplyValue(m_name, value))
		{
			throw std::runtime_error(std::format("Failed to update environment variable '{}'.", m_name));
		}
	}

	ScopedEnvVar::ScopedEnvVar(std::string name, std::string value)
		: ScopedEnvVar(std::move(name), MarmotCompiler::Optional<std::string>(std::move(value)))
	{
	}

	ScopedEnvVar::~ScopedEnvVar() noexcept
	{
		static_cast<void>(ApplyValue(m_name, m_original_value));
	}

	MarmotCompiler::Optional<std::string> ScopedEnvVar::Read(std::string_view name)
	{
#ifdef _WIN32
		char* value = nullptr;
		size_t length = 0u;
		const std::string env_name(name);
		if (_dupenv_s(&value, &length, env_name.c_str()) != 0 || value == nullptr)
		{
			return MarmotCompiler::None;
		}

		std::string result(value);
		std::free(value);
		if (result.empty())
		{
			return MarmotCompiler::None;
		}

		return result;
#else
		const std::string env_name(name);
		const char* value = std::getenv(env_name.c_str());
		if (value == nullptr || value[0] == '\0')
		{
			return MarmotCompiler::None;
		}

		return std::string(value);
#endif
	}

	bool ScopedEnvVar::ApplyValue(std::string_view name, const MarmotCompiler::Optional<std::string>& value) noexcept
	{
		const std::string env_name(name);

#ifdef _WIN32
		if (value.HasValue())
		{
			return _putenv_s(env_name.c_str(), value->c_str()) == 0;
		}

		return _putenv_s(env_name.c_str(), "") == 0;
#else
		if (value.HasValue())
		{
			return setenv(env_name.c_str(), value->c_str(), 1) == 0;
		}

		return unsetenv(env_name.c_str()) == 0;
#endif
	}
}
