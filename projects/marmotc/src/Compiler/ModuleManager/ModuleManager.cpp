#include "ModuleManager.h"
#include "Compiler/Error/CompilerError.h"
#include "Compiler/ImportResolver/ImportResolver.h"
#include "Compiler/Lexer/Lexer.h"
#include "Compiler/Source/Source.h"
#include "Compiler/Token/Token.h"
#include "Support/Expected.h"
#include "Support/Optional.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <sstream>
#include <unordered_map>

using namespace std::string_literals;

namespace
{
	// `A -> B -> A`, by module name, pointing at the import that closes it.
	CompilerError ImportCycleError(const BuildGraph& build_graph, std::ranges::subrange<std::vector<std::string>::const_iterator> cycle, const Token& import_token, const BuildGraph::BuildNode& importer)
	{
		const std::string chain = cycle
			| std::views::transform([&build_graph](const std::string& file_path) { return build_graph.m_module_declarations.at(file_path).ModuleName(); })
			| std::views::join_with(std::string_view(" -> "))
			| std::ranges::to<std::string>();
		return CompilerError::WithToken
		(
			CompilerStage::Module,
			std::format("Import cycle: {} -> {}.", chain, build_graph.m_module_declarations.at(cycle.front()).ModuleName()),
			import_token,
			importer.m_file_name,
			importer.m_source_lines,
			"A module cannot import itself, directly or through the modules it imports.",
			CompilerErrorCode::ModuleCircularDependency
		);
	}

	std::string JoinDottedSegments(const std::vector<std::string>& segments, size_t count)
	{
		return segments | std::views::take(count) | std::views::join_with('.') | std::ranges::to<std::string>();
	}
}

struct ModuleManager::DiscoveryState
{
	const CompilationInputs& m_inputs;
	BuildGraph m_build_graph;
	std::vector<std::string> m_active_modules;
	// Report a cycle after finishing the imports of the module it leads back to.
	std::unordered_map<std::string, CompilerError> m_cycles;

	explicit DiscoveryState(const CompilationInputs& inputs)
		: m_inputs(inputs)
	{
	}
};

ModuleManager::ModuleManager(TokenStream tokens, std::string_view file_name, std::vector<std::string> source_lines)
	: m_tokens(std::move(tokens)),
	m_file_name(file_name),
	m_source_lines(std::move(source_lines))
{
}

MarmotResult::ModuleManagerResult ModuleManager::GenerateBuildGraph(TokenStream tokens, std::string_view file_name, std::vector<std::string> source_lines, const CompilationInputs& inputs)
{
	DiscoveryState discovery(inputs);
	MarmotResult::VoidResult result = ModuleManager(std::move(tokens), file_name, std::move(source_lines)).DiscoverModule(discovery);
	if (!result.HasValue())
	{
		return MarmotCompiler::Failure(std::move(result.Error()));
	}

	return std::move(discovery.m_build_graph);
}

MarmotResult::Result<ModuleManager> ModuleManager::LoadImportedModule(const std::string& file_path, const CompilationInputs& inputs, int import_line) const
{
	std::string source;
	if (const std::string* overlay = inputs.SourceOverlay(file_path); overlay != nullptr)
	{
		source = *overlay;
	}
	else
	{
		std::ifstream file(file_path);
		// Linux opens a directory as a stream that reads nothing.
		if (std::filesystem::is_directory(file_path) || !file.is_open())
		{
			return MarmotCompiler::Failure(MarmotError::GenerateModuleErrorWithContext(
				CompilerErrorCode::ModuleImportFileOpenFailed, "Could not open import file: " + file_path, import_line, m_file_name));
		}
		std::ostringstream buffer;
		buffer << file.rdbuf();
		source = std::move(buffer).str();
	}

	MarmotSource::RemoveByteOrderMark(source);
	std::vector<std::string> source_lines;
	std::istringstream stream(source);
	std::string line;
	while (std::getline(stream, line))
	{
		source_lines.push_back(std::move(line));
	}
	MarmotResult::LexerResult lexed = Lexer(std::move(source), file_path).Lex();
	if (!lexed.HasValue())
	{
		return MarmotCompiler::Failure(std::move(lexed.Error()));
	}
	return ModuleManager(std::move(lexed).Value(), file_path, std::move(source_lines));
}

