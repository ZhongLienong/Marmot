#pragma once

#include <cstdio>
#include <string_view>

namespace RuntimeDiagnostics
{
	void Configure(bool trace, bool opcode_metrics, bool gc_metrics) noexcept;
	[[nodiscard]] bool TraceEnabled() noexcept;
	[[nodiscard]] bool OpcodeMetricsEnabled() noexcept;
	[[nodiscard]] bool GcMetricsEnabled() noexcept;
	[[nodiscard]] std::FILE* Output() noexcept;
	void Print(std::string_view text);

	// Preserve the diagnostic stream while JSON execution captures program output.
	class ScopedOutput
	{
	private:
		std::FILE* m_output;

	public:
		ScopedOutput();
		~ScopedOutput();
		ScopedOutput(const ScopedOutput&) = delete;
		ScopedOutput& operator=(const ScopedOutput&) = delete;
	};
}
