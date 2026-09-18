#include "SemanticFacts.h"

#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace OptimizerAnalysis = MidoriAnalysis;

namespace
{
	using OptimizerAnalysis::BlockLocalAccessSummary;
	using OptimizerAnalysis::ConstantValue;
	using OptimizerAnalysis::StatementLocalAccessSummary;
	using OptimizerAnalysis::UnitConstant;

	std::optional<MidoriInteger> SafeParseInteger(const std::string& lexeme)
	{
		return ParseIntegerLiteral(lexeme);
	}

	std::optional<MidoriFloat> SafeParseFloat(const std::string& lexeme)
	{
		try
		{
			return std::stod(lexeme);
		}
		catch (const std::invalid_argument&)
		{
			return std::nullopt;
		}
		catch (const std::out_of_range&)
		{
			return std::nullopt;
		}
	}

	std::optional<MidoriByte> SafeParseByte(const std::string& lexeme)
	{
		const std::optional<uint64_t> value = ParseUnsignedLiteral(lexeme);
		if (!value.has_value() || value.value() > 0xFFu)
		{
			return std::nullopt;
		}
		return static_cast<MidoriByte>(value.value());
	}

	std::optional<MidoriWord> SafeParseWord(const std::string& lexeme)
	{
		return ParseUnsignedLiteral(lexeme);
	}

	std::optional<bool> TryGetBoolValue(const ConstantValue& value)
	{
		if (!value.Is<MidoriBool>())
		{
			return std::nullopt;
		}
		return value.Get<MidoriBool>();
	}

	std::optional<ConstantValue> TryGetPatternLiteralValue(const MidoriPattern::Literal& literal)
	{
		switch (literal.m_kind)
		{
		case MidoriPattern::LiteralKind::Bool:
			return ConstantValue{ literal.m_token.m_token_name == Token::Name::TRUE };
		case MidoriPattern::LiteralKind::Integer:
		{
			const std::optional<MidoriInteger> value = SafeParseInteger(literal.m_token.m_lexeme);
			return value.has_value() ? std::optional<ConstantValue>{ ConstantValue{ value.value() } } : std::nullopt;
		}
		case MidoriPattern::LiteralKind::Byte:
		{
			const std::optional<MidoriByte> value = SafeParseByte(literal.m_token.m_lexeme);
			return value.has_value() ? std::optional<ConstantValue>{ ConstantValue{ value.value() } } : std::nullopt;
		}
		case MidoriPattern::LiteralKind::Word:
		{
			const std::optional<MidoriWord> value = SafeParseWord(literal.m_token.m_lexeme);
			return value.has_value() ? std::optional<ConstantValue>{ ConstantValue{ value.value() } } : std::nullopt;
		}
		case MidoriPattern::LiteralKind::Float:
		{
			const std::optional<MidoriFloat> value = SafeParseFloat(literal.m_token.m_lexeme);
			return value.has_value() ? std::optional<ConstantValue>{ ConstantValue{ value.value() } } : std::nullopt;
		}
		case MidoriPattern::LiteralKind::Text:
			return ConstantValue{ literal.m_token.m_lexeme };
		case MidoriPattern::LiteralKind::Unit:
			return ConstantValue{ UnitConstant{} };
		default:
			return std::nullopt;
		}
	}

	template<typename T>
	bool IsEmptyRange(T start, T step, T end)
	{
		if (step > static_cast<T>(0))
		{
			return !(start < end);
		}

		return !(start > end);
	}

	std::optional<int> TryGetLocalIndex(const MidoriExpression::NameContext::Tag& name_ctx)
	{
		if (!std::holds_alternative<MidoriExpression::NameContext::Local>(name_ctx))
		{
			return std::nullopt;
		}

		return std::get<MidoriExpression::NameContext::Local>(name_ctx).m_index;
	}

	void RecordRead(StatementLocalAccessSummary& summary, int local_index)
	{
		summary.m_locals[local_index].m_reads += 1;
	}

	void RecordAssignment(StatementLocalAccessSummary& summary, int local_index)
	{
		summary.m_locals[local_index].m_assignments += 1;
	}

	void RecordDefinition(StatementLocalAccessSummary& summary, const std::optional<int>& local_index)
	{
		if (local_index.has_value())
		{
			RecordAssignment(summary, local_index.value());
		}
	}

	std::optional<ConstantValue> EvalConstant(const MidoriExpression& expr);

	bool IsKnownPatternMatchValue(const MidoriExpression& expr)
	{
		const MidoriExpression* stripped_expr = OptimizerAnalysis::StripRedundantGroups(&expr);
		return stripped_expr != nullptr
			&& (OptimizerAnalysis::IsLiteralExpression(*stripped_expr)
				|| stripped_expr->IsExpression<MidoriExpression::Tuple>()
				|| stripped_expr->IsExpression<MidoriExpression::Array>()
				|| stripped_expr->IsExpression<MidoriExpression::Construct>());
	}

