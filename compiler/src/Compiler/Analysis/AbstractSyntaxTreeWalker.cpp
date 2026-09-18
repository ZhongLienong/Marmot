#include "AbstractSyntaxTreeWalker.h"

void MidoriAbstractSyntaxTreeWalker::VisitProgram(MidoriProgramTree& program_tree)
{
	for (std::unique_ptr<MidoriStatement>& statement : program_tree)
	{
		VisitStatement(statement);
	}
}

void MidoriAbstractSyntaxTreeWalker::VisitStatement(MidoriStatement& statement)
{
	VisitNode([this](auto& node) { (*this)(node); }, statement);
}

void MidoriAbstractSyntaxTreeWalker::VisitStatement(std::unique_ptr<MidoriStatement>& statement)
{
	VisitNode([this](auto& node) { (*this)(node); }, statement);
}

void MidoriAbstractSyntaxTreeWalker::VisitExpression(MidoriExpression& expression)
{
	VisitNode([this](auto& node) { (*this)(node); }, expression);
}

void MidoriAbstractSyntaxTreeWalker::VisitExpression(std::unique_ptr<MidoriExpression>& expression)
{
	VisitNode([this](auto& node) { (*this)(node); }, expression);
}

void MidoriAbstractSyntaxTreeWalker::VisitPattern(MidoriPattern& pattern)
{
	VisitNode([this](auto& node) { (*this)(node); }, pattern);
}

void MidoriAbstractSyntaxTreeWalker::VisitPattern(std::unique_ptr<MidoriPattern>& pattern)
{
	VisitNode([this](auto& node) { (*this)(node); }, pattern);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriStatement::ExpressionStatement& simple)
{
	VisitExpression(simple.m_expr);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriStatement::VariableDefinition& def)
{
	VisitExpression(def.m_value);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriStatement::TupleDefinition& def_tuple)
{
	VisitExpression(def_tuple.m_value);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriStatement::FunctionDefinition& defun)
{
	VisitExpression(defun.m_body);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriStatement::ForeignDefinition&)
{
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriStatement::Struct&)
{
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriStatement::Union&)
{
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriStatement::Class&)
{
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriStatement::Instance&)
{
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriStatement::TypeAlias&)
{
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriPattern::Binding&)
{
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriPattern::Wildcard&)
{
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriPattern::Literal&)
{
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriPattern::Tuple& tuple)
{
	for (std::unique_ptr<MidoriPattern>& element : tuple.m_elements)
	{
		VisitPattern(element);
	}
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriPattern::Array& array)
{
	for (std::unique_ptr<MidoriPattern>& element : array.m_elements)
	{
		VisitPattern(element);
	}
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriPattern::Constructor& constructor)
{
	for (std::unique_ptr<MidoriPattern>& argument : constructor.m_args)
	{
		VisitPattern(argument);
	}
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::As& as)
{
	VisitExpression(as.m_expr);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::Binary& binary)
{
	VisitExpression(binary.m_left);
	VisitExpression(binary.m_right);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::Group& group)
{
	VisitExpression(group.m_expr_in);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::Tuple& tuple)
{
	for (std::unique_ptr<MidoriExpression>& element : tuple.m_elements)
	{
		VisitExpression(element);
	}
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::UnaryPrefix& unary)
{
	VisitExpression(unary.m_expr);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::UnarySuffix& unary)
{
	VisitExpression(unary.m_expr);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::Call& call)
{
	VisitExpression(call.m_callee);
	for (std::unique_ptr<MidoriExpression>& argument : call.m_arguments)
	{
		VisitExpression(argument);
	}
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::MemberAccess& get)
{
	VisitExpression(get.m_struct);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::NameAccess&)
{
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::Literal&)
{
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::Function& function)
{
	VisitExpression(function.m_body);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::Construct& construct)
{
	for (std::unique_ptr<MidoriExpression>& parameter : construct.m_params)
	{
		VisitExpression(parameter);
	}
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::RecordUpdate& record_update)
{
	VisitExpression(record_update.m_source);
	for (MidoriExpression::RecordUpdate::FieldUpdate& update : record_update.m_updates)
	{
		VisitExpression(update.m_value);
	}
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::Spawn& spawn)
{
	for (std::unique_ptr<MidoriExpression>& argument : spawn.m_arguments)
	{
		VisitExpression(argument);
	}

	VisitExpression(spawn.m_callee);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::Join& join)
{
	VisitExpression(join.m_worker);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::ChannelCreate& channel_create)
{
	VisitExpression(channel_create.m_capacity);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::Send& send)
{
	VisitExpression(send.m_channel);
	VisitExpression(send.m_value);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::Receive& receive)
{
	VisitExpression(receive.m_channel);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::Array& array)
{
	for (std::unique_ptr<MidoriExpression>& element : array.m_elems)
	{
		VisitExpression(element);
	}
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::IndexAccess& array_get)
{
	VisitExpression(array_get.m_arr_var);
	VisitExpression(array_get.m_index);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::ArrayComprehension& comp)
{
	VisitExpression(comp.m_transform_expr);
	VisitExpression(comp.m_range);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::RangeBinary& range_binary)
{
	VisitExpression(range_binary.m_start);
	VisitExpression(range_binary.m_end);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::RangeTernary& range_ternary)
{
	VisitExpression(range_ternary.m_start);
	VisitExpression(range_ternary.m_step);
	VisitExpression(range_ternary.m_end);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::IfElse& if_else)
{
	VisitExpression(if_else.m_condition);
	VisitExpression(if_else.m_true_branch);
	VisitExpression(if_else.m_else_branch);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::Block& block)
{
	for (std::unique_ptr<MidoriStatement>& statement : block.m_stmts)
	{
		VisitStatement(statement);
	}

	if (block.m_final_expr.has_value())
	{
		VisitExpression(block.m_final_expr.value());
	}
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::Match& match)
{
	VisitExpression(match.m_arg_expr);
	for (std::unique_ptr<MidoriExpression>& case_expr : match.m_cases)
	{
		VisitExpression(case_expr);
	}
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::Case& case_expr)
{
	VisitPattern(case_expr.m_pattern);
	if (case_expr.HasGuard())
	{
		VisitExpression(case_expr.m_guard.value());
	}
	VisitExpression(case_expr.m_expr);
}

void MidoriAbstractSyntaxTreeWalker::operator()(MidoriExpression::For& for_expr)
{
	VisitExpression(for_expr.m_range);
	VisitExpression(for_expr.m_body);
}
