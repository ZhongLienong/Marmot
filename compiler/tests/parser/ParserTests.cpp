#include <catch2/catch_test_macros.hpp>

#include "Common/Constant/Constant.h"
#include "Compiler/BuildGraph/BuildGraph.h"
#include "Compiler/Lexer/Lexer.h"
#include "Compiler/ModuleManager/ModuleManager.h"
#include "Compiler/Parser/Parser.h"
#include "Compiler/Token/Token.h"
#include "support/CompileHelpers.h"
#include "support/DiagnosticMatchers.h"
#include "support/TempProject.h"

#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

struct ParserTestAccess
{
	static Token::Name CurrentTokenName(Parser& parser)
	{
		return parser.Peek(0).m_token_name;
	}
};

namespace
{
	const MidoriStatement::VariableDefinition& RequireVariableDefinition(const MidoriProgramTree& program, size_t index, std::string_view expected_name)
	{
		REQUIRE(index < program.size());
		REQUIRE(program[index] != nullptr);
		REQUIRE(program[index]->IsStatement<MidoriStatement::VariableDefinition>());

		const MidoriStatement::VariableDefinition& definition = program[index]->GetStatement<MidoriStatement::VariableDefinition>();
		REQUIRE(definition.m_name.m_lexeme == expected_name);
		return definition;
	}

	template<typename ExpressionType>
	const ExpressionType& RequireExpression(const std::unique_ptr<MidoriExpression>& expression)
	{
		REQUIRE(expression != nullptr);
		REQUIRE(expression->template IsExpression<ExpressionType>());
		return expression->template GetExpression<ExpressionType>();
	}

	const MidoriExpression::Literal& RequireLiteral(const std::unique_ptr<MidoriExpression>& expression, MidoriExpression::LiteralKind kind)
	{
		const MidoriExpression::Literal& literal = RequireExpression<MidoriExpression::Literal>(expression);
		REQUIRE(literal.m_kind == kind);
		return literal;
	}

	// `def Name = fn(...)` parses as a variable definition whose value is a lambda; the
	// FunctionDefinition node is now only produced by instance methods, deriving and
	// closure lifting.
	const MidoriExpression::Function& RequireFunctionBinding(const MidoriProgramTree& program, size_t index, std::string_view expected_name)
	{
		const MidoriStatement::VariableDefinition& definition = RequireVariableDefinition(program, index, expected_name);
		return RequireExpression<MidoriExpression::Function>(definition.m_value);
	}

	template<typename PatternType>
	const PatternType& RequirePattern(const std::unique_ptr<MidoriPattern>& pattern)
	{
		REQUIRE(pattern != nullptr);
		REQUIRE(pattern->template IsPattern<PatternType>());
		return pattern->template GetPattern<PatternType>();
	}

	struct PreparedParser
	{
		MidoriTest::SourceFixture m_source;
		std::optional<ModuleDeclaration> m_module_declaration;
		std::vector<UseImport> m_use_imports;
		Parser m_parser;

		PreparedParser(MidoriTest::SourceFixture source, TokenStream&& tokens, std::optional<ModuleDeclaration>&& module_declaration, std::vector<UseImport>&& use_imports)
			: m_source(std::move(source)),
			m_module_declaration(std::move(module_declaration)),
			m_use_imports(std::move(use_imports)),
			m_parser
			(
				std::move(tokens),
				m_source.FileName(),
				m_source.SourceLines(),
				{},
				{},
				m_use_imports,
				m_module_declaration.has_value() ? &m_module_declaration.value() : nullptr
			)
		{
		}
	};

	std::expected<std::unique_ptr<PreparedParser>, CompilerError> PrepareParser(std::string source_code, std::string file_name)
	{
		MidoriTest::SourceFixture source(std::move(source_code), std::move(file_name));
		MidoriResult::LexerResult lex_result = Lexer(std::string(source.SourceCode()), source.FileName()).Lex();
		if (!lex_result.has_value())
		{
			return std::unexpected(std::move(lex_result.error()));
		}

		MidoriResult::ModuleManagerResult build_graph_result = ModuleManager(std::move(lex_result.value()), source.FileName(), source.SourceLines()).GenerateBuildGraph();
		if (!build_graph_result.has_value())
		{
			return std::unexpected(std::move(build_graph_result.error()));
		}

		BuildGraph build_graph = std::move(build_graph_result.value());
		std::unordered_map<std::string, BuildGraph::BuildNode>::iterator node_it = build_graph.m_nodes.find(source.FileName());
		if (node_it == build_graph.m_nodes.end())
		{
			return std::unexpected(CompilerError::Simple(CompilerStage::Module, "Failed to locate the prepared parser module."));
		}

		std::optional<ModuleDeclaration> module_declaration = std::nullopt;
		std::unordered_map<std::string, ModuleDeclaration>::const_iterator module_it = build_graph.m_module_declarations.find(source.FileName());
		if (module_it != build_graph.m_module_declarations.end())
		{
			module_declaration = module_it->second;
		}

		return std::make_unique<PreparedParser>
		(
			std::move(source),
			std::move(node_it->second.m_tokens),
			std::move(module_declaration),
			std::move(node_it->second.m_use_imports)
		);
	}

	void RequireErrorMatches(const CompilerError& error, const MidoriTest::ErrorExpectation& expectation)
	{
		std::string mismatch;
		const bool matched = MidoriTest::Matches(error, expectation, &mismatch);
		CAPTURE(mismatch);
		REQUIRE(matched);
	}
}