MarmotResult::VoidResult ModuleManager::DiscoverModule(DiscoveryState& discovery)
{
	BuildGraph& build_graph = discovery.m_build_graph;
	if (m_tokens.Size() != 0)
	{
		const std::vector<StatementSpan> spans = ScanModuleStatements(m_tokens);

		MarmotResult::Result<ModuleDeclaration> module_result = ExtractModuleDeclaration(m_tokens, spans);
		if (!module_result.HasValue())
		{
			return MarmotCompiler::Failure(std::move(module_result.Error()));
		}

		const std::string& module_name = module_result->ModuleName();
		const auto [module_file, inserted] = build_graph.m_module_name_to_file.try_emplace(module_name, m_file_name);
		if (!inserted && module_file->second != m_file_name)
		{
			return MarmotCompiler::Failure
			(
				MarmotError::GenerateModuleErrorWithContext
				(
					CompilerErrorCode::ModuleDeclarationDuplicate,
					std::format
					(
						"Duplicate module declaration: '{}' is declared in multiple files:\n  First:  {}\n  Second: {}",
						module_name,
						module_file->second,
						m_file_name
					),
					1,
					m_file_name
				)
			);
		}
		build_graph.m_module_declarations.emplace(m_file_name, std::move(module_result).Value());

		MarmotResult::Result<std::vector<SourceImport>> import_result = ExtractImports(m_tokens, spans);
		if (!import_result.HasValue())
		{
			return MarmotCompiler::Failure(std::move(import_result.Error()));
		}

		const std::vector<SourceImport>& imports = import_result.Value();
		MarmotResult::Result<std::vector<UseImport>> use_import_result = ExtractUseStatements(m_tokens, spans);
		if (!use_import_result.HasValue())
		{
			return MarmotCompiler::Failure(std::move(use_import_result.Error()));
		}
		std::vector<UseImport> use_imports = std::move(use_import_result.Value());

		for (const StatementSpan& span : spans | std::views::reverse)
		{
			m_tokens.Erase(m_tokens.begin() + span.m_start, m_tokens.begin() + span.m_end);
		}

		BuildGraph::BuildNode& node = build_graph.m_nodes[m_file_name];
		node.m_tokens = std::move(m_tokens);
		node.m_file_name = m_file_name;
		node.m_source_lines = std::move(m_source_lines);
		node.m_use_imports = std::move(use_imports);
		discovery.m_active_modules.push_back(m_file_name);

		const ImportResolver resolver(m_file_name, discovery.m_inputs.SearchPaths());
		for (const SourceImport& imported : imports)
		{
			const Token& import_token = imported.m_token;
			const bool is_system_import = import_token.m_token_name == Token::Name::LEFT_ANGLE;
			const MarmotCompiler::Optional<std::string> resolved_opt = is_system_import
				? resolver.ResolveSystemImport(imported.m_specifier)
				: resolver.ResolvePathImport(imported.m_specifier);
			if (!resolved_opt.HasValue())
			{
				const std::string specifier = is_system_import ? "<"s + imported.m_specifier + ">"s : imported.m_specifier;
				return MarmotCompiler::Failure(CompilerError::WithToken(CompilerStage::Module, "Could not resolve import: "s + specifier, import_token, m_file_name, node.m_source_lines, MarmotCompiler::None, CompilerErrorCode::ModuleImportResolutionFailed));
			}

			const std::string& include_absolute_path_str = resolved_opt.Value();

			if (node.m_import_tokens.try_emplace(include_absolute_path_str, import_token).second)
			{
				node.m_dependencies.emplace_back(include_absolute_path_str);
			}

			if (build_graph.m_nodes.contains(include_absolute_path_str))
			{
				const std::vector<std::string>::const_iterator cycle_start = std::ranges::find(discovery.m_active_modules, include_absolute_path_str);
				if (cycle_start != discovery.m_active_modules.cend())
				{
					discovery.m_cycles.emplace(include_absolute_path_str, ImportCycleError(build_graph, std::ranges::subrange(cycle_start, discovery.m_active_modules.cend()), import_token, node));
				}
				continue;
			}

			MarmotResult::Result<ModuleManager> imported_module = LoadImportedModule(include_absolute_path_str, discovery.m_inputs, import_token.m_line);
			if (!imported_module.HasValue())
			{
				return MarmotCompiler::Failure(std::move(imported_module.Error()));
			}

			MarmotResult::VoidResult nested_build_graph_result = imported_module->DiscoverModule(discovery);
			if (!nested_build_graph_result.HasValue())
			{
				return MarmotCompiler::Failure(std::move(nested_build_graph_result.Error()));
			}
		}
		discovery.m_active_modules.pop_back();
	}

	// Finish the owning module before reporting its cycle, so errors in its
	// remaining imports keep their original precedence and diagnostic location.
	const std::unordered_map<std::string, CompilerError>::const_iterator cycle = discovery.m_cycles.find(m_file_name);
	if (cycle != discovery.m_cycles.cend())
	{
		return MarmotCompiler::Failure(cycle->second);
	}

	return {};
}

