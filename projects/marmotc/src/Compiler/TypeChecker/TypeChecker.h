#pragma once

#include <array>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "Compiler/Error/CompilerError.h"
#include "Compiler/Module/TypeclassMetadata.h"
#include "Compiler/Result/Result.h"
#include "Compiler/TypeTransform/TypeTransform.h"

class ExpectedTypeGuard;
class PatternCoverage;

struct TypePairHash
{
	std::size_t operator()(const std::pair<MarmotType*, MarmotType*>& pair) const noexcept;
};

class TypeChecker
{
	friend class ExpectedTypeGuard;
	friend class NominalUnifyGuard;
	friend class DefiningGenericGuard;
	friend class RigidTypeVariablesGuard;

public:
	using TypeEnvironment = std::unordered_map<std::string, std::shared_ptr<MarmotType>>;
	using AssociatedTypeEnvironment = std::unordered_map<std::string, std::shared_ptr<MarmotType>>;
	enum class UnifyDiagnosticMode
	{
		Symmetric,
		ActualExpected,
		ExpectedActual
	};

	struct ClassInfo
	{
		std::string m_name;
		std::vector<std::string> m_type_param_names;
		AssociatedTypeEnvironment m_associated_types;
		std::unordered_map<std::string, std::shared_ptr<MarmotType>> m_method_types;

		ClassInfo() = default;
		ClassInfo(const std::string& name, std::vector<std::string>&& params, AssociatedTypeEnvironment&& associated_types, std::unordered_map<std::string, std::shared_ptr<MarmotType>>&& methods);
	};

private:
	struct TypeScope
	{
		TypeEnvironment m_names;
		std::unordered_map<BindingId, std::shared_ptr<MarmotType>> m_bindings;

		TypeScope() = default;
		explicit TypeScope(TypeEnvironment names);
	};
	using TypeEnvironmentStack = std::vector<TypeScope>;
	using TypeSubstitution = std::unordered_map<int, std::shared_ptr<MarmotType>>;
	using GenericFunctionNames = std::unordered_map<std::string, LexicalFunctionId>;
	using GenericStructNames = std::unordered_set<std::string>;
	using GenericUnionNames = std::unordered_set<std::string>;
	using TypeDefinitionMap = std::unordered_map<std::string, std::shared_ptr<MarmotType>>;

	struct InstanceKey
	{
		std::string m_class_name;
		std::vector<std::string> m_concrete_types;

		bool operator==(const InstanceKey& other) const;
	};

	struct InstanceKeyHash
	{
		std::size_t operator()(const InstanceKey& key) const;
	};

	struct InstanceInfo
	{
		std::string m_class_name;
		std::vector<std::shared_ptr<MarmotType>> m_type_args;
		std::vector<MarmotType::ClassConstraint> m_constraints;
		AssociatedTypeEnvironment m_associated_type_bindings;
		std::unordered_map<std::string, std::unique_ptr<MarmotStatement>> m_method_impls;
		bool m_is_derived = false;

		InstanceInfo() = default;
		InstanceInfo(const std::string& tc_name, std::vector<std::shared_ptr<MarmotType>>&& args, std::vector<MarmotType::ClassConstraint>&& constraints, AssociatedTypeEnvironment&& associated_type_bindings, std::unordered_map<std::string, std::unique_ptr<MarmotStatement>>&& methods, bool is_derived = false);
	};

	struct ResolvedInstanceMatch
	{
		const InstanceInfo* m_instance = nullptr;
		TypeEnvironment m_substitutions;
	};

	class ScopeSession;
	class FunctionBodyContext;

