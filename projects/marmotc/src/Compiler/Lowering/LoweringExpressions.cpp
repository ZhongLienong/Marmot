#include "FunctionLowering.h"
#include "Compiler/Operator/Ordering.h"
#include "Support/Expected.h"
#include "Support/Optional.h"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <span>
#include <utility>

namespace
{
	using TypeRef = std::shared_ptr<MarmotType>;

	MarmotCompiler::Optional<MarmotIRScalar> ScalarOf(const TypeRef& type)
	{
		const TypeRef representation = GenericTypes::RepresentationOf(type);
		if (representation->IsType<MarmotType::IntegerType>())
		{
			return MarmotIRScalar::Int;
		}
		if (representation->IsType<MarmotType::FloatType>())
		{
			return MarmotIRScalar::Float;
		}
		if (representation->IsType<MarmotType::ByteType>())
		{
			return MarmotIRScalar::Byte;
		}
		if (representation->IsType<MarmotType::WordType>())
		{
			return MarmotIRScalar::Word;
		}
		if (representation->IsType<MarmotType::BoolType>())
		{
			return MarmotIRScalar::Bool;
		}
		if (representation->IsType<MarmotType::TextType>())
		{
			return MarmotIRScalar::Text;
		}
		return MarmotCompiler::None;
	}

	// One op per scalar type, in the order Int, Float, Byte, Word, Bool, Text;
	// nothing where the operator does not apply to that type.
	using ScalarOps = std::array<MarmotCompiler::Optional<MarmotIROp>, 6u>;

	MarmotCompiler::Optional<MarmotIROp> SelectOp(const ScalarOps& ops, MarmotIRScalar scalar)
	{
		return ops[static_cast<size_t>(scalar)];
	}

	MarmotCompiler::Optional<ScalarOps> BinaryOps(Token::Name op)
	{
		using enum MarmotIROp;
		switch (op)
		{
		case Token::Name::SINGLE_PLUS:
			return ScalarOps{ AddInt, AddFloat, AddByte, AddWord, MarmotCompiler::None, MarmotCompiler::None };
		case Token::Name::SINGLE_MINUS:
			return ScalarOps{ SubInt, SubFloat, SubByte, SubWord, MarmotCompiler::None, MarmotCompiler::None };
		case Token::Name::STAR:
			return ScalarOps{ MulInt, MulFloat, MulByte, MulWord, MarmotCompiler::None, MarmotCompiler::None };
		case Token::Name::SLASH:
			return ScalarOps{ DivInt, DivFloat, DivByte, DivWord, MarmotCompiler::None, MarmotCompiler::None };
		case Token::Name::PERCENT:
			return ScalarOps{ ModInt, ModFloat, ModByte, ModWord, MarmotCompiler::None, MarmotCompiler::None };
		case Token::Name::LEFT_SHIFT:
			return ScalarOps{ ShlInt, MarmotCompiler::None, ShlByte, ShlWord, MarmotCompiler::None, MarmotCompiler::None };
		case Token::Name::RIGHT_SHIFT:
			return ScalarOps{ ShrInt, MarmotCompiler::None, ShrByte, ShrWord, MarmotCompiler::None, MarmotCompiler::None };
		case Token::Name::SINGLE_AMPERSAND:
			return ScalarOps{ BitAndInt, MarmotCompiler::None, BitAndByte, BitAndWord, MarmotCompiler::None, MarmotCompiler::None };
		case Token::Name::SINGLE_BAR:
			return ScalarOps{ BitOrInt, MarmotCompiler::None, BitOrByte, BitOrWord, MarmotCompiler::None, MarmotCompiler::None };
		case Token::Name::CARET:
			return ScalarOps{ BitXorInt, MarmotCompiler::None, BitXorByte, BitXorWord, MarmotCompiler::None, MarmotCompiler::None };
		case Token::Name::LEFT_ANGLE:
			return ScalarOps{ LtInt, LtFloat, LtByte, LtWord, MarmotCompiler::None, MarmotCompiler::None };
		case Token::Name::LESS_EQUAL:
			return ScalarOps{ LeInt, LeFloat, LeByte, LeWord, MarmotCompiler::None, MarmotCompiler::None };
		case Token::Name::RIGHT_ANGLE:
			return ScalarOps{ GtInt, GtFloat, GtByte, GtWord, MarmotCompiler::None, MarmotCompiler::None };
		case Token::Name::GREATER_EQUAL:
			return ScalarOps{ GeInt, GeFloat, GeByte, GeWord, MarmotCompiler::None, MarmotCompiler::None };
		case Token::Name::DOUBLE_EQUAL:
			return ScalarOps{ EqInt, EqFloat, EqByte, EqWord, EqBool, EqText };
		case Token::Name::BANG_EQUAL:
			return ScalarOps{ NeInt, NeFloat, NeByte, NeWord, NeBool, NeText };
		default:
			return MarmotCompiler::None;
		}
	}

	struct ScalarConversion
	{
		MarmotIRScalar m_from;
		MarmotIRScalar m_to;
		std::array<MarmotIROp, 2u> m_operations;
		std::size_t m_count = 1u;
	};

