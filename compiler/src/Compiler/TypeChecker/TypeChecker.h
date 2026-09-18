#pragma once

#include <array>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "Common/Error/Error.h"
#include "Compiler/Result/Result.h"

class ExpectedTypeGuard;

struct TypePairHash
{
	std::size_t operator()(const std::pair<MidoriType*, MidoriType*>& pair) const noexcept;
};

class TypeChecker
{
	friend class ExpectedTypeGuard;
	friend class NominalUnifyGuard;
	friend class DefiningGenericGuard;

public:
	using TypeEnvironment = std::unordered_map<std::string, std::shared_ptr<MidoriType>>;
	using TypeclassInstanceTypeMap = std::unordered_map<std::string, std::vector<std::vector<std::shared_ptr<MidoriType>>>>;
	using AssociatedTypeEnvironment = std::unordered_map<std::string, std::shared_ptr<MidoriType>>;
	using TypeclassInstanceAssociatedTypeBindingMap = std::unordered_map<std::string, std::vector<AssociatedTypeEnvironment>>;
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
		std::vector<MidoriType::ClassConstraint> m_superclasses;
		AssociatedTypeEnvironment m_associated_types;
		std::unordered_map<std::string, std::shared_ptr<MidoriType>> m_method_types;
		std::unordered_set<std::string> m_methods_with_defaults;

		ClassInfo() = default;
		ClassInfo(const std::string& name, std::vector<std::string>&& params, std::vector<MidoriType::ClassConstraint>&& supers, AssociatedTypeEnvironment&& associated_types, std::unordered_map<std::string, std::shared_ptr<MidoriType>>&& methods, std::unordered_set<std::string>&& defaults);
	};

private:
	using TypeEnvironmentStack = std::vector<TypeEnvironment>;
	using TypeSubstitution = std::unordered_map<int, std::shared_ptr<MidoriType>>;
	using GenericFunctionNames = std::unordered_set<std::string>;
	using GenericStructNames = std::unordered_set<std::string>;
	using GenericUnionNames = std::unordered_set<std::string>;
	using TypeDefinitionMap = std::unordered_map<std::string, std::shared_ptr<MidoriType>>;
	struct FresheningContext
	{
		std::unordered_map<std::string, std::shared_ptr<MidoriType>> m_generic_params;
		std::unordered_map<const MidoriType*, std::shared_ptr<MidoriType>> m_type_cache;  // Maps type pointers to their fresh versions (for cycle detection)
	};

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
		std::vector<std::shared_ptr<MidoriType>> m_type_args;
		std::vector<MidoriType::ClassConstraint> m_constraints;
		AssociatedTypeEnvironment m_associated_type_bindings;
		std::unordered_map<std::string, std::unique_ptr<MidoriStatement>> m_method_impls;
		bool m_is_derived = false;

		InstanceInfo() = default;
		InstanceInfo(const std::string& tc_name, std::vector<std::shared_ptr<MidoriType>>&& args, std::vector<MidoriType::ClassConstraint>&& constraints, AssociatedTypeEnvironment&& associated_type_bindings, std::unordered_map<std::string, std::unique_ptr<MidoriStatement>>&& methods, bool is_derived = false);
	};

	struct ResolvedInstanceMatch
	{
		const InstanceInfo* m_instance = nullptr;
		TypeEnvironment m_substitutions;
	};

	class ScopeSession;

	MidoriProgramTree m_program_tree;
	TypeEnvironmentStack m_name_type_table;
	TypeSubstitution m_type_substitution;
	std::unordered_map<std::string, ClassInfo> m_classes;
	std::unordered_map<InstanceKey, InstanceInfo, InstanceKeyHash> m_instances;
	std::unordered_set<std::pair<MidoriType*, MidoriType*>, TypePairHash> m_unify_visited;
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
	std::vector<MidoriType::ClassConstraint> m_active_constraints;
	// Names of the generic definitions whose bodies are currently being checked.
	// A call to one of these from inside its own body must not be freshened: it is
	// recursion, not a fresh instantiation.
	std::vector<std::string> m_defining_generic_names;
	std::string m_file_name;
	const std::vector<std::string>& m_source_lines;
	std::shared_ptr<MidoriType> m_expected_return_type;
	std::shared_ptr<MidoriType> m_expected_expr_type; 
	int m_next_type_var_id;
	static const std::array<Token::Name, 5u> kBinaryArithmeticOperators;
	static const std::array<Token::Name, 1u> kBinaryConcatenationOperators;
	static const std::array<Token::Name, 4u> kBinaryPartialOrderComparisonOperators;
	static const std::array<Token::Name, 2u> kBinaryEqualityOperators;
	static const std::array<Token::Name, 2u> kBinaryLogicalOperators;
	static const std::array<Token::Name, 5u> kBinaryBitwiseOperators;
	static constexpr size_t s_max_instance_constraint_depth = 16u;

