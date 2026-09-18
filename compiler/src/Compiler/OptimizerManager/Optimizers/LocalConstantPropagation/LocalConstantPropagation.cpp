#include "LocalConstantPropagation.h"

#include <type_traits>

namespace
{
	std::optional<int> TryGetLocalIndex(const MidoriExpression::NameContext::Tag& name_ctx)
	{
		if (!std::holds_alternative<MidoriExpression::NameContext::Local>(name_ctx))
		{
			return std::nullopt;
		}

		return std::get<MidoriExpression::NameContext::Local>(name_ctx).m_index;
	}
}

MidoriResult::OptimizerResult LocalConstantPropagation::Optimize(MidoriProgramTree program_tree)
{
	ResetPassState();
	m_environments.clear();
	PushEnvironment({});

	ProcessStatements(program_tree);

	PopEnvironment();
	return std::move(program_tree);
}

std::string_view LocalConstantPropagation::GetName() const
{
	return "LocalConstantPropagation";
}

LocalConstantPropagation::Environment& LocalConstantPropagation::CurrentEnvironment()
{
	return m_environments.back();
}

const LocalConstantPropagation::Environment& LocalConstantPropagation::CurrentEnvironment() const
{
	return m_environments.back();
}

void LocalConstantPropagation::PushEnvironment(Environment&& environment)
{
	m_environments.emplace_back(std::move(environment));
}

LocalConstantPropagation::Environment LocalConstantPropagation::PopEnvironment()
{
	Environment environment = std::move(m_environments.back());
	m_environments.pop_back();
	return environment;
}

void LocalConstantPropagation::ProcessStatements(std::vector<std::unique_ptr<MidoriStatement>>& statements)
{
	for (std::unique_ptr<MidoriStatement>& statement : statements)
	{
		VisitStatement(statement);
		FinalizeStatement(*statement);
	}
}

void LocalConstantPropagation::FinalizeStatement(MidoriStatement& statement)
{
	std::visit
	(
		[this](auto& node)
		{
			Finalize(node);
		},
		*statement
	);
}

void LocalConstantPropagation::Finalize(const MidoriStatement::ExpressionStatement&)
{
}

void LocalConstantPropagation::Finalize(const MidoriStatement::VariableDefinition& def)
{
	if (!def.m_local_index.has_value())
	{
		return;
	}

	const int local_index = def.m_local_index.value();
	InvalidateLocal(local_index);

	if (def.m_is_elided)
	{
		return;
	}

	const std::optional<Replacement> replacement = TryCreateReplacement(*def.m_value);
	if (!replacement.has_value())
	{
		return;
	}

	if (std::holds_alternative<AliasReplacement>(replacement->m_value))
	{
		const AliasReplacement& alias = std::get<AliasReplacement>(replacement->m_value);
		if (alias.m_local_index == local_index)
		{
			return;
		}
	}

	CurrentEnvironment()[local_index] = replacement.value();
}

void LocalConstantPropagation::Finalize(const MidoriStatement::TupleDefinition& def_tuple)
{
	for (const std::optional<int>& local_index : def_tuple.m_local_indices)
	{
		if (local_index.has_value())
		{
			InvalidateLocal(local_index.value());
		}
	}
}

void LocalConstantPropagation::Finalize(const MidoriStatement::FunctionDefinition& defun)
{
	if (defun.m_local_index.has_value())
	{
		InvalidateLocal(defun.m_local_index.value());
	}

	ClearAllReplacements();
}

void LocalConstantPropagation::Finalize(const MidoriStatement::ForeignDefinition& foreign)
{
	if (foreign.m_local_index.has_value())
	{
		InvalidateLocal(foreign.m_local_index.value());
	}
}

void LocalConstantPropagation::Finalize(const MidoriStatement::Struct&)
{
}

void LocalConstantPropagation::Finalize(const MidoriStatement::Union&)
{
}

void LocalConstantPropagation::Finalize(const MidoriStatement::Class&)
{
	ClearAllReplacements();
}

void LocalConstantPropagation::Finalize(const MidoriStatement::Instance&)
{
	ClearAllReplacements();
}

void LocalConstantPropagation::Finalize(const MidoriStatement::TypeAlias&)
{
}

LocalConstantPropagation::Environment LocalConstantPropagation::VisitInEnvironment(std::unique_ptr<MidoriExpression>& expr, const Environment& environment)
{
	PushEnvironment(Environment(environment));
	VisitAndReplace(expr);
	return PopEnvironment();
}

void LocalConstantPropagation::VisitInFreshEnvironment(std::unique_ptr<MidoriExpression>& expr)
{
	PushEnvironment({});
	VisitAndReplace(expr);
	PopEnvironment();
}

void LocalConstantPropagation::InvalidateLocal(int local_index)
{
	Environment& environment = CurrentEnvironment();
	environment.erase(local_index);

	for (Environment::iterator it = environment.begin(); it != environment.end();)
	{
		if (std::holds_alternative<AliasReplacement>(it->second.m_value)
			&& std::get<AliasReplacement>(it->second.m_value).m_local_index == local_index)
		{
			it = environment.erase(it);
		}
		else
		{
			++it;
		}
	}
}

