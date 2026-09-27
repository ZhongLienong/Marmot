#pragma once

#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"
#include "Compiler/BytecodeModule/BytecodeModule.h"
#include "Compiler/Lowering/GenericFunctionTable.h"
#include "Compiler/Lowering/GenericTypes.h"
#include "Compiler/Lowering/InstanceResolver.h"
#include "Compiler/MidoriIR/MidoriIR.h"
#include "Compiler/MidoriIR/MidoriIRBuilder.h"
#include "Compiler/Result/Result.h"

#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// A name this module exports, and the token that declared it. A function
// export also names the function that is its procedure.
struct LoweredExport
{
	std::string m_name;
	BytecodeModule::SymbolType m_kind;
	std::optional<MidoriIRGlobalSlot> m_slot;
	std::optional<MidoriIRFunctionId> m_function;
	Token m_token;

	LoweredExport(std::string name, BytecodeModule::SymbolType kind, std::optional<MidoriIRGlobalSlot> slot, std::optional<MidoriIRFunctionId> function, Token token);
};

// Where an imported global is first named, for the linker's diagnostics.
struct LoweredImport
{
	MidoriIRGlobalSlot m_slot;
	Token m_token;

	LoweredImport(MidoriIRGlobalSlot slot, Token token);
};

// What the backend needs: the module's MidoriIR, and what the IR does not
// carry: the names it exports and imports, the generics a module importing it
// specializes, and the native libraries its foreign functions come from.
struct LoweredModule
{
	MidoriIRModule m_module;
	std::vector<LoweredExport> m_exports;
	std::vector<LoweredImport> m_imports;
	GenericFunctionTable::Functions m_generic_functions;
	std::map<std::string, std::set<std::string>> m_native_imports;

	explicit LoweredModule(MidoriIRModule module);
};

// What lowering must know about the modules this one imports.
struct LoweringImports
{
	GenericFunctionTable::Functions m_generic_functions;
	InstanceResolver::ClassMethods m_class_methods;
	InstanceResolver::ClassInstances m_class_instances;
	InstanceResolver::ClassInstanceTypes m_class_instance_type_args;
	InstanceResolver::ClassInstanceAssociatedTypes m_class_instance_associated_types;
};

// The typed AST of one module to MidoriIR. Lowering is where each construct
// becomes blocks and instructions, where generics are specialized and where
// class methods resolve to instances; nothing downstream sees the AST. It
// changes the AST only to take the bodies of this module's generics, and never
// changes a generic's body: every module that imports it specializes it too.
// docs/midori-ir.md describes what each construct becomes.
class Lowering
{
private:
	using TypeRef = std::shared_ptr<MidoriType>;
	using Lowered = std::expected<MidoriIRValueId, CompilerError>;
	using Emitted = std::expected<void, CompilerError>;
	using LoweredValues = std::expected<std::vector<MidoriIRValueId>, CompilerError>;
	using ForeignIndices = std::unordered_map<std::string, size_t>;

	// What a function's body is lowered against. A specialization substitutes
	// its type arguments, resolves the class methods its constraints provide,
	// and, when another module wrote it, reads that module's names. A lambda
	// shares its enclosing function's.
	struct Specialization
	{
		GenericTypes::TypeEnvironment m_types;
		MethodResolutionMap m_methods;
		std::optional<std::string> m_source_module;
		std::shared_ptr<const ForeignIndices> m_foreign_indices;

		Specialization(GenericTypes::TypeEnvironment types, MethodResolutionMap methods, std::optional<std::string> source_module, std::shared_ptr<const ForeignIndices> foreign_indices);
	};

	struct FunctionScope;

	// A local of the function being lowered in `m_owner`, by the parser's index.
	struct LocalKey
	{
		const FunctionScope* m_owner;
		int m_index;

		bool operator==(const LocalKey&) const = default;
	};

	// A closure made before a local it captures is defined, as a local
	// function that names itself or one defined after it is. BindCaptures
	// fills the capture once the local is defined.
	struct PendingCapture
	{
		MidoriIRValueId m_closure;
		uint32_t m_capture;
		int m_local;
	};

