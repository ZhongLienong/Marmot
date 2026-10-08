#include "ModuleManager.h"
#include "Compiler/Error/CompilerError.h"
#include "ModuleSourceLoader.h"
#include "Compiler/Token/Token.h"
#include "Compiler/ImportResolver/ImportResolver.h"

#include <filesystem>
#include <expected>
#include <format>
#include <algorithm>
#include <ranges>

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
		return CompilerError::WithToken(
			CompilerStage::Module,
			std::format("Import cycle: {} -> {}.", chain, build_graph.m_module_declarations.at(cycle.front()).ModuleName()),
			import_token,
			importer.m_file_name,
			importer.m_source_lines,
			"A module cannot import itself, directly or through the modules it imports.",
			CompilerErrorCode::ModuleCircularDependency);
	}

	std::string JoinDottedSegments(const std::vector<std::string>& segments, size_t count)
	{
		std::string result;
		for (size_t i = 0u; i < count; i += 1u)
		{
			if (!result.empty())
			{
				result.push_back('.');
			}

			result.append(segments[i]);
		}

		return result;
	}
}

ModuleManager::ModuleManager(TokenStream&& main_file_tokens, std::string_view main_file_name, std::vector<std::string> main_source_lines, CompilationInputs inputs)
	: m_main_token_stream(std::move(main_file_tokens)),
	m_main_file_name(main_file_name),
	m_main_source_lines(std::move(main_source_lines)),
	m_inputs(std::move(inputs))
{
}

MidoriResult::ModuleManagerResult ModuleManager::GenerateBuildGraph()
{
	BuildGraph build_graph;
	ModuleSourceLoader source_loader(m_inputs.Jobs().value_or(std::max(1u, std::thread::hardware_concurrency())));
	DiscoveryState discovery;
	MidoriResult::VoidResult result = GenerateBuildGraphImpl(build_graph, source_loader, discovery);
	if (!result.has_value())
	{
		return std::unexpected(std::move(result.error()));
	}

	return build_graph;
}

