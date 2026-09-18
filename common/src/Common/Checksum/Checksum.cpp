#include "Common/Checksum/Checksum.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <format>
#include <iomanip>
#include <algorithm>
#include <ranges>
#include <sstream>
#include <system_error>

namespace
{
	class Sha256
	{
	public:
		void Update(const void* data, size_t size)
		{
			const uint8_t* bytes = static_cast<const uint8_t*>(data);
			for (size_t index = 0u; index < size; index += 1u)
			{
				m_buffer[m_buffer_size] = bytes[index];
				m_buffer_size += 1u;
				m_bit_count += 8u;

				if (m_buffer_size == m_buffer.size())
				{
					ProcessBlock(m_buffer.data());
					m_buffer_size = 0u;
				}
			}
		}

		[[nodiscard]] std::string Finalize()
		{
			const uint64_t original_bit_count = m_bit_count;
			const uint8_t padding = 0x80u;
			Update(&padding, 1u);

			const uint8_t zero = 0u;
			while (m_buffer_size != 56u)
			{
				Update(&zero, 1u);
			}

			uint8_t bit_count_bytes[8u]{};
			for (size_t index = 0u; index < 8u; index += 1u)
			{
				bit_count_bytes[7u - index] = static_cast<uint8_t>((original_bit_count >> (index * 8u)) & 0xFFu);
			}
			Update(bit_count_bytes, 8u);

			std::ostringstream output;
			output << "sha256:";
			output << std::hex << std::setfill('0');
			for (const uint32_t word : m_state)
			{
				output << std::setw(8) << word;
			}
			return output.str();
		}

	private:
		static constexpr std::array<uint32_t, 64u> s_round_constants
		{
			0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
			0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
			0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
			0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
			0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
			0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
			0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
			0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
		};

		std::array<uint32_t, 8u> m_state
		{
			0x6a09e667u,
			0xbb67ae85u,
			0x3c6ef372u,
			0xa54ff53au,
			0x510e527fu,
			0x9b05688cu,
			0x1f83d9abu,
			0x5be0cd19u
		};
		std::array<uint8_t, 64u> m_buffer{};
		size_t m_buffer_size = 0u;
		uint64_t m_bit_count = 0u;

		[[nodiscard]] static uint32_t RotateRight(uint32_t value, int shift)
		{
			return (value >> shift) | (value << (32 - shift));
		}

		static void ProcessBlock(const uint8_t* block, std::array<uint32_t, 8u>& state)
		{
			std::array<uint32_t, 64u> schedule{};
			for (size_t index = 0u; index < 16u; index += 1u)
			{
				schedule[index] =
					(static_cast<uint32_t>(block[index * 4u]) << 24) |
					(static_cast<uint32_t>(block[index * 4u + 1u]) << 16) |
					(static_cast<uint32_t>(block[index * 4u + 2u]) << 8) |
					static_cast<uint32_t>(block[index * 4u + 3u]);
			}

			for (size_t index = 16u; index < schedule.size(); index += 1u)
			{
				const uint32_t s0 = RotateRight(schedule[index - 15u], 7) ^ RotateRight(schedule[index - 15u], 18) ^ (schedule[index - 15u] >> 3);
				const uint32_t s1 = RotateRight(schedule[index - 2u], 17) ^ RotateRight(schedule[index - 2u], 19) ^ (schedule[index - 2u] >> 10);
				schedule[index] = schedule[index - 16u] + s0 + schedule[index - 7u] + s1;
			}

			uint32_t a = state[0u];
			uint32_t b = state[1u];
			uint32_t c = state[2u];
			uint32_t d = state[3u];
			uint32_t e = state[4u];
			uint32_t f = state[5u];
			uint32_t g = state[6u];
			uint32_t h = state[7u];

			for (size_t index = 0u; index < schedule.size(); index += 1u)
			{
				const uint32_t sigma1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
				const uint32_t choice = (e & f) ^ ((~e) & g);
				const uint32_t temp1 = h + sigma1 + choice + s_round_constants[index] + schedule[index];
				const uint32_t sigma0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
				const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
				const uint32_t temp2 = sigma0 + majority;

				h = g;
				g = f;
				f = e;
				e = d + temp1;
				d = c;
				c = b;
				b = a;
				a = temp1 + temp2;
			}

			state[0u] += a;
			state[1u] += b;
			state[2u] += c;
			state[3u] += d;
			state[4u] += e;
			state[5u] += f;
			state[6u] += g;
			state[7u] += h;
		}

