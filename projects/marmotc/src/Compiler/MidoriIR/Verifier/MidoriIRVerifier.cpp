#include "MidoriIRVerifier.h"
#include "Compiler/Lowering/GenericTypes.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <span>
#include <unordered_map>
#include <utility>

namespace
{
	using TypeRef = std::shared_ptr<MidoriType>;

	bool SameType(const TypeRef& left, const TypeRef& right)
	{
		return MidoriIRSameType(left, right);
	}

	bool IsNever(const TypeRef& type)
	{
		return type->IsType<MidoriType::NeverType>();
	}

	// What a type is at run time: a newtype is its representation.
	TypeRef ShapeOf(const TypeRef& type)
	{
		return GenericTypes::RepresentationOf(type);
	}

	bool SameTypes(std::span<const TypeRef> left, std::span<const TypeRef> right)
	{
		return std::ranges::equal(left, right, SameType);
	}

	using UnionMember = std::pair<const std::string, MidoriType::UnionType::UnionMemberContext>;

	const MidoriType::UnionType::UnionMemberContext* FindMember(const MidoriType::UnionType& union_type, int tag)
	{
		const std::unordered_map<std::string, MidoriType::UnionType::UnionMemberContext>::const_iterator member = std::ranges::find_if(union_type.m_member_info, [tag](const UnionMember& entry) { return entry.second.m_tag == tag; });
		return member == union_type.m_member_info.end() ? nullptr : &member->second;
	}

	struct Definition
	{
		MidoriIRBlockId m_block;
		// A block parameter is defined before the block's first instruction.
		int m_position;
	};

	class FunctionVerifier
	{
	private:
		const MidoriIRModule& m_module;
		const MidoriIRFunction& m_function;
		std::vector<MidoriIRViolation>& m_violations;
		std::vector<std::optional<Definition>> m_definitions;
		std::vector<std::vector<uint32_t>> m_predecessors;
		// The immediate dominator of each block reachable from the entry.
		std::vector<std::optional<uint32_t>> m_dominators;

	public:
		FunctionVerifier(const MidoriIRModule& module, const MidoriIRFunction& function, std::vector<MidoriIRViolation>& violations)
			: m_module(module),
			m_function(function),
			m_violations(violations),
			m_definitions(function.m_values.size()),
			m_predecessors(function.m_blocks.size()),
			m_dominators(function.m_blocks.size())
		{
		}

		void Verify()
		{
			if (m_function.m_blocks.empty())
			{
				Report(MidoriIRRule::Terminator, std::nullopt, "has no blocks");
				return;
			}

			CheckTerminators();
			CollectDefinitions();
			ComputeDominators();
			CheckUses();
			for (uint32_t block = 0u; block < m_function.m_blocks.size(); block += 1u)
			{
				if (!IsReachable(block))
				{
					continue;
				}
				CheckNeverCalls(MidoriIRBlockId{ block });
				for (const MidoriIRInstruction& instruction : m_function.m_blocks[block].m_instructions)
				{
					CheckSuccessors(MidoriIRBlockId{ block }, instruction);
					if (AllValuesExist(instruction))
					{
						CheckNeverUses(MidoriIRBlockId{ block }, instruction);
						CheckInstruction(MidoriIRBlockId{ block }, instruction);
					}
				}
			}
		}

	private:
		void Report(MidoriIRRule rule, std::optional<MidoriIRBlockId> block, std::string message)
		{
			m_violations.emplace_back(rule, m_function.m_name, block, std::move(message));
		}

		bool Expect(bool condition, MidoriIRBlockId block, const MidoriIRInstruction& instruction, std::string_view message)
		{
			if (!condition)
			{
				Report(MidoriIRRule::OperandTypes, block, std::format("{}: {}", GetMidoriIROpInfo(instruction.m_op).m_name, message));
			}
			return condition;
		}

		bool IsValue(MidoriIRValueId value) const
		{
			return value.m_index < m_function.m_values.size();
		}

		bool AllValuesExist(const MidoriIRInstruction& instruction) const
		{
			const bool results_exist = !instruction.m_result.has_value() || IsValue(instruction.m_result.value());
			const bool operands_exist = std::ranges::all_of(instruction.m_operands, [this](MidoriIRValueId value) { return IsValue(value); });
			const bool arguments_exist = std::ranges::all_of(instruction.m_successors, [this](const MidoriIRSuccessor& successor)
			{
				return std::ranges::all_of(successor.m_arguments, [this](MidoriIRValueId value) { return IsValue(value); });
			});
			return results_exist && operands_exist && arguments_exist;
		}

		const TypeRef& TypeOf(MidoriIRValueId value) const
		{
			return m_function.TypeOf(value);
		}

		TypeRef Shape(MidoriIRValueId value) const
		{
			return ShapeOf(TypeOf(value));
		}

