#include "FunctionInlining.h"

#include <algorithm>

MidoriResult::OptimizerResult FunctionInlining::Optimize(MidoriProgramTree program_tree)
{
	ResetPassState();
	m_candidates.clear();
	CollectCandidates(program_tree);

	if (!m_candidates.empty())
	{
		std::ranges::for_each
		(
			program_tree,
			[this](std::unique_ptr<MidoriStatement>& stmt)
			{
				VisitStatement(stmt);
			}
		);
	}

	return std::move(program_tree);
}

std::string_view FunctionInlining::GetName() const
{
	return "FunctionInlining";
}

void FunctionInlining::CollectCandidates(const MidoriProgramTree& program_tree)
{
	for (const std::unique_ptr<MidoriStatement>& stmt : program_tree)
	{
		if (!stmt->IsStatement<MidoriStatement::FunctionDefinition>())
		{
			continue;
		}

		const MidoriStatement::FunctionDefinition& defun = stmt->GetStatement<MidoriStatement::FunctionDefinition>();
		if (!defun.m_generic_params.empty()
			|| !defun.m_constraints.empty()
			|| defun.m_captured_count != 0
			|| defun.m_is_lift_wrapper
			|| defun.m_body == nullptr)
		{
			continue;
		}

		const MidoriExpression* body = defun.m_body.get();
		int node_budget = s_max_body_nodes;
		const int arity = static_cast<int>(defun.m_params.size());
		InlineCandidate candidate;
		candidate.m_defun = &defun;
		candidate.m_body = body;
		candidate.m_arity = arity;
		candidate.m_param_use_counts.assign(static_cast<size_t>(arity), 0);
		candidate.m_param_use_conditional.assign(static_cast<size_t>(arity), false);
		if (!IsInlinableBody(*body, candidate, node_budget, false))
		{
			continue;
		}

		m_candidates[defun.m_name.m_lexeme] = std::move(candidate);
	}
}

bool FunctionInlining::IsInlinableBody(const MidoriExpression& expr, InlineCandidate& candidate, int& node_budget, bool conditional)
{
	node_budget -= 1;
	if (node_budget < 0)
	{
		return false;
	}

	if (expr.IsExpression<MidoriExpression::Literal>())
	{
		return true;
	}

	if (expr.IsExpression<MidoriExpression::NameAccess>())
	{
		const MidoriExpression::NameAccess& access = expr.GetExpression<MidoriExpression::NameAccess>();
		if (const MidoriExpression::NameContext::Local* local = std::get_if<MidoriExpression::NameContext::Local>(&access.m_name_ctx))
		{
			if (local->m_index < 0 || local->m_index >= candidate.m_arity)
			{
				return false;
			}

			const size_t param = static_cast<size_t>(local->m_index);
			candidate.m_param_use_counts[param] += 1;
			if (conditional)
			{
				candidate.m_param_use_conditional[param] = true;
			}
			return true;
		}

		return std::holds_alternative<MidoriExpression::NameContext::Global>(access.m_name_ctx);
	}

	if (expr.IsExpression<MidoriExpression::Binary>())
	{
		const MidoriExpression::Binary& binary = expr.GetExpression<MidoriExpression::Binary>();
		if (binary.m_uses_concatenable || binary.m_uses_equatable || binary.m_uses_orderable)
		{
			return false;
		}

		return IsInlinableBody(*binary.m_left, candidate, node_budget, conditional) && IsInlinableBody(*binary.m_right, candidate, node_budget, conditional);
	}

	if (expr.IsExpression<MidoriExpression::Group>())
	{
		return IsInlinableBody(*expr.GetExpression<MidoriExpression::Group>().m_expr_in, candidate, node_budget, conditional);
	}

	if (expr.IsExpression<MidoriExpression::UnaryPrefix>())
	{
		const MidoriExpression::UnaryPrefix& unary = expr.GetExpression<MidoriExpression::UnaryPrefix>();
		if (unary.m_uses_countable)
		{
			return false;
		}

		return IsInlinableBody(*unary.m_expr, candidate, node_budget, conditional);
	}

	if (expr.IsExpression<MidoriExpression::As>())
	{
		const MidoriExpression::As& as = expr.GetExpression<MidoriExpression::As>();
		if (as.m_uses_convertable)
		{
			return false;
		}

		return IsInlinableBody(*as.m_expr, candidate, node_budget, conditional);
	}

	if (expr.IsExpression<MidoriExpression::IfElse>())
	{
		const MidoriExpression::IfElse& if_else = expr.GetExpression<MidoriExpression::IfElse>();
		if (if_else.m_condition == nullptr || if_else.m_true_branch == nullptr || if_else.m_else_branch == nullptr)
		{
			return false;
		}

		return IsInlinableBody(*if_else.m_condition, candidate, node_budget, conditional)
			&& IsInlinableBody(*if_else.m_true_branch, candidate, node_budget, true)
			&& IsInlinableBody(*if_else.m_else_branch, candidate, node_budget, true);
	}

	return false;
}

