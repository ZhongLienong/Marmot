#include "BytecodeLinker.h"
#include "Common/Constant/Constant.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <numeric>
#include <ranges>
#include <unordered_set>

namespace
{
	std::string EntrySourcePathForModule(const std::vector<BytecodeModule>& modules, std::string_view entry_module_name)
	{
		for (const BytecodeModule& module : modules)
		{
			if (module.m_module_name == entry_module_name)
			{
				return module.m_source_path.string();
			}
		}

		for (const BytecodeModule& module : modules)
		{
			if (module.m_source_path.stem() == entry_module_name)
			{
				return module.m_source_path.string();
			}
		}

		return std::string(entry_module_name);
	}

	struct InstanceGlobalInit
	{
		size_t m_proc_index;
		size_t m_global_index;
	};

	int ReadWideOperand(const BytecodeStream& procedure, int offset)
	{
		const int high_byte = static_cast<int>(procedure.ReadByteCode(offset + 1));
		const int low_byte = static_cast<int>(procedure.ReadByteCode(offset + 2));
		return (high_byte << SHIFT_8_BITS) | low_byte;
	}

	void WriteWideOperand(BytecodeStream& procedure, int offset, int value)
	{
		const int high_byte = (value >> SHIFT_8_BITS) & BYTE_MASK;
		const int low_byte = value & BYTE_MASK;
		procedure.SetByteCode(offset + 1, static_cast<OpCode>(high_byte));
		procedure.SetByteCode(offset + 2, static_cast<OpCode>(low_byte));
	}

	int ReadShortOperandLE(const BytecodeStream& procedure, int offset)
	{
		const int low_byte = static_cast<int>(procedure.ReadByteCode(offset + 1));
		const int high_byte = static_cast<int>(procedure.ReadByteCode(offset + 2));
		return low_byte | (high_byte << SHIFT_8_BITS);
	}

	void WriteShortOperandLE(BytecodeStream& procedure, int offset, int value)
	{
		const int low_byte = value & BYTE_MASK;
		const int high_byte = (value >> SHIFT_8_BITS) & BYTE_MASK;
		procedure.SetByteCode(offset + 1, static_cast<OpCode>(low_byte));
		procedure.SetByteCode(offset + 2, static_cast<OpCode>(high_byte));
	}

	bool IsInstanceMethodName(std::string_view name)
	{
		const size_t first_underscore = name.find('_');
		if (first_underscore == std::string::npos || first_underscore + 1u >= name.size())
		{
			return false;
		}
		if (std::isupper(static_cast<unsigned char>(name[first_underscore + 1u])) == 0)
		{
			return false;
		}
		const size_t second_underscore = name.find('_', first_underscore + 1u);
		return second_underscore != std::string::npos;
	}

	void AddModuleInitializer(BytecodeStream& bootstrap, const BytecodeModule& module, const std::unordered_map<std::string, size_t>& module_base_procedure_indices)
	{
		const std::unordered_map<std::string, size_t>::const_iterator base_it =
			module_base_procedure_indices.find(module.m_module_name);

		const size_t module_base_index = base_it->second;
		const size_t module_global_index = module_base_index + 1u;

		bootstrap.AddByteCode(OpCode::MAKE_CLOSURE, 0);
		bootstrap.AddByteCode(static_cast<OpCode>(module_global_index), 0);
		bootstrap.AddByteCode(OpCode::BIND_CAPTURES, 0);
		bootstrap.AddByteCode(static_cast<OpCode>(0), 0);
		bootstrap.AddByteCode(OpCode::CALL, 0);
		bootstrap.AddByteCode(static_cast<OpCode>(0), 0);
		bootstrap.AddByteCode(OpCode::POP, 0);
	}

	void EmitInstanceGlobal(BytecodeStream& stream, size_t proc_idx, size_t global_index)
	{
		stream.AddByteCode(OpCode::MAKE_FUNCTION, 0);
		stream.AddByteCode(static_cast<OpCode>(proc_idx), 0);

		if (global_index <= MAX_LOCAL_VARIABLES)
		{
			stream.AddByteCode(OpCode::DEFINE_GLOBAL, 0);
			stream.AddByteCode(static_cast<OpCode>(global_index), 0);
			return;
		}

		stream.AddByteCode(OpCode::DEFINE_GLOBAL_WIDE, 0);
		const int high_byte = (static_cast<int>(global_index) >> SHIFT_8_BITS) & BYTE_MASK;
		const int low_byte = static_cast<int>(global_index) & BYTE_MASK;
		stream.AddByteCode(static_cast<OpCode>(high_byte), 0);
		stream.AddByteCode(static_cast<OpCode>(low_byte), 0);
	}

