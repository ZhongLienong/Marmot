#include "IntegerOverflowDiagnostic.h"

#include "Common/Value/Value.h"

#include <limits>
#include <optional>
#include <string>

namespace
{
	std::optional<MidoriInteger> TryParseIntegerLiteral(const MidoriExpression& expression)
	{
		const MidoriExpression* current = &expression;
		while (current->IsExpression<MidoriExpression::Group>())
		{
			current = current->GetExpression<MidoriExpression::Group>().m_expr_in.get();
		}

		if (current->IsLiteral(MidoriExpression::LiteralKind::Integer))
		{
			return ParseIntegerLiteral(current->GetExpression<MidoriExpression::Literal>().m_token.m_lexeme);
		}

		if (current->IsExpression<MidoriExpression::UnaryPrefix>())
		{
			const MidoriExpression::UnaryPrefix& unary = current->GetExpression<MidoriExpression::UnaryPrefix>();
			if (unary.m_op.m_token_name == Token::Name::SINGLE_MINUS)
			{
				const std::optional<MidoriInteger> inner = TryParseIntegerLiteral(*unary.m_expr);
				if (!inner.has_value() || *inner == (std::numeric_limits<MidoriInteger>::min)())
				{
					return std::nullopt;
				}
				return -*inner;
			}
		}

		return std::nullopt;
	}

	bool WouldAdditionOverflow(MidoriInteger left, MidoriInteger right)
	{
		const MidoriInteger max_value = (std::numeric_limits<MidoriInteger>::max)();
		const MidoriInteger min_value = (std::numeric_limits<MidoriInteger>::min)();
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

	bool WouldSubtractionOverflow(MidoriInteger left, MidoriInteger right)
	{
		const MidoriInteger max_value = (std::numeric_limits<MidoriInteger>::max)();
		const MidoriInteger min_value = (std::numeric_limits<MidoriInteger>::min)();
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

	bool WouldMultiplicationOverflow(MidoriInteger left, MidoriInteger right)
	{
		const MidoriInteger max_value = (std::numeric_limits<MidoriInteger>::max)();
		const MidoriInteger min_value = (std::numeric_limits<MidoriInteger>::min)();

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

	bool WouldLeftShiftOverflow(MidoriInteger value, MidoriInteger shift_amount)
	{
		constexpr int bit_width = std::numeric_limits<MidoriInteger>::digits;
		const MidoriInteger max_value = (std::numeric_limits<MidoriInteger>::max)();

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

std::string_view IntegerOverflowDiagnostic::GetName() const
{
	return "IntegerOverflowDiagnostic";
}

void IntegerOverflowDiagnostic::operator()(MidoriExpression::Binary& binary)
{
	VisitExpression(binary.m_left);
	VisitExpression(binary.m_right);

	if (!binary.m_type_data->IsType<MidoriType::IntegerType>())
	{
		return;
	}

	const std::optional<MidoriInteger> left_value = TryParseIntegerLiteral(*binary.m_left);
	const std::optional<MidoriInteger> right_value = TryParseIntegerLiteral(*binary.m_right);
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