	std::optional<bool> TryMatchPatternKnown(const MidoriPattern& pattern, const MidoriExpression& expr)
	{
		const MidoriExpression* stripped_expr = OptimizerAnalysis::StripRedundantGroups(&expr);
		if (stripped_expr == nullptr)
		{
			return std::nullopt;
		}

		return std::visit
		(
			[&](const auto& node) -> std::optional<bool>
			{
				using T = std::decay_t<decltype(node)>;

				if constexpr (std::is_same_v<T, MidoriPattern::Binding>)
				{
					return true;
				}
				else if constexpr (std::is_same_v<T, MidoriPattern::Wildcard>)
				{
					return true;
				}
				else if constexpr (std::is_same_v<T, MidoriPattern::Literal>)
				{
					const std::optional<ConstantValue> pattern_value = TryGetPatternLiteralValue(node);
					if (!pattern_value.has_value())
					{
						return std::nullopt;
					}

					const std::optional<ConstantValue> expr_value = EvalConstant(*stripped_expr);
					if (expr_value.has_value())
					{
						return expr_value->m_value == pattern_value->m_value;
					}

					return IsKnownPatternMatchValue(*stripped_expr) ? std::optional<bool>{ false } : std::nullopt;
				}
				else if constexpr (std::is_same_v<T, MidoriPattern::Tuple>)
				{
					if (!stripped_expr->IsExpression<MidoriExpression::Tuple>())
					{
						return IsKnownPatternMatchValue(*stripped_expr) ? std::optional<bool>{ false } : std::nullopt;
					}

					const MidoriExpression::Tuple& tuple_expr = stripped_expr->GetExpression<MidoriExpression::Tuple>();
					if (tuple_expr.m_elements.size() != node.m_elements.size())
					{
						return false;
					}

					for (size_t i = 0u; i < node.m_elements.size(); i += 1u)
					{
						const std::optional<bool> matched = TryMatchPatternKnown(*node.m_elements[i], *tuple_expr.m_elements[i]);
						if (!matched.has_value())
						{
							return std::nullopt;
						}
						if (!matched.value())
						{
							return false;
						}
					}

					return true;
				}
				else if constexpr (std::is_same_v<T, MidoriPattern::Array>)
				{
					if (!stripped_expr->IsExpression<MidoriExpression::Array>())
					{
						return IsKnownPatternMatchValue(*stripped_expr) ? std::optional<bool>{ false } : std::nullopt;
					}

					const MidoriExpression::Array& array_expr = stripped_expr->GetExpression<MidoriExpression::Array>();
					if (array_expr.m_elems.size() != node.m_elements.size())
					{
						return false;
					}

					for (size_t i = 0u; i < node.m_elements.size(); i += 1u)
					{
						const std::optional<bool> matched = TryMatchPatternKnown(*node.m_elements[i], *array_expr.m_elems[i]);
						if (!matched.has_value())
						{
							return std::nullopt;
						}
						if (!matched.value())
						{
							return false;
						}
					}

					return true;
				}
				else if constexpr (std::is_same_v<T, MidoriPattern::Constructor>)
				{
					if (!stripped_expr->IsExpression<MidoriExpression::Construct>())
					{
						return IsKnownPatternMatchValue(*stripped_expr) ? std::optional<bool>{ false } : std::nullopt;
					}

					const MidoriExpression::Construct& construct_expr = stripped_expr->GetExpression<MidoriExpression::Construct>();
					if (construct_expr.m_params.size() != node.m_args.size())
					{
						return false;
					}

					if (node.m_is_union)
					{
						if (!construct_expr.IsConstructTypeOf<MidoriExpression::Construct::Union>())
						{
							return false;
						}

						const MidoriExpression::Construct::Union& union_ctx = std::get<MidoriExpression::Construct::Union>(construct_expr.m_construct_ctx);
						if (node.m_tag >= 0)
						{
							if (union_ctx.m_index != node.m_tag)
							{
								return false;
							}
						}
						else if (construct_expr.m_data_name.m_lexeme != node.m_name)
						{
							return false;
						}
					}
					else
					{
						if (!construct_expr.IsConstructTypeOf<MidoriExpression::Construct::Struct>() || construct_expr.m_data_name.m_lexeme != node.m_name)
						{
							return false;
						}
					}

					for (size_t i = 0u; i < node.m_args.size(); i += 1u)
					{
						const std::optional<bool> matched = TryMatchPatternKnown(*node.m_args[i], *construct_expr.m_params[i]);
						if (!matched.has_value())
						{
							return std::nullopt;
						}
						if (!matched.value())
						{
							return false;
						}
					}

					return true;
				}
				else
				{
					return std::nullopt;
				}
			},
			*pattern
		);
	}