void ModuleManager::SkipWhiteSpace(const TokenStream& tokens, int& current_index)
{
	while (current_index < tokens.Size() && tokens[current_index].m_token_name == Token::Name::WHITESPACE)
	{
		current_index += 1;
	}
};

bool ModuleManager::SkipBracedBlock(const TokenStream& tokens, int& current_index)
{
	if (current_index >= tokens.Size() || tokens[current_index].m_token_name != Token::Name::LEFT_BRACE)
	{
		return false;
	}

	current_index += 1;
	int depth = 1;
	while (current_index < tokens.Size() && depth > 0)
	{
		if (tokens[current_index].m_token_name == Token::Name::LEFT_BRACE)
		{
			depth += 1;
		}
		else if (tokens[current_index].m_token_name == Token::Name::RIGHT_BRACE)
		{
			depth -= 1;
		}
		current_index += 1;
	}
	return true;
}

std::vector<ModuleManager::StatementSpan> ModuleManager::ScanModuleStatements(const TokenStream& tokens)
{
	std::vector<StatementSpan> spans;
	int brace_depth = 0;

	for (int i = 0; i < tokens.Size(); i += 1)
	{
		const Token& token = tokens[i];

		if (token.m_token_name == Token::Name::LEFT_BRACE)
		{
			brace_depth += 1;
		}
		else if (token.m_token_name == Token::Name::RIGHT_BRACE)
		{
			brace_depth -= 1;
		}
		else if (brace_depth == 0)
		{
			StatementType stmt_type = StatementType::MODULE;
			bool is_module_statement = false;

			if (token.m_token_name == Token::Name::MODULE)
			{
				stmt_type = StatementType::MODULE;
				is_module_statement = true;
			}
			else if (token.m_token_name == Token::Name::IMPORT)
			{
				stmt_type = StatementType::IMPORT;
				is_module_statement = true;
			}
			else if (token.m_token_name == Token::Name::USE)
			{
				stmt_type = StatementType::USE;
				is_module_statement = true;
			}
			else if (token.m_token_name == Token::Name::EXPORT)
			{
				stmt_type = StatementType::EXPORT;
				is_module_statement = true;
			}
			else if (token.m_token_name == Token::Name::PUBLIC || token.m_token_name == Token::Name::PRIVATE)
			{
				int lookahead = i + 1;
				SkipWhiteSpace(tokens, lookahead);
				if (lookahead < tokens.Size() && tokens[lookahead].m_token_name == Token::Name::EXPORT)
				{
					stmt_type = StatementType::EXPORT;
					is_module_statement = true;
				}
			}

			if (is_module_statement)
			{
				int end = ComputeStatementEnd(tokens, i, stmt_type);
				spans.push_back(StatementSpan{ stmt_type, i, end, token.m_line });
				i = end - 1;
			}
		}
	}

	return spans;
}

