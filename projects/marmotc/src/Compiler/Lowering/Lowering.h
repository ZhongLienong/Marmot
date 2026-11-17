#pragma once

#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"
#include "Compiler/BytecodeModule/BytecodeModule.h"
#include "Compiler/Lowering/GenericFunctionTable.h"
#include "Compiler/Lowering/GenericTypes.h"
#include "Compiler/Lowering/InstanceResolver.h"
#include "Compiler/MarmotIR/MarmotIR.h"
#include "Compiler/MarmotIR/Builder/MarmotIRBuilder.h"
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
	std::optional<MarmotIRGlobalSlot> m_slot;
	std::optional<MarmotIRFunctionId> m_function;
	Token m_token;

	LoweredExport(std::string name, BytecodeModule::SymbolType kind, std::optional<MarmotIRGlobalSlot> slot, std::optional<MarmotIRFunctionId> function, Token token);
};

// Where an imported global is first named, for the linker's diagnostics.
struct LoweredImport
{
	MarmotIRGlobalSlot m_slot;
	Token m_token;

	LoweredImport(MarmotIRGlobalSlot slot, Token token);
};

// What the backend needs: the module's MarmotIR, and what the IR does not
// carry: the names it exports and imports, the generics a module importing it
// specializes, and the native libraries its foreign functions come from.
struct LoweredModule
{
	MarmotIRModule m_module;
	std::vector<LoweredExport> m_exports;
	std::vector<LoweredImport> m_imports;
	GenericFunctionTable::Functions m_generic_functions;
	std::map<std::string, std::set<std::string>> m_native_imports;

	explicit LoweredModule(MarmotIRModule module);
};

// What lowering must know about the modules this one imports.
struct LoweringImports
{
	GenericFunctionTable::Functions m_generic_functions;
	TypeclassMetadataMap m_typeclasses;
};

// The typed AST of one module to MarmotIR. Lowering is where each construct
// becomes blocks and instructions, where generics are specialized and where
// class methods resolve to instances; nothing downstream sees the AST. It
// changes the AST only to take the bodies of this module's generics, and never
// changes a generic's body: every module that imports it specializes it too.
// docs/marmot-ir.md describes what each construct becomes.
class Lowering
{
private:
	using TypeRef = std::shared_ptr<MarmotType>;
	using Lowered = std::expected<MarmotIRValueId, CompilerError>;
	using Emitted = std::expected<void, CompilerError>;
	using LoweredValues = std::expected<std::vector<MarmotIRValueId>, CompilerError>;
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

	// A source binding in one lowered specialization of its declaring function.
	struct LocalKey
	{
		const FunctionScope* m_owner;
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

	// A function being built. Lowering a function nests inside lowering
	// another: a lambda's body inside its enclosing function's, a
	// specialization's wherever it is first called.
	struct FunctionScope
	{
		MarmotIRFunctionId m_id;
		MarmotIRFunction& m_function;
		MarmotIRBuilder m_builder;
		const Specialization& m_specialization;
		// The function a lambda is written in; a Cell name reaches its locals.
		FunctionScope* m_parent;
		LexicalFunctionId m_identity;
		std::unordered_map<BindingId, MarmotIRValueId> m_locals;
		std::vector<LocalKey> m_captures;
		std::vector<MarmotIRValueId> m_capture_values;
		std::vector<PendingCapture> m_pending_captures;
		// A block is live once a live block jumps to it; the entry is. Code
		// after a call that returns Never goes to a block nothing reaches.
		std::vector<bool> m_live_blocks;
		// A generic lambda bound by a local 'def'. It has no
		// value of its own: each use makes the closure for its types.
		std::unordered_map<BindingId, const MarmotExpression::Function*> m_local_generics;

		FunctionScope(MarmotIRFunctionId id, MarmotIRFunction& function, const Specialization& specialization, FunctionScope* parent, LexicalFunctionId identity);
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

	// A top-level definition: its global, and, for a function, the function
	// that is its value, which a call may name directly.
	struct TopLevelName
	{
		MarmotIRGlobalSlot m_slot;
		std::optional<MarmotIRFunctionId> m_function;
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

	MarmotProgramTree& m_program;
	std::string m_file_name;
	const std::vector<std::string>& m_source_lines;
	std::string m_module_name;
	const std::unordered_set<std::string>& m_export_symbols;

