#pragma once

#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"

class MarmotAbstractSyntaxTreeWalker
{
public:
	virtual ~MarmotAbstractSyntaxTreeWalker() = default;

protected:
	void VisitProgram(MarmotProgramTree& program_tree);

	void VisitStatement(MarmotStatement& statement);
	void VisitStatement(std::unique_ptr<MarmotStatement>& statement);

	void VisitExpression(MarmotExpression& expression);
	void VisitExpression(std::unique_ptr<MarmotExpression>& expression);

	void VisitPattern(MarmotPattern& pattern);
	void VisitPattern(std::unique_ptr<MarmotPattern>& pattern);

	virtual void operator()(MarmotStatement::ExpressionStatement& simple);
	virtual void operator()(MarmotStatement::VariableDefinition& def);
	virtual void operator()(MarmotStatement::TupleDefinition& def_tuple);
	virtual void operator()(MarmotStatement::FunctionDefinition& defun);
	virtual void operator()(MarmotStatement::ForeignDefinition& foreign);
	virtual void operator()(MarmotStatement::Struct& struct_stmt);
	virtual void operator()(MarmotStatement::Union& union_stmt);
	virtual void operator()(MarmotStatement::Class& typeclass_stmt);
	virtual void operator()(MarmotStatement::Instance& instance_stmt);
	virtual void operator()(MarmotStatement::TypeAlias& type_alias);

	virtual void operator()(MarmotPattern::Binding& binding);
	virtual void operator()(MarmotPattern::Wildcard& wildcard);
	virtual void operator()(MarmotPattern::Literal& literal);
	virtual void operator()(MarmotPattern::Tuple& tuple);
	virtual void operator()(MarmotPattern::Array& array);
	virtual void operator()(MarmotPattern::Constructor& constructor);
	virtual void operator()(MarmotPattern::As& as);
	virtual void operator()(MarmotPattern::Or& either);
	virtual void operator()(MarmotPattern::Range& range);
	virtual void operator()(MarmotPattern::Record& record);

	virtual void operator()(MarmotExpression::As& as);
	virtual void operator()(MarmotExpression::Binary& binary);
	virtual void operator()(MarmotExpression::Group& group);
	virtual void operator()(MarmotExpression::Tuple& tuple);
	virtual void operator()(MarmotExpression::UnaryPrefix& unary);
	virtual void operator()(MarmotExpression::Call& call);
	virtual void operator()(MarmotExpression::MemberAccess& get);
	virtual void operator()(MarmotExpression::NameAccess& variable);
	virtual void operator()(MarmotExpression::Literal& literal);
	virtual void operator()(MarmotExpression::Function& function);
	virtual void operator()(MarmotExpression::Construct& construct);
	virtual void operator()(MarmotExpression::RecordUpdate& record_update);
	virtual void operator()(MarmotExpression::Send& send);
	virtual void operator()(MarmotExpression::Receive& receive);
	virtual void operator()(MarmotExpression::Array& array);
	virtual void operator()(MarmotExpression::IndexAccess& array_get);
	virtual void operator()(MarmotExpression::ArrayComprehension& comp);
	virtual void operator()(MarmotExpression::RangeBinary& range_binary);
	virtual void operator()(MarmotExpression::RangeTernary& range_ternary);
	virtual void operator()(MarmotExpression::IfElse& if_else);
	virtual void operator()(MarmotExpression::Block& block);
	virtual void operator()(MarmotExpression::Match& match);
	virtual void operator()(MarmotExpression::Case& case_expr);
	virtual void operator()(MarmotExpression::For& for_expr);
};
