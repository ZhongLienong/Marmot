#pragma once

#include <algorithm>
#include <cstdint>
#include <stack>
#include <unordered_set>
#include <optional>
#include <set>

#include "Common/Error/Error.h"
#include "Compiler/Result/Result.h"
#include "Compiler/BytecodeModule/BytecodeModule.h"
#include "Library/MidoriBuiltinFFIRegistry/MidoriFFIRegistry.h"
#include "GenericFunctionInfo.h"

class CodeGenerator
{
private:
	enum class LocalStorageKind : uint8_t
	{
		ValueLocal,
		CellLocal
	};

	struct ResolvedMethodCandidate
	{
		std::string m_first_type_name;
		std::string m_second_type_name;
		std::string m_resolved_name;
		bool m_has_instance = false;
	};

	struct LoopContext
	{
		std::vector<int> m_break_positions;
		int m_loop_start = 0;
		int m_continue_target = 0;
	};



	struct FunctionSignature
	{
		std::string m_base_name;
		std::vector<std::string> m_concrete_types;

		bool operator==(const FunctionSignature& other) const;
	};

	struct FunctionSignatureHash
	{
		std::size_t operator()(const FunctionSignature& sig) const;
	};

	struct TypePairHash
	{
		std::size_t operator()(const std::pair<MidoriType*, MidoriType*>& pair) const;
	};

	struct BytecodeBuilder
	{
		MidoriExecutable::Procedures m_procedures{ BytecodeStream() };
		std::vector<std::string> m_procedure_names;
		MidoriExecutable::StringPool m_string_pool;
		size_t m_current_procedure_index = 0u;
		int m_string_pool_index = 0;
		OpCode m_last_opcode = OpCode::HALT;

		BytecodeBuilder EmitByte(OpCode byte, int line) &&;
		BytecodeBuilder PopByte(int line) &&;
	};

	using TypeEnvironment = std::unordered_map<std::string, std::shared_ptr<MidoriType>>;
	using TypeclassMethodMap = std::unordered_map<std::string, std::unordered_set<std::string>>;
	using TypeclassInstanceMap = std::unordered_map<std::string, std::vector<std::string>>;
	using TypeclassInstanceTypeMap = std::unordered_map<std::string, std::vector<std::vector<std::shared_ptr<MidoriType>>>>;

	MidoriProgramTree m_program_tree;
	std::string m_file_name;
	const std::vector<std::string>& m_source_lines;
	std::optional<std::string> m_module_name;
	// While specializing a generic declared in another module, the body's
	// unqualified global references belong to that module, not this one.
	std::optional<std::string> m_specialization_source_module;
	std::unordered_set<std::string> m_export_symbols;

	BytecodeBuilder m_builder;
	std::vector<BytecodeModule::ExportedSymbol> m_tracked_exports;
	std::vector<BytecodeModule::ImportedSymbol> m_tracked_imports;

	std::unordered_map<std::string, int> m_global_variables;
	std::unordered_map<int, int> m_direct_proc_global_indices;
	std::unordered_map<std::string, int> m_local_variables;
	std::unordered_map<std::string, GenericFunctionInfo> m_generic_functions;
	std::unordered_set<std::string> m_generic_instance_methods;
	std::unordered_map<FunctionSignature, int, FunctionSignatureHash> m_specialized_functions;
	TypeEnvironment m_param_type_map;
	TypeEnvironment m_generic_type_substitution;
	TypeclassMethodMap m_class_methods;
	TypeclassInstanceMap m_class_instances;
	TypeclassInstanceTypeMap m_class_instance_type_args;
	std::unordered_map<std::string, std::vector<ResolvedMethodCandidate>> m_method_resolution_map;
	std::unordered_map<std::string, size_t> m_ffi_indices;
	std::vector<std::vector<LocalStorageKind>> m_procedure_local_kinds{ std::vector<LocalStorageKind>() };
	std::vector<int> m_procedure_capture_counts{ 0 };

	MidoriExecutable m_executable;
	std::stack<LoopContext> m_loop_contexts;
	// Code generation preserves recoverable per-module diagnostics when lowering can
	// continue, but it does not promise recovery after arbitrary internal corruption.
	MidoriResult::CompilerDiagnostics m_errors;
	int m_local_count = 0;

public:

	CodeGenerator(MidoriProgramTree&& program_tree, std::string_view file_name, const std::vector<std::string>& source_lines, std::string module_name, std::unordered_set<std::string> export_symbols, const TypeclassMethodMap& imported_class_methods = {}, const TypeclassInstanceMap& imported_class_instances = {}, const TypeclassInstanceTypeMap& imported_class_instance_type_args = {}, const std::unordered_map<std::string, GenericFunctionInfo>& imported_generic_functions = {});

	MidoriResult::CodeGeneratorResult GenerateModuleBytecode() &;
	MidoriResult::CodeGeneratorResult GenerateModuleBytecode() &&;

private:

	void AddError(CompilerError error);
	[[nodiscard]] std::optional<BytecodeModule::SourceProvenance> MakeSourceProvenance(const Token& token) const;
	[[nodiscard]] std::optional<BytecodeModule::SourceProvenance> MakeSourceProvenance(int line) const;

	void PopByte(int line);

	void EmitTextConstant(std::string_view data, int line);
	void EmitTextLiteral(const MidoriExpression::Literal& literal);
	void EmitBoolLiteral(const MidoriExpression::Literal& literal);
	void EmitFloatLiteral(const MidoriExpression::Literal& literal);
	void EmitIntegerLiteral(const MidoriExpression::Literal& literal);
	void EmitByteLiteral(const MidoriExpression::Literal& literal);
	void EmitWordLiteral(const MidoriExpression::Literal& literal);
	void EmitUnitLiteral(const MidoriExpression::Literal& literal);

	void EmitByte(OpCode byte, int line);

	void EmitTwoBytes(int byte1, int byte2, int line);

	void EmitThreeBytes(int byte1, int byte2, int byte3, int line);

	void EmitNumericConstant(MidoriInteger val, int line, bool is_integer);

	void EmitFloatConstant(MidoriFloat value, int line);

	void EmitIntegerConstant(MidoriInteger value, int line);

	void EmitByteConstant(MidoriByte value, int line);

	void EmitWordConstant(MidoriWord value, int line);

	void EmitVariable(int variable_index, OpCode op, int line);

	void EnsureProcedureMetadataSize(size_t procedure_index);

	void EnsureLocalKindCapacity(size_t procedure_index, int local_count);

	void NoteCaptureBinding(int captured_count, int line);

	int CurrentProcedureCaptureCount() const;

	LocalStorageKind GetLocalStorageKind(int variable_index) const;

	void RewriteEmittedLocalOps(int variable_index, LocalStorageKind previous_kind, LocalStorageKind new_kind);

	OpCode GetLocalLoadOpcode(int variable_index) const;

	OpCode GetLocalStoreOpcode(int variable_index) const;

	OpCode GetCellLoadOpcode() const;

	OpCode GetCellStoreOpcode() const;

	void EmitCall(int arity, int line);

	void EmitCallProc(int proc_index, int arity, int line);

	void EmitCallGlobal(int global_index, int arity, int line);

	bool MatchInstanceTypeArg(const std::shared_ptr<MidoriType>& pattern, const std::shared_ptr<MidoriType>& concrete, TypeEnvironment& substitutions, std::unordered_set<std::pair<MidoriType*, MidoriType*>, TypePairHash>& visited) const;

	bool EmitIterableNextInvocation(const std::string& resolved_name, const std::shared_ptr<MidoriType>& iter_type, int line);

	bool EmitIterableNextCall(const std::shared_ptr<MidoriType>& iter_type, const std::shared_ptr<MidoriType>& item_type, int line);

	int GetImportPlaceholder(const std::string& module_name, const std::string& symbol_name, int line, const std::optional<BytecodeModule::SourceProvenance>& source_provenance = std::nullopt);

	int EmitJump(OpCode op, int line);

	void PatchJump(int offset, int line);

	void EmitLoop(int loop_start, int line);

	bool EmitConcatenableConcat(const std::shared_ptr<MidoriType>& operand_type, int line);

	void EmitEquatableEquals(const std::shared_ptr<MidoriType>& operand_type, int line);

	void EmitOrderableCompare(const std::shared_ptr<MidoriType>& operand_type, int line);

	void EmitPopCount(int count, int line);

	void EmitInstanceMethodDefinitions();

	int CountPatternBindings(const MidoriPattern& pattern) const;

	void EmitPatternCheck(const MidoriPattern& pattern, std::vector<int>& failure_jumps, int extra_pops);