	std::span<const MarmotIROp> ConversionOps(MarmotIRScalar from, MarmotIRScalar to)
	{
		using enum MarmotIROp;
		using enum MarmotIRScalar;
		static constexpr ScalarConversion s_conversions[] =
		{
			{ Int, Float, { IntToFloat } },
			{ Text, Float, { TextToFloat } },
			{ Byte, Float, { ByteToFloat } },
			{ Word, Float, { WordToFloat } },
			{ Float, Int, { FloatToInt } },
			{ Text, Int, { TextToInt } },
			{ Byte, Int, { ByteToInt } },
			{ Word, Int, { WordToInt } },
			{ Int, Byte, { IntToByte } },
			{ Word, Byte, { WordToByte } },
			{ Float, Byte, { FloatToByte } },
			{ Int, Word, { IntToWord } },
			{ Byte, Word, { ByteToWord } },
			{ Float, Word, { FloatToWord } },
			{ Float, Text, { FloatToText } },
			{ Int, Text, { IntToText } },
			{ Byte, Text, { ByteToInt, IntToText }, 2u },
			{ Word, Text, { WordToText } }
		};

		const ScalarConversion* found = std::ranges::find_if(s_conversions, [from, to](const ScalarConversion& conversion)
		{
			return conversion.m_from == from && conversion.m_to == to;
		});
		return found == std::end(s_conversions) ? std::span<const MarmotIROp>{} : std::span(found->m_operations).first(found->m_count);
	}

	const TypeRef& UnitType()
	{
		return MarmotType::MakeLiteralType<MarmotType::UnitType>();
	}

	const TypeRef& IntType()
	{
		return MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	}

	// An operand the `++` may extend in place, because nothing else holds it:
	// another `++` a class does not provide, or an array literal or
	// comprehension. Not a text literal: every load of one pushes the same
	// cached text.
	bool IsFreshConcatTemporary(const MarmotExpression& expression)
	{
		if (expression.IsExpression<MarmotExpression::Binary>())
		{
			const MarmotExpression::Binary& binary = expression.GetExpression<MarmotExpression::Binary>();
			return binary.m_op.m_token_name == Token::Name::DOUBLE_PLUS && !binary.m_uses_concatenable;
		}

		return expression.IsExpression<MarmotExpression::Array>()
			|| expression.IsExpression<MarmotExpression::ArrayComprehension>();
	}
}

Lowering::Lowered Lowering::FunctionLowering::Lower(const MarmotExpression& expression)
{
	struct ExpressionLowering
	{
		FunctionLowering& m_self;

		Lowered operator()(const MarmotExpression::Literal& literal) const
		{
			return m_self.LowerLiteral(literal);
		}

		Lowered operator()(const MarmotExpression::Group& group) const
		{
			return m_self.Lower(*group.m_expr_in);
		}

		Lowered operator()(const MarmotExpression::Binary& binary) const
		{
			return m_self.LowerBinary(binary);
		}

		Lowered operator()(const MarmotExpression::UnaryPrefix& unary) const
		{
			return m_self.LowerUnary(unary);
		}

		Lowered operator()(const MarmotExpression::As& as) const
		{
			return m_self.LowerAs(as);
		}

		Lowered operator()(const MarmotExpression::NameAccess& name) const
		{
			return m_self.LowerName(name);
		}

		Lowered operator()(const MarmotExpression::IfElse& if_else) const
		{
			return m_self.LowerIf(if_else);
		}

		Lowered operator()(const MarmotExpression::Block& block) const
		{
			return m_self.LowerBlock(block);
		}

		Lowered operator()(const MarmotExpression::Function& function) const
		{
			return m_self.LowerFunction(function);
		}

		Lowered operator()(const MarmotExpression::Call& call) const
		{
			return m_self.LowerCall(call);
		}

		Lowered operator()(const MarmotExpression::Tuple& tuple) const
		{
			return m_self.LowerTuple(tuple);
		}

		Lowered operator()(const MarmotExpression::Construct& construct) const
		{
			return m_self.LowerConstruct(construct);
		}

		Lowered operator()(const MarmotExpression::RecordUpdate& record_update) const
		{
			return m_self.LowerRecordUpdate(record_update);
		}

		Lowered operator()(const MarmotExpression::MemberAccess& member) const
		{
			return m_self.LowerMemberAccess(member);
		}

		Lowered operator()(const MarmotExpression::Array& array) const
		{
			return m_self.LowerArray(array);
		}

		Lowered operator()(const MarmotExpression::IndexAccess& index) const
		{
			return m_self.LowerIndex(index);
		}

		Lowered operator()(const MarmotExpression::ArrayComprehension& comprehension) const
		{
			return m_self.LowerComprehension(comprehension);
		}

		Lowered operator()(const MarmotExpression::RangeBinary& range) const
		{
			return m_self.LowerRange(*range.m_start, nullptr, *range.m_end, m_self.Concrete(range.m_type_data), range.m_range_op.m_line);
		}

		Lowered operator()(const MarmotExpression::RangeTernary& range) const
		{
			return m_self.LowerRange(*range.m_start, range.m_step.get(), *range.m_end, m_self.Concrete(range.m_type_data), range.m_first_range_op.m_line);
		}

		Lowered operator()(const MarmotExpression::Match& match) const
		{
			return m_self.LowerMatch(match);
		}

		// Only a match holds its cases.
		Lowered operator()(const MarmotExpression::Case&) const
		{
			std::unreachable();
		}

		Lowered operator()(const MarmotExpression::For& for_expression) const
		{
			return m_self.LowerFor(for_expression);
		}
	};

	return std::visit(ExpressionLowering{ *this }, *expression);
}

