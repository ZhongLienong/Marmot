#include "Lowering.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace
{
	using TypeRef = std::shared_ptr<MidoriType>;

	const TypeRef& IntType()
	{
		return MidoriType::MakeLiteralType<MidoriType::IntegerType>();
	}

	const MidoriType::UnionType::UnionMemberContext& UnionMember(const TypeRef& union_type, int tag)
	{
		const MidoriType::UnionType& members = GenericTypes::RepresentationOf(union_type)->GetType<MidoriType::UnionType>();
		return std::ranges::find_if(members.m_member_info, [tag](const std::pair<const std::string, MidoriType::UnionType::UnionMemberContext>& member) { return member.second.m_tag == tag; })->second;
	}

	MidoriIROp EqualityOf(MidoriLiteralKind kind)
	{
		switch (kind)
		{
		case MidoriLiteralKind::Float:
			return MidoriIROp::EqFloat;
		case MidoriLiteralKind::Byte:
			return MidoriIROp::EqByte;
		case MidoriLiteralKind::Word:
			return MidoriIROp::EqWord;
		case MidoriLiteralKind::Text:
			return MidoriIROp::EqText;
		case MidoriLiteralKind::Bool:
			return MidoriIROp::EqBool;
		default:
			return MidoriIROp::EqInt;
		}
	}

	const TypeRef& LiteralType(MidoriLiteralKind kind)
	{
		switch (kind)
		{
		case MidoriLiteralKind::Float:
			return MidoriIRScalarType(MidoriIRScalar::Float);
		case MidoriLiteralKind::Byte:
			return MidoriIRScalarType(MidoriIRScalar::Byte);
		case MidoriLiteralKind::Word:
			return MidoriIRScalarType(MidoriIRScalar::Word);
		case MidoriLiteralKind::Text:
			return MidoriIRScalarType(MidoriIRScalar::Text);
		case MidoriLiteralKind::Bool:
			return MidoriIRScalarType(MidoriIRScalar::Bool);
		default:
			return MidoriIRScalarType(MidoriIRScalar::Int);
		}
	}
}

// A call in tail position runs without a new frame, whatever it calls: the
// language promises it. Tail position reaches through `if` branches, match
// arms and a block's final expression.
Lowering::Emitted Lowering::LowerReturn(MidoriExpression& expression)
{
	const BranchSink tail = [this](MidoriExpression* branch) -> Emitted
	{
		if (branch == nullptr)
		{
			Builder().Return(Builder().ConstUnit());
			return {};
		}
		return LowerReturn(*branch);
	};

	if (expression.IsExpression<MidoriExpression::Group>())
	{
		return LowerReturn(*expression.GetExpression<MidoriExpression::Group>().m_expr_in);
	}
	if (expression.IsExpression<MidoriExpression::IfElse>())
	{
		return LowerIfWith(expression.GetExpression<MidoriExpression::IfElse>(), tail);
	}
	if (expression.IsExpression<MidoriExpression::Match>())
	{
		return LowerMatchWith(expression.GetExpression<MidoriExpression::Match>(), tail);
	}
	if (expression.IsExpression<MidoriExpression::Block>())
	{
		MidoriExpression::Block& block = expression.GetExpression<MidoriExpression::Block>();
		PushLocalFrame();
		const Emitted lowered = LowerStatements(block)
			.and_then([&]() -> Emitted
			{
				if (!block.m_final_expr.has_value())
				{
					Builder().AtLine(block.m_right_brace.m_line).Return(Builder().ConstUnit());
					return {};
				}
				return LowerReturn(*block.m_final_expr.value());
			});
		PopLocalFrame();
		return lowered;
	}
	if (expression.IsExpression<MidoriExpression::Call>())
	{
		return LowerTailCall(expression.GetExpression<MidoriExpression::Call>());
	}

	return Lower(expression)
		.transform([this](MidoriIRValueId value)
		{
			Builder().Return(value);
		});
}