bool FunctionInlining::IsSimpleArgument(const MidoriExpression& expr)
{
	return expr.IsExpression<MidoriExpression::Literal>()
		|| expr.IsExpression<MidoriExpression::NameAccess>();
}

bool FunctionInlining::IsSubstitutablePureArgument(const MidoriExpression& expr)
{
	if (IsSimpleArgument(expr))
	{
		return true;
	}

	if (expr.IsExpression<MidoriExpression::Binary>())
	{
		const MidoriExpression::Binary& binary = expr.GetExpression<MidoriExpression::Binary>();
		if (binary.m_uses_concatenable || binary.m_uses_equatable || binary.m_uses_orderable)
		{
			return false;
		}

		// Division and modulo can fault; substitution may reorder evaluation
		// relative to other arguments, so keep faulting operators out.
		if (binary.m_op.m_token_name == Token::Name::SLASH || binary.m_op.m_token_name == Token::Name::PERCENT)
		{
			return false;
		}

		return IsSubstitutablePureArgument(*binary.m_left) && IsSubstitutablePureArgument(*binary.m_right);
	}

	if (expr.IsExpression<MidoriExpression::Group>())
	{
		return IsSubstitutablePureArgument(*expr.GetExpression<MidoriExpression::Group>().m_expr_in);
	}

	if (expr.IsExpression<MidoriExpression::UnaryPrefix>())
	{
		const MidoriExpression::UnaryPrefix& unary = expr.GetExpression<MidoriExpression::UnaryPrefix>();
		return !unary.m_uses_countable && IsSubstitutablePureArgument(*unary.m_expr);
	}

	return false;
}

std::unique_ptr<MidoriExpression> FunctionInlining::CloneSimple(const MidoriExpression& expr)
{
	std::unique_ptr<MidoriExpression> clone = nullptr;
	if (expr.IsExpression<MidoriExpression::Literal>())
	{
		const MidoriExpression::Literal& literal = expr.GetExpression<MidoriExpression::Literal>();
		clone = std::make_unique<MidoriExpression>(MidoriExpression::Literal(literal.m_token, literal.m_kind));
	}
	else if (expr.IsExpression<MidoriExpression::NameAccess>())
	{
		const MidoriExpression::NameAccess& access = expr.GetExpression<MidoriExpression::NameAccess>();
		clone = std::make_unique<MidoriExpression>(MidoriExpression::NameAccess(access.m_name, MidoriExpression::NameContext::Tag(access.m_name_ctx)));
	}

	if (clone != nullptr)
	{
		clone->GetType() = expr.GetType();
	}
	return clone;
}

