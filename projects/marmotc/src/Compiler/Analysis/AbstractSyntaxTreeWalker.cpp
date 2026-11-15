#include "AbstractSyntaxTreeWalker.h"

void MarmotAbstractSyntaxTreeWalker::VisitProgram(MarmotProgramTree& program_tree)
{
	for (std::unique_ptr<MarmotStatement>& statement : program_tree)
	{
		VisitStatement(statement);
	}
}

void MarmotAbstractSyntaxTreeWalker::VisitStatement(MarmotStatement& statement)
{
	VisitNode([this](auto& node) { (*this)(node); }, statement);
}

void MarmotAbstractSyntaxTreeWalker::VisitStatement(std::unique_ptr<MarmotStatement>& statement)
{
	VisitNode([this](auto& node) { (*this)(node); }, statement);
}

void MarmotAbstractSyntaxTreeWalker::VisitExpression(MarmotExpression& expression)
{
	VisitNode([this](auto& node) { (*this)(node); }, expression);
}

void MarmotAbstractSyntaxTreeWalker::VisitExpression(std::unique_ptr<MarmotExpression>& expression)
{
	VisitNode([this](auto& node) { (*this)(node); }, expression);
}

void MarmotAbstractSyntaxTreeWalker::VisitPattern(MarmotPattern& pattern)
{
	VisitNode([this](auto& node) { (*this)(node); }, pattern);
}

void MarmotAbstractSyntaxTreeWalker::VisitPattern(std::unique_ptr<MarmotPattern>& pattern)
{
	VisitNode([this](auto& node) { (*this)(node); }, pattern);
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotStatement::ExpressionStatement& simple)
{
	VisitExpression(simple.m_expr);
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotStatement::VariableDefinition& def)
{
	VisitExpression(def.m_value);
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotStatement::TupleDefinition& def_tuple)
{
	VisitExpression(def_tuple.m_value);
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotStatement::FunctionDefinition& defun)
{
	VisitExpression(defun.m_body);
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotStatement::ForeignDefinition&)
{
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotStatement::Struct&)
{
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotStatement::Union&)
{
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotStatement::Class&)
{
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotStatement::Instance&)
{
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotStatement::TypeAlias&)
{
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotPattern::Binding&)
{
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotPattern::Wildcard&)
{
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotPattern::Range&)
{
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotPattern::Record& record)
{
	for (MarmotPattern::Record::Field& field : record.m_fields)
	{
		VisitPattern(field.m_pattern);
	}
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotPattern::Literal&)
{
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotPattern::Tuple& tuple)
{
	for (std::unique_ptr<MarmotPattern>& element : tuple.m_elements)
	{
		VisitPattern(element);
	}
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotPattern::Array& array)
{
	for (std::unique_ptr<MarmotPattern>& element : array.m_elements)
	{
		VisitPattern(element);
	}
	if (array.m_rest.has_value())
	{
		VisitPattern(array.m_rest->m_pattern);
	}
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotPattern::Constructor& constructor)
{
	for (std::unique_ptr<MarmotPattern>& argument : constructor.m_args)
	{
		VisitPattern(argument);
	}
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotPattern::As& as)
{
	VisitPattern(as.m_pattern);
	VisitPattern(as.m_binding);
}

// The later alternatives bind the names the first does, to the same locals, and
// patterns hold nothing else a pass looks at.
void MarmotAbstractSyntaxTreeWalker::operator()(MarmotPattern::Or& either)
{
	VisitPattern(either.m_alternatives.front());
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::As& as)
{
	VisitExpression(as.m_expr);
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::Binary& binary)
{
	VisitExpression(binary.m_left);
	VisitExpression(binary.m_right);
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::Group& group)
{
	VisitExpression(group.m_expr_in);
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::Tuple& tuple)
{
	for (std::unique_ptr<MarmotExpression>& element : tuple.m_elements)
	{
		VisitExpression(element);
	}
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::UnaryPrefix& unary)
{
	VisitExpression(unary.m_expr);
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::Call& call)
{
	VisitExpression(call.m_callee);
	for (std::unique_ptr<MarmotExpression>& argument : call.m_arguments)
	{
		VisitExpression(argument);
	}
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::MemberAccess& get)
{
	VisitExpression(get.m_struct);
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::NameAccess&)
{
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::Literal&)
{
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::Function& function)
{
	VisitExpression(function.m_body);
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::Construct& construct)
{
	for (std::unique_ptr<MarmotExpression>& parameter : construct.m_params)
	{
		VisitExpression(parameter);
	}
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::RecordUpdate& record_update)
{
	VisitExpression(record_update.m_source);
	for (MarmotExpression::RecordUpdate::FieldUpdate& update : record_update.m_updates)
	{
		VisitExpression(update.m_value);
	}
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::Send& send)
{
	VisitExpression(send.m_channel);
	VisitExpression(send.m_value);
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::Receive& receive)
{
	VisitExpression(receive.m_channel);
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::Array& array)
{
	for (std::unique_ptr<MarmotExpression>& element : array.m_elems)
	{
		VisitExpression(element);
	}
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::IndexAccess& array_get)
{
	VisitExpression(array_get.m_arr_var);
	VisitExpression(array_get.m_index);
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::ArrayComprehension& comp)
{
	VisitExpression(comp.m_transform_expr);
	VisitExpression(comp.m_range);
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::RangeBinary& range_binary)
{
	VisitExpression(range_binary.m_start);
	VisitExpression(range_binary.m_end);
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::RangeTernary& range_ternary)
{
	VisitExpression(range_ternary.m_start);
	VisitExpression(range_ternary.m_step);
	VisitExpression(range_ternary.m_end);
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::IfElse& if_else)
{
	VisitExpression(if_else.m_condition);
	VisitExpression(if_else.m_true_branch);
	VisitExpression(if_else.m_else_branch);
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::Block& block)
{
	for (std::unique_ptr<MarmotStatement>& statement : block.m_stmts)
	{
		VisitStatement(statement);
	}

	if (block.m_final_expr.has_value())
	{
		VisitExpression(block.m_final_expr.value());
	}
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::Match& match)
{
	VisitExpression(match.m_arg_expr);
	for (std::unique_ptr<MarmotExpression>& case_expr : match.m_cases)
	{
		VisitExpression(case_expr);
	}
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::Case& case_expr)
{
	VisitPattern(case_expr.m_pattern);
	if (case_expr.HasGuard())
	{
		VisitExpression(case_expr.m_guard.value());
	}
	VisitExpression(case_expr.m_expr);
}

void MarmotAbstractSyntaxTreeWalker::operator()(MarmotExpression::For& for_expr)
{
	VisitExpression(for_expr.m_range);
	VisitExpression(for_expr.m_body);
}