MarmotCompiler::Optional<MarmotIRImmediate> Lowering::FunctionLowering::ConstantOf(const MarmotLiteral& literal) const
{
	const std::string& lexeme = literal.m_token.m_lexeme;
	switch (literal.m_kind)
	{
	case MarmotLiteralKind::Text:
		return MarmotIRImmediate(lexeme);
	case MarmotLiteralKind::Bool:
		return MarmotIRImmediate(lexeme == Token::Spelling(Token::Name::TRUE));
	case MarmotLiteralKind::Float:
		return MarmotIRImmediate(literal.m_float_value.Value());
	case MarmotLiteralKind::Unit:
		return MarmotIRImmediate();
	case MarmotLiteralKind::Integer:
		return ParseIntegerLiteral(lexeme).Transform([](int64_t value) { return MarmotIRImmediate(value); });
	case MarmotLiteralKind::Byte:
	{
		const MarmotCompiler::Optional<uint64_t> value = ParseUnsignedLiteral(lexeme);
		if (!value.HasValue() || value.Value() > 0xFFu)
		{
			return MarmotCompiler::None;
		}
		return MarmotIRImmediate(MarmotIRByte{ static_cast<uint8_t>(value.Value()) });
	}
	case MarmotLiteralKind::Word:
		return ParseUnsignedLiteral(lexeme).Transform([](uint64_t value) { return MarmotIRImmediate(MarmotIRWord{ value }); });
	}
	std::unreachable();
}

Lowering::Lowered Lowering::FunctionLowering::LowerLiteral(const MarmotExpression::Literal& literal)
{
	const Token& token = literal.m_token;
	const MarmotCompiler::Optional<MarmotIRImmediate> constant = ConstantOf(literal);
	if (!constant.HasValue())
	{
		switch (literal.m_kind)
		{
		case MarmotLiteralKind::Integer:
			return MarmotCompiler::Failure(m_owner.Error(CompilerErrorCode::None, "Integer literal '" + token.m_lexeme + "' is out of range. Maximum value is 9223372036854775807 (2^63 - 1), minimum value is -9223372036854775808 (-2^63).", token));
		case MarmotLiteralKind::Byte:
			return MarmotCompiler::Failure(m_owner.Error(CompilerErrorCode::None, "Byte literal '" + token.m_lexeme + "' is out of range. Maximum value is 255 (0xFF).", token));
		case MarmotLiteralKind::Word:
			return MarmotCompiler::Failure(m_owner.Error(CompilerErrorCode::None, "Word literal '" + token.m_lexeme + "' is out of range. Maximum value is 18446744073709551615 (0xFFFFFFFFFFFFFFFF).", token));
		default:
			std::unreachable();
		}
	}

	static constexpr std::array<MarmotIRScalar, 6u> s_scalars = { MarmotIRScalar::Bool, MarmotIRScalar::Int, MarmotIRScalar::Byte, MarmotIRScalar::Word, MarmotIRScalar::Float, MarmotIRScalar::Text };
	const TypeRef& type = literal.m_kind == MarmotLiteralKind::Unit ? UnitType() : MarmotIRScalarType(s_scalars[static_cast<size_t>(literal.m_kind)]);
	return m_builder.AtLine(token.m_line).Emit(MarmotIROp::Const, type, {}, constant.Value());
}

Lowering::Lowered Lowering::FunctionLowering::LowerBinary(const MarmotExpression::Binary& binary)
{
	const Token& op = binary.m_op;
	if (op.m_token_name == Token::Name::DOUBLE_AMPERSAND || op.m_token_name == Token::Name::DOUBLE_BAR)
	{
		return LowerShortCircuit(binary);
	}

	return Lower(*binary.m_left)
		.AndThen
		(
			[this, &binary](MarmotIRValueId left)
			{
				return Lower(*binary.m_right)
					.Transform([left](MarmotIRValueId right) { return std::pair{ left, right }; });
			}
		)
		.AndThen
		(
			[this, &binary, &op](std::pair<MarmotIRValueId, MarmotIRValueId> operands) -> Lowered
			{
				const auto [left, right] = operands;
				if (IsDead())
				{
					return Placeholder(Concrete(binary.m_type_data));
				}
				if (op.m_token_name == Token::Name::COLON_EQUAL)
				{
					m_builder.AtLine(op.m_line).Emit(MarmotIROp::CellWrite, m_function.TypeOf(right), { left, right });
					return m_builder.ConstUnit();
				}
				if (binary.m_uses_concatenable || binary.m_uses_equatable || binary.m_ordering.HasValue())
				{
					return LowerClassOperator(binary, left, right);
				}
				if (op.m_token_name == Token::Name::DOUBLE_PLUS)
				{
					return LowerConcat(binary, left, right);
				}

				return LowerScalarBinary(op, left, right);
			}
		);
}