		std::vector<TypeRef> TypesOf(std::span<const MidoriIRValueId> values) const
		{
			return values
				| std::views::transform([this](MidoriIRValueId value) { return TypeOf(value); })
				| std::ranges::to<std::vector>();
		}

		std::vector<uint32_t> Successors(uint32_t block) const
		{
			const std::vector<MidoriIRInstruction>& instructions = m_function.m_blocks[block].m_instructions;
			if (instructions.empty())
			{
				return {};
			}
			return instructions.back().m_successors
				| std::views::transform([](const MidoriIRSuccessor& successor) { return successor.m_block.m_index; })
				| std::views::filter([this](uint32_t target) { return target < m_function.m_blocks.size(); })
				| std::ranges::to<std::vector>();
		}

		// Rule 1.
		void CheckTerminators()
		{
			for (uint32_t block = 0u; block < m_function.m_blocks.size(); block += 1u)
			{
				const std::vector<MidoriIRInstruction>& instructions = m_function.m_blocks[block].m_instructions;
				if (instructions.empty() || !IsMidoriIRTerminator(instructions.back().m_op))
				{
					Report(MidoriIRRule::Terminator, MidoriIRBlockId{ block }, "does not end in a terminator");
				}
				for (size_t position = 0u; position + 1u < instructions.size(); position += 1u)
				{
					if (IsMidoriIRTerminator(instructions[position].m_op))
					{
						Report(MidoriIRRule::Terminator, MidoriIRBlockId{ block }, std::format("has {} before its end", GetMidoriIROpInfo(instructions[position].m_op).m_name));
					}
				}
			}
		}

		void Define(MidoriIRValueId value, Definition definition)
		{
			if (!IsValue(value))
			{
				Report(MidoriIRRule::Dominance, definition.m_block, std::format("defines %{}, which is not a value of this function", value.m_index));
				return;
			}
			if (m_definitions[value.m_index].has_value())
			{
				Report(MidoriIRRule::Dominance, definition.m_block, std::format("defines %{} a second time", value.m_index));
				return;
			}
			m_definitions[value.m_index] = definition;
		}

		void CollectDefinitions()
		{
			for (uint32_t block = 0u; block < m_function.m_blocks.size(); block += 1u)
			{
				const MidoriIRBlock& current = m_function.m_blocks[block];
				for (const MidoriIRValueId parameter : current.m_parameters)
				{
					Define(parameter, Definition{ MidoriIRBlockId{ block }, -1 });
				}
				for (size_t position = 0u; position < current.m_instructions.size(); position += 1u)
				{
					const std::optional<MidoriIRValueId>& result = current.m_instructions[position].m_result;
					if (result.has_value())
					{
						Define(result.value(), Definition{ MidoriIRBlockId{ block }, static_cast<int>(position) });
					}
				}
			}
		}

		// Cooper, Harvey and Kennedy, "A Simple, Fast Dominance Algorithm".
		void ComputeDominators()
		{
			std::vector<uint32_t> postorder;
			std::vector<bool> visited(m_function.m_blocks.size(), false);
			std::vector<std::pair<uint32_t, size_t>> stack = { { 0u, 0u } };
			visited[0u] = true;
			while (!stack.empty())
			{
				const uint32_t block = stack.back().first;
				const std::vector<uint32_t> successors = Successors(block);
				if (stack.back().second < successors.size())
				{
					const uint32_t next = successors[stack.back().second];
					stack.back().second += 1u;
					if (!visited[next])
					{
						visited[next] = true;
						stack.emplace_back(next, 0u);
					}
					continue;
				}
				postorder.push_back(block);
				stack.pop_back();
			}

			std::vector<size_t> postorder_number(m_function.m_blocks.size(), 0u);
			for (size_t index = 0u; index < postorder.size(); index += 1u)
			{
				postorder_number[postorder[index]] = index;
			}
			for (const uint32_t block : postorder)
			{
				for (const uint32_t successor : Successors(block))
				{
					m_predecessors[successor].push_back(block);
				}
			}

			const auto intersect = [&](uint32_t left, uint32_t right)
			{
				while (left != right)
				{
					while (postorder_number[left] < postorder_number[right])
					{
						left = m_dominators[left].value();
					}
					while (postorder_number[right] < postorder_number[left])
					{
						right = m_dominators[right].value();
					}
				}
				return left;
			};

			m_dominators[0u] = 0u;
			bool changed = true;
			while (changed)
			{
				changed = false;
				for (const uint32_t block : postorder | std::views::reverse | std::views::drop(1))
				{
					std::optional<uint32_t> dominator = std::nullopt;
					for (const uint32_t predecessor : m_predecessors[block])
					{
						if (!m_dominators[predecessor].has_value())
						{
							continue;
						}
						dominator = dominator.has_value() ? intersect(predecessor, dominator.value()) : predecessor;
					}
					if (dominator != m_dominators[block])
					{
						m_dominators[block] = dominator;
						changed = true;
					}
				}
			}
		}