TEST_CASE("Parser requires parentheses between operators that do not bind equally", "[parser]")
{
	// Operators of different precedence used to bind by a table the reader had to
	// remember. Parentheses say it instead, so a chain of one operator parses as
	// before and a mixed one is rejected.
	const std::string source_code =
		R"(module ParserPrecedence
def chain = 1 + 2 - 3;
def grouped = 1 + (2 * 3);
def tuple_value = (1 + 2, 3);
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "ParserPrecedence.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	const MidoriProgramTree& program = parse_result->m_program;
	REQUIRE(program.size() == 3u);

	// `1 + 2 - 3` is one precedence class, so it still nests to the left.
	const MidoriStatement::VariableDefinition& chain_definition = RequireVariableDefinition(program, 0u, "chain");
	const MidoriExpression::Binary& chain_expr = RequireExpression<MidoriExpression::Binary>(chain_definition.m_value);
	REQUIRE(chain_expr.m_op.m_token_name == Token::Name::SINGLE_MINUS);
	const MidoriExpression::Binary& chain_left = RequireExpression<MidoriExpression::Binary>(chain_expr.m_left);
	REQUIRE(chain_left.m_op.m_token_name == Token::Name::SINGLE_PLUS);
	REQUIRE(RequireLiteral(chain_left.m_left, MidoriExpression::LiteralKind::Integer).m_token.m_lexeme == "1");
	REQUIRE(RequireLiteral(chain_left.m_right, MidoriExpression::LiteralKind::Integer).m_token.m_lexeme == "2");
	REQUIRE(RequireLiteral(chain_expr.m_right, MidoriExpression::LiteralKind::Integer).m_token.m_lexeme == "3");

	// `1 + (2 * 3)` keeps the multiplication in its own group.
	const MidoriStatement::VariableDefinition& grouped_definition = RequireVariableDefinition(program, 1u, "grouped");
	const MidoriExpression::Binary& grouped_expr = RequireExpression<MidoriExpression::Binary>(grouped_definition.m_value);
	REQUIRE(grouped_expr.m_op.m_token_name == Token::Name::SINGLE_PLUS);
	REQUIRE(RequireLiteral(grouped_expr.m_left, MidoriExpression::LiteralKind::Integer).m_token.m_lexeme == "1");
	const MidoriExpression::Group& grouped_right = RequireExpression<MidoriExpression::Group>(grouped_expr.m_right);
	const MidoriExpression::Binary& multiplied_expr = RequireExpression<MidoriExpression::Binary>(grouped_right.m_expr_in);
	REQUIRE(multiplied_expr.m_op.m_token_name == Token::Name::STAR);
	REQUIRE(RequireLiteral(multiplied_expr.m_left, MidoriExpression::LiteralKind::Integer).m_token.m_lexeme == "2");
	REQUIRE(RequireLiteral(multiplied_expr.m_right, MidoriExpression::LiteralKind::Integer).m_token.m_lexeme == "3");

	const MidoriStatement::VariableDefinition& tuple_definition = RequireVariableDefinition(program, 2u, "tuple_value");
	const MidoriExpression::Tuple& tuple_expr = RequireExpression<MidoriExpression::Tuple>(tuple_definition.m_value);
	REQUIRE(tuple_expr.m_elements.size() == 2u);

	const MidoriExpression::Binary& tuple_first_element = RequireExpression<MidoriExpression::Binary>(tuple_expr.m_elements[0u]);
	REQUIRE(tuple_first_element.m_op.m_token_name == Token::Name::SINGLE_PLUS);
	REQUIRE(RequireLiteral(tuple_first_element.m_left, MidoriExpression::LiteralKind::Integer).m_token.m_lexeme == "1");
	REQUIRE(RequireLiteral(tuple_first_element.m_right, MidoriExpression::LiteralKind::Integer).m_token.m_lexeme == "2");
	REQUIRE(RequireLiteral(tuple_expr.m_elements[1u], MidoriExpression::LiteralKind::Integer).m_token.m_lexeme == "3");

	// The same expression without parentheses is an error that names both operators.
	const std::string mixed_source =
		R"(module ParserMixed
def mixed = 1 + 2 * 3;
)";
	std::expected<MidoriTest::ParsedSnippet, CompilerError> mixed_result = MidoriTest::ParseSnippet(mixed_source, "ParserMixed.mmt");
	REQUIRE_FALSE(mixed_result.has_value());
	const std::string rendered(mixed_result.error().Rendered());
	REQUIRE(rendered.find("do not bind equally") != std::string::npos);
	REQUIRE(rendered.find("'+'") != std::string::npos);
	REQUIRE(rendered.find("'*'") != std::string::npos);
}

TEST_CASE("Parser accepts nested generic closers in type contexts without affecting shift expressions", "[parser]")
{
	const std::string source_code =
		R"(module ParserNestedGenericClosers
class Show<T> {
    show: fn(value: T) -> Text;
};

type Box<T> where Show<T> = {
    value: T
};

instance Show<Array<Array<Text>>> {
    def show = fn(value: Array<Array<Text>>) -> Text => "nested";
};

def nested : Array<Array<Text>> = [["hello"]];
def boxed : Box<Array<Array<Text>>> = Box(nested);
def shift = fn(value: Int, data: Array<Array<Text>>) -> Int where Show<Array<Array<Text>>> => value >> 1;
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "ParserNestedGenericClosers.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	REQUIRE(parse_result->m_program.size() == 6u);

	const MidoriStatement::VariableDefinition& boxed_definition = RequireVariableDefinition(parse_result->m_program, 4u, "boxed");
	const MidoriExpression::Construct& boxed_construct = RequireExpression<MidoriExpression::Construct>(boxed_definition.m_value);
	// `new Box<...>(...)` used to carry the closers on the expression itself. With `new` gone
	// the annotation is where they sit, so the construction is only checked for its shape.
	REQUIRE(boxed_construct.IsConstructTypeOf<MidoriExpression::Construct::Struct>());
	REQUIRE(boxed_construct.m_params.size() == 1u);

	const MidoriExpression::Function& shift_definition = RequireFunctionBinding(parse_result->m_program, 5u, "shift");
	const MidoriExpression::Binary& shift_expr = RequireExpression<MidoriExpression::Binary>(shift_definition.m_body);
	REQUIRE(shift_expr.m_op.m_token_name == Token::Name::RIGHT_SHIFT);
}

TEST_CASE("Parser accepts associated type arguments with nested generic closers", "[parser]")
{
	const std::string source_code =
		R"(module ParserNestedAssociatedTypes
class Iterable<Iter> {
    type Item;
    next: fn(iter: Iter) -> Iterable::Item<Iter>;
};

def NextValue = fn(iter: Array<Array<Int>>) -> Iterable::Item<Array<Array<Int>>> where Iterable<Array<Array<Int>>> => iter;
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "ParserNestedAssociatedTypes.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	REQUIRE(parse_result->m_program.size() == 2u);
}

TEST_CASE("Parser exposes module exports and use imports extracted from the module preamble", "[parser]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile(
			"Helper.mmt",
			R"(module Helper
public export { increment, decrement }
def increment = 1;
def decrement = 0;
)"),
		MidoriTest::TempProjectFile(
			"Main.mmt",
			R"(module Main
public export { local_value }
import { "Helper.mmt" }
use Helper.{increment, decrement}
def local_value = 1;
)")
	});

	const std::filesystem::path main_path = project.Path("Main.mmt");
	const std::string main_source_code =
		R"(module Main