	void EmitPatternBind(const MidoriPattern& pattern);

	void EmitPatternLiteralConstant(const MidoriPattern::Literal& literal);

	void EmitPatternLiteralEquals(const MidoriPattern::Literal& literal, int line);

	void BeginLoop(int loop_start);

	void EndLoop(int line);

	void Visit(const std::unique_ptr<MidoriStatement>& statement);

	void Visit(const std::unique_ptr<MidoriExpression>& expression);

	void Visit(const std::shared_ptr<MidoriExpression>& expression);

	void DispatchStatement(MidoriStatement& statement);

	void DispatchExpression(MidoriExpression& expression);

	void operator()(MidoriStatement::ExpressionStatement& simple);

	void operator()(MidoriStatement::VariableDefinition& def);

	void operator()(MidoriStatement::TupleDefinition& def_tuple);

	void operator()(MidoriStatement::FunctionDefinition& defun);


	void operator()(MidoriStatement::ForeignDefinition& foreign);

	void operator()(MidoriStatement::Struct& struct_stmt);

	void operator()(MidoriStatement::Union& union_stmt);

	void operator()(MidoriStatement::Class& typeclass_stmt);

	void operator()(MidoriStatement::Instance& instance_stmt);

	void operator()(MidoriStatement::TypeAlias& type_alias);

	void operator()(MidoriExpression::As& as);

	void operator()(MidoriExpression::Binary& binary);

	void operator()(MidoriExpression::Group& group);

	void operator()(MidoriExpression::Tuple& tuple);

	void operator()(MidoriExpression::UnaryPrefix& unary);

	void operator()(MidoriExpression::UnarySuffix& unary);

	void operator()(MidoriExpression::Spawn& spawn);

	void operator()(MidoriExpression::Join& join);

	void operator()(MidoriExpression::ChannelCreate& channel_create);

	void operator()(MidoriExpression::Send& send);

	void operator()(MidoriExpression::Receive& receive);

	void operator()(MidoriExpression::Call& call);

	void operator()(MidoriExpression::MemberAccess& get);


	void operator()(MidoriExpression::NameAccess& variable);



	void operator()(MidoriExpression::Literal& literal);

	void operator()(MidoriExpression::Function& function);

	void operator()(MidoriExpression::Construct& construct);

	void operator()(MidoriExpression::RecordUpdate& record_update);

	void operator()(MidoriExpression::Array& array);

	void operator()(MidoriExpression::IndexAccess& array_get);


	void operator()(MidoriExpression::RangeBinary& range_binary);

	void operator()(MidoriExpression::RangeTernary& range_ternary);

	void operator()(MidoriExpression::IfElse& if_else);

	void operator()(MidoriExpression::Block& block);

	void operator()(MidoriExpression::Match& match);

	void operator()(MidoriExpression::Case& case_expr);

	void operator()(MidoriExpression::For& for_expr);

	void operator()(MidoriExpression::ArrayComprehension& comp);



	void EmitNumericConditionalJump(MidoriExpression::ConditionOperandType operand_type, std::unique_ptr<MidoriExpression>& true_branch, std::unique_ptr<MidoriExpression>& else_branch, int line);

	// Operand-position block support: locals declared while operand temporaries
	// are pending on the value stack physically live above those temporaries,
	// so their emitted indices are shifted by the pending operand count.
	struct OperandBlockShift
	{
		int m_first_local_index = 0;
		int m_offset = 0;
	};

	std::vector<OperandBlockShift> m_operand_block_shifts;
	std::unordered_map<size_t, std::unordered_set<int>> m_operand_scoped_locals;

	// Loop variables, by procedure. A loop variable is one frame slot rebound on
	// every iteration. Once a closure captures it the slot holds a cell, and an
	// ordinary cell store writes THROUGH that cell, so every closure built in the
	// loop would share it and see the value after the loop. A rebinding store
	// overwrites the slot instead, leaving earlier closures their own cell; the
	// next capture then allocates a fresh one. Only loop variables qualify: they
	// are always stored before the body can capture them. A recursive local
	// closure is captured BEFORE its store, and must keep writing into its cell.
	std::unordered_map<size_t, std::unordered_set<int>> m_rebinding_locals;

	void RegisterRebindingLocal(int variable_index);
	bool IsRebindingLocal(int variable_index) const;
	int m_operand_depth = 0;