Lowering::Lowered Lowering::FunctionLowering::LowerScalarBinary(const Token& op, MarmotIRValueId left, MarmotIRValueId right)
{
	const TypeRef& operand_type = m_function.TypeOf(left);
	const MarmotCompiler::Optional<MarmotIRScalar> scalar = ScalarOf(operand_type);
	const MarmotCompiler::Optional<ScalarOps> ops = BinaryOps(op.m_token_name);
	const MarmotCompiler::Optional<MarmotIROp> ir_op = (scalar.HasValue() && ops.HasValue()) ? SelectOp(ops.Value(), scalar.Value()) : MarmotCompiler::None;
	if (!ir_op.HasValue())
	{
		return MarmotCompiler::Failure(m_owner.Unsupported(std::format("'{}' on {}", op.m_lexeme, operand_type->DisplayString()), op));
	}

	m_builder.AtLine(op.m_line);
	if (GetMarmotIROpInfo(ir_op.Value()).m_arity == MarmotIRArity::Binary)
	{
		return m_builder.Binary(ir_op.Value(), left, right);
	}
	return m_builder.Emit(ir_op.Value(), MarmotIRScalarType(scalar.Value()), { left, right });
}

// `a && b` is `b` when `a` holds and `a` otherwise; `a || b` the other way.
Lowering::Lowered Lowering::FunctionLowering::LowerShortCircuit(const MarmotExpression::Binary& binary)
{
	const bool is_and = binary.m_op.m_token_name == Token::Name::DOUBLE_AMPERSAND;
	return Lower(*binary.m_left)
		.AndThen
		(
			[this, &binary, is_and](MarmotIRValueId left) -> Lowered
			{
				const MarmotIRBlockId right_block = NewBlock();
				const MarmotIRBlockId join = NewBlock();
				const MarmotIRValueId result = m_builder.AddParameter(join, MarmotIRScalarType(MarmotIRScalar::Bool));
				const MarmotIRSuccessor evaluate_right(right_block);
				const MarmotIRSuccessor skip_right(join, { left });
				m_builder.AtLine(binary.m_op.m_line);
				Branch(left, is_and ? evaluate_right : skip_right, is_and ? skip_right : evaluate_right);

				PositionAt(right_block);
				return Lower(*binary.m_right)
					.Transform
					(
						[this, join, result](MarmotIRValueId right)
						{
							Jump(join, { right });
							PositionAt(join);
							return result;
						}
					);
			}
		);
}

// The operators a class provides call the instance method they resolve to,
// keyed by the operand's own type: a newtype selects its own instance.
Lowering::Lowered Lowering::FunctionLowering::LowerClassOperator(const MarmotExpression::Binary& binary, MarmotIRValueId left, MarmotIRValueId right)
{
	const Token& op = binary.m_op;
	const TypeRef operand_type = NominalType(*binary.m_left, left);
	const std::vector<TypeRef> argument_types = { operand_type, NominalType(*binary.m_right, right) };
	const MethodResolutionMap& methods = m_specialization.m_methods;
	const TypeRef& bool_type = MarmotIRScalarType(MarmotIRScalar::Bool);

	if (binary.m_uses_concatenable)
	{
		const MethodResolution<ResolvedInstanceName> resolved = m_owner.m_resolver.ResolveConcat(methods, operand_type);
		if (!resolved.HasValue())
		{
			return MarmotCompiler::Failure(m_owner.ResolutionError(resolved.Error(), op));
		}
		return CallResolved(resolved.Value(), { left, right }, argument_types, operand_type, op);
	}

	if (binary.m_uses_equatable)
	{
		const MethodResolution<ResolvedInstanceName> resolved = m_owner.m_resolver.ResolveEquals(operand_type);
		if (!resolved.HasValue())
		{
			return MarmotCompiler::Failure(m_owner.ResolutionError(resolved.Error(), op));
		}
		return CallResolved(resolved.Value(), { left, right }, argument_types, bool_type, op)
			.Transform([this, &op](MarmotIRValueId equal)
			{
				return op.m_token_name == Token::Name::BANG_EQUAL ? m_builder.AtLine(op.m_line).Unary(MarmotIROp::NotBool, equal) : equal;
			});
	}

	const MethodResolution<ResolvedInstanceName> resolved = m_owner.m_resolver.ResolveCompare(operand_type);
	if (!resolved.HasValue())
	{
		return MarmotCompiler::Failure(m_owner.ResolutionError(resolved.Error(), op));
	}
	const OrderingType& ordering = binary.m_ordering.Value();
	const bool is_less = op.m_token_name == Token::Name::LEFT_ANGLE || op.m_token_name == Token::Name::LESS_EQUAL;
	const bool includes_equal = op.m_token_name == Token::Name::LESS_EQUAL || op.m_token_name == Token::Name::GREATER_EQUAL;
	return CallResolved(resolved.Value(), { left, right }, argument_types, ordering.m_type, op)
		.Transform
		(
			[this, &op, &ordering, is_less, includes_equal](MarmotIRValueId value)
			{
				m_builder.AtLine(op.m_line);
				const MarmotIRValueId tag = m_builder.Emit(MarmotIROp::GetTag, IntType(), { value });
				const int expected = ordering.Tag(is_less ? MarmotOrdering::Less : MarmotOrdering::Greater);
				const MarmotIRValueId matches = m_builder.Binary(MarmotIROp::EqInt, tag, m_builder.ConstInt(expected));
				if (!includes_equal)
				{
					return matches;
				}
				const MarmotIRBlockId check_equal = NewBlock();
				const MarmotIRBlockId join = NewBlock();
				const MarmotIRValueId result = m_builder.AddParameter(join, MarmotIRScalarType(MarmotIRScalar::Bool));
				Branch(matches, MarmotIRSuccessor(join, { m_builder.ConstBool(true) }), MarmotIRSuccessor(check_equal));
				PositionAt(check_equal);
				const MarmotIRValueId equal = m_builder.Binary(MarmotIROp::EqInt, tag, m_builder.ConstInt(ordering.Tag(MarmotOrdering::Equal)));
				Jump(join, { equal });
				PositionAt(join);
				return result;
			}
		);
}