	std::unordered_map<std::string, std::vector<InstanceGlobalInit>> BuildInstanceGlobalInits(const std::vector<std::string>& procedure_names, const std::vector<std::string>& global_variables)
	{
		std::unordered_map<std::string, std::vector<InstanceGlobalInit>> instance_inits_by_module;
		std::unordered_set<size_t> initialized_globals;

		for (size_t proc_idx = 0u; proc_idx < procedure_names.size(); proc_idx += 1u)
		{
			const std::string& proc_name = procedure_names[proc_idx];
			const size_t at_pos = proc_name.find(ModuleSeparator);
			if (at_pos == std::string::npos)
			{
				continue;
			}

			const std::string base_name = proc_name.substr(0u, at_pos);
			if (base_name.starts_with(MAIN_PROCEDURE_PREFIX) || base_name.starts_with(MODULE_BOOTSTRAP_PREFIX))
			{
				continue;
			}
			if (!IsInstanceMethodName(base_name))
			{
				continue;
			}

			const std::vector<std::string>::const_iterator global_it = std::ranges::find_if
			(
				global_variables,
				[&base_name](const std::string& global_name)
				{
					return global_name == base_name;
				}
			);

			if (global_it == global_variables.end())
			{
				continue;
			}

			const size_t global_index = static_cast<size_t>(std::distance(global_variables.cbegin(), global_it));
			if (!initialized_globals.insert(global_index).second)
			{
				continue;
			}

			const std::string module_name = proc_name.substr(at_pos + 1u);
			instance_inits_by_module[module_name].push_back(InstanceGlobalInit{proc_idx, global_index});
		}

		return instance_inits_by_module;
	}

	std::string DiagnosticFileName(const BytecodeModule& module)
	{
		if (!module.m_source_path.empty())
		{
			return module.m_source_path.string();
		}

		return module.m_module_name;
	}

	std::string FormatModuleOrigin(const BytecodeModule& module, const std::optional<BytecodeModule::SourceProvenance>& source_provenance)
	{
		const std::string file_name = DiagnosticFileName(module);
		if (!file_name.empty())
		{
			if (source_provenance.has_value() && source_provenance->m_line > 0)
			{
				return std::format("{}:{}", file_name, source_provenance->m_line);
			}

			return file_name;
		}

		if (!module.m_module_name.empty())
		{
			return std::format("module {}", module.m_module_name);
		}

		return "<unknown module>";
	}

	std::optional<std::string_view> ToOptionalStringView(const std::optional<std::string>& source_line)
	{
		if (!source_line.has_value())
		{
			return std::nullopt;
		}

		return std::string_view(source_line.value());
	}

	std::optional<CompilerError> MakeContextualLinkerError(CompilerErrorCode code, std::string_view message, const BytecodeModule& module, const std::optional<BytecodeModule::SourceProvenance>& source_provenance)
	{
		if (!source_provenance.has_value() || source_provenance->m_line <= 0)
		{
			return std::nullopt;
		}

		return CompilerError::WithContext(
			CompilerStage::BytecodeLinker,
			message,
			source_provenance->m_line,
			DiagnosticFileName(module),
			source_provenance->m_column,
			source_provenance->m_caret_length,
			std::nullopt,
			ToOptionalStringView(source_provenance->m_source_line),
			code);
	}
}

BytecodeLinker::BytecodeLinker(std::vector<BytecodeModule>&& modules, std::string_view entry_module_name)
	: m_modules(std::move(modules)),
	m_entry_module_name(entry_module_name)
{
}

