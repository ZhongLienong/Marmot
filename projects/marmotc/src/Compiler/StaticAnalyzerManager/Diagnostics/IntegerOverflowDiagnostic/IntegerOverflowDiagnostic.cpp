#include "IntegerOverflowDiagnostic.h"

#include "Bytecode/Scalar/Scalar.h"

#include <limits>
#include <optional>
#include <string>

namespace
{
	std::optional<MarmotInteger> TryParseIntegerLiteral(const MarmotExpression& expression)
	{
		const MarmotExpression* current = &expression;
		while (current->IsExpression<MarmotExpression::Group>())
		{
			current = current->GetExpression<MarmotExpression::Group>().m_expr_in.get();
		}

		if (current->IsLiteral(MarmotExpression::LiteralKind::Integer))
		{
			return ParseIntegerLiteral(current->GetExpression<MarmotExpression::Literal>().m_token.m_lexeme);
		}

		if (current->IsExpression<MarmotExpression::UnaryPrefix>())
		{
			const MarmotExpression::UnaryPrefix& unary = current->GetExpression<MarmotExpression::UnaryPrefix>();
			if (unary.m_op.m_token_name == Token::Name::SINGLE_MINUS)
			{
				const std::optional<MarmotInteger> inner = TryParseIntegerLiteral(*unary.m_expr);
				if (!inner.has_value() || *inner == (std::numeric_limits<MarmotInteger>::min)())
				{
					return std::nullopt;
				}
				return -*inner;
			}
		}

		return std::nullopt;
	}

	bool WouldAdditionOverflow(MarmotInteger left, MarmotInteger right)
	{
		const MarmotInteger max_value = (std::numeric_limits<MarmotInteger>::max)();
		const MarmotInteger min_value = (std::numeric_limits<MarmotInteger>::min)();
		if (right > 0)
		{
			return left > max_value - right;
		}
		if (right < 0)
		{
			return left < min_value - right;
		}
		return false;
	}

	bool WouldSubtractionOverflow(MarmotInteger left, MarmotInteger right)
	{
		const MarmotInteger max_value = (std::numeric_limits<MarmotInteger>::max)();
		const MarmotInteger min_value = (std::numeric_limits<MarmotInteger>::min)();
		if (right > 0)
		{
			return left < min_value + right;
		}
		if (right < 0)
		{
			return left > max_value + right;
		}
		return false;
	}

	bool WouldMultiplicationOverflow(MarmotInteger left, MarmotInteger right)
	{
		const MarmotInteger max_value = (std::numeric_limits<MarmotInteger>::max)();
		const MarmotInteger min_value = (std::numeric_limits<MarmotInteger>::min)();

		if (left == 0 || right == 0)
		{
			return false;
		}
		if ((left == -1 && right == min_value) || (right == -1 && left == min_value))
		{
			return true;
		}
		if (left > 0)
		{
			if (right > 0)
			{
				return left > max_value / right;
			}
			return right < min_value / left;
		}
		if (right > 0)
		{
			return left < min_value / right;
		}
		return left < max_value / right;
	}

	bool WouldLeftShiftOverflow(MarmotInteger value, MarmotInteger shift_amount)
	{
		constexpr int bit_width = std::numeric_limits<MarmotInteger>::digits;
		const MarmotInteger max_value = (std::numeric_limits<MarmotInteger>::max)();

		if (shift_amount < 0 || shift_amount >= bit_width)
		{
			return true;
		}
		if (shift_amount == 0 || value == 0)
		{
			return false;
		}
		if (value < 0)
		{
			return true;
		}

		return value > (max_value >> static_cast<int>(shift_amount));
	}
}

void IntegerOverflowDiagnostic::operator()(MarmotExpression::Binary& binary)
{
	VisitExpression(binary.m_left);
	VisitExpression(binary.m_right);

	if (!binary.m_type_data->IsType<MarmotType::IntegerType>())
	{
		return;
	}

	const std::optional<MarmotInteger> left_value = TryParseIntegerLiteral(*binary.m_left);
	const std::optional<MarmotInteger> right_value = TryParseIntegerLiteral(*binary.m_right);
	if (!left_value.has_value() || !right_value.has_value())
	{
		return;
	}

	std::optional<std::string> message = std::nullopt;
	switch (binary.m_op.m_token_name)
	{
	case Token::Name::SINGLE_PLUS:
		if (WouldAdditionOverflow(*left_value, *right_value))
		{
			message = "Literal integer addition overflows Int.";
		}
		break;
	case Token::Name::SINGLE_MINUS:
		if (WouldSubtractionOverflow(*left_value, *right_value))
		{
			message = "Literal integer subtraction overflows Int.";
		}
		break;
	case Token::Name::STAR:
		if (WouldMultiplicationOverflow(*left_value, *right_value))
		{
			message = "Literal integer multiplication overflows Int.";
		}
		break;
	case Token::Name::LEFT_SHIFT:
		if (WouldLeftShiftOverflow(*left_value, *right_value))
		{
			message = "Literal left shift overflows Int or exceeds the Int bit width.";
		}
		break;
	default:
		break;
	}

	if (message.has_value())
	{
		EmitWarning(CompilerWarningCode::IntegerOverflow, binary.m_op, *message);
	}
}