		void ProcessBlock(const uint8_t* block)
		{
			ProcessBlock(block, m_state);
		}
	};

	[[nodiscard]] std::vector<std::filesystem::path> CollectPackageFiles(const std::filesystem::path& package_directory)
	{
		std::vector<std::filesystem::path> files;
		const std::filesystem::path manifest_path = package_directory / "package.marmot";
		if (std::filesystem::exists(manifest_path))
		{
			files.push_back(manifest_path);
		}

		if (!std::filesystem::exists(package_directory))
		{
			return files;
		}

		for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(package_directory))
		{
			if (!entry.is_regular_file())
			{
				continue;
			}

			if (entry.path().extension() == ".mmt")
			{
				files.push_back(entry.path());
			}
		}

		std::sort(files.begin(), files.end());
		return files;
	}
}

namespace MidoriChecksum
{
	std::string HashBytes(std::string_view bytes)
	{
		Sha256 hash;
		hash.Update(bytes.data(), bytes.size());
		return hash.Finalize();
	}

	std::expected<std::string, std::string> HashFile(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open())
		{
			return std::unexpected(std::format("Failed to open file for hashing: {}", path.string()));
		}

		Sha256 hash;
		std::array<char, 4096u> buffer{};
		while (input.good())
		{
			input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const std::streamsize count = input.gcount();
			if (count > 0)
			{
				hash.Update(buffer.data(), static_cast<size_t>(count));
			}
		}

		if (!input.eof())
		{
			return std::unexpected(std::format("Failed while reading file for hashing: {}", path.string()));
		}

		return hash.Finalize();
	}

	std::expected<std::string, std::string> HashFiles(const std::vector<std::filesystem::path>& files, const std::filesystem::path& root)
	{
		Sha256 hash;
		for (const std::filesystem::path& file : files)
		{
			std::error_code error_code;
			const std::filesystem::path relative_path = std::filesystem::relative(file, root, error_code);
			const std::string path_text = error_code ? file.lexically_normal().generic_string() : relative_path.generic_string();
			hash.Update(path_text.data(), path_text.size());
			static constexpr char separator = '\n';
			hash.Update(&separator, 1u);

			std::ifstream input(file, std::ios::binary);
			if (!input.is_open())
			{
				return std::unexpected(std::format("Failed to open file for hashing: {}", file.string()));
			}

			std::array<char, 4096u> buffer{};
			while (input.good())
			{
				input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
				const std::streamsize count = input.gcount();
				if (count > 0)
				{
					hash.Update(buffer.data(), static_cast<size_t>(count));
				}
			}

			if (!input.eof())
			{
				return std::unexpected(std::format("Failed while reading file for hashing: {}", file.string()));
			}

			hash.Update(&separator, 1u);
		}

		return hash.Finalize();
	}

	std::expected<std::string, std::string> HashPackageSources(const std::filesystem::path& package_directory)
	{
		const std::vector<std::filesystem::path> files = CollectPackageFiles(package_directory);
		return HashFiles(files, package_directory);
	}

	std::expected<bool, std::string> VerifyFileChecksum(const std::filesystem::path& path, std::string_view expected_checksum)
	{
		if (!expected_checksum.starts_with("sha256:"))
		{
			return std::unexpected(std::format("Unsupported checksum format '{}'.", expected_checksum));
		}

		const std::expected<std::string, std::string> actual_checksum = HashFile(path);
		if (!actual_checksum.has_value())
		{
			return std::unexpected(actual_checksum.error());
		}

		return actual_checksum.value() == expected_checksum;
	}
}
