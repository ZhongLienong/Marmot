#include "Bytecode/Intrinsics/IntrinsicTable.h"

#include "Interpreter/VirtualMachine/VirtualMachine.h"
#include "Library/ProgramArguments/ProgramArguments.h"
#include "Support/Cancellation/Cancellation.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <ranges>
#include <utility>

namespace
{
	const MarmotArray& Array(const MarmotValue& value) noexcept
	{
		return value.GetPointer()->GetTraceable<MarmotArray>();
	}

	const MarmotText& Text(const MarmotValue& value) noexcept
	{
		return value.GetPointer()->GetTraceable<MarmotText>();
	}
}

namespace VmIntrinsics
{
	MarmotValue Exit(VirtualMachine&, std::span<const MarmotValue> args, bool) noexcept
	{
		std::exit(static_cast<int>(args[0u].GetInteger()));
	}

	MarmotValue Sleep(VirtualMachine&, std::span<const MarmotValue> args, bool) noexcept
	{
		ThreadCancellation::SleepInterruptible(std::chrono::milliseconds(args[0u].GetInteger()));
		return {};
	}

	MarmotValue ArrayConcat(VirtualMachine& vm, std::span<const MarmotValue> args, bool holds_pointers) noexcept
	{
		return vm.AllocateArray(MarmotArray::Concatenate(Array(args[0u]), Array(args[1u])), holds_pointers);
	}

	MarmotValue ArrayLength(VirtualMachine&, std::span<const MarmotValue> args, bool) noexcept
	{
		return static_cast<MarmotInteger>(Array(args[0u]).GetLength());
	}

	MarmotValue ArraySlice(VirtualMachine& vm, std::span<const MarmotValue> args, bool holds_pointers) noexcept
	{
		return vm.AllocateArray(Array(args[0u]).Slice(args[1u].GetInteger(), args[2u].GetInteger()), holds_pointers);
	}

	MarmotValue ArrayReverse(VirtualMachine& vm, std::span<const MarmotValue> args, bool holds_pointers) noexcept
	{
		return vm.AllocateArray(Array(args[0u]).Reverse(), holds_pointers);
	}

	MarmotValue ArrayConcatAll(VirtualMachine& vm, std::span<const MarmotValue> args, bool holds_pointers) noexcept
	{
		const MarmotArray& chunks = Array(args[0u]);
		MarmotInteger length = 0;
		for (int index = 0; index < chunks.GetLength(); index += 1)
		{
			length += Array(chunks[index]).GetLength();
			if (length > std::numeric_limits<int>::max())
			{
				FatalOutOfMemory("concatenating arrays", static_cast<size_t>(length) * sizeof(MarmotValue));
			}
		}

		MarmotArray result(static_cast<int>(length));
		int destination = 0;
		for (int index = 0; index < chunks.GetLength(); index += 1)
		{
			const MarmotArray& chunk = Array(chunks[index]);
			if (chunk.GetLength() > 0)
			{
				std::ranges::copy_n(&chunk[0], chunk.GetLength(), &result[destination]);
				destination += chunk.GetLength();
			}
		}
		return vm.AllocateArray(std::move(result), holds_pointers);
	}

	MarmotValue TextConcat(VirtualMachine& vm, std::span<const MarmotValue> args, bool) noexcept
	{
		return vm.AllocateTraceable(MarmotText::Concatenate(Text(args[0u]), Text(args[1u])));
	}

	MarmotValue TextSubstring(VirtualMachine& vm, std::span<const MarmotValue> args, bool) noexcept
	{
		return vm.AllocateTraceable(Text(args[0u]).Substring(args[1u].GetInteger(), args[2u].GetInteger()));
	}

	MarmotValue TextSplit(VirtualMachine& vm, std::span<const MarmotValue> args, bool) noexcept
	{
		std::vector<MarmotText> parts = Text(args[0u]).Split(Text(args[1u]));
		MarmotArray result(static_cast<int>(parts.size()));
		for (int index = 0; index < result.GetLength(); index += 1)
		{
			result[index] = vm.AllocateTraceable(std::move(parts[static_cast<size_t>(index)]));
		}
		return vm.AllocateArray(std::move(result), true);
	}

	MarmotValue TextReverse(VirtualMachine& vm, std::span<const MarmotValue> args, bool) noexcept
	{
		return vm.AllocateTraceable(Text(args[0u]).Reverse());
	}

	MarmotValue TextFromCodePoint(VirtualMachine& vm, std::span<const MarmotValue> args, bool) noexcept
	{
		const std::array<MarmotInteger, 1u> code_point{ args[0u].GetInteger() };
		return vm.AllocateTraceable(MarmotText::FromCodePoints(code_point));
	}

