#pragma once

#include <array>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "Compiler/Error/CompilerError.h"
#include "Compiler/Module/TypeclassMetadata.h"
#include "Compiler/Result/Result.h"
#include "Compiler/TypeTransform/TypeTransform.h"
#include "Support/Optional.h"

class PatternCoverage;

struct TypePairHash
{
	std::size_t operator()(const std::pair<MarmotType*, MarmotType*>& pair) const noexcept;
};

class TypeChecker
{
	friend class NominalUnifyGuard;

public:
	using TypeEnvironment = std::unordered_map<std::string, std::shared_ptr<MarmotType>>;
	using AssociatedTypeEnvironment = std::unordered_map<std::string, std::shared_ptr<MarmotType>>;
	enum class UnifyDiagnosticMode
	{
		Symmetric,
		CommonType,
		ReferenceWrite,
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
	struct LocalGenericFunction
	{
		BindingId m_binding;
		LexicalFunctionId m_function;
		std::vector<int> m_parameters;

		LocalGenericFunction(BindingId binding, LexicalFunctionId function, std::vector<int> parameters);
	};

	struct TypeScope
	{
		TypeScope* m_parent;
		TypeEnvironment m_names;
		std::unordered_map<BindingId, std::shared_ptr<MarmotType>> m_bindings;
		std::vector<LocalGenericFunction> m_generic_functions;

		explicit TypeScope(TypeScope* parent);
		explicit TypeScope(TypeEnvironment names);
	};

	static void ApplyTransferableParameterConstraints(MarmotType::FunctionType& signature);
	static std::string ConstructorKey(const std::string& module_name, const std::string& constructor_name);
	static bool HasTypeVariables(const std::shared_ptr<MarmotType>& type);
	static std::unordered_set<int> CollectTypeVariableIds(const std::shared_ptr<MarmotType>& type);
	static const std::string& ConstrainedClassName(const MarmotType::ClassConstraint& constraint);
	static void AppendUniqueConstraint(std::vector<MarmotType::ClassConstraint>& constraints, MarmotType::ClassConstraint&& constraint);
	static std::vector<MarmotType::ClassConstraint> CollectSignatureConstraints(
		const std::vector<std::shared_ptr<MarmotType>>& param_types,
		const std::shared_ptr<MarmotType>& return_type);

	static std::shared_ptr<MarmotType> MakeGlobalFunctionType(const std::vector<std::shared_ptr<MarmotType>>& parameters, const std::shared_ptr<MarmotType>& result, const std::vector<MarmotType::ClassConstraint>& constraints);

	struct CheckingAssumptions
	{
		std::vector<MarmotType::ClassConstraint> m_constraints;
		std::unordered_map<int, std::string> m_rigid_type_vars;
		std::vector<LexicalFunctionId> m_defining_generic_functions;
	};

	struct CheckingContext
	{
		TypeScope* m_scope;
		std::shared_ptr<MarmotType> m_expected_expr_type;
		std::shared_ptr<MarmotType> m_expected_return_type;
		std::shared_ptr<const CheckingAssumptions> m_assumptions;

		explicit CheckingContext(TypeScope& scope);
		CheckingContext WithScope(TypeScope& scope) const;
		CheckingContext WithExpectedType(std::shared_ptr<MarmotType> type) const;
		CheckingContext WithAssumptions(std::shared_ptr<const CheckingAssumptions> assumptions) const;
		CheckingContext ForFunction(std::shared_ptr<MarmotType> return_type, const std::vector<MarmotType::ClassConstraint>& constraints) const;
	};

	struct GenericContext
	{
		CheckingContext m_context;
		std::vector<std::pair<int, std::string>> m_parameters;

		GenericContext(CheckingContext context, std::vector<std::pair<int, std::string>> parameters);
	};

	enum class NameUse
	{
		Value,
		Callee
	};

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

	MarmotProgramTree m_program_tree;
	TypeEnvironment m_imported_types;
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
	// A generic used as a value before its types are decided, as the argument of a
	// call that decides them or a definition a later use does. Its constraints wait,
	// with the ones in force where it was used, until its types are decided; one still
	// undecided once the module is checked is an error.
	struct PendingInstance
	{
		Token m_token;
		std::shared_ptr<MarmotType> m_instance;
		std::shared_ptr<const CheckingAssumptions> m_assumptions;
		std::string m_undecided_message;