int ModuleManager::ComputeStatementEnd(const TokenStream& tokens, int start, StatementType type)
{
	int current = start + 1;
	SkipWhiteSpace(tokens, current);

	if (type == StatementType::MODULE)
	{
		bool expect_identifier = true;
		while (current < tokens.Size())
		{
			const Token::Name token_name = tokens[current].m_token_name;
			if (expect_identifier)
			{
				if (token_name != Token::Name::IDENTIFIER_LITERAL && !Token::IsKeyword(token_name))
				{
					break;
				}

				expect_identifier = false;
				current += 1;
				SkipWhiteSpace(tokens, current);
				continue;
			}

			if (token_name != Token::Name::SINGLE_DOT)
			{
				break;
			}

			expect_identifier = true;
			current += 1;
			SkipWhiteSpace(tokens, current);
		}
		return current;
	}
	else if (type == StatementType::EXPORT)
	{
		SkipWhiteSpace(tokens, current);

		if (current < tokens.Size() && tokens[current].m_token_name == Token::Name::EXPORT)
		{
			current += 1;
			SkipWhiteSpace(tokens, current);
		}

		SkipBracedBlock(tokens, current);
		return current;
	}
	else if (type == StatementType::IMPORT)
	{
		SkipBracedBlock(tokens, current);
		return current;
	}
	else if (type == StatementType::USE)
	{
		while
		(
			current < tokens.Size() &&
			(
				tokens[current].m_token_name == Token::Name::IDENTIFIER_LITERAL ||
				tokens[current].m_token_name == Token::Name::SINGLE_DOT
			)
		)
		{
			current += 1;
			SkipWhiteSpace(tokens, current);
		}

		if (!SkipBracedBlock(tokens, current))
		{
			while (current < tokens.Size() && tokens[current].m_token_name == Token::Name::IDENTIFIER_LITERAL)
			{
				current += 1;
				SkipWhiteSpace(tokens, current);
			}
		}

		if (current < tokens.Size() && tokens[current].m_token_name == Token::Name::SINGLE_SEMICOLON)
		{
			current += 1;
		}

		return current;
	}

	return current;
}

MarmotResult::Result<const ModuleManager::StatementSpan*> ModuleManager::FindModuleDeclaration(const TokenStream& tokens, const std::vector<StatementSpan>& spans) const
{
	const auto module = std::ranges::find(spans, StatementType::MODULE, &StatementSpan::m_type);
	if (module == spans.end())
	{
		return MarmotCompiler::Failure
		(
			MarmotError::GenerateModuleErrorWithContext
			(
				CompilerErrorCode::ModuleDeclarationMissing,
				"Module declaration required. Each .mmt file must contain exactly one 'module ModuleName' declaration as its first top-level statement.",
				1,
				m_file_name
			)
		);
	}

	const auto duplicate = std::ranges::find(std::ranges::subrange(module + 1, spans.end()), StatementType::MODULE, &StatementSpan::m_type);
	if (duplicate != spans.end())
	{
		return MarmotCompiler::Failure
		(
			MarmotError::GenerateModuleErrorWithContext
			(
				CompilerErrorCode::ModuleDeclarationDuplicate,
				"Multiple module declarations found. Each .mmt file must contain exactly one 'module ModuleName' declaration.",
				duplicate->m_line,
				m_file_name
			)
		);
	}

	int first_token = 0;
	SkipWhiteSpace(tokens, first_token);
	while (first_token < tokens.Size() && tokens[first_token].m_token_name == Token::Name::END_OF_FILE)
	{
		first_token += 1;
	}

	if (first_token != module->m_start)
	{
		return MarmotCompiler::Failure
		(
			MarmotError::GenerateModuleErrorWithContext
			(
				"Module declaration must be the first top-level statement in the file.",
				module->m_line,
				m_file_name
			)
		);
	}

	return &*module;
}

MarmotResult::Result<std::string> ModuleManager::ExtractModuleName(const TokenStream& tokens, const StatementSpan& module_span) const
{
	std::string module_name;
	int current = module_span.m_start + 1;
	SkipWhiteSpace(tokens, current);

	if (current >= module_span.m_end)
	{
		return MarmotCompiler::Failure
		(
			MarmotError::GenerateModuleErrorWithContext
			(
				"Expected module name after 'module'.",
				module_span.m_line,
				m_file_name
			)
		);
	}

	bool expect_identifier = true;
	while (current < module_span.m_end)
	{
		const Token& token = tokens[current];
		if (expect_identifier)
		{
			if (token.m_token_name == Token::Name::IDENTIFIER_LITERAL)
			{
				module_name.append(token.m_lexeme);
				expect_identifier = false;
				current += 1;
				SkipWhiteSpace(tokens, current);
				continue;
			}

			if (Token::IsKeyword(token.m_token_name))
			{
				return MarmotCompiler::Failure
				(
					MarmotError::GenerateModuleErrorWithContext
					(
						"'" + token.m_lexeme + "' is a reserved keyword and cannot be used as a module name.",
						token.m_line,
						m_file_name
					)
				);
			}

			return MarmotCompiler::Failure
			(
				MarmotError::GenerateModuleErrorWithContext
				(
					"Expected identifier in module declaration.",
					token.m_line,
					m_file_name
				)
			);
		}

		if (token.m_token_name != Token::Name::SINGLE_DOT)
		{
			return MarmotCompiler::Failure
			(
				MarmotError::GenerateModuleErrorWithContext
				(
					"Unexpected token in module declaration.",
					token.m_line,
					m_file_name
				)
			);
		}

		module_name.push_back('.');
		expect_identifier = true;
		current += 1;
		SkipWhiteSpace(tokens, current);
	}

	if (expect_identifier)
	{
		return MarmotCompiler::Failure
		(
			MarmotError::GenerateModuleErrorWithContext
			(
				"Expected identifier after '.' in module declaration.",
				module_span.m_line,
				m_file_name
			)
		);
	}

	return module_name;
}