Lowering::Lowered Lowering::FunctionLowering::LowerConcat(const MarmotExpression::Binary& binary, MarmotIRValueId left, MarmotIRValueId right)
{
	const TypeRef type = GenericTypes::RepresentationOf(m_function.TypeOf(left));
	if (!type->IsType<MarmotType::TextType>() && !type->IsType<MarmotType::ArrayType>())
	{
		return MarmotCompiler::Failure(m_owner.Error(CompilerErrorCode::None, std::format("Concatenation operator '++' requires Text or Array type (got {})", type->ToString()), binary.m_op));
	}
	const MarmotIROp op = IsFreshConcatTemporary(*binary.m_left) ? MarmotIROp::Extend : MarmotIROp::Concat;
	return m_builder.AtLine(binary.m_op.m_line).Emit(op, type, { left, right });
}

Lowering::Lowered Lowering::FunctionLowering::LowerUnary(const MarmotExpression::UnaryPrefix& unary)
{
	const Token& op = unary.m_op;
	if (op.m_token_name == Token::Name::SINGLE_PLUS)
	{
		return Lower(*unary.m_expr);
	}

	return Lower(*unary.m_expr)
		.AndThen
		(
			[this, &unary, &op](MarmotIRValueId operand) -> Lowered
			{
				if (IsDead())
				{
					return Placeholder(Concrete(unary.m_type_data));
				}
				const TypeRef& operand_type = m_function.TypeOf(operand);
				if (op.m_token_name == Token::Name::REF || op.m_token_name == Token::Name::STAR)
				{
					const MarmotIROp cell_op = op.m_token_name == Token::Name::REF ? MarmotIROp::CellNew : MarmotIROp::CellRead;
					return m_builder.AtLine(op.m_line).Emit(cell_op, Concrete(unary.m_type_data), { operand });
				}
				if (op.m_token_name == Token::Name::HASH)
				{
					if (GenericTypes::RepresentationOf(operand_type)->IsType<MarmotType::ArrayType>())
					{
						return m_builder.AtLine(op.m_line).Emit(MarmotIROp::ArrayLength, IntType(), { operand });
					}
					return LowerCount(unary, operand);
				}

				return LowerScalarUnary(op, operand);
			}
		);
}

Lowering::Lowered Lowering::FunctionLowering::LowerScalarUnary(const Token& op, MarmotIRValueId operand)
{
	if (op.m_token_name == Token::Name::SINGLE_PLUS)
	{
		return operand;
	}
	const TypeRef& operand_type = m_function.TypeOf(operand);
	if (op.m_token_name == Token::Name::SINGLE_MINUS && (operand_type->IsType<MarmotType::ByteType>() || operand_type->IsType<MarmotType::WordType>()))
	{
		const bool byte = operand_type->IsType<MarmotType::ByteType>();
		m_builder.AtLine(op.m_line);
		const MarmotIRValueId zero = m_builder.Emit(MarmotIROp::Const, operand_type, {}, byte ? MarmotIRImmediate(MarmotIRByte{ 0u }) : MarmotIRImmediate(MarmotIRWord{ 0u }));
		return m_builder.Binary(byte ? MarmotIROp::SubByte : MarmotIROp::SubWord, zero, operand);
	}
	const MarmotCompiler::Optional<MarmotIRScalar> scalar = ScalarOf(operand_type);
	const MarmotCompiler::Optional<MarmotIROp> ir_op = [&]() -> MarmotCompiler::Optional<MarmotIROp>
	{
		if (!scalar.HasValue())
		{
			return MarmotCompiler::None;
		}
		using enum MarmotIROp;
		switch (op.m_token_name)
		{
		case Token::Name::SINGLE_MINUS:
			return SelectOp(ScalarOps{ NegInt, NegFloat, MarmotCompiler::None, MarmotCompiler::None, MarmotCompiler::None, MarmotCompiler::None }, scalar.Value());
		case Token::Name::BANG:
			return SelectOp(ScalarOps{ MarmotCompiler::None, MarmotCompiler::None, MarmotCompiler::None, MarmotCompiler::None, NotBool, MarmotCompiler::None }, scalar.Value());
		case Token::Name::TILDE:
			return SelectOp(ScalarOps{ BitNotInt, MarmotCompiler::None, BitNotByte, BitNotWord, MarmotCompiler::None, MarmotCompiler::None }, scalar.Value());
		default:
			return MarmotCompiler::None;
		}
	}();
	if (!ir_op.HasValue())
	{
		return MarmotCompiler::Failure(m_owner.Unsupported(std::format("'{}' on {}", op.m_lexeme, operand_type->DisplayString()), op));
	}
	return m_builder.AtLine(op.m_line).Unary(ir_op.Value(), operand);
}

