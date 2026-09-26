#pragma once

#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"
#include "Compiler/BytecodeModule/BytecodeModule.h"
#include "Compiler/MidoriIR/MidoriIR.h"
#include "Compiler/MidoriIR/MidoriIRBuilder.h"
#include "Compiler/Result/Result.h"

#include <deque>
#include <expected>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// A name this module exports, and the token that declared it.
struct LoweredExport
{
	std::string m_name;
	BytecodeModule::SymbolType m_kind;
	std::optional<MidoriIRGlobalSlot> m_slot;
	Token m_token;

	LoweredExport(std::string name, BytecodeModule::SymbolType kind, std::optional<MidoriIRGlobalSlot> slot, Token token);
};

// Where an imported global is first named, for the linker's diagnostics.
struct LoweredImport
{
	MidoriIRGlobalSlot m_slot;
	Token m_token;

	LoweredImport(MidoriIRGlobalSlot slot, Token token);
};

// What the backend needs: the module's MidoriIR, and the names it exports and
// imports, which the IR does not carry.
struct LoweredModule
{
	MidoriIRModule m_module;
	std::vector<LoweredExport> m_exports;
	std::vector<LoweredImport> m_imports;

	explicit LoweredModule(MidoriIRModule module);
};

// What lowering must know about the modules this one imports.
struct LoweringImports
{
	// `Module::name` for every generic function an import provides.
	std::unordered_set<std::string> m_generic_functions;
	// Each imported class, with its methods.
	std::unordered_map<std::string, std::unordered_set<std::string>> m_class_methods;
};

// The typed AST of one module to MidoriIR. Lowering is where each construct
// becomes blocks and instructions; nothing downstream sees the AST. A
// construct it does not lower yet stops the module with
// CodeGeneratorUnsupportedLowering; docs/midori-ir.md lists what it lowers.
class Lowering
{
private:
	using Lowered = std::expected<MidoriIRValueId, CompilerError>;
	using Emitted = std::expected<void, CompilerError>;

	// A function being built. Lowering a function nests inside lowering
	// another, so each keeps its own builder and locals.
	struct FunctionScope
	{
		MidoriIRFunctionId m_id;
		MidoriIRBuilder m_builder;
		// The parser's local index to the value it names now.
		std::unordered_map<int, MidoriIRValueId> m_locals;

		FunctionScope(MidoriIRFunctionId id, MidoriIRFunction& function);
	};

	// A top-level definition: its global, and, for `def f = fn ...`, the
	// function that is its value, which a call may name directly.
	struct TopLevelName
	{
		MidoriIRGlobalSlot m_slot;
		std::optional<MidoriIRFunctionId> m_function;
	};

	MidoriProgramTree& m_program;
	std::string m_file_name;
	const std::vector<std::string>& m_source_lines;
	std::string m_module_name;
	const std::unordered_set<std::string>& m_export_symbols;
	const LoweringImports& m_imports;

	MidoriIRModule m_module;
	// Functions are built here, where a reference to one survives adding
	// another, and moved into the module at the end.
	std::deque<MidoriIRFunction> m_functions;
	std::unordered_map<std::string, TopLevelName> m_top_level_names;
	std::unordered_map<std::string, MidoriIRGlobalSlot> m_import_slots;
	std::vector<LoweredImport> m_import_tokens;
	// A builtin foreign function, by the name this module declares it as.
	std::unordered_map<std::string, std::string> m_builtin_foreigns;
	std::vector<FunctionScope*> m_scopes;

public:
	Lowering(MidoriProgramTree& program, std::string_view file_name, const std::vector<std::string>& source_lines, std::string module_name, const std::unordered_set<std::string>& export_symbols, const LoweringImports& imports);

	MidoriResult::DiagnosticsResult<LoweredModule> Lower() &&;

private:
	CompilerError Unsupported(std::string_view construct, const Token& token) const;

	FunctionScope& Scope();
	MidoriIRBuilder& Builder();
	MidoriIRFunctionId NewFunction(std::string name, std::shared_ptr<MidoriType> return_type);

	Emitted ReserveTopLevelNames();
	Emitted ReserveTopLevelName(MidoriStatement& statement);
	Emitted ReserveForeign(const MidoriStatement::ForeignDefinition& foreign);
	std::vector<LoweredExport> CollectExports() const;

	Emitted LowerTopLevelStatement(MidoriStatement& statement);
	Emitted LowerStatement(MidoriStatement& statement);
	Emitted LowerLocalDefinition(MidoriStatement::VariableDefinition& definition);
	Emitted LowerFunctionBody(MidoriIRFunctionId id, const std::vector<std::shared_ptr<MidoriType>>& param_types, const std::vector<Token>& params, MidoriExpression& body);

	Lowered Lower(MidoriExpression& expression);
	// Lowers an expression whose value the function returns, so that a call
	// there becomes a tail call; the block ends with a terminator.
	Emitted LowerReturn(MidoriExpression& expression);

	Lowered LowerLiteral(const MidoriExpression::Literal& literal);
	Lowered LowerBinary(MidoriExpression::Binary& binary);
	Lowered LowerShortCircuit(MidoriExpression::Binary& binary);
	Lowered LowerUnary(MidoriExpression::UnaryPrefix& unary);
	Lowered LowerAs(MidoriExpression::As& as);
	Lowered LowerBoolToText(MidoriIRValueId value, int line);
	Lowered LowerName(const MidoriExpression::NameAccess& name);
	Lowered LowerIf(MidoriExpression::IfElse& if_else);
	Emitted LowerJoinedBranch(MidoriIRBlockId block, MidoriExpression* branch, MidoriIRBlockId join);
	Lowered LowerBlock(MidoriExpression::Block& block);
	Lowered LowerFunction(MidoriExpression::Function& function);
	Lowered LowerCall(MidoriExpression::Call& call);
	Emitted LowerTailCall(MidoriExpression::Call& call);

	std::expected<std::vector<MidoriIRValueId>, CompilerError> LowerArguments(std::vector<std::unique_ptr<MidoriExpression>>& arguments);
	// The global a name reads: this module's, or an import, reserved the
	// first time it is named. A generic function or a class method is not one.
	std::expected<MidoriIRGlobalSlot, CompilerError> GlobalFor(const Token& name, const std::shared_ptr<MidoriType>& type);
	std::optional<MidoriIRFunctionId> DirectCallee(const MidoriExpression& callee) const;
	std::optional<std::string> BuiltinForeignCallee(const MidoriExpression::Call& call) const;
};