MarmotResult::Result<ModuleDeclaration> ModuleManager::ExtractModuleDeclaration(const TokenStream& tokens, const std::vector<StatementSpan>& spans) const
{
	const MarmotResult::Result<const StatementSpan*> module_span = FindModuleDeclaration(tokens, spans);
	if (!module_span.HasValue())
	{
		return MarmotCompiler::Failure(module_span.Error());
	}

	MarmotResult::Result<std::string> module_name_result = ExtractModuleName(tokens, *module_span.Value());
	if (!module_name_result.HasValue())
	{
		return MarmotCompiler::Failure(std::move(module_name_result.Error()));
	}

	std::vector<ModuleExport> all_exports;

	for (const StatementSpan& span : spans)
	{
		if (span.m_type == StatementType::EXPORT)
		{
			int current = span.m_start;
			VisibilityLevel visibility = VisibilityLevel::Public;

			if (tokens[current].m_token_name == Token::Name::EXPORT)
			{
				return MarmotCompiler::Failure(CompilerError::WithToken(CompilerStage::Module, "An export list needs a visibility.", tokens[current], m_file_name, m_source_lines, "Write 'public export { ... }' to export to every module, or 'private export { ... }' to export within this module's namespace."));
			}

			if (tokens[current].m_token_name == Token::Name::PUBLIC)
			{
				visibility = VisibilityLevel::Public;
			}
			else if (tokens[current].m_token_name == Token::Name::PRIVATE)
			{
				visibility = VisibilityLevel::Private;
			}

			current += 1;
			SkipWhiteSpace(tokens, current);

			if (current < tokens.Size() && tokens[current].m_token_name == Token::Name::EXPORT)
			{
				current += 1;
				SkipWhiteSpace(tokens, current);
			}

			if (current < tokens.Size() && tokens[current].m_token_name == Token::Name::LEFT_BRACE)
			{
				current += 1;
				SkipWhiteSpace(tokens, current);

				while (current < span.m_end && tokens[current].m_token_name != Token::Name::RIGHT_BRACE)
				{
					if (tokens[current].m_token_name == Token::Name::IDENTIFIER_LITERAL)
					{
						all_exports.emplace_back(tokens[current], visibility);
						current += 1;
						SkipWhiteSpace(tokens, current);

						if (current < span.m_end && tokens[current].m_token_name == Token::Name::COMMA)
						{
							current += 1;
							SkipWhiteSpace(tokens, current);
						}
					}
					else
					{
						current += 1;
					}
				}
			}
		}
	}

	return ModuleDeclaration(module_name_result.Value(), m_file_name).WithHasModuleDeclaration(true).WithExports(std::move(all_exports));
}