public export { local_value }
import { "Helper.mmt" }
use Helper.{increment, decrement}
def local_value = 1;
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(main_source_code, main_path.string());
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	REQUIRE(parse_result->m_module_declaration.has_value());
	const ModuleDeclaration& module_declaration = parse_result->m_module_declaration.value();
	REQUIRE(module_declaration.ModuleName() == "Main");
	REQUIRE(module_declaration.HasExport("local_value"));
	REQUIRE(module_declaration.GetExportVisibility("local_value") == VisibilityLevel::Public);

	REQUIRE(parse_result->m_use_imports.size() == 2u);
	REQUIRE(parse_result->m_use_imports[0u].m_module_name == "Helper");
	REQUIRE(parse_result->m_use_imports[0u].m_symbol_name == "increment");
	REQUIRE(parse_result->m_use_imports[1u].m_module_name == "Helper");
	REQUIRE(parse_result->m_use_imports[1u].m_symbol_name == "decrement");

	const MidoriProgramTree& program = parse_result->m_program;
	REQUIRE(program.size() == 1u);
	static_cast<void>(RequireVariableDefinition(program, 0u, "local_value"));
}

TEST_CASE("Parser errors preserve parser stage and exact source metadata", "[parser][diagnostic]")
{
	const std::string source_code =
		R"(module ParserFailure
def value = ;
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "ParserFailure.mmt");
	REQUIRE_FALSE(parse_result.has_value());

	const CompilerError& error = parse_result.error();
	RequireErrorMatches(
		error,
		MidoriTest::ErrorExpectation
		{
			.m_stage = CompilerStage::Parser,
			.m_line = 2,
			.m_rendered_substrings = { "Parser Error", "ParserFailure.mmt:2", "def value = ;" }
		});

	REQUIRE(error.m_location.has_value());
	CHECK(error.m_location->m_file_name == "ParserFailure.mmt");
	CHECK(error.m_location->m_line == 2);
	CHECK(error.m_location->m_column == 12);
	CHECK(error.m_location->m_caret_length == 1u);
	CHECK(error.m_location->m_source_line == std::optional<std::string>("def value = ;"));
}

TEST_CASE("Parser reports array comprehension near-miss syntax before name resolution", "[parser][diagnostic]")
{
	SECTION("missing for")
	{
		const std::string source_code =
			R"(module SyntaxMissingFor
def value = [i i in 0..1..10];
)";

		std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "SyntaxMissingFor.mmt");
		REQUIRE_FALSE(parse_result.has_value());
		const CompilerError& error = parse_result.error();
		RequireErrorMatches(
			error,
			MidoriTest::ErrorExpectation
			{
				.m_stage = CompilerStage::Parser,
				.m_line = 2,
				.m_message_substrings = { "Expected 'for' in array comprehension." },
				.m_rendered_substrings = { "SyntaxMissingFor.mmt:2", "def value = [i i in 0..1..10];" }
			});

		CHECK(error.m_message.find("Undefined name.") == std::string::npos);
		REQUIRE(error.m_location.has_value());
		REQUIRE(error.m_location->m_column.has_value());
		CHECK(error.m_location->m_column.value() == 15);
	}

	SECTION("missing in")
	{
		const std::string source_code =
			R"(module SyntaxMissingIn
def value = [i for i 0..1..10];
)";

		std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "SyntaxMissingIn.mmt");
		REQUIRE_FALSE(parse_result.has_value());
		const CompilerError& error = parse_result.error();
		RequireErrorMatches(
			error,
			MidoriTest::ErrorExpectation
			{
				.m_stage = CompilerStage::Parser,
				.m_line = 2,
				.m_message_substrings = { "Expected 'in' after loop variable in array comprehension." },
				.m_rendered_substrings = { "SyntaxMissingIn.mmt:2", "def value = [i for i 0..1..10];" }
			});

		CHECK(error.m_message.find("Undefined name.") == std::string::npos);
		REQUIRE(error.m_location.has_value());
		REQUIRE(error.m_location->m_column.has_value());
		CHECK(error.m_location->m_column.value() == 21);
	}
}

TEST_CASE("Parser keeps arrays whose first element is a for-expression out of the comprehension path", "[parser]")
{
	const std::string source_code =
		R"(module ArrayForLiteral
def value = [for i in 0..1..10 i];
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "ArrayForLiteral.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	const MidoriStatement::VariableDefinition& value_definition = RequireVariableDefinition(parse_result->m_program, 0u, "value");
	const MidoriExpression::Array& array_expr = RequireExpression<MidoriExpression::Array>(value_definition.m_value);
	REQUIRE(array_expr.m_elems.size() == 1u);
	static_cast<void>(RequireExpression<MidoriExpression::For>(array_expr.m_elems[0u]));
}

TEST_CASE("Parser synchronizes consume failures to every top-level declaration starter in Phase 2", "[parser][recovery]")
{
	struct SyncCase
	{
		std::string_view m_name;
		std::string_view m_next_declaration;
		Token::Name m_expected_token;
	};

	const std::vector<SyncCase> cases =
	{
		{ "def", "def next = fn() -> Int => 0;\n", Token::Name::DEF },
		{ "class", "class Next<T> {\n\tproject: fn(value: T) -> T;\n};\n", Token::Name::CLASS },
		{ "instance", "instance Next<Int> {\n\tdef project = fn(value: Int) -> Int => value;\n};\n", Token::Name::INSTANCE },
		{ "foreign", "foreign \"MIDORI_FFI_Next\" NextForeign : fn() -> Int;\n", Token::Name::FOREIGN },
		{ "type", "type Nominal = Int;\n", Token::Name::TYPE },
		{ "alias", "alias Shorthand = Int;\n", Token::Name::ALIAS },
	};

	for (const SyncCase& sync_case : cases)
	{
		SECTION(std::string(sync_case.m_name))
		{
			const std::string source_code = std::format
			(
				R"(module ParserRecovery
def value = 1
{})",
				sync_case.m_next_declaration
			);

			std::expected<std::unique_ptr<PreparedParser>, CompilerError> prepared_result = PrepareParser(source_code, std::format("ParserRecovery_{}.mmt", sync_case.m_name));
			if (!prepared_result.has_value())
			{
				FAIL(std::string(prepared_result.error().Rendered()));
			}

			PreparedParser& prepared = *prepared_result.value();
			MidoriResult::ParserResult parse_result = prepared.m_parser.Parse();
			REQUIRE_FALSE(parse_result.has_value());
			CHECK(parse_result.error().First().m_message.find("Expected ';' after name binding.") != std::string::npos);
			CHECK(ParserTestAccess::CurrentTokenName(prepared.m_parser) == sync_case.m_expected_token);
		}
	}
}