MidoriResult::BytecodeLinkerResult BytecodeLinker::Link()
{
	if (m_modules.empty())
	{
		return std::unexpected(CompilerError::Simple(CompilerStage::BytecodeLinker, "Cannot link: no modules were successfully compiled.\n\nPossible causes:\n  - All source files failed to compile (check for syntax/type errors above)\n  - Circular module dependencies detected\n  - Module resolution failed (check import paths)\n  - Empty build graph (no valid modules to compile)", CompilerErrorCode::BytecodeLinkerNoModulesToLink));
	}

	AssignModuleBaseOffsets();

	MidoriResult::VoidResult symbol_result = BuildGlobalSymbolTable();
	if (!symbol_result.has_value())
	{
		return std::unexpected(symbol_result.error());
	}

	MergeConstantPools();
	MergeFunctionNames();
	MergeGlobalVariables();
	for (const BytecodeModule& module : m_modules)
	{
		for (const auto& [file_name, source_lines] : module.m_source_files)
		{
			if (!file_name.empty() && !m_global_source_files.contains(file_name))
			{
				m_global_source_files.emplace(file_name, source_lines);
			}
		}
	}

	MidoriResult::VoidResult import_result = ResolveImportsAndPatch();
	if (!import_result.has_value())
	{
		return std::unexpected(import_result.error());
	}

	ConcatenateBytecode();

	if (!m_unresolved_imports.empty())
	{
		std::string message = "Unresolved import(s) during linking:";
		for (const std::string& unresolved : m_unresolved_imports)
		{
			message.push_back(static_cast<char>(10));
			message += "  ";
			message += unresolved;
		}
		message.push_back(static_cast<char>(10));
		return std::unexpected(CompilerError::Simple(CompilerStage::BytecodeLinker, message, CompilerErrorCode::BytecodeLinkerUnresolvedImport));
	}


	m_global_procedures.insert(m_global_procedures.begin(), BytecodeStream());
	m_global_procedure_source_paths.insert(m_global_procedure_source_paths.begin(), EntrySourcePathForModule(m_modules, m_entry_module_name));

	// Create bootstrap name with entry module context for debugging
	std::string bootstrap_name = std::format("{}@{}", MODULE_BOOTSTRAP_PREFIX, m_entry_module_name);
	m_global_procedure_names.insert(m_global_procedure_names.begin(), std::move(bootstrap_name));

	PatchBootstrapOffsets();

	m_global_procedures[0u] = BuildBootstrapProcedure();

	MidoriExecutable executable;
	executable.AttachProcedures(std::move(m_global_procedures));
	executable.AttachProcedureNames(std::move(m_global_procedure_names));
	executable.AttachProcedureSourcePaths(std::move(m_global_procedure_source_paths));
	executable.AttachSourceFiles(std::move(m_global_source_files));
	executable.AddStringPool(std::move(m_global_string_pool));
	executable.SetFileName(EntrySourcePathForModule(m_modules, m_entry_module_name));

	std::ranges::for_each
	(
		m_global_variables,
		[&executable](std::string& global_var)
		{
			executable.AddGlobalVariable(std::move(global_var));
		}
	);

	return executable;
}

void BytecodeLinker::AssignModuleBaseOffsets()
{
	static_cast<void>
	(
		std::accumulate
		(
			m_modules.begin(),
			m_modules.end(),
			0uz,
			[this](size_t current_index, const BytecodeModule& module)
			{
				m_module_base_procedure_indices[module.m_module_name] = current_index;
				return current_index + module.m_procedures.size();
			}
		)
	);
}

MidoriResult::VoidResult BytecodeLinker::BuildGlobalSymbolTable()
{
	for (const BytecodeModule& module : m_modules)
	{
		const size_t module_base = m_module_base_procedure_indices.at(module.m_module_name);

		for (const BytecodeModule::ExportedSymbol& exp : module.m_exports)
		{
			const std::string symbol_key = MakeSymbolKey(module.m_module_name, exp.m_name);
			const size_t global_procedure_index = module_base + exp.m_procedure_index;
			const std::unordered_map<std::string, LinkedExport>::const_iterator existing_it = m_global_symbol_table.find(symbol_key);
			if (existing_it != m_global_symbol_table.end())
			{
				const LinkedExport& existing_export = existing_it->second;
				const std::string message = std::format(
					"Duplicate symbol export: {} from module {}. Previous export defined at {}.",
					exp.m_name,
					module.m_module_name,
					FormatModuleOrigin(*existing_export.m_module, existing_export.m_export->m_source_provenance));

				if (std::optional<CompilerError> contextual_error = MakeContextualLinkerError(CompilerErrorCode::BytecodeLinkerDuplicateExportedSymbol, message, module, exp.m_source_provenance))
				{
					return std::unexpected(std::move(*contextual_error));
				}
				if (std::optional<CompilerError> fallback_context = MakeContextualLinkerError(CompilerErrorCode::BytecodeLinkerDuplicateExportedSymbol, message, *existing_export.m_module, existing_export.m_export->m_source_provenance))
				{
					return std::unexpected(std::move(*fallback_context));
				}

				return std::unexpected(CompilerError::WithFile(CompilerStage::BytecodeLinker, message, DiagnosticFileName(module), CompilerErrorCode::BytecodeLinkerDuplicateExportedSymbol));
			}

			m_global_symbol_table.emplace(symbol_key, LinkedExport{ global_procedure_index, &module, &exp });
		}
	}

	return {};
}

