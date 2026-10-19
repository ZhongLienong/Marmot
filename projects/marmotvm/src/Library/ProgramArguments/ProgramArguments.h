#pragma once

#include <string>
#include <vector>

// What the program was given after the VM's own arguments: set once before it
// starts, and only read after that, from any worker.
namespace ProgramArguments
{
	void Set(std::vector<std::string> arguments);

	[[nodiscard]] const std::vector<std::string>& Get();
}
