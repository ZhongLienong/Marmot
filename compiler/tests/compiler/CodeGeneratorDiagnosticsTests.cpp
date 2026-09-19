#include <string>

#include <catch2/catch_test_macros.hpp>

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

	std::string PackageForeignSource()
	{
		return
			R"(module Image
foreign "MIDORI_FFI_Image_ReadInfo" ReadInfo : fn(Text) -> Array<Int>;
foreign "MIDORI_FFI_Image_ReadInfoo" ReadInfoTypo : fn(Text) -> Array<Int>;
)";
	}

	NativePackage ImagePackage()
	{
		NativePackage package;
		package.m_name = "Image";
		package.m_root = "pkg";
		package.m_library = "pkg/marmot_image.dll";
		package.m_functions = { { "MIDORI_FFI_Image_ReadInfo", "marmot_image_read_info" } };
		return package;
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

TEST_CASE("CodeGenerator accepts exactly the foreign names the module's native package declares", "[compiler][codegen][diagnostics][ffi]")
{
	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> bytecode_result =
		MidoriTest::GenerateBytecodeSnippetWithDiagnostics(PackageForeignSource(), "pkg/Image.mmt", ImagePackage());
	REQUIRE_FALSE(bytecode_result.has_value());
	REQUIRE(bytecode_result.error().Size() == 1u);

	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::CodeGenerator;
	expectation.m_code = CompilerErrorCode::CodeGeneratorUnknownForeignFunction;
	expectation.m_line = 3;
	expectation.m_message_substrings = { "Unknown foreign function 'MIDORI_FFI_Image_ReadInfoo'" };
	RequireErrorMatches(bytecode_result.error().m_errors[0u], expectation);
}

TEST_CASE("CodeGenerator accepts no package foreign names in a module without a native package", "[compiler][codegen][diagnostics][ffi]")
{
	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> bytecode_result =
		MidoriTest::GenerateBytecodeSnippetWithDiagnostics(PackageForeignSource(), "pkg/Image.mmt");
	REQUIRE_FALSE(bytecode_result.has_value());
	REQUIRE(bytecode_result.error().Size() == 2u);

	MidoriTest::ErrorExpectation declared_expectation;
	declared_expectation.m_stage = CompilerStage::CodeGenerator;
	declared_expectation.m_code = CompilerErrorCode::CodeGeneratorUnknownForeignFunction;
	declared_expectation.m_line = 2;
	declared_expectation.m_message_substrings = { "Unknown foreign function 'MIDORI_FFI_Image_ReadInfo'" };
	RequireErrorMatches(bytecode_result.error().m_errors[0u], declared_expectation);

	MidoriTest::ErrorExpectation typo_expectation;
	typo_expectation.m_stage = CompilerStage::CodeGenerator;
	typo_expectation.m_code = CompilerErrorCode::CodeGeneratorUnknownForeignFunction;
	typo_expectation.m_line = 3;
	typo_expectation.m_message_substrings = { "Unknown foreign function 'MIDORI_FFI_Image_ReadInfoo'" };
	RequireErrorMatches(bytecode_result.error().m_errors[1u], typo_expectation);
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