	std::optional<ConstantValue> EvalBinary(const MidoriExpression::Binary& binary)
	{
		switch (binary.m_op.m_token_name)
		{
		case Token::Name::DOUBLE_AMPERSAND:
		{
			std::optional<bool> left_truth = OptimizerAnalysis::TryEvalTruthValue(*binary.m_left);
			if (left_truth.has_value() && !left_truth.value())
			{
				return ConstantValue{ false };
			}

			std::optional<bool> right_truth = OptimizerAnalysis::TryEvalTruthValue(*binary.m_right);
			if (left_truth.has_value() && right_truth.has_value())
			{
				return ConstantValue{ left_truth.value() && right_truth.value() };
			}
			return std::nullopt;
		}
		case Token::Name::DOUBLE_BAR:
		{
			std::optional<bool> left_truth = OptimizerAnalysis::TryEvalTruthValue(*binary.m_left);
			if (left_truth.has_value() && left_truth.value())
			{
				return ConstantValue{ true };
			}

			std::optional<bool> right_truth = OptimizerAnalysis::TryEvalTruthValue(*binary.m_right);
			if (left_truth.has_value() && right_truth.has_value())
			{
				return ConstantValue{ left_truth.value() || right_truth.value() };
			}
			return std::nullopt;
		}
		default:
			break;
		}

		std::optional<ConstantValue> left_value = EvalConstant(*binary.m_left);
		std::optional<ConstantValue> right_value = EvalConstant(*binary.m_right);
		if (!left_value.has_value() || !right_value.has_value())
		{
			return std::nullopt;
		}

		const Token::Name op = binary.m_op.m_token_name;

		if (left_value->Is<MidoriInteger>() && right_value->Is<MidoriInteger>())
		{
			const MidoriInteger left = left_value->Get<MidoriInteger>();
			const MidoriInteger right = right_value->Get<MidoriInteger>();

			switch (op)
			{
			case Token::Name::SINGLE_PLUS:
				return ConstantValue{ left + right };
			case Token::Name::SINGLE_MINUS:
				return ConstantValue{ left - right };
			case Token::Name::STAR:
				return ConstantValue{ left * right };
			case Token::Name::SLASH:
				if (right == 0ll)
				{
					return std::nullopt;
				}
				return ConstantValue{ left / right };
			case Token::Name::PERCENT:
				if (right == 0ll)
				{
					return std::nullopt;
				}
				return ConstantValue{ left % right };
			case Token::Name::LEFT_SHIFT:
				return ConstantValue{ left << right };
			case Token::Name::RIGHT_SHIFT:
				return ConstantValue{ left >> right };
			case Token::Name::SINGLE_AMPERSAND:
				return ConstantValue{ left & right };
			case Token::Name::SINGLE_BAR:
				return ConstantValue{ left | right };
			case Token::Name::CARET:
				return ConstantValue{ left ^ right };
			case Token::Name::LEFT_ANGLE:
				return ConstantValue{ left < right };
			case Token::Name::RIGHT_ANGLE:
				return ConstantValue{ left > right };
			case Token::Name::LESS_EQUAL:
				return ConstantValue{ left <= right };
			case Token::Name::GREATER_EQUAL:
				return ConstantValue{ left >= right };
			case Token::Name::DOUBLE_EQUAL:
				return ConstantValue{ left == right };
			case Token::Name::BANG_EQUAL:
				return ConstantValue{ left != right };
			default:
				return std::nullopt;
			}
		}

		if (left_value->Is<MidoriFloat>() && right_value->Is<MidoriFloat>())
		{
			const MidoriFloat left = left_value->Get<MidoriFloat>();
			const MidoriFloat right = right_value->Get<MidoriFloat>();

			switch (op)
			{
			case Token::Name::SINGLE_PLUS:
				return ConstantValue{ left + right };
			case Token::Name::SINGLE_MINUS:
				return ConstantValue{ left - right };
			case Token::Name::STAR:
				return ConstantValue{ left * right };
			case Token::Name::SLASH:
				if (right == 0.0)
				{
					return std::nullopt;
				}
				return ConstantValue{ left / right };
			default:
				return std::nullopt;
			}
		}

		if (left_value->Is<MidoriByte>() && right_value->Is<MidoriByte>())
		{
			const MidoriByte left = left_value->Get<MidoriByte>();
			const MidoriByte right = right_value->Get<MidoriByte>();

			switch (op)
			{
			case Token::Name::SINGLE_PLUS:
				return ConstantValue{ static_cast<MidoriByte>(left + right) };
			case Token::Name::SINGLE_MINUS:
				return ConstantValue{ static_cast<MidoriByte>(left - right) };
			case Token::Name::STAR:
				return ConstantValue{ static_cast<MidoriByte>(left * right) };
			case Token::Name::SLASH:
				if (right == 0u)
				{
					return std::nullopt;
				}
				return ConstantValue{ static_cast<MidoriByte>(left / right) };
			case Token::Name::PERCENT:
				if (right == 0u)
				{
					return std::nullopt;
				}
				return ConstantValue{ static_cast<MidoriByte>(left % right) };
			case Token::Name::LEFT_SHIFT:
				return ConstantValue{ static_cast<MidoriByte>(left << right) };
			case Token::Name::RIGHT_SHIFT:
				return ConstantValue{ static_cast<MidoriByte>(left >> right) };
			case Token::Name::SINGLE_AMPERSAND:
				return ConstantValue{ static_cast<MidoriByte>(left & right) };
			case Token::Name::SINGLE_BAR:
				return ConstantValue{ static_cast<MidoriByte>(left | right) };
			case Token::Name::CARET:
				return ConstantValue{ static_cast<MidoriByte>(left ^ right) };
			case Token::Name::LEFT_ANGLE:
				return ConstantValue{ left < right };
			case Token::Name::RIGHT_ANGLE:
				return ConstantValue{ left > right };
			case Token::Name::LESS_EQUAL:
				return ConstantValue{ left <= right };
			case Token::Name::GREATER_EQUAL:
				return ConstantValue{ left >= right };
			case Token::Name::DOUBLE_EQUAL:
				return ConstantValue{ left == right };
			case Token::Name::BANG_EQUAL:
				return ConstantValue{ left != right };
			default:
				return std::nullopt;
			}
		}

		if (left_value->Is<MidoriWord>() && right_value->Is<MidoriWord>())
		{
			const MidoriWord left = left_value->Get<MidoriWord>();
			const MidoriWord right = right_value->Get<MidoriWord>();

			switch (op)
			{
			case Token::Name::SINGLE_PLUS:
				return ConstantValue{ left + right };
			case Token::Name::SINGLE_MINUS:
				return ConstantValue{ left - right };
			case Token::Name::STAR:
				return ConstantValue{ left * right };
			case Token::Name::SLASH:
				if (right == 0ull)
				{
					return std::nullopt;
				}
				return ConstantValue{ left / right };
			case Token::Name::PERCENT:
				if (right == 0ull)
				{
					return std::nullopt;
				}
				return ConstantValue{ left % right };
			case Token::Name::LEFT_SHIFT:
				return ConstantValue{ left << right };
			case Token::Name::RIGHT_SHIFT:
				return ConstantValue{ left >> right };
			case Token::Name::SINGLE_AMPERSAND:
				return ConstantValue{ left & right };
			case Token::Name::SINGLE_BAR:
				return ConstantValue{ left | right };
			case Token::Name::CARET:
				return ConstantValue{ left ^ right };
			case Token::Name::LEFT_ANGLE:
				return ConstantValue{ left < right };
			case Token::Name::RIGHT_ANGLE:
				return ConstantValue{ left > right };
			case Token::Name::LESS_EQUAL:
				return ConstantValue{ left <= right };
			case Token::Name::GREATER_EQUAL:
				return ConstantValue{ left >= right };
			case Token::Name::DOUBLE_EQUAL:
				return ConstantValue{ left == right };
			case Token::Name::BANG_EQUAL:
				return ConstantValue{ left != right };
			default:
				return std::nullopt;
			}
		}

		if (left_value->Is<MidoriBool>() && right_value->Is<MidoriBool>())
		{
			const MidoriBool left = left_value->Get<MidoriBool>();
			const MidoriBool right = right_value->Get<MidoriBool>();

			switch (op)
			{
			case Token::Name::DOUBLE_AMPERSAND:
				return ConstantValue{ left && right };
			case Token::Name::DOUBLE_BAR:
				return ConstantValue{ left || right };
			case Token::Name::DOUBLE_EQUAL:
				return ConstantValue{ left == right };
			case Token::Name::BANG_EQUAL:
				return ConstantValue{ left != right };
			default:
				return std::nullopt;
			}
		}

		if (left_value->Is<std::string>() && right_value->Is<std::string>())
		{
			if (op != Token::Name::DOUBLE_PLUS)
			{
				return std::nullopt;
			}

			return ConstantValue{ left_value->Get<std::string>() + right_value->Get<std::string>() };
		}

		return std::nullopt;
	}

