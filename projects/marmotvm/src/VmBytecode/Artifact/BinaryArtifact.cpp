#include "VmBytecode/Artifact/BinaryArtifact.h"

#include <array>
#include <cstdint>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "VmBytecode/Format/Format.h"

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

	};
	static constexpr uint32_t k_flag_embed_sources = 1u << 0;
}

namespace VmBinaryArtifact
{
	std::expected<VmExecutable, std::string> ReadExecutable(std::istream& in)
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
		if (format_version != VmMbcFormatVersion)
		{
			return std::unexpected(std::format(
				"Bytecode artifact format version mismatch: expected {}, got {}. Rebuild the artifact.",
				VmMbcFormatVersion,
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

		VmExecutable executable;

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
		VmExecutable::StringPool string_pool;
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

		VmExecutable::Procedures procedures;
		procedures.reserve(procedure_count);
		std::vector<std::string> procedure_names;
		procedure_names.reserve(procedure_count);
		VmExecutable::ProcedureSourcePaths procedure_source_paths;
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

			std::vector<VmOpCode> bytecode;
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
				bytecode.push_back(static_cast<VmOpCode>(byte));
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

			procedures.emplace_back(std::move(bytecode), std::move(line_info));
			procedure_names.push_back(std::move(proc_name));
			procedure_source_paths.push_back(std::move(source_path));
		}

		executable.AttachProcedures(std::move(procedures));
		executable.AttachProcedureNames(std::move(procedure_names));
		executable.AttachProcedureSourcePaths(std::move(procedure_source_paths));

		uint32_t native_library_count;
		if (!payload_reader.ReadU32(native_library_count))
		{
			return std::unexpected("Corrupt artifact: could not read native library count.");
		}

		std::vector<VmNativeLibraryImport> native_libraries;
		for (uint32_t library_index = 0u; library_index < native_library_count; library_index += 1u)
		{
			VmNativeLibraryImport library;
			uint32_t symbol_count;
			if (!payload_reader.ReadString(library.m_name) || !payload_reader.ReadU32(symbol_count))
			{
				return std::unexpected(std::format("Corrupt artifact: could not read native library {}.", library_index));
			}
			for (uint32_t symbol_index = 0u; symbol_index < symbol_count; symbol_index += 1u)
			{
				std::string symbol;
				if (!payload_reader.ReadString(symbol))
				{
					return std::unexpected(std::format("Corrupt artifact: could not read symbol {} of native library {}.", symbol_index, library_index));
				}
				library.m_symbols.push_back(std::move(symbol));
			}

			uint32_t directory_count;
			if (!payload_reader.ReadU32(directory_count))
			{
				return std::unexpected(std::format("Corrupt artifact: could not read native library {}.", library_index));
			}
			for (uint32_t directory_index = 0u; directory_index < directory_count; directory_index += 1u)
			{
				std::string directory;
				if (!payload_reader.ReadString(directory))
				{
					return std::unexpected(std::format("Corrupt artifact: could not read a directory of native library {}.", library_index));
				}
				library.m_hint_directories.push_back(std::move(directory));
			}

			uint8_t thread_safe;
			uint8_t has_checksum;
			if (!payload_reader.ReadU8(thread_safe) || !payload_reader.ReadU8(has_checksum))
			{
				return std::unexpected(std::format("Corrupt artifact: could not read native library {}.", library_index));
			}
			library.m_policy.m_thread_safe = thread_safe != 0u;
			if (has_checksum != 0u)
			{
				std::string checksum;
				if (!payload_reader.ReadString(checksum))
				{
					return std::unexpected(std::format("Corrupt artifact: could not read the checksum of native library {}.", library_index));
				}
				library.m_policy.m_checksum = std::move(checksum);
			}
			native_libraries.push_back(std::move(library));
		}
		executable.AttachNativeLibraries(std::move(native_libraries));

		// source_files (optional, only when flag bit 0 is set)
		if (flags & k_flag_embed_sources)
		{
			uint32_t source_file_count;
			if (!payload_reader.ReadU32(source_file_count))
			{
				return std::unexpected("Corrupt artifact: could not read source file count.");
			}

			VmExecutable::SourceFileTable source_files;
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
	std::expected<VmExecutable, std::string> ReadExecutableFromFile(const std::filesystem::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in.is_open())
		{
			return std::unexpected(std::format("Could not open bytecode artifact: {}", path.string()));
		}

		return ReadExecutable(in);
	}
}