std::unique_ptr<MidoriExpression> FunctionInlining::CloneWithSubstitution(const MidoriExpression& expr, const std::vector<std::unique_ptr<MidoriExpression>>* arguments, int param_offset)
{
	if (expr.IsExpression<MidoriExpression::NameAccess>())
	{
		const MidoriExpression::NameAccess& access = expr.GetExpression<MidoriExpression::NameAccess>();
		const MidoriExpression::NameContext::Local* local = std::get_if<MidoriExpression::NameContext::Local>(&access.m_name_ctx);
		if (arguments != nullptr && local != nullptr)
		{
			const MidoriExpression& argument = *(*arguments)[static_cast<size_t>(local->m_index)];
			return IsSimpleArgument(argument) ? CloneSimple(argument) : CloneWithSubstitution(argument, nullptr);
		}

		if (local != nullptr && param_offset > 0)
		{
			std::unique_ptr<MidoriExpression> remapped = std::make_unique<MidoriExpression>(MidoriExpression::NameAccess(access.m_name, MidoriExpression::NameContext::Local{ local->m_index + param_offset }));
			remapped->GetType() = expr.GetType();
			return remapped;
		}

		return CloneSimple(expr);
	}

	if (expr.IsExpression<MidoriExpression::Binary>())
	{
		const MidoriExpression::Binary& binary = expr.GetExpression<MidoriExpression::Binary>();
		std::unique_ptr<MidoriExpression> left = CloneWithSubstitution(*binary.m_left, arguments, param_offset);
		std::unique_ptr<MidoriExpression> right = CloneWithSubstitution(*binary.m_right, arguments, param_offset);
		if (left == nullptr || right == nullptr)
		{
			return nullptr;
		}

		MidoriExpression::Binary copy(binary.m_op, std::move(left), std::move(right));
		copy.m_uses_concatenable = binary.m_uses_concatenable;
		copy.m_uses_equatable = binary.m_uses_equatable;
		copy.m_uses_orderable = binary.m_uses_orderable;
		copy.m_type_data = binary.m_type_data;
		return std::make_unique<MidoriExpression>(std::move(copy));
	}

	if (expr.IsExpression<MidoriExpression::Group>())
	{
		std::unique_ptr<MidoriExpression> inner = CloneWithSubstitution(*expr.GetExpression<MidoriExpression::Group>().m_expr_in, arguments, param_offset);
		if (inner == nullptr)
		{
			return nullptr;
		}

		MidoriExpression::Group copy(std::move(inner));
		copy.m_type_data = expr.GetType();
		return std::make_unique<MidoriExpression>(std::move(copy));
	}

	if (expr.IsExpression<MidoriExpression::UnaryPrefix>())
	{
		const MidoriExpression::UnaryPrefix& unary = expr.GetExpression<MidoriExpression::UnaryPrefix>();
		std::unique_ptr<MidoriExpression> inner = CloneWithSubstitution(*unary.m_expr, arguments, param_offset);
		if (inner == nullptr)
		{
			return nullptr;
		}

		MidoriExpression::UnaryPrefix copy(unary.m_op, std::move(inner));
		copy.m_uses_countable = unary.m_uses_countable;
		copy.m_type_data = unary.m_type_data;
		return std::make_unique<MidoriExpression>(std::move(copy));
	}

	if (expr.IsExpression<MidoriExpression::As>())
	{
		const MidoriExpression::As& as = expr.GetExpression<MidoriExpression::As>();
		std::unique_ptr<MidoriExpression> inner = CloneWithSubstitution(*as.m_expr, arguments, param_offset);
		if (inner == nullptr)
		{
			return nullptr;
		}

		MidoriExpression::As copy(as.m_as_keyword, std::shared_ptr<MidoriType>(as.m_to_type), std::move(inner));
		copy.m_from_type = as.m_from_type;
		copy.m_uses_convertable = as.m_uses_convertable;
		copy.m_type_data = as.m_type_data;
		return std::make_unique<MidoriExpression>(std::move(copy));
	}

	if (expr.IsExpression<MidoriExpression::IfElse>())
	{
		const MidoriExpression::IfElse& if_else = expr.GetExpression<MidoriExpression::IfElse>();
		std::unique_ptr<MidoriExpression> condition = CloneWithSubstitution(*if_else.m_condition, arguments, param_offset);
		std::unique_ptr<MidoriExpression> true_branch = CloneWithSubstitution(*if_else.m_true_branch, arguments, param_offset);
		std::unique_ptr<MidoriExpression> else_branch = CloneWithSubstitution(*if_else.m_else_branch, arguments, param_offset);
		if (condition == nullptr || true_branch == nullptr || else_branch == nullptr)
		{
			return nullptr;
		}

		MidoriExpression::IfElse copy(if_else.m_if_token, if_else.m_then_token, if_else.m_else_token, std::move(condition), std::move(true_branch), std::move(else_branch), if_else.m_condition_operand_type);
		copy.m_type_data = if_else.m_type_data;
		return std::make_unique<MidoriExpression>(std::move(copy));
	}

	return CloneSimple(expr);
}