	std::optional<ConstantValue> EvalUnary(const MidoriExpression::UnaryPrefix& unary)
	{
		std::optional<ConstantValue> inner_value = EvalConstant(*unary.m_expr);
		if (!inner_value.has_value())
		{
			return std::nullopt;
		}

		switch (unary.m_op.m_token_name)
		{
		case Token::Name::SINGLE_MINUS:
			if (inner_value->Is<MidoriInteger>())
			{
				return ConstantValue{ -inner_value->Get<MidoriInteger>() };
			}
			if (inner_value->Is<MidoriFloat>())
			{
				return ConstantValue{ -inner_value->Get<MidoriFloat>() };
			}
			return std::nullopt;
		case Token::Name::BANG:
			if (!inner_value->Is<MidoriBool>())
			{
				return std::nullopt;
			}
			return ConstantValue{ !inner_value->Get<MidoriBool>() };
		case Token::Name::TILDE:
			if (inner_value->Is<MidoriInteger>())
			{
				return ConstantValue{ ~inner_value->Get<MidoriInteger>() };
			}
			if (inner_value->Is<MidoriByte>())
			{
				return ConstantValue{ static_cast<MidoriByte>(~inner_value->Get<MidoriByte>()) };
			}
			if (inner_value->Is<MidoriWord>())
			{
				return ConstantValue{ static_cast<MidoriWord>(~inner_value->Get<MidoriWord>()) };
			}
			return std::nullopt;
		default:
			return std::nullopt;
		}
	}

	std::optional<ConstantValue> EvalConstant(const MidoriExpression& expr)
	{
		const MidoriExpression* stripped_expr = OptimizerAnalysis::StripRedundantGroups(&expr);
		if (stripped_expr == nullptr)
		{
			return std::nullopt;
		}

		return std::visit
		(
			[](const auto& node) -> std::optional<ConstantValue>
			{
				using T = std::decay_t<decltype(node)>;

				if constexpr (std::is_same_v<T, MidoriExpression::Literal>)
				{
					switch (node.m_kind)
					{
					case MidoriExpression::LiteralKind::Bool:
						return ConstantValue{ node.m_token.m_token_name == Token::Name::TRUE };
					case MidoriExpression::LiteralKind::Integer:
					{
						std::optional<MidoriInteger> value = SafeParseInteger(node.m_token.m_lexeme);
						return value.has_value() ? std::optional<ConstantValue>{ ConstantValue{ value.value() } } : std::nullopt;
					}
					case MidoriExpression::LiteralKind::Float:
					{
						std::optional<MidoriFloat> value = SafeParseFloat(node.m_token.m_lexeme);
						return value.has_value() ? std::optional<ConstantValue>{ ConstantValue{ value.value() } } : std::nullopt;
					}
					case MidoriExpression::LiteralKind::Byte:
					{
						std::optional<MidoriByte> value = SafeParseByte(node.m_token.m_lexeme);
						return value.has_value() ? std::optional<ConstantValue>{ ConstantValue{ value.value() } } : std::nullopt;
					}
					case MidoriExpression::LiteralKind::Word:
					{
						std::optional<MidoriWord> value = SafeParseWord(node.m_token.m_lexeme);
						return value.has_value() ? std::optional<ConstantValue>{ ConstantValue{ value.value() } } : std::nullopt;
					}
					case MidoriExpression::LiteralKind::Text:
						return ConstantValue{ node.m_token.m_lexeme };
					case MidoriExpression::LiteralKind::Unit:
						return ConstantValue{ UnitConstant{} };
					}
					return std::nullopt;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::UnaryPrefix>)
				{
					return EvalUnary(node);
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Binary>)
				{
					return EvalBinary(node);
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::IfElse>)
				{
					std::optional<bool> condition_value = OptimizerAnalysis::TryEvalTruthValue(*node.m_condition);
					if (!condition_value.has_value())
					{
						return std::nullopt;
					}

					return condition_value.value()
						? EvalConstant(*node.m_true_branch)
						: EvalConstant(*node.m_else_branch);
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::As>)
				{
					if (node.m_uses_convertable)
					{
						return std::nullopt;
					}

					std::shared_ptr<MidoriType> from_type = node.m_from_type.lock();
					if (!from_type || *from_type != *node.m_to_type)
					{
						return std::nullopt;
					}

					return EvalConstant(*node.m_expr);
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Block>)
				{
					if (!node.m_stmts.empty() || !node.m_final_expr.has_value())
					{
						return std::nullopt;
					}

					return EvalConstant(*node.m_final_expr.value());
				}
				else
				{
					return std::nullopt;
				}
			},
			**stripped_expr
		);
	}

	bool IsPureStatement(const MidoriStatement& stmt);

