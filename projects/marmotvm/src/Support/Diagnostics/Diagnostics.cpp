#include "Diagnostics.h"

#include <atomic>
#include <cerrno>
#include <print>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace
{
	std::atomic<bool> s_trace = false;
	std::atomic<bool> s_opcode_metrics = false;
	std::atomic<bool> s_gc_metrics = false;
	std::atomic<std::FILE*> s_output = stderr;
}

void RuntimeDiagnostics::Configure(bool trace, bool opcode_metrics, bool gc_metrics) noexcept
{
	s_trace.store(trace);
	s_opcode_metrics.store(opcode_metrics);
	s_gc_metrics.store(gc_metrics);
}

bool RuntimeDiagnostics::TraceEnabled() noexcept
{
	return s_trace.load();
}

bool RuntimeDiagnostics::OpcodeMetricsEnabled() noexcept
{
	return s_opcode_metrics.load();
}

bool RuntimeDiagnostics::GcMetricsEnabled() noexcept
{
	return s_gc_metrics.load();
}

std::FILE* RuntimeDiagnostics::Output() noexcept
{
	return s_output.load();
}

void RuntimeDiagnostics::Print(std::string_view text)
{
	std::print(Output(), "{}", text);
}

RuntimeDiagnostics::ScopedOutput::ScopedOutput()
{
#ifdef _WIN32
	const int descriptor = _dup(_fileno(stderr));
	m_output = descriptor < 0 ? nullptr : _fdopen(descriptor, "w");
#else
	const int descriptor = dup(fileno(stderr));
	m_output = descriptor < 0 ? nullptr : fdopen(descriptor, "w");
#endif
	if (m_output == nullptr)
	{
		if (descriptor >= 0)
		{
#ifdef _WIN32
			_close(descriptor);
#else
			close(descriptor);
#endif
		}
		throw std::system_error(errno, std::generic_category(), "Open diagnostic stream");
	}
	s_output.store(m_output);
}

RuntimeDiagnostics::ScopedOutput::~ScopedOutput()
{
	s_output.store(stderr);
	std::fclose(m_output);
}