void BytecodeLinker::MergeConstantPools()
{
	std::ranges::for_each
	(
		m_modules,
		[this](const BytecodeModule& module)
		{
			std::vector<size_t> string_index_mapping;
			string_index_mapping.reserve(module.m_string_pool.size());
			std::ranges::transform
			(
				module.m_string_pool,
				std::back_inserter(string_index_mapping),
				[this](const std::string& str)
				{
					return MergeString(str);
				}
			);

			m_module_string_index_mappings[module.m_module_name] = std::move(string_index_mapping);
		}
	);
}

void BytecodeLinker::MergeFunctionNames()
{
	std::ranges::for_each
	(
		m_modules,
		[this](const BytecodeModule& module)
		{
			std::ranges::copy(module.m_procedure_names, std::back_inserter(m_global_procedure_names));
		}
	);
}

void BytecodeLinker::MergeGlobalVariables()
{
	static_cast<void>
	(
		std::accumulate
		(
			m_modules.begin(),
			m_modules.end(),
			0uz,
			[this](size_t current_index, const BytecodeModule& module)
			{
				m_module_base_global_indices[module.m_module_name] = current_index;

				std::ranges::copy(module.m_global_variables, std::back_inserter(m_global_variables));

				return current_index + module.m_global_variables.size();
			}
		)
	);
}

MidoriResult::VoidResult BytecodeLinker::ResolveImportsAndPatch()
{
	for (const BytecodeModule& module : m_modules)
	{
		for (const BytecodeModule::ImportedSymbol& import : module.m_imports)
		{
			const MidoriResult::VoidResult result = ValidateImport(module, import);
			if (!result.has_value())
			{
				return result;
			}
		}
	}

	return {};
}

void BytecodeLinker::ConcatenateBytecode()
{
	std::ranges::for_each
	(
		m_modules,
		[this](BytecodeModule& module)
		{
			const size_t module_proc_base_offset = m_module_base_procedure_indices.at(module.m_module_name);
			const size_t module_global_base_offset = m_module_base_global_indices.at(module.m_module_name);
			const std::vector<size_t>& string_mapping = m_module_string_index_mappings.at(module.m_module_name);
			const std::vector<size_t> import_resolved_indices = ResolveImports(module);

			std::ranges::for_each
			(
				module.m_procedures,
				[this, &module, module_proc_base_offset, module_global_base_offset, &import_resolved_indices, &string_mapping](BytecodeStream& procedure)
				{
					PatchProcedure(procedure, module_proc_base_offset, module_global_base_offset, import_resolved_indices, string_mapping);
					m_global_procedure_source_paths.emplace_back(module.m_source_path.string());
					m_global_procedures.push_back(std::move(procedure));
				}
			);
		}
	);
}

