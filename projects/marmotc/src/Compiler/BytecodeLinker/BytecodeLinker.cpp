#include "BytecodeLinker.h"
#include "Bytecode/Format/Format.h"
#include "Compiler/Constant/Constant.h"
#include "Compiler/Names/SpecialNames.h"
#include "Support/Expected.h"
#include "Support/Optional.h"

#include <map>
#include <set>
#include <algorithm>
#include <format>
#include <numeric>
#include <ranges>
#include <utility>

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

	// Where an instruction's shape operand starts, counted from its opcode.
	MarmotCompiler::Optional<int> ShapeOperandOffset(OpCode opcode)
	{
		switch (opcode)
		{
		case OpCode::CONSTRUCT_STRUCT:
		case OpCode::SPAWN_WORKER:
			return 2;
		case OpCode::CONSTRUCT_UNION:
			return 3;
		case OpCode::CREATE_TUPLE:
			return 4;
		case OpCode::LOCAL_UNION2:
			return 5;
		default:
			return MarmotCompiler::None;
		}
	}

	// A program may have more than 256 procedures.
	void AddProcedure(BytecodeStream& stream, size_t procedure)
	{
		stream.AddByteCode(OpCode::MAKE_FUNCTION_WIDE, 0);
		stream.AddByteCode(static_cast<OpCode>(procedure & BYTE_MASK), 0);
		stream.AddByteCode(static_cast<OpCode>((procedure >> SHIFT_8_BITS) & BYTE_MASK), 0);
		stream.GetReferencedProcedures().push_back(static_cast<uint16_t>(procedure));
	}

	void AddModuleInitializer(BytecodeStream& bootstrap, const BytecodeModule& module, const std::unordered_map<std::string, size_t>& module_base_procedure_indices)
	{
		const std::unordered_map<std::string, size_t>::const_iterator base_it =
			module_base_procedure_indices.find(module.m_module_name);

		const size_t module_base_index = base_it->second;
		const size_t module_global_index = module_base_index + 1u;

		AddProcedure(bootstrap, module_global_index);
		bootstrap.AddByteCode(OpCode::CALL, 0);
		bootstrap.AddByteCode(static_cast<OpCode>(0), 0);
		// A module's top level returns Unit, onto an otherwise empty frame.
		bootstrap.GetFrameMap().m_safepoints.push_back(FrameMap::Safepoint{ bootstrap.GetByteCodeSize(), {}, false });
		bootstrap.AddByteCode(OpCode::POP, 0);
	}

	// Its procedure is two bytes, low first, so it survives a program of more
	// than 256 procedures.
	bool HasWideProcedureOperand(OpCode opcode)
	{
		return opcode == OpCode::MAKE_CLOSURE_OF || opcode == OpCode::CALL_PROC_WIDE || opcode == OpCode::MAKE_FUNCTION_WIDE;
	}

	std::string DiagnosticFileName(const BytecodeModule& module)
	{
		if (!module.m_source_path.empty())
		{
			return module.m_source_path.string();
		}

		return module.m_module_name;
	}

	std::string FormatModuleOrigin(const BytecodeModule& module, const MarmotCompiler::Optional<BytecodeModule::SourceProvenance>& source_provenance)
	{
		const std::string file_name = DiagnosticFileName(module);
		if (!file_name.empty())
		{
			if (source_provenance.HasValue() && source_provenance->m_line > 0)
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

	MarmotCompiler::Optional<std::string_view> ToOptionalStringView(const MarmotCompiler::Optional<std::string>& source_line)
	{
		if (!source_line.HasValue())
		{
			return MarmotCompiler::None;
		}

		return std::string_view(source_line.Value());
	}

	MarmotCompiler::Optional<CompilerError> MakeContextualLinkerError(CompilerErrorCode code, std::string_view message, const BytecodeModule& module, const MarmotCompiler::Optional<BytecodeModule::SourceProvenance>& source_provenance)
	{
		if (!source_provenance.HasValue() || source_provenance->m_line <= 0)
		{
			return MarmotCompiler::None;
		}

		return CompilerError::WithContext
		(
			CompilerStage::BytecodeLinker,
			message,
			source_provenance->m_line,
			DiagnosticFileName(module),
			source_provenance->m_column,
			source_provenance->m_caret_length,
			MarmotCompiler::None,
			ToOptionalStringView(source_provenance->m_source_line),
			code
		);
	}
}

BytecodeLinker::BytecodeLinker(std::vector<BytecodeModule>&& modules, std::string_view entry_module_name)
	: m_modules(std::move(modules)),
	m_entry_module_name(entry_module_name)
{
}

MarmotResult::BytecodeLinkerResult BytecodeLinker::Link()
{
	AssignModuleBaseOffsets();

	MarmotResult::VoidResult symbol_result = BuildGlobalSymbolTable();
	if (!symbol_result.HasValue())
	{
		return MarmotCompiler::Failure(symbol_result.Error());
	}

	MergeConstantPools();
	MarmotResult::VoidResult shape_result = MergeShapes();
	if (!shape_result.HasValue())
	{
		return MarmotCompiler::Failure(shape_result.Error());
	}
	MergeFunctionNames();
	MergeGlobalVariables();
	if (m_global_procedure_names.size() > UINT16_MAX)
	{
		return MarmotCompiler::Failure(CompilerError::Simple(CompilerStage::BytecodeLinker, std::format("Too many procedures in the program (max {} including bootstrap)", UINT16_MAX + 1), CompilerErrorCode::CodeGeneratorLimitExceeded));
	}
	if (m_global_variables.size() > static_cast<size_t>(UINT16_MAX) + 1uz)
	{
		return MarmotCompiler::Failure(CompilerError::Simple(CompilerStage::BytecodeLinker, std::format("Too many globals in the program (max {})", UINT16_MAX + 1), CompilerErrorCode::CodeGeneratorLimitExceeded));
	}
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

	MarmotResult::Result<std::vector<std::vector<size_t>>> imports = ResolveImports();
	if (!imports)
	{
		return MarmotCompiler::Failure(std::move(imports).Error());
	}
	ConcatenateBytecode(imports.Value());

	m_global_procedures.insert(m_global_procedures.begin(), BytecodeStream());
	m_global_procedure_source_paths.insert(m_global_procedure_source_paths.begin(), EntrySourcePathForModule(m_modules, m_entry_module_name));

	// Create bootstrap name with entry module context for debugging
	std::string bootstrap_name = std::format("{}@{}", MmcWire::MODULE_BOOTSTRAP_PREFIX, m_entry_module_name);
	m_global_procedure_names.insert(m_global_procedure_names.begin(), std::move(bootstrap_name));

	m_global_procedures[0u] = BuildBootstrapProcedure();
	PopulateReferencedGlobals();

	MarmotExecutable executable;
	executable.AttachProcedures(std::move(m_global_procedures));
	executable.AttachProcedureNames(std::move(m_global_procedure_names));
	executable.AttachProcedureSourcePaths(std::move(m_global_procedure_source_paths));
	executable.AttachSourceFiles(std::move(m_global_source_files));
	executable.AttachNativeLibraries(MergeNativeLibraries());
	executable.AddStringPool(std::move(m_global_string_pool));
	executable.AttachShapes(std::move(m_global_shapes));
	executable.SetFileName(EntrySourcePathForModule(m_modules, m_entry_module_name));

	for (size_t index = 0uz; index < m_global_variables.size(); index += 1uz)
	{
		executable.AddGlobalVariable(std::move(m_global_variables[index]), m_global_pointers[index]);
	}

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

MarmotResult::VoidResult BytecodeLinker::BuildGlobalSymbolTable()
{
	for (const BytecodeModule& module : m_modules)
	{
		for (const BytecodeModule::ExportedSymbol& exp : module.m_exports)
		{
			const std::string symbol_key = MakeSymbolKey(module.m_module_name, exp.m_name);
			const std::unordered_map<std::string, LinkedExport>::const_iterator existing_it = m_global_symbol_table.find(symbol_key);
			if (existing_it != m_global_symbol_table.end())
			{
				const LinkedExport& existing_export = existing_it->second;
				const std::string message = std::format
				(
					"Duplicate symbol export: {} from module {}. Previous export defined at {}.",
					exp.m_name,
					module.m_module_name,
					FormatModuleOrigin(*existing_export.m_module, existing_export.m_export->m_source_provenance)
				);

				if (MarmotCompiler::Optional<CompilerError> contextual_error = MakeContextualLinkerError(CompilerErrorCode::BytecodeLinkerDuplicateExportedSymbol, message, module, exp.m_source_provenance))
				{
					return MarmotCompiler::Failure(std::move(*contextual_error));
				}
				if (MarmotCompiler::Optional<CompilerError> fallback_context = MakeContextualLinkerError(CompilerErrorCode::BytecodeLinkerDuplicateExportedSymbol, message, *existing_export.m_module, existing_export.m_export->m_source_provenance))
				{
					return MarmotCompiler::Failure(std::move(*fallback_context));
				}

				return MarmotCompiler::Failure(CompilerError::WithFile(CompilerStage::BytecodeLinker, message, DiagnosticFileName(module), CompilerErrorCode::BytecodeLinkerDuplicateExportedSymbol));
			}

			m_global_symbol_table.emplace(symbol_key, LinkedExport{ &module, &exp });
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

MarmotResult::VoidResult BytecodeLinker::MergeShapes()
{
	for (const BytecodeModule& module : m_modules)
	{
		std::vector<uint16_t> shape_mapping;
		shape_mapping.reserve(module.m_shapes.size());
		for (const Shape& shape : module.m_shapes)
		{
			const std::map<Shape, uint16_t>::const_iterator existing = m_global_shape_indices.find(shape);
			if (existing != m_global_shape_indices.cend())
			{
				shape_mapping.push_back(existing->second);
				continue;
			}
			if (m_global_shapes.size() > static_cast<size_t>(UINT16_MAX))
			{
				return MarmotCompiler::Failure(CompilerError::Simple(CompilerStage::BytecodeLinker, std::format("Too many aggregate shapes in the program (max {})", UINT16_MAX + 1), CompilerErrorCode::CodeGeneratorLimitExceeded));
			}
			const uint16_t index = static_cast<uint16_t>(m_global_shapes.size());
			m_global_shapes.push_back(shape);
			m_global_shape_indices.emplace(shape, index);
			shape_mapping.push_back(index);
		}
		m_module_shape_mappings[module.m_module_name] = std::move(shape_mapping);
	}
	return {};
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

// One entry per library, in name order, with every module's symbols and
// directories, each once and sorted.
std::vector<NativeLibraryImport> BytecodeLinker::MergeNativeLibraries() const
{
	std::map<std::string, std::pair<std::set<std::string>, std::set<std::string>>> merged;
	for (const BytecodeModule& module : m_modules)
	{
		for (const NativeLibraryImport& library : module.m_native_libraries)
		{
			std::pair<std::set<std::string>, std::set<std::string>>& entry = merged[library.m_name];
			entry.first.insert(library.m_symbols.begin(), library.m_symbols.end());
			entry.second.insert(library.m_hint_directories.begin(), library.m_hint_directories.end());
		}
	}

	std::vector<NativeLibraryImport> libraries;
	for (const auto& [name, entry] : merged)
	{
		libraries.push_back(NativeLibraryImport
		{
			.m_name = name,
			.m_symbols = std::vector<std::string>(entry.first.begin(), entry.first.end()),
			.m_hint_directories = std::vector<std::string>(entry.second.begin(), entry.second.end())
		});
	}
	return libraries;
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
				std::ranges::copy(module.m_global_pointers, std::back_inserter(m_global_pointers));

				return current_index + module.m_global_variables.size();
			}
		)
	);
}

void BytecodeLinker::ConcatenateBytecode(const std::vector<std::vector<size_t>>& import_indices)
{
	std::unordered_map<std::string, std::string> source_path_by_module;
	for (const BytecodeModule& module : m_modules)
	{
		source_path_by_module.emplace(module.m_module_name, module.m_source_path.string());
	}

	for (auto&& [module, indices] : std::views::zip(m_modules, import_indices))
	{
		const size_t procedure_base = m_module_base_procedure_indices.at(module.m_module_name) + 1uz;
		const size_t global_base = m_module_base_global_indices.at(module.m_module_name);
		const std::vector<size_t>& strings = m_module_string_index_mappings.at(module.m_module_name);
		const std::vector<uint16_t>& shapes = m_module_shape_mappings.at(module.m_module_name);

		for (size_t index = 0uz; index < module.m_procedures.size(); index += 1uz)
		{
			BytecodeStream& procedure = module.m_procedures[index];
			PatchProcedure(procedure, procedure_base, global_base, indices, strings, shapes);

			// A specialization of an imported generic is named for the module it came from.
			const std::string& name = module.m_procedure_names[index];
			const auto owner = source_path_by_module.find(name.substr(name.rfind(MmcWire::MODULE_SEPARATOR) + 1u));
			m_global_procedure_source_paths.emplace_back(owner != source_path_by_module.cend() ? owner->second : module.m_source_path.string());
			m_global_procedures.push_back(std::move(procedure));
		}
	}
}

void BytecodeLinker::PopulateReferencedGlobals()
{
	std::vector<std::vector<size_t>> dependents(m_global_procedures.size());
	std::vector<std::vector<size_t>> users(m_global_variables.size());
	for (size_t proc_index = 0uz; proc_index < m_global_procedures.size(); proc_index += 1uz)
	{
		BytecodeStream& procedure = m_global_procedures[proc_index];
		for (const uint16_t target : std::exchange(procedure.GetReferencedProcedures(), {}))
		{
			if (dependents[target].empty() || dependents[target].back() != proc_index)
			{
				dependents[target].push_back(proc_index);
			}
		}
		for (const uint16_t global : std::exchange(procedure.GetReferencedGlobals(), {}))
		{
			if (users[global].empty() || users[global].back() != proc_index)
			{
				users[global].push_back(proc_index);
			}
		}
	}

	// Visit the dependents of each global's direct users. The last index appended
	// doubles as a visited mark, so cycles terminate and each table is sorted.
	for (size_t global = 0uz; global < users.size(); global += 1uz)
	{
		std::vector<size_t> pending = std::move(users[global]);
		while (!pending.empty())
		{
			const size_t proc_index = pending.back();
			pending.pop_back();
			std::vector<uint16_t>& globals = m_global_procedures[proc_index].GetReferencedGlobals();
			if (globals.empty() || globals.back() != global)
			{
				globals.push_back(static_cast<uint16_t>(global));
				const std::vector<size_t>& procedure_dependents = dependents[proc_index];
				pending.insert(pending.end(), procedure_dependents.begin(), procedure_dependents.end());
			}
		}
	}
}

BytecodeStream BytecodeLinker::BuildBootstrapProcedure() const
{
	BytecodeStream bootstrap;

	std::ranges::for_each
	(
		m_modules,
		[this, &bootstrap](const BytecodeModule& module)
		{
			AddModuleInitializer(bootstrap, module, m_module_base_procedure_indices);
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

const BytecodeModule* BytecodeLinker::DefiningModule(const std::string& module_name, const std::string& symbol_name) const
{
	const BytecodeModule* module = FindModule(module_name);
	while (module != nullptr)
	{
		const BytecodeModule::ReexportMap::const_iterator reexport_it = module->m_reexports.find(symbol_name);
		if (reexport_it == module->m_reexports.cend())
		{
			return module;
		}

		module = FindModule(reexport_it->second);
	}

	return nullptr;
}

MarmotResult::Result<size_t> BytecodeLinker::ResolveImport(const BytecodeModule& module, const BytecodeModule::ImportedSymbol& import) const
{
	const BytecodeModule* owner = DefiningModule(import.m_from_module, import.m_name);
	if (owner != nullptr)
	{
		const size_t base = m_module_base_global_indices.at(owner->m_module_name);
		const auto exported = m_global_symbol_table.find(MakeSymbolKey(owner->m_module_name, import.m_name));
		if (exported != m_global_symbol_table.end())
		{
			return base + exported->second.m_export->m_global_index;
		}

		// Instance methods live among the globals under their mangled names.
		// The parser has already checked the export rules for names in source.
		const auto global = std::ranges::find(owner->m_global_variables, import.m_name);
		if (global != owner->m_global_variables.end())
		{
			return base + static_cast<size_t>(global - owner->m_global_variables.begin());
		}
	}

	const std::string message = std::format("Unresolved import: {} from module {}.", import.m_name, import.m_from_module);
	if (MarmotCompiler::Optional<CompilerError> error = MakeContextualLinkerError(CompilerErrorCode::BytecodeLinkerUnresolvedImport, message, module, import.m_source_provenance))
	{
		return MarmotCompiler::Failure(std::move(*error));
	}
	const std::string message_with_origin = std::format
	(
		"Unresolved import: {} from module {} (imported by {}).",
		import.m_name,
		import.m_from_module,
		FormatModuleOrigin(module, import.m_source_provenance)
	);
	return MarmotCompiler::Failure(CompilerError::WithFile(CompilerStage::BytecodeLinker, message_with_origin, DiagnosticFileName(module), CompilerErrorCode::BytecodeLinkerUnresolvedImport));
}

MarmotResult::Result<std::vector<std::vector<size_t>>> BytecodeLinker::ResolveImports() const
{
	std::vector<std::vector<size_t>> module_indices;
	module_indices.reserve(m_modules.size());
	for (const BytecodeModule& module : m_modules)
	{
		std::vector<size_t>& indices = module_indices.emplace_back();
		indices.reserve(module.m_imports.size());
		for (const BytecodeModule::ImportedSymbol& import : module.m_imports)
		{
			MarmotResult::Result<size_t> index = ResolveImport(module, import);
			if (!index)
			{
				return MarmotCompiler::Failure(std::move(index).Error());
			}
			indices.push_back(index.Value());
		}
	}
	return module_indices;
}

void BytecodeLinker::PatchProcedure(
	BytecodeStream& procedure,
	size_t module_proc_base_offset,
	size_t module_global_base_offset,
	const std::vector<size_t>& import_resolved_indices,
	const std::vector<size_t>& string_mapping,
	const std::vector<uint16_t>& shape_mapping) const
{
	FrameMap& frame = procedure.GetFrameMap();
	frame.m_capture_shape = shape_mapping[frame.m_capture_shape];

	const int bytecode_size = procedure.GetByteCodeSize();
	const int proc_base_offset = static_cast<int>(module_proc_base_offset);
	const int global_base_offset = static_cast<int>(module_global_base_offset);
	const auto resolve_global = [global_base_offset, &import_resolved_indices](int index) -> int
	{
		if (index >= IMPORT_PLACEHOLDER_BASE)
		{
			const size_t import_index = static_cast<size_t>(index - IMPORT_PLACEHOLDER_BASE);
			if (import_index < import_resolved_indices.size())
			{
				return static_cast<int>(import_resolved_indices[import_index]);
			}
		}
		return index + global_base_offset;
	};
	for (uint16_t& global : procedure.GetReferencedGlobals())
	{
		global = static_cast<uint16_t>(resolve_global(global));
	}
	for (uint16_t& target : procedure.GetReferencedProcedures())
	{
		target = static_cast<uint16_t>(target + proc_base_offset);
	}

	for (int offset = 0; offset < bytecode_size; )
	{
		const OpCode opcode = procedure.ReadByteCode(offset);
		const int advance = OpCodeTable::Length(opcode);

		if (const MarmotCompiler::Optional<int> shape_offset = ShapeOperandOffset(opcode))
		{
			const int at = offset + shape_offset.Value() - 1;
			WriteShortOperandLE(procedure, at, shape_mapping[static_cast<size_t>(ReadShortOperandLE(procedure, at))]);
		}

		if (HasWideProcedureOperand(opcode))
		{
			WriteShortOperandLE(procedure, offset, ReadShortOperandLE(procedure, offset) + proc_base_offset);
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
			opcode == OpCode::DEFINE_GLOBAL_WIDE ||
			opcode == OpCode::GET_GLOBAL_WIDE ||
			opcode == OpCode::SET_GLOBAL_WIDE ||
			opcode == OpCode::CALL_GLOBAL_WIDE
		)
		{
			WriteShortOperandLE(procedure, offset, resolve_global(ReadShortOperandLE(procedure, offset)));
		}

		offset += advance;
	}
}
