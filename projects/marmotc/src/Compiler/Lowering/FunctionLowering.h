#pragma once

#include "Lowering.h"
#include "Compiler/MarmotIR/Builder/MarmotIRBuilder.h"

#include <functional>

// Owns the construction state of one IR function. Nested functions lower
// through their own instance and keep an explicit lexical parent.
class Lowering::FunctionLowering
{
private:
	// A source binding in one lowered specialization of its declaring function.
	struct LocalKey
	{
		const FunctionLowering* m_owner;
		BindingId m_binding;

		bool operator==(const LocalKey&) const = default;
	};

	// A closure made before a local it captures is defined, as a local
	// function that names itself or one defined after it is. BindCaptures
	// fills the capture once the local is defined.
	struct PendingCapture
	{
		MarmotIRValueId m_closure;
		uint32_t m_capture;
		BindingId m_binding;
	};

	// A local generic lambda's closure for one set of types, while its body is
	// lowered. A use at the same types inside it is the closure itself, held
	// as a synthetic binding of the scope that makes it.
	struct ActiveGenericLambda
	{
		const MarmotExpression::Function* m_function;
		std::string m_signature;
		LocalKey m_self;

		ActiveGenericLambda(const MarmotExpression::Function* function, std::string signature, LocalKey self);
	};

	// What a call runs.
	struct Callee
	{
		MarmotIRImmediate m_target;
		std::optional<MarmotIRValueId> m_value;
	};

	// Where the value of an `if` branch or a match arm goes: a join block's
	// parameter, or, in tail position, the function's return.
	using BranchSink = std::function<Emitted(const MarmotExpression*)>;

	struct JoinPoint
	{
		MarmotIRBlockId m_block;
		TypeRef m_type;

		JoinPoint(MarmotIRBlockId block, TypeRef type);
	};

	Lowering& m_owner;
	MarmotIRFunctionId m_id;
	MarmotIRFunction& m_function;
	MarmotIRBuilder m_builder;
	const Specialization& m_specialization;
	FunctionLowering* m_parent;
	LexicalFunctionId m_identity;
	std::unordered_map<BindingId, MarmotIRValueId> m_locals;
	std::vector<LocalKey> m_captures;
	std::vector<MarmotIRValueId> m_capture_values;
	std::vector<PendingCapture> m_pending_captures;
	// Only blocks reachable from the entry contribute live successors.
	std::vector<bool> m_live_blocks;
	std::unordered_map<BindingId, const MarmotExpression::Function*> m_local_generics;
	std::optional<ActiveGenericLambda> m_generic_lambda;

public:
	FunctionLowering(Lowering& owner, MarmotIRFunctionId id, const Specialization& specialization, FunctionLowering* parent, LexicalFunctionId identity, std::optional<ActiveGenericLambda> generic_lambda = std::nullopt);

	Emitted LowerTopLevel();

private:
	TypeRef Concrete(const TypeRef& type);
	TypeRef TypeOf(const MarmotExpression& expression);
	// The type an instance is selected by: the expression's own, which a
	// conversion that erases a newtype keeps nominal, unless the type checker
	// left it undecided.
	TypeRef NominalType(const MarmotExpression& expression, MarmotIRValueId value);

	MarmotIRBlockId NewBlock();
	bool IsDead();
	void PositionAt(MarmotIRBlockId block);
	void Jump(MarmotIRBlockId target, std::vector<MarmotIRValueId> arguments = {});
	void Branch(MarmotIRValueId condition, MarmotIRSuccessor if_true, MarmotIRSuccessor if_false);
	// A value of any type, standing in for one that code nothing runs would
	// have computed.
	MarmotIRValueId Placeholder(const TypeRef& type);
	// Ends the block after a call that returns Never, and goes on in one
	// nothing reaches.
	void AfterNever();

