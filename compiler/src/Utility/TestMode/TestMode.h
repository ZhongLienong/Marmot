#pragma once

// MARMOT_TEST_MODE, or an override, silences the compiler's internal
// diagnostics so test output holds only what a program prints.
namespace CompilerTestMode
{
	class ScopedOverride
	{
	private:
		int m_previous_value = -1;

	public:
		explicit ScopedOverride(bool enabled) noexcept;

		~ScopedOverride() noexcept;

		ScopedOverride(const ScopedOverride&) = delete;
		ScopedOverride& operator=(const ScopedOverride&) = delete;
		ScopedOverride(ScopedOverride&&) = delete;
		ScopedOverride& operator=(ScopedOverride&&) = delete;
	};

	[[nodiscard]] bool ShouldEmitInternalDiagnostics() noexcept;
}