TEST_CASE("Parser recovers limited helper failures at the top-level parse boundary", "[parser][recovery]")
{
	const std::string source_code =
		R"(module ParserRecoveryLimited
def broken = fn(value next) -> Int => value;
class Next<T> {
	project: fn(value: T) -> T;
};
)";

	std::expected<std::unique_ptr<PreparedParser>, CompilerError> prepared_result = PrepareParser(source_code, "ParserRecoveryLimited.mmt");
	if (!prepared_result.has_value())
	{
		FAIL(std::string(prepared_result.error().Rendered()));
	}

	PreparedParser& prepared = *prepared_result.value();
	MidoriResult::ParserResult parse_result = prepared.m_parser.Parse();
	REQUIRE_FALSE(parse_result.has_value());
	CHECK(parse_result.error().First().m_message.find("Expected ',' after function parameter.") != std::string::npos);
	CHECK(ParserTestAccess::CurrentTokenName(prepared.m_parser) == Token::Name::CLASS);
}

TEST_CASE("Parser rejects qualified access to private exports outside the current namespace", "[parser][module]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile(
			"Secrets.mmt",
			R"(module Secrets
private export { hidden }
def hidden = 7;
)"),
		MidoriTest::TempProjectFile(
			"Main.mmt",
			R"(module Main
import { "./Secrets.mmt" }
def value = Secrets::hidden;
def main = fn() -> Int => value;
)")
	});

	const std::filesystem::path main_path = project.Path("Main.mmt");
	const std::string main_source_code =
		R"(module Main
import { "./Secrets.mmt" }
def value = Secrets::hidden;
def main = fn() -> Int => value;
)";

	MidoriResult::CompilerResult compile_result = MidoriTest::CompileSnippet(main_source_code, main_path.string());
	REQUIRE_FALSE(compile_result.has_value());

	const CompilerError& error = compile_result.error().First();
	CHECK(error.m_message.find("private to module 'Secrets'") != std::string::npos);
	CHECK(error.m_message.find("current namespace 'Main'") != std::string::npos);
}

TEST_CASE("Parser resolves dotted use imports against the full module name", "[parser][module]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile
		(
			"MathVector.mmt",
			R"(module Math.Vector
public export { add }
def add = fn() -> Int => 41;
)"
		),
		MidoriTest::TempProjectFile
		(
			"Main.mmt",
			R"(module Main
import { "./MathVector.mmt" }
use Math.Vector.{add}
def main = fn() -> Int => add();
)"
		)
	});

	const std::filesystem::path main_path = project.Path("Main.mmt");
	const std::string main_source_code =
		R"(module Main
import { "./MathVector.mmt" }
use Math.Vector.{add}
def main = fn() -> Int => add();
)";

	MidoriResult::CompilerResult compile_result = MidoriTest::CompileSnippet(main_source_code, main_path.string());
	REQUIRE(compile_result.has_value());
}

TEST_CASE("Parser rejects ambiguous use imports from different modules", "[parser][module]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile
		(
			"Left.mmt",
			R"(module Left
public export { value }
def value = fn() -> Int => 1;
)"
		),
		MidoriTest::TempProjectFile
		(
			"Right.mmt",
			R"(module Right
public export { value }
def value = fn() -> Int => 2;
)"
		),
		MidoriTest::TempProjectFile
		(
			"Main.mmt",
			R"(module Main
import { "./Left.mmt", "./Right.mmt" }
use Left.{value}
use Right.{value}
def main = fn() -> Int => value();
)"
		)
	});

	const std::filesystem::path main_path = project.Path("Main.mmt");
	const std::string main_source_code =
		R"(module Main
import { "./Left.mmt", "./Right.mmt" }
use Left.{value}
use Right.{value}
def main = fn() -> Int => value();
)";

	MidoriResult::CompilerResult compile_result = MidoriTest::CompileSnippet(main_source_code, main_path.string());
	REQUIRE_FALSE(compile_result.has_value());

	const CompilerError& error = compile_result.error().First();
	CHECK(error.m_message.find("Ambiguous use import for symbol 'value'") != std::string::npos);
	CHECK(error.m_message.find("Left::value") != std::string::npos);
}

TEST_CASE("Parser treats duplicate same-module use imports as idempotent", "[parser][module]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile
		(
			"Helper.mmt",
			R"(module Helper
public export { value }
def value = fn() -> Int => 7;
)"
		),
		MidoriTest::TempProjectFile
		(
			"Main.mmt",
			R"(module Main
import { "./Helper.mmt" }
use Helper.{value}
use Helper.{value}
def main = fn() -> Int => value();
)"
		)
	});

	const std::filesystem::path main_path = project.Path("Main.mmt");
	const std::string main_source_code =
		R"(module Main
import { "./Helper.mmt" }
use Helper.{value}
use Helper.{value}
def main = fn() -> Int => value();
)";

	MidoriResult::CompilerResult compile_result = MidoriTest::CompileSnippet(main_source_code, main_path.string());
	REQUIRE(compile_result.has_value());
}

TEST_CASE("Compiler uses the declared entry module name for linked executable metadata", "[parser][module]")
{
	const std::string source_code =
		R"(module App.Main
def main = fn() -> Int => 0;
)";

	MidoriResult::CompilerResult compile_result = MidoriTest::CompileSnippet(source_code, "EntryPoint.mmt");
	REQUIRE(compile_result.has_value());

	const MidoriExecutable& executable = compile_result.value();
	CHECK(std::filesystem::path(executable.GetFileName()).filename() == "EntryPoint.mmt");
	REQUIRE_FALSE(executable.m_procedure_names.empty());
	CHECK(std::string(executable.m_procedure_names[0u].c_str()) == std::format("{}@{}", MODULE_BOOTSTRAP_PREFIX, "App.Main"));
}

