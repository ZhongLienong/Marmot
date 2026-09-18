#include "ModuleManager.h"
#include "Common/Error/Error.h"
#include "Common/Printer/Printer.h"
#include "Compiler/Lexer/Lexer.h"
#include "Compiler/Token/Token.h"
#include "Compiler/ImportResolver/ImportResolver.h"
#include "Compiler/PackageManager/PackageManifest.h"
#include "Library/SharedLibraryCache/SharedLibraryCache.h"

#include <filesystem>
#include <expected>
#include <format>
#include <fstream>
#include <queue>
#include <sstream>
#include <algorithm>

using namespace std::string_literals;

namespace
{
	std::vector<std::string> SplitSourceLines(const std::string& source)
	{
		std::vector<std::string> source_lines;
		std::istringstream stream(source);
		std::string line;
		while (std::getline(stream, line))
		{
			source_lines.emplace_back(std::move(line));
		}

		return source_lines;
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

ModuleManager::ModuleManager(TokenStream&& main_file_tokens, std::string_view main_file_name, std::vector<std::string> main_source_lines)
	: m_main_token_stream(std::move(main_file_tokens)),
	m_main_file_name(main_file_name),
	m_main_source_lines(std::move(main_source_lines))
{
}

MidoriResult::ModuleManagerResult ModuleManager::GenerateBuildGraph()
{
	BuildGraph build_graph;
	return GenerateBuildGraphImpl(build_graph);
}

MidoriResult::ModuleManagerResult ModuleManager::GenerateBuildGraphImpl(BuildGraph& build_graph)
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
		m_module_declarations[m_main_file_name] = std::move(module_decl);

		MidoriResult::Result<std::vector<std::pair<std::string, int>>> import_result = ExtractImports(m_main_token_stream, spans);
		if (!import_result.has_value())
		{
			return std::unexpected(std::move(import_result.error()));
		}

		std::vector<std::pair<std::string, int>> import_paths = std::move(import_result.value());
		MidoriResult::Result<std::vector<UseImport>> use_import_result = ExtractUseStatements(m_main_token_stream, spans);
		if (!use_import_result.has_value())
		{
			return std::unexpected(std::move(use_import_result.error()));
		}
		std::vector<UseImport> use_imports = std::move(use_import_result.value());

		std::ranges::sort(spans, [](const StatementSpan& a, const StatementSpan& b) { return a.m_start > b.m_start; });

		for (const StatementSpan& span : spans)
		{
			for (int i = 0; i < span.m_end - span.m_start; i += 1)
			{
				m_main_token_stream.Erase(m_main_token_stream.begin() + span.m_start);
			}
		}

		BuildGraph::BuildNode& main_node = build_graph.m_nodes[m_main_file_name];
		main_node.m_tokens = m_main_token_stream;
		main_node.m_file_name = m_main_file_name;
		main_node.m_source_lines = m_main_source_lines;
		main_node.m_use_imports = std::move(use_imports);

		ImportResolver resolver(m_main_file_name);

		for (const auto& [import_specifier, line] : import_paths)
		{
			std::optional<ImportResolver::ResolvedImport> resolved_opt = resolver.Resolve(import_specifier);
			if (!resolved_opt.has_value())
			{
				return std::unexpected(MidoriError::GenerateModuleErrorWithContext(CompilerErrorCode::ModuleImportResolutionFailed, "Could not resolve import: "s + import_specifier, line, m_main_file_name));
			}

			std::string include_absolute_path_str = resolved_opt->m_absolute_path;

			m_dependency_graph[m_main_file_name].emplace_back(include_absolute_path_str);

			std::filesystem::path import_path(include_absolute_path_str);
			std::filesystem::path package_dir = import_path.parent_path();
			std::filesystem::path package_manifest_path = package_dir / "package.marmot";

			if (std::filesystem::exists(package_manifest_path))
			{
				std::optional<PackageManifest> manifest_opt = PackageManifest::Load(package_dir);
				if (manifest_opt.has_value())
				{
					const PackageManifest& manifest = manifest_opt.value();
					const PackageFFI& ffi = manifest.GetFFI();

					if (ffi.m_enabled)
					{
						std::filesystem::path library_path = manifest.GetFFILibraryPath();
						std::optional<std::string_view> expected_checksum = std::nullopt;
						const std::optional<PrebuiltBinary> selected_prebuilt = manifest.GetSelectedPrebuiltBinary();
						if (selected_prebuilt.has_value() && !selected_prebuilt->m_checksum.empty())
						{
							expected_checksum = selected_prebuilt->m_checksum;
						}

						if (std::filesystem::exists(library_path))
						{
							SharedLibraryCache& cache = SharedLibraryCache::GetInstance();
							if (!cache.IsLibraryLoaded(manifest.GetInfo().m_name))
							{
								const std::expected<void, std::string> load_result =
									cache.LoadLibraryWithFunctions(library_path, manifest.GetInfo().m_name, ffi.m_functions, ffi.m_thread_safe, expected_checksum);
								if (!load_result.has_value())
								{
									return std::unexpected(MidoriError::GenerateModuleErrorWithContext(load_result.error(), line, m_main_file_name));
								}
							}
						}
					}
				}
			}

			if (build_graph.m_nodes.contains(include_absolute_path_str))
			{
				continue;
			}

			std::ifstream include_file(include_absolute_path_str);
			if (!include_file.is_open())
			{
				return std::unexpected(MidoriError::GenerateModuleErrorWithContext(CompilerErrorCode::ModuleImportFileOpenFailed, "Could not open import file: "s + include_absolute_path_str, line, m_main_file_name));
			}

			if (HasCircularDependency())
			{
				return std::unexpected(MidoriError::GenerateModuleErrorWithContext(CompilerErrorCode::ModuleCircularDependency, "Circular dependency detected: "s + include_absolute_path_str, line, m_main_file_name));
			}

			std::ostringstream include_file_stream;
			include_file_stream << include_file.rdbuf();
			std::string include_source = include_file_stream.str();
			std::vector<std::string> include_source_lines = SplitSourceLines(include_source);

			MidoriResult::LexerResult lex_result = Lexer(std::move(include_source), include_absolute_path_str).Lex();
			if (!lex_result.has_value())
			{
				return std::unexpected(std::move(lex_result.error()));
			}

			TokenStream imported_token_stream = std::move(lex_result.value());

			ModuleManager module_manager(std::move(imported_token_stream), std::move(include_absolute_path_str), std::move(include_source_lines));
			MidoriResult::ModuleManagerResult nested_build_graph_result = module_manager.GenerateBuildGraphImpl(build_graph);
			if (!nested_build_graph_result.has_value())
			{
				return std::unexpected(std::move(nested_build_graph_result.error()));
			}

			BuildGraph& nested_build_graph = nested_build_graph_result.value();
			for (const auto& [file_name, node] : nested_build_graph.m_nodes)
			{
				if (!build_graph.m_nodes.contains(file_name))
				{
					build_graph.m_nodes[file_name] = node;
				}
			}

			for (const auto& [nested_file_path, nested_module_decl] : module_manager.m_module_declarations)
			{
				if (!m_module_declarations.contains(nested_file_path))
				{
					m_module_declarations[nested_file_path] = nested_module_decl;
				}
			}

			for (const auto& [src, dependencies] : module_manager.m_dependency_graph)
			{
				for (const std::string& dependency : dependencies)
				{
					if (!std::ranges::contains(m_dependency_graph[src], dependency))
					{
						m_dependency_graph[src].emplace_back(dependency);
					}
				}
			}
		}
	}