void BytecodeLinker::PatchBootstrapOffsets()
{
	std::ranges::for_each
	(
		std::views::iota(1u, m_global_procedures.size()),
		[this](size_t proc_idx)
		{
			BytecodeStream& procedure = m_global_procedures[proc_idx];
			int bytecode_size = procedure.GetByteCodeSize();

			for (int offset = 0; offset < bytecode_size; )
			{
				OpCode opcode = procedure.ReadByteCode(offset);
				int advance = CalculateInstructionSize(opcode);

				if (opcode == OpCode::MAKE_CLOSURE || opcode == OpCode::MAKE_FUNCTION)
				{
					int old_proc_index = static_cast<int>(procedure.ReadByteCode(offset + 1));
					int new_proc_index = old_proc_index + 1;
					procedure.SetByteCode(offset + 1, static_cast<OpCode>(new_proc_index));
				}
				else if (opcode == OpCode::CALL_PROC || opcode == OpCode::CALL_PROC_0 || opcode == OpCode::CALL_PROC_1 || opcode == OpCode::CALL_PROC_2 || opcode == OpCode::CALL_PROC_3)
				{
					int old_proc_index = static_cast<int>(procedure.ReadByteCode(offset + 1));
					int new_proc_index = old_proc_index + 1;
					procedure.SetByteCode(offset + 1, static_cast<OpCode>(new_proc_index));
				}

				offset += advance;
			}
		}
	);
}

BytecodeStream BytecodeLinker::BuildBootstrapProcedure() const
{
	BytecodeStream bootstrap;
	const std::unordered_map<std::string, std::vector<InstanceGlobalInit>> instance_inits_by_module = BuildInstanceGlobalInits(m_global_procedure_names, m_global_variables);

	std::ranges::for_each
	(
		m_modules,
		[this, &bootstrap, &instance_inits_by_module](const BytecodeModule& module)
		{
			AddModuleInitializer(bootstrap, module, m_module_base_procedure_indices);

			const std::unordered_map<std::string, std::vector<InstanceGlobalInit>>::const_iterator init_it = instance_inits_by_module.find(module.m_module_name);
			if (init_it == instance_inits_by_module.end())
			{
				return;
			}

			std::ranges::for_each
			(
				init_it->second,
				[&bootstrap](const InstanceGlobalInit& init)
				{
					EmitInstanceGlobal(bootstrap, init.m_proc_index, init.m_global_index);
				}
			);
		}
	);

	bootstrap.AddByteCode(OpCode::HALT, 0);

	return bootstrap;
}

const BytecodeModule* BytecodeLinker::FindModule(const std::string& module_name) const
{
	std::vector<BytecodeModule>::const_iterator it = std::ranges::find_if
	(
		m_modules,
		[&module_name](const BytecodeModule& module) { return module.m_module_name == module_name; }
	);

	return (it != m_modules.end()) ? &(*it) : nullptr;
}

std::string BytecodeLinker::MakeSymbolKey(const std::string& module_name, const std::string& symbol_name) const
{
	return module_name + std::string(NameSeparator) + symbol_name;
}

bool BytecodeLinker::IsImportIndex(int global_index) const
{
	return global_index >= 248;
}

size_t BytecodeLinker::ConvertImportIndex(int global_index) const
{
	int import_index = global_index - 256;
	return static_cast<size_t>(-import_index - 1);
}

bool BytecodeLinker::IsImportIndexWide(int global_index) const
{
	return global_index >= IMPORT_PLACEHOLDER_BASE;
}

size_t BytecodeLinker::ConvertImportIndexWide(int global_index) const
{
	return static_cast<size_t>(global_index - IMPORT_PLACEHOLDER_BASE);
}

size_t BytecodeLinker::MergeString(const std::string& str)
{
	std::vector<std::string>::const_iterator it = std::ranges::find(m_global_string_pool, str);

	if (it != m_global_string_pool.end())
	{
		return std::distance(m_global_string_pool.cbegin(), it);
	}

	size_t new_index = m_global_string_pool.size();
	m_global_string_pool.push_back(str);
	return new_index;
}

std::optional<size_t> BytecodeLinker::FindSymbolInExports(const BytecodeModule& module, const std::string& symbol_name) const
{
	std::vector<BytecodeModule::ExportedSymbol>::const_iterator it = std::ranges::find_if
	(
		module.m_exports,
		[&symbol_name](const BytecodeModule::ExportedSymbol& exp) { return exp.m_name == symbol_name; }
	);

	if (it == module.m_exports.end())
	{
		return std::nullopt;
	}

	std::unordered_map<std::string, size_t>::const_iterator base_it = m_module_base_global_indices.find(module.m_module_name);
	size_t base_offset = base_it->second;

	return base_offset + it->m_global_index;
}