TEST_CASE("Parser builds constructor and wildcard match patterns without brittle tree snapshots", "[parser]")
{
	const std::string source_code =
		R"(module ParserMatch
type Option = None | Some(Int);
def result = match Option::Some(7) with
	case Option::Some(_) => 1
	case Option::None() => 0
;
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "ParserMatch.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	const MidoriProgramTree& program = parse_result->m_program;
	REQUIRE(program.size() == 2u);

	const MidoriStatement::VariableDefinition& result_definition = RequireVariableDefinition(program, 1u, "result");
	const MidoriExpression::Match& match_expr = RequireExpression<MidoriExpression::Match>(result_definition.m_value);
	REQUIRE(match_expr.m_cases.size() == 2u);

	const MidoriExpression::Case& some_case = RequireExpression<MidoriExpression::Case>(match_expr.m_cases[0u]);
	const MidoriPattern::Constructor& some_pattern = RequirePattern<MidoriPattern::Constructor>(some_case.m_pattern);
	REQUIRE(some_pattern.m_args.size() == 1u);
	static_cast<void>(RequirePattern<MidoriPattern::Wildcard>(some_pattern.m_args[0u]));
	REQUIRE(RequireLiteral(some_case.m_expr, MidoriExpression::LiteralKind::Integer).m_token.m_lexeme == "1");

	const MidoriExpression::Case& none_case = RequireExpression<MidoriExpression::Case>(match_expr.m_cases[1u]);
	const MidoriPattern::Constructor& none_pattern = RequirePattern<MidoriPattern::Constructor>(none_case.m_pattern);
	REQUIRE(none_pattern.m_args.empty());
	REQUIRE(RequireLiteral(none_case.m_expr, MidoriExpression::LiteralKind::Integer).m_token.m_lexeme == "0");
}

TEST_CASE("Parser tells a record update apart from a block", "[parser]")
{
	// '{' opens a block. The record-update probe scans at depth 0 for the first ';', '}'
	// or 'with'. The pending-match counter is what keeps `{ match x with case ... }` a
	// block: simulating the probe over the whole .mmt corpus misclassifies 0 braces with
	// that counter and 51 without it. These cases pin that behaviour.
	const std::string source_code =
		R"(module ParserRecordUpdate
type Option = Some(Int) | None;
type Point =
{
	x : Int,
	y : Int
};
def p = Point(1, 2);
def updated = { p with x = 5, y = 6 };
def block_with_match = { match p.x with case _ => 1 };
def plain_block = { def local = 1; local };
def block_expression_only = { p.x };
def nested_source = { { p with x = 1 } with y = 2 };
def projected = { p with x = 9 }.x;
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "ParserRecordUpdate.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	const MidoriProgramTree& program = parse_result->m_program;

	const MidoriStatement::VariableDefinition& updated_definition = RequireVariableDefinition(program, 3u, "updated");
	const MidoriExpression::RecordUpdate& updated_expr = RequireExpression<MidoriExpression::RecordUpdate>(updated_definition.m_value);
	REQUIRE(updated_expr.m_updates.size() == 2u);
	REQUIRE(updated_expr.m_updates[0u].m_name.m_lexeme == "x");
	REQUIRE(updated_expr.m_updates[1u].m_name.m_lexeme == "y");
	static_cast<void>(RequireExpression<MidoriExpression::NameAccess>(updated_expr.m_source));

	// A bare `match ... with` inside braces is still a block, not a record update.
	const MidoriStatement::VariableDefinition& match_definition = RequireVariableDefinition(program, 4u, "block_with_match");
	static_cast<void>(RequireExpression<MidoriExpression::Block>(match_definition.m_value));

	const MidoriStatement::VariableDefinition& plain_definition = RequireVariableDefinition(program, 5u, "plain_block");
	static_cast<void>(RequireExpression<MidoriExpression::Block>(plain_definition.m_value));

	const MidoriStatement::VariableDefinition& expression_only_definition = RequireVariableDefinition(program, 6u, "block_expression_only");
	static_cast<void>(RequireExpression<MidoriExpression::Block>(expression_only_definition.m_value));

	// A record update may itself be the source of another.
	const MidoriStatement::VariableDefinition& nested_definition = RequireVariableDefinition(program, 7u, "nested_source");
	const MidoriExpression::RecordUpdate& nested_expr = RequireExpression<MidoriExpression::RecordUpdate>(nested_definition.m_value);
	static_cast<void>(RequireExpression<MidoriExpression::RecordUpdate>(nested_expr.m_source));

	// A record update is a value, so the postfix chain still applies to it.
	const MidoriStatement::VariableDefinition& projected_definition = RequireVariableDefinition(program, 8u, "projected");
	const MidoriExpression::MemberAccess& projected_expr = RequireExpression<MidoriExpression::MemberAccess>(projected_definition.m_value);
	static_cast<void>(RequireExpression<MidoriExpression::RecordUpdate>(projected_expr.m_struct));
}

TEST_CASE("Parser keeps a record update in function body position out of the block path", "[parser]")
{
	// The function-body brace is a second, separate '{' dispatch site that bypasses
	// ParsePrimary. Patching only ParsePrimary would leave `fn ... => { c with ... }`
	// parsing as a block, which is precisely the builder-style form record update exists
	// for. A record update there also continues through the postfix chain; a block
	// deliberately does not, so that `fn() => {}()` still parses its call separately.
	const std::string source_code =
		R"(module ParserRecordUpdateBody
type Config =
{
	host : Text,
	port : Int
};
def WithPort = fn(c : Config, p : Int) -> Config => { c with port = p };
def PortOf = fn(c : Config) -> Int => { c with port = 1 }.port;
def BlockBodied = fn() -> Int => { def local = 2; local };
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "ParserRecordUpdateBody.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	const MidoriProgramTree& program = parse_result->m_program;

	REQUIRE(program.size() >= 4u);
	const MidoriExpression::Function& with_port = RequireFunctionBinding(program, 1u, "WithPort");
	const MidoriExpression::RecordUpdate& with_port_body = RequireExpression<MidoriExpression::RecordUpdate>(with_port.m_body);
	REQUIRE(with_port_body.m_updates.size() == 1u);
	REQUIRE(with_port_body.m_updates[0u].m_name.m_lexeme == "port");

	const MidoriExpression::Function& port_of = RequireFunctionBinding(program, 2u, "PortOf");
	const MidoriExpression::MemberAccess& port_of_body = RequireExpression<MidoriExpression::MemberAccess>(port_of.m_body);
	static_cast<void>(RequireExpression<MidoriExpression::RecordUpdate>(port_of_body.m_struct));

	const MidoriExpression::Function& block_bodied = RequireFunctionBinding(program, 3u, "BlockBodied");
	static_cast<void>(RequireExpression<MidoriExpression::Block>(block_bodied.m_body));
}

