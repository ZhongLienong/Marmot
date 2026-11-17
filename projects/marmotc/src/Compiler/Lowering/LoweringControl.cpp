#include "FunctionLowering.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace
{
	using TypeRef = std::shared_ptr<MarmotType>;

	const TypeRef& IntType()
	{
		return MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	}

	const MarmotType::UnionType::UnionMemberContext& UnionMember(const TypeRef& union_type, int tag)
	{
		const MarmotType::UnionType& members = GenericTypes::RepresentationOf(union_type)->GetType<MarmotType::UnionType>();
		return std::ranges::find_if(members.m_member_info, [tag](const std::pair<const std::string, MarmotType::UnionType::UnionMemberContext>& member) { return member.second.m_tag == tag; })->second;
	}

	MarmotIROp EqualityOf(MarmotLiteralKind kind)
	{
		switch (kind)
		{
		case MarmotLiteralKind::Float:
			return MarmotIROp::EqFloat;
		case MarmotLiteralKind::Byte:
			return MarmotIROp::EqByte;
		case MarmotLiteralKind::Word:
			return MarmotIROp::EqWord;
		case MarmotLiteralKind::Text:
			return MarmotIROp::EqText;
		case MarmotLiteralKind::Bool:
			return MarmotIROp::EqBool;
		default:
			return MarmotIROp::EqInt;
		}
	}

	const TypeRef& LiteralType(MarmotLiteralKind kind)
	{
		switch (kind)
		{
		case MarmotLiteralKind::Float:
			return MarmotIRScalarType(MarmotIRScalar::Float);
		case MarmotLiteralKind::Byte:
			return MarmotIRScalarType(MarmotIRScalar::Byte);
		case MarmotLiteralKind::Word:
			return MarmotIRScalarType(MarmotIRScalar::Word);
		case MarmotLiteralKind::Text:
			return MarmotIRScalarType(MarmotIRScalar::Text);
		case MarmotLiteralKind::Bool:
			return MarmotIRScalarType(MarmotIRScalar::Bool);
		default:
			return MarmotIRScalarType(MarmotIRScalar::Int);
		}
	}
}

// A call in tail position runs without a new frame, whatever it calls: the
// language promises it. Tail position reaches through `if` branches, match
// arms and a block's final expression.
Lowering::Emitted Lowering::FunctionLowering::LowerReturn(const MarmotExpression& expression)
{
	const BranchSink tail = [this](const MarmotExpression* branch) -> Emitted
	{
		if (branch == nullptr)
		{
			m_builder.Return(m_builder.ConstUnit());
			return {};
		}
		return LowerReturn(*branch);
	};

	if (expression.IsExpression<MarmotExpression::Group>())
	{
		return LowerReturn(*expression.GetExpression<MarmotExpression::Group>().m_expr_in);
	}
	if (expression.IsExpression<MarmotExpression::IfElse>())
	{
		return LowerIfWith(expression.GetExpression<MarmotExpression::IfElse>(), tail);
	}
	if (expression.IsExpression<MarmotExpression::Match>())
	{
		return LowerMatchWith(expression.GetExpression<MarmotExpression::Match>(), tail);
	}
	if (expression.IsExpression<MarmotExpression::Block>())
	{
		const MarmotExpression::Block& block = expression.GetExpression<MarmotExpression::Block>();
		const Emitted lowered = LowerStatements(block)
			.and_then
			(
				[&]() -> Emitted
				{
					if (!block.m_final_expr.has_value())
					{
						m_builder.AtLine(block.m_right_brace.m_line).Return(m_builder.ConstUnit());
						return {};
					}
					return LowerReturn(*block.m_final_expr.value());
				}
			);
		return lowered;
	}
	if (expression.IsExpression<MarmotExpression::Call>())
	{
		return LowerTailCall(expression.GetExpression<MarmotExpression::Call>());
	}

	return Lower(expression)
		.transform([this](MarmotIRValueId value)
		{
			m_builder.Return(value);
		});
}