		PendingInstance(const Token& token, std::shared_ptr<MarmotType> instance, std::shared_ptr<const CheckingAssumptions> assumptions, std::string undecided_message);
	};
	std::vector<PendingInstance> m_pending_instances;
	// Generic parameter identities survive their checking contexts so final
	// inference validation can distinguish them from undecided call variables.
	std::unordered_map<int, std::string> m_generic_parameter_names;
	// Calls whose result type nothing decided, found once the module is checked.
	std::vector<CompilerError> m_undecided_call_errors;
	std::string m_file_name;
	const std::vector<std::string>& m_source_lines;
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
	[[nodiscard]] std::unordered_map<int, std::string> TakeGenericParameterNames() &&;

	// Extract type signatures from parsed AST (for parallel type checking)
	static TypeEnvironment ExtractTypeSignatures(const MarmotProgramTree& ast, const std::unordered_set<std::string>* exported_symbols = nullptr);

private:

	MarmotResult::TypeResult Evaluate(const CheckingContext& context, const std::unique_ptr<MarmotStatement>& statement);

	MarmotResult::TypeResult Evaluate(const CheckingContext& context, const std::unique_ptr<MarmotExpression>& expression, NameUse use = NameUse::Value);

	MarmotResult::TypeResult CheckRefNew(const CheckingContext& context, const Token& token, const std::unique_ptr<MarmotExpression>& value);

	MarmotResult::TypeResult CheckRefAccess(const CheckingContext& context, const std::unique_ptr<MarmotExpression>& cell, const std::unique_ptr<MarmotExpression>* value, const Token& token, std::string_view error_message);

	void BindParameters(const CheckingContext& context, const FunctionParameters& parameters, const std::vector<std::shared_ptr<MarmotType>>& types);
	void BindValue(const CheckingContext& context, const Token& name, const MarmotCompiler::Optional<BindingId>& binding, std::shared_ptr<MarmotType> type);
	std::shared_ptr<MarmotType>* FindBindingType(const CheckingContext& context, const BindingId& binding);
	const std::shared_ptr<MarmotType>* FindValueType(const CheckingContext& context, const MarmotExpression::NameAccess& name) const;

	MarmotResult::TypeResult CheckFunctionBody(const CheckingContext& context, const std::unique_ptr<MarmotExpression>& body, std::shared_ptr<MarmotType>& return_type, const std::vector<MarmotType::ClassConstraint>& constraints, const Token& token);

	template <typename Fn>
	std::invoke_result_t<Fn, const CheckingContext&> InScope(const CheckingContext& context, Fn&& check)
	{
		TypeScope scope(context.m_scope);
		const CheckingContext scoped_context = context.WithScope(scope);
		return std::forward<Fn>(check)(scoped_context);
	}

	GenericContext ForGenericParameters(const CheckingContext& context, const TypeEnvironment& parameters, LexicalFunctionId function);
	const LocalGenericFunction* FindLocalGeneric(const CheckingContext& context, const BindingId& binding) const;


	MarmotResult::TypeResult CheckPattern(const CheckingContext& context, MarmotPattern& pattern, const std::shared_ptr<MarmotType>& expected_type);

	MarmotCompiler::Optional<CompilerError> CheckCaseReachable(const PatternCoverage& coverage, const MarmotExpression::Case& match_case);
	MarmotCompiler::Optional<CompilerError> CheckCaseGuard(const CheckingContext& context, MarmotExpression::Case& case_expr);

	MarmotCompiler::Optional<MarmotPrimitive> CalledPrimitive(const CheckingContext& context, const MarmotExpression::Call& call) const;

	MarmotResult::TypeResult CheckSpawnCall(const CheckingContext& context, MarmotExpression::Call& call);

	MarmotCompiler::Optional<CompilerError> CheckSpawnArgument(const CheckingContext& context, MarmotExpression::Call& call, const MarmotType::FunctionType& function_type);

	MarmotCompiler::Optional<CompilerError> CheckPrimitiveDeclaration(const MarmotStatement::ForeignDefinition& foreign) const;