	// A function being built. Lowering a function nests inside lowering
	// another: a lambda's body inside its enclosing function's, a
	// specialization's wherever it is first called.
	struct FunctionScope
	{
		MidoriIRFunctionId m_id;
		MidoriIRFunction& m_function;
		MidoriIRBuilder m_builder;
		const Specialization& m_specialization;
		// The function a lambda is written in; a Cell name reaches its locals.
		FunctionScope* m_parent;
		// How many enclosing locals the parser numbered before this function's
		// own, its m_captured_count. A Cell index below it names one of the
		// parent's own captures; past it, one of the parent's locals.
		int m_environment_length;
		std::unordered_map<int, MidoriIRValueId> m_locals;
		std::vector<std::vector<int>> m_local_frames;
		std::vector<LocalKey> m_captures;
		std::vector<MidoriIRValueId> m_capture_values;
		std::vector<PendingCapture> m_pending_captures;
		// A block is live once a live block jumps to it; the entry is. Code
		// after a call that returns Never goes to a block nothing reaches.
		std::vector<bool> m_live_blocks;

		FunctionScope(MidoriIRFunctionId id, MidoriIRFunction& function, const Specialization& specialization, FunctionScope* parent, int environment_length);
	};

	// A top-level definition: its global, and, for a function, the function
	// that is its value, which a call may name directly.
	struct TopLevelName
	{
		MidoriIRGlobalSlot m_slot;
		std::optional<MidoriIRFunctionId> m_function;
	};

	// What a call runs.
	struct Callee
	{
		MidoriIRImmediate m_target;
		std::optional<MidoriIRValueId> m_value;
	};

	// Where the value of an `if` branch or a match arm goes: a join block's
	// parameter, or, in tail position, the function's return.
	using BranchSink = std::function<Emitted(MidoriExpression*)>;

	struct JoinPoint
	{
		MidoriIRBlockId m_block;
		TypeRef m_type;

		JoinPoint(MidoriIRBlockId block, TypeRef type);
	};

	MidoriProgramTree& m_program;
	std::string m_file_name;
	const std::vector<std::string>& m_source_lines;
	std::string m_module_name;
	const std::unordered_set<std::string>& m_export_symbols;

	MidoriIRModule m_module;
	// Functions are built here, where a reference to one survives adding
	// another, and moved into the module at the end.
	std::deque<MidoriIRFunction> m_functions;
	std::deque<Specialization> m_specializations;
	std::unordered_map<std::string, MidoriIRFunctionId> m_specialized_functions;
	// The first specialization for each set of argument types, for a call whose
	// type the type checker left undecided.
	std::unordered_map<std::string, MidoriIRFunctionId> m_specializations_by_arguments;
	std::unordered_map<std::string, TopLevelName> m_top_level_names;
	std::unordered_map<std::string, MidoriIRGlobalSlot> m_import_slots;
	std::vector<LoweredImport> m_import_tokens;
	// This module's builtin foreign functions, by the name it declares them
	// as. Shared with the generics it exports, whose bodies may call them.
	std::shared_ptr<ForeignIndices> m_foreign_indices;
	std::map<std::string, std::set<std::string>> m_native_imports;
	GenericFunctionTable m_generics;
	InstanceResolver m_resolver;
	std::vector<FunctionScope*> m_scopes;

public:
	Lowering(MidoriProgramTree& program, std::string_view file_name, const std::vector<std::string>& source_lines, std::string module_name, const std::unordered_set<std::string>& export_symbols, const LoweringImports& imports);

	MidoriResult::DiagnosticsResult<LoweredModule> Lower() &&;

private:
	// Lowering.cpp: the module, functions, names and calls.
	CompilerError Unsupported(std::string_view construct, const Token& token) const;
	CompilerError Error(CompilerErrorCode code, std::string_view message, const Token& token) const;
	CompilerError ResolutionError(const MethodResolutionError& error, const Token& token) const;

	FunctionScope& Scope();
	MidoriIRBuilder& Builder();
	MidoriIRFunctionId NewFunction(std::string name, TypeRef return_type, const Specialization& specialization);
	TypeRef Concrete(const TypeRef& type);
	TypeRef Substitute(const TypeRef& type, const GenericTypes::TypeEnvironment& types) const;
	TypeRef TypeOf(const MidoriExpression& expression);
	// The type an instance is selected by: the expression's own, which a
	// conversion that erases a newtype keeps nominal, unless the type checker
	// left it undecided.
	TypeRef NominalType(const MidoriExpression& expression, MidoriIRValueId value);

