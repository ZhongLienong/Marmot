#include "Bytecode/Artifact/BinaryArtifact.h"

#include <array>
#include <cstdint>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include <zlib.h>

#include "Bytecode/Format/Format.h"
#include "Support/Expected.h"

namespace
{
	class BinaryWriter
	{
		std::ostream& m_out;

	public:
		explicit BinaryWriter(std::ostream& out)
			: m_out(out)
		{
		}

		void WriteRaw(const void* data, size_t size)
		{
			m_out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
		}

		void WriteU8(uint8_t value)
		{
			m_out.write(reinterpret_cast<const char*>(&value), 1);
		}

		void WriteU16(uint16_t value)
		{
			const uint8_t bytes[2] =
			{
				static_cast<uint8_t>(value & 0xFFu),
				static_cast<uint8_t>((value >> 8) & 0xFFu),
			};
			m_out.write(reinterpret_cast<const char*>(bytes), 2);
		}

		void WriteU32(uint32_t value)
		{
			const uint8_t bytes[4] =
			{
				static_cast<uint8_t>(value & 0xFFu),
				static_cast<uint8_t>((value >> 8) & 0xFFu),
				static_cast<uint8_t>((value >> 16) & 0xFFu),
				static_cast<uint8_t>((value >> 24) & 0xFFu),
			};
			m_out.write(reinterpret_cast<const char*>(bytes), 4);
		}

		void WriteI32(int32_t value)
		{
			WriteU32(static_cast<uint32_t>(value));
		}

		void WriteU64(uint64_t value)
		{
			const uint8_t bytes[8] =
			{
				static_cast<uint8_t>(value & 0xFFu),
				static_cast<uint8_t>((value >> 8) & 0xFFu),
				static_cast<uint8_t>((value >> 16) & 0xFFu),
				static_cast<uint8_t>((value >> 24) & 0xFFu),
				static_cast<uint8_t>((value >> 32) & 0xFFu),
				static_cast<uint8_t>((value >> 40) & 0xFFu),
				static_cast<uint8_t>((value >> 48) & 0xFFu),
				static_cast<uint8_t>((value >> 56) & 0xFFu),
			};
			m_out.write(reinterpret_cast<const char*>(bytes), 8);
		}

		void WriteString(std::string_view str)
		{
			WriteU32(static_cast<uint32_t>(str.size()));
			if (!str.empty())
			{
				m_out.write(str.data(), static_cast<std::streamsize>(str.size()));
			}
		}