	bool IsPureExpression(const MidoriExpression& expr)
	{
		const MidoriExpression* stripped_expr = OptimizerAnalysis::StripRedundantGroups(&expr);
		if (stripped_expr == nullptr)
		{
			return false;
		}

		return std::visit
		(
			[](const auto& node) -> bool
			{
				using T = std::decay_t<decltype(node)>;

				if constexpr (std::is_same_v<T, MidoriExpression::Literal>
					|| std::is_same_v<T, MidoriExpression::NameAccess>)
				{
					return true;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::As>)
				{
					if (node.m_uses_convertable)
					{
						return false;
					}

					std::shared_ptr<MidoriType> from_type = node.m_from_type.lock();
					if (!from_type || *from_type != *node.m_to_type)
					{
						return false;
					}

					return IsPureExpression(*node.m_expr);
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Binary>)
				{
					if (node.m_uses_concatenable || node.m_uses_equatable || node.m_uses_orderable)
					{
						return false;
					}

					if (node.m_op.m_token_name == Token::Name::SLASH || node.m_op.m_token_name == Token::Name::PERCENT)
					{
						return false;
					}

					if (node.m_op.m_token_name == Token::Name::DOUBLE_AMPERSAND)
					{
						std::optional<bool> left_truth = OptimizerAnalysis::TryEvalTruthValue(*node.m_left);
						if (left_truth.has_value() && !left_truth.value())
						{
							return IsPureExpression(*node.m_left);
						}
					}
					else if (node.m_op.m_token_name == Token::Name::DOUBLE_BAR)
					{
						std::optional<bool> left_truth = OptimizerAnalysis::TryEvalTruthValue(*node.m_left);
						if (left_truth.has_value() && left_truth.value())
						{
							return IsPureExpression(*node.m_left);
						}
					}

					return IsPureExpression(*node.m_left) && IsPureExpression(*node.m_right);
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Tuple>)
				{
					for (const std::unique_ptr<MidoriExpression>& element : node.m_elements)
					{
						if (!IsPureExpression(*element))
						{
							return false;
						}
					}
					return true;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::UnaryPrefix>)
				{
					if (node.m_uses_countable)
					{
						return false;
					}
					return IsPureExpression(*node.m_expr);
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::UnarySuffix>)
				{
					return false;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Call>
					|| std::is_same_v<T, MidoriExpression::For>					|| std::is_same_v<T, MidoriExpression::ArrayComprehension>)
				{
					return false;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Function>)
				{
					return true;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::RecordUpdate>)
				{
					// A record update reads the source's members and builds a new struct, so it
					// is pure exactly when the source and every value expression are. This case
					// has to be spelled out: the chain ends in `else { return false; }`, so
					// omitting it would silently mark every record update impure and cost dead
					// code elimination the ability to drop one.
					if (!IsPureExpression(*node.m_source))
					{
						return false;
					}

					for (const MidoriExpression::RecordUpdate::FieldUpdate& update : node.m_updates)
					{
						if (!IsPureExpression(*update.m_value))
						{
							return false;
						}
					}
					return true;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Construct>)
				{
					for (const std::unique_ptr<MidoriExpression>& arg : node.m_params)
					{
						if (!IsPureExpression(*arg))
						{
							return false;
						}
					}
					return true;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::IfElse>)
				{
					std::optional<bool> condition_value = OptimizerAnalysis::TryEvalTruthValue(*node.m_condition);
					if (condition_value.has_value())
					{
						return IsPureExpression(*node.m_condition)
							&& (condition_value.value()
								? IsPureExpression(*node.m_true_branch)
								: IsPureExpression(*node.m_else_branch));
					}

					return IsPureExpression(*node.m_condition)
						&& IsPureExpression(*node.m_true_branch)
						&& IsPureExpression(*node.m_else_branch);
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::MemberAccess>)
				{
					return IsPureExpression(*node.m_struct);
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Array>)
				{
					for (const std::unique_ptr<MidoriExpression>& elem : node.m_elems)
					{
						if (!IsPureExpression(*elem))
						{
							return false;
						}
					}
					return true;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::IndexAccess>)
				{
					return false;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::RangeBinary>)
				{
					return IsPureExpression(*node.m_start) && IsPureExpression(*node.m_end);
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::RangeTernary>)
				{
					return IsPureExpression(*node.m_start)
						&& IsPureExpression(*node.m_step)
						&& IsPureExpression(*node.m_end);
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Block>)
				{
					for (const std::unique_ptr<MidoriStatement>& stmt : node.m_stmts)
					{
						if (!IsPureStatement(*stmt))
						{
							return false;
						}
					}

					return !node.m_final_expr.has_value() || IsPureExpression(*node.m_final_expr.value());
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Match>)
				{
					if (!IsPureExpression(*node.m_arg_expr))
					{
						return false;
					}

					for (const std::unique_ptr<MidoriExpression>& case_expr : node.m_cases)
					{
						if (!IsPureExpression(*case_expr))
						{
							return false;
						}
					}

					return true;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Case>)
				{
					if (node.HasGuard() && !IsPureExpression(*node.m_guard.value()))
					{
						return false;
					}

					return IsPureExpression(*node.m_expr);
				}
				else
				{
					return false;
				}
			},
			**stripped_expr
		);
	}

	bool IsPureStatement(const MidoriStatement& stmt)
	{
		return std::visit
		(
			[](const auto& node) -> bool
			{
				using T = std::decay_t<decltype(node)>;

				if constexpr (std::is_same_v<T, MidoriStatement::ExpressionStatement>)
				{
					return IsPureExpression(*node.m_expr);
				}
				else if constexpr (std::is_same_v<T, MidoriStatement::VariableDefinition>)
				{
					return node.m_is_elided || IsPureExpression(*node.m_value);
				}
				else if constexpr (std::is_same_v<T, MidoriStatement::TupleDefinition>)
				{
					return IsPureExpression(*node.m_value);
				}
				else if constexpr (std::is_same_v<T, MidoriStatement::FunctionDefinition>)
				{
					return true;
				}
				else
				{
					return false;
				}
			},
			*stmt
		);
	}

	class LocalAccessCollector
	{
	public:
		explicit LocalAccessCollector(StatementLocalAccessSummary& summary)
			: m_summary(summary)
		{
		}

		void VisitStatement(const MidoriStatement& stmt)
		{
			std::visit([this](const auto& node) { Visit(node); }, *stmt);
		}

		void VisitExpression(const MidoriExpression& expr)
		{
			std::visit([this](const auto& node) { Visit(node); }, *expr);
		}

	private:
		StatementLocalAccessSummary& m_summary;

		void NoteNestedCallableBoundary()
		{
			m_summary.m_has_nested_callable_boundary = true;
		}

		void RecordLocalRead(const MidoriExpression::NameContext::Tag& name_ctx)
		{
			std::optional<int> local_index = TryGetLocalIndex(name_ctx);
			if (local_index.has_value())
			{
				RecordRead(m_summary, local_index.value());
			}
		}

		void RecordLocalAssignment(const MidoriExpression::NameContext::Tag& name_ctx, bool also_reads)
		{
			std::optional<int> local_index = TryGetLocalIndex(name_ctx);
			if (!local_index.has_value())
			{
				return;
			}

			if (also_reads)
			{
				RecordRead(m_summary, local_index.value());
			}
			RecordAssignment(m_summary, local_index.value());
		}

		void RecordSyntheticLocal(int local_index)
		{
			if (local_index >= 0)
			{
				RecordAssignment(m_summary, local_index);
			}
		}

		void VisitPattern(const MidoriPattern& pattern)
		{
			std::visit
			(
				[this](const auto& node)
				{
					using T = std::decay_t<decltype(node)>;

					if constexpr (std::is_same_v<T, MidoriPattern::Binding>)
					{
						RecordDefinition(m_summary, node.m_local_index);
					}
					else if constexpr (std::is_same_v<T, MidoriPattern::Wildcard>)
					{
					}
					else if constexpr (std::is_same_v<T, MidoriPattern::Tuple> || std::is_same_v<T, MidoriPattern::Array>)
					{
						for (const std::unique_ptr<MidoriPattern>& element : node.m_elements)
						{
							VisitPattern(*element);
						}
					}
					else if constexpr (std::is_same_v<T, MidoriPattern::Constructor>)
					{
						for (const std::unique_ptr<MidoriPattern>& arg : node.m_args)
						{
							VisitPattern(*arg);
						}
					}
				},
				*pattern
			);
		}

		void Visit(const MidoriStatement::ExpressionStatement& node)
		{
			VisitExpression(*node.m_expr);
		}

