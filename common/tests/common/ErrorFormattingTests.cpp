#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include "Common/Error/Error.h"
#include "Compiler/Token/Token.h"
#include "support/CompileHelpers.h"
#include "support/DiagnosticMatchers.h"
#include "Utility/Driver/MidoriDriver.h"

#include <string>
#include <string_view>
#include <vector>

namespace
{
	std::string StripAnsiCodes(std::string_view text)
	{
		std::string stripped;
		stripped.reserve(text.size());

		bool in_escape = false;
		for (const char ch : text)
		{
			if (ch == '\033')
			{
				in_escape = true;
				continue;
			}

			if (in_escape)
			{
				if (ch == 'm')
				{
					in_escape = false;
				}
				continue;
			}

			stripped.push_back(ch);
		}

		return stripped;
	}
}

TEST_CASE("CompilerError WithContext renders source, caret, and suggestion", "[error][format]")
{
	const CompilerError error = CompilerError::WithContext(
		CompilerStage::Parser,
		"Expected expression",
		2,
		"Format.mmt",
		4,
		3u,
		"Try adding a literal",
		"def value = ;");

	const std::string expected_render =
		"Parser Error at Format.mmt:2\n"
		"  |\n"
		"2 | def value = ;\n"
		"  |     ^^^ Expected expression\n"
		"  |\n"
		"  | Try adding a literal\n";

	REQUIRE(StripAnsiCodes(error.Rendered()) == expected_render);
}

TEST_CASE("CompilerWarning WithToken highlights the matching token span", "[warning][format]")
{
	const Token token(std::string("shadowed"), Token::Name::IDENTIFIER_LITERAL, 1, "Warning.mmt", 4, 8u);
	const std::vector<std::string> source_lines
	{
		"def shadowed = value"
	};

	const CompilerWarning warning = CompilerWarning::WithToken(
		CompilerStage::StaticAnalyzer,
		"Unused local",
		token,
		"Warning.mmt",
		source_lines,
		"Prefix with '_' if intentional",
		CompilerWarningCode::UnusedLocal);

	const std::string expected_render =
		"Static Analyzer Warning at Warning.mmt:1\n"
		"  |\n"
		"1 | def shadowed = value\n"
		"  |     ^^^^^^^^ Unused local\n"
		"  |\n"
		"  | Prefix with '_' if intentional\n";

	REQUIRE(StripAnsiCodes(warning.Rendered()) == expected_render);
	REQUIRE(warning.m_code == CompilerWarningCode::UnusedLocal);
}

TEST_CASE("CompilerError WithToken uses the stored token span instead of searching the line text", "[error][format]")
{
	const Token token(std::string("alpha"), Token::Name::IDENTIFIER_LITERAL, 1, "Span.mmt", 20, 5u);
	const std::vector<std::string> source_lines
	{
		"def alpha = alpha + alpha"
	};

	const CompilerError error = CompilerError::WithToken(
		CompilerStage::Parser,
		"Unexpected identifier",
		token,
		"Span.mmt",
		source_lines);

	const std::string expected_render =
		"Parser Error at Span.mmt:1\n"
		"  |\n"
		"1 | def alpha = alpha + alpha\n"
		"  | " + std::string(20u, ' ') + "^^^^^ Unexpected identifier\n"
		"  |\n";

	REQUIRE(StripAnsiCodes(error.Rendered()) == expected_render);
}

TEST_CASE("CompilerError WithToken renders a single caret for zero-length token spans", "[error][format]")
{
	const Token token(std::string{}, Token::Name::END_OF_FILE, 1, "Eof.mmt", 13, 0u);
	const std::vector<std::string> source_lines
	{
		"def value = 1"
	};

	const CompilerError error = CompilerError::WithToken(
		CompilerStage::Parser,
		"Unexpected end of file",
		token,
		"Eof.mmt",
		source_lines);

	const std::string expected_render =
		"Parser Error at Eof.mmt:1\n"
		"  |\n"
		"1 | def value = 1\n"
		"  | " + std::string(13u, ' ') + "^ Unexpected end of file\n"
		"  |\n";

	REQUIRE(StripAnsiCodes(error.Rendered()) == expected_render);
}

TEST_CASE("Simple compiler diagnostics render as plain messages", "[error][warning][format]")
{
	const CompilerError error = CompilerError::Simple(CompilerStage::Module, "Missing import");
	const CompilerWarning warning = CompilerWarning::Simple(CompilerStage::Optimizer, "Dead store removed");

	REQUIRE(error.Rendered() == "Missing import");
	REQUIRE(warning.Rendered() == "Dead store removed");
}

