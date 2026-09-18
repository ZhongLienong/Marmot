#pragma once

#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"

class MidoriAbstractSyntaxTreeWalker
{
public:
	virtual ~MidoriAbstractSyntaxTreeWalker() = default;

protected:
	void VisitProgram(MidoriProgramTree& program_tree);

	void VisitStatement(MidoriStatement& statement);
	void VisitStatement(std::unique_ptr<MidoriStatement>& statement);

	void VisitExpression(MidoriExpression& expression);
	void VisitExpression(std::unique_ptr<MidoriExpression>& expression);

	void VisitPattern(MidoriPattern& pattern);
	void VisitPattern(std::unique_ptr<MidoriPattern>& pattern);

	virtual void operator()(MidoriStatement::ExpressionStatement& simple);
	virtual void operator()(MidoriStatement::VariableDefinition& def);
	virtual void operator()(MidoriStatement::TupleDefinition& def_tuple);
	virtual void operator()(MidoriStatement::FunctionDefinition& defun);
	virtual void operator()(MidoriStatement::ForeignDefinition& foreign);
	virtual void operator()(MidoriStatement::Struct& struct_stmt);
	virtual void operator()(MidoriStatement::Union& union_stmt);
	virtual void operator()(MidoriStatement::Class& typeclass_stmt);
	virtual void operator()(MidoriStatement::Instance& instance_stmt);
	virtual void operator()(MidoriStatement::TypeAlias& type_alias);

	virtual void operator()(MidoriPattern::Binding& binding);
	virtual void operator()(MidoriPattern::Wildcard& wildcard);
	virtual void operator()(MidoriPattern::Literal& literal);
	virtual void operator()(MidoriPattern::Tuple& tuple);
	virtual void operator()(MidoriPattern::Array& array);
	virtual void operator()(MidoriPattern::Constructor& constructor);

	virtual void operator()(MidoriExpression::As& as);
	virtual void operator()(MidoriExpression::Binary& binary);
	virtual void operator()(MidoriExpression::Group& group);
	virtual void operator()(MidoriExpression::Tuple& tuple);
	virtual void operator()(MidoriExpression::UnaryPrefix& unary);
	virtual void operator()(MidoriExpression::UnarySuffix& unary);
	virtual void operator()(MidoriExpression::Call& call);
	virtual void operator()(MidoriExpression::MemberAccess& get);
	virtual void operator()(MidoriExpression::NameAccess& variable);
	virtual void operator()(MidoriExpression::Literal& literal);
	virtual void operator()(MidoriExpression::Function& function);
	virtual void operator()(MidoriExpression::Construct& construct);
	virtual void operator()(MidoriExpression::RecordUpdate& record_update);
	virtual void operator()(MidoriExpression::Spawn& spawn);
	virtual void operator()(MidoriExpression::Join& join);
	virtual void operator()(MidoriExpression::ChannelCreate& channel_create);
	virtual void operator()(MidoriExpression::Send& send);
	virtual void operator()(MidoriExpression::Receive& receive);
	virtual void operator()(MidoriExpression::Array& array);
	virtual void operator()(MidoriExpression::IndexAccess& array_get);
	virtual void operator()(MidoriExpression::ArrayComprehension& comp);
	virtual void operator()(MidoriExpression::RangeBinary& range_binary);
	virtual void operator()(MidoriExpression::RangeTernary& range_ternary);
	virtual void operator()(MidoriExpression::IfElse& if_else);
	virtual void operator()(MidoriExpression::Block& block);
	virtual void operator()(MidoriExpression::Match& match);
	virtual void operator()(MidoriExpression::Case& case_expr);
	virtual void operator()(MidoriExpression::For& for_expr);
};