	MarmotProgramTree m_program_tree;
	TypeEnvironmentStack m_type_scopes;
	TypeEnvironment m_module_types;
	// The written-down types of this module's top-level definitions, for a
	// body that names one before the checker has reached it. A definition
	// without a complete signature is in m_later_unannotated instead.
	TypeEnvironment m_later_definitions;
	std::unordered_set<std::string> m_later_unannotated;
	// Top-level definitions whose own check failed. A use of one takes an unknown
	// type, and the errors of the statement that used it are dropped: they would
	// be about that unknown type, and the error shown should be the real one.
	std::unordered_set<std::string> m_failed_definitions;
	bool m_used_failed_definition = false;
	TypeSubstitution m_type_substitution;
	std::unordered_map<std::string, ClassInfo> m_classes;
	std::unordered_map<InstanceKey, InstanceInfo, InstanceKeyHash> m_instances;
	std::unordered_set<std::pair<MarmotType*, MarmotType*>, TypePairHash> m_unify_visited;
	// Names of the nominal types whose members are currently being walked, by
	// Unify or by the occurs check. The pointer guard above cannot catch a type
	// that reaches itself through a fresh node -- Fork(Array<Tree>) rebuilds the
	// Array on every descent -- so nominal recursion is tracked by name as well.
	std::unordered_set<std::string> m_unify_nominal_active;
	GenericFunctionNames m_generic_functions;
	GenericStructNames m_generic_structs;
	GenericUnionNames m_generic_unions;
	TypeDefinitionMap m_struct_type_definitions;
	TypeDefinitionMap m_union_type_definitions;
	std::vector<MarmotType::ClassConstraint> m_active_constraints;
	// Identities of the generic definitions whose bodies are currently being checked.
	// A call to one of these from inside its own body must not be freshened: it is
	// recursion, not a fresh instantiation.
	std::vector<LexicalFunctionId> m_defining_generic_functions;
	// A generic lambda bound by a 'def' inside a body: the scope it is bound
	// in, and the variables that are its own, the ones a use picks.
	struct LocalGenericFunction
	{
		size_t m_depth;
		BindingId m_binding;
		LexicalFunctionId m_function;
		std::vector<int> m_parameters;

		LocalGenericFunction(size_t depth, BindingId binding, LexicalFunctionId function, std::vector<int> parameters);
	};
	std::vector<LocalGenericFunction> m_local_generic_functions;
	// A generic used as a value before its types are decided, as the argument of a
	// call that decides them or a definition a later use does. Its constraints wait,
	// with the ones in force where it was used, until its types are decided; one still
	// undecided once the module is checked is an error.
	struct PendingInstance
	{
		Token m_token;
		std::shared_ptr<MarmotType> m_instance;
		std::vector<MarmotType::ClassConstraint> m_active_constraints;
		std::string m_undecided_message;

		PendingInstance(const Token& token, std::shared_ptr<MarmotType> instance, std::vector<MarmotType::ClassConstraint> active_constraints, std::string undecided_message);
	};
	std::vector<PendingInstance> m_pending_instances;
	// The name a call or spawn is about to call: using it there is not using
	// it as a value.
	const MarmotExpression::NameAccess* m_callee_name = nullptr;
	// The variables standing for the generic parameters of the definitions being
	// checked, with the name each was written as. A body may not decide one: it
	// stands for whatever type a caller picks.
	std::unordered_map<int, std::string> m_rigid_type_vars;
	// Every variable that was ever one of those, kept after its definition is
	// checked: once the module is, any other variable left in a call's type is
	// one nothing decided.
	std::unordered_set<int> m_generic_parameter_ids;
	// Calls whose result type nothing decided, found once the module is checked.
	std::vector<CompilerError> m_undecided_call_errors;
	std::string m_file_name;
	const std::vector<std::string>& m_source_lines;
	std::shared_ptr<MarmotType> m_expected_return_type;
	std::shared_ptr<MarmotType> m_expected_expr_type;
	int m_next_type_var_id;
	static const std::array<Token::Name, 5u> kBinaryArithmeticOperators;
	static const std::array<Token::Name, 1u> kBinaryConcatenationOperators;
	static const std::array<Token::Name, 4u> kBinaryPartialOrderComparisonOperators;
	static const std::array<Token::Name, 2u> kBinaryEqualityOperators;
	static const std::array<Token::Name, 2u> kBinaryLogicalOperators;
	static const std::array<Token::Name, 5u> kBinaryBitwiseOperators;
	static constexpr size_t s_max_instance_constraint_depth = 16u;

public:

	TypeChecker(MarmotProgramTree&& parser_result, std::string_view file_name, const std::vector<std::string>& source_lines, TypeEnvironment imported_types = {}, const TypeclassMetadataMap& imported_typeclasses = {});

	MarmotResult::TypeCheckerResult TypeCheck();


	void CollectLaterDefinitions();
	void RecordFailedDefinition(const MarmotStatement& statement);

	// The module's own names with the types checking gave them, kept after a
	// successful TypeCheck(). A name whose type only inference knows - an
	// un-annotated `def` - has one here and nowhere else.
	[[nodiscard]] const TypeEnvironment& ModuleTypes() const noexcept;

	// Extract type signatures from parsed AST (for parallel type checking)
	static TypeEnvironment ExtractTypeSignatures(const MarmotProgramTree& ast, const std::unordered_set<std::string>* exported_symbols = nullptr);

private:

	MarmotResult::TypeResult Evaluate(const std::unique_ptr<MarmotStatement>& statement);

	MarmotResult::TypeResult Evaluate(const std::unique_ptr<MarmotExpression>& expression);

	MarmotResult::TypeResult CheckRefNew(const std::unique_ptr<MarmotExpression>& value);

	MarmotResult::TypeResult CheckRefAccess(const std::unique_ptr<MarmotExpression>& cell, const std::unique_ptr<MarmotExpression>* value, const Token& token, std::string_view error_message);

	void BindParameters(const FunctionParameters& parameters, const std::vector<std::shared_ptr<MarmotType>>& types);
	void BindValue(const Token& name, const std::optional<BindingId>& binding, std::shared_ptr<MarmotType> type);
	std::shared_ptr<MarmotType>* FindBindingType(const BindingId& binding);
	const std::shared_ptr<MarmotType>* FindValueType(const MarmotExpression::NameAccess& name) const;

	MarmotResult::TypeResult CheckFunctionBody(const std::unique_ptr<MarmotExpression>& body, std::shared_ptr<MarmotType>& return_type, const std::vector<MarmotType::ClassConstraint>& constraints, const Token& token);

	TypeChecker& BeginScope();

	TypeChecker& EndScope();


	MarmotResult::TypeResult CheckPattern(MarmotPattern& pattern, const std::shared_ptr<MarmotType>& expected_type);

	std::optional<CompilerError> CheckCaseReachable(const PatternCoverage& coverage, const MarmotExpression::Case& match_case);
	std::optional<CompilerError> CheckCaseGuard(MarmotExpression::Case& case_expr);

	std::optional<MarmotPrimitive> CalledPrimitive(const MarmotExpression::Call& call) const;

	MarmotResult::TypeResult CheckSpawnCall(MarmotExpression::Call& call);

	std::optional<CompilerError> CheckSpawnArgument(MarmotExpression::Call& call, const MarmotType::FunctionType& function_type);

	std::optional<CompilerError> CheckPrimitiveDeclaration(const MarmotStatement::ForeignDefinition& foreign) const;


	std::shared_ptr<MarmotType>* FindNameType(const std::string& name);

	const std::shared_ptr<MarmotType>* FindNameType(const std::string& name) const;

	std::shared_ptr<MarmotType> FreshTypeVar();

	std::shared_ptr<MarmotType> Freshen(const std::shared_ptr<MarmotType>& type);

	TypeTransform::Freshener MakeLambdaFresheningContext();

	std::unordered_set<int> CollectEnclosingTypeVariableIds();