MarmotResult::Result<std::vector<ModuleManager::SourceImport>> ModuleManager::ExtractImports(const TokenStream& tokens, const std::vector<StatementSpan>& spans) const
{
	std::vector<SourceImport> import_paths;
	const std::string_view import_suggestion = R"(Use 'import { <IO> }' for system modules or 'import { "./File.mmt" }' for path imports.)";

	const auto make_import_error = [this, import_suggestion](std::string_view message, const Token& token) -> CompilerError
	{
		return CompilerError::WithToken(CompilerStage::Module, message, token, m_file_name, m_source_lines, import_suggestion);
	};

	for (const StatementSpan& span : spans)
	{
		if (span.m_type == StatementType::IMPORT)
		{
			int current = span.m_start + 1;
			SkipWhiteSpace(tokens, current);
			const Token& import_token = tokens[span.m_start];

			if (current >= span.m_end || tokens[current].m_token_name != Token::Name::LEFT_BRACE)
			{
				const Token& error_token = current < tokens.Size() ? tokens[current] : import_token;
				return MarmotCompiler::Failure(make_import_error("Expected '{' after 'import'.", error_token));
			}

			current += 1;
			SkipWhiteSpace(tokens, current);

			bool parsed_any_import = false;
			while (current < span.m_end && tokens[current].m_token_name != Token::Name::RIGHT_BRACE)
			{
				std::string import_specifier;
				Token import_entry_token = tokens[current];

				if (tokens[current].m_token_name == Token::Name::TEXT_LITERAL)
				{
					import_specifier = tokens[current].m_lexeme;
					if (import_specifier.size() >= 3 && import_specifier.starts_with('<') && import_specifier.ends_with('>'))
					{
						return MarmotCompiler::Failure(make_import_error("System imports must use unquoted angle brackets.", import_entry_token));
					}
					current += 1;
				}
				else if (tokens[current].m_token_name == Token::Name::LEFT_ANGLE)
				{
					const Token& left_angle_token = tokens[current];
					current += 1;
					SkipWhiteSpace(tokens, current);
					std::string module_name;
					bool expect_identifier = true;

					while (current < span.m_end && tokens[current].m_token_name != Token::Name::RIGHT_ANGLE)
					{
						if (expect_identifier)
						{
							if (tokens[current].m_token_name != Token::Name::IDENTIFIER_LITERAL)
							{
								return MarmotCompiler::Failure(make_import_error("Expected identifier in system import.", tokens[current]));
							}

							module_name += tokens[current].m_lexeme;
							current += 1;
							SkipWhiteSpace(tokens, current);
							expect_identifier = false;
							continue;
						}

						if (tokens[current].m_token_name != Token::Name::SINGLE_DOT)
						{
							return MarmotCompiler::Failure(make_import_error("Expected '.' or '>' in system import.", tokens[current]));
						}

						module_name.push_back('.');
						current += 1;
						SkipWhiteSpace(tokens, current);
						expect_identifier = true;
					}

					if (current >= span.m_end || tokens[current].m_token_name != Token::Name::RIGHT_ANGLE)
					{
						return MarmotCompiler::Failure(make_import_error("Expected '>' to close system import.", left_angle_token));
					}

					if (expect_identifier)
					{
						return MarmotCompiler::Failure(make_import_error("Expected identifier in system import.", tokens[current]));
					}

					if (tokens[current].m_line == left_angle_token.m_line)
					{
						import_entry_token.m_source_length = static_cast<size_t>(tokens[current].m_column.Value() + 1 - left_angle_token.m_column.Value());
					}
					current += 1;
					import_specifier = std::move(module_name);
				}
				else
				{
					return MarmotCompiler::Failure(make_import_error("Expected path import or system import in import list.", tokens[current]));
				}

				parsed_any_import = true;
				import_paths.emplace_back(std::move(import_specifier), std::move(import_entry_token));
				SkipWhiteSpace(tokens, current);

				if (current < span.m_end && tokens[current].m_token_name == Token::Name::COMMA)
				{
					current += 1;
					SkipWhiteSpace(tokens, current);
					continue;
				}

				if (current < span.m_end && tokens[current].m_token_name != Token::Name::RIGHT_BRACE)
				{
					return MarmotCompiler::Failure(make_import_error("Expected ',' or '}' in import list.", tokens[current]));
				}
			}

			if (!parsed_any_import)
			{
				const Token& error_token = current < tokens.Size() ? tokens[current] : import_token;
				return MarmotCompiler::Failure(make_import_error("Expected at least one import in import list.", error_token));
			}

			if (current >= span.m_end || tokens[current].m_token_name != Token::Name::RIGHT_BRACE)
			{
				return MarmotCompiler::Failure(make_import_error("Expected '}' to close import list.", import_token));
			}
		}
	}

	return import_paths;
}