std::optional<size_t> BytecodeLinker::FindSymbolInGlobals(const BytecodeModule& module, const std::string& symbol_name, size_t base_global_offset) const
{
	std::vector<std::string>::const_iterator it = std::ranges::find_if
	(
		module.m_global_variables,
		[&symbol_name](const std::string& global_var) { return global_var == symbol_name; }
	);

	if (it == module.m_global_variables.end())
	{
		return std::nullopt;
	}

	size_t local_index = std::distance(module.m_global_variables.cbegin(), it);
	return base_global_offset + local_index;
}

MidoriResult::VoidResult BytecodeLinker::ValidateImport(const BytecodeModule& module, const BytecodeModule::ImportedSymbol& import) const
{
	const std::string symbol_key = MakeSymbolKey(import.m_from_module, import.m_name);
	if (m_global_symbol_table.contains(symbol_key))
	{
		return {};
	}

	const std::string message_with_origin = std::format(
		"Unresolved import: {} from module {} (imported by {}).",
		import.m_name,
		import.m_from_module,
		FormatModuleOrigin(module, import.m_source_provenance));
	const std::string contextual_message = std::format("Unresolved import: {} from module {}.", import.m_name, import.m_from_module);

	const BytecodeModule* imported_module = FindModule(import.m_from_module);
	if (imported_module == nullptr)
	{
		if (std::optional<CompilerError> contextual_error = MakeContextualLinkerError(CompilerErrorCode::BytecodeLinkerUnresolvedImport, contextual_message, module, import.m_source_provenance))
		{
			return std::unexpected(std::move(*contextual_error));
		}

		return std::unexpected(CompilerError::WithFile(CompilerStage::BytecodeLinker, message_with_origin, DiagnosticFileName(module), CompilerErrorCode::BytecodeLinkerUnresolvedImport));
	}

	const size_t base_offset = m_module_base_global_indices.at(imported_module->m_module_name);
	const std::optional<size_t> global_result = FindSymbolInGlobals(*imported_module, import.m_name, base_offset);
	if (!global_result.has_value())
	{
		if (std::optional<CompilerError> contextual_error = MakeContextualLinkerError(CompilerErrorCode::BytecodeLinkerUnresolvedImport, contextual_message, module, import.m_source_provenance))
		{
			return std::unexpected(std::move(*contextual_error));
		}

		return std::unexpected(CompilerError::WithFile(CompilerStage::BytecodeLinker, message_with_origin, DiagnosticFileName(module), CompilerErrorCode::BytecodeLinkerUnresolvedImport));
	}

	return {};
}

std::vector<size_t> BytecodeLinker::ResolveImports(const BytecodeModule& module) const
{
	std::vector<size_t> import_resolved_indices;
	import_resolved_indices.reserve(module.m_imports.size());

	std::ranges::transform
	(
		module.m_imports,
		std::back_inserter(import_resolved_indices),
		[this, &module](const BytecodeModule::ImportedSymbol& import)
		{
			const BytecodeModule* imported_module = FindModule(import.m_from_module);
			if (imported_module == nullptr)
			{
				m_unresolved_imports.emplace_back(std::format("'{}' from unknown module '{}', imported by '{}'", import.m_name, import.m_from_module, module.m_module_name));
				return 0uz;
			}

			const std::optional<size_t> export_result = FindSymbolInExports(*imported_module, import.m_name);
			if (export_result.has_value())
			{
				return export_result.value();
			}

			const size_t base_offset = m_module_base_global_indices.at(imported_module->m_module_name);
			const std::optional<size_t> global_result = FindSymbolInGlobals(*imported_module, import.m_name, base_offset);
			if (!global_result.has_value())
			{
				m_unresolved_imports.emplace_back(std::format("'{}' from module '{}', imported by '{}'", import.m_name, import.m_from_module, module.m_module_name));
				return 0uz;
			}

			return global_result.value();
		}
	);

	return import_resolved_indices;
}