		bool IsReachable(uint32_t block) const
		{
			return m_dominators[block].has_value();
		}

		bool Dominates(uint32_t dominator, uint32_t block) const
		{
			while (block != dominator)
			{
				if (block == 0u)
				{
					return false;
				}
				block = m_dominators[block].value();
			}
			return true;
		}

		// Rule 2, for the blocks the entry reaches: a use in a block no path
		// runs has nothing to be dominated by.
		void CheckUses()
		{
			for (uint32_t block = 0u; block < m_function.m_blocks.size(); block += 1u)
			{
				if (!IsReachable(block))
				{
					continue;
				}
				const std::vector<MidoriIRInstruction>& instructions = m_function.m_blocks[block].m_instructions;
				for (size_t position = 0u; position < instructions.size(); position += 1u)
				{
					for (const MidoriIRValueId operand : instructions[position].m_operands)
					{
						CheckUse(operand, block, static_cast<int>(position));
					}
					for (const MidoriIRSuccessor& successor : instructions[position].m_successors)
					{
						for (const MidoriIRValueId argument : successor.m_arguments)
						{
							CheckUse(argument, block, static_cast<int>(position));
						}
					}
				}
			}
		}

		void CheckUse(MidoriIRValueId value, uint32_t block, int position)
		{
			if (!IsValue(value))
			{
				Report(MidoriIRRule::Dominance, MidoriIRBlockId{ block }, std::format("uses %{}, which is not a value of this function", value.m_index));
				return;
			}
			const std::optional<Definition>& definition = m_definitions[value.m_index];
			if (!definition.has_value())
			{
				Report(MidoriIRRule::Dominance, MidoriIRBlockId{ block }, std::format("uses %{}, which nothing defines", value.m_index));
				return;
			}

			const uint32_t defining_block = definition->m_block.m_index;
			const bool dominated = defining_block == block
				? definition->m_position < position
				: IsReachable(defining_block) && Dominates(defining_block, block);
			if (!dominated)
			{
				Report(MidoriIRRule::Dominance, MidoriIRBlockId{ block }, std::format("uses %{}, whose definition in bb{} does not dominate the use", value.m_index, defining_block));
			}
		}

		bool IsConstant(MidoriIRValueId value) const
		{
			const std::optional<Definition>& definition = m_definitions[value.m_index];
			return definition.has_value() && definition->m_position >= 0
				&& m_function.m_blocks[definition->m_block.m_index].m_instructions[static_cast<size_t>(definition->m_position)].m_op == MidoriIROp::Const;
		}

		// Rule 1: nothing runs after a call that returns Never.
		void CheckNeverCalls(MidoriIRBlockId block)
		{
			const std::vector<MidoriIRInstruction>& instructions = m_function.Block(block).m_instructions;
			for (size_t position = 0u; position + 1u < instructions.size(); position += 1u)
			{
				const bool returns_never = instructions[position].m_result.has_value() && IsNever(instructions[position].m_type);
				if (returns_never && instructions[position + 1u].m_op != MidoriIROp::Unreachable)
				{
					Report(MidoriIRRule::Terminator, block, std::format("{} returns Never, but {} follows it", GetMidoriIROpInfo(instructions[position].m_op).m_name, GetMidoriIROpInfo(instructions[position + 1u].m_op).m_name));
				}
			}
		}

		// Rule 4: a Never value is never made, so nothing can use one.
		void CheckNeverUses(MidoriIRBlockId block, const MidoriIRInstruction& instruction)
		{
			const bool uses_never = std::ranges::any_of(instruction.m_operands, [this](MidoriIRValueId value) { return IsNever(TypeOf(value)); })
				|| std::ranges::any_of(instruction.m_successors, [this](const MidoriIRSuccessor& successor)
				{
					return std::ranges::any_of(successor.m_arguments, [this](MidoriIRValueId value) { return IsNever(TypeOf(value)); });
				});
			Expect(!uses_never, block, instruction, "uses a Never value");
		}