Lowering::Lowered Lowering::FunctionLowering::LowerCount(const MarmotExpression::UnaryPrefix& unary, MarmotIRValueId operand)
{
	const TypeRef operand_type = NominalType(*unary.m_expr, operand);
	const MethodResolution<MarmotCompiler::Optional<ResolvedInstanceName>> resolved = m_owner.m_resolver.ResolveCount(m_specialization.m_methods, operand_type, unary.m_uses_countable);
	if (!resolved.HasValue())
	{
		return MarmotCompiler::Failure(m_owner.ResolutionError(resolved.Error(), unary.m_op));
	}
	if (!resolved.Value().HasValue())
	{
		return MarmotCompiler::Failure(m_owner.Unsupported(std::format("'#' on {}", operand_type->DisplayString()), unary.m_op));
	}
	return CallResolved(resolved.Value().Value(), { operand }, { operand_type }, IntType(), unary.m_op);
}

// An instance a program writes wins over a builtin conversion; a conversion
// between a newtype and its representation is no instruction at all.
Lowering::Lowered Lowering::FunctionLowering::LowerAs(const MarmotExpression::As& as)
{
	const Token& keyword = as.m_as_keyword;
	const TypeRef to_type = Concrete(as.m_to_type);
	return Lower(*as.m_expr)
		.AndThen
		(
			[this, &as, &keyword, &to_type](MarmotIRValueId value) -> Lowered
			{
				if (IsDead())
				{
					return Placeholder(to_type);
				}

				const TypeRef from_type = NominalType(*as.m_expr, value);
				const MethodResolution<MarmotCompiler::Optional<ResolvedInstanceName>> resolved = m_owner.m_resolver.ResolveConvert(m_specialization.m_methods, from_type, from_type, to_type, as.m_uses_convertible);
				if (!resolved.HasValue())
				{
					return MarmotCompiler::Failure(m_owner.ResolutionError(resolved.Error(), keyword));
				}
				if (resolved.Value().HasValue())
				{
					return CallResolved(resolved.Value().Value(), { value }, { from_type }, to_type, keyword);
				}
				if (MarmotIRSameType(from_type, to_type))
				{
					return value;
				}

				m_builder.AtLine(keyword.m_line);
				if (to_type->IsType<MarmotType::UnitType>())
				{
					return m_builder.ConstUnit();
				}

				const MarmotCompiler::Optional<MarmotIRScalar> from = ScalarOf(from_type);
				const MarmotCompiler::Optional<MarmotIRScalar> to = ScalarOf(to_type);
				if (from == MarmotIRScalar::Bool && to == MarmotIRScalar::Text)
				{
					return LowerBoolToText(value, keyword.m_line);
				}

				const std::span<const MarmotIROp> ops = (from.HasValue() && to.HasValue()) ? ConversionOps(from.Value(), to.Value()) : std::span<const MarmotIROp>{};
				if (ops.empty())
				{
					return MarmotCompiler::Failure(m_owner.Unsupported(std::format("a conversion from {} to {}", from_type->DisplayString(), to_type->DisplayString()), keyword));
				}
				return std::ranges::fold_left(ops, value, [this](MarmotIRValueId operand, MarmotIROp op) { return m_builder.Unary(op, operand); });
			}
		);
}

Lowering::Lowered Lowering::FunctionLowering::LowerBoolToText(MarmotIRValueId value, int line)
{
	const MarmotIRBlockId when_true = NewBlock();
	const MarmotIRBlockId when_false = NewBlock();
	const MarmotIRBlockId join = NewBlock();
	const MarmotIRValueId text = m_builder.AddParameter(join, MarmotIRScalarType(MarmotIRScalar::Text));
	m_builder.AtLine(line);
	Branch(value, MarmotIRSuccessor(when_true), MarmotIRSuccessor(when_false));

	PositionAt(when_true);
	Jump(join, { m_builder.ConstText(std::string(Token::Spelling(Token::Name::TRUE))) });
	PositionAt(when_false);
	Jump(join, { m_builder.ConstText(std::string(Token::Spelling(Token::Name::FALSE))) });
	PositionAt(join);
	return text;
}

Lowering::Lowered Lowering::FunctionLowering::LowerOperation(MarmotIROp op, const TypeRef& type, std::vector<const MarmotExpression*> operands, MarmotIRImmediate immediate, int line)
{
	std::vector<MarmotIRValueId> values;
	values.reserve(operands.size());
	for (const MarmotExpression* operand : operands)
	{
		const Lowered value = Lower(*operand);
		if (!value.HasValue())
		{
			return value;
		}
		values.push_back(value.Value());
	}
	if (IsDead())
	{
		return Placeholder(type);
	}
	return m_builder.AtLine(line).Emit(op, type, std::move(values), std::move(immediate));
}