// Each branch goes to `sink`, which ends its block. An `if` without an `else`
// is Unit when its condition fails.
Lowering::Emitted Lowering::FunctionLowering::LowerIfWith(const MarmotExpression::IfElse& if_else, const BranchSink& sink)
{
	return Lower(*if_else.m_condition)
		.and_then
		(
			[&](MarmotIRValueId condition) -> Emitted
			{
				const MarmotIRBlockId then_block = NewBlock();
				const MarmotIRBlockId else_block = NewBlock();
				m_builder.AtLine(if_else.m_if_token.m_line);
				Branch(condition, MarmotIRSuccessor(then_block), MarmotIRSuccessor(else_block));

				PositionAt(then_block);
				return sink(if_else.m_true_branch.get())
					.and_then
					(
						[&]()
						{
							PositionAt(else_block);
							return sink(if_else.m_else_branch.get());
						}
					);
			}
		);
}

// The join's parameter has the construct's type. The type checker gives one
// whose first branch never returns Never, whatever the others give, so then it
// takes the type of a value that reaches it.
std::pair<std::shared_ptr<Lowering::FunctionLowering::JoinPoint>, Lowering::FunctionLowering::BranchSink> Lowering::FunctionLowering::Join(const TypeRef& type)
{
	const std::shared_ptr<JoinPoint> join = std::make_shared<JoinPoint>(NewBlock(), type);
	BranchSink sink = [this, join](const MarmotExpression* branch) -> Emitted
	{
		const Lowered value = branch == nullptr ? Lowered(m_builder.ConstUnit()) : Lower(*branch);
		return value.transform
		(
			[this, &join](MarmotIRValueId branch_value)
			{
				if (!IsDead() && join->m_type->IsType<MarmotType::NeverType>())
				{
					join->m_type = m_function.TypeOf(branch_value);
				}
				Jump(join->m_block, { branch_value });
			}
		);
	};
	return { join, std::move(sink) };
}

MarmotIRValueId Lowering::FunctionLowering::EnterJoin(const JoinPoint& join)
{
	const MarmotIRValueId result = m_builder.AddParameter(join.m_block, join.m_type);
	PositionAt(join.m_block);
	return result;
}

Lowering::Lowered Lowering::FunctionLowering::LowerIf(const MarmotExpression::IfElse& if_else)
{
	const auto [join, sink] = Join(Concrete(if_else.m_type_data));
	return LowerIfWith(if_else, sink)
		.transform([this, &join]()
		{
			return EnterJoin(*join);
		});
}

Lowering::Emitted Lowering::FunctionLowering::LowerStatements(const MarmotExpression::Block& block)
{
	for (const std::unique_ptr<MarmotStatement>& statement : block.m_stmts)
	{
		const Emitted lowered = LowerStatement(*statement);
		if (!lowered.has_value())
		{
			return lowered;
		}
	}
	return {};
}

Lowering::Lowered Lowering::FunctionLowering::LowerBlock(const MarmotExpression::Block& block)
{
	const Lowered lowered = LowerStatements(block)
		.and_then
		(
			[&]() -> Lowered
			{
				if (block.m_final_expr.has_value())
				{
					return Lower(*block.m_final_expr.value());
				}
				return m_builder.AtLine(block.m_right_brace.m_line).ConstUnit();
			}
		);
	return lowered;
}

// The cases are tried in order, each a chain of tests that jumps to the next
// case on the first that fails, binding its names as it goes. The type checker
// makes a match exhaustive, so nothing follows the last case; a case that
// cannot fail ends the chain.
Lowering::Emitted Lowering::FunctionLowering::LowerMatchWith(const MarmotExpression::Match& match, const BranchSink& sink)
{
	return Lower(*match.m_arg_expr)
		.and_then
		(
			[&](MarmotIRValueId scrutinee) -> Emitted
			{
				m_builder.AtLine(match.m_match_keyword.m_line);
				for (const std::unique_ptr<MarmotExpression>& case_expression : match.m_cases)
				{
					const MarmotExpression::Case& match_case = case_expression->GetExpression<MarmotExpression::Case>();
					const MarmotIRBlockId fail = NewBlock();
					const Emitted lowered = LowerPattern(*match_case.m_pattern, scrutinee, fail)
						.and_then
						(
							[&]() -> Emitted
							{
								if (!match_case.HasGuard())
								{
									return {};
								}
								return Lower(*match_case.m_guard.value())
									.and_then([&](MarmotIRValueId guard) { return TestOrFail(guard, fail); });
							}
						)
						.and_then([&]() { return sink(match_case.m_expr.get()); });
					if (!lowered.has_value())
					{
						return lowered;
					}

					PositionAt(fail);
					if (IsDead())
					{
						break;
					}
				}
				m_builder.Unreachable();
				return {};
			}
		);
}

