#include "TestMode.h"

#include <atomic>
#include <cstdlib>
#include <string_view>

namespace
{
	std::atomic<int> s_override{ -1 };

	bool EnvironmentFlagEnabled(const char* name) noexcept
	{
#ifdef _WIN32
		char* value = nullptr;
		size_t length = 0u;
		if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
		{
			return false;
		}
#else
		const char* value = std::getenv(name);
		if (value == nullptr)
		{
			return false;
		}
#endif

		const std::string_view view(value);
		const bool enabled = !view.empty() && view != "0" && view != "false" && view != "False" && view != "FALSE";

#ifdef _WIN32
		free(value);
#endif

		return enabled;
	}

	bool IsTestMode() noexcept
	{
		const int override_value = s_override.load(std::memory_order_acquire);
		if (override_value >= 0)
		{
			return override_value != 0;
		}

		static const bool s_is_test_mode = EnvironmentFlagEnabled("MARMOT_TEST_MODE");
		return s_is_test_mode;
	}
}

namespace RuntimeTestMode
{
	ScopedOverride::ScopedOverride(bool enabled) noexcept
		: m_previous_value(s_override.exchange(enabled ? 1 : 0, std::memory_order_acq_rel))
	{
	}

	ScopedOverride::~ScopedOverride() noexcept
	{
		s_override.store(m_previous_value, std::memory_order_release);
	}

	bool ShouldEmitInternalDiagnostics() noexcept
	{
		return !IsTestMode();
	}
}