		// Rule 3.
		void CheckSuccessors(MidoriIRBlockId block, const MidoriIRInstruction& instruction)
		{
			for (const MidoriIRSuccessor& successor : instruction.m_successors)
			{
				if (successor.m_block.m_index >= m_function.m_blocks.size())
				{
					Report(MidoriIRRule::SuccessorArguments, block, std::format("goes to bb{}, which does not exist", successor.m_block.m_index));
					continue;
				}

				const std::vector<MidoriIRValueId>& parameters = m_function.Block(successor.m_block).m_parameters;
				if (successor.m_arguments.size() != parameters.size())
				{
					Report(MidoriIRRule::SuccessorArguments, block, std::format("passes {} arguments to bb{}, which takes {}", successor.m_arguments.size(), successor.m_block.m_index, parameters.size()));
					continue;
				}
				for (size_t index = 0u; index < parameters.size(); index += 1u)
				{
					const MidoriIRValueId argument = successor.m_arguments[index];
					if (!IsValue(argument) || !IsValue(parameters[index]))
					{
						continue;
					}
					if (!SameType(TypeOf(argument), TypeOf(parameters[index])))
					{
						Report(MidoriIRRule::SuccessorArguments, block, std::format("passes {} as parameter {} of bb{}, which is {}", TypeOf(argument)->ToString(), index, successor.m_block.m_index, TypeOf(parameters[index])->ToString()));
					}
				}
			}
		}

		const MidoriIRGlobal* Global(MidoriIRBlockId block, const MidoriIRInstruction& instruction)
		{
			const MidoriIRGlobalSlot* slot = std::get_if<MidoriIRGlobalSlot>(&instruction.m_immediate);
			if (slot == nullptr)
			{
				Expect(false, block, instruction, "needs a global slot");
				return nullptr;
			}
			if (slot->m_value >= m_module.m_globals.size())
			{
				Report(MidoriIRRule::GlobalSlot, block, std::format("{} names global @{}, which was never reserved", GetMidoriIROpInfo(instruction.m_op).m_name, slot->m_value));
				return nullptr;
			}
			return &m_module.m_globals[slot->m_value];
		}

		const MidoriIRFunction* Callee(MidoriIRBlockId block, const MidoriIRInstruction& instruction)
		{
			const MidoriIRFunctionId* function = std::get_if<MidoriIRFunctionId>(&instruction.m_immediate);
			if (!Expect(function != nullptr, block, instruction, "needs a function") || !Expect(function->m_index < m_module.m_functions.size(), block, instruction, std::format("names function {}, which does not exist", function->m_index)))
			{
				return nullptr;
			}
			return &m_module.Function(*function);
		}

		// A tail call of a function that returns Never may end a function of
		// any return type, as a call that returns its own type may.
		void ExpectCall(MidoriIRBlockId block, const MidoriIRInstruction& instruction, std::span<const TypeRef> parameters, const TypeRef& return_type, std::span<const MidoriIRValueId> arguments, const TypeRef& result)
		{
			Expect(SameTypes(TypesOf(arguments), parameters), block, instruction, "arguments do not match the callee's parameters");
			const bool is_tail_call_of_never = instruction.m_op == MidoriIROp::TailCall && IsNever(return_type);
			Expect(is_tail_call_of_never || SameType(result, return_type), block, instruction, std::format("result is {}, but the callee returns {}", result->ToString(), return_type->ToString()));
		}

		void ExpectCallOfType(MidoriIRBlockId block, const MidoriIRInstruction& instruction, const TypeRef& callee, std::span<const MidoriIRValueId> arguments, const TypeRef& result)
		{
			if (Expect(callee->IsType<MidoriType::FunctionType>(), block, instruction, std::format("calls a {}, which is not a function", callee->ToString())))
			{
				const MidoriType::FunctionType& function_type = callee->GetType<MidoriType::FunctionType>();
				ExpectCall(block, instruction, function_type.m_param_types, function_type.m_return_type, arguments, result);
			}
		}