// Each branch goes to `sink`, which ends its block. An `if` without an `else`
// is Unit when its condition fails.
Lowering::Emitted Lowering::LowerIfWith(MidoriExpression::IfElse& if_else, const BranchSink& sink)
{
	return Lower(*if_else.m_condition)
		.and_then([&](MidoriIRValueId condition) -> Emitted
		{
			const MidoriIRBlockId then_block = NewBlock();
			const MidoriIRBlockId else_block = NewBlock();
			Builder().AtLine(if_else.m_if_token.m_line);
			Branch(condition, MidoriIRSuccessor(then_block), MidoriIRSuccessor(else_block));

			PositionAt(then_block);
			return sink(if_else.m_true_branch.get())
				.and_then([&]()
				{
					PositionAt(else_block);
					return sink(if_else.m_else_branch.get());
				});
		});
}

// The join's parameter has the construct's type. The type checker gives one
// whose first branch never returns Never, whatever the others give, so then it
// takes the type of a value that reaches it.
std::pair<std::shared_ptr<Lowering::JoinPoint>, Lowering::BranchSink> Lowering::Join(const TypeRef& type)
{
	const std::shared_ptr<JoinPoint> join = std::make_shared<JoinPoint>(NewBlock(), type);
	BranchSink sink = [this, join](MidoriExpression* branch) -> Emitted
	{
		const Lowered value = branch == nullptr ? Lowered(Builder().ConstUnit()) : Lower(*branch);
		return value.transform([this, &join](MidoriIRValueId branch_value)
		{
			if (!IsDead() && join->m_type->IsType<MidoriType::NeverType>())
			{
				join->m_type = Scope().m_function.TypeOf(branch_value);
			}
			Jump(join->m_block, { branch_value });
		});
	};
	return { join, std::move(sink) };
}

MidoriIRValueId Lowering::EnterJoin(const JoinPoint& join)
{
	const MidoriIRValueId result = Builder().AddParameter(join.m_block, join.m_type);
	PositionAt(join.m_block);
	return result;
}

Lowering::Lowered Lowering::LowerIf(MidoriExpression::IfElse& if_else)
{
	const auto [join, sink] = Join(Concrete(if_else.m_type_data));
	return LowerIfWith(if_else, sink)
		.transform([this, &join]()
		{
			return EnterJoin(*join);
		});
}

Lowering::Emitted Lowering::LowerStatements(MidoriExpression::Block& block)
{
	for (std::unique_ptr<MidoriStatement>& statement : block.m_stmts)
	{
		const Emitted lowered = LowerStatement(*statement);
		if (!lowered.has_value())
		{
			return lowered;
		}
	}
	return {};
}

Lowering::Lowered Lowering::LowerBlock(MidoriExpression::Block& block)
{
	PushLocalFrame();
	const Lowered lowered = LowerStatements(block)
		.and_then([&]() -> Lowered
		{
			if (block.m_final_expr.has_value())
			{
				return Lower(*block.m_final_expr.value());
			}
			return Builder().AtLine(block.m_right_brace.m_line).ConstUnit();
		});
	PopLocalFrame();
	return lowered;
}