	int EffectiveLocalIndex(int variable_index) const;

	// Blocks are not the only expressions that declare locals: match, for and
	// array comprehensions keep hidden locals (a match value slot and its pattern
	// bindings, a loop variable and iteration state) on the same value stack.
	// When one is evaluated while an operand is pending, its locals sit above that
	// operand too, and must be shifted the same way or every write lands on the
	// pending operand. The scope pops its shift on every exit, including early
	// returns, because a shift left behind silently misaddresses every later local
	// in the procedure.
	class OperandShiftScope
	{
	public:
		OperandShiftScope(CodeGenerator& generator, const std::vector<int>& hidden_local_indices);
		~OperandShiftScope();

		OperandShiftScope(const OperandShiftScope&) = delete;
		OperandShiftScope& operator=(const OperandShiftScope&) = delete;

	private:
		CodeGenerator& m_generator;
		bool m_pushed = false;
	};

	void CollectPatternLocalIndices(const MidoriPattern& pattern, std::vector<int>& indices) const;

	std::optional<int> GetFusibleLocalIndex(const MidoriExpression& expr) const;

	static std::optional<MidoriInteger> GetFusibleSmallInt(const MidoriExpression& expr);

	static OpCode GetSmallIntOpcode(MidoriInteger value);

	bool TryEmitFusedBinary(MidoriExpression::Binary& binary, int line);

	std::optional<int> TryEmitFusedConditionBranch(std::unique_ptr<MidoriExpression>& condition, int line);

	int EmitFunction(const std::vector<Token>& params, std::unique_ptr<MidoriExpression>& body, const std::string& debug_name, int line, int captured_count = 0, int direct_proc_global_index = -1);

	bool IsGenericType(const std::shared_ptr<MidoriType>& type);

	void DeduceGenericTypesRecursive(const std::shared_ptr<MidoriType>& param_type, const std::shared_ptr<MidoriType>& concrete_type, std::unordered_map<std::string, std::shared_ptr<MidoriType>>& map, std::unordered_set<std::pair<MidoriType*, MidoriType*>, TypePairHash>& visited);

	int SpecializeGenericFunction(const std::string& base_name, const std::vector<std::shared_ptr<MidoriType>>& concrete_arg_types, int line);

	std::shared_ptr<MidoriType> GetConcreteTypeForExpression(const std::unique_ptr<MidoriExpression>& expr);

	static std::shared_ptr<MidoriType> RepresentationOf(const std::shared_ptr<MidoriType>& type);

	static bool IsNewTypeErasedConversion(const std::shared_ptr<MidoriType>& from_type, const std::shared_ptr<MidoriType>& target_type);

	std::shared_ptr<MidoriType> SubstituteGenericTypes(const std::shared_ptr<MidoriType>& type, const TypeEnvironment& generic_type_map);

	std::optional<std::string> ResolveMethodNameForCall(const std::string& callee_name, const MidoriExpression::Call& call, int line);

	std::optional<std::string> ResolveConcreteTypeclassMethodName(const std::string& callee_name, const MidoriExpression::Call& call, int line);

	bool EmitResolvedNameGetGlobal(const std::string& resolved_name, int line);

	std::optional<int> ResolveResolvedNameGlobalIndex(const std::string& resolved_name, int line);

	std::optional<std::string> ResolveInstanceName(const std::string& class_name, const std::string& base_name) const;

	std::optional<std::string> FindGenericFunctionKey(const std::string& resolved_name) const;

	bool RejectGenericFunctionValueUse(const Token& name);

	std::optional<std::string> ResolveInstanceNameForTypeArgs(const std::string& class_name, const std::string& method_name, const std::vector<std::shared_ptr<MidoriType>>& concrete_type_args) const;

	bool AreTypeArgsEqual(const std::vector<std::shared_ptr<MidoriType>>& left, const std::vector<std::shared_ptr<MidoriType>>& right) const;

	void AddInstanceTypeArgs(const std::string& class_name, const std::vector<std::shared_ptr<MidoriType>>& type_args);

	bool EmitCountableCall(const MidoriExpression::UnaryPrefix& unary, const std::shared_ptr<MidoriType>& count_type, int line);

	bool EmitIndexableCall(MidoriExpression::IndexAccess& array_get, int line);
};
