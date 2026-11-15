#include "MarmotIRPrinter.h"
#include "Compiler/Constant/Constant.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <iterator>
#include <ranges>
#include <string_view>
#include <unordered_map>

namespace
{
	std::string JoinValues(const std::vector<MarmotIRValueId>& values, const std::vector<std::string>& names)
	{
		return values
			| std::views::transform([&names](MarmotIRValueId value) { return names[value.m_index]; })
			| std::views::join_with(std::string_view(", "))
			| std::ranges::to<std::string>();
	}

	// Shortest text that reads back as the same double, always with a point
	// or an exponent so it does not read as an Int.
	std::string PrintFloat(double value)
	{
		std::string text = std::format("{}", value);
		if (std::ranges::none_of(text, [](char character) { return character == '.' || character == 'e' || character == 'n' || character == 'i'; }))
		{
			text.append(".0");
		}
		return text;
	}

	std::string QuoteText(std::string_view text)
	{
		std::string quoted = "\"";
		for (const char character : text)
		{
			switch (character)
			{
			case '"':
				quoted.append("\\\"");
				break;
			case '\\':
				quoted.append("\\\\");
				break;
			case '\n':
				quoted.append("\\n");
				break;
			case '\t':
				quoted.append("\\t");
				break;
			case '\r':
				quoted.append("\\r");
				break;
			default:
				if (std::isprint(static_cast<unsigned char>(character)) != 0 || static_cast<unsigned char>(character) >= 0x80u)
				{
					quoted.push_back(character);
				}
				else
				{
					quoted.append(std::format("\\x{:02x}", static_cast<unsigned char>(character)));
				}
				break;
			}
		}
		quoted.push_back('"');
		return quoted;
	}

	std::string TerminatorName(MarmotIROp op)
	{
		return GetMarmotIROpInfo(op).m_name
			| std::views::transform([](char character) { return static_cast<char>(std::tolower(static_cast<unsigned char>(character))); })
			| std::ranges::to<std::string>();
	}
}

MarmotIRPrinter::MarmotIRPrinter(const MarmotIRModule& module)
	: m_module(module)
{
}

std::string MarmotIRPrinter::Print() const
{
	std::string text = std::format("module {}\n", m_module.m_name);
	for (size_t slot = 0u; slot < m_module.m_globals.size(); slot += 1u)
	{
		const MarmotIRGlobal& global = m_module.m_globals[slot];
		const std::string name = global.IsImported() ? std::format("{}{}{}", global.m_module, NameSeparator, global.m_name) : global.m_name;
		text.append(std::format("global @{} {}: {}\n", slot, name, global.m_type->ToString()));
	}
	if (m_module.m_top_level.has_value())
	{
		text.append(std::format("top-level {}\n", m_module.Function(m_module.m_top_level.value()).m_name));
	}

	for (const MarmotIRFunction& function : m_module.m_functions)
	{
		text.push_back('\n');
		text.append(PrintFunction(function));
	}
	return text;
}

std::string MarmotIRPrinter::PrintFunction(const MarmotIRFunction& function) const
{
	const std::vector<std::string> names = ValueNames(function);
	std::string text = std::format("fn {}({}) -> {}", function.m_name, MarmotType::JoinToStrings(function.ParameterTypes()), function.m_return_type->ToString());
	if (!function.m_capture_types.empty())
	{
		text.append(std::format(" captures({})", MarmotType::JoinToStrings(function.m_capture_types)));
	}
	text.push_back('\n');

	for (size_t block_index = 0u; block_index < function.m_blocks.size(); block_index += 1u)
	{
		const MarmotIRBlock& block = function.m_blocks[block_index];
		text.append(std::format("bb{}", block_index));
		if (!block.m_parameters.empty())
		{
			const std::string parameters = block.m_parameters
				| std::views::transform([&](MarmotIRValueId parameter) { return std::format("{}: {}", names[parameter.m_index], function.TypeOf(parameter)->ToString()); })
				| std::views::join_with(std::string_view(", "))
				| std::ranges::to<std::string>();
			text.append(std::format("({})", parameters));
		}
		text.append(":\n");

		for (const MarmotIRInstruction& instruction : block.m_instructions)
		{
			text.append(std::format("  {}\n", PrintInstruction(instruction, names)));
		}
	}
	return text;
}

