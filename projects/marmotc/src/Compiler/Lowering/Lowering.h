#pragma once

#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"
#include "Compiler/BytecodeModule/BytecodeModule.h"
#include "Compiler/Lowering/GenericFunctionTable.h"
#include "Compiler/Lowering/GenericTypes.h"
#include "Compiler/Lowering/InstanceResolver.h"
#include "Compiler/MarmotIR/MarmotIR.h"
#include "Compiler/Result/Result.h"

#include <deque>
#include <expected>
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

	// What a function's body is lowered against. A specialization substitutes
	// its type arguments, resolves the class methods its constraints provide,
	// and, when another module wrote it, reads that module's names. A lambda
	// shares its enclosing function's.
	struct Specialization
	{
		GenericTypes::TypeEnvironment m_types;
		MethodResolutionMap m_methods;
		std::optional<std::string> m_source_module;

		Specialization(GenericTypes::TypeEnvironment types, MethodResolutionMap methods, std::optional<std::string> source_module);
	};

	class FunctionLowering;

	// A top-level definition: its global, and, for a function, the function
	// that is its value, which a call may name directly.
	struct TopLevelName
	{
		MarmotIRGlobalSlot m_slot;
		std::optional<MarmotIRFunctionId> m_function;
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
	std::unordered_map<std::string, MarmotIRFunctionId> m_specialized_functions;
	// The first specialization for each set of argument types, for a call whose
	// type the type checker left undecided.
	std::unordered_map<std::string, MarmotIRFunctionId> m_specializations_by_arguments;
	std::unordered_map<std::string, TopLevelName> m_top_level_names;
	std::unordered_map<std::string, MarmotIRGlobalSlot> m_import_slots;
	std::vector<LoweredImport> m_import_tokens;
	std::map<std::string, std::set<std::string>> m_native_imports;
	GenericFunctionTable m_generics;
	InstanceResolver m_resolver;

public:
	Lowering(MarmotProgramTree& program, std::string_view file_name, const std::vector<std::string>& source_lines, std::string module_name, const std::unordered_set<std::string>& export_symbols, const LoweringImports& imports);

	MarmotResult::DiagnosticsResult<LoweredModule> Lower() &&;

private:
	CompilerError Unsupported(std::string_view construct, const Token& token) const;
	CompilerError Error(CompilerErrorCode code, std::string_view message, const Token& token) const;
	CompilerError ResolutionError(const MethodResolutionError& error, const Token& token) const;

	MarmotIRFunctionId NewFunction(std::string name, TypeRef return_type, const Specialization& specialization);
	TypeRef Substitute(const TypeRef& type, const GenericTypes::TypeEnvironment& types) const;

	std::vector<CompilerError> ReserveTopLevelNames();
	Emitted ReserveTopLevelName(MarmotStatement& statement);
	Emitted ReserveFunction(const std::string& name, const TypeRef& closure_type);
	Emitted ReserveForeign(const MarmotStatement::ForeignDefinition& foreign);
	// A foreign function's value is the name the VM looks it up by.
	std::expected<std::string, CompilerError> ForeignName(const MarmotStatement::ForeignDefinition& foreign);
	void RegisterGeneric(const std::string& name, const FunctionParameters& params, const std::vector<TypeRef>& param_types, const std::vector<Token>& generic_params, const std::vector<MarmotType::ClassConstraint>& constraints, const TypeRef& return_type, std::unique_ptr<MarmotExpression>& body);
	bool IsGenericMethod(const MarmotStatement::Instance& instance, const MarmotStatement::FunctionDefinition& method) const;
	std::vector<LoweredExport> CollectExports() const;
};