	MarmotIRModule m_module;
	// Functions are built here, where a reference to one survives adding
	// another, and moved into the module at the end.
	std::deque<MarmotIRFunction> m_functions;
	std::deque<Specialization> m_specializations;
	std::vector<ActiveGenericLambda> m_active_generic_lambdas;
	std::unordered_map<std::string, MarmotIRFunctionId> m_specialized_functions;
	// The first specialization for each set of argument types, for a call whose
	// type the type checker left undecided.
	std::unordered_map<std::string, MarmotIRFunctionId> m_specializations_by_arguments;
	std::unordered_map<std::string, TopLevelName> m_top_level_names;
	std::unordered_map<std::string, MarmotIRGlobalSlot> m_import_slots;
	std::vector<LoweredImport> m_import_tokens;
	// This module's builtin foreign functions, by the name it declares them
	// as. Shared with the generics it exports, whose bodies may call them.
	std::shared_ptr<ForeignIndices> m_foreign_indices;
	std::map<std::string, std::set<std::string>> m_native_imports;
	GenericFunctionTable m_generics;
	InstanceResolver m_resolver;
	std::vector<FunctionScope*> m_scopes;

public:
	Lowering(MarmotProgramTree& program, std::string_view file_name, const std::vector<std::string>& source_lines, std::string module_name, const std::unordered_set<std::string>& export_symbols, const LoweringImports& imports);

	MarmotResult::DiagnosticsResult<LoweredModule> Lower() &&;

private:
	// Lowering.cpp: the module, functions, names and calls.
	CompilerError Unsupported(std::string_view construct, const Token& token) const;
	CompilerError Error(CompilerErrorCode code, std::string_view message, const Token& token) const;
	CompilerError ResolutionError(const MethodResolutionError& error, const Token& token) const;

	FunctionScope& Scope();
	MarmotIRBuilder& Builder();
	MarmotIRFunctionId NewFunction(std::string name, TypeRef return_type, const Specialization& specialization);
	TypeRef Concrete(const TypeRef& type);
	TypeRef Substitute(const TypeRef& type, const GenericTypes::TypeEnvironment& types) const;
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

	std::vector<CompilerError> ReserveTopLevelNames();
	Emitted ReserveTopLevelName(MarmotStatement& statement);
	Emitted ReserveFunction(const std::string& name, const TypeRef& closure_type);
	Emitted ReserveForeign(const MarmotStatement::ForeignDefinition& foreign);
	// A foreign function's value is the name the VM looks it up by.
	std::expected<std::string, CompilerError> ForeignName(const MarmotStatement::ForeignDefinition& foreign);
	void RegisterGeneric(const std::string& name, const FunctionParameters& params, const std::vector<TypeRef>& param_types, const std::vector<Token>& generic_params, const std::vector<MarmotType::ClassConstraint>& constraints, const TypeRef& return_type, std::unique_ptr<MarmotExpression>& body);
	bool IsGenericMethod(const MarmotStatement::Instance& instance, const MarmotStatement::FunctionDefinition& method) const;
	std::vector<LoweredExport> CollectExports() const;

	Emitted LowerTopLevelStatement(const MarmotStatement& statement);
	Emitted LowerTopLevelFunction(const std::string& name, const FunctionParameters& params, const MarmotExpression& body, int line);
	Emitted LowerStatement(const MarmotStatement& statement);
	Emitted LowerLocalDefinition(const MarmotStatement::VariableDefinition& definition);
	Emitted LowerTupleDefinition(const MarmotStatement::TupleDefinition& definition);

	void DefineLocal(const BindingId& binding, MarmotIRValueId value);
	LocalKey KeyForBinding(const FunctionScope& scope, const BindingId& binding) const;
	MarmotIRValueId CaptureFor(FunctionScope& scope, const LocalKey& key, const TypeRef& type);

	Emitted LowerFunctionBody(FunctionScope& scope, const std::vector<TypeRef>& param_types, const FunctionParameters& params, const MarmotExpression& body);
	Lowered MakeClosure(FunctionScope& child, const TypeRef& closure_type, int line, std::string name = {});
	Lowered LowerFunction(const MarmotExpression::Function& function);
	Lowered LowerGenericLambda(const MarmotExpression::Function& function, const TypeRef& type, std::string name, int line);
	Lowered LowerLocalGeneric(const MarmotExpression::Function& function, const TypeRef& type, const Token& name);
	std::expected<MarmotIRFunctionId, CompilerError> Specialize(const std::string& key, const std::vector<TypeRef>& argument_types, const TypeRef& call_type);

	Lowered LowerName(const MarmotExpression::NameAccess& name);
	bool IsForeignFunction(const TypeRef& type) const;
	bool IsForeignDeclaration(const std::string& name);
	Lowered LowerForeignValue(const Token& name, const TypeRef& type);
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
	std::optional<MarmotIRImmediate> ConstantOf(MarmotLiteralKind kind, const std::string& lexeme) const;
	Lowered LowerLiteral(const MarmotExpression::Literal& literal);
	Lowered LowerBinary(const MarmotExpression::Binary& binary);
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