void BytecodeLinker::PatchProcedure(
	BytecodeStream& procedure,
	size_t module_proc_base_offset,
	size_t module_global_base_offset,
	const std::vector<size_t>& import_resolved_indices,
	const std::vector<size_t>& string_mapping) const
{
	const int bytecode_size = procedure.GetByteCodeSize();
	const int proc_base_offset = static_cast<int>(module_proc_base_offset);
	const int global_base_offset = static_cast<int>(module_global_base_offset);

	for (int offset = 0; offset < bytecode_size; )
	{
		const OpCode opcode = procedure.ReadByteCode(offset);
		const int advance = CalculateInstructionSize(opcode);

		if (opcode == OpCode::MAKE_CLOSURE || opcode == OpCode::MAKE_FUNCTION || opcode == OpCode::CALL_PROC ||
			opcode == OpCode::CALL_PROC_0 || opcode == OpCode::CALL_PROC_1 || opcode == OpCode::CALL_PROC_2 || opcode == OpCode::CALL_PROC_3)
		{
			const int old_proc_index = static_cast<int>(procedure.ReadByteCode(offset + 1));
			const int new_proc_index = old_proc_index + proc_base_offset;
			procedure.SetByteCode(offset + 1, static_cast<OpCode>(new_proc_index));
		}
		else if (opcode == OpCode::LOAD_STRING)
		{
			const int old_string_index = static_cast<int>(procedure.ReadByteCode(offset + 1));
			if (old_string_index >= 0 && static_cast<size_t>(old_string_index) < string_mapping.size())
			{
				const size_t new_string_index = string_mapping[old_string_index];
				procedure.SetByteCode(offset + 1, static_cast<OpCode>(new_string_index));
			}
		}
		else if (opcode == OpCode::LOAD_STRING_WIDE)
		{
			const int old_string_index = ReadShortOperandLE(procedure, offset);
			if (old_string_index >= 0 && static_cast<size_t>(old_string_index) < string_mapping.size())
			{
				const size_t new_string_index = string_mapping[old_string_index];
				WriteShortOperandLE(procedure, offset, static_cast<int>(new_string_index));
			}
		}
		else if (
			opcode == OpCode::DEFINE_GLOBAL ||
			opcode == OpCode::GET_GLOBAL ||
			opcode == OpCode::SET_GLOBAL ||
			opcode == OpCode::CALL_GLOBAL
		)
		{
			const int old_global_index = static_cast<int>(procedure.ReadByteCode(offset + 1));

			if (IsImportIndex(old_global_index))
			{
				const size_t import_array_index = ConvertImportIndex(old_global_index);
				if (import_array_index < import_resolved_indices.size())
				{
					const size_t resolved_index = import_resolved_indices[import_array_index];
					procedure.SetByteCode(offset + 1, static_cast<OpCode>(resolved_index));
				}
			}
			else
			{
				const int new_global_index = old_global_index + global_base_offset;
				procedure.SetByteCode(offset + 1, static_cast<OpCode>(new_global_index));
			}
		}
		else if (
			opcode == OpCode::DEFINE_GLOBAL_WIDE ||
			opcode == OpCode::GET_GLOBAL_WIDE ||
			opcode == OpCode::SET_GLOBAL_WIDE ||
			opcode == OpCode::CALL_GLOBAL_WIDE
		)
		{
			const int old_global_index = ReadWideOperand(procedure, offset);

			if (IsImportIndexWide(old_global_index))
			{
				const size_t import_array_index = ConvertImportIndexWide(old_global_index);
				if (import_array_index < import_resolved_indices.size())
				{
					const size_t resolved_index = import_resolved_indices[import_array_index];
					WriteWideOperand(procedure, offset, static_cast<int>(resolved_index));
				}
			}
			else
			{
				const int new_global_index = old_global_index + global_base_offset;
				WriteWideOperand(procedure, offset, new_global_index);
			}
		}

		offset += advance;
	}
}