	Emitted LowerTopLevelStatement(const MarmotStatement& statement);
	Emitted LowerTopLevelFunction(const std::string& name, const FunctionParameters& params, const MarmotExpression& body, int line);
	Emitted LowerStatement(const MarmotStatement& statement);
	Emitted LowerLocalDefinition(const MarmotStatement::VariableDefinition& definition);
	Emitted LowerTupleDefinition(const MarmotStatement::TupleDefinition& definition);

	void DefineLocal(const BindingId& binding, MarmotIRValueId value);
	LocalKey KeyForBinding(const BindingId& binding) const;
	MarmotIRValueId CaptureFor(const LocalKey& key, const TypeRef& type);

	Emitted LowerBody(const std::vector<TypeRef>& param_types, const FunctionParameters& params, const MarmotExpression& body);
	Lowered MakeClosure(FunctionLowering& child, const TypeRef& closure_type, int line, std::string name = {});
	Lowered LowerFunction(const MarmotExpression::Function& function);
	Lowered LowerGenericLambda(const MarmotExpression::Function& function, const TypeRef& type, std::string name, int line, std::optional<ActiveGenericLambda> generic_lambda = std::nullopt);
	Lowered LowerLocalGeneric(const MarmotExpression::Function& function, const TypeRef& type, const Token& name);
	std::expected<MarmotIRFunctionId, CompilerError> Specialize(const std::string& key, const std::vector<TypeRef>& argument_types, const TypeRef& call_type);

	Lowered LowerName(const MarmotExpression::NameAccess& name);
	bool IsForeignFunction(const TypeRef& type) const;
	bool IsForeignDeclaration(const std::string& name);
	Lowered LowerForeignValue(const MarmotExpression::NameAccess& declaration, const TypeRef& type);
	std::expected<MarmotIRGlobalSlot, CompilerError> GlobalFor(const Token& name, const TypeRef& type);
	std::expected<MarmotIRGlobalSlot, CompilerError> GlobalForResolved(const ResolvedInstanceName& resolved_name, const TypeRef& type, const Token& at);
	MarmotIRGlobalSlot ImportGlobal(const std::string& module, const std::string& symbol, const TypeRef& type, const Token& at);
	Lowered LoadResolved(const ResolvedInstanceName& resolved_name, const TypeRef& type, const Token& at);
	// A call of a class method an instance provides, by its resolved name.
	Lowered CallResolved(const ResolvedInstanceName& resolved_name, std::vector<MarmotIRValueId> arguments, const std::vector<TypeRef>& argument_types, const TypeRef& result_type, const Token& at);

	Lowered LowerCall(const MarmotExpression::Call& call);
	Emitted LowerTailCall(const MarmotExpression::Call& call);
	Lowered LowerPrimitive(const MarmotExpression::Call& call);
	std::expected<Callee, CompilerError> ResolveCallee(const MarmotExpression::Call& call, const std::vector<MarmotIRValueId>& arguments, const TypeRef& call_type);
	TypeRef ResultTypeOf(const Callee& callee, const TypeRef& call_type);
	MarmotIRValueId EmitCall(const Callee& callee, std::vector<MarmotIRValueId> arguments, const TypeRef& call_type, int line);
	LoweredValues LowerArguments(const std::vector<std::unique_ptr<MarmotExpression>>& arguments);
	std::vector<TypeRef> TypesOf(const std::vector<MarmotIRValueId>& values);