		void Visit(const MidoriStatement::VariableDefinition& node)
		{
			RecordDefinition(m_summary, node.m_local_index);
			if (!node.m_is_elided)
			{
				VisitExpression(*node.m_value);
			}
		}

		void Visit(const MidoriStatement::TupleDefinition& node)
		{
			for (const std::optional<int>& local_index : node.m_local_indices)
			{
				RecordDefinition(m_summary, local_index);
			}
			VisitExpression(*node.m_value);
		}

		void Visit(const MidoriStatement::FunctionDefinition& node)
		{
			RecordDefinition(m_summary, node.m_local_index);
			NoteNestedCallableBoundary();
		}

		void Visit(const MidoriStatement::ForeignDefinition& node)
		{
			RecordDefinition(m_summary, node.m_local_index);
		}

		void Visit(const MidoriStatement::Struct&)
		{
		}

		void Visit(const MidoriStatement::Union&)
		{
		}

		void Visit(const MidoriStatement::Class&)
		{
			NoteNestedCallableBoundary();
		}

		void Visit(const MidoriStatement::Instance&)
		{
			NoteNestedCallableBoundary();
		}

		void Visit(const MidoriStatement::TypeAlias&)
		{
		}

		void Visit(const MidoriExpression::As& node)
		{
			VisitExpression(*node.m_expr);
		}

		void Visit(const MidoriExpression::Binary& node)
		{
			VisitExpression(*node.m_left);
			VisitExpression(*node.m_right);
		}

		void Visit(const MidoriExpression::Group& node)
		{
			VisitExpression(*node.m_expr_in);
		}

		void Visit(const MidoriExpression::Tuple& node)
		{
			for (const std::unique_ptr<MidoriExpression>& element : node.m_elements)
			{
				VisitExpression(*element);
			}
		}

		void Visit(const MidoriExpression::Literal&)
		{
		}

		void Visit(const MidoriExpression::UnaryPrefix& node)
		{
			VisitExpression(*node.m_expr);
		}

		void Visit(const MidoriExpression::UnarySuffix& node)
		{
			VisitExpression(*node.m_expr);
		}

		void Visit(const MidoriExpression::Spawn& node)
		{
			for (const std::unique_ptr<MidoriExpression>& argument : node.m_arguments)
			{
				VisitExpression(*argument);
			}

			VisitExpression(*node.m_callee);
		}

		void Visit(const MidoriExpression::Join& node)
		{
			VisitExpression(*node.m_worker);
		}

		void Visit(const MidoriExpression::ChannelCreate& node)
		{
			VisitExpression(*node.m_capacity);
		}

		void Visit(const MidoriExpression::Send& node)
		{
			VisitExpression(*node.m_channel);
			VisitExpression(*node.m_value);
		}

		void Visit(const MidoriExpression::Receive& node)
		{
			VisitExpression(*node.m_channel);
		}

		void Visit(const MidoriExpression::NameAccess& node)
		{
			RecordLocalRead(node.m_name_ctx);
		}

		void Visit(const MidoriExpression::Call& node)
		{
			VisitExpression(*node.m_callee);
			for (const std::unique_ptr<MidoriExpression>& arg : node.m_arguments)
			{
				VisitExpression(*arg);
			}
		}

		void Visit(const MidoriExpression::Function&)
		{
			NoteNestedCallableBoundary();
		}

		void Visit(const MidoriExpression::Construct& node)
		{
			for (const std::unique_ptr<MidoriExpression>& param : node.m_params)
			{
				VisitExpression(*param);
			}
		}

		void Visit(const MidoriExpression::RecordUpdate& node)
		{
			VisitExpression(*node.m_source);
			for (const MidoriExpression::RecordUpdate::FieldUpdate& update : node.m_updates)
			{
				VisitExpression(*update.m_value);
			}
		}

		void Visit(const MidoriExpression::IfElse& node)
		{
			VisitExpression(*node.m_condition);
			VisitExpression(*node.m_true_branch);
			VisitExpression(*node.m_else_branch);
		}

		void Visit(const MidoriExpression::MemberAccess& node)
		{
			VisitExpression(*node.m_struct);
		}

		void Visit(const MidoriExpression::Array& node)
		{
			for (const std::unique_ptr<MidoriExpression>& elem : node.m_elems)
			{
				VisitExpression(*elem);
			}
		}

		void Visit(const MidoriExpression::IndexAccess& node)
		{
			VisitExpression(*node.m_arr_var);
			VisitExpression(*node.m_index);
		}

		void Visit(const MidoriExpression::ArrayComprehension& node)
		{
			RecordSyntheticLocal(node.m_loop_variable_index);
			RecordSyntheticLocal(node.m_hidden_step_index);
			RecordSyntheticLocal(node.m_hidden_end_index);
			RecordSyntheticLocal(node.m_hidden_array_index);
			RecordSyntheticLocal(node.m_result_array_index);
			VisitExpression(*node.m_transform_expr);
			VisitExpression(*node.m_range);
		}

		void Visit(const MidoriExpression::RangeBinary& node)
		{
			VisitExpression(*node.m_start);
			VisitExpression(*node.m_end);
		}

		void Visit(const MidoriExpression::RangeTernary& node)
		{
			VisitExpression(*node.m_start);
			VisitExpression(*node.m_step);
			VisitExpression(*node.m_end);
		}

		void Visit(const MidoriExpression::Block& node)
		{
			for (const std::unique_ptr<MidoriStatement>& stmt : node.m_stmts)
			{
				VisitStatement(*stmt);
			}

			if (node.m_final_expr.has_value())
			{
				VisitExpression(*node.m_final_expr.value());
			}
		}

		void Visit(const MidoriExpression::Match& node)
		{
			RecordSyntheticLocal(node.m_match_value_index);
			VisitExpression(*node.m_arg_expr);
			for (const std::unique_ptr<MidoriExpression>& case_expr : node.m_cases)
			{
				VisitExpression(*case_expr);
			}
		}

		void Visit(const MidoriExpression::Case& node)
		{
			VisitPattern(*node.m_pattern);
			if (node.HasGuard())
			{
				VisitExpression(*node.m_guard.value());
			}
			VisitExpression(*node.m_expr);
		}

