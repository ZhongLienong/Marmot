#include "Common/BytecodeArtifact/BinaryArtifact.h"

#include <array>
#include <cstdint>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "Common/BuildConfig/BuildConfig.h"

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

	// -------------------------------------------------------------------------
	// Binary I/O helpers (little-endian on disk)
	// -------------------------------------------------------------------------

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

	class BinaryReader
	{
		std::istream& m_in;

	public:
		explicit BinaryReader(std::istream& in)
			: m_in(in)
		{
		}

		[[nodiscard]] bool ReadRaw(void* out, size_t size)
		{
			m_in.read(static_cast<char*>(out), static_cast<std::streamsize>(size));
			return m_in.gcount() == static_cast<std::streamsize>(size);
		}

		[[nodiscard]] bool ReadU8(uint8_t& out)
		{
			return ReadRaw(&out, 1u);
		}

		[[nodiscard]] bool ReadU16(uint16_t& out)
		{
			uint8_t bytes[2];
			if (!ReadRaw(bytes, 2u))
			{
				return false;
			}
			out = static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8);
			return true;
		}

		[[nodiscard]] bool ReadU32(uint32_t& out)
		{
			uint8_t bytes[4];
			if (!ReadRaw(bytes, 4u))
			{
				return false;
			}
			out = static_cast<uint32_t>(bytes[0])
				| (static_cast<uint32_t>(bytes[1]) << 8)
				| (static_cast<uint32_t>(bytes[2]) << 16)
				| (static_cast<uint32_t>(bytes[3]) << 24);
			return true;
		}

		[[nodiscard]] bool ReadI32(int32_t& out)
		{
			uint32_t value;
			if (!ReadU32(value))
			{
				return false;
			}
			out = static_cast<int32_t>(value);
			return true;
		}

		[[nodiscard]] bool ReadU64(uint64_t& out)
		{
			uint8_t bytes[8];
			if (!ReadRaw(bytes, 8u))
			{
				return false;
			}
			out = static_cast<uint64_t>(bytes[0])
				| (static_cast<uint64_t>(bytes[1]) << 8)
				| (static_cast<uint64_t>(bytes[2]) << 16)
				| (static_cast<uint64_t>(bytes[3]) << 24)
				| (static_cast<uint64_t>(bytes[4]) << 32)
				| (static_cast<uint64_t>(bytes[5]) << 40)
				| (static_cast<uint64_t>(bytes[6]) << 48)
				| (static_cast<uint64_t>(bytes[7]) << 56);
			return true;
		}

		[[nodiscard]] bool ReadString(std::string& out)
		{
			uint32_t len;
			if (!ReadU32(len))
			{
				return false;
			}
			out.resize(len);
			if (len > 0u)
			{
				m_in.read(out.data(), static_cast<std::streamsize>(len));
				if (m_in.gcount() != static_cast<std::streamsize>(len))
				{
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool Good() const
		{
			return m_in.good() || m_in.eof();
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

	static constexpr size_t k_header_size = 32u;
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

		// globals — serialized via GetCString()/GetByteLength(), never memcpy of MidoriText struct
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

			// bytecode — raw u8 stream, already LSB-first per CodeGenerator
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
			const std::string_view version = MidoriBuild::VersionString;
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
		header_writer.WriteU32(MidoriBuild::MbcFormatVersion);
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

	std::expected<MidoriExecutable, std::string> ReadExecutable(std::istream& in)
	{
		BinaryReader reader(in);

		// Read and validate header
		uint8_t magic[4];
		if (!reader.ReadRaw(magic, 4u))
		{
			return std::unexpected("Truncated artifact: could not read magic.");
		}
		if (magic[0] != s_magic[0] || magic[1] != s_magic[1] || magic[2] != s_magic[2] || magic[3] != s_magic[3])
		{
			return std::unexpected("Not a Marmot bytecode artifact (bad magic).");
		}

		uint32_t format_version;
		if (!reader.ReadU32(format_version))
		{
			return std::unexpected("Truncated artifact: could not read format version.");
		}
		if (format_version != MidoriBuild::MbcFormatVersion)
		{
			return std::unexpected(std::format(
				"Bytecode artifact format version mismatch: expected {}, got {}. Rebuild the artifact.",
				MidoriBuild::MbcFormatVersion,
				format_version));
		}

		// informational version fields (read but not validated)
		uint16_t ver_major;
		uint16_t ver_minor;
		uint16_t ver_patch;
		uint16_t reserved;
		if (!reader.ReadU16(ver_major) || !reader.ReadU16(ver_minor) || !reader.ReadU16(ver_patch) || !reader.ReadU16(reserved))
		{
			return std::unexpected("Truncated artifact: could not read version fields.");
		}

		uint32_t flags;
		if (!reader.ReadU32(flags))
		{
			return std::unexpected("Truncated artifact: could not read flags.");
		}

		uint64_t payload_size;
		if (!reader.ReadU64(payload_size))
		{
			return std::unexpected("Truncated artifact: could not read payload size.");
		}

		uint32_t expected_crc;
		if (!reader.ReadU32(expected_crc))
		{
			return std::unexpected("Truncated artifact: could not read CRC32.");
		}

		// Read payload
		std::string payload(payload_size, '\0');
		if (!reader.ReadRaw(payload.data(), payload_size))
		{
			return std::unexpected(std::format(
				"Truncated artifact: payload is shorter than declared size ({} bytes).",
				payload_size));
		}

		// Validate CRC32
		const uint32_t actual_crc = Crc32(
			reinterpret_cast<const uint8_t*>(payload.data()),
			payload.size());
		if (actual_crc != expected_crc)
		{
			return std::unexpected("Bytecode artifact is corrupt (CRC32 mismatch).");
		}

		// Parse payload
		std::istringstream payload_stream(payload);
		BinaryReader payload_reader(payload_stream);

		MidoriExecutable executable;

		// file_name
		std::string file_name;
		if (!payload_reader.ReadString(file_name))
		{
			return std::unexpected("Corrupt artifact: could not read file name.");
		}
		executable.SetFileName(std::move(file_name));

		// string_pool
		uint32_t string_pool_count;
		if (!payload_reader.ReadU32(string_pool_count))
		{
			return std::unexpected("Corrupt artifact: could not read string pool count.");
		}
		MidoriExecutable::StringPool string_pool;
		string_pool.reserve(string_pool_count);
		for (uint32_t index = 0u; index < string_pool_count; index += 1u)
		{
			std::string entry;
			if (!payload_reader.ReadString(entry))
			{
				return std::unexpected(std::format("Corrupt artifact: could not read string pool entry {}.", index));
			}
			string_pool.push_back(std::move(entry));
		}
		executable.AddStringPool(std::move(string_pool));

		// globals
		uint32_t global_count;
		if (!payload_reader.ReadU32(global_count))
		{
			return std::unexpected("Corrupt artifact: could not read global count.");
		}
		for (uint32_t index = 0u; index < global_count; index += 1u)
		{
			std::string name;
			if (!payload_reader.ReadString(name))
			{
				return std::unexpected(std::format("Corrupt artifact: could not read global name {}.", index));
			}
			executable.AddGlobalVariable(std::move(name));
		}

		// procedures
		uint32_t procedure_count;
		if (!payload_reader.ReadU32(procedure_count))
		{
			return std::unexpected("Corrupt artifact: could not read procedure count.");
		}

		MidoriExecutable::Procedures procedures;
		procedures.reserve(procedure_count);
		std::vector<std::string> procedure_names;
		procedure_names.reserve(procedure_count);
		MidoriExecutable::ProcedureSourcePaths procedure_source_paths;
		procedure_source_paths.reserve(procedure_count);

		for (uint32_t proc_index = 0u; proc_index < procedure_count; proc_index += 1u)
		{
			std::string proc_name;
			if (!payload_reader.ReadString(proc_name))
			{
				return std::unexpected(std::format("Corrupt artifact: could not read procedure name {}.", proc_index));
			}

			std::string source_path;
			if (!payload_reader.ReadString(source_path))
			{
				return std::unexpected(std::format("Corrupt artifact: could not read source path for procedure {}.", proc_index));
			}

			uint32_t bytecode_size;
			if (!payload_reader.ReadU32(bytecode_size))
			{
				return std::unexpected(std::format("Corrupt artifact: could not read bytecode size for procedure {}.", proc_index));
			}

			std::vector<OpCode> bytecode;
			bytecode.reserve(bytecode_size);
			for (uint32_t instr = 0u; instr < bytecode_size; instr += 1u)
			{
				uint8_t byte;
				if (!payload_reader.ReadU8(byte))
				{
					return std::unexpected(std::format(
						"Corrupt artifact: bytecode truncated at instruction {} of procedure {}.",
						instr, proc_index));
				}
				bytecode.push_back(static_cast<OpCode>(byte));
			}

			uint32_t line_info_count;
			if (!payload_reader.ReadU32(line_info_count))
			{
				return std::unexpected(std::format("Corrupt artifact: could not read line info count for procedure {}.", proc_index));
			}

			std::vector<std::pair<int, int>> line_info;
			line_info.reserve(line_info_count);
			for (uint32_t entry_index = 0u; entry_index < line_info_count; entry_index += 1u)
			{
				int32_t line;
				int32_t count;
				if (!payload_reader.ReadI32(line) || !payload_reader.ReadI32(count))
				{
					return std::unexpected(std::format(
						"Corrupt artifact: line info truncated at entry {} of procedure {}.",
						entry_index, proc_index));
				}
				line_info.emplace_back(static_cast<int>(line), static_cast<int>(count));
			}

			procedures.push_back(BytecodeStream::FromRaw(std::move(bytecode), std::move(line_info)));
			procedure_names.push_back(std::move(proc_name));
			procedure_source_paths.push_back(std::move(source_path));
		}

		executable.AttachProcedures(std::move(procedures));
		executable.AttachProcedureNames(std::move(procedure_names));
		executable.AttachProcedureSourcePaths(std::move(procedure_source_paths));

		// source_files (optional, only when flag bit 0 is set)
		if (flags & k_flag_embed_sources)
		{
			uint32_t source_file_count;
			if (!payload_reader.ReadU32(source_file_count))
			{
				return std::unexpected("Corrupt artifact: could not read source file count.");
			}

			MidoriExecutable::SourceFileTable source_files;
			for (uint32_t file_index = 0u; file_index < source_file_count; file_index += 1u)
			{
				std::string path;
				if (!payload_reader.ReadString(path))
				{
					return std::unexpected(std::format("Corrupt artifact: could not read source file path {}.", file_index));
				}

				uint32_t line_count;
				if (!payload_reader.ReadU32(line_count))
				{
					return std::unexpected(std::format("Corrupt artifact: could not read line count for source file {}.", file_index));
				}

				std::vector<std::string> lines;
				lines.reserve(line_count);
				for (uint32_t line_index = 0u; line_index < line_count; line_index += 1u)
				{
					std::string line;
					if (!payload_reader.ReadString(line))
					{
						return std::unexpected(std::format(
							"Corrupt artifact: could not read line {} of source file {}.",
							line_index, file_index));
					}
					lines.push_back(std::move(line));
				}

				source_files.emplace(std::move(path), std::move(lines));
			}

			executable.AttachSourceFiles(std::move(source_files));
		}

		return executable;
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

	std::expected<MidoriExecutable, std::string> ReadExecutableFromFile(const std::filesystem::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in.is_open())
		{
			return std::unexpected(std::format("Could not open bytecode artifact: {}", path.string()));
		}

		return ReadExecutable(in);
	}
}