void LocalConstantPropagation::ClearAllReplacements()
{
	if (!m_environments.empty())
	{
		CurrentEnvironment().clear();
	}
}

void LocalConstantPropagation::ClearAliasReplacements()
{
	if (m_environments.empty())
	{
		return;
	}

	Environment& environment = CurrentEnvironment();
	for (Environment::iterator it = environment.begin(); it != environment.end();)
	{
		if (std::holds_alternative<AliasReplacement>(it->second.m_value))
		{
			it = environment.erase(it);
		}
		else
		{
			++it;
		}
	}
}

LocalConstantPropagation::Environment LocalConstantPropagation::FilterRepeatedEnvironment(const Environment& environment, const MidoriExpression& expr) const
{
	const MidoriAnalysis::StatementLocalAccessSummary access_summary = MidoriAnalysis::AnalyzeExpressionLocalAccess(expr);
	Environment filtered_environment;

	for (const std::pair<const int, Replacement>& entry : environment)
	{
		const int local_index = entry.first;
		const Replacement& replacement = entry.second;

		if (access_summary.AssignsLocal(local_index))
		{
			continue;
		}

		bool replacement_depends_on_assigned_local = false;
		for (const std::pair<const int, MidoriAnalysis::LocalAccessInfo>& access_entry : access_summary.m_locals)
		{
			if (access_entry.second.m_assignments <= 0)
			{
				continue;
			}

			if (ReplacementDependsOnLocal(replacement, access_entry.first))
			{
				replacement_depends_on_assigned_local = true;
				break;
			}
		}

		if (!replacement_depends_on_assigned_local)
		{
			filtered_environment.emplace(local_index, replacement);
		}
	}

	return filtered_environment;
}

std::optional<LocalConstantPropagation::Replacement> LocalConstantPropagation::TryCreateReplacement(const MidoriExpression& expr) const
{
	const MidoriExpression* stripped_expr = MidoriAnalysis::StripRedundantGroups(&expr);
	if (stripped_expr == nullptr)
	{
		return std::nullopt;
	}

	if (MidoriAnalysis::IsLiteralExpression(*stripped_expr))
	{
		const std::optional<MidoriAnalysis::ConstantValue> constant_value = MidoriAnalysis::TryEvalConstant(*stripped_expr);
		if (constant_value.has_value())
		{
			return Replacement{ constant_value.value() };
		}
	}

	if (!stripped_expr->IsExpression<MidoriExpression::NameAccess>())
	{
		return std::nullopt;
	}

	const MidoriExpression::NameAccess& name_access = stripped_expr->GetExpression<MidoriExpression::NameAccess>();
	const std::optional<int> local_index = TryGetLocalIndex(name_access.m_name_ctx);
	if (!local_index.has_value())
	{
		return std::nullopt;
	}

	return Replacement{ AliasReplacement{ name_access.m_name, local_index.value() } };
}

std::unique_ptr<MidoriExpression> LocalConstantPropagation::MaterializeReplacement(const Replacement& replacement, const MidoriExpression::NameAccess& use_site) const
{
	return std::visit
	(
		[&use_site](const auto& value) -> std::unique_ptr<MidoriExpression>
		{
			using T = std::decay_t<decltype(value)>;

			if constexpr (std::is_same_v<T, MidoriAnalysis::ConstantValue>)
			{
				std::unique_ptr<MidoriExpression> replacement_expr = MidoriAnalysis::MakeLiteralExpression(value, use_site.m_name);
				replacement_expr->GetType() = use_site.m_type_data;
				return replacement_expr;
			}
			else
			{
				std::unique_ptr<MidoriExpression> replacement_expr = std::make_unique<MidoriExpression>(MidoriExpression::NameAccess(value.m_name, MidoriExpression::NameContext::Local{ value.m_local_index }));
				replacement_expr->GetType() = use_site.m_type_data;
				return replacement_expr;
			}
		},
		replacement.m_value
	);
}

bool LocalConstantPropagation::ReplacementDependsOnLocal(const Replacement& replacement, int local_index)
{
	if (!std::holds_alternative<AliasReplacement>(replacement.m_value))
	{
		return false;
	}

	return std::get<AliasReplacement>(replacement.m_value).m_local_index == local_index;
}

bool LocalConstantPropagation::AreEquivalent(const Replacement& left, const Replacement& right)
{
	if (left.m_value.index() != right.m_value.index())
	{
		return false;
	}

	if (std::holds_alternative<MidoriAnalysis::ConstantValue>(left.m_value))
	{
		return std::get<MidoriAnalysis::ConstantValue>(left.m_value).m_value
			== std::get<MidoriAnalysis::ConstantValue>(right.m_value).m_value;
	}

	return std::get<AliasReplacement>(left.m_value).m_local_index
		== std::get<AliasReplacement>(right.m_value).m_local_index;
}

