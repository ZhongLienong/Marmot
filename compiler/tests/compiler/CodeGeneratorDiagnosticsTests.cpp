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
	void RequireErrorMatches(const CompilerError& error, const MidoriTest::ErrorExpectation& expectation)
	{
		std::string mismatch;
		const bool matched = MidoriTest::Matches(error, expectation, &mismatch);
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
foreign "MIDORI_FFI_ReadPairA" ReadPairA : fn() -> Pair;
foreign "MIDORI_FFI_ReadPairB" ReadPairB : fn() -> Pair;
def main = fn() -> Int => {
	ReadPairA();
	ReadPairB();
	0
};
)";
	}
}

TEST_CASE("CodeGenerator rejects a foreign name that is neither builtin nor package-declared", "[compiler][codegen][diagnostics][ffi]")
{
	const std::string source =
		R"(module ForeignTypo
foreign "MIDORI_FFI_Print" Print : fn(Text) -> Unit;
foreign "MIDORI_FFI_PrintLin" PrintTypo : fn(Text) -> Unit;
)";

	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> bytecode_result =
		MidoriTest::GenerateBytecodeSnippetWithDiagnostics(source, "ForeignTypo.mmt");
	REQUIRE_FALSE(bytecode_result.has_value());
	REQUIRE(bytecode_result.error().Size() == 1u);

	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::CodeGenerator;
	expectation.m_code = CompilerErrorCode::CodeGeneratorUnknownForeignFunction;
	expectation.m_line = 3;
	expectation.m_message_substrings = { "Unknown foreign function 'MIDORI_FFI_PrintLin'" };
	expectation.m_rendered_substrings = { "Code Generator Error", "ForeignTypo.mmt:3" };
	RequireErrorMatches(bytecode_result.error().m_errors[0u], expectation);
}

TEST_CASE("CodeGenerator records the native libraries that foreign declarations name", "[compiler][codegen][ffi]")
{
	const std::string source =
		R"(module Image
foreign "image_write" Write : fn(Text) -> Bool from "marmot_image";
foreign "marmot_image"
{
	"image_read_info" ReadInfo : fn(Text) -> Array<Int>;
	"image_version" Version : fn() -> Int;
}
foreign "MIDORI_FFI_Print" Print : fn(Text) -> Unit;
foreign "helper" Helper : fn() -> Int from "other";
)";

	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> bytecode_result =
		MidoriTest::GenerateBytecodeSnippetWithDiagnostics(source, "pkg/Image.mmt");
	REQUIRE(bytecode_result.has_value());

	const std::vector<NativeLibraryImport>& libraries = bytecode_result->m_native_libraries;
	REQUIRE(libraries.size() == 2u);
	CHECK(libraries[0u].m_name == "marmot_image");
	CHECK(libraries[0u].m_symbols == std::vector<std::string>{ "image_read_info", "image_version", "image_write" });
	REQUIRE(libraries[0u].m_hint_directories.size() == 1u);
	CHECK(std::filesystem::path(libraries[0u].m_hint_directories[0u]).filename() == "pkg");
	CHECK(libraries[1u].m_name == "other");
	CHECK(libraries[1u].m_symbols == std::vector<std::string>{ "helper" });
}

TEST_CASE("CodeGenerator asks for the library of a foreign function that is not a builtin", "[compiler][codegen][diagnostics][ffi]")
{
	const std::string source =
		R"(module Image
foreign "image_read_info" ReadInfo : fn(Text) -> Array<Int>;
)";

	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> bytecode_result =
		MidoriTest::GenerateBytecodeSnippetWithDiagnostics(source, "Image.mmt");
	REQUIRE_FALSE(bytecode_result.has_value());
	REQUIRE(bytecode_result.error().Size() == 1u);

	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::CodeGenerator;
	expectation.m_code = CompilerErrorCode::CodeGeneratorUnknownForeignFunction;
	expectation.m_line = 2;
	expectation.m_message_substrings = { "Unknown foreign function 'image_read_info'", "from \"library\"" };
	RequireErrorMatches(bytecode_result.error().m_errors[0u], expectation);
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
	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> local_result =
		MidoriTest::GenerateBytecodeSnippetWithDiagnostics(local_block, "Local.mmt");
	REQUIRE_FALSE(local_result.has_value());
	CHECK(local_result.error().m_errors[0u].m_message.find("top level") != std::string::npos);

	const std::string empty_block = "module Empty\nforeign \"lib\" { }\n";
	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> empty_result =
		MidoriTest::GenerateBytecodeSnippetWithDiagnostics(empty_block, "Empty.mmt");
	REQUIRE_FALSE(empty_result.has_value());
	CHECK(empty_result.error().m_errors[0u].m_message.find("declares no functions") != std::string::npos);
}

