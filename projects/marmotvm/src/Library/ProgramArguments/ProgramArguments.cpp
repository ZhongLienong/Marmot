#include "Library/ProgramArguments/ProgramArguments.h"

namespace
{
	std::vector<std::string> s_arguments;
}

namespace ProgramArguments
{
	void Set(std::vector<std::string> arguments)
	{
		s_arguments = std::move(arguments);
	}

	const std::vector<std::string>& Get()
	{
		return s_arguments;
	}
}