Lowering::Lowered Lowering::FunctionLowering::LowerMatch(const MarmotExpression::Match& match)
{
	const auto [join, sink] = Join(Concrete(match.m_type_data));
	return LowerMatchWith(match, sink)
		.transform([this, &join]()
		{
			return EnterJoin(*join);
		});
}

Lowering::Emitted Lowering::FunctionLowering::TestOrFail(MarmotIRValueId condition, MarmotIRBlockId fail)
{
	const MarmotIRBlockId matched = NewBlock();
	Branch(condition, MarmotIRSuccessor(matched), MarmotIRSuccessor(fail));
	PositionAt(matched);
	return {};
}

// Tests `value` against the pattern, going to `fail` when it does not match,
// and defines the names it binds on the way.
Lowering::Emitted Lowering::FunctionLowering::LowerPattern(const MarmotPattern& pattern, MarmotIRValueId value, MarmotIRBlockId fail)
{
	struct PatternLowering
	{
		FunctionLowering& m_self;
		MarmotIRValueId m_value;
		MarmotIRBlockId m_fail;

		TypeRef Representation() const
		{
			return GenericTypes::RepresentationOf(m_self.m_function.TypeOf(m_value));
		}

		Emitted Elements(const std::vector<std::unique_ptr<MarmotPattern>>& elements, const std::function<MarmotIRValueId(uint32_t)>& element) const
		{
			for (uint32_t index = 0u; index < elements.size(); index += 1u)
			{
				const Emitted lowered = m_self.LowerPattern(*elements[index], element(index), m_fail);
				if (!lowered.has_value())
				{
					return lowered;
				}
			}
			return {};
		}

		Emitted operator()(const MarmotPattern::Binding& binding) const
		{
			if (binding.m_binding.has_value())
			{
				m_self.DefineLocal(binding.m_binding.value(), m_value);
			}
			return {};
		}

		Emitted operator()(const MarmotPattern::Wildcard&) const
		{
			return {};
		}

		Emitted operator()(const MarmotPattern::Literal& literal) const
		{
			if (literal.m_kind == MarmotLiteralKind::Unit)
			{
				return {};
			}
			const std::optional<MarmotIRImmediate> constant = m_self.ConstantOf(literal.m_kind, literal.m_token.m_lexeme);
			if (!constant.has_value())
			{
				return std::unexpected(m_self.m_owner.Error(CompilerErrorCode::None, "Invalid literal in pattern '" + literal.m_token.m_lexeme + "'", literal.m_token));
			}
			m_self.m_builder.AtLine(literal.m_token.m_line);
			const MarmotIRValueId expected = m_self.m_builder.Emit(MarmotIROp::Const, LiteralType(literal.m_kind), {}, constant.value());
			return m_self.TestOrFail(m_self.m_builder.Binary(EqualityOf(literal.m_kind), m_value, expected), m_fail);
		}

		Emitted operator()(const MarmotPattern::Tuple& tuple) const
		{
			const std::vector<TypeRef> element_types = Representation()->GetType<MarmotType::TupleType>().m_element_types;
			m_self.m_builder.AtLine(tuple.m_left_paren.m_line);
			return Elements(tuple.m_elements, [&](uint32_t index)
			{
				return m_self.m_builder.Emit(MarmotIROp::TupleGet, element_types[index], { m_value }, MarmotIRIndex{ index });
			});
		}

		// With a rest, the elements after it are counted from the end, and the rest
		// is the slice between the two ends.
		Emitted operator()(const MarmotPattern::Array& array) const
		{
			const TypeRef array_type = Representation();
			const TypeRef element_type = array_type->GetType<MarmotType::ArrayType>().m_element_type;
			const int64_t count = static_cast<int64_t>(array.m_elements.size());
			const int64_t before = array.m_rest.has_value() ? static_cast<int64_t>(array.m_rest->m_position) : count;
			m_self.m_builder.AtLine(array.m_left_bracket.m_line);
			const MarmotIRValueId length = m_self.m_builder.Emit(MarmotIROp::ArrayLength, IntType(), { m_value });
			const MarmotIROp comparison = array.m_rest.has_value() ? MarmotIROp::GeInt : MarmotIROp::EqInt;
			const auto from_end = [&](int64_t distance)
				{
					return m_self.m_builder.Binary(MarmotIROp::SubInt, length, m_self.m_builder.ConstInt(distance));
				};
			return m_self.TestOrFail(m_self.m_builder.Binary(comparison, length, m_self.m_builder.ConstInt(count)), m_fail)
				.and_then
				(
					[&]()
					{
						return Elements(array.m_elements, [&](uint32_t index)
						{
							const MarmotIRValueId position = index < before ? m_self.m_builder.ConstInt(index) : from_end(count - index);
							return m_self.m_builder.Emit(MarmotIROp::ArrayGet, element_type, { m_value, position });
						});
					}
				)
				.and_then
				(
					[&]() -> Emitted
					{
						if (!array.m_rest.has_value() || !array.m_rest->m_pattern->IsPattern<MarmotPattern::Binding>())
						{
							return {};
						}
						const MarmotIRValueId rest = m_self.m_builder.Emit(MarmotIROp::CallForeign, array_type, { m_value, m_self.m_builder.ConstInt(before), from_end(count - before) }, MarmotIRForeign{ "MARMOT_FFI_ArraySlice" });
						return m_self.LowerPattern(*array.m_rest->m_pattern, rest, m_fail);
					}
				);
		}

		Emitted operator()(const MarmotPattern::Record& record) const
		{
			m_self.m_builder.AtLine(record.m_name_token.m_line);
			const std::vector<TypeRef> member_types = Representation()->GetType<MarmotType::StructType>().m_member_types;
			for (const MarmotPattern::Record::Field& field : record.m_fields)
			{
				const uint32_t index = static_cast<uint32_t>(field.m_index);
				const MarmotIRValueId member = m_self.m_builder.Emit(MarmotIROp::GetMember, member_types[index], { m_value }, MarmotIRIndex{ index });
				const Emitted lowered = m_self.LowerPattern(*field.m_pattern, member, m_fail);
				if (!lowered.has_value())
				{
					return lowered;
				}
			}
			return {};
		}

		Emitted operator()(const MarmotPattern::Range& range) const
		{
			m_self.m_builder.AtLine(range.m_start.m_line);
			return m_self.TestOrFail(m_self.m_builder.Binary(MarmotIROp::GeInt, m_value, m_self.m_builder.ConstInt(range.m_low)), m_fail)
				.and_then([&]() { return m_self.TestOrFail(m_self.m_builder.Binary(MarmotIROp::LeInt, m_value, m_self.m_builder.ConstInt(range.m_high)), m_fail); });
		}

		// Each alternative that matches jumps to one block with what it bound, so
		// the case reads its names from that block's parameters.
		Emitted operator()(const MarmotPattern::Or& either) const
		{
			const std::vector<BindingId> locals = either.m_alternatives.front()->Bindings()
				| std::views::filter([](const MarmotPattern::Binding* binding) { return binding->m_binding.has_value(); })
				| std::views::transform([](const MarmotPattern::Binding* binding) { return binding->m_binding.value(); })
				| std::ranges::to<std::vector>();
			const MarmotIRBlockId matched = m_self.NewBlock();
			std::vector<MarmotIRValueId> parameters;
			for (size_t index = 0uz; index < either.m_alternatives.size(); index += 1uz)
			{
				const bool is_last = index + 1uz == either.m_alternatives.size();
				const MarmotIRBlockId next = is_last ? m_fail : m_self.NewBlock();
				const Emitted lowered = m_self.LowerPattern(*either.m_alternatives[index], m_value, next);
				if (!lowered.has_value())
				{
					return lowered;
				}

				const std::vector<MarmotIRValueId> values = locals
					| std::views::transform([this](const BindingId& local) { return m_self.m_locals.at(local); })
					| std::ranges::to<std::vector>();
				if (index == 0uz)
				{
					parameters = values
						| std::views::transform([this, matched](MarmotIRValueId value) { return m_self.m_builder.AddParameter(matched, m_self.m_function.TypeOf(value)); })
						| std::ranges::to<std::vector>();
				}
				m_self.Jump(matched, values);
				if (!is_last)
				{
					m_self.PositionAt(next);
				}
			}

			m_self.PositionAt(matched);
			for (size_t index = 0uz; index < locals.size(); index += 1uz)
			{
				m_self.DefineLocal(locals[index], parameters[index]);
			}
			return {};
		}

		Emitted operator()(const MarmotPattern::As& as) const
		{
			return m_self.LowerPattern(*as.m_pattern, m_value, m_fail)
				.and_then([&]() { return m_self.LowerPattern(*as.m_binding, m_value, m_fail); });
		}

		Emitted operator()(const MarmotPattern::Constructor& constructor) const
		{
			m_self.m_builder.AtLine(constructor.m_name_token.m_line);
			const TypeRef type = Representation();
			if (!constructor.m_is_union)
			{
				const std::vector<TypeRef> member_types = type->GetType<MarmotType::StructType>().m_member_types;
				return Elements(constructor.m_args, [&](uint32_t index)
				{
					return m_self.m_builder.Emit(MarmotIROp::GetMember, member_types[index], { m_value }, MarmotIRIndex{ index });
				});
			}

			const int tag = constructor.m_tag;
			const std::vector<TypeRef> field_types = UnionMember(type, tag).m_member_types;
			const MarmotIRValueId actual = m_self.m_builder.Emit(MarmotIROp::GetTag, IntType(), { m_value });
			const MarmotIRValueId expected = m_self.m_builder.ConstInt(tag);
			return m_self.TestOrFail(m_self.m_builder.Binary(MarmotIROp::EqInt, actual, expected), m_fail)
				.and_then
				(
					[&]()
					{
						return Elements(constructor.m_args, [&](uint32_t index)
						{
							return m_self.m_builder.Emit(MarmotIROp::UnionField, field_types[index], { m_value }, MarmotIRUnionField{ tag, index });
						});
					}
				);
		}
	};

	return std::visit(PatternLowering{ *this, value, fail }, *pattern);
}