MarmotResult::Result<std::vector<UseImport>> ModuleManager::ExtractUseStatements(const TokenStream& tokens, const std::vector<StatementSpan>& spans)
{
	std::vector<UseImport> use_imports;

	for (const StatementSpan& span : spans)
	{
		if (span.m_type == StatementType::USE)
		{
			int current = span.m_start + 1;
			SkipWhiteSpace(tokens, current);

			if (current >= tokens.Size() || tokens[current].m_token_name != Token::Name::IDENTIFIER_LITERAL)
			{
				return MarmotCompiler::Failure
				(
					MarmotError::GenerateModuleErrorWithContext
					(
						"Expected module name after 'use'.",
						span.m_line,
						m_file_name
					)
				);
			}

			std::vector<std::string> segments;
			segments.emplace_back(tokens[current].m_lexeme);
			current += 1;
			SkipWhiteSpace(tokens, current);

			bool parsed_braced_use = false;
			while (current < span.m_end && tokens[current].m_token_name == Token::Name::SINGLE_DOT)
			{
				const Token& dot_token = tokens[current];
				current += 1;
				SkipWhiteSpace(tokens, current);

				if (current >= span.m_end)
				{
					return MarmotCompiler::Failure
					(
						MarmotError::GenerateModuleErrorWithContext
						(
							"Expected identifier or '{' after '.' in use statement.",
							dot_token.m_line,
							m_file_name
						)
					);
				}

				if (tokens[current].m_token_name == Token::Name::LEFT_BRACE)
				{
					const std::string module_name = JoinDottedSegments(segments, segments.size());
					current += 1;
					SkipWhiteSpace(tokens, current);

					if (current < span.m_end && tokens[current].m_token_name == Token::Name::RIGHT_BRACE)
					{
						return MarmotCompiler::Failure
						(
							MarmotError::GenerateModuleErrorWithContext
							(
								"Expected at least one identifier in use import list.",
								tokens[current].m_line,
								m_file_name
							)
						);
					}

					while (current < span.m_end && tokens[current].m_token_name != Token::Name::RIGHT_BRACE)
					{
						if (tokens[current].m_token_name != Token::Name::IDENTIFIER_LITERAL)
						{
							return MarmotCompiler::Failure
							(
								MarmotError::GenerateModuleErrorWithContext
								(
									"Expected identifier in use import list.",
									tokens[current].m_line,
									m_file_name
								)
							);
						}

						use_imports.emplace_back(module_name, tokens[current].m_lexeme);
						current += 1;
						SkipWhiteSpace(tokens, current);

						if (current < span.m_end && tokens[current].m_token_name == Token::Name::COMMA)
						{
							current += 1;
							SkipWhiteSpace(tokens, current);
						}
						else if (current < span.m_end && tokens[current].m_token_name != Token::Name::RIGHT_BRACE)
						{
							return MarmotCompiler::Failure
							(
								MarmotError::GenerateModuleErrorWithContext
								(
									"Expected ',' or '}' in use import list.",
									tokens[current].m_line,
									m_file_name
								)
							);
						}
					}

					if (current >= span.m_end || tokens[current].m_token_name != Token::Name::RIGHT_BRACE)
					{
						return MarmotCompiler::Failure
						(
							MarmotError::GenerateModuleErrorWithContext
							(
								"Expected '}' to close use import list.",
								span.m_line,
								m_file_name
							)
						);
					}

					current += 1;
					SkipWhiteSpace(tokens, current);
					parsed_braced_use = true;
					break;
				}

				if (tokens[current].m_token_name != Token::Name::IDENTIFIER_LITERAL)
				{
					return MarmotCompiler::Failure
					(
						MarmotError::GenerateModuleErrorWithContext
						(
							"Expected identifier or '{' after '.' in use statement.",
							tokens[current].m_line,
							m_file_name
						)
					);
				}

				segments.emplace_back(tokens[current].m_lexeme);
				current += 1;
				SkipWhiteSpace(tokens, current);
			}

			if (!parsed_braced_use)
			{
				if (segments.size() < 2u)
				{
					return MarmotCompiler::Failure
					(
						MarmotError::GenerateModuleErrorWithContext
						(
							"Expected imported symbol after module qualifier in use statement.",
							span.m_line,
							m_file_name
						)
					);
				}

				const std::string module_name = JoinDottedSegments(segments, segments.size() - 1u);
				const std::string& symbol_name = segments.back();
				use_imports.emplace_back(module_name, symbol_name);
			}

			if (current < span.m_end && tokens[current].m_token_name == Token::Name::SINGLE_SEMICOLON)
			{
				current += 1;
				SkipWhiteSpace(tokens, current);
			}

			if (current < span.m_end)
			{
				return MarmotCompiler::Failure
				(
					MarmotError::GenerateModuleErrorWithContext
					(
						"Unexpected token in use statement.",
						tokens[current].m_line,
						m_file_name
					)
				);
			}
		}
	}

	return use_imports;
}
