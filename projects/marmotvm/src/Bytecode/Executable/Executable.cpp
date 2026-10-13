#include "Executable.h"

#include <algorithm>
#include <utility>

const VmFrameMap::Safepoint* VmFrameMap::FindSafepoint(int offset) const
{
	const std::vector<Safepoint>::const_iterator found = std::ranges::lower_bound(m_safepoints, offset, {}, &Safepoint::m_offset);
	return found != m_safepoints.cend() && found->m_offset == offset ? &*found : nullptr;
}

VmBytecodeStream::VmBytecodeStream(std::vector<VmOpCode>&& bytecode, std::vector<std::pair<int, int>>&& line_info, VmFrameMap&& frame)
	: m_bytecode(std::move(bytecode)),
	  m_line_info(std::move(line_info)),
	  m_frame(std::move(frame))
{
}

const VmFrameMap& VmBytecodeStream::GetFrameMap() const
{
	return m_frame;
}

VmBytecodeStream::const_iterator VmBytecodeStream::cbegin() const
{
	return m_bytecode.cbegin();
}

VmBytecodeStream::const_iterator VmBytecodeStream::cend() const
{
	return m_bytecode.cend();
}

VmOpCode VmBytecodeStream::ReadByteCode(int index) const
{
	return m_bytecode[static_cast<size_t>(index)];
}

int VmBytecodeStream::GetByteCodeSize() const
{
	return static_cast<int>(m_bytecode.size());
}

int VmBytecodeStream::GetLine(int index) const
{
	int cumulative_count = 0;
	for (const auto& [line, count] : m_line_info)
	{
		cumulative_count += count;
		if (index < cumulative_count)
		{
			return line;
		}
	}

	std::unreachable();
}

const VmOpCode* VmBytecodeStream::operator[](int index) const
{
	return &m_bytecode[static_cast<size_t>(index)];
}

int VmExecutable::AddGlobalVariable(std::string&& name, bool is_pointer)
{
	m_globals.emplace_back(std::move(name));
	m_global_pointers.push_back(is_pointer);
	return static_cast<int>(m_globals.size()) - 1;
}

const std::string& VmExecutable::GetGlobalVariable(int index) const
{
	return m_globals[static_cast<size_t>(index)];
}

bool VmExecutable::IsGlobalPointer(int index) const
{
	return m_global_pointers[static_cast<size_t>(index)];
}

void VmExecutable::AttachShapes(std::vector<VmShape>&& shapes)
{
	m_shapes = std::move(shapes);
}

const std::vector<VmShape>& VmExecutable::GetShapes() const
{
	return m_shapes;
}

void VmExecutable::AttachProcedures(Procedures&& bytecode)
{
	m_procedures = std::move(bytecode);
}

void VmExecutable::AddStringPool(StringPool&& string_pool)
{
	m_string_pool = std::move(string_pool);
}

void VmExecutable::AttachProcedureNames(std::vector<std::string>&& procedure_names)
{
	m_procedure_names = std::move(procedure_names);
}

void VmExecutable::AttachProcedureSourcePaths(ProcedureSourcePaths&& procedure_source_paths)
{
	m_procedure_source_paths = std::move(procedure_source_paths);
}

void VmExecutable::AttachSourceFiles(SourceFileTable&& source_files)
{
	m_source_files = std::move(source_files);
}

void VmExecutable::AttachNativeLibraries(std::vector<VmNativeLibraryImport>&& native_libraries)
{
	m_native_libraries = std::move(native_libraries);
}

const std::vector<VmNativeLibraryImport>& VmExecutable::GetNativeLibraries() const
{
	return m_native_libraries;
}

void VmExecutable::SetFileName(std::string&& file_name)
{
	m_file_name = std::move(file_name);
}

std::string_view VmExecutable::GetFileName() const
{
	return m_file_name;
}

std::string_view VmExecutable::GetProcedureSourcePath(int proc_index) const
{
	if (proc_index < 0 || proc_index >= static_cast<int>(m_procedure_source_paths.size()))
	{
		return m_file_name;
	}

	const std::string& source_path = m_procedure_source_paths[static_cast<size_t>(proc_index)];
	if (source_path.empty())
	{
		return m_file_name;
	}

	return source_path;
}

int VmExecutable::GetLine(int instr_index, int proc_index) const
{
	return m_procedures[static_cast<size_t>(proc_index)].GetLine(instr_index);
}

const VmBytecodeStream& VmExecutable::GetBytecodeStream(int proc_index) const
{
	return m_procedures[static_cast<size_t>(proc_index)];
}

VmOpCode VmExecutable::ReadByteCode(int instr_index, int proc_index) const
{
	return m_procedures[static_cast<size_t>(proc_index)].ReadByteCode(instr_index);
}

int VmExecutable::GetByteCodeSize(int proc_index) const
{
	return m_procedures[static_cast<size_t>(proc_index)].GetByteCodeSize();
}

int VmExecutable::GetProcedureCount() const
{
	return static_cast<int>(m_procedures.size());
}

int VmExecutable::GetGlobalVariableCount() const
{
	return static_cast<int>(m_globals.size());
}

const VmExecutable::StringPool& VmExecutable::GetStringPool() const
{
	return m_string_pool;
}

const std::vector<std::string>* VmExecutable::FindSourceLines(std::string_view file_name) const
{
	const SourceFileTable::const_iterator it = m_source_files.find(std::string(file_name));
	if (it != m_source_files.end())
	{
		return &it->second;
	}

	return nullptr;
}