// The loop variable is a value of the loop's body, so a closure made there
// captures that iteration's. The header's parameter is where the loop is: the
// element for a range, the index for an array, the iterator for an Iterable.
// `carried` goes round the loop beside it, and out of the loop when it ends.
std::expected<std::vector<MarmotIRValueId>, CompilerError> Lowering::FunctionLowering::LowerLoop(const MarmotExpression& iterated, const LoopKind& kind, const BindingId& loop_variable, std::vector<MarmotIRValueId> carried, const LoopBody& body, const Token& at)
{
	const Lowered lowered_source = Lower(iterated);
	if (!lowered_source.has_value())
	{
		return std::unexpected(lowered_source.error());
	}
	const MarmotIRValueId source = lowered_source.value();
	if (IsDead())
	{
		return carried;
	}

	const std::vector<TypeRef> carried_types = TypesOf(carried);
	const TypeRef source_type = kind.m_is_iterable ? NominalType(iterated, source) : GenericTypes::RepresentationOf(m_function.TypeOf(source));
	m_builder.AtLine(at.m_line);

	TypeRef position_type = source_type;
	MarmotIRValueId start = source;
	MarmotIRValueId step = source;
	MarmotIRValueId end = source;
	const bool is_range = !kind.m_is_array && !kind.m_is_iterable;
	bool is_float = false;
	if (kind.m_is_array)
	{
		position_type = IntType();
		start = m_builder.ConstInt(0);
		end = m_builder.Emit(MarmotIROp::ArrayLength, IntType(), { source });
	}
	else if (is_range)
	{
		position_type = source_type->GetType<MarmotType::RangeType>().m_element_type;
		is_float = position_type->IsType<MarmotType::FloatType>();
		start = m_builder.Emit(MarmotIROp::RangeStart, position_type, { source });
		step = m_builder.Emit(MarmotIROp::RangeStep, position_type, { source });
		end = m_builder.Emit(MarmotIROp::RangeEnd, position_type, { source });
	}

	const MarmotIRBlockId header = NewBlock();
	const MarmotIRValueId position = m_builder.AddParameter(header, position_type);
	const std::vector<MarmotIRValueId> header_carried = carried_types
		| std::views::transform([&](const TypeRef& type) { return m_builder.AddParameter(header, type); })
		| std::ranges::to<std::vector>();
	const MarmotIRBlockId body_block = NewBlock();
	const MarmotIRBlockId exit = NewBlock();
	const std::vector<MarmotIRValueId> exit_carried = carried_types
		| std::views::transform([&](const TypeRef& type) { return m_builder.AddParameter(exit, type); })
		| std::ranges::to<std::vector>();

	std::vector<MarmotIRValueId> entry_arguments = { start };
	entry_arguments.insert(entry_arguments.end(), carried.begin(), carried.end());
	Jump(header, std::move(entry_arguments));
	PositionAt(header);

	MarmotIRValueId element = position;
	std::optional<MarmotIRValueId> next_iterator;
	if (kind.m_is_array)
	{
		Branch(m_builder.Binary(MarmotIROp::LtInt, position, end), MarmotIRSuccessor(body_block), MarmotIRSuccessor(exit, header_carried));
		PositionAt(body_block);
		element = m_builder.Emit(MarmotIROp::ArrayGet, source_type->GetType<MarmotType::ArrayType>().m_element_type, { source, position });
	}
	else if (is_range)
	{
		// A positive step counts up to the end, any other down to it.
		const MarmotIRValueId zero = is_float ? m_builder.ConstFloat(0.0) : m_builder.ConstInt(0);
		const MarmotIRBlockId upward = NewBlock();
		const MarmotIRBlockId downward = NewBlock();
		Branch(m_builder.Binary(is_float ? MarmotIROp::GtFloat : MarmotIROp::GtInt, step, zero), MarmotIRSuccessor(upward), MarmotIRSuccessor(downward));
		PositionAt(upward);
		Branch(m_builder.Binary(is_float ? MarmotIROp::LtFloat : MarmotIROp::LtInt, position, end), MarmotIRSuccessor(body_block), MarmotIRSuccessor(exit, header_carried));
		PositionAt(downward);
		Branch(m_builder.Binary(is_float ? MarmotIROp::GtFloat : MarmotIROp::GtInt, position, end), MarmotIRSuccessor(body_block), MarmotIRSuccessor(exit, header_carried));
		PositionAt(body_block);
	}
	else
	{
		// Next gives Some((item, next iterator)) while there is one.
		const Lowered next = CallIterableNext(position, kind.m_item_type, kind.m_next_type, at);
		if (!next.has_value())
		{
			return std::unexpected(next.error());
		}
		const TypeRef next_type = m_function.TypeOf(next.value());
		const MarmotIRValueId tag = m_builder.AtLine(at.m_line).Emit(MarmotIROp::GetTag, IntType(), { next.value() });
		Branch(m_builder.Binary(MarmotIROp::EqInt, tag, m_builder.ConstInt(kind.m_some_tag)), MarmotIRSuccessor(body_block), MarmotIRSuccessor(exit, header_carried));
		PositionAt(body_block);
		const TypeRef payload_type = UnionMember(next_type, kind.m_some_tag).m_member_types.front();
		const std::vector<TypeRef>& pair_types = GenericTypes::RepresentationOf(payload_type)->GetType<MarmotType::TupleType>().m_element_types;
		const MarmotIRValueId payload = m_builder.Emit(MarmotIROp::UnionField, payload_type, { next.value() }, MarmotIRUnionField{ kind.m_some_tag, 0u });
		element = m_builder.Emit(MarmotIROp::TupleGet, pair_types[0u], { payload }, MarmotIRIndex{ 0u });
		next_iterator = m_builder.Emit(MarmotIROp::TupleGet, pair_types[1u], { payload }, MarmotIRIndex{ 1u });
	}

	DefineLocal(loop_variable, element);
	std::expected<std::vector<MarmotIRValueId>, CompilerError> updated = body(header_carried);
	if (!updated.has_value())
	{
		return updated;
	}

	m_builder.AtLine(at.m_line);
	const MarmotIRValueId advanced = next_iterator.has_value()
		? next_iterator.value()
		: m_builder.Binary(kind.m_is_array ? MarmotIROp::AddInt : is_float ? MarmotIROp::AddFloat : MarmotIROp::AddInt, position, kind.m_is_array ? m_builder.ConstInt(1) : step);
	std::vector<MarmotIRValueId> back_arguments = { advanced };
	back_arguments.insert(back_arguments.end(), updated.value().begin(), updated.value().end());
	Jump(header, std::move(back_arguments));
	PositionAt(exit);
	return exit_carried;
}

