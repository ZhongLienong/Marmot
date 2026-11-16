#include "Lowering.h"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <utility>

namespace
{
	using TypeRef = std::shared_ptr<MarmotType>;

	std::optional<MarmotIRScalar> ScalarOf(const TypeRef& type)
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
		return std::nullopt;
	}

	// One op per scalar type, in the order Int, Float, Byte, Word, Bool, Text;
	// nothing where the operator does not apply to that type.
	using ScalarOps = std::array<std::optional<MarmotIROp>, 6u>;

	std::optional<MarmotIROp> SelectOp(const ScalarOps& ops, MarmotIRScalar scalar)
	{
		return ops[static_cast<size_t>(scalar)];
	}

	std::optional<ScalarOps> BinaryOps(Token::Name op)
	{
		using enum MarmotIROp;
		switch (op)
		{
		case Token::Name::SINGLE_PLUS:
			return ScalarOps{ AddInt, AddFloat, AddByte, AddWord, std::nullopt, std::nullopt };
		case Token::Name::SINGLE_MINUS:
			return ScalarOps{ SubInt, SubFloat, SubByte, SubWord, std::nullopt, std::nullopt };
		case Token::Name::STAR:
			return ScalarOps{ MulInt, MulFloat, MulByte, MulWord, std::nullopt, std::nullopt };
		case Token::Name::SLASH:
			return ScalarOps{ DivInt, DivFloat, DivByte, DivWord, std::nullopt, std::nullopt };
		case Token::Name::PERCENT:
			return ScalarOps{ ModInt, ModFloat, ModByte, ModWord, std::nullopt, std::nullopt };
		case Token::Name::LEFT_SHIFT:
			return ScalarOps{ ShlInt, std::nullopt, ShlByte, ShlWord, std::nullopt, std::nullopt };
		case Token::Name::RIGHT_SHIFT:
			return ScalarOps{ ShrInt, std::nullopt, ShrByte, ShrWord, std::nullopt, std::nullopt };
		case Token::Name::SINGLE_AMPERSAND:
			return ScalarOps{ BitAndInt, std::nullopt, BitAndByte, BitAndWord, std::nullopt, std::nullopt };
		case Token::Name::SINGLE_BAR:
			return ScalarOps{ BitOrInt, std::nullopt, BitOrByte, BitOrWord, std::nullopt, std::nullopt };
		case Token::Name::CARET:
			return ScalarOps{ BitXorInt, std::nullopt, BitXorByte, BitXorWord, std::nullopt, std::nullopt };
		case Token::Name::LEFT_ANGLE:
			return ScalarOps{ LtInt, LtFloat, LtByte, LtWord, std::nullopt, std::nullopt };
		case Token::Name::LESS_EQUAL:
			return ScalarOps{ LeInt, LeFloat, LeByte, LeWord, std::nullopt, std::nullopt };
		case Token::Name::RIGHT_ANGLE:
			return ScalarOps{ GtInt, GtFloat, GtByte, GtWord, std::nullopt, std::nullopt };
		case Token::Name::GREATER_EQUAL:
			return ScalarOps{ GeInt, GeFloat, GeByte, GeWord, std::nullopt, std::nullopt };
		case Token::Name::DOUBLE_EQUAL:
			return ScalarOps{ EqInt, EqFloat, EqByte, EqWord, EqBool, EqText };
		case Token::Name::BANG_EQUAL:
			return ScalarOps{ NeInt, NeFloat, NeByte, NeWord, NeBool, NeText };
		default:
			return std::nullopt;
		}
	}

	using Conversion = std::pair<std::pair<MarmotIRScalar, MarmotIRScalar>, std::vector<MarmotIROp>>;

	// A conversion between two scalar types, as the ops that make it; empty
	// when there is none.
	std::vector<MarmotIROp> ConversionOps(MarmotIRScalar from, MarmotIRScalar to)
	{
		using enum MarmotIROp;
		using enum MarmotIRScalar;
		const std::pair<MarmotIRScalar, MarmotIRScalar> key{ from, to };
		static const std::array<Conversion, 18u> s_conversions =
		{{
			{ { Int, Float }, { IntToFloat } },
			{ { Text, Float }, { TextToFloat } },
			{ { Byte, Float }, { ByteToFloat } },
			{ { Word, Float }, { WordToFloat } },
			{ { Float, Int }, { FloatToInt } },
			{ { Text, Int }, { TextToInt } },
			{ { Byte, Int }, { ByteToInt } },
			{ { Word, Int }, { WordToInt } },
			{ { Int, Byte }, { IntToByte } },
			{ { Word, Byte }, { WordToByte } },
			{ { Float, Byte }, { FloatToByte } },
			{ { Int, Word }, { IntToWord } },
			{ { Byte, Word }, { ByteToWord } },
			{ { Float, Word }, { FloatToWord } },
			{ { Float, Text }, { FloatToText } },
			{ { Int, Text }, { IntToText } },
			{ { Byte, Text }, { ByteToInt, IntToText } },
			{ { Word, Text }, { WordToText } }
		}};

		const std::array<Conversion, 18u>::const_iterator found = std::ranges::find(s_conversions, key, &Conversion::first);
		return found == s_conversions.cend() ? std::vector<MarmotIROp>{} : found->second;
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

Lowering::Lowered Lowering::Lower(const MarmotExpression& expression)
{
	struct ExpressionLowering
	{
		Lowering& m_self;

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

		Lowered operator()(const MarmotExpression::Send& send) const
		{
			return m_self.LowerOperation(MarmotIROp::Send, m_self.Concrete(send.m_type_data), { send.m_channel.get(), send.m_value.get() }, {}, send.m_arrow.m_line);
		}

		Lowered operator()(const MarmotExpression::Receive& receive) const
		{
			return m_self.LowerOperation(MarmotIROp::Receive, m_self.Concrete(receive.m_type_data), { receive.m_channel.get() }, {}, receive.m_arrow.m_line);
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

std::optional<MarmotIRImmediate> Lowering::ConstantOf(MarmotLiteralKind kind, const std::string& lexeme) const
{
	switch (kind)
	{
	case MarmotLiteralKind::Text:
		return MarmotIRImmediate(lexeme);
	case MarmotLiteralKind::Bool:
		return MarmotIRImmediate(lexeme == Token::Spelling(Token::Name::TRUE));
	case MarmotLiteralKind::Float:
		return ParseFloatLiteral(lexeme).transform([](double value) { return MarmotIRImmediate(value); });
	case MarmotLiteralKind::Unit:
		return MarmotIRImmediate();
	case MarmotLiteralKind::Integer:
		return ParseIntegerLiteral(lexeme).transform([](int64_t value) { return MarmotIRImmediate(value); });
	case MarmotLiteralKind::Byte:
	{
		const std::optional<uint64_t> value = ParseUnsignedLiteral(lexeme);
		if (!value.has_value() || value.value() > 0xFFu)
		{
			return std::nullopt;
		}
		return MarmotIRImmediate(MarmotIRByte{ static_cast<uint8_t>(value.value()) });
	}
	case MarmotLiteralKind::Word:
		return ParseUnsignedLiteral(lexeme).transform([](uint64_t value) { return MarmotIRImmediate(MarmotIRWord{ value }); });
	}
	std::unreachable();
}

Lowering::Lowered Lowering::LowerLiteral(const MarmotExpression::Literal& literal)
{
	const Token& token = literal.m_token;
	const std::optional<MarmotIRImmediate> constant = ConstantOf(literal.m_kind, token.m_lexeme);
	if (!constant.has_value())
	{
		switch (literal.m_kind)
		{
		case MarmotLiteralKind::Integer:
			return std::unexpected(Error(CompilerErrorCode::None, "Integer literal '" + token.m_lexeme + "' is out of range. Maximum value is 9223372036854775807 (2^63 - 1), minimum value is -9223372036854775808 (-2^63).", token));
		case MarmotLiteralKind::Byte:
			return std::unexpected(Error(CompilerErrorCode::None, "Byte literal '" + token.m_lexeme + "' is out of range. Maximum value is 255 (0xFF).", token));
		default:
			return std::unexpected(Error(CompilerErrorCode::None, "Word literal '" + token.m_lexeme + "' is out of range. Maximum value is 18446744073709551615 (0xFFFFFFFFFFFFFFFF).", token));
		}
	}

	static constexpr std::array<MarmotIRScalar, 6u> s_scalars = { MarmotIRScalar::Bool, MarmotIRScalar::Int, MarmotIRScalar::Byte, MarmotIRScalar::Word, MarmotIRScalar::Float, MarmotIRScalar::Text };
	const TypeRef& type = literal.m_kind == MarmotLiteralKind::Unit ? UnitType() : MarmotIRScalarType(s_scalars[static_cast<size_t>(literal.m_kind)]);
	return Builder().AtLine(token.m_line).Emit(MarmotIROp::Const, type, {}, constant.value());
}

Lowering::Lowered Lowering::LowerBinary(const MarmotExpression::Binary& binary)
{
	const Token& op = binary.m_op;
	if (op.m_token_name == Token::Name::DOUBLE_AMPERSAND || op.m_token_name == Token::Name::DOUBLE_BAR)
	{
		return LowerShortCircuit(binary);
	}

	return Lower(*binary.m_left)
		.and_then([this, &binary](MarmotIRValueId left)
		{
			return Lower(*binary.m_right)
				.transform([left](MarmotIRValueId right) { return std::pair{ left, right }; });
		})
		.and_then([this, &binary, &op](std::pair<MarmotIRValueId, MarmotIRValueId> operands) -> Lowered
		{
			const auto [left, right] = operands;
			if (IsDead())
			{
				return Placeholder(Concrete(binary.m_type_data));
			}
			if (op.m_token_name == Token::Name::COLON_EQUAL)
			{
				Builder().AtLine(op.m_line).Emit(MarmotIROp::CellWrite, Scope().m_function.TypeOf(right), { left, right });
				return Builder().ConstUnit();
			}
			if (binary.m_uses_concatenable || binary.m_uses_equatable || binary.m_uses_orderable)
			{
				return LowerClassOperator(binary, left, right);
			}
			if (op.m_token_name == Token::Name::DOUBLE_PLUS)
			{
				return LowerConcat(binary, left, right);
			}

			const TypeRef& operand_type = Scope().m_function.TypeOf(left);
			const std::optional<MarmotIRScalar> scalar = ScalarOf(operand_type);
			const std::optional<ScalarOps> ops = BinaryOps(op.m_token_name);
			const std::optional<MarmotIROp> ir_op = (scalar.has_value() && ops.has_value()) ? SelectOp(ops.value(), scalar.value()) : std::nullopt;
			if (!ir_op.has_value())
			{
				return std::unexpected(Unsupported(std::format("'{}' on {}", op.m_lexeme, operand_type->DisplayString()), op));
			}

			Builder().AtLine(op.m_line);
			if (GetMarmotIROpInfo(ir_op.value()).m_arity == MarmotIRArity::Binary)
			{
				return Builder().Binary(ir_op.value(), left, right);
			}
			return Builder().Emit(ir_op.value(), MarmotIRScalarType(scalar.value()), { left, right });
		});
}

// `a && b` is `b` when `a` holds and `a` otherwise; `a || b` the other way.
Lowering::Lowered Lowering::LowerShortCircuit(const MarmotExpression::Binary& binary)
{
	const bool is_and = binary.m_op.m_token_name == Token::Name::DOUBLE_AMPERSAND;
	return Lower(*binary.m_left)
		.and_then([this, &binary, is_and](MarmotIRValueId left) -> Lowered
		{
			const MarmotIRBlockId right_block = NewBlock();
			const MarmotIRBlockId join = NewBlock();
			const MarmotIRValueId result = Builder().AddParameter(join, MarmotIRScalarType(MarmotIRScalar::Bool));
			const MarmotIRSuccessor evaluate_right(right_block);
			const MarmotIRSuccessor skip_right(join, { left });
			Builder().AtLine(binary.m_op.m_line);
			Branch(left, is_and ? evaluate_right : skip_right, is_and ? skip_right : evaluate_right);

			PositionAt(right_block);
			return Lower(*binary.m_right)
				.transform([this, join, result](MarmotIRValueId right)
				{
					Jump(join, { right });
					PositionAt(join);
					return result;
				});
		});
}

// The operators a class provides call the instance method they resolve to,
// keyed by the operand's own type: a newtype selects its own instance.
Lowering::Lowered Lowering::LowerClassOperator(const MarmotExpression::Binary& binary, MarmotIRValueId left, MarmotIRValueId right)
{
	const Token& op = binary.m_op;
	const TypeRef operand_type = NominalType(*binary.m_left, left);
	const std::vector<TypeRef> argument_types = { operand_type, NominalType(*binary.m_right, right) };
	const MethodResolutionMap& methods = Scope().m_specialization.m_methods;
	const TypeRef& bool_type = MarmotIRScalarType(MarmotIRScalar::Bool);

	if (binary.m_uses_concatenable)
	{
		const MethodResolution<ResolvedInstanceName> resolved = m_resolver.ResolveConcat(methods, operand_type);
		if (!resolved.has_value())
		{
			return std::unexpected(ResolutionError(resolved.error(), op));
		}
		return CallResolved(resolved.value(), { left, right }, argument_types, operand_type, op);
	}

	if (binary.m_uses_equatable)
	{
		const MethodResolution<ResolvedInstanceName> resolved = m_resolver.ResolveEquals(operand_type);
		if (!resolved.has_value())
		{
			return std::unexpected(ResolutionError(resolved.error(), op));
		}
		return CallResolved(resolved.value(), { left, right }, argument_types, bool_type, op)
			.transform([this, &op](MarmotIRValueId equal)
			{
				return op.m_token_name == Token::Name::BANG_EQUAL ? Builder().AtLine(op.m_line).Unary(MarmotIROp::NotBool, equal) : equal;
			});
	}

	const MethodResolution<ResolvedInstanceName> resolved = m_resolver.ResolveCompare(operand_type);
	if (!resolved.has_value())
	{
		return std::unexpected(ResolutionError(resolved.error(), op));
	}
	const MarmotIROp compare = SelectOp(BinaryOps(op.m_token_name).value(), MarmotIRScalar::Int).value();
	return CallResolved(resolved.value(), { left, right }, argument_types, IntType(), op)
		.transform([this, &op, compare](MarmotIRValueId ordering)
		{
			Builder().AtLine(op.m_line);
			return Builder().Binary(compare, ordering, Builder().ConstInt(0));
		});
}

Lowering::Lowered Lowering::LowerConcat(const MarmotExpression::Binary& binary, MarmotIRValueId left, MarmotIRValueId right)
{
	const TypeRef type = GenericTypes::RepresentationOf(Scope().m_function.TypeOf(left));
	if (!type->IsType<MarmotType::TextType>() && !type->IsType<MarmotType::ArrayType>())
	{
		return std::unexpected(Error(CompilerErrorCode::None, std::format("Concatenation operator '++' requires Text or Array type (got {})", type->ToString()), binary.m_op));
	}
	const MarmotIROp op = IsFreshConcatTemporary(*binary.m_left) ? MarmotIROp::Extend : MarmotIROp::Concat;
	return Builder().AtLine(binary.m_op.m_line).Emit(op, type, { left, right });
}

Lowering::Lowered Lowering::LowerUnary(const MarmotExpression::UnaryPrefix& unary)
{
	const Token& op = unary.m_op;
	if (op.m_token_name == Token::Name::SINGLE_PLUS)
	{
		return Lower(*unary.m_expr);
	}

	return Lower(*unary.m_expr)
		.and_then([this, &unary, &op](MarmotIRValueId operand) -> Lowered
		{
			if (IsDead())
			{
				return Placeholder(Concrete(unary.m_type_data));
			}
			const TypeRef& operand_type = Scope().m_function.TypeOf(operand);
			if (op.m_token_name == Token::Name::REF || op.m_token_name == Token::Name::STAR)
			{
				const MarmotIROp cell_op = op.m_token_name == Token::Name::REF ? MarmotIROp::CellNew : MarmotIROp::CellRead;
				return Builder().AtLine(op.m_line).Emit(cell_op, Concrete(unary.m_type_data), { operand });
			}
			if (op.m_token_name == Token::Name::HASH)
			{
				if (GenericTypes::RepresentationOf(operand_type)->IsType<MarmotType::ArrayType>())
				{
					return Builder().AtLine(op.m_line).Emit(MarmotIROp::ArrayLength, IntType(), { operand });
				}
				return LowerCount(unary, operand);
			}

			const std::optional<MarmotIRScalar> scalar = ScalarOf(operand_type);
			const std::optional<MarmotIROp> ir_op = [&]() -> std::optional<MarmotIROp>
			{
				if (!scalar.has_value())
				{
					return std::nullopt;
				}
				using enum MarmotIROp;
				switch (op.m_token_name)
				{
				case Token::Name::SINGLE_MINUS:
					return SelectOp(ScalarOps{ NegInt, NegFloat, std::nullopt, std::nullopt, std::nullopt, std::nullopt }, scalar.value());
				case Token::Name::BANG:
					return SelectOp(ScalarOps{ std::nullopt, std::nullopt, std::nullopt, std::nullopt, NotBool, std::nullopt }, scalar.value());
				case Token::Name::TILDE:
					return SelectOp(ScalarOps{ BitNotInt, std::nullopt, BitNotByte, BitNotWord, std::nullopt, std::nullopt }, scalar.value());
				default:
					return std::nullopt;
				}
			}();
			if (!ir_op.has_value())
			{
				return std::unexpected(Unsupported(std::format("'{}' on {}", op.m_lexeme, operand_type->DisplayString()), op));
			}
			return Builder().AtLine(op.m_line).Unary(ir_op.value(), operand);
		});
}

Lowering::Lowered Lowering::LowerCount(const MarmotExpression::UnaryPrefix& unary, MarmotIRValueId operand)
{
	const TypeRef operand_type = NominalType(*unary.m_expr, operand);
	const MethodResolution<std::optional<ResolvedInstanceName>> resolved = m_resolver.ResolveCount(Scope().m_specialization.m_methods, operand_type, unary.m_uses_countable);
	if (!resolved.has_value())
	{
		return std::unexpected(ResolutionError(resolved.error(), unary.m_op));
	}
	if (!resolved.value().has_value())
	{
		return std::unexpected(Unsupported(std::format("'#' on {}", operand_type->DisplayString()), unary.m_op));
	}
	return CallResolved(resolved.value().value(), { operand }, { operand_type }, IntType(), unary.m_op);
}

// An instance a program writes wins over a builtin conversion; a conversion
// between a newtype and its representation is no instruction at all.
Lowering::Lowered Lowering::LowerAs(const MarmotExpression::As& as)
{
	const Token& keyword = as.m_as_keyword;
	const TypeRef to_type = Concrete(as.m_to_type);
	return Lower(*as.m_expr)
		.and_then([this, &as, &keyword, &to_type](MarmotIRValueId value) -> Lowered
		{
			if (IsDead())
			{
				return Placeholder(to_type);
			}

			const TypeRef from_type = NominalType(*as.m_expr, value);
			const MethodResolution<std::optional<ResolvedInstanceName>> resolved = m_resolver.ResolveConvert(Scope().m_specialization.m_methods, from_type, from_type, to_type, as.m_uses_convertable);
			if (!resolved.has_value())
			{
				return std::unexpected(ResolutionError(resolved.error(), keyword));
			}
			if (resolved.value().has_value())
			{
				return CallResolved(resolved.value().value(), { value }, { from_type }, to_type, keyword);
			}
			if (MarmotIRSameType(from_type, to_type))
			{
				return value;
			}

			Builder().AtLine(keyword.m_line);
			if (to_type->IsType<MarmotType::UnitType>())
			{
				return Builder().ConstUnit();
			}

			const std::optional<MarmotIRScalar> from = ScalarOf(from_type);
			const std::optional<MarmotIRScalar> to = ScalarOf(to_type);
			if (from == MarmotIRScalar::Bool && to == MarmotIRScalar::Text)
			{
				return LowerBoolToText(value, keyword.m_line);
			}

			const std::vector<MarmotIROp> ops = (from.has_value() && to.has_value()) ? ConversionOps(from.value(), to.value()) : std::vector<MarmotIROp>{};
			if (ops.empty())
			{
				return std::unexpected(Unsupported(std::format("a conversion from {} to {}", from_type->DisplayString(), to_type->DisplayString()), keyword));
			}
			return std::ranges::fold_left(ops, value, [this](MarmotIRValueId operand, MarmotIROp op) { return Builder().Unary(op, operand); });
		});
}

Lowering::Lowered Lowering::LowerBoolToText(MarmotIRValueId value, int line)
{
	const MarmotIRBlockId when_true = NewBlock();
	const MarmotIRBlockId when_false = NewBlock();
	const MarmotIRBlockId join = NewBlock();
	const MarmotIRValueId text = Builder().AddParameter(join, MarmotIRScalarType(MarmotIRScalar::Text));
	Builder().AtLine(line);
	Branch(value, MarmotIRSuccessor(when_true), MarmotIRSuccessor(when_false));

	PositionAt(when_true);
	Jump(join, { Builder().ConstText(std::string(Token::Spelling(Token::Name::TRUE))) });
	PositionAt(when_false);
	Jump(join, { Builder().ConstText(std::string(Token::Spelling(Token::Name::FALSE))) });
	PositionAt(join);
	return text;
}

Lowering::Lowered Lowering::LowerOperation(MarmotIROp op, const TypeRef& type, std::vector<const MarmotExpression*> operands, MarmotIRImmediate immediate, int line)
{
	std::vector<MarmotIRValueId> values;
	values.reserve(operands.size());
	for (const MarmotExpression* operand : operands)
	{
		const Lowered value = Lower(*operand);
		if (!value.has_value())
		{
			return value;
		}
		values.push_back(value.value());
	}
	if (IsDead())
	{
		return Placeholder(type);
	}
	return Builder().AtLine(line).Emit(op, type, std::move(values), std::move(immediate));
}

Lowering::Lowered Lowering::LowerTuple(const MarmotExpression::Tuple& tuple)
{
	std::vector<const MarmotExpression*> elements = tuple.m_elements
		| std::views::transform([](const std::unique_ptr<MarmotExpression>& element) -> const MarmotExpression* { return element.get(); })
		| std::ranges::to<std::vector>();
	return LowerOperation(MarmotIROp::MakeTuple, Concrete(tuple.m_type_data), std::move(elements), {}, tuple.m_op.m_line);
}

Lowering::Lowered Lowering::LowerArray(const MarmotExpression::Array& array)
{
	std::vector<const MarmotExpression*> elements = array.m_elems
		| std::views::transform([](const std::unique_ptr<MarmotExpression>& element) -> const MarmotExpression* { return element.get(); })
		| std::ranges::to<std::vector>();
	return LowerOperation(MarmotIROp::MakeArray, Concrete(array.m_type_data), std::move(elements), {}, array.m_op.m_line);
}

// An array is indexed by the instruction. The array fast path is load-bearing:
// the prelude's Indexable<Array<T>, Int> instance indexes an array in its
// body, so sending arrays through the instance would make it call itself.
Lowering::Lowered Lowering::LowerIndex(const MarmotExpression::IndexAccess& index)
{
	const Token& op = index.m_op;
	const TypeRef result_type = Concrete(index.m_type_data);
	return Lower(*index.m_arr_var)
		.and_then([this, &index](MarmotIRValueId container)
		{
			return Lower(*index.m_index)
				.transform([container](MarmotIRValueId position) { return std::pair{ container, position }; });
		})
		.and_then([this, &index, &op, &result_type](std::pair<MarmotIRValueId, MarmotIRValueId> operands) -> Lowered
		{
			const auto [container, position] = operands;
			if (IsDead())
			{
				return Placeholder(result_type);
			}
			const TypeRef container_type = Scope().m_function.TypeOf(container);
			if (index.m_uses_indexable)
			{
				const std::vector<TypeRef> argument_types = { NominalType(*index.m_arr_var, container), NominalType(*index.m_index, position) };
				const MethodResolution<ResolvedInstanceName> resolved = m_resolver.ResolveIndex(Scope().m_specialization.m_methods, argument_types[0u], argument_types[1u]);
				if (!resolved.has_value())
				{
					return std::unexpected(ResolutionError(resolved.error(), op));
				}
				return CallResolved(resolved.value(), { container, position }, argument_types, result_type, op);
			}
			const TypeRef& element_type = GenericTypes::RepresentationOf(container_type)->GetType<MarmotType::ArrayType>().m_element_type;
			return Builder().AtLine(op.m_line).Emit(MarmotIROp::ArrayGet, element_type, { container, position });
		});
}

Lowering::Lowered Lowering::LowerConstruct(const MarmotExpression::Construct& construct)
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
Lowering::Lowered Lowering::LowerRecordUpdate(const MarmotExpression::RecordUpdate& record_update)
{
	const int line = record_update.m_with_keyword.m_line;
	const TypeRef type = Concrete(record_update.m_type_data);
	const std::vector<const MarmotExpression::RecordUpdate::FieldUpdate*> updates = record_update.m_updates
		| std::views::transform([](const MarmotExpression::RecordUpdate::FieldUpdate& update) { return &update; })
		| std::ranges::to<std::vector>();
	return Lower(*record_update.m_source)
		.and_then([&](MarmotIRValueId source) -> Lowered
		{
			if (IsDead())
			{
				return Placeholder(type);
			}
			return LowerUpdatedRecord(source, type, updates, 0u, line);
		});
}

// `source` rebuilt with each update's value at the end of its path, a record
// on the way rebuilt from its old members and the updates that pass through it.
Lowering::Lowered Lowering::LowerUpdatedRecord(MarmotIRValueId source, const TypeRef& type, const std::vector<const MarmotExpression::RecordUpdate::FieldUpdate*>& updates, size_t depth, int line)
{
	const std::vector<TypeRef>& member_types = GenericTypes::RepresentationOf(Scope().m_function.TypeOf(source))->GetType<MarmotType::StructType>().m_member_types;
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
			const MarmotIRValueId old_member = Builder().AtLine(line).Emit(MarmotIROp::GetMember, member_types[slot], { source }, MarmotIRIndex{ static_cast<uint32_t>(slot) });
			return here.empty() ? Lowered(old_member) : LowerUpdatedRecord(old_member, member_types[slot], here, depth + 1u, line);
		}();
		if (!value.has_value())
		{
			return value;
		}
		members.push_back(value.value());
	}
	if (IsDead())
	{
		return Placeholder(type);
	}
	return Builder().AtLine(line).Emit(MarmotIROp::Construct, type, std::move(members));
}

Lowering::Lowered Lowering::LowerMemberAccess(const MarmotExpression::MemberAccess& member)
{
	const int line = member.m_member_name.m_line;
	return Lower(*member.m_struct)
		.transform([this, &member, line](MarmotIRValueId record)
		{
			if (IsDead())
			{
				return Placeholder(Concrete(member.m_type_data));
			}
			const std::vector<TypeRef>& member_types = GenericTypes::RepresentationOf(Scope().m_function.TypeOf(record))->GetType<MarmotType::StructType>().m_member_types;
			const uint32_t index = static_cast<uint32_t>(member.m_index);
			return Builder().AtLine(line).Emit(MarmotIROp::GetMember, member_types[index], { record }, MarmotIRIndex{ index });
		});
}

// A range without a step steps by one.
Lowering::Lowered Lowering::LowerRange(const MarmotExpression& start, const MarmotExpression* step, const MarmotExpression& end, const TypeRef& range_type, int line)
{
	const bool is_float = range_type->GetType<MarmotType::RangeType>().m_element_type->IsType<MarmotType::FloatType>();
	return Lower(start)
		.and_then([&](MarmotIRValueId first) -> LoweredValues
		{
			const Lowered stride = step == nullptr
				? Lowered(is_float ? Builder().AtLine(line).ConstFloat(1.0) : Builder().AtLine(line).ConstInt(1))
				: Lower(*step);
			return stride.and_then([&](MarmotIRValueId by)
			{
				return Lower(end).transform([first, by](MarmotIRValueId last) { return std::vector<MarmotIRValueId>{ first, by, last }; });
			});
		})
		.transform([&](std::vector<MarmotIRValueId> bounds)
		{
			if (IsDead())
			{
				return Placeholder(range_type);
			}
			return Builder().AtLine(line).Emit(MarmotIROp::MakeRange, range_type, std::move(bounds));
		});
}

// The one argument stands for all of the function's parameters: the value
// itself for one, a tuple spread across them for several, and a discarded
// Unit for none. The function is evaluated after it.
Lowering::Lowered Lowering::LowerSpawn(const MarmotExpression::Call& spawn)
{
	const int line = spawn.m_paren.m_line;
	const TypeRef type = Concrete(spawn.m_type_data);
	const MarmotExpression& function = *spawn.m_arguments[1u];
	const size_t arity = Concrete(function.GetType())->GetType<MarmotType::FunctionType>().m_param_types.size();
	return Lower(*spawn.m_arguments[0u])
		.and_then([&](MarmotIRValueId argument) -> Lowered
		{
			std::vector<MarmotIRValueId> operands;
			if (arity == 1u)
			{
				operands.push_back(argument);
			}
			else if (arity >= 2u && !IsDead())
			{
				const std::vector<TypeRef>& element_types = GenericTypes::RepresentationOf(Scope().m_function.TypeOf(argument))->GetType<MarmotType::TupleType>().m_element_types;
				for (uint32_t index = 0u; index < static_cast<uint32_t>(arity); index += 1u)
				{
					operands.push_back(Builder().AtLine(line).Emit(MarmotIROp::TupleGet, element_types[index], { argument }, MarmotIRIndex{ index }));
				}
			}

			return Lower(function)
				.transform([&](MarmotIRValueId callee)
				{
					if (IsDead())
					{
						return Placeholder(type);
					}
					operands.push_back(callee);
					return Builder().AtLine(line).Emit(MarmotIROp::Spawn, type, std::move(operands));
				});
		});
}