		void Visit(const MidoriExpression::For& node)
		{
			RecordSyntheticLocal(node.m_loop_variable_index);
			RecordSyntheticLocal(node.m_hidden_step_index);
			RecordSyntheticLocal(node.m_hidden_end_index);
			RecordSyntheticLocal(node.m_hidden_array_index);
			VisitExpression(*node.m_range);
			VisitExpression(*node.m_body);
		}
	};
}

namespace MidoriAnalysis
{
	const MidoriExpression* StripRedundantGroups(const MidoriExpression* expr)
	{
		const MidoriExpression* current = expr;
		while (current != nullptr && current->IsExpression<MidoriExpression::Group>())
		{
			current = current->GetExpression<MidoriExpression::Group>().m_expr_in.get();
		}
		return current;
	}

	std::unique_ptr<MidoriExpression> StripRedundantGroups(std::unique_ptr<MidoriExpression> expr)
	{
		while (expr && expr->IsExpression<MidoriExpression::Group>())
		{
			expr = std::move(expr->GetExpression<MidoriExpression::Group>().m_expr_in);
		}
		return expr;
	}

	bool IsLiteralExpression(const MidoriExpression& expr)
	{
		const MidoriExpression* stripped_expr = StripRedundantGroups(&expr);
		return stripped_expr != nullptr && stripped_expr->IsExpression<MidoriExpression::Literal>();
	}

	bool IsPure(const MidoriExpression& expr)
	{
		return IsPureExpression(expr);
	}

	std::optional<ConstantValue> TryEvalConstant(const MidoriExpression& expr)
	{
		return EvalConstant(expr);
	}

	std::optional<ConstantValue> TryEvalConstant(const MidoriExpression::Binary& expr)
	{
		return EvalBinary(expr);
	}

	std::optional<ConstantValue> TryEvalConstant(const MidoriExpression::UnaryPrefix& expr)
	{
		return EvalUnary(expr);
	}

	std::optional<bool> TryEvalTruthValue(const MidoriExpression& expr)
	{
		std::optional<ConstantValue> value = TryEvalConstant(expr);
		if (!value.has_value())
		{
			return std::nullopt;
		}

		return TryGetBoolValue(value.value());
	}

	std::unique_ptr<MidoriExpression> MakeLiteralExpression(const ConstantValue& value, const Token& source_token)
	{
		return std::visit
		(
			[&source_token](const auto& literal_value) -> std::unique_ptr<MidoriExpression>
			{
				using T = std::decay_t<decltype(literal_value)>;

				if constexpr (std::is_same_v<T, MidoriBool>)
				{
					if (literal_value)
					{
						return std::make_unique<MidoriExpression>(MidoriExpression::Literal(Token("true", Token::Name::TRUE, source_token), MidoriExpression::LiteralKind::Bool));
					}

					return std::make_unique<MidoriExpression>(MidoriExpression::Literal(Token("false", Token::Name::FALSE, source_token), MidoriExpression::LiteralKind::Bool));
				}
				else if constexpr (std::is_same_v<T, MidoriInteger>)
				{
					return std::make_unique<MidoriExpression>(MidoriExpression::Literal(Token(std::to_string(literal_value), Token::Name::INTEGER_LITERAL, source_token), MidoriExpression::LiteralKind::Integer));
				}
				else if constexpr (std::is_same_v<T, MidoriFloat>)
				{
					return std::make_unique<MidoriExpression>(MidoriExpression::Literal(Token(std::to_string(literal_value), Token::Name::FLOAT_LITERAL, source_token), MidoriExpression::LiteralKind::Float));
				}
				else if constexpr (std::is_same_v<T, MidoriByte>)
				{
					return std::make_unique<MidoriExpression>(MidoriExpression::Literal(Token(std::to_string(literal_value), Token::Name::INTEGER_LITERAL, source_token), MidoriExpression::LiteralKind::Byte));
				}
				else if constexpr (std::is_same_v<T, MidoriWord>)
				{
					return std::make_unique<MidoriExpression>(MidoriExpression::Literal(Token(std::to_string(literal_value), Token::Name::INTEGER_LITERAL, source_token), MidoriExpression::LiteralKind::Word));
				}
				else if constexpr (std::is_same_v<T, std::string>)
				{
					return std::make_unique<MidoriExpression>(MidoriExpression::Literal(Token(std::string(literal_value), Token::Name::TEXT_LITERAL, source_token), MidoriExpression::LiteralKind::Text));
				}
				else
				{
					return std::make_unique<MidoriExpression>(MidoriExpression::Literal(Token("()", Token::Name::UNIT, source_token), MidoriExpression::LiteralKind::Unit));
				}
			},
			value.m_value
		);
	}

	std::optional<std::size_t> TryEvalConstantIndex(const MidoriExpression& expr)
	{
		const std::optional<ConstantValue> value = TryEvalConstant(expr);
		if (!value.has_value())
		{
			return std::nullopt;
		}

		if (value->Is<MidoriInteger>())
		{
			const MidoriInteger index = value->Get<MidoriInteger>();
			return index >= 0ll ? std::optional<std::size_t>{ static_cast<std::size_t>(index) } : std::nullopt;
		}

		if (value->Is<MidoriByte>())
		{
			return static_cast<std::size_t>(value->Get<MidoriByte>());
		}

		if (value->Is<MidoriWord>())
		{
			return static_cast<std::size_t>(value->Get<MidoriWord>());
		}

		return std::nullopt;
	}

