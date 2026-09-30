#include "Lowering.h"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <utility>

namespace
{
	using TypeRef = std::shared_ptr<MidoriType>;

	std::optional<MidoriIRScalar> ScalarOf(const TypeRef& type)
	{
		const TypeRef representation = GenericTypes::RepresentationOf(type);
		if (representation->IsType<MidoriType::IntegerType>())
		{
			return MidoriIRScalar::Int;
		}
		if (representation->IsType<MidoriType::FloatType>())
		{
			return MidoriIRScalar::Float;
		}
		if (representation->IsType<MidoriType::ByteType>())
		{
			return MidoriIRScalar::Byte;
		}
		if (representation->IsType<MidoriType::WordType>())
		{
			return MidoriIRScalar::Word;
		}
		if (representation->IsType<MidoriType::BoolType>())
		{
			return MidoriIRScalar::Bool;
		}
		if (representation->IsType<MidoriType::TextType>())
		{
			return MidoriIRScalar::Text;
		}
		return std::nullopt;
	}

	// One op per scalar type, in the order Int, Float, Byte, Word, Bool, Text;
	// nothing where the operator does not apply to that type.
	using ScalarOps = std::array<std::optional<MidoriIROp>, 6u>;

	std::optional<MidoriIROp> SelectOp(const ScalarOps& ops, MidoriIRScalar scalar)
	{
		return ops[static_cast<size_t>(scalar)];
	}