public:

	TypeChecker(MidoriProgramTree&& parser_result, std::string_view file_name, const std::vector<std::string>& source_lines, TypeEnvironment imported_types = {}, const std::unordered_map<std::string, ClassInfo>& imported_typeclasses = {}, TypeclassInstanceTypeMap imported_instance_types = {}, TypeclassInstanceAssociatedTypeBindingMap imported_instance_associated_type_bindings = {});

	MidoriResult::TypeCheckerResult TypeCheck();

	// Extract type signatures from parsed AST (for parallel type checking)
	static TypeEnvironment ExtractTypeSignatures(const MidoriProgramTree& ast, const std::unordered_set<std::string>* exported_symbols = nullptr);

private:

	MidoriResult::TypeResult Evaluate(const std::unique_ptr<MidoriStatement>& statement);

	MidoriResult::TypeResult Evaluate(const std::unique_ptr<MidoriExpression>& expression);

	TypeChecker& BeginScope();

	TypeChecker& EndScope();

	static MidoriExpression::ConditionOperandType ResolveConditionOperandType(MidoriExpression::ConditionOperandType fallback, const std::unique_ptr<MidoriExpression>& expr);

	MidoriResult::TypeResult CheckPattern(MidoriPattern& pattern, const std::shared_ptr<MidoriType>& expected_type);

	std::optional<CompilerError> CheckCaseGuard(MidoriExpression::Case& case_expr);

	std::optional<CompilerError> CheckSpawnArgument(MidoriExpression::Spawn& spawn, const MidoriType::FunctionType& function_type);

	bool IsIrrefutablePattern(const MidoriPattern& pattern, const std::shared_ptr<MidoriType>& expected_type);

	std::shared_ptr<MidoriType>* FindNameType(const std::string& name);

	const std::shared_ptr<MidoriType>* FindNameType(const std::string& name) const;

	std::shared_ptr<MidoriType> FreshTypeVar();

	std::shared_ptr<MidoriType> Freshen(const std::shared_ptr<MidoriType>& type);

	std::shared_ptr<MidoriType> Freshen(const std::shared_ptr<MidoriType>& type, FresheningContext& context);

	FresheningContext MakeLambdaFresheningContext();

	std::unordered_set<int> CollectEnclosingTypeVariableIds();

	MidoriResult::TypeResult TypeCheckGenericLambdaDefinition(MidoriStatement::VariableDefinition& def, MidoriExpression::Function& function);

	MidoriResult::TypeResult ResolveFunctionExpressionSignature(MidoriExpression::Function& function, const std::unordered_set<int>& outer_visible_type_vars);

	std::shared_ptr<MidoriType> ApplySubstitution(const std::shared_ptr<MidoriType>& type);

	std::shared_ptr<MidoriType> ApplySubstitution(const std::shared_ptr<MidoriType>& type, std::unordered_map<const MidoriType*, std::shared_ptr<MidoriType>>& cache);

	bool OccursCheck(int var_id, const std::shared_ptr<MidoriType>& type);

	bool OccursCheck(int var_id, const std::shared_ptr<MidoriType>& type, std::unordered_set<const MidoriType*>& visited);

	std::string DescribeConstraint(const MidoriType::ClassConstraint& constraint) const;

	CompilerError MakeConstraintFailureError(const Token& token, const MidoriType::ClassConstraint& constraint, std::optional<std::string_view> suggestion = std::nullopt) const;

	CompilerError MakeUnificationError(const Token& token, const std::shared_ptr<MidoriType>& left, const std::shared_ptr<MidoriType>& right, UnifyDiagnosticMode diagnostic_mode) const;

	CompilerError MakeFunctionArityError(const Token& token, size_t left_count, size_t right_count, UnifyDiagnosticMode diagnostic_mode) const;

	CompilerError MakeTupleArityError(const Token& token, size_t left_count, size_t right_count, UnifyDiagnosticMode diagnostic_mode) const;

	std::optional<std::vector<std::pair<std::string, std::shared_ptr<MidoriType>>>> ResolveGenericTypeArguments(const std::shared_ptr<MidoriType>& prototype, const std::shared_ptr<MidoriType>& concrete_type) const;

	std::shared_ptr<MidoriType> ResolveAssociatedType(const MidoriType::AssociatedType& associated_type);

	std::shared_ptr<MidoriType> ReduceProjectionByEqualityConstraint(const std::shared_ptr<MidoriType>& type) const;

	std::optional<ResolvedInstanceMatch> FindMatchingInstance(const std::string& class_name, const std::vector<std::shared_ptr<MidoriType>>& type_args) const;

	void RegisterIdentityConversion(const std::shared_ptr<MidoriType>& from_type, const std::shared_ptr<MidoriType>& to_type);

	MidoriResult::TypeResult ResolveIndexableElementType(const Token& op, const std::shared_ptr<MidoriType>& container_type, const std::shared_ptr<MidoriType>& index_type);

	std::optional<CompilerError> DischargeEqualityConstraint(const Token& token, const MidoriType::ClassConstraint& constraint, const std::shared_ptr<MidoriType>& resolved_lhs, const std::shared_ptr<MidoriType>& resolved_rhs);

	bool IsSatisfiedByActiveConstraint(const MidoriType::ClassConstraint& resolved_constraint);

	MidoriResult::TypeResult ValidateFunctionConstraints(const Token& token, const MidoriType::FunctionType& function_type);

	MidoriResult::TypeResult ValidateInstanceConstraints(const Token& token, const InstanceInfo& instance_info, const TypeEnvironment& substitutions, size_t depth);

	std::shared_ptr<MidoriType> NarrowClassMethodType(const std::string& class_name, const ClassInfo& class_info, const std::shared_ptr<MidoriType>& declared_method_type, const std::vector<std::shared_ptr<MidoriType>>& known_arg_types, size_t arity);

	std::optional<CompilerError> TryMakeGenericParameterMismatchError(const Token& token, const std::shared_ptr<MidoriType>& left, const std::shared_ptr<MidoriType>& right) const;

	MidoriResult::TypeResult Unify(const Token& token, std::shared_ptr<MidoriType>& left, std::shared_ptr<MidoriType>& right, UnifyDiagnosticMode diagnostic_mode = UnifyDiagnosticMode::Symmetric);

	MidoriResult::TypeResult UnifyTypeArguments(const Token& token, std::vector<std::shared_ptr<MidoriType>>& left, std::vector<std::shared_ptr<MidoriType>>& right, UnifyDiagnosticMode diagnostic_mode);

	MidoriResult::TypeResult operator()(MidoriStatement::ExpressionStatement& simple);

	MidoriResult::TypeResult operator()(MidoriStatement::VariableDefinition& def);

	MidoriResult::TypeResult operator()(MidoriStatement::TupleDefinition& def_tuple);

	MidoriResult::TypeResult operator()(MidoriStatement::FunctionDefinition& defun);


	MidoriResult::TypeResult operator()(MidoriStatement::ForeignDefinition& foreign_stmt);

	MidoriResult::TypeResult operator()(MidoriStatement::Struct& struct_stmt);

	MidoriResult::TypeResult operator()(MidoriStatement::Union& union_stmt);

	MidoriResult::TypeResult operator()(MidoriStatement::Class& typeclass_stmt);

	MidoriResult::TypeResult operator()(MidoriStatement::Instance& instance_stmt);

	MidoriResult::TypeResult operator()(MidoriStatement::TypeAlias& type_alias);

	MidoriResult::TypeResult operator()(MidoriExpression::As& as);

	MidoriResult::TypeResult operator()(MidoriExpression::Binary& binary);

	MidoriResult::TypeResult operator()(MidoriExpression::Group& group);

	MidoriResult::TypeResult operator()(MidoriExpression::Tuple& tuple);

	MidoriResult::TypeResult operator()(MidoriExpression::UnaryPrefix& unary);

	MidoriResult::TypeResult operator()(MidoriExpression::UnarySuffix& unary);

	MidoriResult::TypeResult operator()(MidoriExpression::Spawn& spawn);

	MidoriResult::TypeResult operator()(MidoriExpression::Join& join);

	MidoriResult::TypeResult operator()(MidoriExpression::ChannelCreate& channel_create);

	MidoriResult::TypeResult operator()(MidoriExpression::Send& send);

	MidoriResult::TypeResult operator()(MidoriExpression::Receive& receive);

	MidoriResult::TypeResult operator()(MidoriExpression::Call& call);

	MidoriResult::TypeResult operator()(MidoriExpression::MemberAccess& get);


	MidoriResult::TypeResult operator()(MidoriExpression::NameAccess& variable);



	MidoriResult::TypeResult operator()(MidoriExpression::Literal& literal);

	MidoriResult::TypeResult operator()(MidoriExpression::Function& function);

	MidoriResult::TypeResult operator()(MidoriExpression::Construct& construct);

	MidoriResult::TypeResult operator()(MidoriExpression::RecordUpdate& record_update);

	MidoriResult::TypeResult operator()(MidoriExpression::Array& array);

	MidoriResult::TypeResult operator()(MidoriExpression::IndexAccess& array_get);


	MidoriResult::TypeResult operator()(MidoriExpression::RangeBinary& range_binary);

	MidoriResult::TypeResult operator()(MidoriExpression::RangeTernary& range_ternary);

	MidoriResult::TypeResult operator()(MidoriExpression::IfElse& if_else);

	MidoriResult::TypeResult operator()(MidoriExpression::Block& block);

	MidoriResult::TypeResult operator()(MidoriExpression::Match& match);

	MidoriResult::TypeResult operator()(MidoriExpression::Case& case_expr);

	MidoriResult::TypeResult operator()(MidoriExpression::For& for_expr);

	MidoriResult::TypeResult operator()(MidoriExpression::ArrayComprehension& comp);



	bool HasActiveConstraint(const std::string& class_name, const std::shared_ptr<MidoriType>& type);

	std::optional<CompilerError> EnsureTransferable(const Token& token, const std::shared_ptr<MidoriType>& type);

	std::optional<CompilerError> EnsureTransferable(const Token& token, const std::shared_ptr<MidoriType>& type, std::unordered_set<const MidoriType*>& visited);
};