Lowering::Lowered Lowering::FunctionLowering::LowerTuple(const MarmotExpression::Tuple& tuple)
{
	std::vector<const MarmotExpression*> elements = tuple.m_elements
		| std::views::transform([](const std::unique_ptr<MarmotExpression>& element) -> const MarmotExpression* { return element.get(); })
		| std::ranges::to<std::vector>();
	return LowerOperation(MarmotIROp::MakeTuple, Concrete(tuple.m_type_data), std::move(elements), {}, tuple.m_op.m_line);
}

Lowering::Lowered Lowering::FunctionLowering::LowerArray(const MarmotExpression::Array& array)
{
	std::vector<const MarmotExpression*> elements = array.m_elems
		| std::views::transform([](const std::unique_ptr<MarmotExpression>& element) -> const MarmotExpression* { return element.get(); })
		| std::ranges::to<std::vector>();
	return LowerOperation(MarmotIROp::MakeArray, Concrete(array.m_type_data), std::move(elements), {}, array.m_op.m_line);
}

// An array is indexed by the instruction. The array fast path is load-bearing:
// the prelude's Indexable<Array<T>, Int> instance indexes an array in its
// body, so sending arrays through the instance would make it call itself.
Lowering::Lowered Lowering::FunctionLowering::LowerIndex(const MarmotExpression::IndexAccess& index)
{
	const Token& op = index.m_op;
	const TypeRef result_type = Concrete(index.m_type_data);
	return Lower(*index.m_arr_var)
		.AndThen
		(
			[this, &index](MarmotIRValueId container)
			{
				return Lower(*index.m_index)
					.Transform([container](MarmotIRValueId position) { return std::pair{ container, position }; });
			}
		)
		.AndThen
		(
			[this, &index, &op, &result_type](std::pair<MarmotIRValueId, MarmotIRValueId> operands) -> Lowered
			{
				const auto [container, position] = operands;
				if (IsDead())
				{
					return Placeholder(result_type);
				}
				const TypeRef container_type = m_function.TypeOf(container);
				if (index.m_uses_indexable)
				{
					const std::vector<TypeRef> argument_types = { NominalType(*index.m_arr_var, container), NominalType(*index.m_index, position) };
					const MethodResolution<ResolvedInstanceName> resolved = m_owner.m_resolver.ResolveIndex(m_specialization.m_methods, argument_types[0u], argument_types[1u]);
					if (!resolved.HasValue())
					{
						return MarmotCompiler::Failure(m_owner.ResolutionError(resolved.Error(), op));
					}
					return CallResolved(resolved.Value(), { container, position }, argument_types, result_type, op);
				}
				const TypeRef& element_type = GenericTypes::RepresentationOf(container_type)->GetType<MarmotType::ArrayType>().m_element_type;
				return m_builder.AtLine(op.m_line).Emit(MarmotIROp::ArrayGet, element_type, { container, position });
			}
		);
}

Lowering::Lowered Lowering::FunctionLowering::LowerConstruct(const MarmotExpression::Construct& construct)
{
	std::vector<const MarmotExpression*> fields = construct.m_params
		| std::views::transform([](const std::unique_ptr<MarmotExpression>& field) -> const MarmotExpression* { return field.get(); })
		| std::ranges::to<std::vector>();
	const TypeRef type = Concrete(construct.m_type_data);
	const int line = construct.m_data_name.m_line;
	if (construct.IsConstructTypeOf<MarmotExpression::Construct::Struct>())
	{
		return LowerOperation(MarmotIROp::Construct, type, std::move(fields), {}, line);
	}
	const int tag = std::get<MarmotExpression::Construct::Union>(construct.m_construct_ctx).m_index;
	return LowerOperation(MarmotIROp::MakeUnion, type, std::move(fields), MarmotIRTag{ tag }, line);
}

// A copy of the source with some members replaced. Every replacement is
// evaluated against the original, in member order, which is what makes
// `{ r with a = r.b, b = r.a }` a swap rather than two updates.
Lowering::Lowered Lowering::FunctionLowering::LowerRecordUpdate(const MarmotExpression::RecordUpdate& record_update)
{
	const int line = record_update.m_with_keyword.m_line;
	const TypeRef type = Concrete(record_update.m_type_data);
	const std::vector<const MarmotExpression::RecordUpdate::FieldUpdate*> updates = record_update.m_updates
		| std::views::transform([](const MarmotExpression::RecordUpdate::FieldUpdate& update) { return &update; })
		| std::ranges::to<std::vector>();
	return Lower(*record_update.m_source)
		.AndThen
		(
			[&](MarmotIRValueId source) -> Lowered
			{
				if (IsDead())
				{
					return Placeholder(type);
				}
				return LowerUpdatedRecord(source, type, updates, 0u, line);
			}
		);
}