	std::optional<ScalarOps> BinaryOps(Token::Name op)
	{
		using enum MidoriIROp;
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

	using Conversion = std::pair<std::pair<MidoriIRScalar, MidoriIRScalar>, std::vector<MidoriIROp>>;

	// A conversion between two scalar types, as the ops that make it; empty
	// when there is none.
	std::vector<MidoriIROp> ConversionOps(MidoriIRScalar from, MidoriIRScalar to)
	{
		using enum MidoriIROp;
		using enum MidoriIRScalar;
		const std::pair<MidoriIRScalar, MidoriIRScalar> key{ from, to };
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
		return found == s_conversions.cend() ? std::vector<MidoriIROp>{} : found->second;
	}

	const TypeRef& UnitType()
	{
		return MidoriType::MakeLiteralType<MidoriType::UnitType>();
	}

	const TypeRef& IntType()
	{
		return MidoriType::MakeLiteralType<MidoriType::IntegerType>();
	}

	// An operand the `++` may extend in place, because nothing else holds it:
	// another `++` a class does not provide, or an array literal or
	// comprehension. Not a text literal: every load of one pushes the same
	// cached text.
	bool IsFreshConcatTemporary(const MidoriExpression& expression)
	{
		if (expression.IsExpression<MidoriExpression::Binary>())
		{
			const MidoriExpression::Binary& binary = expression.GetExpression<MidoriExpression::Binary>();
			return binary.m_op.m_token_name == Token::Name::DOUBLE_PLUS && !binary.m_uses_concatenable;
		}

		return expression.IsExpression<MidoriExpression::Array>()
			|| expression.IsExpression<MidoriExpression::ArrayComprehension>();
	}
}

Lowering::Lowered Lowering::Lower(MidoriExpression& expression)
{
	struct ExpressionLowering
	{
		Lowering& m_self;

		Lowered operator()(MidoriExpression::Literal& literal) const
		{
			return m_self.LowerLiteral(literal);
		}

		Lowered operator()(MidoriExpression::Group& group) const
		{
			return m_self.Lower(*group.m_expr_in);
		}

		Lowered operator()(MidoriExpression::Binary& binary) const
		{
			return m_self.LowerBinary(binary);
		}

		Lowered operator()(MidoriExpression::UnaryPrefix& unary) const
		{
			return m_self.LowerUnary(unary);
		}

		Lowered operator()(MidoriExpression::As& as) const
		{
			return m_self.LowerAs(as);
		}

		Lowered operator()(MidoriExpression::NameAccess& name) const
		{
			return m_self.LowerName(name);
		}

		Lowered operator()(MidoriExpression::IfElse& if_else) const
		{
			return m_self.LowerIf(if_else);
		}

		Lowered operator()(MidoriExpression::Block& block) const
		{
			return m_self.LowerBlock(block);
		}

		Lowered operator()(MidoriExpression::Function& function) const
		{
			return m_self.LowerFunction(function);
		}

		Lowered operator()(MidoriExpression::Call& call) const
		{
			return m_self.LowerCall(call);
		}

		Lowered operator()(MidoriExpression::UnarySuffix& unary) const
		{
			return std::unexpected(m_self.Unsupported("a suffix operator", unary.m_op));
		}

		Lowered operator()(MidoriExpression::Tuple& tuple) const
		{
			return m_self.LowerTuple(tuple);
		}

		Lowered operator()(MidoriExpression::Spawn& spawn) const
		{
			return m_self.LowerSpawn(spawn);
		}

		Lowered operator()(MidoriExpression::Join& join) const
		{
			return m_self.LowerJoin(join);
		}

		Lowered operator()(MidoriExpression::ChannelCreate& channel) const
		{
			return m_self.LowerOperation(MidoriIROp::ChannelNew, m_self.Concrete(channel.m_type_data), { channel.m_capacity.get() }, {}, channel.m_channel_keyword.m_line);
		}

		Lowered operator()(MidoriExpression::Send& send) const
		{
			return m_self.LowerOperation(MidoriIROp::Send, m_self.Concrete(send.m_type_data), { send.m_channel.get(), send.m_value.get() }, {}, send.m_arrow.m_line);
		}

		Lowered operator()(MidoriExpression::Receive& receive) const
		{
			return m_self.LowerOperation(MidoriIROp::Receive, m_self.Concrete(receive.m_type_data), { receive.m_channel.get() }, {}, receive.m_arrow.m_line);
		}

		Lowered operator()(MidoriExpression::Construct& construct) const
		{
			return m_self.LowerConstruct(construct);
		}

		Lowered operator()(MidoriExpression::RecordUpdate& record_update) const
		{
			return m_self.LowerRecordUpdate(record_update);
		}

		Lowered operator()(MidoriExpression::MemberAccess& member) const
		{
			return m_self.LowerMemberAccess(member);
		}

		Lowered operator()(MidoriExpression::Array& array) const
		{
			return m_self.LowerArray(array);
		}

		Lowered operator()(MidoriExpression::IndexAccess& index) const
		{
			return m_self.LowerIndex(index);
		}

		Lowered operator()(MidoriExpression::ArrayComprehension& comprehension) const
		{
			return m_self.LowerComprehension(comprehension);
		}

		Lowered operator()(MidoriExpression::RangeBinary& range) const
		{
			return m_self.LowerRange(*range.m_start, nullptr, *range.m_end, m_self.Concrete(range.m_type_data), range.m_range_op.m_line);
		}

		Lowered operator()(MidoriExpression::RangeTernary& range) const
		{
			return m_self.LowerRange(*range.m_start, range.m_step.get(), *range.m_end, m_self.Concrete(range.m_type_data), range.m_first_range_op.m_line);
		}

		Lowered operator()(MidoriExpression::Match& match) const
		{
			return m_self.LowerMatch(match);
		}

		// Only a match holds its cases.
		Lowered operator()(MidoriExpression::Case&) const
		{
			std::unreachable();
		}

		Lowered operator()(MidoriExpression::For& for_expression) const
		{
			return m_self.LowerFor(for_expression);
		}
	};

	return VisitNode(ExpressionLowering{ *this }, expression);
}

std::optional<MidoriIRImmediate> Lowering::ConstantOf(MidoriLiteralKind kind, const std::string& lexeme) const
{
	switch (kind)
	{
	case MidoriLiteralKind::Text:
		return MidoriIRImmediate(lexeme);
	case MidoriLiteralKind::Bool:
		return MidoriIRImmediate(lexeme == "true");
	case MidoriLiteralKind::Float:
		return ParseFloatLiteral(lexeme).transform([](double value) { return MidoriIRImmediate(value); });
	case MidoriLiteralKind::Unit:
		return MidoriIRImmediate();
	case MidoriLiteralKind::Integer:
		return ParseIntegerLiteral(lexeme).transform([](int64_t value) { return MidoriIRImmediate(value); });
	case MidoriLiteralKind::Byte:
	{
		const std::optional<uint64_t> value = ParseUnsignedLiteral(lexeme);
		if (!value.has_value() || value.value() > 0xFFu)
		{
			return std::nullopt;
		}
		return MidoriIRImmediate(MidoriIRByte{ static_cast<uint8_t>(value.value()) });
	}
	case MidoriLiteralKind::Word:
		return ParseUnsignedLiteral(lexeme).transform([](uint64_t value) { return MidoriIRImmediate(MidoriIRWord{ value }); });
	}
	std::unreachable();
}

Lowering::Lowered Lowering::LowerLiteral(const MidoriExpression::Literal& literal)
{
	const Token& token = literal.m_token;
	const std::optional<MidoriIRImmediate> constant = ConstantOf(literal.m_kind, token.m_lexeme);
	if (!constant.has_value())
	{
		switch (literal.m_kind)
		{
		case MidoriLiteralKind::Integer:
			return std::unexpected(Error(CompilerErrorCode::None, "Integer literal '" + token.m_lexeme + "' is out of range. Maximum value is 9223372036854775807 (2^63 - 1), minimum value is -9223372036854775808 (-2^63).", token));
		case MidoriLiteralKind::Byte:
			return std::unexpected(Error(CompilerErrorCode::None, "Byte literal '" + token.m_lexeme + "' is out of range. Maximum value is 255 (0xFF).", token));
		default:
			return std::unexpected(Error(CompilerErrorCode::None, "Word literal '" + token.m_lexeme + "' is out of range. Maximum value is 18446744073709551615 (0xFFFFFFFFFFFFFFFF).", token));
		}
	}

	static constexpr std::array<MidoriIRScalar, 6u> s_scalars = { MidoriIRScalar::Bool, MidoriIRScalar::Int, MidoriIRScalar::Byte, MidoriIRScalar::Word, MidoriIRScalar::Float, MidoriIRScalar::Text };
	const TypeRef& type = literal.m_kind == MidoriLiteralKind::Unit ? UnitType() : MidoriIRScalarType(s_scalars[static_cast<size_t>(literal.m_kind)]);
	return Builder().AtLine(token.m_line).Emit(MidoriIROp::Const, type, {}, constant.value());
}

Lowering::Lowered Lowering::LowerBinary(MidoriExpression::Binary& binary)
{
	const Token& op = binary.m_op;
	if (op.m_token_name == Token::Name::DOUBLE_AMPERSAND || op.m_token_name == Token::Name::DOUBLE_BAR)
	{
		return LowerShortCircuit(binary);
	}

	return Lower(*binary.m_left)
		.and_then([this, &binary](MidoriIRValueId left)
		{
			return Lower(*binary.m_right)
				.transform([left](MidoriIRValueId right) { return std::pair{ left, right }; });
		})
		.and_then([this, &binary, &op](std::pair<MidoriIRValueId, MidoriIRValueId> operands) -> Lowered
		{
			const auto [left, right] = operands;
			if (IsDead())
			{
				return Placeholder(Concrete(binary.m_type_data));
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
			const std::optional<MidoriIRScalar> scalar = ScalarOf(operand_type);
			const std::optional<ScalarOps> ops = BinaryOps(op.m_token_name);
			const std::optional<MidoriIROp> ir_op = (scalar.has_value() && ops.has_value()) ? SelectOp(ops.value(), scalar.value()) : std::nullopt;
			if (!ir_op.has_value())
			{
				return std::unexpected(Unsupported(std::format("'{}' on {}", op.m_lexeme, operand_type->DisplayString()), op));
			}

			Builder().AtLine(op.m_line);
			if (GetMidoriIROpInfo(ir_op.value()).m_arity == MidoriIRArity::Binary)
			{
				return Builder().Binary(ir_op.value(), left, right);
			}
			return Builder().Emit(ir_op.value(), MidoriIRScalarType(scalar.value()), { left, right });
		});
}

// `a && b` is `b` when `a` holds and `a` otherwise; `a || b` the other way.
Lowering::Lowered Lowering::LowerShortCircuit(MidoriExpression::Binary& binary)
{
	const bool is_and = binary.m_op.m_token_name == Token::Name::DOUBLE_AMPERSAND;
	return Lower(*binary.m_left)
		.and_then([this, &binary, is_and](MidoriIRValueId left) -> Lowered
		{
			const MidoriIRBlockId right_block = NewBlock();
			const MidoriIRBlockId join = NewBlock();
			const MidoriIRValueId result = Builder().AddParameter(join, MidoriIRScalarType(MidoriIRScalar::Bool));
			const MidoriIRSuccessor evaluate_right(right_block);
			const MidoriIRSuccessor skip_right(join, { left });
			Builder().AtLine(binary.m_op.m_line);
			Branch(left, is_and ? evaluate_right : skip_right, is_and ? skip_right : evaluate_right);

			PositionAt(right_block);
			return Lower(*binary.m_right)
				.transform([this, join, result](MidoriIRValueId right)
				{
					Jump(join, { right });
					PositionAt(join);
					return result;
				});
		});
}

// The operators a class provides call the instance method they resolve to,
// keyed by the operand's own type: a newtype selects its own instance.
Lowering::Lowered Lowering::LowerClassOperator(MidoriExpression::Binary& binary, MidoriIRValueId left, MidoriIRValueId right)
{
	const Token& op = binary.m_op;
	const TypeRef operand_type = NominalType(*binary.m_left, left);
	const std::vector<TypeRef> argument_types = { operand_type, NominalType(*binary.m_right, right) };
	const MethodResolutionMap& methods = Scope().m_specialization.m_methods;
	const TypeRef& bool_type = MidoriIRScalarType(MidoriIRScalar::Bool);

	if (binary.m_uses_concatenable)
	{
		const MethodResolution<std::string> resolved = m_resolver.ResolveConcat(methods, operand_type);
		if (!resolved.has_value())
		{
			return std::unexpected(ResolutionError(resolved.error(), op));
		}
		return CallResolved(resolved.value(), { left, right }, argument_types, operand_type, op);
	}

	if (binary.m_uses_equatable)
	{
		const MethodResolution<std::string> resolved = m_resolver.ResolveEquals(operand_type);
		if (!resolved.has_value())
		{
			return std::unexpected(ResolutionError(resolved.error(), op));
		}
		return CallResolved(resolved.value(), { left, right }, argument_types, bool_type, op)
			.transform([this, &op](MidoriIRValueId equal)
			{
				return op.m_token_name == Token::Name::BANG_EQUAL ? Builder().AtLine(op.m_line).Unary(MidoriIROp::NotBool, equal) : equal;
			});
	}

	const MethodResolution<std::string> resolved = m_resolver.ResolveCompare(operand_type);
	if (!resolved.has_value())
	{
		return std::unexpected(ResolutionError(resolved.error(), op));
	}
	const MidoriIROp compare = SelectOp(BinaryOps(op.m_token_name).value(), MidoriIRScalar::Int).value();
	return CallResolved(resolved.value(), { left, right }, argument_types, IntType(), op)
		.transform([this, &op, compare](MidoriIRValueId ordering)
		{
			Builder().AtLine(op.m_line);
			return Builder().Binary(compare, ordering, Builder().ConstInt(0));
		});
}

Lowering::Lowered Lowering::LowerConcat(MidoriExpression::Binary& binary, MidoriIRValueId left, MidoriIRValueId right)
{
	const TypeRef type = GenericTypes::RepresentationOf(Scope().m_function.TypeOf(left));
	if (!type->IsType<MidoriType::TextType>() && !type->IsType<MidoriType::ArrayType>())
	{
		return std::unexpected(Error(CompilerErrorCode::None, std::format("Concatenation operator '++' requires Text or Array type (got {})", type->ToString()), binary.m_op));
	}
	const MidoriIROp op = IsFreshConcatTemporary(*binary.m_left) ? MidoriIROp::Extend : MidoriIROp::Concat;
	return Builder().AtLine(binary.m_op.m_line).Emit(op, type, { left, right });
}

Lowering::Lowered Lowering::LowerUnary(MidoriExpression::UnaryPrefix& unary)
{
	const Token& op = unary.m_op;
	if (op.m_token_name == Token::Name::SINGLE_PLUS)
	{
		return Lower(*unary.m_expr);
	}

	return Lower(*unary.m_expr)
		.and_then([this, &unary, &op](MidoriIRValueId operand) -> Lowered
		{
			if (IsDead())
			{
				return Placeholder(Concrete(unary.m_type_data));
			}
			const TypeRef& operand_type = Scope().m_function.TypeOf(operand);
			if (op.m_token_name == Token::Name::HASH)
			{
				if (GenericTypes::RepresentationOf(operand_type)->IsType<MidoriType::ArrayType>())
				{
					return Builder().AtLine(op.m_line).Emit(MidoriIROp::ArrayLength, IntType(), { operand });
				}
				return LowerCount(unary, operand);
			}

			const std::optional<MidoriIRScalar> scalar = ScalarOf(operand_type);
			const std::optional<MidoriIROp> ir_op = [&]() -> std::optional<MidoriIROp>
			{
				if (!scalar.has_value())
				{
					return std::nullopt;
				}
				using enum MidoriIROp;
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

Lowering::Lowered Lowering::LowerCount(MidoriExpression::UnaryPrefix& unary, MidoriIRValueId operand)
{
	const TypeRef operand_type = NominalType(*unary.m_expr, operand);
	const MethodResolution<std::optional<std::string>> resolved = m_resolver.ResolveCount(Scope().m_specialization.m_methods, operand_type, unary.m_uses_countable);
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
Lowering::Lowered Lowering::LowerAs(MidoriExpression::As& as)
{
	const Token& keyword = as.m_as_keyword;
	const TypeRef to_type = Concrete(as.m_to_type);
	return Lower(*as.m_expr)
		.and_then([this, &as, &keyword, &to_type](MidoriIRValueId value) -> Lowered
		{
			if (IsDead())
			{
				return Placeholder(to_type);
			}

			const TypeRef from_type = NominalType(*as.m_expr, value);
			const MethodResolution<std::optional<std::string>> resolved = m_resolver.ResolveConvert(Scope().m_specialization.m_methods, from_type, from_type, to_type, as.m_uses_convertable);
			if (!resolved.has_value())
			{
				return std::unexpected(ResolutionError(resolved.error(), keyword));
			}
			if (resolved.value().has_value())
			{
				return CallResolved(resolved.value().value(), { value }, { from_type }, to_type, keyword);
			}
			if (MidoriIRSameType(from_type, to_type))
			{
				return value;
			}

			Builder().AtLine(keyword.m_line);
			if (to_type->IsType<MidoriType::UnitType>())
			{
				return Builder().ConstUnit();
			}

			const std::optional<MidoriIRScalar> from = ScalarOf(from_type);
			const std::optional<MidoriIRScalar> to = ScalarOf(to_type);
			if (from == MidoriIRScalar::Bool && to == MidoriIRScalar::Text)
			{
				return LowerBoolToText(value, keyword.m_line);
			}

			const std::vector<MidoriIROp> ops = (from.has_value() && to.has_value()) ? ConversionOps(from.value(), to.value()) : std::vector<MidoriIROp>{};
			if (ops.empty())
			{
				return std::unexpected(Unsupported(std::format("a conversion from {} to {}", from_type->DisplayString(), to_type->DisplayString()), keyword));
			}
			return std::ranges::fold_left(ops, value, [this](MidoriIRValueId operand, MidoriIROp op) { return Builder().Unary(op, operand); });
		});
}

Lowering::Lowered Lowering::LowerBoolToText(MidoriIRValueId value, int line)
{
	const MidoriIRBlockId when_true = NewBlock();
	const MidoriIRBlockId when_false = NewBlock();
	const MidoriIRBlockId join = NewBlock();
	const MidoriIRValueId text = Builder().AddParameter(join, MidoriIRScalarType(MidoriIRScalar::Text));
	Builder().AtLine(line);
	Branch(value, MidoriIRSuccessor(when_true), MidoriIRSuccessor(when_false));

	PositionAt(when_true);
	Jump(join, { Builder().ConstText("true") });
	PositionAt(when_false);
	Jump(join, { Builder().ConstText("false") });
	PositionAt(join);
	return text;
}

Lowering::Lowered Lowering::LowerOperation(MidoriIROp op, const TypeRef& type, std::vector<MidoriExpression*> operands, MidoriIRImmediate immediate, int line)
{
	std::vector<MidoriIRValueId> values;
	values.reserve(operands.size());
	for (MidoriExpression* operand : operands)
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

Lowering::Lowered Lowering::LowerTuple(MidoriExpression::Tuple& tuple)
{
	std::vector<MidoriExpression*> elements = tuple.m_elements
		| std::views::transform([](std::unique_ptr<MidoriExpression>& element) { return element.get(); })
		| std::ranges::to<std::vector>();
	return LowerOperation(MidoriIROp::MakeTuple, Concrete(tuple.m_type_data), std::move(elements), {}, tuple.m_op.m_line);
}

Lowering::Lowered Lowering::LowerArray(MidoriExpression::Array& array)
{
	std::vector<MidoriExpression*> elements = array.m_elems
		| std::views::transform([](std::unique_ptr<MidoriExpression>& element) { return element.get(); })
		| std::ranges::to<std::vector>();
	return LowerOperation(MidoriIROp::MakeArray, Concrete(array.m_type_data), std::move(elements), {}, array.m_op.m_line);
}

// An array is indexed by the instruction. The array fast path is load-bearing:
// the prelude's Indexable<Array<T>, Int> instance indexes an array in its
// body, so sending arrays through the instance would make it call itself.
Lowering::Lowered Lowering::LowerIndex(MidoriExpression::IndexAccess& index)
{
	const Token& op = index.m_op;
	const TypeRef result_type = Concrete(index.m_type_data);
	return Lower(*index.m_arr_var)
		.and_then([this, &index](MidoriIRValueId container)
		{
			return Lower(*index.m_index)
				.transform([container](MidoriIRValueId position) { return std::pair{ container, position }; });
		})
		.and_then([this, &index, &op, &result_type](std::pair<MidoriIRValueId, MidoriIRValueId> operands) -> Lowered
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
				const MethodResolution<std::string> resolved = m_resolver.ResolveIndex(Scope().m_specialization.m_methods, argument_types[0u], argument_types[1u]);
				if (!resolved.has_value())
				{
					return std::unexpected(ResolutionError(resolved.error(), op));
				}
				return CallResolved(resolved.value(), { container, position }, argument_types, result_type, op);
			}
			const TypeRef& element_type = GenericTypes::RepresentationOf(container_type)->GetType<MidoriType::ArrayType>().m_element_type;
			return Builder().AtLine(op.m_line).Emit(MidoriIROp::ArrayGet, element_type, { container, position });
		});
}

Lowering::Lowered Lowering::LowerConstruct(MidoriExpression::Construct& construct)
{
	std::vector<MidoriExpression*> fields = construct.m_params
		| std::views::transform([](std::unique_ptr<MidoriExpression>& field) { return field.get(); })
		| std::ranges::to<std::vector>();
	const TypeRef type = Concrete(construct.m_type_data);
	const int line = construct.m_data_name.m_line;
	if (construct.IsConstructTypeOf<MidoriExpression::Construct::Struct>())
	{
		return LowerOperation(MidoriIROp::Construct, type, std::move(fields), {}, line);
	}
	const int tag = std::get<MidoriExpression::Construct::Union>(construct.m_construct_ctx).m_index;
	return LowerOperation(MidoriIROp::MakeUnion, type, std::move(fields), MidoriIRTag{ tag }, line);
}

// A copy of the source with some members replaced. Every replacement is
// evaluated against the original, in member order, which is what makes
// `{ r with a = r.b, b = r.a }` a swap rather than two updates.
Lowering::Lowered Lowering::LowerRecordUpdate(MidoriExpression::RecordUpdate& record_update)
{
	const int line = record_update.m_with_keyword.m_line;
	const TypeRef type = Concrete(record_update.m_type_data);
	return Lower(*record_update.m_source)
		.and_then([&](MidoriIRValueId source) -> Lowered
		{
			if (IsDead())
			{
				return Placeholder(type);
			}
			const std::vector<TypeRef>& member_types = GenericTypes::RepresentationOf(Scope().m_function.TypeOf(source))->GetType<MidoriType::StructType>().m_member_types;
			std::vector<MidoriIRValueId> members;
			members.reserve(record_update.m_slot_sources.size());
			for (size_t slot = 0u; slot < record_update.m_slot_sources.size(); slot += 1u)
			{
				const int update = record_update.m_slot_sources[slot];
				if (update < 0)
				{
					members.push_back(Builder().AtLine(line).Emit(MidoriIROp::GetMember, member_types[slot], { source }, MidoriIRIndex{ static_cast<uint32_t>(slot) }));
					continue;
				}
				const Lowered value = Lower(*record_update.m_updates[static_cast<size_t>(update)].m_value);
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
			return Builder().AtLine(line).Emit(MidoriIROp::Construct, type, std::move(members));
		});
}

Lowering::Lowered Lowering::LowerMemberAccess(MidoriExpression::MemberAccess& member)
{
	const int line = member.m_member_name.m_line;
	return Lower(*member.m_struct)
		.transform([this, &member, line](MidoriIRValueId record)
		{
			if (IsDead())
			{
				return Placeholder(Concrete(member.m_type_data));
			}
			const std::vector<TypeRef>& member_types = GenericTypes::RepresentationOf(Scope().m_function.TypeOf(record))->GetType<MidoriType::StructType>().m_member_types;
			const uint32_t index = static_cast<uint32_t>(member.m_index);
			return Builder().AtLine(line).Emit(MidoriIROp::GetMember, member_types[index], { record }, MidoriIRIndex{ index });
		});
}

// A range without a step steps by one.
Lowering::Lowered Lowering::LowerRange(MidoriExpression& start, MidoriExpression* step, MidoriExpression& end, const TypeRef& range_type, int line)
{
	const bool is_float = range_type->GetType<MidoriType::RangeType>().m_element_type->IsType<MidoriType::FloatType>();
	return Lower(start)
		.and_then([&](MidoriIRValueId first) -> LoweredValues
		{
			const Lowered stride = step == nullptr
				? Lowered(is_float ? Builder().AtLine(line).ConstFloat(1.0) : Builder().AtLine(line).ConstInt(1))
				: Lower(*step);
			return stride.and_then([&](MidoriIRValueId by)
			{
				return Lower(end).transform([first, by](MidoriIRValueId last) { return std::vector<MidoriIRValueId>{ first, by, last }; });
			});
		})
		.transform([&](std::vector<MidoriIRValueId> bounds)
		{
			if (IsDead())
			{
				return Placeholder(range_type);
			}
			return Builder().AtLine(line).Emit(MidoriIROp::MakeRange, range_type, std::move(bounds));
		});
}

// The one argument stands for all of the function's parameters: the value
// itself for one, a tuple spread across them for several, and a discarded
// Unit for none. The function is evaluated after it.
Lowering::Lowered Lowering::LowerSpawn(MidoriExpression::Spawn& spawn)
{
	const int line = spawn.m_spawn_keyword.m_line;
	const TypeRef type = Concrete(spawn.m_type_data);
	return Lower(*spawn.m_arguments.front())
		.and_then([&](MidoriIRValueId argument) -> Lowered
		{
			std::vector<MidoriIRValueId> operands;
			if (spawn.m_callee_arity == 1)
			{
				operands.push_back(argument);
			}
			else if (spawn.m_callee_arity >= 2 && !IsDead())
			{
				const std::vector<TypeRef>& element_types = GenericTypes::RepresentationOf(Scope().m_function.TypeOf(argument))->GetType<MidoriType::TupleType>().m_element_types;
				for (uint32_t index = 0u; index < static_cast<uint32_t>(spawn.m_callee_arity); index += 1u)
				{
					operands.push_back(Builder().AtLine(line).Emit(MidoriIROp::TupleGet, element_types[index], { argument }, MidoriIRIndex{ index }));
				}
			}

			return Lower(*spawn.m_callee)
				.transform([&](MidoriIRValueId callee)
				{
					if (IsDead())
					{
						return Placeholder(type);
					}
					operands.push_back(callee);
					return Builder().AtLine(line).Emit(MidoriIROp::Spawn, type, std::move(operands));
				});
		});
}

// `join` builds Result<T, WorkerError> itself, from the tags the type checker
// recorded.
Lowering::Lowered Lowering::LowerJoin(MidoriExpression::Join& join)
{
	return LowerOperation(MidoriIROp::Join, Concrete(join.m_type_data), { join.m_worker.get() }, MidoriIRJoinTags{ join.m_ok_tag, join.m_err_tag, join.m_cancelled_tag, join.m_failed_tag }, join.m_join_keyword.m_line);
}
