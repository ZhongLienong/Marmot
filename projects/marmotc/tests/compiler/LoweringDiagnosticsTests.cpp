#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "Compiler/BytecodeModule/BytecodeModule.h"
#include "support/CompileHelpers.h"
#include "support/DiagnosticMatchers.h"

using namespace std::string_literals;

namespace
{
	void RequireErrorMatches(const CompilerError& error, const MarmotTest::ErrorExpectation& expectation)
	{
		std::string mismatch;
		const bool matched = MarmotTest::Matches(error, expectation, &mismatch);
		CAPTURE(mismatch);
		REQUIRE(matched);
	}

	std::string UnsupportedForeignReturnSource()
	{
		return
			R"(module ForeignDiagnostics
type Pair =
{
	value: Int
};
foreign "MARMOT_FFI_ReadPairA" ReadPairA : fn() -> Pair;
foreign "MARMOT_FFI_ReadPairB" ReadPairB : fn() -> Pair;
def main = fn() -> Int => {
	ReadPairA();
	ReadPairB();
	0
};
)";
	}
}

TEST_CASE("Lowering rejects a foreign name that is neither builtin nor package-declared", "[compiler][lowering][diagnostics][ffi]")
{
	const std::string source =
		R"(module ForeignTypo
foreign "MARMOT_FFI_ReportPanic" Report : fn(Text) -> Unit;
foreign "MARMOT_FFI_PrintLin" PrintTypo : fn(Text) -> Unit;
)";

	MarmotResult::DiagnosticsResult<BytecodeModule> bytecode_result =
		MarmotTest::GenerateBytecodeSnippetWithDiagnostics(source, "ForeignTypo.mmt");
	REQUIRE_FALSE(bytecode_result.HasValue());
	REQUIRE(bytecode_result.Error().Size() == 1u);

	MarmotTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::Lowering;
	expectation.m_code = CompilerErrorCode::LoweringUnknownForeignFunction;
	expectation.m_line = 3;
	expectation.m_message_substrings = { "Unknown foreign function 'MARMOT_FFI_PrintLin'" };
	expectation.m_rendered_substrings = { "Lowering Error", "ForeignTypo.mmt:3" };
	RequireErrorMatches(bytecode_result.Error().m_items[0u], expectation);
}

TEST_CASE("Intrinsic declarations must match their VM signatures", "[compiler][lowering][diagnostics][ffi]")
{
	const std::string source =
		R"(module BadIntrinsics
foreign "MARMOT_FFI_HashFloat" Hash : fn() -> Int;
foreign "MARMOT_FFI_ArraySlice" Slice : fn(Array<Int>, Int, Int) -> Array<Text>;
)";

	const auto result = MarmotTest::GenerateBytecodeSnippetWithDiagnostics(source, "BadIntrinsics.mmt");
	REQUIRE_FALSE(result.HasValue());
	REQUIRE(result.Error().Size() == 2u);
	CHECK(result.Error().m_items[0u].m_message == "Intrinsic 'MARMOT_FFI_HashFloat' requires fn(Float) -> Int");
	CHECK(result.Error().m_items[1u].m_message == "Intrinsic 'MARMOT_FFI_ArraySlice' requires fn(Array<T>, Int, Int) -> Array<T>");
}

TEST_CASE("The backend records the native libraries that foreign declarations name", "[compiler][backend][ffi]")
{
	const std::string source =
		R"(module Image
foreign "image_write" Write : fn(Text) -> Bool from "marmot_image";
foreign "marmot_image"
{
	"image_read_info" ReadInfo : fn(Text) -> Array<Int>;
	"image_version" Version : fn() -> Int;
}
foreign "MARMOT_FFI_ReportPanic" Report : fn(Text) -> Unit;
foreign "helper" Helper : fn() -> Int from "other";
)";

	MarmotResult::DiagnosticsResult<BytecodeModule> bytecode_result =
		MarmotTest::GenerateBytecodeSnippetWithDiagnostics(source, "pkg/Image.mmt");
	REQUIRE(bytecode_result.HasValue());

	const std::vector<NativeLibraryImport>& libraries = bytecode_result->m_native_libraries;
	REQUIRE(libraries.size() == 2u);
	CHECK(libraries[0u].m_name == "marmot_image");
	CHECK(libraries[0u].m_symbols == std::vector<std::string>{ "image_read_info", "image_version", "image_write" });
	REQUIRE(libraries[0u].m_hint_directories.size() == 1u);
	CHECK(std::filesystem::path(libraries[0u].m_hint_directories[0u]).filename() == "pkg");
	CHECK(libraries[1u].m_name == "other");
	CHECK(libraries[1u].m_symbols == std::vector<std::string>{ "helper" });
}

