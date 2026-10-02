#include <catch2/catch_test_macros.hpp>

#include "Loader/Standalone.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
	void AppendSize(std::string& bytes, uint64_t size)
	{
		for (int shift = 0; shift < 64; shift += 8)
		{
			bytes.push_back(static_cast<char>((size >> shift) & 0xffu));
		}
	}

	void AppendBlob(std::string& bytes, std::string_view contents)
	{
		AppendSize(bytes, contents.size());
		bytes.append(contents);
	}

	std::string Fixture(std::string_view name)
	{
		std::ifstream in(std::filesystem::path(MARMOT_MMC_FIXTURES) / (std::string(name) + ".mmc"), std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}

	std::string Image(std::string payload)
	{
		const uint64_t size = payload.size();
		std::string image = "runtime";
		image += payload;
		AppendSize(image, size);
		image.append("MARMOT-EXE-v1\0\0\0", 16u);
		return image;
	}

	std::string Payload(std::string_view fixture, uint64_t libraries)
	{
		std::string payload;
		AppendBlob(payload, Fixture(fixture));
		AppendSize(payload, libraries);
		return payload;
	}

	std::expected<std::optional<MidoriStandalone::Package>, std::string> Read(std::string bytes)
	{
		std::istringstream in(std::move(bytes));
		return MidoriStandalone::Read(in);
	}
}

TEST_CASE("An ordinary VM image has no bundled program", "[standalone]")
{
	for (const std::string& image : { std::string("runtime"), std::string(64u, 'x') })
	{
		const std::expected<std::optional<MidoriStandalone::Package>, std::string> package = Read(image);
		REQUIRE(package.has_value());
		CHECK_FALSE(package->has_value());
	}
}

TEST_CASE("A standalone image reads the existing bytecode contract", "[standalone]")
{
	const std::expected<std::optional<MidoriStandalone::Package>, std::string> package =
		Read(Image(Payload("minimal", 0u)));
	REQUIRE(package.has_value());
	REQUIRE(package->has_value());
	CHECK(package->value().m_program.GetProcedureCount() > 0);
	CHECK(package->value().m_libraries.empty());
}

TEST_CASE("Standalone package lengths and bytecode corruption are rejected", "[standalone]")
{
	std::string truncated = Image(Payload("minimal", 0u));
	for (size_t index = truncated.size() - 24u; index < truncated.size() - 16u; index += 1u)
	{
		truncated[index] = static_cast<char>(0xffu);
	}
	const std::expected<std::optional<MidoriStandalone::Package>, std::string> oversized = Read(truncated);
	REQUIRE_FALSE(oversized.has_value());
	CHECK(oversized.error().find("package exceeds") != std::string::npos);

	const std::expected<std::optional<MidoriStandalone::Package>, std::string> bad_count =
		Read(Image(Payload("minimal", 1u)));
	REQUIRE_FALSE(bad_count.has_value());
	CHECK(bad_count.error().find("library count") != std::string::npos);

	const std::expected<std::optional<MidoriStandalone::Package>, std::string> corrupt =
		Read(Image(Payload("checksum", 0u)));
	REQUIRE_FALSE(corrupt.has_value());
	CHECK(corrupt.error().find("CRC32 mismatch") != std::string::npos);

	const std::expected<std::optional<MidoriStandalone::Package>, std::string> trailing =
		Read(Image(Payload("minimal", 0u) + "extra"));
	REQUIRE_FALSE(trailing.has_value());
	CHECK(trailing.error().find("trailing package bytes") != std::string::npos);
}

TEST_CASE("Standalone native libraries have file names confined to the extraction directory", "[standalone]")
{
	for (const std::string& file_name : { std::string("../escape.dll"), std::string("/escape.dll"), std::string("lib test.bin") })
	{
		std::string payload = Payload("native", 1u);
		AppendBlob(payload, "marmot_test_native");
		AppendBlob(payload, file_name);
		AppendBlob(payload, "library");
		const std::expected<std::optional<MidoriStandalone::Package>, std::string> package = Read(Image(payload));
		if (file_name == "lib test.bin")
		{
			REQUIRE(package.has_value());
			REQUIRE(package->has_value());
			CHECK(package->value().m_libraries[0u].m_file_name == file_name);
			CHECK(package->value().m_libraries[0u].m_bytes == "library");
		}
		else
		{
			REQUIRE_FALSE(package.has_value());
			CHECK(package.error().find("invalid native library file name") != std::string::npos);
		}
	}
}
