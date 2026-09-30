#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace MidoriUtility
{
	struct CapturedOutput
	{
		std::string m_stdout;
		std::string m_stderr;

		CapturedOutput() = default;
		CapturedOutput(std::string stdout_output, std::string stderr_output);
	};

	class OutputCapture
	{
	public:
		enum class Stream
		{
			StdOut,
			StdErr,
			Both
		};

	private:
		struct StreamState
		{
			bool m_enabled = false;
			int m_saved_fd = -1;
			int m_target_fd = -1;
			FILE* m_target_stream = nullptr;
			FILE* m_capture_file = nullptr;
			std::string m_label;
			std::filesystem::path m_capture_path;
		};

		StreamState m_stdout;
		StreamState m_stderr;
		bool m_active = false;

	public:
		explicit OutputCapture(Stream stream = Stream::Both);

		~OutputCapture() noexcept;

		OutputCapture(const OutputCapture&) = delete;
		OutputCapture& operator=(const OutputCapture&) = delete;
		OutputCapture(OutputCapture&&) = delete;
		OutputCapture& operator=(OutputCapture&&) = delete;

		[[nodiscard]] CapturedOutput Stop();

	private:
		static StreamState MakeStreamState(int target_fd, FILE* target_stream, std::string_view label);

		static void StartCapture(StreamState& state);

		[[nodiscard]] static std::string StopCapture(StreamState& state);

		static void CleanupCapture(StreamState& state) noexcept;
	};
}