		[[nodiscard]] bool Good() const
		{
			return m_out.good();
		}
	};
	void WritePayload(BinaryWriter& writer, const MarmotExecutable& executable, bool embed_sources)
	{
		// file_name
		writer.WriteString(executable.GetFileName());

		// string_pool
		const MarmotExecutable::StringPool& string_pool = executable.GetStringPool();
		writer.WriteU32(static_cast<uint32_t>(string_pool.size()));
		for (const std::string& entry : string_pool)
		{
			writer.WriteString(entry);
		}

		// globals: name, and kind (1 for a pointer)
		writer.WriteU32(static_cast<uint32_t>(executable.GetGlobalVariableCount()));
		for (int index = 0; index < executable.GetGlobalVariableCount(); index += 1)
		{
			const std::string& name = executable.GetGlobalVariable(index);
			writer.WriteString(name);
			writer.WriteU8(executable.IsGlobalPointer(index) ? 1u : 0u);
		}

		// shapes: the pointer fields of each, by index
		const std::vector<Shape>& shapes = executable.GetShapes();
		writer.WriteU32(static_cast<uint32_t>(shapes.size()));
		for (const Shape& shape : shapes)
		{
			writer.WriteU32(static_cast<uint32_t>(shape.size()));
			for (const uint32_t field : shape)
			{
				writer.WriteU32(field);
			}
		}

		// procedures
		writer.WriteU32(static_cast<uint32_t>(executable.GetProcedureCount()));
		for (int proc_index = 0; proc_index < executable.GetProcedureCount(); proc_index += 1)
		{
			// name
			const std::string& proc_name = executable.m_procedure_names[static_cast<size_t>(proc_index)];
			writer.WriteString(proc_name);

			// source_path
			writer.WriteString(executable.GetProcedureSourcePath(proc_index));

			// bytecode — raw u8 stream, already LSB-first per BytecodeBackend
			const int bytecode_size = executable.GetByteCodeSize(proc_index);
			writer.WriteU32(static_cast<uint32_t>(bytecode_size));
			for (int instr = 0; instr < bytecode_size; instr += 1)
			{
				writer.WriteU8(static_cast<uint8_t>(executable.ReadByteCode(instr, proc_index)));
			}

			// line_info — compact (line, count) pairs
			const std::vector<std::pair<int, int>>& line_info = executable.GetBytecodeStream(proc_index).GetLineInfo();
			writer.WriteU32(static_cast<uint32_t>(line_info.size()));
			for (const auto& [line, count] : line_info)
			{
				writer.WriteI32(static_cast<int32_t>(line));
				writer.WriteI32(static_cast<int32_t>(count));
			}

			// frame map: slot kinds, the captures' shape, then each safepoint's
			// offset, temporary kinds and result kind
			const FrameMap& frame = executable.GetBytecodeStream(proc_index).GetFrameMap();
			writer.WriteU32(static_cast<uint32_t>(frame.m_slot_pointers.size()));
			for (const bool is_pointer : frame.m_slot_pointers)
			{
				writer.WriteU8(is_pointer ? 1u : 0u);
			}
			writer.WriteU16(frame.m_capture_shape);
			writer.WriteU32(static_cast<uint32_t>(frame.m_safepoints.size()));
			for (const FrameMap::Safepoint& safepoint : frame.m_safepoints)
			{
				writer.WriteU32(static_cast<uint32_t>(safepoint.m_offset));
				writer.WriteU32(static_cast<uint32_t>(safepoint.m_temporaries.size()));
				for (const bool is_pointer : safepoint.m_temporaries)
				{
					writer.WriteU8(is_pointer ? 1u : 0u);
				}
				writer.WriteU8(safepoint.m_result_is_pointer ? 1u : 0u);
			}

			const std::vector<uint16_t>& referenced_globals = executable.GetBytecodeStream(proc_index).GetReferencedGlobals();
			writer.WriteU32(static_cast<uint32_t>(referenced_globals.size()));
			for (const uint16_t global : referenced_globals)
			{
				writer.WriteU16(global);
			}
		}

		// native_libraries: name, symbols, hint directories, thread_safe, checksum
		const std::vector<NativeLibraryImport>& native_libraries = executable.GetNativeLibraries();
		writer.WriteU32(static_cast<uint32_t>(native_libraries.size()));
		for (const NativeLibraryImport& library : native_libraries)
		{
			writer.WriteString(library.m_name);
			writer.WriteU32(static_cast<uint32_t>(library.m_symbols.size()));
			for (const std::string& symbol : library.m_symbols)
			{
				writer.WriteString(symbol);
			}
			writer.WriteU32(static_cast<uint32_t>(library.m_hint_directories.size()));
			for (const std::string& directory : library.m_hint_directories)
			{
				writer.WriteString(directory);
			}
			writer.WriteU8(library.m_policy.m_thread_safe ? 1u : 0u);
			writer.WriteU8(library.m_policy.m_checksum.HasValue() ? 1u : 0u);
			if (library.m_policy.m_checksum.HasValue())
			{
				writer.WriteString(library.m_policy.m_checksum.Value());
			}
		}

		// source_files (only when embed_sources flag is set)
		if (embed_sources)
		{
			// We iterate over procedure source paths to gather all unique source files
			// (the source file table is private; we embed what the VM uses)
			std::vector<std::pair<std::string, const std::vector<std::string>*>> files_to_embed;
			for (int proc_index = 0; proc_index < executable.GetProcedureCount(); proc_index += 1)
			{
				const std::string_view path = executable.GetProcedureSourcePath(proc_index);
				const std::vector<std::string>* lines = executable.FindSourceLines(path);
				if (lines == nullptr)
				{
					continue;
				}

				bool already_included = false;
				for (const auto& [existing_path, _] : files_to_embed)
				{
					if (existing_path == path)
					{
						already_included = true;
						break;
					}
				}
				if (!already_included)
				{
					files_to_embed.emplace_back(std::string(path), lines);
				}
			}

			writer.WriteU32(static_cast<uint32_t>(files_to_embed.size()));
			for (const auto& [path, lines] : files_to_embed)
			{
				writer.WriteString(path);
				writer.WriteU32(static_cast<uint32_t>(lines->size()));
				for (const std::string& line : *lines)
				{
					writer.WriteString(line);
				}
			}
		}
	}
}
namespace MarmotBinaryArtifact
{
	MarmotCompiler::Expected<void, std::string> WriteExecutable(
		const MarmotExecutable& executable,
		std::ostream& out,
		bool embed_sources)
	{
		// Build payload in memory so we can compute size + CRC32 before writing the header
		std::ostringstream payload_buf;
		{
			BinaryWriter payload_writer(payload_buf);
			WritePayload(payload_writer, executable, embed_sources);
			if (!payload_writer.Good())
			{
				return MarmotCompiler::Failure("Failed to build bytecode artifact payload.");
			}
		}

		const std::string payload_str = payload_buf.str();
		const uint64_t payload_size = static_cast<uint64_t>(payload_str.size());
		const uint32_t payload_crc = static_cast<uint32_t>(crc32_z(0, reinterpret_cast<const Bytef*>(payload_str.data()), payload_str.size()));

		// Parse version string "major.minor.patch"
		uint16_t ver_major = 1u;
		uint16_t ver_minor = 0u;
		uint16_t ver_patch = 0u;
		{
			const std::string_view version = MARMOT_VERSION_STRING;
			size_t pos = 0u;
			size_t dot1 = version.find('.', pos);
			size_t dot2 = dot1 != std::string_view::npos ? version.find('.', dot1 + 1u) : std::string_view::npos;
			if (dot1 != std::string_view::npos)
			{
				ver_major = static_cast<uint16_t>(std::stoul(std::string(version.substr(0u, dot1))));
			}
			if (dot1 != std::string_view::npos && dot2 != std::string_view::npos)
			{
				ver_minor = static_cast<uint16_t>(std::stoul(std::string(version.substr(dot1 + 1u, dot2 - dot1 - 1u))));
				ver_patch = static_cast<uint16_t>(std::stoul(std::string(version.substr(dot2 + 1u))));
			}
		}

		const uint32_t flags = embed_sources ? MmcWire::SOURCE_FILES_EMBEDDED_FLAG : 0u;

		BinaryWriter header_writer(out);
		header_writer.WriteRaw(MmcWire::MAGIC.data(), MmcWire::MAGIC.size());
		// format marker
		header_writer.WriteU32(MmcWire::FORMAT_MARKER);
		// marmot_version
		header_writer.WriteU16(ver_major);
		header_writer.WriteU16(ver_minor);
		header_writer.WriteU16(ver_patch);
		// _reserved
		header_writer.WriteU16(0u);
		// flags
		header_writer.WriteU32(flags);
		// payload_size
		header_writer.WriteU64(payload_size);
		// payload_crc32
		header_writer.WriteU32(payload_crc);

		out.write(payload_str.data(), static_cast<std::streamsize>(payload_str.size()));
		if (!out.good())
		{
			return MarmotCompiler::Failure("Failed to write bytecode artifact to stream.");
		}

		return {};
	}

	MarmotCompiler::Expected<void, std::string> WriteExecutableToFile(
		const MarmotExecutable& executable,
		const std::filesystem::path& path,
		bool embed_sources)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out.is_open())
		{
			return MarmotCompiler::Failure(std::format("Could not open bytecode artifact for writing: {}", path.string()));
		}

		return WriteExecutable(executable, out, embed_sources);
	}
}