TEST_CASE("Parser lowers a type record onto a Struct node", "[parser]")
{
	// The whole point of the record form is that it is not a new node: it lowers onto
	// the Struct statement and StructType the rest of the pipeline already handles,
	// which is what let the type checker, code generator and VM stay untouched. This
	// used to be pinned by parsing an equivalent `struct` beside it and comparing the
	// two; `struct` is gone, so the expected shape is written out instead.
	const std::string source_code =
		R"(module TypeRecordLowering
type FromType = { x: Int, y: Text };
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "TypeRecordLowering.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	const MidoriProgramTree& program = parse_result->m_program;
	REQUIRE(program.size() == 1u);

	REQUIRE(program[0u] != nullptr);
	REQUIRE(program[0u]->IsStatement<MidoriStatement::Struct>());

	const MidoriStatement::Struct& from_type = program[0u]->GetStatement<MidoriStatement::Struct>();

	REQUIRE(from_type.m_name.m_lexeme == "FromType");
	REQUIRE(from_type.m_self_type->IsType<MidoriType::StructType>());

	const MidoriType::StructType& type_type = from_type.m_self_type->GetType<MidoriType::StructType>();

	REQUIRE(type_type.m_member_names == std::vector<std::string>{ "x", "y" });
	REQUIRE(type_type.m_member_types.size() == 2u);
	REQUIRE(type_type.m_member_types[0u]->IsType<MidoriType::IntegerType>());
	REQUIRE(type_type.m_member_types[1u]->IsType<MidoriType::TextType>());
	REQUIRE(type_type.m_generic_params.empty());
	REQUIRE(from_type.m_generic_params.empty());
}

TEST_CASE("Parser lowers a type sum onto a Union node", "[parser]")
{
	// As with the record form, the sum form is not a new node: it lowers onto the
	// Union statement and UnionType. Pinned against the expected shape now that the
	// `union` spelling it used to be compared against is gone.
	const std::string source_code =
		R"(module TypeSumLowering
type FromType<T> = Empty | Full(T);
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "TypeSumLowering.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	const MidoriProgramTree& program = parse_result->m_program;
	REQUIRE(program.size() == 1u);

	REQUIRE(program[0u] != nullptr);
	REQUIRE(program[0u]->IsStatement<MidoriStatement::Union>());

	const MidoriStatement::Union& from_type = program[0u]->GetStatement<MidoriStatement::Union>();

	REQUIRE(from_type.m_name.m_lexeme == "FromType");

	// Generic parameters flow through the shared declaration prologue.
	REQUIRE(from_type.m_generic_params.size() == 1u);
	REQUIRE(from_type.m_generic_params[0u].m_lexeme == "T");
	REQUIRE(from_type.m_constructor_names.size() == 2u);

	REQUIRE(from_type.m_self_type->IsType<MidoriType::UnionType>());
	const MidoriType::UnionType& type_type = from_type.m_self_type->GetType<MidoriType::UnionType>();

	REQUIRE(type_type.m_member_info.size() == 2u);
	REQUIRE(type_type.m_generic_params.size() == 1u);

	// Variants are namespaced under the type's own name, so index by the suffix and
	// pin the tag order and arity the declaration produces.
	const auto variant_suffix = [](const std::string& qualified_name) -> std::string
	{
		const size_t separator = qualified_name.rfind(':');
		return separator == std::string::npos ? qualified_name : qualified_name.substr(separator + 1u);
	};

	std::unordered_map<std::string, std::pair<int, size_t>> tags_by_variant;
	for (const std::pair<const std::string, MidoriType::UnionType::UnionMemberContext>& entry : type_type.m_member_info)
	{
		tags_by_variant.emplace(variant_suffix(entry.first), std::make_pair(entry.second.m_tag, entry.second.m_member_types.size()));
	}

	const std::unordered_map<std::string, std::pair<int, size_t>> expected_variants
	{
		{ "Empty", std::make_pair(0, size_t{ 0u }) },
		{ "Full", std::make_pair(1, size_t{ 1u }) },
	};

	REQUIRE(tags_by_variant == expected_variants);
}

TEST_CASE("Parser lowers a bare type name onto a nominal newtype", "[parser]")
{
	const std::string source_code =
		R"(module NewtypeLowering
type Meters = Int;
alias Feet = Int;
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "NewtypeLowering.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	const MidoriProgramTree& program = parse_result->m_program;
	REQUIRE(program.size() == 2u);

	REQUIRE(program[0u]->IsStatement<MidoriStatement::TypeAlias>());
	REQUIRE(program[1u]->IsStatement<MidoriStatement::TypeAlias>());

	// Same node, opposite semantics: the newtype carries a NewType, the alias
	// carries the bare representation. That difference is the feature.
	const MidoriStatement::TypeAlias& newtype = program[0u]->GetStatement<MidoriStatement::TypeAlias>();
	const MidoriStatement::TypeAlias& alias = program[1u]->GetStatement<MidoriStatement::TypeAlias>();

	REQUIRE(newtype.m_name.m_lexeme == "Meters");
	REQUIRE(newtype.m_aliased_type->IsType<MidoriType::NewType>());
	REQUIRE(newtype.m_aliased_type->GetType<MidoriType::NewType>().m_representation->IsType<MidoriType::IntegerType>());

	REQUIRE(alias.m_name.m_lexeme == "Feet");
	REQUIRE(alias.m_aliased_type->IsType<MidoriType::IntegerType>());
}

TEST_CASE("Parser still reads a leading-bar single-variant sum as a union", "[parser]")
{
	const std::string source_code =
		R"(module LeadingBarSum
type Solo = | Only(Int);
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "LeadingBarSum.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	const MidoriProgramTree& program = parse_result->m_program;
	REQUIRE(program.size() == 1u);
	REQUIRE(program[0u]->IsStatement<MidoriStatement::Union>());
	REQUIRE(program[0u]->GetStatement<MidoriStatement::Union>().m_name.m_lexeme == "Solo");
}

TEST_CASE("A type binding inside a class body is still an associated type", "[parser]")
{
	// `type Item ...` here is the same syntax as a top-level newtype. The two
	// readings are kept apart by parse position, not by syntax, so this pins the
	// boundary rather than trusting it.
	const std::string source_code =
		R"(module AssociatedTypeBoundary
class Container<T> {
	type Item;
	First: fn(value: T) -> Item;
};
type Meters = Int;
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "AssociatedTypeBoundary.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	const MidoriProgramTree& program = parse_result->m_program;
	REQUIRE(program.size() == 2u);

	// The class body's `type Item` stayed inside the class and produced no
	// top-level declaration of its own.
	REQUIRE(program[0u]->IsStatement<MidoriStatement::Class>());
	REQUIRE(program[1u]->IsStatement<MidoriStatement::TypeAlias>());
	REQUIRE(program[1u]->GetStatement<MidoriStatement::TypeAlias>().m_aliased_type->IsType<MidoriType::NewType>());
}