void FunctionInlining::operator()(MidoriExpression::Call& call)
{
	VisitAndReplace(call.m_callee);
	std::ranges::for_each
	(
		call.m_arguments,
		[this](std::unique_ptr<MidoriExpression>& argument)
		{
			VisitAndReplace(argument);
		}
	);

	if (call.m_is_foreign || !call.m_callee->IsExpression<MidoriExpression::NameAccess>())
	{
		return;
	}

	bool arguments_ok = false;
	const InlineCandidate* candidate = FindSubstitutionCandidate(call, arguments_ok);
	if (candidate == nullptr || !arguments_ok)
	{
		return;
	}

	std::unique_ptr<MidoriExpression> inlined = CloneWithSubstitution(*candidate->m_body, &call.m_arguments);
	if (inlined == nullptr)
	{
		return;
	}

	m_pending_replacement = std::move(inlined);
	MarkOptimization();
}

const FunctionInlining::InlineCandidate* FunctionInlining::FindSubstitutionCandidate(const MidoriExpression::Call& call, bool& arguments_ok) const
{
	arguments_ok = false;
	if (call.m_is_foreign || !call.m_callee->IsExpression<MidoriExpression::NameAccess>())
	{
		return nullptr;
	}

	const MidoriExpression::NameAccess& callee = call.m_callee->GetExpression<MidoriExpression::NameAccess>();
	if (!std::holds_alternative<MidoriExpression::NameContext::Global>(callee.m_name_ctx))
	{
		return nullptr;
	}

	std::unordered_map<std::string, InlineCandidate>::const_iterator candidate_it = m_candidates.find(callee.m_name.m_lexeme);
	if (candidate_it == m_candidates.end())
	{
		return nullptr;
	}

	const InlineCandidate& candidate = candidate_it->second;
	if (candidate.m_arity != static_cast<int>(call.m_arguments.size()))
	{
		return nullptr;
	}

	arguments_ok = true;
	for (size_t param = 0u; param < call.m_arguments.size(); param += 1u)
	{
		const MidoriExpression& argument = *call.m_arguments[param];
		if (IsSimpleArgument(argument))
		{
			continue;
		}

		// A non-trivial argument may only be substituted where the body reads
		// the parameter exactly once and never behind a branch.
		if (candidate.m_param_use_counts[param] == 1
			&& !candidate.m_param_use_conditional[param]
			&& IsSubstitutablePureArgument(argument))
		{
			continue;
		}

		arguments_ok = false;
		break;
	}

	return &candidate;
}

// Binding inlining: when substitution is not possible but the call sits at a
// clean-stack position (statement expression, definition value, block final,
// or return value) with a known local depth, bind each argument to a fresh
// local in a block and remap the body's parameter reads onto those slots.
// Operand positions are excluded: locals declared while temporaries are on
// the stack do not line up with their frame slots.
void FunctionInlining::TryBindingInline(std::unique_ptr<MidoriExpression>& expr)
{
	if (!m_depth_known || expr == nullptr || !expr->IsExpression<MidoriExpression::Call>())
	{
		return;
	}

	MidoriExpression::Call& call = expr->GetExpression<MidoriExpression::Call>();
	bool arguments_ok = false;
	const InlineCandidate* candidate = FindSubstitutionCandidate(call, arguments_ok);
	if (candidate == nullptr || arguments_ok)
	{
		return;
	}

	std::unique_ptr<MidoriExpression> body = CloneWithSubstitution(*candidate->m_body, nullptr, m_local_depth);
	if (body == nullptr)
	{
		return;
	}

	std::vector<std::unique_ptr<MidoriStatement>> bindings;
	bindings.reserve(call.m_arguments.size());
	for (size_t param = 0u; param < call.m_arguments.size(); param += 1u)
	{
		std::optional<std::shared_ptr<MidoriType>> annotated_type = candidate->m_defun->m_param_types[param];
		MidoriStatement::VariableDefinition binding(
			candidate->m_defun->m_params[param],
			std::move(call.m_arguments[param]),
			std::move(annotated_type),
			m_local_depth + static_cast<int>(param));
		bindings.emplace_back(std::make_unique<MidoriStatement>(std::move(binding)));
	}

	const std::shared_ptr<MidoriType> result_type = expr->GetType();
	MidoriExpression::Block block(call.m_paren, std::move(bindings), candidate->m_arity, std::move(body));
	block.m_type_data = result_type;
	expr = std::make_unique<MidoriExpression>(std::move(block));
	MarkOptimization();
}