MidoriResult::VoidResult ModuleManager::GenerateBuildGraphImpl(BuildGraph& build_graph, ModuleSourceLoader& source_loader, DiscoveryState& discovery)
{
	if (m_main_token_stream.Size() != 0)
	{
		std::vector<StatementSpan> spans = ScanModuleStatements(m_main_token_stream);

		MidoriResult::Result<std::tuple<std::string, std::vector<ModuleExport>>> module_result = ExtractModuleDeclaration(m_main_token_stream, spans);
		if (!module_result.has_value())
		{
			return std::unexpected(std::move(module_result.error()));
		}

		auto [module_name, exports] = std::move(module_result.value());

		if (build_graph.m_module_name_to_file.contains(module_name))
		{
			const std::string& existing_file = build_graph.m_module_name_to_file.at(module_name);
			if (existing_file != m_main_file_name)
			{
				return std::unexpected(MidoriError::GenerateModuleErrorWithContext(
					CompilerErrorCode::ModuleDeclarationDuplicate,
					std::format
					(
						"Duplicate module declaration: '{}' is declared in multiple files:\n  First:  {}\n  Second: {}",
						module_name,
						existing_file,
						m_main_file_name
					),
					1,
					m_main_file_name
				));
			}
		}
		else
		{
			build_graph.m_module_name_to_file[module_name] = m_main_file_name;
		}

		bool has_module_decl = std::ranges::any_of(spans, [](const StatementSpan& span) { return span.m_type == StatementType::MODULE; });

		ModuleDeclaration module_decl = ModuleDeclaration(module_name, m_main_file_name)
			.WithHasModuleDeclaration(has_module_decl)
			.WithExports(std::move(exports));
		build_graph.m_module_declarations.emplace(m_main_file_name, std::move(module_decl));

		MidoriResult::Result<std::vector<std::pair<std::string, Token>>> import_result = ExtractImports(m_main_token_stream, spans);
		if (!import_result.has_value())
		{
			return std::unexpected(std::move(import_result.error()));
		}

		std::vector<std::pair<std::string, Token>> import_paths = std::move(import_result.value());
		MidoriResult::Result<std::vector<UseImport>> use_import_result = ExtractUseStatements(m_main_token_stream, spans);
		if (!use_import_result.has_value())
		{
			return std::unexpected(std::move(use_import_result.error()));
		}
		std::vector<UseImport> use_imports = std::move(use_import_result.value());

		std::ranges::sort(spans, [](const StatementSpan& a, const StatementSpan& b) { return a.m_start > b.m_start; });

		for (const StatementSpan& span : spans)
		{
			m_main_token_stream.Erase(m_main_token_stream.begin() + span.m_start, m_main_token_stream.begin() + span.m_end);
		}

		BuildGraph::BuildNode& main_node = build_graph.m_nodes[m_main_file_name];
		main_node.m_tokens = std::move(m_main_token_stream);
		main_node.m_file_name = m_main_file_name;
		main_node.m_source_lines = std::move(m_main_source_lines);
		main_node.m_use_imports = std::move(use_imports);
		discovery.m_active_modules.push_back(m_main_file_name);

		ImportResolver resolver(m_main_file_name, m_inputs.SearchPaths());

		std::vector<std::optional<ImportResolver::ResolvedImport>> resolved_imports;
		std::vector<std::string> loading;
		resolved_imports.reserve(import_paths.size());
		for (const auto& [import_specifier, import_token] : import_paths)
		{
			resolved_imports.push_back(resolver.Resolve(import_specifier));
			const std::optional<ImportResolver::ResolvedImport>& resolved = resolved_imports.back();
			if (resolved.has_value() && !build_graph.m_nodes.contains(resolved->m_absolute_path))
			{
				loading.push_back(resolved->m_absolute_path);
			}
		}
		source_loader.Prefetch(loading);
		std::unordered_set<std::string_view> dependencies;
		dependencies.reserve(import_paths.size());

		// Consume prefetched results in the original depth-first order so a
		// faster sibling cannot change which declaration or error comes first.
		for (size_t index = 0u; index < import_paths.size(); index += 1u)
		{
			const auto& [import_specifier, import_token] = import_paths[index];
			const std::optional<ImportResolver::ResolvedImport>& resolved_opt = resolved_imports[index];
			if (!resolved_opt.has_value())
			{
				return std::unexpected(CompilerError::WithToken(CompilerStage::Module, "Could not resolve import: "s + import_specifier, import_token, m_main_file_name, main_node.m_source_lines, std::nullopt, CompilerErrorCode::ModuleImportResolutionFailed));
			}

			const std::string& include_absolute_path_str = resolved_opt->m_absolute_path;

			if (dependencies.emplace(include_absolute_path_str).second)
			{
				main_node.m_dependencies.emplace_back(include_absolute_path_str);
				main_node.m_import_tokens.emplace(include_absolute_path_str, import_token);
			}

			if (build_graph.m_nodes.contains(include_absolute_path_str))
			{
				const std::vector<std::string>::const_iterator cycle_start = std::ranges::find(discovery.m_active_modules, include_absolute_path_str);
				if (cycle_start != discovery.m_active_modules.cend())
				{
					discovery.m_cycles.emplace(include_absolute_path_str, ImportCycleError(build_graph, std::ranges::subrange(cycle_start, discovery.m_active_modules.cend()), import_token, main_node));
				}
				continue;
			}

			MidoriResult::Result<ImportedSource> source = source_loader.Take(include_absolute_path_str, m_main_file_name, import_token.m_line);
			if (!source.has_value())
			{
				return std::unexpected(std::move(source.error()));
			}

			ModuleManager module_manager(std::move(source->m_tokens), include_absolute_path_str, std::move(source->m_source_lines), m_inputs);
			MidoriResult::VoidResult nested_build_graph_result = module_manager.GenerateBuildGraphImpl(build_graph, source_loader, discovery);
			if (!nested_build_graph_result.has_value())
			{
				return std::unexpected(std::move(nested_build_graph_result.error()));
			}
		}
		discovery.m_active_modules.pop_back();
	}

	// Finish the owning module before reporting its cycle, so errors in its
	// remaining imports keep their original precedence and diagnostic location.
	const std::unordered_map<std::string, CompilerError>::const_iterator cycle = discovery.m_cycles.find(m_main_file_name);
	if (cycle != discovery.m_cycles.cend())
	{
		return std::unexpected(cycle->second);
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
				if (token_name != Token::Name::IDENTIFIER_LITERAL && !IsKeyword(token_name))
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

		if (current < tokens.Size() && tokens[current].m_token_name == Token::Name::LEFT_BRACE)
		{
			current += 1;
			int depth = 1;
			while (current < tokens.Size() && depth > 0)
			{
				if (tokens[current].m_token_name == Token::Name::LEFT_BRACE)
				{
					depth += 1;
				}
				else if (tokens[current].m_token_name == Token::Name::RIGHT_BRACE)
				{
					depth -= 1;
				}
				current += 1;
			}
		}
		return current;
	}
	else if (type == StatementType::IMPORT)
	{
		if (current < tokens.Size() && tokens[current].m_token_name == Token::Name::LEFT_BRACE)
		{
			current += 1;
			int depth = 1;
			while (current < tokens.Size() && depth > 0)
			{
				if (tokens[current].m_token_name == Token::Name::LEFT_BRACE)
				{
					depth += 1;
				}
				else if (tokens[current].m_token_name == Token::Name::RIGHT_BRACE)
				{
					depth -= 1;
				}
				current += 1;
			}
		}
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

		if (current < tokens.Size() && tokens[current].m_token_name == Token::Name::LEFT_BRACE)
		{
			current += 1;
			int depth = 1;
			while (current < tokens.Size() && depth > 0)
			{
				if (tokens[current].m_token_name == Token::Name::LEFT_BRACE)
				{
					depth += 1;
				}
				else if (tokens[current].m_token_name == Token::Name::RIGHT_BRACE)
				{
					depth -= 1;
				}
				current += 1;
			}
		}
		else
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

bool ModuleManager::IsKeyword(Token::Name token_name)
{
	return token_name != Token::Name::IDENTIFIER_LITERAL &&
	       token_name != Token::Name::TEXT_LITERAL &&
	       token_name != Token::Name::INTEGER_LITERAL &&
	       token_name != Token::Name::FLOAT_LITERAL &&
	       token_name != Token::Name::WHITESPACE &&
	       token_name != Token::Name::END_OF_FILE &&
	       static_cast<int>(token_name) >= static_cast<int>(Token::Name::ELSE);
}

MidoriResult::VoidResult ModuleManager::ValidateModuleDeclarationPolicy(const TokenStream& tokens, const std::vector<StatementSpan>& spans) const
{
	std::vector<const StatementSpan*> module_spans;
	for (const StatementSpan& span : spans)
	{
		if (span.m_type == StatementType::MODULE)
		{
			module_spans.emplace_back(&span);
		}
	}

	if (module_spans.empty())
	{
		return std::unexpected
		(
			MidoriError::GenerateModuleErrorWithContext
			(
				CompilerErrorCode::ModuleDeclarationMissing,
				"Module declaration required. Each .mmt file must contain exactly one 'module ModuleName' declaration as its first top-level statement.",
				1,
				m_main_file_name
			)
		);
	}

	if (module_spans.size() > 1u)
	{
		return std::unexpected
		(
			MidoriError::GenerateModuleErrorWithContext
			(
				CompilerErrorCode::ModuleDeclarationDuplicate,
				"Multiple module declarations found. Each .mmt file must contain exactly one 'module ModuleName' declaration.",
				module_spans[1]->m_line,
				m_main_file_name
			)
		);
	}

	int first_token = 0;
	SkipWhiteSpace(tokens, first_token);
	while (first_token < tokens.Size() && tokens[first_token].m_token_name == Token::Name::END_OF_FILE)
	{
		first_token += 1;
	}

	if (first_token != module_spans.front()->m_start)
	{
		return std::unexpected
		(
			MidoriError::GenerateModuleErrorWithContext
			(
				"Module declaration must be the first top-level statement in the file.",
				module_spans.front()->m_line,
				m_main_file_name
			)
		);
	}

	return {};
}

MidoriResult::Result<std::string> ModuleManager::ExtractModuleName(const TokenStream& tokens, const StatementSpan& module_span) const
{
	std::string module_name;
	int current = module_span.m_start + 1;
	SkipWhiteSpace(tokens, current);

	if (current >= module_span.m_end)
	{
		return std::unexpected
		(
			MidoriError::GenerateModuleErrorWithContext
			(
				"Expected module name after 'module'.",
				module_span.m_line,
				m_main_file_name
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

			if (IsKeyword(token.m_token_name))
			{
				return std::unexpected
				(
					MidoriError::GenerateModuleErrorWithContext
					(
						"'" + token.m_lexeme + "' is a reserved keyword and cannot be used as a module name.",
						token.m_line,
						m_main_file_name
					)
				);
			}

			return std::unexpected
			(
				MidoriError::GenerateModuleErrorWithContext
				(
					"Expected identifier in module declaration.",
					token.m_line,
					m_main_file_name
				)
			);
		}

		if (token.m_token_name != Token::Name::SINGLE_DOT)
		{
			return std::unexpected
			(
				MidoriError::GenerateModuleErrorWithContext
				(
					"Unexpected token in module declaration.",
					token.m_line,
					m_main_file_name
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
		return std::unexpected
		(
			MidoriError::GenerateModuleErrorWithContext
			(
				"Expected identifier after '.' in module declaration.",
				module_span.m_line,
				m_main_file_name
			)
		);
	}

	return module_name;
}

MidoriResult::Result<std::tuple<std::string, std::vector<ModuleExport>>> ModuleManager::ExtractModuleDeclaration(const TokenStream& tokens, const std::vector<StatementSpan>& spans)
{
	std::vector<ModuleExport> all_exports;
	MidoriResult::VoidResult validation_result = ValidateModuleDeclarationPolicy(tokens, spans);
	if (!validation_result.has_value())
	{
		return std::unexpected(std::move(validation_result.error()));
	}

	const StatementSpan* module_span = nullptr;
	for (const StatementSpan& span : spans)
	{
		if (span.m_type == StatementType::MODULE)
		{
			module_span = &span;
			break;
		}
	}

	if (module_span == nullptr)
	{
		return std::unexpected
		(
			MidoriError::GenerateModuleErrorWithContext
			(
				CompilerErrorCode::ModuleDeclarationMissing,
				"Module declaration required. Each .mmt file must contain exactly one 'module ModuleName' declaration as its first top-level statement.",
				1,
				m_main_file_name
			)
		);
	}

	MidoriResult::Result<std::string> module_name_result = ExtractModuleName(tokens, *module_span);
	if (!module_name_result.has_value())
	{
		return std::unexpected(std::move(module_name_result.error()));
	}

	std::string module_name = std::move(module_name_result.value());

	for (const StatementSpan& span : spans)
	{
		if (span.m_type == StatementType::EXPORT)
		{
			int current = span.m_start;
			VisibilityLevel visibility = VisibilityLevel::Public;

			if (tokens[current].m_token_name == Token::Name::EXPORT)
			{
				return std::unexpected(CompilerError::WithToken(CompilerStage::Module, "An export list needs a visibility.", tokens[current], m_main_file_name, m_main_source_lines, "Write 'public export { ... }' to export to every module, or 'private export { ... }' to export within this module's namespace."));
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

	return std::make_tuple(std::move(module_name), std::move(all_exports));
}

MidoriResult::Result<std::vector<std::pair<std::string, Token>>> ModuleManager::ExtractImports(const TokenStream& tokens, const std::vector<StatementSpan>& spans)
{
	std::vector<std::pair<std::string, Token>> import_paths;
	const std::string_view import_suggestion = R"(Use 'import { <IO> }' for system modules or 'import { "./File.mmt" }' for path imports.)";

	const auto make_import_error = [this, import_suggestion](std::string_view message, const Token& token) -> CompilerError
	{
		return CompilerError::WithToken(CompilerStage::Module, message, token, m_main_file_name, m_main_source_lines, import_suggestion);
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
				return std::unexpected(make_import_error("Expected '{' after 'import'.", error_token));
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
								return std::unexpected(make_import_error("Expected identifier in system import.", tokens[current]));
							}

							module_name += tokens[current].m_lexeme;
							current += 1;
							SkipWhiteSpace(tokens, current);
							expect_identifier = false;
							continue;
						}

						if (tokens[current].m_token_name != Token::Name::SINGLE_DOT)
						{
							return std::unexpected(make_import_error("Expected '.' or '>' in system import.", tokens[current]));
						}

						module_name.push_back('.');
						current += 1;
						SkipWhiteSpace(tokens, current);
						expect_identifier = true;
					}

					if (current >= span.m_end || tokens[current].m_token_name != Token::Name::RIGHT_ANGLE)
					{
						return std::unexpected(make_import_error("Expected '>' to close system import.", left_angle_token));
					}

					if (expect_identifier)
					{
						return std::unexpected(make_import_error("Expected identifier in system import.", tokens[current]));
					}

					if (tokens[current].m_line == left_angle_token.m_line)
					{
						import_entry_token.m_source_length = static_cast<size_t>(tokens[current].m_column.value() + 1 - left_angle_token.m_column.value());
					}
					current += 1;
					import_specifier = "<"s + module_name + ">"s;
				}
				else
				{
					return std::unexpected(make_import_error("Expected path import or system import in import list.", tokens[current]));
				}

				parsed_any_import = true;
				import_paths.emplace_back(import_specifier, import_entry_token);
				SkipWhiteSpace(tokens, current);

				if (current < span.m_end && tokens[current].m_token_name == Token::Name::COMMA)
				{
					current += 1;
					SkipWhiteSpace(tokens, current);
					continue;
				}

				if (current < span.m_end && tokens[current].m_token_name != Token::Name::RIGHT_BRACE)
				{
					return std::unexpected(make_import_error("Expected ',' or '}' in import list.", tokens[current]));
				}
			}

			if (!parsed_any_import)
			{
				const Token& error_token = current < tokens.Size() ? tokens[current] : import_token;
				return std::unexpected(make_import_error("Expected at least one import in import list.", error_token));
			}

			if (current >= span.m_end || tokens[current].m_token_name != Token::Name::RIGHT_BRACE)
			{
				return std::unexpected(make_import_error("Expected '}' to close import list.", import_token));
			}
		}
	}

	return MidoriResult::Result<std::vector<std::pair<std::string, Token>>>(std::move(import_paths));
}

MidoriResult::Result<std::vector<UseImport>> ModuleManager::ExtractUseStatements(const TokenStream& tokens, const std::vector<StatementSpan>& spans)
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
				return std::unexpected
				(
					MidoriError::GenerateModuleErrorWithContext
					(
						"Expected module name after 'use'.",
						span.m_line,
						m_main_file_name
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
					return std::unexpected
					(
						MidoriError::GenerateModuleErrorWithContext
						(
							"Expected identifier or '{' after '.' in use statement.",
							dot_token.m_line,
							m_main_file_name
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
						return std::unexpected
						(
							MidoriError::GenerateModuleErrorWithContext
							(
								"Expected at least one identifier in use import list.",
								tokens[current].m_line,
								m_main_file_name
							)
						);
					}

					while (current < span.m_end && tokens[current].m_token_name != Token::Name::RIGHT_BRACE)
					{
						if (tokens[current].m_token_name != Token::Name::IDENTIFIER_LITERAL)
						{
							return std::unexpected
							(
								MidoriError::GenerateModuleErrorWithContext
								(
									"Expected identifier in use import list.",
									tokens[current].m_line,
									m_main_file_name
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
							return std::unexpected
							(
								MidoriError::GenerateModuleErrorWithContext
								(
									"Expected ',' or '}' in use import list.",
									tokens[current].m_line,
									m_main_file_name
								)
							);
						}
					}

					if (current >= span.m_end || tokens[current].m_token_name != Token::Name::RIGHT_BRACE)
					{
						return std::unexpected
						(
							MidoriError::GenerateModuleErrorWithContext
							(
								"Expected '}' to close use import list.",
								span.m_line,
								m_main_file_name
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
					return std::unexpected
					(
						MidoriError::GenerateModuleErrorWithContext
						(
							"Expected identifier or '{' after '.' in use statement.",
							tokens[current].m_line,
							m_main_file_name
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
					return std::unexpected
					(
						MidoriError::GenerateModuleErrorWithContext
						(
							"Expected imported symbol after module qualifier in use statement.",
							span.m_line,
							m_main_file_name
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
				return std::unexpected
				(
					MidoriError::GenerateModuleErrorWithContext
					(
						"Unexpected token in use statement.",
						tokens[current].m_line,
						m_main_file_name
					)
				);
			}
		}
	}

	return use_imports;
}