		// Rules 4 and 5.
		void CheckInstruction(MidoriIRBlockId block, const MidoriIRInstruction& instruction)
		{
			const MidoriIROpInfo& info = GetMidoriIROpInfo(instruction.m_op);
			const bool is_terminator = info.m_arity == MidoriIRArity::Terminator;
			if (!Expect(instruction.m_result.has_value() != is_terminator, block, instruction, is_terminator ? "a terminator defines no value" : "defines no value"))
			{
				return;
			}
			if (!is_terminator && !Expect(SameType(instruction.m_type, TypeOf(instruction.m_result.value())), block, instruction, "its type is not its value's type"))
			{
				return;
			}
			Expect(is_terminator || instruction.m_successors.empty(), block, instruction, "only a terminator has successors");

			const std::span<const MidoriIRValueId> operands = instruction.m_operands;
			const TypeRef& result = instruction.m_type;
			const auto operand_count = [&](size_t count)
			{
				return Expect(operands.size() == count, block, instruction, std::format("takes {} operands, not {}", count, operands.size()));
			};

			switch (info.m_arity)
			{
			case MidoriIRArity::Unary:
			case MidoriIRArity::Binary:
			{
				if (operand_count(info.m_arity == MidoriIRArity::Unary ? 1u : 2u))
				{
					const TypeRef& operand_type = MidoriIRScalarType(info.m_operand.value());
					for (const MidoriIRValueId operand : operands)
					{
						Expect(SameType(TypeOf(operand), operand_type), block, instruction, std::format("takes {}, not {}", operand_type->ToString(), TypeOf(operand)->ToString()));
					}
					Expect(SameType(result, MidoriIRScalarType(info.m_result.value())), block, instruction, std::format("gives {}, not {}", MidoriIRScalarType(info.m_result.value())->ToString(), result->ToString()));
				}
				return;
			}
			case MidoriIRArity::Special:
			case MidoriIRArity::Terminator:
				break;
			}

			switch (instruction.m_op)
			{
			case MidoriIROp::Const:
			{
				operand_count(0u);
				const bool matches =
					(result->IsType<MidoriType::IntegerType>() && std::holds_alternative<int64_t>(instruction.m_immediate)) ||
					(result->IsType<MidoriType::FloatType>() && std::holds_alternative<double>(instruction.m_immediate)) ||
					(result->IsType<MidoriType::BoolType>() && std::holds_alternative<bool>(instruction.m_immediate)) ||
					(result->IsType<MidoriType::TextType>() && std::holds_alternative<std::string>(instruction.m_immediate)) ||
					(result->IsType<MidoriType::ByteType>() && std::holds_alternative<MidoriIRByte>(instruction.m_immediate)) ||
					(result->IsType<MidoriType::WordType>() && std::holds_alternative<MidoriIRWord>(instruction.m_immediate)) ||
					(result->IsType<MidoriType::UnitType>() && std::holds_alternative<std::monostate>(instruction.m_immediate));
				Expect(matches, block, instruction, std::format("its constant is not a {}", result->ToString()));
				return;
			}
			case MidoriIROp::ShlByte:
			case MidoriIROp::ShrByte:
			case MidoriIROp::ShlWord:
			case MidoriIROp::ShrWord:
			{
				const bool is_byte = instruction.m_op == MidoriIROp::ShlByte || instruction.m_op == MidoriIROp::ShrByte;
				const TypeRef& shifted = MidoriIRScalarType(is_byte ? MidoriIRScalar::Byte : MidoriIRScalar::Word);
				if (operand_count(2u))
				{
					Expect(SameType(TypeOf(operands[0u]), shifted) && SameType(result, shifted), block, instruction, std::format("shifts a {} to a {}", shifted->ToString(), shifted->ToString()));
					Expect(Shape(operands[1u])->IsType<MidoriType::IntegerType>(), block, instruction, "its shift amount is not an Int");
				}
				return;
			}
			case MidoriIROp::MakeTuple:
			{
				if (Expect(ShapeOf(result)->IsType<MidoriType::TupleType>(), block, instruction, "gives a tuple"))
				{
					Expect(SameTypes(TypesOf(operands), ShapeOf(result)->GetType<MidoriType::TupleType>().m_element_types), block, instruction, "operands do not match the tuple's elements");
				}
				return;
			}
			case MidoriIROp::TupleGet:
			{
				const MidoriIRIndex* index = std::get_if<MidoriIRIndex>(&instruction.m_immediate);
				if (operand_count(1u) && Expect(Shape(operands[0u])->IsType<MidoriType::TupleType>(), block, instruction, "takes a tuple") && Expect(index != nullptr, block, instruction, "needs an index"))
				{
					const std::vector<TypeRef>& elements = Shape(operands[0u])->GetType<MidoriType::TupleType>().m_element_types;
					if (Expect(index->m_value < elements.size(), block, instruction, std::format("index #{} is past the tuple's end", index->m_value)))
					{
						Expect(SameType(result, elements[index->m_value]), block, instruction, "result is not the element's type");
					}
				}
				return;
			}
			case MidoriIROp::MakeArray:
			{
				if (Expect(ShapeOf(result)->IsType<MidoriType::ArrayType>(), block, instruction, "gives an array"))
				{
					const TypeRef& element = ShapeOf(result)->GetType<MidoriType::ArrayType>().m_element_type;
					Expect(std::ranges::all_of(operands, [&](MidoriIRValueId operand) { return SameType(TypeOf(operand), element); }), block, instruction, "an operand is not the array's element type");
				}
				return;
			}
			case MidoriIROp::ArrayGet:
			{
				if (operand_count(2u) && Expect(Shape(operands[0u])->IsType<MidoriType::ArrayType>(), block, instruction, "takes an array"))
				{
					Expect(Shape(operands[1u])->IsType<MidoriType::IntegerType>(), block, instruction, "its index is not an Int");
					Expect(SameType(result, Shape(operands[0u])->GetType<MidoriType::ArrayType>().m_element_type), block, instruction, "result is not the element's type");
				}
				return;
			}
			case MidoriIROp::ArrayLength:
			{
				if (operand_count(1u))
				{
					Expect(Shape(operands[0u])->IsType<MidoriType::ArrayType>(), block, instruction, "takes an array");
					Expect(result->IsType<MidoriType::IntegerType>(), block, instruction, "gives an Int");
				}
				return;
			}
			case MidoriIROp::Construct:
			{
				if (Expect(ShapeOf(result)->IsType<MidoriType::StructType>(), block, instruction, "gives a struct"))
				{
					Expect(SameTypes(TypesOf(operands), ShapeOf(result)->GetType<MidoriType::StructType>().m_member_types), block, instruction, "operands do not match the struct's members");
				}
				return;
			}
			case MidoriIROp::GetMember:
			case MidoriIROp::RecordUpdate:
			{
				const bool is_update = instruction.m_op == MidoriIROp::RecordUpdate;
				const MidoriIRIndex* index = std::get_if<MidoriIRIndex>(&instruction.m_immediate);
				if (operand_count(is_update ? 2u : 1u) && Expect(Shape(operands[0u])->IsType<MidoriType::StructType>(), block, instruction, "takes a struct") && Expect(index != nullptr, block, instruction, "needs a member index"))
				{
					const std::vector<TypeRef>& members = Shape(operands[0u])->GetType<MidoriType::StructType>().m_member_types;
					if (Expect(index->m_value < members.size(), block, instruction, std::format("member #{} is past the struct's end", index->m_value)))
					{
						const TypeRef& member = members[index->m_value];
						Expect(is_update ? SameType(TypeOf(operands[1u]), member) && SameType(result, TypeOf(operands[0u])) : SameType(result, member), block, instruction, "types do not match the member's");
					}
				}
				return;
			}
			case MidoriIROp::MakeRange:
			{
				if (operand_count(3u) && Expect(ShapeOf(result)->IsType<MidoriType::RangeType>(), block, instruction, "gives a range"))
				{
					const TypeRef& element = ShapeOf(result)->GetType<MidoriType::RangeType>().m_element_type;
					Expect(std::ranges::all_of(operands, [&](MidoriIRValueId operand) { return SameType(TypeOf(operand), element); }), block, instruction, "its start, step and end are not the range's element type");
				}
				return;
			}
			case MidoriIROp::RangeStart:
			case MidoriIROp::RangeEnd:
			case MidoriIROp::RangeStep:
			{
				if (operand_count(1u) && Expect(Shape(operands[0u])->IsType<MidoriType::RangeType>(), block, instruction, "takes a range"))
				{
					Expect(SameType(result, Shape(operands[0u])->GetType<MidoriType::RangeType>().m_element_type), block, instruction, "result is not the range's element type");
				}
				return;
			}
			case MidoriIROp::MakeUnion:
			{
				const MidoriIRTag* tag = std::get_if<MidoriIRTag>(&instruction.m_immediate);
				if (Expect(ShapeOf(result)->IsType<MidoriType::UnionType>(), block, instruction, "gives a union") && Expect(tag != nullptr, block, instruction, "needs a tag"))
				{
					const MidoriType::UnionType::UnionMemberContext* member = FindMember(ShapeOf(result)->GetType<MidoriType::UnionType>(), tag->m_value);
					if (Expect(member != nullptr, block, instruction, std::format("tag {} is no member of {}", tag->m_value, result->ToString())))
					{
						Expect(SameTypes(TypesOf(operands), member->m_member_types), block, instruction, "operands do not match the member's fields");
					}
				}
				return;
			}
			case MidoriIROp::GetTag:
			{
				if (operand_count(1u))
				{
					Expect(Shape(operands[0u])->IsType<MidoriType::UnionType>(), block, instruction, "takes a union");
					Expect(result->IsType<MidoriType::IntegerType>(), block, instruction, "gives an Int");
				}
				return;
			}
			case MidoriIROp::UnionField:
			{
				const MidoriIRUnionField* field = std::get_if<MidoriIRUnionField>(&instruction.m_immediate);
				if (operand_count(1u) && Expect(Shape(operands[0u])->IsType<MidoriType::UnionType>(), block, instruction, "takes a union") && Expect(field != nullptr, block, instruction, "needs a tag and field"))
				{
					const MidoriType::UnionType::UnionMemberContext* member = FindMember(Shape(operands[0u])->GetType<MidoriType::UnionType>(), field->m_tag);
					if (Expect(member != nullptr, block, instruction, std::format("tag {} is no member of {}", field->m_tag, TypeOf(operands[0u])->ToString())) && Expect(field->m_index < member->m_member_types.size(), block, instruction, std::format("field #{} is past the member's end", field->m_index)))
					{
						Expect(SameType(result, member->m_member_types[field->m_index]), block, instruction, "result is not the field's type");
					}
				}
				return;
			}
			case MidoriIROp::ArrayAppend:
			{
				if (operand_count(2u) && Expect(Shape(operands[0u])->IsType<MidoriType::ArrayType>(), block, instruction, "takes an array"))
				{
					Expect(SameType(TypeOf(operands[1u]), Shape(operands[0u])->GetType<MidoriType::ArrayType>().m_element_type), block, instruction, "appends a value that is not the array's element type");
					Expect(SameType(result, TypeOf(operands[0u])), block, instruction, "result is not the array");
				}
				return;
			}
			case MidoriIROp::Concat:
			case MidoriIROp::Extend:
			{
				if (operand_count(2u))
				{
					const TypeRef& left = TypeOf(operands[0u]);
					Expect(left->IsType<MidoriType::TextType>() || left->IsType<MidoriType::ArrayType>(), block, instruction, "takes Text or an array");
					Expect(SameType(left, TypeOf(operands[1u])) && SameType(left, result), block, instruction, "operands and result are not one type");
					Expect(instruction.m_op != MidoriIROp::Extend || !IsConstant(operands[0u]), block, instruction, "extends a constant in place, which every load of it shares");
				}
				return;
			}
			case MidoriIROp::Call:
			{
				const MidoriIRFunction* callee = Callee(block, instruction);
				if (callee != nullptr && Expect(callee->m_capture_types.empty(), block, instruction, std::format("calls {} directly, which has captures", callee->m_name)))
				{
					ExpectCall(block, instruction, callee->ParameterTypes(), callee->m_return_type, operands, result);
				}
				return;
			}
			case MidoriIROp::CallGlobal:
			{
				const MidoriIRGlobal* global = Global(block, instruction);
				if (global != nullptr)
				{
					ExpectCallOfType(block, instruction, global->m_type, operands, result);
				}
				return;
			}
			case MidoriIROp::CallForeign:
			{
				const bool is_builtin = std::holds_alternative<MidoriIRForeign>(instruction.m_immediate);
				Expect(is_builtin || (!operands.empty() && Shape(operands[0u])->IsType<MidoriType::TextType>()), block, instruction, "needs a builtin foreign function, or the name of one as its first operand");
				return;
			}
			case MidoriIROp::CallValue:
			{
				if (Expect(!operands.empty(), block, instruction, "needs the closure it calls"))
				{
					ExpectCallOfType(block, instruction, TypeOf(operands[0u]), operands.subspan(1u), result);
				}
				return;
			}
			case MidoriIROp::MakeClosure:
			{
				// The captures it leaves out, at the end, are bound with BindCaptures.
				const MidoriIRFunction* callee = Callee(block, instruction);
				const bool is_prefix = callee != nullptr && operands.size() <= callee->m_capture_types.size() && SameTypes(TypesOf(operands), std::span<const TypeRef>(callee->m_capture_types).first(operands.size()));
				if (callee != nullptr && Expect(is_prefix, block, instruction, std::format("operands do not match the captures of {}", callee->m_name)))
				{
					const TypeRef closure = MidoriType::MakeFunctionType(callee->ParameterTypes(), TypeRef(callee->m_return_type));
					Expect(SameType(result, closure), block, instruction, std::format("gives {}, not {}", closure->ToString(), result->ToString()));
				}
				return;
			}
			case MidoriIROp::GetCapture:
			{
				const MidoriIRIndex* index = std::get_if<MidoriIRIndex>(&instruction.m_immediate);
				if (operand_count(0u) && Expect(index != nullptr, block, instruction, "needs a capture index") && Expect(index->m_value < m_function.m_capture_types.size(), block, instruction, std::format("capture #{} is past the function's captures", index->m_value)))
				{
					Expect(SameType(result, m_function.m_capture_types[index->m_value]), block, instruction, "result is not the capture's type");
				}
				return;
			}
			case MidoriIROp::CellNew:
			{
				if (operand_count(1u))
				{
					Expect(SameType(result, MidoriType::MakeCellType(TypeOf(operands[0u]))), block, instruction, "result is not a Cell of its operand");
				}
				return;
			}
			case MidoriIROp::CellRead:
			{
				if (operand_count(1u) && Expect(Shape(operands[0u])->IsType<MidoriType::CellType>(), block, instruction, "takes a Cell"))
				{
					Expect(SameType(result, Shape(operands[0u])->GetType<MidoriType::CellType>().m_element_type), block, instruction, "result is not the Cell's element type");
				}
				return;
			}
			case MidoriIROp::CellWrite:
			{
				if (operand_count(2u) && Expect(Shape(operands[0u])->IsType<MidoriType::CellType>(), block, instruction, "takes a Cell"))
				{
					Expect(SameType(TypeOf(operands[1u]), Shape(operands[0u])->GetType<MidoriType::CellType>().m_element_type), block, instruction, "writes a value that is not the Cell's element type");
					Expect(SameType(result, TypeOf(operands[1u])), block, instruction, "gives the value it writes");
				}
				return;
			}
			case MidoriIROp::GlobalDefine:
			case MidoriIROp::GlobalSet:
			{
				const MidoriIRGlobal* global = Global(block, instruction);
				if (global != nullptr && global->IsImported())
				{
					Report(MidoriIRRule::GlobalSlot, block, std::format("{} writes global @{}, which {} defines", info.m_name, std::get<MidoriIRGlobalSlot>(instruction.m_immediate).m_value, global->m_module));
				}
				if (global != nullptr && operand_count(1u))
				{
					Expect(SameType(TypeOf(operands[0u]), global->m_type), block, instruction, std::format("stores a {} in {}, a {}", TypeOf(operands[0u])->ToString(), global->m_name, global->m_type->ToString()));
					Expect(result->IsType<MidoriType::UnitType>(), block, instruction, "gives Unit");
				}
				return;
			}
			case MidoriIROp::GlobalGet:
			{
				const MidoriIRGlobal* global = Global(block, instruction);
				if (global != nullptr && operand_count(0u))
				{
					Expect(SameType(result, global->m_type), block, instruction, std::format("reads {} as a {}, not a {}", global->m_name, result->ToString(), global->m_type->ToString()));
				}
				return;
			}
			case MidoriIROp::Jump:
			{
				operand_count(0u);
				Expect(instruction.m_successors.size() == 1u, block, instruction, "has one successor");
				return;
			}
			case MidoriIROp::Branch:
			{
				if (operand_count(1u))
				{
					Expect(Shape(operands[0u])->IsType<MidoriType::BoolType>(), block, instruction, "its condition is not a Bool");
				}
				Expect(instruction.m_successors.size() == 2u, block, instruction, "has two successors");
				return;
			}
			case MidoriIROp::Return:
			{
				if (operand_count(1u))
				{
					Expect(SameType(TypeOf(operands[0u]), m_function.m_return_type), block, instruction, std::format("returns a {} from a function that returns {}", TypeOf(operands[0u])->ToString(), m_function.m_return_type->ToString()));
				}
				Expect(instruction.m_successors.empty(), block, instruction, "has no successors");
				return;
			}
			case MidoriIROp::TailCall:
			{
				CheckTailCall(block, instruction);
				return;
			}
			case MidoriIROp::Unreachable:
			{
				operand_count(0u);
				Expect(instruction.m_successors.empty(), block, instruction, "has no successors");
				return;
			}
			default:
				return;
			}
		}