	BuildDependencyGraph(build_graph);

	CalculateInDegrees(build_graph);

	if (HasCircularDependency())
	{
		return std::unexpected(MidoriError::GenerateModuleErrorWithContext(CompilerErrorCode::ModuleCircularDependency, "Circular dependency detected in final build graph", 0, m_main_file_name));
	}

	build_graph.m_module_declarations = m_module_declarations;

	for (const auto& [file_name, node] : build_graph.m_nodes)
	{
		if (!node.m_use_imports.empty())
		{
			build_graph.m_use_imports[file_name] = node.m_use_imports;
		}
	}

	return build_graph;
}

bool ModuleManager::HasCircularDependency() const
{
	std::unordered_set<std::string> visited;
	std::unordered_set<std::string> recursion_stack;

	for (const auto& [node, _] : m_dependency_graph)
	{
		if (CheckCycle(node, visited, recursion_stack))
		{
			return true;
		}
	}

	return false;
}

bool ModuleManager::CheckCycle(const std::string& node, std::unordered_set<std::string>& visited, std::unordered_set<std::string>& recursion_stack) const
{
	if (recursion_stack.contains(node))
	{
		return true;
	}

	if (visited.contains(node))
	{
		return false;
	}

	visited.emplace(node);
	recursion_stack.emplace(node);

	if (m_dependency_graph.contains(node))
	{
		for (const std::string& dependency : m_dependency_graph.at(node))
		{
			if (CheckCycle(dependency, visited, recursion_stack))
			{
				return true;
			}
		}
	}

	recursion_stack.erase(node);
	return false;
}

void ModuleManager::BuildDependencyGraph(BuildGraph& build_graph)
{
	for (const auto& [src, dependencies] : m_dependency_graph)
	{
		if (build_graph.m_nodes.contains(src))
		{
			BuildGraph::BuildNode& node = build_graph.m_nodes[src];
			for (const std::string& dependency : dependencies)
			{
				if (build_graph.m_nodes.contains(dependency) && !std::ranges::contains(node.m_dependencies, dependency))
				{
					node.m_dependencies.emplace_back(dependency);
				}
			}
		}
	}
}

void ModuleManager::CalculateInDegrees(BuildGraph& build_graph)
{
	for (auto& [file, node] : build_graph.m_nodes)
	{
		node.m_in_degree = 0;
	}

	for (const auto& [file, node] : build_graph.m_nodes)
	{
		for (const std::string& dependency : node.m_dependencies)
		{
			if (build_graph.m_nodes.contains(dependency))
			{
				build_graph.m_nodes.at(dependency).m_in_degree += 1;
			}
		}
	}
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
						all_exports.emplace_back(tokens[current].m_lexeme, visibility);
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

MidoriResult::Result<std::vector<std::pair<std::string, int>>> ModuleManager::ExtractImports(const TokenStream& tokens, const std::vector<StatementSpan>& spans)
{
	std::vector<std::pair<std::string, int>> import_paths;
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
				const Token& import_entry_token = tokens[current];
				int import_line = import_entry_token.m_line;

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

					current += 1;
					import_specifier = "<"s + module_name + ">"s;
				}
				else
				{
					return std::unexpected(make_import_error("Expected path import or system import in import list.", tokens[current]));
				}

				parsed_any_import = true;
				import_paths.emplace_back(import_specifier, import_line);
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

	return MidoriResult::Result<std::vector<std::pair<std::string, int>>>(std::move(import_paths));
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
