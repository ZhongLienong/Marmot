#pragma once

#include "Compiler/Analysis/AbstractSyntaxTreeWalker.h"
#include "Compiler/Analysis/SemanticFacts.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace MidoriAnalysis
{
	struct LiftSafetySummary
	{
		bool m_is_safe = true;
		bool m_uses_captured_cells = false;
		bool m_uses_implicit_global = false;
	};

	std::optional<int> TryGetLocalIndex(const MidoriExpression::NameContext::Tag& name_ctx);

	std::string DemangleDisplayName(std::string_view qualified_name);

	bool IsIgnoredBindingName(std::string_view name);


	const Token* GetPrimaryToken(const MidoriStatement& statement);

	const Token* GetPrimaryToken(const MidoriExpression& expression);

	LiftSafetySummary AnalyzeLiftSafety(MidoriExpression& expression, const std::unordered_set<std::string>& visible_globals, std::optional<std::string_view> self_name = std::nullopt);
}