// The cases are tried in order, each a chain of tests that jumps to the next
// case on the first that fails, binding its names as it goes. The type checker
// makes a match exhaustive, so nothing follows the last case; a case that
// cannot fail ends the chain.
Lowering::Emitted Lowering::LowerMatchWith(MidoriExpression::Match& match, const BranchSink& sink)
{
	return Lower(*match.m_arg_expr)
		.and_then([&](MidoriIRValueId scrutinee) -> Emitted
		{
			Builder().AtLine(match.m_match_keyword.m_line);
			for (std::unique_ptr<MidoriExpression>& case_expression : match.m_cases)
			{
				MidoriExpression::Case& match_case = case_expression->GetExpression<MidoriExpression::Case>();
				const MidoriIRBlockId fail = NewBlock();
				PushLocalFrame();
				const Emitted lowered = LowerPattern(*match_case.m_pattern, scrutinee, fail)
					.and_then([&]() -> Emitted
					{
						if (!match_case.HasGuard())
						{
							return {};
						}
						return Lower(*match_case.m_guard.value())
							.and_then([&](MidoriIRValueId guard) { return TestOrFail(guard, fail); });
					})
					.and_then([&]() { return sink(match_case.m_expr.get()); });
				PopLocalFrame();
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
			Builder().Unreachable();
			return {};
		});
}

Lowering::Lowered Lowering::LowerMatch(MidoriExpression::Match& match)
{
	const auto [join, sink] = Join(Concrete(match.m_type_data));
	return LowerMatchWith(match, sink)
		.transform([this, &join]()
		{
			return EnterJoin(*join);
		});
}

Lowering::Emitted Lowering::TestOrFail(MidoriIRValueId condition, MidoriIRBlockId fail)
{
	const MidoriIRBlockId matched = NewBlock();
	Branch(condition, MidoriIRSuccessor(matched), MidoriIRSuccessor(fail));
	PositionAt(matched);
	return {};
}

// Tests `value` against the pattern, going to `fail` when it does not match,
// and defines the names it binds on the way.
Lowering::Emitted Lowering::LowerPattern(const MidoriPattern& pattern, MidoriIRValueId value, MidoriIRBlockId fail)
{
	struct PatternLowering
	{
		Lowering& m_self;
		MidoriIRValueId m_value;
		MidoriIRBlockId m_fail;

		MidoriIRBuilder& Builder() const
		{
			return m_self.Builder();
		}

		TypeRef Representation() const
		{
			return GenericTypes::RepresentationOf(m_self.Scope().m_function.TypeOf(m_value));
		}

		Emitted Elements(const std::vector<std::unique_ptr<MidoriPattern>>& elements, const std::function<MidoriIRValueId(uint32_t)>& element) const
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

		Emitted operator()(const MidoriPattern::Binding& binding) const
		{
			if (binding.m_local_index.has_value())
			{
				m_self.DefineLocal(binding.m_local_index.value(), m_value);
			}
			return {};
		}

		Emitted operator()(const MidoriPattern::Wildcard&) const
		{
			return {};
		}

		Emitted operator()(const MidoriPattern::Literal& literal) const
		{
			if (literal.m_kind == MidoriLiteralKind::Unit)
			{
				return {};
			}
			const std::optional<MidoriIRImmediate> constant = m_self.ConstantOf(literal.m_kind, literal.m_token.m_lexeme);
			if (!constant.has_value())
			{
				return std::unexpected(m_self.Error(CompilerErrorCode::None, "Invalid literal in pattern '" + literal.m_token.m_lexeme + "'", literal.m_token));
			}
			Builder().AtLine(literal.m_token.m_line);
			const MidoriIRValueId expected = Builder().Emit(MidoriIROp::Const, LiteralType(literal.m_kind), {}, constant.value());
			return m_self.TestOrFail(Builder().Binary(EqualityOf(literal.m_kind), m_value, expected), m_fail);
		}

		Emitted operator()(const MidoriPattern::Tuple& tuple) const
		{
			const std::vector<TypeRef> element_types = Representation()->GetType<MidoriType::TupleType>().m_element_types;
			Builder().AtLine(tuple.m_left_paren.m_line);
			return Elements(tuple.m_elements, [&](uint32_t index)
			{
				return Builder().Emit(MidoriIROp::TupleGet, element_types[index], { m_value }, MidoriIRIndex{ index });
			});
		}

		// With a rest, the elements after it are counted from the end, and the rest
		// is the slice between the two ends.
		Emitted operator()(const MidoriPattern::Array& array) const
		{
			const TypeRef array_type = Representation();
			const TypeRef element_type = array_type->GetType<MidoriType::ArrayType>().m_element_type;
			const int64_t count = static_cast<int64_t>(array.m_elements.size());
			const int64_t before = array.m_rest.has_value() ? static_cast<int64_t>(array.m_rest->m_position) : count;
			Builder().AtLine(array.m_left_bracket.m_line);
			const MidoriIRValueId length = Builder().Emit(MidoriIROp::ArrayLength, IntType(), { m_value });
			const MidoriIROp comparison = array.m_rest.has_value() ? MidoriIROp::GeInt : MidoriIROp::EqInt;
			const auto from_end = [&](int64_t distance)
				{
					return Builder().Binary(MidoriIROp::SubInt, length, Builder().ConstInt(distance));
				};
			return m_self.TestOrFail(Builder().Binary(comparison, length, Builder().ConstInt(count)), m_fail)
				.and_then([&]()
				{
					return Elements(array.m_elements, [&](uint32_t index)
					{
						const MidoriIRValueId position = index < before ? Builder().ConstInt(index) : from_end(count - index);
						return Builder().Emit(MidoriIROp::ArrayGet, element_type, { m_value, position });
					});
				})
				.and_then([&]() -> Emitted
				{
					if (!array.m_rest.has_value() || !array.m_rest->m_pattern->IsPattern<MidoriPattern::Binding>())
					{
						return {};
					}
					const MidoriIRValueId rest = Builder().Emit(MidoriIROp::CallForeign, array_type, { m_value, Builder().ConstInt(before), from_end(count - before) }, MidoriIRForeign{ "MIDORI_FFI_ArraySlice" });
					return m_self.LowerPattern(*array.m_rest->m_pattern, rest, m_fail);
				});
		}

		Emitted operator()(const MidoriPattern::Constructor& constructor) const
		{
			Builder().AtLine(constructor.m_name_token.m_line);
			const TypeRef type = Representation();
			if (!constructor.m_is_union)
			{
				const std::vector<TypeRef> member_types = type->GetType<MidoriType::StructType>().m_member_types;
				return Elements(constructor.m_args, [&](uint32_t index)
				{
					return Builder().Emit(MidoriIROp::GetMember, member_types[index], { m_value }, MidoriIRIndex{ index });
				});
			}

			const int tag = constructor.m_tag;
			const std::vector<TypeRef> field_types = UnionMember(type, tag).m_member_types;
			const MidoriIRValueId actual = Builder().Emit(MidoriIROp::GetTag, IntType(), { m_value });
			const MidoriIRValueId expected = Builder().ConstInt(tag);
			return m_self.TestOrFail(Builder().Binary(MidoriIROp::EqInt, actual, expected), m_fail)
				.and_then([&]()
				{
					return Elements(constructor.m_args, [&](uint32_t index)
					{
						return Builder().Emit(MidoriIROp::UnionField, field_types[index], { m_value }, MidoriIRUnionField{ tag, index });
					});
				});
		}
	};

	return std::visit(PatternLowering{ *this, value, fail }, *pattern);
}

// The loop variable is a value of the loop's body, so a closure made there
// captures that iteration's. The header's parameter is where the loop is: the
// element for a range, the index for an array, the iterator for an Iterable.
// `carried` goes round the loop beside it, and out of the loop when it ends.
std::expected<std::vector<MidoriIRValueId>, CompilerError> Lowering::LowerLoop(MidoriExpression& iterated, const LoopKind& kind, int loop_variable, std::vector<MidoriIRValueId> carried, const LoopBody& body, const Token& at)
{
	const Lowered lowered_source = Lower(iterated);
	if (!lowered_source.has_value())
	{
		return std::unexpected(lowered_source.error());
	}
	const MidoriIRValueId source = lowered_source.value();
	if (IsDead())
	{
		return carried;
	}

	const std::vector<TypeRef> carried_types = TypesOf(carried);
	const TypeRef source_type = kind.m_is_iterable ? NominalType(iterated, source) : GenericTypes::RepresentationOf(Scope().m_function.TypeOf(source));
	Builder().AtLine(at.m_line);

	TypeRef position_type = source_type;
	MidoriIRValueId start = source;
	MidoriIRValueId step = source;
	MidoriIRValueId end = source;
	const bool is_range = !kind.m_is_array && !kind.m_is_iterable;
	bool is_float = false;
	if (kind.m_is_array)
	{
		position_type = IntType();
		start = Builder().ConstInt(0);
		end = Builder().Emit(MidoriIROp::ArrayLength, IntType(), { source });
	}
	else if (is_range)
	{
		position_type = source_type->GetType<MidoriType::RangeType>().m_element_type;
		is_float = position_type->IsType<MidoriType::FloatType>();
		start = Builder().Emit(MidoriIROp::RangeStart, position_type, { source });
		step = Builder().Emit(MidoriIROp::RangeStep, position_type, { source });
		end = Builder().Emit(MidoriIROp::RangeEnd, position_type, { source });
	}

	const MidoriIRBlockId header = NewBlock();
	const MidoriIRValueId position = Builder().AddParameter(header, position_type);
	const std::vector<MidoriIRValueId> header_carried = carried_types
		| std::views::transform([&](const TypeRef& type) { return Builder().AddParameter(header, type); })
		| std::ranges::to<std::vector>();
	const MidoriIRBlockId body_block = NewBlock();
	const MidoriIRBlockId exit = NewBlock();
	const std::vector<MidoriIRValueId> exit_carried = carried_types
		| std::views::transform([&](const TypeRef& type) { return Builder().AddParameter(exit, type); })
		| std::ranges::to<std::vector>();

	std::vector<MidoriIRValueId> entry_arguments = { start };
	entry_arguments.insert(entry_arguments.end(), carried.begin(), carried.end());
	Jump(header, std::move(entry_arguments));
	PositionAt(header);

	MidoriIRValueId element = position;
	std::optional<MidoriIRValueId> next_iterator;
	if (kind.m_is_array)
	{
		Branch(Builder().Binary(MidoriIROp::LtInt, position, end), MidoriIRSuccessor(body_block), MidoriIRSuccessor(exit, header_carried));
		PositionAt(body_block);
		element = Builder().Emit(MidoriIROp::ArrayGet, source_type->GetType<MidoriType::ArrayType>().m_element_type, { source, position });
	}
	else if (is_range)
	{
		// A positive step counts up to the end, any other down to it.
		const MidoriIRValueId zero = is_float ? Builder().ConstFloat(0.0) : Builder().ConstInt(0);
		const MidoriIRBlockId upward = NewBlock();
		const MidoriIRBlockId downward = NewBlock();
		Branch(Builder().Binary(is_float ? MidoriIROp::GtFloat : MidoriIROp::GtInt, step, zero), MidoriIRSuccessor(upward), MidoriIRSuccessor(downward));
		PositionAt(upward);
		Branch(Builder().Binary(is_float ? MidoriIROp::LtFloat : MidoriIROp::LtInt, position, end), MidoriIRSuccessor(body_block), MidoriIRSuccessor(exit, header_carried));
		PositionAt(downward);
		Branch(Builder().Binary(is_float ? MidoriIROp::GtFloat : MidoriIROp::GtInt, position, end), MidoriIRSuccessor(body_block), MidoriIRSuccessor(exit, header_carried));
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
		const TypeRef next_type = Scope().m_function.TypeOf(next.value());
		const MidoriIRValueId tag = Builder().AtLine(at.m_line).Emit(MidoriIROp::GetTag, IntType(), { next.value() });
		Branch(Builder().Binary(MidoriIROp::EqInt, tag, Builder().ConstInt(kind.m_some_tag)), MidoriIRSuccessor(body_block), MidoriIRSuccessor(exit, header_carried));
		PositionAt(body_block);
		const TypeRef payload_type = UnionMember(next_type, kind.m_some_tag).m_member_types.front();
		const std::vector<TypeRef>& pair_types = GenericTypes::RepresentationOf(payload_type)->GetType<MidoriType::TupleType>().m_element_types;
		const MidoriIRValueId payload = Builder().Emit(MidoriIROp::UnionField, payload_type, { next.value() }, MidoriIRUnionField{ kind.m_some_tag, 0u });
		element = Builder().Emit(MidoriIROp::TupleGet, pair_types[0u], { payload }, MidoriIRIndex{ 0u });
		next_iterator = Builder().Emit(MidoriIROp::TupleGet, pair_types[1u], { payload }, MidoriIRIndex{ 1u });
	}

	PushLocalFrame();
	DefineLocal(loop_variable, element);
	std::expected<std::vector<MidoriIRValueId>, CompilerError> updated = body(header_carried);
	PopLocalFrame();
	if (!updated.has_value())
	{
		return updated;
	}

	Builder().AtLine(at.m_line);
	const MidoriIRValueId advanced = next_iterator.has_value()
		? next_iterator.value()
		: Builder().Binary(kind.m_is_array ? MidoriIROp::AddInt : is_float ? MidoriIROp::AddFloat : MidoriIROp::AddInt, position, kind.m_is_array ? Builder().ConstInt(1) : step);
	std::vector<MidoriIRValueId> back_arguments = { advanced };
	back_arguments.insert(back_arguments.end(), updated.value().begin(), updated.value().end());
	Jump(header, std::move(back_arguments));
	PositionAt(exit);
	return exit_carried;
}

Lowering::Lowered Lowering::LowerFor(MidoriExpression::For& for_expression)
{
	const LoopKind kind = LoopKindOf(for_expression.m_is_array_iteration, for_expression.m_is_iterable_iteration, for_expression.m_iterable_item_type, for_expression.m_iterable_next_type, for_expression.m_iterable_some_tag);
	return LowerLoop(*for_expression.m_range, kind, for_expression.m_loop_variable_index, {}, [&](std::vector<MidoriIRValueId> carried) -> std::expected<std::vector<MidoriIRValueId>, CompilerError>
		{
			return Lower(*for_expression.m_body).transform([&carried](MidoriIRValueId) { return carried; });
		}, for_expression.m_for_keyword)
		.transform([this](std::vector<MidoriIRValueId>)
		{
			return Builder().ConstUnit();
		});
}

// The result grows by one element each iteration; nothing else holds it, so
// it grows in place.
Lowering::Lowered Lowering::LowerComprehension(MidoriExpression::ArrayComprehension& comprehension)
{
	const LoopKind kind = LoopKindOf(comprehension.m_is_array_iteration, comprehension.m_is_iterable_iteration, comprehension.m_iterable_item_type, comprehension.m_iterable_next_type, comprehension.m_iterable_some_tag);
	const TypeRef type = Concrete(comprehension.m_type_data);
	const MidoriIRValueId empty = Builder().AtLine(comprehension.m_bracket.m_line).Emit(MidoriIROp::MakeArray, type);
	return LowerLoop(*comprehension.m_range, kind, comprehension.m_loop_variable_index, { empty }, [&](std::vector<MidoriIRValueId> carried) -> std::expected<std::vector<MidoriIRValueId>, CompilerError>
		{
			return Lower(*comprehension.m_transform_expr)
				.transform([&](MidoriIRValueId element)
				{
					return std::vector<MidoriIRValueId>{ Builder().AtLine(comprehension.m_bracket.m_line).Emit(MidoriIROp::ArrayAppend, type, { carried.front(), element }) };
				});
		}, comprehension.m_bracket)
		.transform([](std::vector<MidoriIRValueId> result)
		{
			return result.front();
		});
}

Lowering::LoopKind Lowering::LoopKindOf(bool is_array, bool is_iterable, const TypeRef& item_type, const TypeRef& next_type, int some_tag)
{
	return LoopKind{ is_array, is_iterable, is_iterable ? Concrete(item_type) : nullptr, is_iterable ? Concrete(next_type) : nullptr, some_tag };
}

Lowering::Lowered Lowering::CallIterableNext(MidoriIRValueId iterator, const TypeRef& item_type, const TypeRef& next_type, const Token& at)
{
	const TypeRef iterator_type = Scope().m_function.TypeOf(iterator);
	const MethodResolution<std::string> resolved = m_resolver.ResolveIterableNext(Scope().m_specialization.m_methods, iterator_type, item_type);
	if (!resolved.has_value())
	{
		return std::unexpected(ResolutionError(resolved.error(), at));
	}
	return CallResolved(resolved.value(), { iterator }, { iterator_type }, next_type, at);
}