	MidoriIRBlockId NewBlock();
	bool IsDead();
	void PositionAt(MidoriIRBlockId block);
	void Jump(MidoriIRBlockId target, std::vector<MidoriIRValueId> arguments = {});
	void Branch(MidoriIRValueId condition, MidoriIRSuccessor if_true, MidoriIRSuccessor if_false);
	// A value of any type, standing in for one that code nothing runs would
	// have computed.
	MidoriIRValueId Placeholder(const TypeRef& type);
	// Ends the block after a call that returns Never, and goes on in one
	// nothing reaches.
	void AfterNever();

	Emitted ReserveTopLevelNames();
	Emitted ReserveTopLevelName(MidoriStatement& statement);
	Emitted ReserveFunction(const std::string& name, const TypeRef& closure_type);
	Emitted ReserveForeign(const MidoriStatement::ForeignDefinition& foreign);
	// A foreign function's value is the name the VM looks it up by.
	std::expected<std::string, CompilerError> ForeignName(const MidoriStatement::ForeignDefinition& foreign);
	void RegisterGeneric(const std::string& name, const std::vector<Token>& params, const std::vector<TypeRef>& param_types, const std::vector<Token>& generic_params, const std::vector<MidoriType::ClassConstraint>& constraints, const TypeRef& return_type, std::unique_ptr<MidoriExpression>& body, int captured_count);
	bool IsGenericMethod(const MidoriStatement::Instance& instance, const MidoriStatement::FunctionDefinition& method) const;
	std::vector<LoweredExport> CollectExports() const;

	Emitted LowerTopLevelStatement(MidoriStatement& statement);
	Emitted LowerTopLevelFunction(const std::string& name, const std::vector<Token>& params, MidoriExpression& body, int line);
	Emitted LowerStatement(MidoriStatement& statement);
	Emitted LowerLocalDefinition(MidoriStatement::VariableDefinition& definition);
	Emitted LowerTupleDefinition(MidoriStatement::TupleDefinition& definition);

	void PushLocalFrame();
	void PopLocalFrame();
	void DefineLocal(int index, MidoriIRValueId value);
	LocalKey KeyForCell(const FunctionScope& scope, int cell_index) const;
	MidoriIRValueId CaptureFor(FunctionScope& scope, const LocalKey& key, const TypeRef& type);

	Emitted LowerFunctionBody(FunctionScope& scope, const std::vector<TypeRef>& param_types, const std::vector<Token>& params, MidoriExpression& body);
	Lowered MakeClosure(FunctionScope& child, const TypeRef& closure_type, int line, std::string name = {});
	Lowered LowerFunction(MidoriExpression::Function& function);
	std::expected<MidoriIRFunctionId, CompilerError> Specialize(const std::string& key, const std::vector<TypeRef>& argument_types, const TypeRef& call_type);

	Lowered LowerName(const MidoriExpression::NameAccess& name);
	bool IsForeignFunction(const TypeRef& type) const;
	bool IsForeignDeclaration(const std::string& name);
	Lowered LowerForeignValue(const Token& name, const TypeRef& type);
	std::expected<MidoriIRGlobalSlot, CompilerError> GlobalFor(const Token& name, const TypeRef& type);
	std::expected<MidoriIRGlobalSlot, CompilerError> GlobalForResolved(const std::string& resolved_name, const TypeRef& type, const Token& at);
	MidoriIRGlobalSlot ImportGlobal(const std::string& module, const std::string& symbol, const TypeRef& type, const Token& at);
	Lowered LoadResolved(const std::string& resolved_name, const TypeRef& type, const Token& at);
	// A call of a class method an instance provides, by its resolved name.
	Lowered CallResolved(const std::string& resolved_name, std::vector<MidoriIRValueId> arguments, const std::vector<TypeRef>& argument_types, const TypeRef& result_type, const Token& at);

	Lowered LowerCall(MidoriExpression::Call& call);
	Emitted LowerTailCall(MidoriExpression::Call& call);
	std::optional<std::string> IntrinsicName(const MidoriExpression::Call& call) const;
	Lowered LowerIntrinsic(const std::string& name, MidoriExpression::Call& call);
	std::expected<Callee, CompilerError> ResolveCallee(MidoriExpression::Call& call, const std::vector<MidoriIRValueId>& arguments, const TypeRef& call_type);
	TypeRef ResultTypeOf(const Callee& callee, const TypeRef& call_type);
	MidoriIRValueId EmitCall(const Callee& callee, std::vector<MidoriIRValueId> arguments, const TypeRef& call_type, int line);
	LoweredValues LowerArguments(std::vector<std::unique_ptr<MidoriExpression>>& arguments);
	std::vector<TypeRef> TypesOf(const std::vector<MidoriIRValueId>& values);

