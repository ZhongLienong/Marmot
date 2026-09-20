#include "Common/Source/Source.h"

namespace MidoriSource
{
	bool StartsWithByteOrderMark(std::string_view text)
	{
		return text.starts_with(UTF8_BYTE_ORDER_MARK);
	}

	void RemoveByteOrderMark(std::string& text)
	{
		if (StartsWithByteOrderMark(text))
		{
			text.erase(0u, UTF8_BYTE_ORDER_MARK.size());
		}
	}
}