		void CheckTailCall(MidoriIRBlockId block, const MidoriIRInstruction& instruction)
		{
			Expect(instruction.m_successors.empty(), block, instruction, "has no successors");
			const std::span<const MidoriIRValueId> operands = instruction.m_operands;
			const TypeRef& return_type = m_function.m_return_type;
			if (std::holds_alternative<MidoriIRFunctionId>(instruction.m_immediate))
			{
				const MidoriIRFunction* callee = Callee(block, instruction);
				if (callee != nullptr)
				{
					ExpectCall(block, instruction, callee->ParameterTypes(), callee->m_return_type, operands, return_type);
				}
				return;
			}
			if (std::holds_alternative<MidoriIRGlobalSlot>(instruction.m_immediate))
			{
				const MidoriIRGlobal* global = Global(block, instruction);
				if (global != nullptr)
				{
					ExpectCallOfType(block, instruction, global->m_type, operands, return_type);
				}
				return;
			}
			if (Expect(!operands.empty(), block, instruction, "needs the closure it calls"))
			{
				ExpectCallOfType(block, instruction, TypeOf(operands[0u]), operands.subspan(1u), return_type);
			}
		}
	};
}

MidoriIRViolation::MidoriIRViolation(MidoriIRRule rule, std::string function, std::optional<MidoriIRBlockId> block, std::string message)
	: m_rule(rule),
	m_function(std::move(function)),
	m_block(block),
	m_message(std::move(message))
{
}

std::string MidoriIRViolation::ToString() const
{
	const std::string where = m_block.has_value() ? std::format("{} bb{}", m_function, m_block->m_index) : m_function;
	return std::format("rule {}: {}: {}", static_cast<int>(m_rule), where, m_message);
}

MidoriIRVerifier::MidoriIRVerifier(const MidoriIRModule& module)
	: m_module(module)
{
}

std::vector<MidoriIRViolation> MidoriIRVerifier::Verify() const
{
	std::vector<MidoriIRViolation> violations;
	for (const MidoriIRFunction& function : m_module.m_functions)
	{
		FunctionVerifier(m_module, function, violations).Verify();
	}
	return violations;
}