// `source` rebuilt with each update's value at the end of its path, a record
// on the way rebuilt from its old members and the updates that pass through it.
Lowering::Lowered Lowering::FunctionLowering::LowerUpdatedRecord(MarmotIRValueId source, const TypeRef& type, const std::vector<const MarmotExpression::RecordUpdate::FieldUpdate*>& updates, size_t depth, int line)
{
	const std::vector<TypeRef>& member_types = GenericTypes::RepresentationOf(m_function.TypeOf(source))->GetType<MarmotType::StructType>().m_member_types;
	std::vector<MarmotIRValueId> members;
	members.reserve(member_types.size());
	for (size_t slot = 0u; slot < member_types.size(); slot += 1u)
	{
		const std::vector<const MarmotExpression::RecordUpdate::FieldUpdate*> here = updates
			| std::views::filter([depth, slot](const MarmotExpression::RecordUpdate::FieldUpdate* update) { return static_cast<size_t>(update->m_indices[depth]) == slot; })
			| std::ranges::to<std::vector>();
		const Lowered value = [&]() -> Lowered
		{
			if (!here.empty() && here.front()->m_indices.size() == depth + 1u)
			{
				return Lower(*here.front()->m_value);
			}
			const MarmotIRValueId old_member = m_builder.AtLine(line).Emit(MarmotIROp::GetMember, member_types[slot], { source }, MarmotIRIndex{ static_cast<uint32_t>(slot) });
			return here.empty() ? Lowered(old_member) : LowerUpdatedRecord(old_member, member_types[slot], here, depth + 1u, line);
		}();
		if (!value.HasValue())
		{
			return value;
		}
		members.push_back(value.Value());
	}
	if (IsDead())
	{
		return Placeholder(type);
	}
	return m_builder.AtLine(line).Emit(MarmotIROp::Construct, type, std::move(members));
}

Lowering::Lowered Lowering::FunctionLowering::LowerMemberAccess(const MarmotExpression::MemberAccess& member)
{
	const int line = member.m_member_name.m_line;
	return Lower(*member.m_struct)
		.Transform
		(
			[this, &member, line](MarmotIRValueId record)
			{
				if (IsDead())
				{
					return Placeholder(Concrete(member.m_type_data));
				}
				const std::vector<TypeRef>& member_types = GenericTypes::RepresentationOf(m_function.TypeOf(record))->GetType<MarmotType::StructType>().m_member_types;
				const uint32_t index = static_cast<uint32_t>(member.m_index);
				return m_builder.AtLine(line).Emit(MarmotIROp::GetMember, member_types[index], { record }, MarmotIRIndex{ index });
			}
		);
}

// A range without a step steps by one.
Lowering::Lowered Lowering::FunctionLowering::LowerRange(const MarmotExpression& start, const MarmotExpression* step, const MarmotExpression& end, const TypeRef& range_type, int line)
{
	const bool is_float = range_type->GetType<MarmotType::RangeType>().m_element_type->IsType<MarmotType::FloatType>();
	return Lower(start)
		.AndThen
		(
			[&](MarmotIRValueId first) -> LoweredValues
			{
				const Lowered stride = step == nullptr
					? Lowered(is_float ? m_builder.AtLine(line).ConstFloat(1.0) : m_builder.AtLine(line).ConstInt(1))
					: Lower(*step);
				return stride.AndThen([&](MarmotIRValueId by)
				{
					return Lower(end).Transform([first, by](MarmotIRValueId last) { return std::vector<MarmotIRValueId>{ first, by, last }; });
				});
			}
		)
		.Transform
		(
			[&](std::vector<MarmotIRValueId> bounds)
			{
				if (IsDead())
				{
					return Placeholder(range_type);
				}
				return m_builder.AtLine(line).Emit(MarmotIROp::MakeRange, range_type, std::move(bounds));
			}
		);
}

// The one argument stands for all of the function's parameters: the value
// itself for one, a tuple spread across them for several, and a discarded
// Unit for none. The function is evaluated after it.
Lowering::Lowered Lowering::FunctionLowering::LowerSpawn(const MarmotExpression::Call& spawn)
{
	const int line = spawn.m_paren.m_line;
	const TypeRef type = Concrete(spawn.m_type_data);
	const MarmotExpression& function = *spawn.m_arguments[1u];
	const size_t arity = Concrete(function.GetType())->GetType<MarmotType::FunctionType>().m_param_types.size();
	return Lower(*spawn.m_arguments[0u])
		.AndThen
		(
			[&](MarmotIRValueId argument) -> Lowered
			{
				std::vector<MarmotIRValueId> operands;
				if (arity == 1u)
				{
					operands.push_back(argument);
				}
				else if (arity >= 2u && !IsDead())
				{
					const std::vector<TypeRef>& element_types = GenericTypes::RepresentationOf(m_function.TypeOf(argument))->GetType<MarmotType::TupleType>().m_element_types;
					for (uint32_t index = 0u; index < static_cast<uint32_t>(arity); index += 1u)
					{
						operands.push_back(m_builder.AtLine(line).Emit(MarmotIROp::TupleGet, element_types[index], { argument }, MarmotIRIndex{ index }));
					}
				}

				return Lower(function)
					.Transform
					(
						[&](MarmotIRValueId callee)
						{
							if (IsDead())
							{
								return Placeholder(type);
							}
							operands.push_back(callee);
							return m_builder.AtLine(line).Emit(MarmotIROp::Spawn, type, std::move(operands));
						}
					);
			}
		);
}