int BytecodeLinker::CalculateInstructionSize(OpCode opcode) const
{
	switch (opcode)
	{
		case OpCode::ADD_LOCAL_INT:
		case OpCode::IF_LOCAL_LE_INT:
			return 6;
		case OpCode::IF_LOCAL_GE_LOCAL:
			return 7;
		case OpCode::PUSH_LOCAL_SUB_INT:
		case OpCode::GET_LOCAL2:
			return 4;
		case OpCode::INTEGER_CONSTANT:
		case OpCode::FLOAT_CONSTANT:
		case OpCode::WORD_CONSTANT:
			return 9;
		case OpCode::BYTE_CONSTANT:
			return 2;
		case OpCode::CREATE_ARRAY:
		case OpCode::CREATE_TUPLE:
			return 4;
		case OpCode::LOAD_STRING_WIDE:
			return 3;
		case OpCode::JUMP:
		case OpCode::JUMP_IF_FALSE:
		case OpCode::JUMP_IF_TRUE:
		case OpCode::JUMP_BACK:
		case OpCode::IF_INTEGER_EQUAL:
		case OpCode::IF_INTEGER_NOT_EQUAL:
		case OpCode::IF_INTEGER_GREATER:
		case OpCode::IF_INTEGER_GREATER_EQUAL:
		case OpCode::IF_INTEGER_LESS:
		case OpCode::IF_INTEGER_LESS_EQUAL:
		case OpCode::IF_FLOAT_EQUAL:
		case OpCode::IF_FLOAT_NOT_EQUAL:
		case OpCode::IF_FLOAT_GREATER:
		case OpCode::IF_FLOAT_GREATER_EQUAL:
		case OpCode::IF_FLOAT_LESS:
		case OpCode::IF_FLOAT_LESS_EQUAL:
			return 3;
		case OpCode::SPAWN_WORKER:
			return 2;  // opcode + argument count; the function comes off the stack
		case OpCode::JOIN_WORKER:
			return 5;  // opcode + ok, err, cancelled and failed tags
		case OpCode::CHANNEL_CREATE:
		case OpCode::CHANNEL_SEND:
		case OpCode::CHANNEL_RECEIVE:
		case OpCode::CHANNEL_CLOSE:
		case OpCode::WORKER_IS_DONE:
		case OpCode::WORKER_CANCEL:
		case OpCode::MAKE_CELL:
		case OpCode::READ_CELL:
		case OpCode::WRITE_CELL:
			return 1;
		case OpCode::CALL_FOREIGN:
			return 3;
		case OpCode::CALL_FOREIGN_INDEXED:
			return 4;  // opcode + ffi_index + arity + return_type
		case OpCode::CALL_PROC:
			return 3;  // opcode + proc_index + arity
		case OpCode::CALL_PROC_0:
		case OpCode::CALL_PROC_1:
		case OpCode::CALL_PROC_2:
		case OpCode::CALL_PROC_3:
			return 2;  // opcode + proc_index
		case OpCode::CALL_GLOBAL:
			return 3;  // opcode + global_index + arity
		case OpCode::CALL_GLOBAL_WIDE:
			return 4;  // opcode + global_index(2) + arity
		case OpCode::DEFINE_GLOBAL_WIDE:
		case OpCode::GET_GLOBAL_WIDE:
		case OpCode::SET_GLOBAL_WIDE:
		case OpCode::GET_LOCAL_WIDE:
		case OpCode::SET_LOCAL_WIDE:
		case OpCode::GET_LOCAL_CELL_WIDE:
		case OpCode::SET_LOCAL_CELL_WIDE:
		case OpCode::GET_CELL_WIDE:
		case OpCode::SET_CELL_WIDE:
			return 3;
		case OpCode::MAKE_CLOSURE:
		case OpCode::MAKE_FUNCTION:
		case OpCode::LOAD_STRING:
		case OpCode::DEFINE_GLOBAL:
		case OpCode::GET_GLOBAL:
		case OpCode::SET_GLOBAL:
		case OpCode::GET_LOCAL:
		case OpCode::SET_LOCAL:
		case OpCode::GET_LOCAL_CELL:
		case OpCode::SET_LOCAL_CELL:
		case OpCode::GET_CELL:
		case OpCode::SET_CELL:
		case OpCode::CALL:
		case OpCode::BIND_CAPTURES:
		case OpCode::GET_MEMBER:
		case OpCode::POP_VALUES:
		case OpCode::POP_LOCAL_SCOPE:
		case OpCode::POP_BLOCK_SCOPE:
		case OpCode::POP_MATCH_SCOPE:
		case OpCode::TAIL_CALL:
		case OpCode::CONSTRUCT_STRUCT:
		case OpCode::CONSTRUCT_UNION:
		case OpCode::LOAD_EMPTY_UNION:
		case OpCode::SET_TAG:
			return 2;
		case OpCode::CALL_0:
		case OpCode::CALL_1:
		case OpCode::CALL_2:
		case OpCode::CALL_3:
		case OpCode::GET_ARRAY:
		case OpCode::GET_TUPLE:
			return 1;
		default:
			return 1;
	}
}