	std::optional<CompilerError> CheckGenericParametersStayGeneric(const std::vector<std::pair<int, std::string>>& generic_params, const std::vector<Token>& declared_params, const Token& definition_token);

	std::shared_ptr<MarmotType> InstantiateWrittenType(const std::shared_ptr<MarmotType>& written_type) const;

	// The name of the generic parameter a resolved type stands for, when it is one.
	std::optional<std::string> RigidParameterName(const std::shared_ptr<MarmotType>& resolved_type) const;

	MarmotResult::TypeResult TypeCheckGenericLambdaDefinition(MarmotStatement::VariableDefinition& def, MarmotExpression::Function& function);

	// Checks the body once for any choice of the generic parameters. `bind` is
	// given the generic type and its own variables before the body, so a named
	// lambda can call itself.
	MarmotResult::TypeResult CheckGenericLambda(MarmotExpression::Function& function, const Token& anchor, const std::function<void(const std::shared_ptr<MarmotType>&, const std::vector<int>&)>& bind);

	// `type` with each of `parameters` replaced by a fresh variable, and every
	// other variable, an enclosing definition's, kept.
	std::shared_ptr<MarmotType> Instantiate(const std::shared_ptr<MarmotType>& type, const std::vector<int>& parameters);

	MarmotResult::TypeResult ResolveFunctionExpressionSignature(MarmotExpression::Function& function, const std::unordered_set<int>& outer_visible_type_vars);

	std::shared_ptr<MarmotType> ApplySubstitution(const std::shared_ptr<MarmotType>& type);

	// Inference binds a type variable after the types around it are recorded,
	// so once the module is checked every recorded type gets what its
	// variables were bound to. Lowering specializes by these types.
	void ResolveRecordedTypes(MarmotStatement& statement);
	void ResolveRecordedTypes(MarmotExpression& expression);
	void ResolveRecordedTypes(MarmotPattern& pattern);

	void CheckCallTypeDecided(const MarmotExpression::Call& call);

	bool OccursCheck(int var_id, const std::shared_ptr<MarmotType>& type);

	bool OccursCheck(int var_id, const std::shared_ptr<MarmotType>& type, std::unordered_set<const MarmotType*>& visited);

	std::string DescribeConstraint(const MarmotType::ClassConstraint& constraint) const;

	CompilerError MakeConstraintFailureError(const Token& token, const MarmotType::ClassConstraint& constraint, std::optional<std::string_view> suggestion = std::nullopt) const;

	CompilerError MakeUnificationError(const Token& token, const std::shared_ptr<MarmotType>& left, const std::shared_ptr<MarmotType>& right, UnifyDiagnosticMode diagnostic_mode) const;

	CompilerError MakeFunctionArityError(const Token& token, size_t left_count, size_t right_count, UnifyDiagnosticMode diagnostic_mode) const;

	CompilerError MakeTupleArityError(const Token& token, size_t left_count, size_t right_count, UnifyDiagnosticMode diagnostic_mode) const;

	std::optional<std::vector<std::pair<std::string, std::shared_ptr<MarmotType>>>> ResolveGenericTypeArguments(const std::shared_ptr<MarmotType>& prototype, const std::shared_ptr<MarmotType>& concrete_type) const;

	std::shared_ptr<MarmotType> ResolveAssociatedType(const MarmotType::AssociatedType& associated_type);

	std::shared_ptr<MarmotType> ReduceProjectionByEqualityConstraint(const std::shared_ptr<MarmotType>& type) const;

	std::optional<ResolvedInstanceMatch> FindMatchingInstance(const std::string& class_name, const std::vector<std::shared_ptr<MarmotType>>& type_args) const;

	std::vector<ResolvedInstanceMatch> FindMatchingInstances(const std::string& class_name, const std::vector<std::shared_ptr<MarmotType>>& type_args) const;