void FunctionInlining::operator()(MidoriStatement::FunctionDefinition& defun)
{
	const int saved_depth = m_local_depth;
	const bool saved_known = m_depth_known;
	m_local_depth = static_cast<int>(defun.m_params.size());
	m_depth_known = true;

	VisitAndReplace(defun.m_body);

	m_local_depth = saved_depth;
	m_depth_known = saved_known;
}

void FunctionInlining::operator()(MidoriExpression::Function& function)
{
	const int saved_depth = m_local_depth;
	const bool saved_known = m_depth_known;
	m_local_depth = static_cast<int>(function.m_params.size());
	m_depth_known = true;

	VisitAndReplace(function.m_body);

	m_local_depth = saved_depth;
	m_depth_known = saved_known;
}

void FunctionInlining::operator()(MidoriStatement::VariableDefinition& def)
{
	if (def.m_is_elided)
	{
		return;
	}

	if (def.m_local_index.has_value() && m_depth_known)
	{
		if (def.m_local_index.value() != m_local_depth)
		{
			// The recorded slot disagrees with the model: stop trusting it.
			m_depth_known = false;
			VisitAndReplace(def.m_value);
			return;
		}

		// The definition's own slot is reserved before its initializer runs,
		// so expressions inside the value see one extra live slot.
		m_local_depth += 1;
		VisitAndReplace(def.m_value);
		TryBindingInline(def.m_value);
		return;
	}

	VisitAndReplace(def.m_value);
	if (def.m_local_index.has_value())
	{
		m_depth_known = false;
	}
	else
	{
		TryBindingInline(def.m_value);
	}
}

void FunctionInlining::operator()(MidoriStatement::TupleDefinition& def_tuple)
{
	MidoriOptimizer::operator()(def_tuple);
	m_depth_known = false;
}

void FunctionInlining::operator()(MidoriStatement::ExpressionStatement& simple)
{
	VisitAndReplace(simple.m_expr);
	TryBindingInline(simple.m_expr);
}

void FunctionInlining::operator()(MidoriExpression::Block& block)
{
	const int saved_depth = m_local_depth;
	const bool saved_known = m_depth_known;

	for (std::unique_ptr<MidoriStatement>& statement : block.m_stmts)
	{
		VisitStatement(statement);
	}

	if (block.m_final_expr.has_value())
	{
		VisitAndReplace(block.m_final_expr.value());
		TryBindingInline(block.m_final_expr.value());
	}

	m_local_depth = saved_depth;
	m_depth_known = saved_known;
}

void FunctionInlining::operator()(MidoriExpression::Match& match)
{
	const bool saved_known = m_depth_known;
	m_depth_known = false;

	VisitAndReplace(match.m_arg_expr);
	for (std::unique_ptr<MidoriExpression>& case_expr : match.m_cases)
	{
		VisitAndReplace(case_expr);
	}

	m_depth_known = saved_known;
}

void FunctionInlining::operator()(MidoriExpression::For& for_expr)
{
	const bool saved_known = m_depth_known;
	m_depth_known = false;

	VisitAndReplace(for_expr.m_range);
	VisitAndReplace(for_expr.m_body);

	m_depth_known = saved_known;
}

void FunctionInlining::operator()(MidoriExpression::ArrayComprehension& comp)
{
	const bool saved_known = m_depth_known;
	m_depth_known = false;

	VisitAndReplace(comp.m_transform_expr);
	VisitAndReplace(comp.m_range);

	m_depth_known = saved_known;
}