	// LoweringExpressions.cpp: values.
	Lowered Lower(const MarmotExpression& expression);
	std::optional<MarmotIRImmediate> ConstantOf(const MarmotLiteral& literal) const;
	Lowered LowerLiteral(const MarmotExpression::Literal& literal);
	Lowered LowerBinary(const MarmotExpression::Binary& binary);
	Lowered LowerScalarBinary(const Token& op, MarmotIRValueId left, MarmotIRValueId right);
	Lowered LowerScalarUnary(const Token& op, MarmotIRValueId operand);
	Lowered LowerShortCircuit(const MarmotExpression::Binary& binary);
	Lowered LowerClassOperator(const MarmotExpression::Binary& binary, MarmotIRValueId left, MarmotIRValueId right);
	Lowered LowerConcat(const MarmotExpression::Binary& binary, MarmotIRValueId left, MarmotIRValueId right);
	Lowered LowerUnary(const MarmotExpression::UnaryPrefix& unary);
	Lowered LowerCount(const MarmotExpression::UnaryPrefix& unary, MarmotIRValueId operand);
	Lowered LowerAs(const MarmotExpression::As& as);
	Lowered LowerBoolToText(MarmotIRValueId value, int line);
	Lowered LowerTuple(const MarmotExpression::Tuple& tuple);
	Lowered LowerArray(const MarmotExpression::Array& array);
	Lowered LowerIndex(const MarmotExpression::IndexAccess& index);
	Lowered LowerConstruct(const MarmotExpression::Construct& construct);
	Lowered LowerRecordUpdate(const MarmotExpression::RecordUpdate& record_update);
	Lowered LowerUpdatedRecord(MarmotIRValueId source, const TypeRef& type, const std::vector<const MarmotExpression::RecordUpdate::FieldUpdate*>& updates, size_t depth, int line);
	Lowered LowerMemberAccess(const MarmotExpression::MemberAccess& member);
	Lowered LowerRange(const MarmotExpression& start, const MarmotExpression* step, const MarmotExpression& end, const TypeRef& range_type, int line);
	Lowered LowerSpawn(const MarmotExpression::Call& spawn);
	Lowered LowerOperation(MarmotIROp op, const TypeRef& type, std::vector<const MarmotExpression*> operands, MarmotIRImmediate immediate, int line);

	// LoweringControl.cpp: control flow.
	Emitted LowerReturn(const MarmotExpression& expression);
	Emitted LowerIfWith(const MarmotExpression::IfElse& if_else, const BranchSink& sink);
	Lowered LowerIf(const MarmotExpression::IfElse& if_else);
	Emitted LowerStatements(const MarmotExpression::Block& block);
	Lowered LowerBlock(const MarmotExpression::Block& block);
	Emitted LowerMatchWith(const MarmotExpression::Match& match, const BranchSink& sink);
	Lowered LowerMatch(const MarmotExpression::Match& match);
	Emitted LowerPattern(const MarmotPattern& pattern, MarmotIRValueId value, MarmotIRBlockId fail);
	Emitted TestOrFail(MarmotIRValueId condition, MarmotIRBlockId fail);
	// A join block whose parameter is the value of the construct that jumps
	// to it, and the sink each branch lowers into. EnterJoin gives the
	// parameter its type once every branch has jumped.
	std::pair<std::shared_ptr<JoinPoint>, BranchSink> Join(const TypeRef& type);
	MarmotIRValueId EnterJoin(const JoinPoint& join);

	// What `for` and an array comprehension iterate: a range, an array, or an
	// Iterable, whose Next gives Some((item, next iterator)) with m_some_tag.
	struct LoopKind
	{
		bool m_is_array;
		bool m_is_iterable;
		TypeRef m_item_type;
		TypeRef m_next_type;
		int m_some_tag;
	};

	// `body` is lowered once, with the loop variable defined, and gets the
	// values the loop carries round, which it returns updated.
	using LoopBody = std::function<std::expected<std::vector<MarmotIRValueId>, CompilerError>(std::vector<MarmotIRValueId>)>;
	std::expected<std::vector<MarmotIRValueId>, CompilerError> LowerLoop(const MarmotExpression& iterated, const LoopKind& kind, const BindingId& loop_variable, std::vector<MarmotIRValueId> carried, const LoopBody& body, const Token& at);
	LoopKind LoopKindOf(bool is_array, bool is_iterable, const TypeRef& item_type, const TypeRef& next_type, int some_tag);
	Lowered LowerFor(const MarmotExpression::For& for_expression);
	Lowered LowerComprehension(const MarmotExpression::ArrayComprehension& comprehension);
	Lowered CallIterableNext(MarmotIRValueId iterator, const TypeRef& item_type, const TypeRef& next_type, const Token& at);
};