	std::shared_ptr<MarmotType>* FindNameType(const CheckingContext& context, const std::string& name);

	const std::shared_ptr<MarmotType>* FindNameType(const CheckingContext& context, const std::string& name) const;

	std::shared_ptr<MarmotType> FreshTypeVar();

	std::shared_ptr<MarmotType> Freshen(const std::shared_ptr<MarmotType>& type);

	TypeTransform::Freshener MakeLambdaFresheningContext(const CheckingContext& context);

	std::unordered_set<int> CollectEnclosingTypeVariableIds(const CheckingContext& context);

	MarmotCompiler::Optional<CompilerError> CheckGenericParametersStayGeneric(const CheckingContext& context, const std::vector<std::pair<int, std::string>>& generic_params, const std::vector<Token>& declared_params, const Token& definition_token);

	std::shared_ptr<MarmotType> InstantiateWrittenType(const CheckingContext& context, const std::shared_ptr<MarmotType>& written_type) const;

	// The name of the generic parameter a resolved type stands for, when it is one.
	MarmotCompiler::Optional<std::string> RigidParameterName(const CheckingContext& context, const std::shared_ptr<MarmotType>& resolved_type) const;

	MarmotResult::TypeResult TypeCheckGenericLambdaDefinition(const CheckingContext& context, MarmotStatement::VariableDefinition& def, MarmotExpression::Function& function);

	// Checks the body once for any choice of the generic parameters. `bind` is
	// given the generic type and its own variables before the body, so a named
	// lambda can call itself.
	MarmotResult::TypeResult CheckGenericLambda(const CheckingContext& context, MarmotExpression::Function& function, const Token& anchor, const BindingId& self, const std::function<void(const std::shared_ptr<MarmotType>&, const std::vector<int>&)>& bind);

	MarmotResult::TypeResult CheckLambda(const CheckingContext& context, MarmotExpression::Function& function, const BindingId& self);

	// `type` with each of `parameters` replaced by a fresh variable, and every
	// other variable, an enclosing definition's, kept.
	std::shared_ptr<MarmotType> Instantiate(const CheckingContext& context, const std::shared_ptr<MarmotType>& type, const std::vector<int>& parameters);

	MarmotResult::TypeResult ResolveFunctionExpressionSignature(const CheckingContext& context, MarmotExpression::Function& function, const std::unordered_set<int>& outer_visible_type_vars);

	std::shared_ptr<MarmotType> ApplySubstitution(const CheckingContext& context, const std::shared_ptr<MarmotType>& type);

	// Inference binds a type variable after the types around it are recorded,
	// so once the module is checked every recorded type gets what its
	// variables were bound to. Lowering specializes by these types.
	void ResolveRecordedTypes(const CheckingContext& context, MarmotStatement& statement);
	void ResolveRecordedTypes(const CheckingContext& context, MarmotExpression& expression);
	void ResolveRecordedTypes(const CheckingContext& context, MarmotPattern& pattern);

	void CheckCallTypeDecided(const MarmotExpression::Call& call);

	bool OccursCheck(const CheckingContext& context, int var_id, const std::shared_ptr<MarmotType>& type);

	bool OccursCheck(const CheckingContext& context, int var_id, const std::shared_ptr<MarmotType>& type, std::unordered_set<const MarmotType*>& visited);

	std::string DescribeConstraint(const CheckingContext& context, const MarmotType::ClassConstraint& constraint) const;

	CompilerError MakeConstraintFailureError(const CheckingContext& context, const Token& token, const MarmotType::ClassConstraint& constraint, MarmotCompiler::Optional<std::string_view> suggestion = MarmotCompiler::None) const;

	CompilerError MakeUnificationError(const Token& token, const std::shared_ptr<MarmotType>& left, const std::shared_ptr<MarmotType>& right, UnifyDiagnosticMode diagnostic_mode) const;

	CompilerError MakeFunctionArityError(const Token& token, size_t left_count, size_t right_count, UnifyDiagnosticMode diagnostic_mode) const;

	CompilerError MakeTupleArityError(const Token& token, size_t left_count, size_t right_count, UnifyDiagnosticMode diagnostic_mode) const;