TEST_CASE("Runtime errors render file-backed source lines", "[error][format][runtime]")
{
	const RuntimeError runtime_error =
		MidoriError::GenerateRuntimeError(
			RuntimeErrorCode::IndexOutOfBounds,
			"Index out of bounds at index: 4.",
			CompilerErrorLocation
			{
				.m_file_name = "Runtime.mmt",
				.m_line = 2,
				.m_source_line = "def value = [1, 2][4];"
			});
	const std::string rendered = StripAnsiCodes(runtime_error.Rendered());

	const std::string expected_render =
		"error[IndexOutOfBounds]: Index out of bounds at index: 4.\n"
		" --> Runtime.mmt:2\n"
		"  |\n"
		"2 | def value = [1, 2][4];\n"
		"  | Index out of bounds at index: 4.\n"
		"  |\n";

	REQUIRE(rendered == expected_render);
	REQUIRE(runtime_error.ExitCode() == 1);
}

TEST_CASE("Compiler report renders grouped warnings and structured machine-readable warnings", "[compiler][warning][report]")
{
	CompilerWarning alpha_warning = CompilerWarning::WithContext(
		CompilerStage::StaticAnalyzer,
		"Unused local",
		3,
		"C:/repo/Alpha.mmt",
		4,
		6u,
		"Prefix with '_' if intentional",
		"def unused = 1;",
		CompilerWarningCode::UnusedLocal);

	CompilerWarning second_alpha_warning = CompilerWarning::WithContext(
		CompilerStage::StaticAnalyzer,
		"Captured closure escapes",
		5,
		"C:/repo/Alpha.mmt",
		1,
		6u,
		std::nullopt,
		"next",
		CompilerWarningCode::CaptureEscape);

	CompilerWarning beta_warning = CompilerWarning::WithContext(
		CompilerStage::Parser,
		"Shadowed name",
		2,
		"C:/repo/Beta.mmt",
		2,
		4u,
		std::nullopt,
		"def value = value;",
		CompilerWarningCode::NameShadowing);

	MidoriResult::CompilerReport report(MidoriResult::CompilerWarnings(std::vector<CompilerWarning>
	{
		alpha_warning,
		second_alpha_warning,
		beta_warning
	}));

	const std::string rendered = StripAnsiCodes(report.RenderedWarnings());
	CHECK(rendered.find("[warning] 2 warning(s) in Alpha.mmt\n") != std::string::npos);
	CHECK(rendered.find("[warning] 1 warning(s) in Beta.mmt\n") != std::string::npos);

	const std::string machine = report.MachineReadableWarnings();
	CHECK(machine.find("\"stage\":\"StaticAnalyzer\"") != std::string::npos);
	CHECK(machine.find("\"code\":\"UnusedLocal\"") != std::string::npos);
	CHECK(machine.find("\"file_path\":\"C:/repo/Alpha.mmt\"") != std::string::npos);
	CHECK(machine.find("\"column\":4") != std::string::npos);
	CHECK(machine.find("\"caret_length\":6") != std::string::npos);
	CHECK(machine.find("\"suggestion\":\"Prefix with '_' if intentional\"") != std::string::npos);

	const std::string machine_json = report.MachineReadableJson();
	CHECK(machine_json.find("\"warnings\":[{") != std::string::npos);
	CHECK(machine_json.find("\"errors\":[]") != std::string::npos);
	CHECK(machine_json.find("\"code\":\"CaptureEscape\"") != std::string::npos);
	CHECK(machine_json.find("\"message\":\"Shadowed name\"") != std::string::npos);
}

TEST_CASE("Driver renders compilation warnings and errors behind one banner", "[compiler][driver][diagnostics]")
{
	CompilerWarning warning = CompilerWarning::WithContext(
		CompilerStage::StaticAnalyzer,
		"Unused local",
		3,
		"Warn.mmt",
		4,
		6u,
		std::nullopt,
		"def unused = 1;",
		CompilerWarningCode::UnusedLocal);

	std::vector<CompilerError> errors;
	errors.emplace_back(CompilerError::WithContext(
		CompilerStage::CodeGenerator,
		"First lowering failure",
		2,
		"First.mmt",
		5,
		3u,
		std::nullopt,
		"def x = y;",
		CompilerErrorCode::CodeGeneratorUnsupportedLowering));
	errors.emplace_back(CompilerError::WithContext(
		CompilerStage::BytecodeLinker,
		"Second linker failure",
		4,
		"Second.mmt",
		1,
		4u,
		std::nullopt,
		"use Missing.{run};",
		CompilerErrorCode::BytecodeLinkerUnresolvedImport));

	const MidoriDriver::DriverError error =
		MidoriDriver::DriverError::Compilation(MidoriResult::CompilerReport(
			MidoriResult::CompilerWarnings(std::move(warning)),
			MidoriResult::CompilerDiagnostics(std::move(errors))));
	const std::string rendered = StripAnsiCodes(error.Rendered());

	REQUIRE(rendered.starts_with("Compilation failed :( \n"));

	const size_t warning_position = rendered.find("[warning] 1 warning(s) in Warn.mmt");
	const size_t first_position = rendered.find("Code Generator Error at First.mmt:2");
	const size_t second_position = rendered.find("Bytecode Linker Error at Second.mmt:4");
	REQUIRE(warning_position != std::string::npos);
	REQUIRE(first_position != std::string::npos);
	REQUIRE(second_position != std::string::npos);
	CHECK(warning_position < first_position);
	CHECK(first_position < second_position);
}