	std::optional<CompilerError> CheckConstraintHolds(const Token& token, const MarmotType::ClassConstraint& resolved_constraint, size_t depth, std::optional<std::string_view> suggestion = std::nullopt);

	void RegisterIdentityConversion(const std::shared_ptr<MarmotType>& from_type, const std::shared_ptr<MarmotType>& to_type);

	MarmotResult::TypeResult ResolveIndexableElementType(const Token& op, const std::shared_ptr<MarmotType>& container_type, const std::shared_ptr<MarmotType>& index_type);

	std::optional<CompilerError> DischargeEqualityConstraint(const Token& token, const MarmotType::ClassConstraint& constraint, const std::shared_ptr<MarmotType>& resolved_lhs, const std::shared_ptr<MarmotType>& resolved_rhs);

	bool IsSatisfiedByActiveConstraint(const MarmotType::ClassConstraint& resolved_constraint);

	MarmotResult::TypeResult ValidateFunctionConstraints(const Token& token, const MarmotType::FunctionType& function_type);
	// Checks the constraints of each instance pending since `from` whose types
	// are decided, and keeps the rest for an enclosing call.
	std::optional<CompilerError> CheckDecidedPendingInstances(size_t from);
	// A generic used as one value: an anonymous generic lambda, or a generic
	// function named anywhere but as a callee. Its types come from the type
	// expected of it or from what is in scope, as a lambda's do.
	MarmotResult::TypeResult UseGenericInstance(const Token& token, std::shared_ptr<MarmotType> instance, std::string_view undecided_message);
	bool IsDecided(const std::shared_ptr<MarmotType>& type) const;

	MarmotResult::TypeResult ValidateInstanceConstraints(const Token& token, const InstanceInfo& instance_info, const TypeEnvironment& substitutions, size_t depth);

	std::shared_ptr<MarmotType> NarrowClassMethodType(const std::string& class_name, const ClassInfo& class_info, const std::shared_ptr<MarmotType>& declared_method_type, const std::vector<std::shared_ptr<MarmotType>>& known_arg_types, size_t arity);

	std::optional<CompilerError> TryMakeGenericParameterMismatchError(const Token& token, const std::shared_ptr<MarmotType>& left, const std::shared_ptr<MarmotType>& right) const;

	MarmotResult::TypeResult Unify(const Token& token, std::shared_ptr<MarmotType>& left, std::shared_ptr<MarmotType>& right, UnifyDiagnosticMode diagnostic_mode = UnifyDiagnosticMode::Symmetric);

	MarmotResult::TypeResult UnifyTypeArguments(const Token& token, std::vector<std::shared_ptr<MarmotType>>& left, std::vector<std::shared_ptr<MarmotType>>& right, UnifyDiagnosticMode diagnostic_mode);

	MarmotResult::TypeResult operator()(MarmotStatement::ExpressionStatement& simple);

	MarmotResult::TypeResult operator()(MarmotStatement::VariableDefinition& def);

	MarmotResult::TypeResult operator()(MarmotStatement::TupleDefinition& def_tuple);

	MarmotResult::TypeResult operator()(MarmotStatement::FunctionDefinition& defun);


	MarmotResult::TypeResult operator()(MarmotStatement::ForeignDefinition& foreign_stmt);

	MarmotResult::TypeResult operator()(MarmotStatement::Struct& struct_stmt);

	MarmotResult::TypeResult operator()(MarmotStatement::Union& union_stmt);

	MarmotResult::TypeResult operator()(MarmotStatement::Class& typeclass_stmt);

	MarmotResult::TypeResult operator()(MarmotStatement::Instance& instance_stmt);

	MarmotResult::TypeResult RegisterInstance(MarmotStatement::Instance& instance_stmt);

	std::optional<CompilerError> ByteLiteralBesideInt(const MarmotExpression::Binary& binary, const std::shared_ptr<MarmotType>& left_type, const std::shared_ptr<MarmotType>& right_type);

