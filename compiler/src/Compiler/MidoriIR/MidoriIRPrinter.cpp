#include "MidoriIRPrinter.h"
#include "Common/Constant/Constant.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <iterator>
#include <ranges>
#include <string_view>
#include <unordered_map>

namespace
{
	std::string JoinTypes(const std::vector<std::shared_ptr<MidoriType>>& types)
	{
		return types
			| std::views::transform([](const std::shared_ptr<MidoriType>& type) { return type->ToString(); })
			| std::views::join_with(std::string_view(", "))
			| std::ranges::to<std::string>();
	}

	std::string JoinValues(const std::vector<MidoriIRValueId>& values, const std::vector<std::string>& names)
	{
		return values
			| std::views::transform([&names](MidoriIRValueId value) { return names[value.m_index]; })
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

	std::string TerminatorName(MidoriIROp op)
	{
		return GetMidoriIROpInfo(op).m_name
			| std::views::transform([](char character) { return static_cast<char>(std::tolower(static_cast<unsigned char>(character))); })
			| std::ranges::to<std::string>();
	}
}

MidoriIRPrinter::MidoriIRPrinter(const MidoriIRModule& module)
	: m_module(module)
{
}

std::string MidoriIRPrinter::Print() const
{
	std::string text = std::format("module {}\n", m_module.m_name);
	for (size_t slot = 0u; slot < m_module.m_globals.size(); slot += 1u)
	{
		const MidoriIRGlobal& global = m_module.m_globals[slot];
		const std::string name = global.IsImported() ? std::format("{}{}{}", global.m_module, NameSeparator, global.m_name) : global.m_name;
		text.append(std::format("global @{} {}: {}\n", slot, name, global.m_type->ToString()));
	}
	if (m_module.m_top_level.has_value())
	{
		text.append(std::format("top-level {}\n", m_module.Function(m_module.m_top_level.value()).m_name));
	}

	for (const MidoriIRFunction& function : m_module.m_functions)
	{
		text.push_back('\n');
		text.append(PrintFunction(function));
	}
	return text;
}

std::string MidoriIRPrinter::PrintFunction(const MidoriIRFunction& function) const
{
	const std::vector<std::string> names = ValueNames(function);
	std::string text = std::format("fn {}({}) -> {}", function.m_name, JoinTypes(function.ParameterTypes()), function.m_return_type->ToString());
	if (!function.m_capture_types.empty())
	{
		text.append(std::format(" captures({})", JoinTypes(function.m_capture_types)));
	}
	text.push_back('\n');

	for (size_t block_index = 0u; block_index < function.m_blocks.size(); block_index += 1u)
	{
		const MidoriIRBlock& block = function.m_blocks[block_index];
		text.append(std::format("bb{}", block_index));
		if (!block.m_parameters.empty())
		{
			const std::string parameters = block.m_parameters
				| std::views::transform([&](MidoriIRValueId parameter) { return std::format("{}: {}", names[parameter.m_index], function.TypeOf(parameter)->ToString()); })
				| std::views::join_with(std::string_view(", "))
				| std::ranges::to<std::string>();
			text.append(std::format("({})", parameters));
		}
		text.append(":\n");

		for (const MidoriIRInstruction& instruction : block.m_instructions)
		{
			text.append(std::format("  {}\n", PrintInstruction(instruction, names)));
		}
	}
	return text;
}

std::vector<std::string> MidoriIRPrinter::ValueNames(const MidoriIRFunction& function)
{
	std::unordered_map<std::string, size_t> uses;
	for (const MidoriIRValue& value : function.m_values)
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

std::string MidoriIRPrinter::PrintImmediate(const MidoriIRImmediate& immediate) const
{
	struct ImmediateVisitor
	{
		const MidoriIRModule& m_module;

		std::string operator()(std::monostate) const { return {}; }
		std::string operator()(int64_t value) const { return std::format("{}", value); }
		std::string operator()(double value) const { return PrintFloat(value); }
		std::string operator()(bool value) const { return value ? "true" : "false"; }
		std::string operator()(const std::string& value) const { return QuoteText(value); }
		std::string operator()(MidoriIRByte value) const { return std::format("{}", value.m_value); }
		std::string operator()(MidoriIRWord value) const { return std::format("{}", value.m_value); }
		std::string operator()(MidoriIRIndex value) const { return std::format("#{}", value.m_value); }
		std::string operator()(MidoriIRTag value) const { return std::format("tag {}", value.m_value); }
		std::string operator()(MidoriIRUnionField value) const { return std::format("tag {} #{}", value.m_tag, value.m_index); }
		std::string operator()(MidoriIRGlobalSlot value) const { return std::format("@{}", value.m_value); }
		std::string operator()(MidoriIRFunctionId value) const { return m_module.Function(value).m_name; }
		std::string operator()(const MidoriIRForeign& value) const { return std::format("foreign {}", QuoteText(value.m_name)); }
	};

	return std::visit(ImmediateVisitor{ m_module }, immediate);
}

std::string MidoriIRPrinter::PrintInstruction(const MidoriIRInstruction& instruction, const std::vector<std::string>& names) const
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
	std::ranges::transform(instruction.m_successors, std::back_inserter(parts), [&](const MidoriIRSuccessor& successor)
	{
		const bool is_default = instruction.m_op == MidoriIROp::Switch && !successor.m_case_tag.has_value();
		return is_default ? std::format("default: {}", PrintSuccessor(successor, names)) : PrintSuccessor(successor, names);
	});

	const std::string arguments = parts | std::views::join_with(std::string_view(", ")) | std::ranges::to<std::string>();
	if (IsMidoriIRTerminator(instruction.m_op))
	{
		return arguments.empty() ? TerminatorName(instruction.m_op) : std::format("{} {}", TerminatorName(instruction.m_op), arguments);
	}

	std::string text = std::format("{}: {} = {}", names[instruction.m_result.value().m_index], instruction.m_type->ToString(), GetMidoriIROpInfo(instruction.m_op).m_name);
	if (!arguments.empty())
	{
		text.append(std::format(" {}", arguments));
	}
	if (instruction.m_effect.m_kind != MidoriIREffectKind::Pure)
	{
		text.append(std::format("  !{}", MidoriIREffectName(instruction.m_effect.m_kind)));
		if (instruction.m_effect.m_fault != MidoriIRFault::None)
		{
			text.append(std::format("({})", MidoriIRFaultName(instruction.m_effect.m_fault)));
		}
	}
	return text;
}

std::string MidoriIRPrinter::PrintSuccessor(const MidoriIRSuccessor& successor, const std::vector<std::string>& names)
{
	std::string text = successor.m_case_tag.has_value() ? std::format("{}: bb{}", successor.m_case_tag.value(), successor.m_block.m_index) : std::format("bb{}", successor.m_block.m_index);
	if (!successor.m_arguments.empty())
	{
		text.append(std::format("({})", JoinValues(successor.m_arguments, names)));
	}
	return text;
}