TEST_CASE("CodeGenerator preserves structured diagnostics for recoverable lowering failures", "[compiler][codegen][diagnostics]")
{
	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> bytecode_result =
		MidoriTest::GenerateBytecodeSnippetWithDiagnostics(UnsupportedForeignReturnSource(), "ForeignDiagnostics.mmt");
	REQUIRE_FALSE(bytecode_result.has_value());
	REQUIRE(bytecode_result.error().Size() == 2u);
	REQUIRE(MidoriTest::FindError(bytecode_result.error(), CompilerStage::CodeGenerator, CompilerErrorCode::CodeGeneratorUnsupportedLowering) != nullptr);

	MidoriTest::ErrorExpectation first_expectation;
	first_expectation.m_stage = CompilerStage::CodeGenerator;
	first_expectation.m_code = CompilerErrorCode::CodeGeneratorUnsupportedLowering;
	first_expectation.m_line = 6;
	first_expectation.m_message_substrings = { "Unsupported return type for foreign function" };
	first_expectation.m_rendered_substrings = { "Code Generator Error", "ForeignDiagnostics.mmt:6" };
	RequireErrorMatches(bytecode_result.error().m_errors[0u], first_expectation);

	MidoriTest::ErrorExpectation second_expectation;
	second_expectation.m_stage = CompilerStage::CodeGenerator;
	second_expectation.m_code = CompilerErrorCode::CodeGeneratorUnsupportedLowering;
	second_expectation.m_line = 7;
	second_expectation.m_message_substrings = { "Unsupported return type for foreign function" };
	second_expectation.m_rendered_substrings = { "Code Generator Error", "ForeignDiagnostics.mmt:7" };
	RequireErrorMatches(bytecode_result.error().m_errors[1u], second_expectation);
}

TEST_CASE("Compiler preserves all codegen diagnostics through the compile boundary", "[compiler][codegen][diagnostics]")
{
	MidoriResult::CompilerResult compile_result = MidoriTest::CompileSnippet(UnsupportedForeignReturnSource(), "ForeignDiagnostics.mmt");
	REQUIRE_FALSE(compile_result.has_value());
	REQUIRE(compile_result.error().Size() == 2u);
	REQUIRE(MidoriTest::FindError(compile_result.error(), CompilerStage::CodeGenerator, CompilerErrorCode::CodeGeneratorUnsupportedLowering) != nullptr);

	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::CodeGenerator;
	expectation.m_code = CompilerErrorCode::CodeGeneratorUnsupportedLowering;
	expectation.m_line = 6;
	expectation.m_message_substrings = { "Unsupported return type for foreign function" };
	expectation.m_rendered_substrings = { "Code Generator Error", "ForeignDiagnostics.mmt:6" };
	RequireErrorMatches(compile_result.error().m_errors[0u], expectation);

	MidoriTest::ErrorExpectation second_expectation;
	second_expectation.m_stage = CompilerStage::CodeGenerator;
	second_expectation.m_code = CompilerErrorCode::CodeGeneratorUnsupportedLowering;
	second_expectation.m_line = 7;
	second_expectation.m_message_substrings = { "Unsupported return type for foreign function" };
	second_expectation.m_rendered_substrings = { "Code Generator Error", "ForeignDiagnostics.mmt:7" };
	RequireErrorMatches(compile_result.error().m_errors[1u], second_expectation);
}