Lowering::Lowered Lowering::FunctionLowering::LowerFor(const MarmotExpression::For& for_expression)
{
	const LoopKind kind = LoopKindOf(for_expression.m_is_array_iteration, for_expression.m_is_iterable_iteration, for_expression.m_iterable_item_type, for_expression.m_iterable_next_type, for_expression.m_iterable_some_tag);
	return LowerLoop(*for_expression.m_range, kind, for_expression.m_loop_binding, {}, [&](std::vector<MarmotIRValueId> carried) -> std::expected<std::vector<MarmotIRValueId>, CompilerError>
		{
			return Lower(*for_expression.m_body).transform([&carried](MarmotIRValueId) { return carried; });
		}, for_expression.m_for_keyword)
		.transform([this](std::vector<MarmotIRValueId>)
		{
			return m_builder.ConstUnit();
		});
}

// The result grows by one element each iteration; nothing else holds it, so
// it grows in place.
Lowering::Lowered Lowering::FunctionLowering::LowerComprehension(const MarmotExpression::ArrayComprehension& comprehension)
{
	const LoopKind kind = LoopKindOf(comprehension.m_is_array_iteration, comprehension.m_is_iterable_iteration, comprehension.m_iterable_item_type, comprehension.m_iterable_next_type, comprehension.m_iterable_some_tag);
	const TypeRef type = Concrete(comprehension.m_type_data);
	const MarmotIRValueId empty = m_builder.AtLine(comprehension.m_bracket.m_line).Emit(MarmotIROp::MakeArray, type);
	return LowerLoop
	(
		*comprehension.m_range,
		kind,
		comprehension.m_loop_binding,
		{ empty },
		[&](std::vector<MarmotIRValueId> carried) -> std::expected<std::vector<MarmotIRValueId>, CompilerError>
		{
			return Lower(*comprehension.m_transform_expr)
				.transform([&](MarmotIRValueId element)
				{
					return std::vector<MarmotIRValueId>{ m_builder.AtLine(comprehension.m_bracket.m_line).Emit(MarmotIROp::ArrayAppend, type, { carried.front(), element }) };
				});
		},
		comprehension.m_bracket
	)
		.transform([](std::vector<MarmotIRValueId> result)
		{
			return result.front();
		});
}

Lowering::FunctionLowering::LoopKind Lowering::FunctionLowering::LoopKindOf(bool is_array, bool is_iterable, const TypeRef& item_type, const TypeRef& next_type, int some_tag)
{
	return LoopKind{ is_array, is_iterable, is_iterable ? Concrete(item_type) : nullptr, is_iterable ? Concrete(next_type) : nullptr, some_tag };
}

Lowering::Lowered Lowering::FunctionLowering::CallIterableNext(MarmotIRValueId iterator, const TypeRef& item_type, const TypeRef& next_type, const Token& at)
{
	const TypeRef iterator_type = m_function.TypeOf(iterator);
	const MethodResolution<ResolvedInstanceName> resolved = m_owner.m_resolver.ResolveIterableNext(m_specialization.m_methods, iterator_type, item_type);
	if (!resolved.has_value())
	{
		return std::unexpected(m_owner.ResolutionError(resolved.error(), at));
	}
	return CallResolved(resolved.value(), { iterator }, { iterator_type }, next_type, at);
}