std::vector<std::string> MarmotIRPrinter::ValueNames(const MarmotIRFunction& function)
{
	std::unordered_map<std::string, size_t> uses;
	for (const MarmotIRValue& value : function.m_values)
	{
		uses[value.m_name] += 1u;
	}

	std::vector<std::string> names;
	names.reserve(function.m_values.size());
	for (size_t index = 0u; index < function.m_values.size(); index += 1u)
	{
		const std::string& name = function.m_values[index].m_name;
		if (name.empty())
		{
			names.push_back(std::format("%{}", index));
		}
		else if (uses.at(name) == 1u)
		{
			names.push_back(name);
		}
		else
		{
			names.push_back(std::format("{}.{}", name, index));
		}
	}
	return names;
}

std::string MarmotIRPrinter::PrintImmediate(const MarmotIRImmediate& immediate) const
{
	struct ImmediateVisitor
	{
		const MarmotIRModule& m_module;

		std::string operator()(std::monostate) const { return {}; }
		std::string operator()(int64_t value) const { return std::format("{}", value); }
		std::string operator()(double value) const { return PrintFloat(value); }
		std::string operator()(bool value) const { return value ? "true" : "false"; }
		std::string operator()(const std::string& value) const { return QuoteText(value); }
		std::string operator()(MarmotIRByte value) const { return std::format("{}", value.m_value); }
		std::string operator()(MarmotIRWord value) const { return std::format("{}", value.m_value); }
		std::string operator()(MarmotIRIndex value) const { return std::format("#{}", value.m_value); }
		std::string operator()(MarmotIRTag value) const { return std::format("tag {}", value.m_value); }
		std::string operator()(MarmotIRUnionField value) const { return std::format("tag {} #{}", value.m_tag, value.m_index); }
		std::string operator()(MarmotIRGlobalSlot value) const { return std::format("@{}", value.m_value); }
		std::string operator()(MarmotIRFunctionId value) const { return m_module.Function(value).m_name; }
		std::string operator()(const MarmotIRForeign& value) const { return std::format("foreign {}", QuoteText(value.m_name)); }
	};

	return std::visit(ImmediateVisitor{ m_module }, immediate);
}

std::string MarmotIRPrinter::PrintInstruction(const MarmotIRInstruction& instruction, const std::vector<std::string>& names) const
{
	std::vector<std::string> parts;
	const std::string immediate = PrintImmediate(instruction.m_immediate);
	if (!immediate.empty())
	{
		parts.push_back(immediate);
	}
	if (!instruction.m_operands.empty())
	{
		parts.push_back(JoinValues(instruction.m_operands, names));
	}
	std::ranges::transform(instruction.m_successors, std::back_inserter(parts), [&names](const MarmotIRSuccessor& successor) { return PrintSuccessor(successor, names); });

	const std::string arguments = parts | std::views::join_with(std::string_view(", ")) | std::ranges::to<std::string>();
	if (IsMarmotIRTerminator(instruction.m_op))
	{
		return arguments.empty() ? TerminatorName(instruction.m_op) : std::format("{} {}", TerminatorName(instruction.m_op), arguments);
	}

	std::string text = std::format("{}: {} = {}", names[instruction.m_result.value().m_index], instruction.m_type->ToString(), GetMarmotIROpInfo(instruction.m_op).m_name);
	if (!arguments.empty())
	{
		text.append(std::format(" {}", arguments));
	}
	if (instruction.m_effect.m_kind != MarmotIREffectKind::Pure)
	{
		text.append(std::format("  !{}", MarmotIREffectName(instruction.m_effect.m_kind)));
		if (instruction.m_effect.m_fault != MarmotIRFault::None)
		{
			text.append(std::format("({})", MarmotIRFaultName(instruction.m_effect.m_fault)));
		}
	}
	return text;
}

std::string MarmotIRPrinter::PrintSuccessor(const MarmotIRSuccessor& successor, const std::vector<std::string>& names)
{
	std::string text = std::format("bb{}", successor.m_block.m_index);
	if (!successor.m_arguments.empty())
	{
		text.append(std::format("({})", JoinValues(successor.m_arguments, names)));
	}
	return text;
}