	// LoweringExpressions.cpp: values.
	Lowered Lower(MidoriExpression& expression);
	std::optional<MidoriIRImmediate> ConstantOf(MidoriLiteralKind kind, const std::string& lexeme) const;
	Lowered LowerLiteral(const MidoriExpression::Literal& literal);
	Lowered LowerBinary(MidoriExpression::Binary& binary);
	Lowered LowerShortCircuit(MidoriExpression::Binary& binary);
	Lowered LowerClassOperator(MidoriExpression::Binary& binary, MidoriIRValueId left, MidoriIRValueId right);
	Lowered LowerConcat(MidoriExpression::Binary& binary, MidoriIRValueId left, MidoriIRValueId right);
	Lowered LowerUnary(MidoriExpression::UnaryPrefix& unary);
	Lowered LowerCount(MidoriExpression::UnaryPrefix& unary, MidoriIRValueId operand);
	Lowered LowerAs(MidoriExpression::As& as);
	Lowered LowerBoolToText(MidoriIRValueId value, int line);
	Lowered LowerTuple(MidoriExpression::Tuple& tuple);
	Lowered LowerArray(MidoriExpression::Array& array);
	Lowered LowerIndex(MidoriExpression::IndexAccess& index);
	Lowered LowerConstruct(MidoriExpression::Construct& construct);
	Lowered LowerRecordUpdate(MidoriExpression::RecordUpdate& record_update);
	Lowered LowerMemberAccess(MidoriExpression::MemberAccess& member);
	Lowered LowerRange(MidoriExpression& start, MidoriExpression* step, MidoriExpression& end, const TypeRef& range_type, int line);
	Lowered LowerSpawn(MidoriExpression::Spawn& spawn);
	Lowered LowerJoin(MidoriExpression::Join& join);
	Lowered LowerOperation(MidoriIROp op, const TypeRef& type, std::vector<MidoriExpression*> operands, MidoriIRImmediate immediate, int line);

	// LoweringControl.cpp: control flow.
	Emitted LowerReturn(MidoriExpression& expression);
	Emitted LowerIfWith(MidoriExpression::IfElse& if_else, const BranchSink& sink);
	Lowered LowerIf(MidoriExpression::IfElse& if_else);
	Emitted LowerStatements(MidoriExpression::Block& block);
	Lowered LowerBlock(MidoriExpression::Block& block);
	Emitted LowerMatchWith(MidoriExpression::Match& match, const BranchSink& sink);
	Lowered LowerMatch(MidoriExpression::Match& match);
	Emitted LowerPattern(const MidoriPattern& pattern, MidoriIRValueId value, MidoriIRBlockId fail);
	Emitted TestOrFail(MidoriIRValueId condition, MidoriIRBlockId fail);
	// A join block whose parameter is the value of the construct that jumps
	// to it, and the sink each branch lowers into. EnterJoin gives the
	// parameter its type once every branch has jumped.
	std::pair<std::shared_ptr<JoinPoint>, BranchSink> Join(const TypeRef& type);
	MidoriIRValueId EnterJoin(const JoinPoint& join);

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
	using LoopBody = std::function<std::expected<std::vector<MidoriIRValueId>, CompilerError>(std::vector<MidoriIRValueId>)>;
	std::expected<std::vector<MidoriIRValueId>, CompilerError> LowerLoop(MidoriExpression& iterated, const LoopKind& kind, int loop_variable, std::vector<MidoriIRValueId> carried, const LoopBody& body, const Token& at);
	LoopKind LoopKindOf(bool is_array, bool is_iterable, const TypeRef& item_type, const TypeRef& next_type, int some_tag);
	Lowered LowerFor(MidoriExpression::For& for_expression);
	Lowered LowerComprehension(MidoriExpression::ArrayComprehension& comprehension);
	Lowered CallIterableNext(MidoriIRValueId iterator, const TypeRef& item_type, const TypeRef& next_type, const Token& at);
};