	MarmotCompiler::Optional<std::vector<std::pair<std::string, std::shared_ptr<MarmotType>>>> ResolveGenericTypeArguments(const std::shared_ptr<MarmotType>& prototype, const std::shared_ptr<MarmotType>& concrete_type) const;

	std::shared_ptr<MarmotType> ResolveAssociatedType(const CheckingContext& context, const MarmotType::AssociatedType& associated_type);

	std::shared_ptr<MarmotType> ReduceProjectionByEqualityConstraint(const CheckingContext& context, const std::shared_ptr<MarmotType>& type) const;

	MarmotCompiler::Optional<ResolvedInstanceMatch> FindMatchingInstance(const std::string& class_name, const std::vector<std::shared_ptr<MarmotType>>& type_args) const;

	std::vector<ResolvedInstanceMatch> FindMatchingInstances(const std::string& class_name, const std::vector<std::shared_ptr<MarmotType>>& type_args) const;

	MarmotCompiler::Optional<CompilerError> CheckConstraintHolds(const CheckingContext& context, const Token& token, const MarmotType::ClassConstraint& resolved_constraint, size_t depth, MarmotCompiler::Optional<std::string_view> suggestion = MarmotCompiler::None);

	void RegisterIdentityConversion(const std::shared_ptr<MarmotType>& from_type, const std::shared_ptr<MarmotType>& to_type);

	MarmotResult::TypeResult ResolveIndexableElementType(const CheckingContext& context, const Token& op, const std::shared_ptr<MarmotType>& container_type, const std::shared_ptr<MarmotType>& index_type);

	MarmotCompiler::Optional<CompilerError> DischargeEqualityConstraint(const CheckingContext& context, const Token& token, const MarmotType::ClassConstraint& constraint, const std::shared_ptr<MarmotType>& resolved_lhs, const std::shared_ptr<MarmotType>& resolved_rhs);

	bool IsSatisfiedByActiveConstraint(const CheckingContext& context, const MarmotType::ClassConstraint& resolved_constraint);

	MarmotResult::TypeResult ValidateFunctionConstraints(const CheckingContext& context, const Token& token, const MarmotType::FunctionType& function_type);
	// Checks the constraints of each instance pending since `from` whose types
	// are decided, and keeps the rest for an enclosing call.
	MarmotCompiler::Optional<CompilerError> CheckDecidedPendingInstances(const CheckingContext& context, size_t from);
	// A generic used as one value: an anonymous generic lambda, or a generic
	// function named anywhere but as a callee. Its types come from the type
	// expected of it or from what is in scope, as a lambda's do.
	MarmotResult::TypeResult UseGenericInstance(const CheckingContext& context, const Token& token, std::shared_ptr<MarmotType> instance, std::string_view undecided_message);
	bool IsDecided(const CheckingContext& context, const std::shared_ptr<MarmotType>& type) const;

	MarmotResult::TypeResult ValidateInstanceConstraints(const CheckingContext& context, const Token& token, const InstanceInfo& instance_info, const TypeEnvironment& substitutions, size_t depth);

	std::shared_ptr<MarmotType> NarrowClassMethodType(const CheckingContext& context, const std::string& class_name, const ClassInfo& class_info, const std::shared_ptr<MarmotType>& declared_method_type, const std::vector<std::shared_ptr<MarmotType>>& known_arg_types, size_t arity);

	MarmotCompiler::Optional<CompilerError> TryMakeGenericParameterMismatchError(const Token& token, const std::shared_ptr<MarmotType>& left, const std::shared_ptr<MarmotType>& right) const;

	MarmotResult::TypeResult Unify(const CheckingContext& context, const Token& token, const std::shared_ptr<MarmotType>& left, const std::shared_ptr<MarmotType>& right, UnifyDiagnosticMode diagnostic_mode = UnifyDiagnosticMode::Symmetric);

