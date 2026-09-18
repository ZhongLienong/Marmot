#include "Utility/OutputCapture/OutputCapture.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <format>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace
{
	std::atomic_uint64_t s_capture_counter = 0u;

	void FlushStandardStreams()
	{
		std::cout.flush();
		std::cerr.flush();
		std::fflush(stdout);
		std::fflush(stderr);
	}

	int DuplicateDescriptor(int descriptor)
	{
#ifdef _WIN32
		return _dup(descriptor);
#else
		return dup(descriptor);
#endif
	}

	bool DuplicateDescriptorTo(int source_descriptor, int target_descriptor) noexcept
	{
#ifdef _WIN32
		return _dup2(source_descriptor, target_descriptor) == 0;
#else
		return dup2(source_descriptor, target_descriptor) >= 0;
#endif
	}

	void CloseDescriptor(int descriptor) noexcept
	{
		if (descriptor < 0)
		{
			return;
		}

#ifdef _WIN32
		_close(descriptor);
#else
		close(descriptor);
#endif
	}

	int GetDescriptor(FILE* stream)
	{
#ifdef _WIN32
		return _fileno(stream);
#else
		return fileno(stream);
#endif
	}

	std::filesystem::path MakeCapturePath(std::string_view label)
	{
		const uint64_t counter_value = s_capture_counter.fetch_add(1u) + 1u;
		const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
		const long long tick_count = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
		return std::filesystem::temp_directory_path() / std::format("marmot-{}-{}-{}.log", label, tick_count, counter_value);
	}

	std::string ReadFileContents(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open())
		{
			return {};
		}

		return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	}
}

namespace MidoriUtility
{
	CapturedOutput::CapturedOutput(std::string stdout_output, std::string stderr_output)
		: m_stdout(std::move(stdout_output)),
		m_stderr(std::move(stderr_output))
	{
	}

	OutputCapture::OutputCapture(Stream stream)
		: m_active(true)
	{
		if (stream == Stream::StdOut || stream == Stream::Both)
		{
			m_stdout = MakeStreamState(GetDescriptor(stdout), stdout, "stdout");
			StartCapture(m_stdout);
		}

		if (stream == Stream::StdErr || stream == Stream::Both)
		{
			m_stderr = MakeStreamState(GetDescriptor(stderr), stderr, "stderr");
			StartCapture(m_stderr);
		}
	}

	OutputCapture::~OutputCapture() noexcept
	{
		if (!m_active)
		{
			return;
		}

		CleanupCapture(m_stdout);
		CleanupCapture(m_stderr);
	}

	CapturedOutput OutputCapture::Stop()
	{
		std::string stdout_output = StopCapture(m_stdout);
		std::string stderr_output = StopCapture(m_stderr);
		m_active = false;
		return CapturedOutput(std::move(stdout_output), std::move(stderr_output));
	}

	OutputCapture::StreamState OutputCapture::MakeStreamState(int target_fd, FILE* target_stream, std::string_view label)
	{
		StreamState state;
		state.m_enabled = true;
		state.m_target_fd = target_fd;
		state.m_target_stream = target_stream;
		state.m_label = std::string(label);
		return state;
	}

	void OutputCapture::StartCapture(StreamState& state)
	{
		if (!state.m_enabled)
		{
			return;
		}

		FlushStandardStreams();

		state.m_capture_path = MakeCapturePath(state.m_label);
		state.m_saved_fd = DuplicateDescriptor(state.m_target_fd);
		if (state.m_saved_fd < 0)
		{
			throw std::runtime_error(std::format("Failed to duplicate {} descriptor.", state.m_label));
		}

		state.m_capture_file = std::fopen(state.m_capture_path.string().c_str(), "w+b");
		if (state.m_capture_file == nullptr)
		{
			CloseDescriptor(state.m_saved_fd);
			state.m_saved_fd = -1;
			throw std::runtime_error(std::format("Failed to open capture file for {}.", state.m_label));
		}

		if (!DuplicateDescriptorTo(GetDescriptor(state.m_capture_file), state.m_target_fd))
		{
			std::fclose(state.m_capture_file);
			state.m_capture_file = nullptr;
			CloseDescriptor(state.m_saved_fd);
			state.m_saved_fd = -1;
			std::error_code error_code;
			std::filesystem::remove(state.m_capture_path, error_code);
			state.m_capture_path.clear();
			throw std::runtime_error(std::format("Failed to redirect {}.", state.m_label));
		}
	}

	std::string OutputCapture::StopCapture(StreamState& state)
	{
		if (!state.m_enabled)
		{
			return {};
		}

		FlushStandardStreams();

		if (state.m_saved_fd >= 0)
		{
			if (!DuplicateDescriptorTo(state.m_saved_fd, state.m_target_fd))
			{
				throw std::runtime_error(std::format("Failed to restore {}.", state.m_label));
			}

			CloseDescriptor(state.m_saved_fd);
			state.m_saved_fd = -1;
		}

		if (state.m_capture_file != nullptr)
		{
			std::fflush(state.m_capture_file);
			std::fclose(state.m_capture_file);
			state.m_capture_file = nullptr;
		}

		std::string captured_text = ReadFileContents(state.m_capture_path);
		std::error_code error_code;
		std::filesystem::remove(state.m_capture_path, error_code);
		state.m_capture_path.clear();
		state.m_enabled = false;
		return captured_text;
	}

	void OutputCapture::CleanupCapture(StreamState& state) noexcept
	{
		if (!state.m_enabled)
		{
			return;
		}

		FlushStandardStreams();

		if (state.m_saved_fd >= 0)
		{
			static_cast<void>(DuplicateDescriptorTo(state.m_saved_fd, state.m_target_fd));
			CloseDescriptor(state.m_saved_fd);
			state.m_saved_fd = -1;
		}

		if (state.m_capture_file != nullptr)
		{
			std::fflush(state.m_capture_file);
			std::fclose(state.m_capture_file);
			state.m_capture_file = nullptr;
		}

		if (!state.m_capture_path.empty())
		{
			std::error_code error_code;
			std::filesystem::remove(state.m_capture_path, error_code);
			state.m_capture_path.clear();
		}

		state.m_enabled = false;
	}
}
