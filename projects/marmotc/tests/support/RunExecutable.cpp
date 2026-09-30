#include "support/CompileHelpers.h"
#include "support/TempDir.h"
#include "Bytecode/Artifact/BinaryArtifact.h"

#include <cstdlib>
#include <format>
#include <stdexcept>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace
{
	std::string NormalizeNewlines(std::string text)
	{
		for (size_t position = text.find("\r\n"); position != std::string::npos; position = text.find("\r\n", position))
		{
			text.erase(position, 1u);
		}
		return text;
	}

	std::string QuoteArgument(std::string_view argument)
	{
#if defined(_WIN32)
		std::string quoted = "\"";
		size_t backslashes = 0u;
		for (char character : argument)
		{
			if (character == '\\')
			{
				backslashes += 1u;
				continue;
			}
			quoted.append(character == '"' ? backslashes * 2u + 1u : backslashes, '\\');
			quoted.push_back(character);
			backslashes = 0u;
		}
		quoted.append(backslashes * 2u, '\\');
		return quoted + '"';
#else
		std::string quoted = "'";
		for (char character : argument)
		{
			quoted += character == '\'' ? "'\\''" : std::string(1u, character);
		}
		return quoted + '\'';
#endif
	}
}

namespace MidoriTest
{
	std::pair<int, CapturedOutput> RunExecutable(const MidoriExecutable& executable, const std::vector<std::string>& arguments)
	{
		const char* vm = std::getenv("MARMOTVM");
		if (vm == nullptr)
		{
			throw std::runtime_error("Set MARMOTVM to run the repository integration tests.");
		}
		const TempDir temporary("marmot-integration");
		const std::filesystem::path artifact = temporary.Path() / "program.mmc";
		MidoriBinaryArtifact::WriteExecutableToFile(executable, artifact, true).value();
		std::string command = QuoteArgument(vm) + " " + QuoteArgument(artifact.string());
		for (const std::string& argument : arguments)
		{
			command += " " + QuoteArgument(argument);
		}
#if defined(_WIN32)
		command = '"' + command + '"';
#endif
		OutputCapture capture;
		const int status = std::system(command.c_str());
#if defined(_WIN32)
		const int exit_code = status;
#else
		const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
		CapturedOutput output = capture.Stop();
		output.m_stdout = NormalizeNewlines(std::move(output.m_stdout));
		output.m_stderr = NormalizeNewlines(std::move(output.m_stderr));
		return { exit_code, std::move(output) };
	}
}