LocalConstantPropagation::Environment LocalConstantPropagation::IntersectEnvironments(const Environment& baseline, const Environment& left, const Environment& right)
{
	Environment merged;

	for (const auto& [local_index, replacement] : baseline)
	{
		const Environment::const_iterator left_it = left.find(local_index);
		const Environment::const_iterator right_it = right.find(local_index);
		if (left_it == left.end() || right_it == right.end())
		{
			continue;
		}

		if (!AreEquivalent(left_it->second, right_it->second))
		{
			continue;
		}

		merged.emplace(local_index, replacement);
	}

	return merged;
}

void LocalConstantPropagation::MergeParentEnvironment(const Environment& incoming, const Environment& completed)
{
	if (m_environments.empty())
	{
		return;
	}

	Environment& parent = CurrentEnvironment();
	for (const auto& [local_index, replacement] : incoming)
	{
		const Environment::const_iterator it = completed.find(local_index);
		if (it == completed.end() || !AreEquivalent(replacement, it->second))
		{
			parent.erase(local_index);
		}
	}
}

void LocalConstantPropagation::operator()(MidoriStatement::FunctionDefinition& defun)
{
	VisitInFreshEnvironment(defun.m_body);
}

void LocalConstantPropagation::operator()(MidoriExpression::Binary& binary)
{
	VisitAndReplace(binary.m_left);

	if (binary.m_op.m_token_name == Token::Name::DOUBLE_AMPERSAND
		|| binary.m_op.m_token_name == Token::Name::DOUBLE_BAR)
	{
		const Environment after_left = CurrentEnvironment();
		const Environment after_right = VisitInEnvironment(binary.m_right, after_left);
		CurrentEnvironment() = IntersectEnvironments(after_left, after_left, after_right);
		return;
	}

	VisitAndReplace(binary.m_right);
}

void LocalConstantPropagation::operator()(MidoriExpression::NameAccess& variable)
{
	if (m_environments.empty())
	{
		return;
	}

	const std::optional<int> local_index = TryGetLocalIndex(variable.m_name_ctx);
	if (!local_index.has_value())
	{
		return;
	}

	const Environment::const_iterator it = CurrentEnvironment().find(local_index.value());
	if (it == CurrentEnvironment().end())
	{
		return;
	}

	m_pending_replacement = MaterializeReplacement(it->second, variable);
}

void LocalConstantPropagation::operator()(MidoriExpression::Call& call)
{
	VisitAndReplace(call.m_callee);

	for (std::unique_ptr<MidoriExpression>& argument : call.m_arguments)
	{
		VisitAndReplace(argument);
	}

	ClearAllReplacements();
}

void LocalConstantPropagation::operator()(MidoriExpression::Function& function)
{
	VisitInFreshEnvironment(function.m_body);
	ClearAllReplacements();
}

void LocalConstantPropagation::operator()(MidoriExpression::ArrayComprehension& comp)
{
	VisitAndReplace(comp.m_range);

	const Environment after_range = CurrentEnvironment();
	const Environment repeated_environment = FilterRepeatedEnvironment(after_range, *comp.m_transform_expr);
	const Environment after_transform = VisitInEnvironment(comp.m_transform_expr, repeated_environment);
	CurrentEnvironment() = IntersectEnvironments(after_range, after_range, after_transform);
}

void LocalConstantPropagation::operator()(MidoriExpression::IfElse& if_else)
{
	VisitAndReplace(if_else.m_condition);

	const Environment after_condition = CurrentEnvironment();
	const Environment after_true = VisitInEnvironment(if_else.m_true_branch, after_condition);
	const Environment after_else = VisitInEnvironment(if_else.m_else_branch, after_condition);
	CurrentEnvironment() = IntersectEnvironments(after_condition, after_true, after_else);
}

void LocalConstantPropagation::operator()(MidoriExpression::Block& block)
{
	const Environment incoming = CurrentEnvironment();
	PushEnvironment(Environment(incoming));

	ProcessStatements(block.m_stmts);

	if (block.m_final_expr.has_value())
	{
		VisitAndReplace(block.m_final_expr.value());
	}

	const Environment completed = PopEnvironment();
	MergeParentEnvironment(incoming, completed);
}

void LocalConstantPropagation::operator()(MidoriExpression::Match& match)
{
	VisitAndReplace(match.m_arg_expr);

	const Environment after_arg = CurrentEnvironment();
	Environment merged = after_arg;
	bool has_case = false;

	for (std::unique_ptr<MidoriExpression>& case_expr : match.m_cases)
	{
		const Environment after_case = VisitInEnvironment(case_expr, after_arg);
		if (!has_case)
		{
			merged = after_case;
			has_case = true;
		}
		else
		{
			merged = IntersectEnvironments(after_arg, merged, after_case);
		}
	}

	if (has_case)
	{
		CurrentEnvironment() = std::move(merged);
	}
}

void LocalConstantPropagation::operator()(MidoriExpression::For& for_expr)
{
	VisitAndReplace(for_expr.m_range);

	const Environment after_range = CurrentEnvironment();
	const Environment repeated_environment = FilterRepeatedEnvironment(after_range, *for_expr.m_body);
	const Environment after_body = VisitInEnvironment(for_expr.m_body, repeated_environment);
	CurrentEnvironment() = IntersectEnvironments(after_range, after_range, after_body);
}