TEST_CASE("A type binding inside an instance body is still an associated type binding", "[parser]")
{
	// The 9 real associated-type bindings in the codebase are all instance-body
	// bindings (`type Item = Int;`), which reads identically, as source syntax, to
	// a top-level newtype declaration. This is the closer analogue to a top-level
	// declaration than the class-body form above, since the class body only
	// declares the associated type name without a representation.
	const std::string source_code =
		R"(module InstanceAssociatedTypeBoundary
class Container<T> {
	type Item;
	First: fn(value: T) -> Item;
};
instance Container<Int> {
	type Item = Int;

	def First = fn(value: Int) -> Int => value;
};
type Meters = Int;
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "InstanceAssociatedTypeBoundary.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	const MidoriProgramTree& program = parse_result->m_program;
	REQUIRE(program.size() == 3u);

	REQUIRE(program[0u]->IsStatement<MidoriStatement::Class>());
	REQUIRE(program[1u]->IsStatement<MidoriStatement::Instance>());
	REQUIRE(program[2u]->IsStatement<MidoriStatement::TypeAlias>());

	// The instance body's `type Item = Int;` stayed inside the instance and bound
	// a plain Int, not a NewType - it never reached top-level dispatch.
	const MidoriStatement::Instance& instance = program[1u]->GetStatement<MidoriStatement::Instance>();
	REQUIRE(instance.m_associated_types.size() == 1u);
	REQUIRE(instance.m_associated_types[0u].m_name.m_lexeme == "Item");
	REQUIRE(instance.m_associated_types[0u].m_type->IsType<MidoriType::IntegerType>());

	REQUIRE(program[2u]->GetStatement<MidoriStatement::TypeAlias>().m_aliased_type->IsType<MidoriType::NewType>());
}

TEST_CASE("Parser records the parameters a parameterised alias binds", "[parser]")
{
	// The expansion's own m_generic_params are cleared by substitution, so the
	// parameters have to survive on the alias itself for the use site to find.
	const std::string source_code =
		R"(module ParameterisedAlias
type Pair<A, B> =
{
	first: A,
	second: B
};
alias IntKeyed<V> = Pair<Int, V>;
def keyed : IntKeyed<Text> = Pair(1, "one");
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "ParameterisedAlias.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	const MidoriProgramTree& program = parse_result->m_program;
	REQUIRE(program.size() == 3u);
	REQUIRE(program[1u] != nullptr);
	REQUIRE(program[1u]->IsStatement<MidoriStatement::TypeAlias>());

	const MidoriStatement::TypeAlias& alias = program[1u]->GetStatement<MidoriStatement::TypeAlias>();
	REQUIRE(alias.m_name.m_lexeme == "IntKeyed");
	REQUIRE(alias.m_generic_params.size() == 1u);
	REQUIRE(alias.m_generic_params[0u].m_lexeme == "V");

	// The stored expansion is the template a use site substitutes into: Pair's own
	// parameters are gone, and its arguments read Int and the alias's V.
	REQUIRE(alias.m_aliased_type->IsType<MidoriType::StructType>());

	const MidoriType::StructType& expansion = alias.m_aliased_type->GetType<MidoriType::StructType>();
	REQUIRE(expansion.m_name == "Pair");
	REQUIRE(expansion.m_generic_params.empty());
	REQUIRE(expansion.m_is_generic_instantiation);
	REQUIRE(expansion.m_type_arguments.size() == 2u);
	REQUIRE(expansion.m_type_arguments[0u]->IsType<MidoriType::IntegerType>());
	REQUIRE(expansion.m_type_arguments[1u]->IsType<MidoriType::GenericParam>());
	REQUIRE(expansion.m_type_arguments[1u]->GetType<MidoriType::GenericParam>().m_name == "V");
}

TEST_CASE("Parser applies a parameterised alias positionally, not by the expansion's parameter names", "[parser]")
{
	// Swapped<A, B> must reach Pair<B, A>. Reusing Pair's names would make this
	// pass for the wrong reason, so the alias deliberately reverses them.
	const std::string source_code =
		R"(module SwappedAlias
type Pair<A, B> =
{
	first: A,
	second: B
};
alias Swapped<A, B> = Pair<B, A>;
def swapped : Swapped<Int, Text> = Pair("one", 2);
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "SwappedAlias.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	const MidoriProgramTree& program = parse_result->m_program;
	REQUIRE(program.size() == 3u);
	REQUIRE(program[2u] != nullptr);
	REQUIRE(program[2u]->IsStatement<MidoriStatement::VariableDefinition>());

	const MidoriStatement::VariableDefinition& definition = program[2u]->GetStatement<MidoriStatement::VariableDefinition>();
	REQUIRE(definition.m_annotated_type.has_value());
	REQUIRE(definition.m_annotated_type.value()->IsType<MidoriType::StructType>());

	const MidoriType::StructType& annotated = definition.m_annotated_type.value()->GetType<MidoriType::StructType>();
	REQUIRE(annotated.m_type_arguments.size() == 2u);
	REQUIRE(annotated.m_type_arguments[0u]->IsType<MidoriType::TextType>());
	REQUIRE(annotated.m_type_arguments[1u]->IsType<MidoriType::IntegerType>());
}

TEST_CASE("Parser rejects a 'where' constraint on an alias", "[parser][diagnostic]")
{
	// An alias is transparent, so a constraint written on it has nothing left to
	// attach to by the time the expansion is checked.
	const std::string source_code =
		R"(module ConstrainedAlias
type Box<T> =
{
	item: T
};
alias BoxAlias<T> where Show<T> = Box<T>;
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "ConstrainedAlias.mmt");
	REQUIRE_FALSE(parse_result.has_value());

	RequireErrorMatches(
		parse_result.error(),
		MidoriTest::ErrorExpectation
		{
			.m_stage = CompilerStage::Parser,
			.m_line = 6,
			.m_rendered_substrings = { "Alias declarations cannot carry 'where' constraints" }
		});
}

TEST_CASE("Parser accepts an alias of an instantiated generic type", "[parser]")
{
	// The restriction is on alias parameters, not on aliasing a parameterised
	// type that has already been given its arguments.
	const std::string source_code =
		R"(module InstantiatedAlias
type Box<T> =
{
	item: T
};
alias IntBox = Box<Int>;
def boxed : IntBox = Box(1);
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "InstantiatedAlias.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	const MidoriProgramTree& program = parse_result->m_program;
	REQUIRE(program.size() == 3u);
	REQUIRE(program[1u] != nullptr);
	REQUIRE(program[1u]->IsStatement<MidoriStatement::TypeAlias>());

	const MidoriStatement::TypeAlias& alias = program[1u]->GetStatement<MidoriStatement::TypeAlias>();
	REQUIRE(alias.m_name.m_lexeme == "IntBox");
	REQUIRE(alias.m_generic_params.empty());
	REQUIRE(alias.m_aliased_type->IsType<MidoriType::StructType>());
}

