#pragma once

#include "Common/BuildConfig/BuildConfig.h"
#include "Compiler/Analysis/AbstractSyntaxTreeWalker.h"
#include "Compiler/Result/Result.h"

class MidoriOptimizer : protected MidoriAbstractSyntaxTreeWalker
{
protected:
#if MIDORI_ENABLE_OPTIMIZER_STATS
	int m_optimizations_performed = 0;
#endif
	bool m_did_change = false;

	std::unique_ptr<MidoriExpression> m_pending_replacement;

public:
	virtual ~MidoriOptimizer() = default;

	virtual MidoriResult::OptimizerResult Optimize(MidoriProgramTree program_tree) = 0;

	virtual std::string_view GetName() const = 0;

	void ResetPassState();

	bool DidChange() const;

#if MIDORI_ENABLE_OPTIMIZER_STATS

	int GetOptimizationsPerformed() const;
#endif

protected:
#if MIDORI_ENABLE_OPTIMIZER_STATS
	void MarkOptimization();
#else
	void MarkOptimization()
	{
		m_did_change = true;
	}
#endif

protected:
	void VisitAndReplace(std::unique_ptr<MidoriExpression>& expr);

	virtual void Replace(std::unique_ptr<MidoriExpression>&& new_node, std::unique_ptr<MidoriExpression>& old_node);

	virtual void operator()(MidoriStatement::ExpressionStatement& simple) override;
	virtual void operator()(MidoriStatement::VariableDefinition& def) override;
	virtual void operator()(MidoriStatement::TupleDefinition& def_tuple) override;
	virtual void operator()(MidoriStatement::FunctionDefinition& defun) override;
	virtual void operator()(MidoriStatement::ForeignDefinition& foreign) override;
	virtual void operator()(MidoriStatement::Struct& struct_stmt) override;
	virtual void operator()(MidoriStatement::Union& union_stmt) override;
	virtual void operator()(MidoriStatement::Class& typeclass_stmt) override;
	virtual void operator()(MidoriStatement::Instance& instance_stmt) override;
	virtual void operator()(MidoriStatement::TypeAlias& type_alias) override;

	virtual void operator()(MidoriExpression::As& as) override;
	virtual void operator()(MidoriExpression::Binary& binary) override;
	virtual void operator()(MidoriExpression::Group& group) override;
	virtual void operator()(MidoriExpression::Tuple& tuple) override;
	virtual void operator()(MidoriExpression::UnaryPrefix& unary) override;
	virtual void operator()(MidoriExpression::UnarySuffix& unary) override;
	virtual void operator()(MidoriExpression::Call& call) override;
	virtual void operator()(MidoriExpression::MemberAccess& get) override;
	virtual void operator()(MidoriExpression::NameAccess& variable) override;
	virtual void operator()(MidoriExpression::Literal& literal) override;
	virtual void operator()(MidoriExpression::Function& function) override;
	virtual void operator()(MidoriExpression::Construct& construct) override;
	virtual void operator()(MidoriExpression::RecordUpdate& record_update) override;
	virtual void operator()(MidoriExpression::Spawn& spawn) override;
	virtual void operator()(MidoriExpression::Join& join) override;
	virtual void operator()(MidoriExpression::ChannelCreate& channel_create) override;
	virtual void operator()(MidoriExpression::Send& send) override;
	virtual void operator()(MidoriExpression::Receive& receive) override;
	virtual void operator()(MidoriExpression::Array& array) override;
	virtual void operator()(MidoriExpression::IndexAccess& array_get) override;
	virtual void operator()(MidoriExpression::ArrayComprehension& comp) override;
	virtual void operator()(MidoriExpression::RangeBinary& range_binary) override;
	virtual void operator()(MidoriExpression::RangeTernary& range_ternary) override;
	virtual void operator()(MidoriExpression::IfElse& if_else) override;
	virtual void operator()(MidoriExpression::Block& block) override;
	virtual void operator()(MidoriExpression::Match& match) override;
	virtual void operator()(MidoriExpression::Case& case_expr) override;
	virtual void operator()(MidoriExpression::For& for_expr) override;
};