	bool IsKnownEmptyIterationSource(const MidoriExpression& expr)
	{
		const MidoriExpression* stripped_expr = StripRedundantGroups(&expr);
		if (stripped_expr == nullptr)
		{
			return false;
		}

		if (stripped_expr->IsExpression<MidoriExpression::Array>())
		{
			return stripped_expr->GetExpression<MidoriExpression::Array>().m_elems.empty();
		}

		if (stripped_expr->IsExpression<MidoriExpression::RangeBinary>())
		{
			const MidoriExpression::RangeBinary& range_expr = stripped_expr->GetExpression<MidoriExpression::RangeBinary>();
			const std::optional<ConstantValue> start = TryEvalConstant(*range_expr.m_start);
			const std::optional<ConstantValue> end = TryEvalConstant(*range_expr.m_end);
			if (!start.has_value() || !end.has_value())
			{
				return false;
			}

			if (start->Is<MidoriInteger>() && end->Is<MidoriInteger>())
			{
				return !(start->Get<MidoriInteger>() < end->Get<MidoriInteger>());
			}

			if (start->Is<MidoriFloat>() && end->Is<MidoriFloat>())
			{
				return !(start->Get<MidoriFloat>() < end->Get<MidoriFloat>());
			}

			if (start->Is<MidoriByte>() && end->Is<MidoriByte>())
			{
				return !(start->Get<MidoriByte>() < end->Get<MidoriByte>());
			}

			if (start->Is<MidoriWord>() && end->Is<MidoriWord>())
			{
				return !(start->Get<MidoriWord>() < end->Get<MidoriWord>());
			}

			return false;
		}

		if (!stripped_expr->IsExpression<MidoriExpression::RangeTernary>())
		{
			return false;
		}

		const MidoriExpression::RangeTernary& range_expr = stripped_expr->GetExpression<MidoriExpression::RangeTernary>();
		const std::optional<ConstantValue> start = TryEvalConstant(*range_expr.m_start);
		const std::optional<ConstantValue> step = TryEvalConstant(*range_expr.m_step);
		const std::optional<ConstantValue> end = TryEvalConstant(*range_expr.m_end);
		if (!start.has_value() || !step.has_value() || !end.has_value())
		{
			return false;
		}

		if (start->Is<MidoriInteger>() && step->Is<MidoriInteger>() && end->Is<MidoriInteger>())
		{
			return IsEmptyRange(start->Get<MidoriInteger>(), step->Get<MidoriInteger>(), end->Get<MidoriInteger>());
		}

		if (start->Is<MidoriFloat>() && step->Is<MidoriFloat>() && end->Is<MidoriFloat>())
		{
			return IsEmptyRange(start->Get<MidoriFloat>(), step->Get<MidoriFloat>(), end->Get<MidoriFloat>());
		}

		if (start->Is<MidoriByte>() && step->Is<MidoriByte>() && end->Is<MidoriByte>())
		{
			return IsEmptyRange(start->Get<MidoriByte>(), step->Get<MidoriByte>(), end->Get<MidoriByte>());
		}

		if (start->Is<MidoriWord>() && step->Is<MidoriWord>() && end->Is<MidoriWord>())
		{
			return IsEmptyRange(start->Get<MidoriWord>(), step->Get<MidoriWord>(), end->Get<MidoriWord>());
		}

		return false;
	}

	bool IsBindingFreePattern(const MidoriPattern& pattern)
	{
		return std::visit
		(
			[](const auto& node) -> bool
			{
				using T = std::decay_t<decltype(node)>;

				if constexpr (std::is_same_v<T, MidoriPattern::Binding>)
				{
					return false;
				}
				else if constexpr (std::is_same_v<T, MidoriPattern::Wildcard>)
				{
					return true;
				}
				else if constexpr (std::is_same_v<T, MidoriPattern::Literal>)
				{
					return true;
				}
				else if constexpr (std::is_same_v<T, MidoriPattern::Tuple> || std::is_same_v<T, MidoriPattern::Array>)
				{
					for (const std::unique_ptr<MidoriPattern>& element : node.m_elements)
					{
						if (!IsBindingFreePattern(*element))
						{
							return false;
						}
					}
					return true;
				}
				else if constexpr (std::is_same_v<T, MidoriPattern::Constructor>)
				{
					for (const std::unique_ptr<MidoriPattern>& arg : node.m_args)
					{
						if (!IsBindingFreePattern(*arg))
						{
							return false;
						}
					}
					return true;
				}
				else
				{
					return false;
				}
			},
			*pattern
		);
	}

	std::optional<bool> TryMatchPattern(const MidoriPattern& pattern, const MidoriExpression& expr)
	{
		return TryMatchPatternKnown(pattern, expr);
	}

	bool StatementLocalAccessSummary::UsesLocal(int local_index) const
	{
		return GetUseCount(local_index) > 0;
	}

	bool StatementLocalAccessSummary::AssignsLocal(int local_index) const
	{
		return GetAssignmentCount(local_index) > 0;
	}

	int StatementLocalAccessSummary::GetUseCount(int local_index) const
	{
		const auto it = m_locals.find(local_index);
		return it == m_locals.end() ? 0 : it->second.m_reads;
	}

	int StatementLocalAccessSummary::GetAssignmentCount(int local_index) const
	{
		const auto it = m_locals.find(local_index);
		return it == m_locals.end() ? 0 : it->second.m_assignments;
	}

	bool BlockLocalAccessSummary::IsLocalUsedAfter(int local_index, std::size_t statement_index) const
	{
		const std::size_t start_index = statement_index + 1u;
		for (std::size_t idx = start_index; idx < m_statement_summaries.size(); idx += 1u)
		{
			if (m_statement_summaries[idx].UsesLocal(local_index))
			{
				return true;
			}
		}

		return m_final_expression_summary.has_value() && m_final_expression_summary->UsesLocal(local_index);
	}

	bool BlockLocalAccessSummary::IsLocalAssignedAfter(int local_index, std::size_t statement_index) const
	{
		const std::size_t start_index = statement_index + 1u;
		for (std::size_t idx = start_index; idx < m_statement_summaries.size(); idx += 1u)
		{
			if (m_statement_summaries[idx].AssignsLocal(local_index))
			{
				return true;
			}
		}

		return m_final_expression_summary.has_value() && m_final_expression_summary->AssignsLocal(local_index);
	}

	StatementLocalAccessSummary AnalyzeStatementLocalAccess(const MidoriStatement& stmt)
	{
		StatementLocalAccessSummary summary;
		LocalAccessCollector collector(summary);
		collector.VisitStatement(stmt);
		return summary;
	}

	StatementLocalAccessSummary AnalyzeExpressionLocalAccess(const MidoriExpression& expr)
	{
		StatementLocalAccessSummary summary;
		LocalAccessCollector collector(summary);
		collector.VisitExpression(expr);
		return summary;
	}

	BlockLocalAccessSummary AnalyzeBlockLocalAccess(const MidoriExpression::Block& block)
	{
		BlockLocalAccessSummary summary;
		summary.m_statement_summaries.reserve(block.m_stmts.size());

		for (const std::unique_ptr<MidoriStatement>& stmt : block.m_stmts)
		{
			summary.m_statement_summaries.emplace_back(AnalyzeStatementLocalAccess(*stmt));
		}

		if (block.m_final_expr.has_value())
		{
			summary.m_final_expression_summary = AnalyzeExpressionLocalAccess(*block.m_final_expr.value());
		}

		return summary;
	}
}