	MarmotResult::TypeResult UnifyTypeArguments(const CheckingContext& context, const Token& token, std::vector<std::shared_ptr<MarmotType>>& left, std::vector<std::shared_ptr<MarmotType>>& right);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotStatement::ExpressionStatement& simple);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotStatement::VariableDefinition& def);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotStatement::TupleDefinition& def_tuple);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotStatement::FunctionDefinition& defun);


	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotStatement::ForeignDefinition& foreign_stmt);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotStatement::Struct& struct_stmt);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotStatement::Union& union_stmt);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotStatement::Class& typeclass_stmt);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotStatement::Instance& instance_stmt);

	MarmotResult::TypeResult RegisterInstance(const CheckingContext& context, MarmotStatement::Instance& instance_stmt);

	using BinaryOperandTypes = std::array<std::shared_ptr<MarmotType>, 2u>;
	MarmotResult::Result<BinaryOperandTypes> CheckBinaryOperands(const CheckingContext& context, Token::Name op, const std::unique_ptr<MarmotExpression>& left, const std::unique_ptr<MarmotExpression>& right);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotStatement::TypeAlias& type_alias);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::As& as);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::Binary& binary);

	MarmotCompiler::Optional<CompilerError> RequireOperandClass(const CheckingContext& context, const Token& op, std::string_view class_name, const std::shared_ptr<MarmotType>& resolved_operand);

	MarmotResult::TypeResult CheckBinaryOperator(const CheckingContext& context, MarmotExpression::Binary& binary, std::shared_ptr<MarmotType>& left_type, std::shared_ptr<MarmotType>& right_type);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::Group& group);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::Tuple& tuple);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::UnaryPrefix& unary);

	std::vector<const MarmotType::ClassConstraint*> MatchingMethodConstraints(const CheckingContext& context, const std::string& qualifier, const ClassInfo& info, const std::shared_ptr<MarmotType>& method, const std::vector<std::shared_ptr<MarmotType>>& arguments);

	MarmotResult::TypeResult CheckConstrainedMethod(const CheckingContext& context, const Token& token, const ClassInfo& tc_info, const std::shared_ptr<MarmotType>& method_type, const MarmotType::ClassConstraint& constraint, std::vector<std::shared_ptr<MarmotType>>& arg_results);

	MarmotResult::TypeResult CheckInstanceMethod(const CheckingContext& context, const Token& token, const std::string& qualifier, const std::string& method_name, const ClassInfo& tc_info, const std::shared_ptr<MarmotType>& method_type, std::vector<std::shared_ptr<MarmotType>>&& arg_results);

	MarmotResult::TypeResult CheckInstanceMethodCall(const CheckingContext& context, MarmotExpression::Call& call, const std::string& qualifier, const std::string& method_name, const ClassInfo& tc_info, const std::shared_ptr<MarmotType>& method_type, std::vector<std::shared_ptr<MarmotType>>&& arg_results);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::Call& call);
	MarmotResult::TypeResult CheckCall(const CheckingContext& context, MarmotExpression::Call& call);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::MemberAccess& get);


	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::NameAccess& variable, NameUse use);



	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::Literal& literal);

	MarmotResult::TypeResult CheckLiteral(MarmotLiteral& literal, const std::shared_ptr<MarmotType>& expected) const;

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::Function& function);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::Construct& construct);

	MarmotCompiler::Optional<CompilerError> PlaceNamedFields(MarmotExpression::Construct& construct, const MarmotType::StructType& struct_type);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::RecordUpdate& record_update);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::Array& array);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::IndexAccess& array_get);


	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::RangeBinary& range_binary);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::RangeTernary& range_ternary);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::IfElse& if_else);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::Block& block);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::Match& match);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::Case& case_expr);

	template <typename IterationNode>
	MarmotResult::TypeResult ResolveIteration(const CheckingContext& context, IterationNode& node, const std::shared_ptr<MarmotType>& range_type, std::string_view error_prefix);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::For& for_expr);

	MarmotResult::TypeResult operator()(const CheckingContext& context, MarmotExpression::ArrayComprehension& comp);



	bool HasActiveConstraint(const CheckingContext& context, const std::string& class_name, const std::shared_ptr<MarmotType>& type);

	MarmotCompiler::Optional<CompilerError> EnsureTransferable(const CheckingContext& context, const Token& token, const std::shared_ptr<MarmotType>& type);

	void InferCaptureTransferability(const CheckingContext& context, MarmotExpression::Function& function, const BindingId& self);

	MarmotCompiler::Optional<CompilerError> EnsureTransferable(const CheckingContext& context, const Token& token, const std::shared_ptr<MarmotType>& type, std::unordered_set<std::shared_ptr<MarmotType>>& visited);
};