	MarmotValue TextCodePoints(VirtualMachine& vm, std::span<const MarmotValue> args, bool) noexcept
	{
		const std::vector<MarmotInteger> code_points = Text(args[0u]).CodePoints();
		MarmotArray values(static_cast<int>(code_points.size()));
		for (int index = 0; index < values.GetLength(); index += 1)
		{
			values[index] = MarmotValue(code_points[static_cast<size_t>(index)]);
		}
		return vm.AllocateArray(std::move(values), false);
	}

	MarmotValue TextFromCodePoints(VirtualMachine& vm, std::span<const MarmotValue> args, bool) noexcept
	{
		const MarmotArray& values = Array(args[0u]);
		const std::vector<MarmotInteger> code_points = std::views::iota(0, values.GetLength())
			| std::views::transform([&values](int index) { return values[index].GetInteger(); })
			| std::ranges::to<std::vector>();
		return vm.AllocateTraceable(MarmotText::FromCodePoints(code_points));
	}

	// The code point at a code-point index, or -1 past the end.
	MarmotValue TextCodePointAt(VirtualMachine&, std::span<const MarmotValue> args, bool) noexcept
	{
		const MarmotText& text = Text(args[0u]);
		const MarmotInteger index = args[1u].GetInteger();
		if ((index < 0) || (index >= text.GetLength()))
		{
			return MarmotInteger{ -1 };
		}

		const MarmotText character = text.Substring(index, index + 1);
		const unsigned char* bytes = reinterpret_cast<const unsigned char*>(character.View().data());
		const int length = character.GetByteLength();
		std::uint32_t code_point = (length == 1) ? bytes[0]
			: (length == 2) ? (bytes[0] & 0x1Fu)
			: (length == 3) ? (bytes[0] & 0x0Fu)
			: (bytes[0] & 0x07u);
		for (int continuation = 1; continuation < length; continuation += 1)
		{
			code_point = (code_point << 6u) | (bytes[continuation] & 0x3Fu);
		}
		return static_cast<MarmotInteger>(code_point);
	}

	MarmotValue TextIsInteger(VirtualMachine&, std::span<const MarmotValue> args, bool) noexcept
	{
		return Text(args[0u]).ParseInteger().has_value();
	}

	MarmotValue TextIsFloat(VirtualMachine&, std::span<const MarmotValue> args, bool) noexcept
	{
		return Text(args[0u]).ParseFloat().has_value();
	}

	// UTF-8 byte order is code point order.
	MarmotValue TextCompare(VirtualMachine&, std::span<const MarmotValue> args, bool) noexcept
	{
		const int order = Text(args[0u]).View().compare(Text(args[1u]).View());
		return static_cast<MarmotInteger>((order > 0) - (order < 0));
	}

	MarmotValue TextContains(VirtualMachine&, std::span<const MarmotValue> args, bool) noexcept
	{
		return Text(args[0u]).Contains(Text(args[1u]));
	}

	MarmotValue TextReplace(VirtualMachine& vm, std::span<const MarmotValue> args, bool) noexcept
	{
		return vm.AllocateTraceable(Text(args[0u]).Replace(Text(args[1u]), Text(args[2u])));
	}

	MarmotValue TextTrim(VirtualMachine& vm, std::span<const MarmotValue> args, bool) noexcept
	{
		return vm.AllocateTraceable(Text(args[0u]).Trim());
	}

	MarmotValue GetArguments(VirtualMachine& vm, std::span<const MarmotValue>, bool) noexcept
	{
		const std::vector<std::string>& arguments = ProgramArguments::Get();
		MarmotArray result(static_cast<int>(arguments.size()));
		for (int index = 0; index < result.GetLength(); index += 1)
		{
			result[index] = vm.AllocateTraceable(MarmotText(arguments[static_cast<size_t>(index)].c_str()));
		}
		return vm.AllocateArray(std::move(result), true);
	}

	MarmotValue TextLength(VirtualMachine&, std::span<const MarmotValue> args, bool) noexcept
	{
		return static_cast<MarmotInteger>(Text(args[0u]).GetLength());
	}

	MarmotValue HashText(VirtualMachine&, std::span<const MarmotValue> args, bool) noexcept
	{
		return static_cast<MarmotInteger>(std::hash<std::string_view>{}(Text(args[0u]).View()));
	}

	MarmotValue HashFloat(VirtualMachine&, std::span<const MarmotValue> args, bool) noexcept
	{
		return static_cast<MarmotInteger>(std::hash<MarmotFloat>{}(args[0u].GetFloat()));
	}

	MarmotValue ReportPanic(VirtualMachine&, std::span<const MarmotValue> args, bool) noexcept
	{
		std::cerr << Text(args[0u]).View();
		return {};
	}
}
