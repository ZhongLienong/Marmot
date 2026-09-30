#include "Bytecode/Artifact/BinaryArtifact.h"

#include <array>
#include <cstdint>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "Bytecode/Format/Format.h"

namespace
{
	// -------------------------------------------------------------------------
	// CRC32 (IEEE 802.3 polynomial, standard zlib/gzip variant)
	// -------------------------------------------------------------------------

	[[nodiscard]] constexpr std::array<uint32_t, 256u> BuildCrc32Table() noexcept
	{
		std::array<uint32_t, 256u> table{};
		for (uint32_t index = 0u; index < 256u; index += 1u)
		{
			uint32_t crc = index;
			for (int bit = 0; bit < 8; bit += 1)
			{
				crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
			}
			table[index] = crc;
		}
		return table;
	}

	static constexpr std::array<uint32_t, 256u> s_crc32_table = BuildCrc32Table();

	[[nodiscard]] uint32_t Crc32(const uint8_t* data, size_t size) noexcept
	{
		uint32_t crc = 0xFFFFFFFFu;
		for (size_t index = 0u; index < size; index += 1u)
		{
			crc = (crc >> 8) ^ s_crc32_table[(crc ^ data[index]) & 0xFFu];
		}
		return crc ^ 0xFFFFFFFFu;
	}

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
	// -------------------------------------------------------------------------
	// Header layout (32 bytes, all fields little-endian)
	//
	//  Offset  Size  Field
	//  0       4     magic "MBC\0"
	//  4       4     format_version u32
	//  8       2     version_major u16
	//  10      2     version_minor u16
	//  12      2     version_patch u16
	//  14      2     _reserved u16 (0)
	//  16      4     flags u32  (bit 0: source_files_embedded)
	//  20      8     payload_size u64
	//  28      4     payload_crc32 u32
	// -------------------------------------------------------------------------

	static constexpr uint32_t k_flag_embed_sources = 1u << 0;

	void WritePayload(BinaryWriter& writer, const MidoriExecutable& executable, bool embed_sources)
	{
		// file_name
		writer.WriteString(executable.GetFileName());

		// string_pool
		const MidoriExecutable::StringPool& string_pool = executable.GetStringPool();
		writer.WriteU32(static_cast<uint32_t>(string_pool.size()));
		for (const std::string& entry : string_pool)
		{
			writer.WriteString(entry);
		}

		// globals, by name
		writer.WriteU32(static_cast<uint32_t>(executable.GetGlobalVariableCount()));
		for (int index = 0; index < executable.GetGlobalVariableCount(); index += 1)
		{
			const std::string& name = executable.GetGlobalVariable(index);
			writer.WriteString(name);
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
			writer.WriteU8(library.m_policy.m_checksum.has_value() ? 1u : 0u);
			if (library.m_policy.m_checksum.has_value())
			{
				writer.WriteString(library.m_policy.m_checksum.value());
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
namespace MidoriBinaryArtifact
{
	std::expected<void, std::string> WriteExecutable(
		const MidoriExecutable& executable,
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
				return std::unexpected("Failed to build bytecode artifact payload.");
			}
		}

		const std::string payload_str = payload_buf.str();
		const uint64_t payload_size = static_cast<uint64_t>(payload_str.size());
		const uint32_t payload_crc = Crc32(
			reinterpret_cast<const uint8_t*>(payload_str.data()),
			payload_str.size());

		// Parse version string "major.minor.patch"
		uint16_t ver_major = 1u;
		uint16_t ver_minor = 0u;
		uint16_t ver_patch = 0u;
		{
			const std::string_view version = MIDORI_VERSION_STRING;
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

		const uint32_t flags = embed_sources ? k_flag_embed_sources : 0u;

		BinaryWriter header_writer(out);
		// magic "MBC\0"
		header_writer.WriteRaw(s_magic, 4u);
		// format_version
		header_writer.WriteU32(MbcFormatVersion);
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
			return std::unexpected("Failed to write bytecode artifact to stream.");
		}

		return {};
	}

	std::expected<void, std::string> WriteExecutableToFile(
		const MidoriExecutable& executable,
		const std::filesystem::path& path,
		bool embed_sources)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out.is_open())
		{
			return std::unexpected(std::format("Could not open bytecode artifact for writing: {}", path.string()));
		}

		return WriteExecutable(executable, out, embed_sources);
	}
}
