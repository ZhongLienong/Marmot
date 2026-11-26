#include "Executable.h"

BytecodeStream::iterator BytecodeStream::begin()
{
	return m_bytecode.begin();
}

BytecodeStream::iterator BytecodeStream::end()
{
	return m_bytecode.end();
}

BytecodeStream::const_iterator BytecodeStream::cbegin() const
{
	return m_bytecode.cbegin();
}

BytecodeStream::const_iterator BytecodeStream::cend() const
{
	return m_bytecode.cend();
}

BytecodeStream::reverse_iterator BytecodeStream::rbegin()
{
	return m_bytecode.rbegin();
}

BytecodeStream::reverse_iterator BytecodeStream::rend()
{
	return m_bytecode.rend();
}

BytecodeStream::const_reverse_iterator BytecodeStream::crbegin() const
{
	return m_bytecode.crbegin();
}

BytecodeStream::const_reverse_iterator BytecodeStream::crend() const
{
	return m_bytecode.crend();
}

OpCode BytecodeStream::ReadByteCode(int index) const
{
	return m_bytecode[static_cast<size_t>(index)];
}

void BytecodeStream::SetByteCode(int index, OpCode byte)
{
	m_bytecode[static_cast<size_t>(index)] = byte;
}

void BytecodeStream::AddByteCode(OpCode byte, int line)
{
	m_bytecode.push_back(byte);

	if (m_line_info.empty() || m_line_info.back().m_line != line)
	{
		m_line_info.emplace_back(line, 1);
	}
	else
	{
		m_line_info.back().m_byte_count += 1;
	}
}

int BytecodeStream::GetByteCodeSize() const
{
	return static_cast<int>(m_bytecode.size());
}

const OpCode* BytecodeStream::operator[](int index) const
{
	return &m_bytecode[static_cast<size_t>(index)];
}

std::span<const OpCode> BytecodeStream::Bytes() const
{
	return m_bytecode;
}

const std::vector<BytecodeStream::LineRun>& BytecodeStream::GetLineInfo() const
{
	return m_line_info;
}

const FrameMap& BytecodeStream::GetFrameMap() const
{
	return m_frame;
}

FrameMap& BytecodeStream::GetFrameMap()
{
	return m_frame;
}

const std::vector<uint16_t>& BytecodeStream::GetReferencedGlobals() const
{
	return m_referenced_globals;
}

std::vector<uint16_t>& BytecodeStream::GetReferencedGlobals()
{
	return m_referenced_globals;
}

std::vector<uint16_t>& BytecodeStream::GetReferencedProcedures()
{
	return m_referenced_procedures;
}

int MarmotExecutable::AddGlobalVariable(std::string&& name, bool is_pointer)
{
	m_globals.emplace_back(std::move(name));
	m_global_pointers.push_back(is_pointer);
	return static_cast<int>(m_globals.size()) - 1;
}

const std::string& MarmotExecutable::GetGlobalVariable(int index) const
{
	return m_globals[static_cast<size_t>(index)];
}

bool MarmotExecutable::IsGlobalPointer(int index) const
{
	return m_global_pointers[static_cast<size_t>(index)];
}

void MarmotExecutable::AttachShapes(std::vector<Shape>&& shapes)
{
	m_shapes = std::move(shapes);
}

const std::vector<Shape>& MarmotExecutable::GetShapes() const
{
	return m_shapes;
}

void MarmotExecutable::AttachProcedures(Procedures&& bytecode)
{
	m_procedures = std::move(bytecode);
}

void MarmotExecutable::AddStringPool(StringPool&& string_pool)
{
	m_string_pool = std::move(string_pool);
}

void MarmotExecutable::AttachProcedureNames(std::vector<std::string>&& procedure_names)
{
	m_procedure_names = std::move(procedure_names);
}

void MarmotExecutable::AttachProcedureSourcePaths(ProcedureSourcePaths&& procedure_source_paths)
{
	m_procedure_source_paths = std::move(procedure_source_paths);
}

void MarmotExecutable::AttachSourceFiles(SourceFileTable&& source_files)
{
	m_source_files = std::move(source_files);
}

void MarmotExecutable::AttachNativeLibraries(std::vector<NativeLibraryImport>&& native_libraries)
{
	m_native_libraries = std::move(native_libraries);
}

const std::vector<NativeLibraryImport>& MarmotExecutable::GetNativeLibraries() const
{
	return m_native_libraries;
}

void MarmotExecutable::SetFileName(std::string&& file_name)
{
	m_file_name = std::move(file_name);
}

std::string_view MarmotExecutable::GetFileName() const
{
	return m_file_name;
}

std::string_view MarmotExecutable::GetProcedureSourcePath(int proc_index) const
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

const BytecodeStream& MarmotExecutable::GetBytecodeStream(int proc_index) const
{
	return m_procedures[static_cast<size_t>(proc_index)];
}

OpCode MarmotExecutable::ReadByteCode(int instr_index, int proc_index) const
{
	return m_procedures[static_cast<size_t>(proc_index)].ReadByteCode(instr_index);
}

int MarmotExecutable::GetByteCodeSize(int proc_index) const
{
	return m_procedures[static_cast<size_t>(proc_index)].GetByteCodeSize();
}

int MarmotExecutable::GetProcedureCount() const
{
	return static_cast<int>(m_procedures.size());
}

int MarmotExecutable::GetGlobalVariableCount() const
{
	return static_cast<int>(m_globals.size());
}

const MarmotExecutable::StringPool& MarmotExecutable::GetStringPool() const
{
	return m_string_pool;
}

const std::vector<std::string>* MarmotExecutable::FindSourceLines(std::string_view file_name) const
{
	const SourceFileTable::const_iterator it = m_source_files.find(std::string(file_name));
	if (it != m_source_files.end())
	{
		return &it->second;
	}

	return nullptr;
}