TEST_CASE("Lowering asks for the library of a foreign function that is not a builtin", "[compiler][lowering][diagnostics][ffi]")
{
	const std::string source =
		R"(module Image
foreign "image_read_info" ReadInfo : fn(Text) -> Array<Int>;
)";

	MarmotResult::DiagnosticsResult<BytecodeModule> bytecode_result =
		MarmotTest::GenerateBytecodeSnippetWithDiagnostics(source, "Image.mmt");
	REQUIRE_FALSE(bytecode_result.HasValue());
	REQUIRE(bytecode_result.Error().Size() == 1u);

	MarmotTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::Lowering;
	expectation.m_code = CompilerErrorCode::LoweringUnknownForeignFunction;
	expectation.m_line = 2;
	expectation.m_message_substrings = { "Unknown foreign function 'image_read_info'", "from \"library\"" };
	RequireErrorMatches(bytecode_result.Error().m_items[0u], expectation);
}

TEST_CASE("A foreign block is only for the top level and must declare something", "[compiler][parser][ffi]")
{
	const std::string local_block =
		R"(module Local
def main = fn() -> Int => {
	foreign "lib" { "f" F : fn() -> Int; }
	0
};
)";
	MarmotResult::DiagnosticsResult<BytecodeModule> local_result =
		MarmotTest::GenerateBytecodeSnippetWithDiagnostics(local_block, "Local.mmt");
	REQUIRE_FALSE(local_result.HasValue());
	CHECK(local_result.Error().m_items[0u].m_message.find("top level") != std::string::npos);

	const std::string empty_block = "module Empty\nforeign \"lib\" { }\n";
	MarmotResult::DiagnosticsResult<BytecodeModule> empty_result =
		MarmotTest::GenerateBytecodeSnippetWithDiagnostics(empty_block, "Empty.mmt");
	REQUIRE_FALSE(empty_result.HasValue());
	CHECK(empty_result.Error().m_items[0u].m_message.find("declares no functions") != std::string::npos);
}

TEST_CASE("Lowering reports every unsupported foreign return type, not only the first", "[compiler][lowering][diagnostics]")
{
	MarmotResult::DiagnosticsResult<BytecodeModule> bytecode_result =
		MarmotTest::GenerateBytecodeSnippetWithDiagnostics(UnsupportedForeignReturnSource(), "ForeignDiagnostics.mmt");
	REQUIRE_FALSE(bytecode_result.HasValue());
	REQUIRE(bytecode_result.Error().Size() == 2u);
	REQUIRE(MarmotTest::FindError(bytecode_result.Error(), CompilerStage::Lowering, CompilerErrorCode::LoweringUnsupportedConstruct) != nullptr);

	MarmotTest::ErrorExpectation first_expectation;
	first_expectation.m_stage = CompilerStage::Lowering;
	first_expectation.m_code = CompilerErrorCode::LoweringUnsupportedConstruct;
	first_expectation.m_line = 6;
	first_expectation.m_message_substrings = { "Unsupported return type for foreign function" };
	first_expectation.m_rendered_substrings = { "Lowering Error", "ForeignDiagnostics.mmt:6" };
	RequireErrorMatches(bytecode_result.Error().m_items[0u], first_expectation);

	MarmotTest::ErrorExpectation second_expectation;
	second_expectation.m_stage = CompilerStage::Lowering;
	second_expectation.m_code = CompilerErrorCode::LoweringUnsupportedConstruct;
	second_expectation.m_line = 7;
	second_expectation.m_message_substrings = { "Unsupported return type for foreign function" };
	second_expectation.m_rendered_substrings = { "Lowering Error", "ForeignDiagnostics.mmt:7" };
	RequireErrorMatches(bytecode_result.Error().m_items[1u], second_expectation);
}

TEST_CASE("Compiler preserves all lowering diagnostics through the compile boundary", "[compiler][lowering][diagnostics]")
{
	MarmotResult::ExecutableDiagnosticsResult compile_result = MarmotTest::CompileSnippet(UnsupportedForeignReturnSource(), "ForeignDiagnostics.mmt");
	REQUIRE_FALSE(compile_result.HasValue());
	REQUIRE(compile_result.Error().Size() == 2u);
	REQUIRE(MarmotTest::FindError(compile_result.Error(), CompilerStage::Lowering, CompilerErrorCode::LoweringUnsupportedConstruct) != nullptr);

	MarmotTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::Lowering;
	expectation.m_code = CompilerErrorCode::LoweringUnsupportedConstruct;
	expectation.m_line = 6;
	expectation.m_message_substrings = { "Unsupported return type for foreign function" };
	expectation.m_rendered_substrings = { "Lowering Error", "ForeignDiagnostics.mmt:6" };
	RequireErrorMatches(compile_result.Error().m_items[0u], expectation);

	MarmotTest::ErrorExpectation second_expectation;
	second_expectation.m_stage = CompilerStage::Lowering;
	second_expectation.m_code = CompilerErrorCode::LoweringUnsupportedConstruct;
	second_expectation.m_line = 7;
	second_expectation.m_message_substrings = { "Unsupported return type for foreign function" };
	second_expectation.m_rendered_substrings = { "Lowering Error", "ForeignDiagnostics.mmt:7" };
	RequireErrorMatches(compile_result.Error().m_items[1u], second_expectation);
}