TEST_CASE("Parser accepts only '->' in return position", "[parser]")
{
	const std::string source_code =
		R"(module ReturnSeparator
def Named = fn(x: Int) -> Int => x;
def lambda = fn(x: Int) -> Int => x;
def inferred = fn(x) => x;
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "ReturnSeparator.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	const MidoriProgramTree& program = parse_result->m_program;
	REQUIRE(program.size() == 3u);

	const MidoriExpression::Function& named = RequireFunctionBinding(program, 0u, "Named");
	REQUIRE(named.m_return_type->ToString() == "Int");

	const MidoriStatement::VariableDefinition& binding = RequireVariableDefinition(program, 1u, "lambda");
	const MidoriExpression::Function& lambda = RequireExpression<MidoriExpression::Function>(binding.m_value);
	REQUIRE(lambda.m_return_type->ToString() == "Int");

	// The return type is still optional. Only its ':' spelling went.
	const MidoriStatement::VariableDefinition& inferred_binding = RequireVariableDefinition(program, 2u, "inferred");
	const MidoriExpression::Function& inferred = RequireExpression<MidoriExpression::Function>(inferred_binding.m_value);
	REQUIRE(inferred.m_params.size() == 1u);
	REQUIRE(inferred.m_return_type->ToString() != "Int");
}

TEST_CASE("Parser names the removal for ':' in return position", "[parser][diagnostic]")
{
	SECTION("function expression")
	{
		const std::string source_code =
			R"(module ColonReturnRemoved
def Twice = fn(x: Int) : Int => x * 2;
)";

		std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "ColonReturnRemoved.mmt");
		REQUIRE_FALSE(parse_result.has_value());

		const CompilerError& error = parse_result.error();
		RequireErrorMatches(
			error,
			MidoriTest::ErrorExpectation
			{
				.m_stage = CompilerStage::Parser,
				.m_line = 2,
				.m_message_substrings = { "':' is no longer supported in return position. Write '-> Type' instead." },
				.m_rendered_substrings = { "ColonReturnRemoved.mmt:2" }
			});

		// The old spelling must not degrade into a report about the missing body.
		CHECK(error.m_message.find("Expected '=>'") == std::string::npos);
	}

	SECTION("instance method definition")
	{
		const std::string source_code =
			R"(module ColonReturnRemovedInstance
class Show<T> {
	show: fn(value: T) -> Text;
};
instance Show<Int> {
	def show = fn(value: Int) : Text => value as Text;
};
)";

		std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "ColonReturnRemovedInstance.mmt");
		REQUIRE_FALSE(parse_result.has_value());

		const CompilerError& error = parse_result.error();
		RequireErrorMatches(
			error,
			MidoriTest::ErrorExpectation
			{
				.m_stage = CompilerStage::Parser,
				.m_line = 6,
				.m_message_substrings = { "':' is no longer supported in return position. Write '-> Type' instead." },
				.m_rendered_substrings = { "ColonReturnRemovedInstance.mmt:6" }
			});
	}
}

TEST_CASE("Parser leaves ':' alone wherever it ascribes a type to a name", "[parser]")
{
	// The removal diagnostic fires on a ':' that follows a parameter list. Every other
	// ':' in the language ascribes a type to a name and is consumed by a different site,
	// so none of these may reach it. This test fails if that diagnostic over-fires.
	const std::string source_code =
		R"(module ColonAscription
type Config =
{
	host: Text,
	port: Int
};
class Show<T> {
	show: fn(value: T) -> Text;
};
foreign "MIDORI_FFI_Now" Now : fn() -> Int;
def count : Int = 5;
def predicate : fn(Int) -> Bool = fn(x: Int) -> Bool => x > 0;
def Describe = fn(config: Config, label: Text) -> Text => label ++ config.host;
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "ColonAscription.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	const MidoriProgramTree& program = parse_result->m_program;

	const MidoriStatement::VariableDefinition& count = RequireVariableDefinition(program, 3u, "count");
	REQUIRE(count.m_annotated_type.has_value());
	REQUIRE(count.m_annotated_type.value()->ToString() == "Int");

	const MidoriStatement::VariableDefinition& predicate = RequireVariableDefinition(program, 4u, "predicate");
	REQUIRE(predicate.m_annotated_type.has_value());
	REQUIRE(predicate.m_annotated_type.value()->IsType<MidoriType::FunctionType>());

	const MidoriExpression::Function& describe = RequireFunctionBinding(program, 5u, "Describe");
	REQUIRE(describe.m_params.size() == 2u);
	REQUIRE(describe.m_return_type->ToString() == "Text");
}

TEST_CASE("Parser separates a function-type annotation from an arrow return type", "[parser]")
{
	// `->` inside the annotation belongs to the function type; the second `->`
	// is the lambda's own return position. They are different positions and the
	// parser must not confuse one for the other.
	const std::string source_code =
		R"(module ArrowAnnotation
def predicate : fn(Int) -> Bool = fn(x: Int) -> Bool => x > 0;
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "ArrowAnnotation.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	const MidoriStatement::VariableDefinition& definition = RequireVariableDefinition(parse_result->m_program, 0u, "predicate");
	REQUIRE(definition.m_annotated_type.has_value());
	REQUIRE(definition.m_annotated_type.value()->IsType<MidoriType::FunctionType>());

	const MidoriExpression::Function& lambda = RequireExpression<MidoriExpression::Function>(definition.m_value);
	REQUIRE(lambda.m_return_type->ToString() == "Bool");
}

TEST_CASE("Parser keeps '->' available as the channel send operator", "[parser]")
{
	const std::string source_code =
		R"(module ArrowSend
def Send = fn(c: Channel<Text>) -> Int => { c -> "value"; 0 };
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "ArrowSend.mmt");
	if (!parse_result.has_value())
	{
		FAIL(std::string(parse_result.error().Rendered()));
	}

	const MidoriProgramTree& program = parse_result->m_program;
	REQUIRE(program.size() == 1u);

	const MidoriExpression::Function& send_definition = RequireFunctionBinding(program, 0u, "Send");
	REQUIRE(send_definition.m_return_type->ToString() == "Int");

	const MidoriExpression::Block& body = RequireExpression<MidoriExpression::Block>(send_definition.m_body);
	REQUIRE(!body.m_stmts.empty());
}