	MarmotResult::TypeResult operator()(MarmotStatement::TypeAlias& type_alias);

	MarmotResult::TypeResult operator()(MarmotExpression::As& as);

	MarmotResult::TypeResult operator()(MarmotExpression::Binary& binary);

	std::optional<CompilerError> RequireOperandClass(const Token& op, std::string_view class_name, const std::shared_ptr<MarmotType>& resolved_operand);

	MarmotResult::TypeResult CheckBinaryOperator(MarmotExpression::Binary& binary, std::shared_ptr<MarmotType>& left_type, std::shared_ptr<MarmotType>& right_type);

	MarmotResult::TypeResult operator()(MarmotExpression::Group& group);

	MarmotResult::TypeResult operator()(MarmotExpression::Tuple& tuple);

	MarmotResult::TypeResult operator()(MarmotExpression::UnaryPrefix& unary);

	MarmotResult::TypeResult operator()(MarmotExpression::Send& send);

	MarmotResult::TypeResult operator()(MarmotExpression::Receive& receive);

	MarmotResult::TypeResult CheckConstrainedMethodCall(MarmotExpression::Call& call, const ClassInfo& tc_info, const std::shared_ptr<MarmotType>& method_type, const MarmotType::ClassConstraint& constraint, std::vector<std::shared_ptr<MarmotType>>& arg_results);

	MarmotResult::TypeResult CheckInstanceMethodCall(MarmotExpression::Call& call, const std::string& qualifier, const std::string& method_name, const ClassInfo& tc_info, const std::shared_ptr<MarmotType>& method_type, std::vector<std::shared_ptr<MarmotType>>&& arg_results);

	MarmotResult::TypeResult operator()(MarmotExpression::Call& call);
	MarmotResult::TypeResult CheckCall(MarmotExpression::Call& call);

	MarmotResult::TypeResult operator()(MarmotExpression::MemberAccess& get);


	MarmotResult::TypeResult operator()(MarmotExpression::NameAccess& variable);



	MarmotResult::TypeResult operator()(MarmotExpression::Literal& literal);

	MarmotResult::TypeResult operator()(MarmotExpression::Function& function);

	MarmotResult::TypeResult operator()(MarmotExpression::Construct& construct);

	std::optional<CompilerError> PlaceNamedFields(MarmotExpression::Construct& construct, const MarmotType::StructType& struct_type);

	MarmotResult::TypeResult operator()(MarmotExpression::RecordUpdate& record_update);

	MarmotResult::TypeResult operator()(MarmotExpression::Array& array);

	MarmotResult::TypeResult operator()(MarmotExpression::IndexAccess& array_get);


	MarmotResult::TypeResult operator()(MarmotExpression::RangeBinary& range_binary);

	MarmotResult::TypeResult operator()(MarmotExpression::RangeTernary& range_ternary);

	MarmotResult::TypeResult operator()(MarmotExpression::IfElse& if_else);

	MarmotResult::TypeResult operator()(MarmotExpression::Block& block);

	MarmotResult::TypeResult operator()(MarmotExpression::Match& match);

	MarmotResult::TypeResult operator()(MarmotExpression::Case& case_expr);

	template <typename IterationNode>
	MarmotResult::TypeResult ResolveIteration(IterationNode& node, const std::shared_ptr<MarmotType>& range_type, std::string_view error_prefix);

	MarmotResult::TypeResult operator()(MarmotExpression::For& for_expr);

	MarmotResult::TypeResult operator()(MarmotExpression::ArrayComprehension& comp);



	bool HasActiveConstraint(const std::string& class_name, const std::shared_ptr<MarmotType>& type);

	std::optional<CompilerError> EnsureTransferable(const Token& token, const std::shared_ptr<MarmotType>& type);

	std::optional<CompilerError> EnsureTransferable(const Token& token, const std::shared_ptr<MarmotType>& type, std::unordered_set<const MarmotType*>& visited);
};