TEST_CASE("Driver does not prepend the compilation banner to runtime diagnostics", "[compiler][driver][diagnostics]")
{
	const MidoriDriver::DriverError error =
		MidoriDriver::DriverError::Diagnostics(MidoriResult::CompilerDiagnostics(
			CompilerError::WithContext(
				CompilerStage::Runtime,
				"Division by zero",
				8,
				"Runtime.mmt",
				10,
				1u,
				std::nullopt,
				"return value / 0;")));

	const std::string rendered = StripAnsiCodes(error.Rendered());
	CHECK(rendered.find("Compilation failed :(") == std::string::npos);
	CHECK(rendered.find("Runtime Error at Runtime.mmt:8") != std::string::npos);
}

TEST_CASE("Machine-readable errors serialize location and code metadata", "[compiler][error][report]")
{
	const CompilerError error = CompilerError::WithContext(
		CompilerStage::CodeGenerator,
		"Unsupported lowering",
		6,
		"Lowering.mmt",
		3,
		5u,
		"Rewrite this expression",
		"bad();",
		CompilerErrorCode::CodeGeneratorUnsupportedLowering);

	const std::string serialized = SerializeMachineReadableError(error);
	CHECK(serialized.find("\"stage\":\"CodeGenerator\"") != std::string::npos);
	CHECK(serialized.find("\"code\":\"CodeGeneratorUnsupportedLowering\"") != std::string::npos);
	CHECK(serialized.find("\"file_path\":\"Lowering.mmt\"") != std::string::npos);
	CHECK(serialized.find("\"line\":6") != std::string::npos);
	CHECK(serialized.find("\"column\":3") != std::string::npos);
	CHECK(serialized.find("\"caret_length\":5") != std::string::npos);
	CHECK(serialized.find("\"suggestion\":\"Rewrite this expression\"") != std::string::npos);
}

TEST_CASE("Machine-readable runtime errors serialize runtime code and stack metadata", "[runtime][error][json]")
{
	RuntimeError runtime_error = MidoriError::GenerateRuntimeError(
		RuntimeErrorCode::StackOverflow,
		"Stack overflow - exceeded maximum call depth.",
		CompilerErrorLocation
		{
			.m_file_name = "Runtime.mmt",
			.m_line = 5,
			.m_source_line = "def value = recurse(0);"
		},
		std::vector<RuntimeStackFrame>
		{
			RuntimeStackFrame
			{
				.m_procedure_name = "recurse",
				.m_module_name = "Runtime",
				.m_location = CompilerErrorLocation
				{
					.m_file_name = "Runtime.mmt",
					.m_line = 2,
					.m_source_line = "def recurse = fn(n : Int) -> Int => recurse(n + 1) + 1;"
				},
				.m_recursive_call_count = 12
			}
		});

	const std::string serialized = SerializeMachineReadableRuntimeError(runtime_error);
	CHECK(serialized.find("\"source\":\"marmot-runtime\"") != std::string::npos);
	CHECK(serialized.find("\"code\":\"StackOverflow\"") != std::string::npos);
	CHECK(serialized.find("\"kind\":\"panic\"") != std::string::npos);
	CHECK(serialized.find("\"exitCode\":2") != std::string::npos);
	CHECK(serialized.find("\"stack\":[{") != std::string::npos);
	CHECK(serialized.find("\"procedure\":\"recurse\"") != std::string::npos);
	CHECK(serialized.find("\"recursiveCount\":12") != std::string::npos);
}

TEST_CASE("Compiler report preserves static-analyzer warning metadata on successful compile", "[compiler][warning][report]")
{
	const std::string source_code =
		R"(module CompileWarning
def Compute = fn() -> Int => {
	def used = 1;
	def unused = 2;
	used
};
)";

	MidoriResult::CompilationResult compile_result = MidoriTest::CompileSnippetWithReport(source_code, "CompileWarning.mmt");
	if (!compile_result.has_value())
	{
		FAIL(MidoriTest::StripAnsiCodes(compile_result.error().Rendered()));
	}

	const MidoriResult::CompilerReport& report = MidoriTest::CompilationReport(compile_result);
	REQUIRE(report.HasWarnings());
	REQUIRE_FALSE(report.HasErrors());

	const CompilerWarning* warning = MidoriTest::FindWarning(report, CompilerStage::StaticAnalyzer, CompilerWarningCode::UnusedLocal);
	REQUIRE(warning != nullptr);
	CHECK(warning->m_stage == CompilerStage::StaticAnalyzer);
	CHECK(warning->m_code == CompilerWarningCode::UnusedLocal);
	REQUIRE(warning->m_location.has_value());
	CHECK(std::filesystem::path(warning->m_location->m_file_name).filename().string() == "CompileWarning.mmt");
	CHECK(warning->m_location->m_line == 4);
}
