#include "SharedAnalysis.h"

#include "Common/Constant/Constant.h"

#include <type_traits>

namespace
{
	using MidoriAnalysis::LiftSafetySummary;

	const Token* GetPrimaryToken(const MidoriStatement& statement);
	const Token* GetPrimaryToken(const MidoriExpression& expression);

	class LiftSafetyWalker final : protected MidoriAbstractSyntaxTreeWalker
	{
	public:
		LiftSafetyWalker(const std::unordered_set<std::string>& visible_globals, std::optional<std::string_view> self_name)
			: m_visible_globals(&visible_globals),
			m_self_name(self_name)
		{
		}

		LiftSafetySummary Analyze(MidoriExpression& expression)
		{
			VisitExpression(expression);
			return m_summary;
		}

	protected:
		void operator()(MidoriExpression::NameAccess& access) override
		{
			CheckAccess(access.m_name, access.m_name_ctx);
		}

	private:
		void CheckAccess(const Token& name, const MidoriExpression::NameContext::Tag& context)
		{
			if (std::holds_alternative<MidoriExpression::NameContext::Cell>(context))
			{
				if (!m_self_name.has_value() || name.m_lexeme != m_self_name.value())
				{
					m_summary.m_uses_captured_cells = true;
					m_summary.m_is_safe = false;
				}
				return;
			}

			if (!std::holds_alternative<MidoriExpression::NameContext::Global>(context))
			{
				return;
			}

			if (name.m_lexeme.find(NameSeparator) == std::string::npos
				&& !m_visible_globals->contains(name.m_lexeme))
			{
				m_summary.m_uses_implicit_global = true;
				m_summary.m_is_safe = false;
			}
		}

		const std::unordered_set<std::string>* m_visible_globals = nullptr;
		std::optional<std::string_view> m_self_name;
		LiftSafetySummary m_summary;
	};

	const Token* GetPrimaryToken(const MidoriStatement& statement)
	{
		return std::visit
		(
			[](const auto& node) -> const Token*
			{
				using T = std::decay_t<decltype(node)>;

				if constexpr (std::is_same_v<T, MidoriStatement::ExpressionStatement>)
				{
					return node.m_expr != nullptr ? GetPrimaryToken(*node.m_expr) : &node.m_semicolon;
				}
				else if constexpr (std::is_same_v<T, MidoriStatement::VariableDefinition>)
				{
					return &node.m_name;
				}
				else if constexpr (std::is_same_v<T, MidoriStatement::TupleDefinition>)
				{
					return !node.m_names.empty() ? &node.m_names.front() : (node.m_value != nullptr ? GetPrimaryToken(*node.m_value) : nullptr);
				}
				else if constexpr (std::is_same_v<T, MidoriStatement::FunctionDefinition>)
				{
					return &node.m_name;
				}
				else if constexpr (std::is_same_v<T, MidoriStatement::ForeignDefinition>)
				{
					return &node.m_function_name;
				}
				else if constexpr (std::is_same_v<T, MidoriStatement::Struct>)
				{
					return &node.m_name;
				}
				else if constexpr (std::is_same_v<T, MidoriStatement::Union>)
				{
					return &node.m_name;
				}
				else if constexpr (std::is_same_v<T, MidoriStatement::Class>)
				{
					return &node.m_name;
				}
				else if constexpr (std::is_same_v<T, MidoriStatement::Instance>)
				{
					return &node.m_class_name;
				}
				else
				{
					return &node.m_name;
				}
			},
			*statement
		);
	}

	const Token* GetPrimaryToken(const MidoriExpression& expression)
	{
		return std::visit
		(
			[](const auto& node) -> const Token*
			{
				using T = std::decay_t<decltype(node)>;

				if constexpr (std::is_same_v<T, MidoriExpression::As>)
				{
					return &node.m_as_keyword;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Binary>)
				{
					return &node.m_op;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Group>)
				{
					return node.m_expr_in != nullptr ? GetPrimaryToken(*node.m_expr_in) : nullptr;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Tuple>)
				{
					return &node.m_op;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Literal>)
				{
					return &node.m_token;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::UnaryPrefix> || std::is_same_v<T, MidoriExpression::UnarySuffix>)
				{
					return &node.m_op;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::NameAccess>)
				{
					return &node.m_name;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Call>)
				{
					return node.m_callee != nullptr ? GetPrimaryToken(*node.m_callee) : &node.m_paren;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Function>)
				{
					return &node.m_function_keyword;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Construct>)
				{
					return &node.m_data_name;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::RecordUpdate>)
				{
					// Spelled out because this chain ends in `else { return nullptr; }`: without
					// it every diagnostic anchored on a record update would lose its location.
					return &node.m_with_keyword;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::IfElse>)
				{
					return &node.m_if_token;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::MemberAccess>)
				{
					return &node.m_member_name;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Array>)
				{
					return &node.m_op;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::IndexAccess>)
				{
					return node.m_arr_var != nullptr ? GetPrimaryToken(*node.m_arr_var) : &node.m_op;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::ArrayComprehension>)
				{
					return &node.m_bracket;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::RangeBinary>)
				{
					return &node.m_range_op;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::RangeTernary>)
				{
					return &node.m_first_range_op;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Block>)
				{
					if (!node.m_stmts.empty())
					{
						return GetPrimaryToken(*node.m_stmts.front());
					}
					if (node.m_final_expr.has_value())
					{
						return GetPrimaryToken(*node.m_final_expr.value());
					}
					return &node.m_right_brace;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Match>)
				{
					return &node.m_match_keyword;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::Case>)
				{
					return &node.m_keyword;
				}
				else if constexpr (std::is_same_v<T, MidoriExpression::For>)
				{
					return &node.m_for_keyword;
				}
				else
				{
					return nullptr;
				}
			},
			*expression
		);
	}
}

namespace MidoriAnalysis
{
	std::optional<int> TryGetLocalIndex(const MidoriExpression::NameContext::Tag& name_ctx)
	{
		if (!std::holds_alternative<MidoriExpression::NameContext::Local>(name_ctx))
		{
			return std::nullopt;
		}

		return std::get<MidoriExpression::NameContext::Local>(name_ctx).m_index;
	}

	std::string DemangleDisplayName(std::string_view qualified_name)
	{
		const size_t separator_pos = qualified_name.rfind(NameSeparator);
		if (separator_pos == std::string_view::npos)
		{
			return std::string(qualified_name);
		}

		return std::string(qualified_name.substr(separator_pos + NameSeparator.length()));
	}

	bool IsIgnoredBindingName(std::string_view name)
	{
		return !name.empty() && name[0u] == '_';
	}

	const Token* GetPrimaryToken(const MidoriStatement& statement)
	{
		return ::GetPrimaryToken(statement);
	}

	const Token* GetPrimaryToken(const MidoriExpression& expression)
	{
		return ::GetPrimaryToken(expression);
	}

	LiftSafetySummary AnalyzeLiftSafety(MidoriExpression& expression, const std::unordered_set<std::string>& visible_globals, std::optional<std::string_view> self_name)
	{
		return LiftSafetyWalker(visible_globals, self_name).Analyze(expression);
	}
}
