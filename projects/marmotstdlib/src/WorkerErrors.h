#pragma once

#include <cstdint>
#include <string>

namespace StdlibWorkerErrors
{
	struct Status
	{
		int64_t m_kind = 0;
		std::string m_message;
	};

	struct State
	{
		Status m_io;
		Status m_system;
	};

	State& Current();
}
