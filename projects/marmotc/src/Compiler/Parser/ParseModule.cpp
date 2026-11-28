#include "Parser.h"
#include "Compiler/Names/SpecialNames.h"
#include "Support/Expected.h"
#include "Support/Optional.h"
#include <algorithm>
#include <format>
#include <fstream>
#include <ranges>

using namespace std::string_literals;

Parser::ParseContext::ParseContext(TokenStream&& tokens, std::string_view file_name, const std::vector<std::string>& source_lines, const ModuleImports& imports, const ModuleDeclaration* module_decl)
	: m_imports(imports),
	m_tokens(std::move(tokens)),
	m_file_name(file_name),
	m_current_module(module_decl),
	m_source_lines(&source_lines)
{
}

Parser::ParseState::ParseState(const std::vector<UseImport>& use_imports)
	: m_current_use_imports(use_imports)
{
}

Parser::PendingDefinition::PendingDefinition(std::string name, LexicalFunctionId function)
	: m_name(std::move(name)),
	m_function(std::move(function))
{
}

Parser::TopLevelDefinition::TopLevelDefinition(int statement, int line, bool is_function)
	: m_statement(statement),
	m_line(line),
	m_is_function(is_function)
{
}

Parser::TopLevelReference::TopLevelReference(int statement, const Token& name)
	: m_statement(statement),
	m_name(name)
{
}

Parser::ParseLifetime::ParseLifetime(ParseState& state)
	: m_state(state),
	m_scope_count(state.m_scopes.size()),
	m_namespace_count(state.m_namespaces.size()),
	m_constraint_count(state.m_active_constraints.size()),
	m_declaring_type_count(state.m_types_being_declared.size()),
	m_pending_definition_count(state.m_pending_definitions.size()),
	m_function(state.m_function),
	m_allow_implicit_generic_params(state.m_allow_implicit_generic_params)
{
}

Parser::ParseLifetime::~ParseLifetime()
{
	m_state.m_scopes.resize(m_scope_count);
	m_state.m_namespaces.resize(m_namespace_count);
	m_state.m_active_constraints.resize(m_constraint_count);
	m_state.m_types_being_declared.resize(m_declaring_type_count);
	m_state.m_pending_definitions.erase(m_state.m_pending_definitions.begin() + static_cast<std::ptrdiff_t>(m_pending_definition_count), m_state.m_pending_definitions.end());
	m_state.m_function = m_function;
	m_state.m_allow_implicit_generic_params = m_allow_implicit_generic_params;
}

Parser::Parser(TokenStream&& tokens,std::string_view file_name, const std::vector<std::string>& source_lines, const ModuleImports& imports, const std::vector<UseImport>& use_imports, const ModuleDeclaration* module_decl)
	: m_context(std::move(tokens), file_name, source_lines, imports, module_decl),
	m_state(use_imports)
{
	for (const auto& [tc_name, metadata] : imports.Typeclasses())
	{
		m_state.m_class_methods[tc_name] = metadata.m_method_names;
		m_state.m_typeclass_type_params[tc_name] = metadata.m_type_param_names;
		m_state.m_typeclass_associated_types[tc_name] = metadata.m_associated_type_names;
		m_state.m_class_instances[tc_name] = metadata.m_instances;
		m_state.m_typeclass_method_types[tc_name] = metadata.m_method_types;
	}
}

std::string Parser::ExtractSymbolName(const std::string& qualified_name) const
{
	size_t last_separator = qualified_name.rfind(NameSeparator);
	if (last_separator != std::string::npos)
	{
		return qualified_name.substr(last_separator + NameSeparator.length());
	}
	else
	{
		return qualified_name;
	}
}

std::string Parser::ExtractQualifier(const std::string& qualified_name) const
{
	size_t last_separator = qualified_name.rfind(NameSeparator);
	if (last_separator != std::string::npos)
	{
		return qualified_name.substr(0u, last_separator);
	}
	else
	{
		return {};
	}
}

Parser::UseImportResolution Parser::ResolveUseImport(const std::string& symbol_name) const
{
	UseImportResolution resolution;

	for (const UseImport& use_import : m_state.m_current_use_imports)
	{
		if
		(
			use_import.m_symbol_name == symbol_name &&
			!std::ranges::contains(resolution.m_conflicting_modules, use_import.m_module_name)
		)
		{
			resolution.m_conflicting_modules.emplace_back(use_import.m_module_name);
		}
	}

	if (resolution.m_conflicting_modules.empty())
	{
		return resolution;
	}

	std::ranges::sort(resolution.m_conflicting_modules);
	if (resolution.m_conflicting_modules.size() == 1u)
	{
		resolution.m_status = UseImportResolutionStatus::Resolved;
		resolution.m_module_name = resolution.m_conflicting_modules[0u];
		return resolution;
	}

	resolution.m_status = UseImportResolutionStatus::Ambiguous;
	return resolution;
}

std::vector<std::string> Parser::ModulesExporting(const std::string& symbol_name) const
{
	return m_context.m_imports.ModulesExporting(DeclaredModuleName(), symbol_name);
}

std::string Parser::BuildUnimportedSymbolError(const std::string& symbol_name, const std::vector<std::string>& module_names) const
{
	std::string message = std::format("'{}' is not in scope. It is exported by ", symbol_name);
	for (size_t index = 0u; index < module_names.size(); index += 1u)
	{
		if (index > 0u)
		{
			message += index + 1u == module_names.size() ? " and " : ", ";
		}
		message += std::format("'{}'", module_names[index]);
	}

	message += std::format(": write '{}{}{}', or bring it into scope with 'use {}.{{{}}}'.",
		module_names.front(), NameSeparator, symbol_name, module_names.front(), symbol_name);
	return message;
}

std::string Parser::BuildAmbiguousUseImportError(const std::string& symbol_name, const std::vector<std::string>& module_names) const
{
	const std::string modules = module_names
		| std::views::transform([](const std::string& module_name) { return std::format("'{}'", module_name); })
		| std::views::join_with(std::string_view(", "))
		| std::ranges::to<std::string>();

	const std::string qualified_example =
		module_names.empty()
		? "ModuleName"s + NameSeparator.data() + symbol_name
		: module_names[0u] + NameSeparator.data() + symbol_name;

	return std::format
	(
		"Ambiguous use import for symbol '{}': imported from modules {}. Use qualified access like '{}' or remove one of the conflicting use imports.",
		symbol_name,
		modules,
		qualified_example
	);
}

std::string_view Parser::DeclaredModuleName() const
{
	return m_context.m_current_module != nullptr && m_context.m_current_module->HasModuleDeclaration()
		? std::string_view(m_context.m_current_module->ModuleName())
		: std::string_view();
}

ImportedSymbolAccess Parser::ResolveImportedSymbolAccess(const std::string& module_name, const std::string& symbol_name) const
{
	return m_context.m_imports.AccessFrom(DeclaredModuleName(), module_name, symbol_name);
}

std::string Parser::BuildImportedSymbolAccessError(const std::string& module_name, const std::string& symbol_name, ImportedSymbolAccess access) const
{
	switch (access)
	{
	case ImportedSymbolAccess::Accessible:
		return {};
	case ImportedSymbolAccess::ModuleNotFound:
		return std::format("Module '{}' not found.", module_name);
	case ImportedSymbolAccess::SymbolNotExported:
		return std::format("Symbol '{}' is not exported by module '{}'.", symbol_name, module_name);
	case ImportedSymbolAccess::PrivateInaccessible:
		if (m_context.m_current_module != nullptr && m_context.m_current_module->HasModuleDeclaration())
		{
			return std::format
			(
				"Symbol '{}' is private to module '{}' and is not accessible from current namespace '{}'.",
				symbol_name,
				module_name,
				TopLevelNamespace(m_context.m_current_module->ModuleName())
			);
		}

		return std::format("Symbol '{}' is private to module '{}' and is not accessible from the current module.", symbol_name, module_name);
	}

	return std::format("Symbol '{}' is not accessible from module '{}'.", symbol_name, module_name);
}

bool Parser::IsBeingDefined(const std::string& name) const
{
	return std::ranges::any_of
	(
		m_state.m_pending_definitions,
		[&name, this](const PendingDefinition& pending)
		{
			return pending.m_name == name && pending.m_function == m_state.m_function;
		}
	);
}

void Parser::CollectTopLevelNames()
{
	int depth = 0;
	bool in_tuple_definition = false;
	const TokenStream::const_iterator end = m_context.m_tokens.cend();
	for (TokenStream::const_iterator it = m_context.m_tokens.cbegin(); it != end; ++it)
	{
		const Token::Name name = it->m_token_name;
		if (in_tuple_definition)
		{
			if (name == Token::Name::IDENTIFIER_LITERAL && !it->IsDiscard())
			{
				m_state.m_top_level_names.insert(it->m_lexeme);
			}
			else if (name == Token::Name::RIGHT_PAREN)
			{
				in_tuple_definition = false;
				depth -= 1;
			}
			continue;
		}

		if ((name == Token::Name::LEFT_BRACE) || (name == Token::Name::LEFT_PAREN) || (name == Token::Name::LEFT_BRACKET))
		{
			depth += 1;
		}
		else if ((name == Token::Name::RIGHT_BRACE) || (name == Token::Name::RIGHT_PAREN) || (name == Token::Name::RIGHT_BRACKET))
		{
			depth -= 1;
		}
		else if ((depth == 0) && (name == Token::Name::FOREIGN))
		{
			CollectForeignName(it);
		}
		else if ((depth == 0) && (name == Token::Name::TYPE) && (std::next(it) != end) && (std::next(it)->m_token_name == Token::Name::IDENTIFIER_LITERAL))
		{
			PredeclareType(static_cast<int>(std::distance(m_context.m_tokens.cbegin(), it)));
		}
		else if ((depth == 0) && (name == Token::Name::DEF) && (std::next(it) != end))
		{
			const Token& following = *std::next(it);
			if (following.m_token_name == Token::Name::IDENTIFIER_LITERAL && !following.IsDiscard())
			{
				m_state.m_top_level_names.insert(following.m_lexeme);
			}
			else if (following.m_token_name == Token::Name::LEFT_PAREN)
			{
				in_tuple_definition = true;
				depth += 1;
				++it;
			}
		}
	}
}

void Parser::CollectForeignName(TokenStream::const_iterator foreign_keyword)
{
	const TokenStream::const_iterator end = m_context.m_tokens.cend();
	TokenStream::const_iterator it = std::next(foreign_keyword);
	if ((it == end) || (it->m_token_name != Token::Name::TEXT_LITERAL) || (std::next(it) == end))
	{
		return;
	}

	++it;
	if (it->m_token_name == Token::Name::IDENTIFIER_LITERAL)
	{
		m_state.m_top_level_names.insert(it->m_lexeme);
	}
}

void Parser::RecordTopLevelDefinition(const MarmotStatement& statement, int statement_index)
{
	if (statement.IsStatement<MarmotStatement::VariableDefinition>())
	{
		const MarmotStatement::VariableDefinition& def = statement.GetStatement<MarmotStatement::VariableDefinition>();
		const bool is_function = (def.m_value != nullptr) && def.m_value->IsExpression<MarmotExpression::Function>();
		m_state.m_top_level_definitions.insert_or_assign(def.m_name.m_lexeme, TopLevelDefinition(statement_index, def.m_name.m_line, is_function));
	}
	else if (statement.IsStatement<MarmotStatement::TupleDefinition>())
	{
		for (const Token& name : statement.GetStatement<MarmotStatement::TupleDefinition>().m_names)
		{
			m_state.m_top_level_definitions.insert_or_assign(name.m_lexeme, TopLevelDefinition(statement_index, name.m_line, false));
		}
	}
	else if (statement.IsStatement<MarmotStatement::FunctionDefinition>())
	{
		const MarmotStatement::FunctionDefinition& defun = statement.GetStatement<MarmotStatement::FunctionDefinition>();
		m_state.m_top_level_definitions.insert_or_assign(defun.m_name.m_lexeme, TopLevelDefinition(statement_index, defun.m_name.m_line, true));
	}
	else if (statement.IsStatement<MarmotStatement::ForeignDefinition>())
	{
		const MarmotStatement::ForeignDefinition& foreign = statement.GetStatement<MarmotStatement::ForeignDefinition>();
		m_state.m_top_level_definitions.insert_or_assign(foreign.m_function_name.m_lexeme, TopLevelDefinition(statement_index, foreign.m_function_name.m_line, false));
	}
}

void Parser::RecordTopLevelReference(const Token& name)
{
	if (m_state.m_top_level_names.contains(name.m_lexeme))
	{
		m_state.m_top_level_references.emplace_back(m_state.m_statement_index, name);
	}
}

// Top-level statements run in order, and a function's body runs when it is
// called. So a statement may use a definition only if it, and everything the
// functions it reaches go on to read, is defined above the statement. That is
// what makes naming a later definition from inside a function safe.
MarmotCompiler::Optional<CompilerError> Parser::CheckDefinitionOrder()
{
	std::unordered_map<int, std::vector<const TopLevelReference*>> references_by_statement;
	for (const TopLevelReference& reference : m_state.m_top_level_references)
	{
		references_by_statement[reference.m_statement].push_back(&reference);
	}

	const std::unordered_set<int> function_statements = [this]()
	{
		std::unordered_set<int> statements;
		for (const std::pair<const std::string, TopLevelDefinition>& definition : m_state.m_top_level_definitions)
		{
			if (definition.second.m_is_function)
			{
				statements.insert(definition.second.m_statement);
			}
		}
		return statements;
	}();

	std::vector<int> executed_statements;
	for (const std::pair<const int, std::vector<const TopLevelReference*>>& entry : references_by_statement)
	{
		if (!function_statements.contains(entry.first))
		{
			executed_statements.push_back(entry.first);
		}
	}
	std::ranges::sort(executed_statements);

	for (const int statement : executed_statements)
	{
		for (const TopLevelReference* reference : references_by_statement.at(statement))
		{
			// Walk what this use reaches: the definition itself, then, through
			// each function on the way, whatever that function names.
			std::vector<std::string> pending{ reference->m_name.m_lexeme };
			std::unordered_set<std::string> visited;
			while (!pending.empty())
			{
				const std::string name = std::move(pending.back());
				pending.pop_back();
				if (!visited.insert(name).second)
				{
					continue;
				}

				const std::unordered_map<std::string, TopLevelDefinition>::const_iterator definition = m_state.m_top_level_definitions.find(name);
				if (definition == m_state.m_top_level_definitions.cend())
				{
					continue;
				}

				if (definition->second.m_statement >= statement)
				{
					std::string message = (name == reference->m_name.m_lexeme)
						? std::format("'{}' is used here before its definition on line {} has run. Move the definition above this.", name, definition->second.m_line)
						: std::format("'{}' is used here, and it reaches '{}', which is not defined until line {}. Move that definition above this.", reference->m_name.m_lexeme, name, definition->second.m_line);
					return GenerateParserError(std::move(message), reference->m_name);
				}

				if (definition->second.m_is_function)
				{
					const std::unordered_map<int, std::vector<const TopLevelReference*>>::const_iterator body = references_by_statement.find(definition->second.m_statement);
					if (body != references_by_statement.cend())
					{
						for (const TopLevelReference* inner : body->second)
						{
							pending.push_back(inner->m_name.m_lexeme);
						}
					}
				}
			}
		}
	}

	return MarmotCompiler::None;
}

std::string Parser::CurrentModuleName() const
{
	return m_context.m_current_module != nullptr ? m_context.m_current_module->ModuleName() : std::string();
}

bool Parser::IsGlobalName(const std::vector<Scope>::const_reverse_iterator& found_scope_it) const
{
	return found_scope_it == std::prev(m_state.m_scopes.crend());
}

bool Parser::IsLocalName(const Scope::VariableTable::const_iterator& found_tbl_it) const
{
	return found_tbl_it->second->m_function == m_state.m_function;
}

bool Parser::IsAtGlobalScope() const
{
	return m_state.m_scopes.size() == 1u;
}

CompilerError Parser::GenerateParserError(std::string&& message, const Token& token)
{
	return GenerateParserError(CompilerErrorCode::None, std::move(message), token);
}

CompilerError Parser::GenerateParserError(CompilerErrorCode code, std::string&& message, const Token& token)
{
	// If the token is from a different file, read that file's source lines
	if (token.m_file_name != m_context.m_file_name && !token.m_file_name.empty())
	{
		std::ifstream file(token.m_file_name);
		if (file.is_open())
		{
			std::vector<std::string> token_source_lines;
			std::string line;
			while (std::getline(file, line))
			{
				token_source_lines.emplace_back(std::move(line));
			}
			return MarmotError::GenerateParserErrorWithContext(code, std::move(message), token, token.m_file_name, token_source_lines);
		}
	}

	return MarmotError::GenerateParserErrorWithContext(code, std::move(message), token, token.m_file_name, *m_context.m_source_lines);
}

bool Parser::IsNoMatchError(const CompilerError& error) const
{
	return error.IsNoMatch();
}

bool Parser::IsAtEnd(ParseState& state)
{
	return Peek(state, 0).m_token_name == Token::Name::END_OF_FILE;
}

bool Parser::Check(ParseState& state, Token::Name type, int offset)
{
	return !IsAtEnd(state) && Peek(state, offset).m_token_name == type;
}

Token& Parser::Peek(ParseState& state, int offset)
{
	return state.m_current_token_index + offset < m_context.m_tokens.Size()
		? m_context.m_tokens[state.m_current_token_index + offset]
		: m_context.m_tokens[m_context.m_tokens.Size() - 1];
}

Token& Parser::Previous(ParseState& state)
{
	return m_context.m_tokens[static_cast<size_t>(state.m_current_token_index - 1)];
}

Token& Parser::Advance(ParseState& state)
{
	if (!IsAtEnd(state))
	{
		state.m_current_token_index += 1;
	}
	return Previous(state);
}

bool Parser::IsAtEnd()
{
	return IsAtEnd(m_state);
}

bool Parser::Check(Token::Name type, int offset)
{
	return Check(m_state, type, offset);
}

Token& Parser::Peek(int offset)
{
	return Peek(m_state, offset);
}

Token& Parser::Previous()
{
	return Previous(m_state);
}

std::vector<Parser::Scope>::const_reverse_iterator Parser::FindTypeScope(std::string& name)
{
	for (std::vector<Parser::Scope>::const_reverse_iterator it = m_state.m_scopes.crbegin(); it != m_state.m_scopes.crend(); ++it)
	{
		if (it->m_defined_types.find(name) != it->m_defined_types.end())
		{
			return it;
		}
	}

	std::string mangled_name;
	for (size_t end_idx : std::views::iota(0u, m_state.m_namespaces.size()))
	{
		std::string stacked_namespace;
		for (size_t idx : std::views::iota(0u, end_idx + 1u))
		{
			stacked_namespace.append(m_state.m_namespaces[idx]).append(NameSeparator);
		}
		mangled_name.append(stacked_namespace).append(name);

		for (std::vector<Parser::Scope>::const_reverse_iterator it = m_state.m_scopes.crbegin(); it != m_state.m_scopes.crend(); ++it)
		{
			if (it->m_defined_types.find(mangled_name) != it->m_defined_types.end())
			{
				name = std::move(mangled_name);
				return it;
			}
		}

		mangled_name.clear();
	}

	return m_state.m_scopes.crend();
}

std::vector<Parser::Scope>::const_reverse_iterator Parser::FindVariableScope(std::string& name)
{
	for (std::vector<Parser::Scope>::const_reverse_iterator it = m_state.m_scopes.crbegin(); it != m_state.m_scopes.crend(); ++it)
	{
		if (it->m_variables.find(name) != it->m_variables.end())
		{
			return it;
		}
	}

	std::string mangled_name;
	for (size_t end_idx : std::views::iota(0u, m_state.m_namespaces.size()))
	{
		std::string stacked_namespace;
		for (size_t idx : std::views::iota(0u, end_idx + 1u))
		{
			stacked_namespace.append(m_state.m_namespaces[idx]).append(NameSeparator);
		}
		mangled_name.append(stacked_namespace).append(name);

		for (std::vector<Parser::Scope>::const_reverse_iterator it = m_state.m_scopes.crbegin(); it != m_state.m_scopes.crend(); ++it)
		{
			if (it->m_variables.find(mangled_name) != it->m_variables.end())
			{
				name = std::move(mangled_name);
				return it;
			}
		}

		mangled_name.clear();
	}

	return m_state.m_scopes.crend();
}

Token& Parser::Advance()
{
	return Advance(m_state);
}

MarmotResult::TokenResult Parser::Consume(Token::Name type, std::string_view message)
{
	if (Check(type, 0))
	{
		return Advance();
	}
	else
	{
		return MarmotCompiler::Failure(GenerateParserError(std::string(message), Peek(0)));
	}
}

std::string Parser::Mangle(std::string_view name)
{
	size_t sep_idx = name.find(NameSeparator);

	if (sep_idx != std::string::npos)
	{
		// Name already has a qualifier (e.g., "UnionName::Member")
		std::string_view top_qualifier = name.substr(0u, sep_idx);
		std::vector<std::string>::const_iterator find_result = std::find(m_state.m_namespaces.cbegin(), m_state.m_namespaces.cend(), top_qualifier);

		if (find_result != m_state.m_namespaces.cend())
		{
			// Found the qualifier in our stack, resolve to absolute path
			std::string mangled_name;

			// Prepend qualifiers up to (but not including) the found one
			for (std::vector<std::string>::const_iterator it = m_state.m_namespaces.cbegin(); it != find_result; ++it)
			{
				mangled_name.append(*it).append(NameSeparator);
			}

			// Append the original name (which already includes the found qualifier)
			mangled_name.append(name);
			return mangled_name;
		}
		else
		{
			// Qualifier not in our stack - return as-is (already absolute)
			return std::string(name);
		}
	}
	else
	{
		// No separator - prepend all current qualification contexts (e.g., union names)
		std::string mangled_name;
		for (const std::string& qualifier : m_state.m_namespaces)
		{
			mangled_name.append(qualifier).append(NameSeparator);
		}
		mangled_name.append(name);
		return mangled_name;
	}
}

MarmotResult::TokenResult Parser::DefineName(Token& name, bool is_variable)
{
	name.m_lexeme = Mangle(name.m_lexeme);

	if (m_state.m_scopes.back().m_defined_names.contains(name.m_lexeme))
	{
		return MarmotCompiler::Failure(GenerateParserError("Name already exists in the current scope", name));
	}

	m_state.m_scopes.back().m_defined_names.emplace(name.m_lexeme);
	if (is_variable)
	{
		m_state.m_scopes.back().m_variables.emplace(name.m_lexeme, BindingId{});
	}

	return name;
}

MarmotResult::TokenResult Parser::DefineBinding(Token& name)
{
	if (name.IsDiscard())
	{
		return name;
	}
	return DefineName(name, true);
}

MarmotCompiler::Optional<BindingId> Parser::RegisterLocalBinding(const std::string& name)
{
	if (IsAtGlobalScope() || name == "_")
	{
		return MarmotCompiler::None;
	}
	const BindingId binding = std::make_shared<BindingIdentity>(m_state.m_function, m_state.m_next_binding_index++);
	m_state.m_scopes.back().m_variables[name] = binding;
	return binding;
}

FunctionParameters Parser::ParametersFor(std::vector<Token> names) const
{
	std::vector<BindingId> bindings;
	bindings.reserve(names.size());
	for (const Token& name : names)
	{
		bindings.push_back(name.IsDiscard() ? nullptr : m_state.m_scopes.back().m_variables.at(name.m_lexeme));
	}
	return FunctionParameters(std::move(names), std::move(bindings), m_state.m_function);
}

MarmotResult::ParserResult Parser::Parse()
{
	MarmotProgramTree programTree;
	std::vector<std::pair<size_t, DeferredOrderingDerivation>> deferred_ordering;
	CollectTopLevelNames();

	while (!IsAtEnd() || !m_pending_statements.empty())
	{
		if (!m_pending_statements.empty())
		{
			PendingStatement& pending = m_pending_statements.front();
			if (auto* statement = std::get_if<std::unique_ptr<MarmotStatement>>(&pending))
			{
				programTree.emplace_back(std::move(*statement));
			}
			else
			{
				deferred_ordering.emplace_back(programTree.size(), std::move(std::get<DeferredOrderingDerivation>(pending)));
				programTree.emplace_back();
			}
			m_pending_statements.pop();
			continue;
		}

		m_state.m_statement_index = static_cast<int>(programTree.size());
		MarmotResult::StatementResult result = ParseDeclaration();
		if (result.HasValue())
		{
			RecordTopLevelDefinition(*result.Value(), m_state.m_statement_index);
			programTree.emplace_back(std::move(result.Value()));
		}
		else
		{
			// Phase 2 recovery policy: Parse() is the single recovery boundary for hard parser errors.
			// The parser still stops at the first hard error after synchronizing to the next declaration starter.
			Synchronize();
			return MarmotCompiler::Failure(MarmotResult::CompilerDiagnostics(std::move(result.Error())));
		}
	}

	if (MarmotCompiler::Optional<CompilerError> instantiation_error = FillDeferredInstantiations())
	{
		return MarmotCompiler::Failure(MarmotResult::CompilerDiagnostics(std::move(instantiation_error.Value())));
	}

	// A derived instance can precede the class and its result union.
	if (!deferred_ordering.empty())
	{
		const Token& target = deferred_ordering.front().second.m_target;
		const auto methods = m_state.m_typeclass_method_types.find(target.m_lexeme);
		if (methods == m_state.m_typeclass_method_types.end())
		{
			return MarmotCompiler::Failure(MarmotResult::CompilerDiagnostics(GenerateParserError("Deriving Orderable requires a visible Orderable class.", target)));
		}
		const MarmotCompiler::Optional<OrderingType> ordering = OrderingType::Resolve(methods->second);
		if (!ordering.HasValue())
		{
			return MarmotCompiler::Failure(MarmotResult::CompilerDiagnostics(GenerateParserError(std::string(OrderingType::s_required_result), target)));
		}
		for (const auto& [index, derivation] : deferred_ordering)
		{
			MarmotResult::StatementResult generated = GenerateDerivedStatement(derivation.m_generator, &*ordering);
			if (!generated.HasValue())
			{
				return MarmotCompiler::Failure(MarmotResult::CompilerDiagnostics(std::move(generated.Error())));
			}
			programTree[index] = std::move(generated.Value());
		}
	}

	if (MarmotCompiler::Optional<CompilerError> order_error = CheckDefinitionOrder())
	{
		return MarmotCompiler::Failure(MarmotResult::CompilerDiagnostics(std::move(order_error.Value())));
	}

	return MarmotResult::ParserResult(std::move(programTree));
}

MarmotResult::TokenResult Parser::MatchNameResolution()
{
	Token resolved_name = Previous();
	std::string& resolved_name_str = resolved_name.m_lexeme;

	// A module's name may be dotted, and then the dots are part of the
	// qualifier: `Math.Vector::Add`. They belong to a module only when '::'
	// follows them; otherwise each dot is a field access on a value.
	int dotted_segments = 0;
	while (Check(Token::Name::SINGLE_DOT, dotted_segments) && Check(Token::Name::IDENTIFIER_LITERAL, dotted_segments + 1))
	{
		dotted_segments += 2;
	}

	if (dotted_segments > 0 && Check(Token::Name::DOUBLE_COLON, dotted_segments))
	{
		for (int consumed = 0; consumed < dotted_segments; consumed += 2)
		{
			Advance();
			Advance();
			resolved_name_str.push_back('.');
			resolved_name_str.append(Previous().m_lexeme);
		}
	}

	while (Match(Token::Name::DOUBLE_COLON))
	{
		// We found the separator, now we must have an identifier
		if (!Match(Token::Name::IDENTIFIER_LITERAL))
		{
			return MarmotCompiler::Failure(GenerateParserError(std::format("Expected identifier after '{}'.", NameSeparator), Previous()));
		}

		resolved_name_str.append(NameSeparator).append(Previous().m_lexeme);
	}

	return resolved_name;
}

Parser& Parser::Synchronize() &
{
	while (!IsAtEnd())
	{
		switch (Peek(0).m_token_name)
		{
			case Token::Name::DEF:
			case Token::Name::CLASS:
			case Token::Name::INSTANCE:
			case Token::Name::FOREIGN:
			case Token::Name::TYPE:
			case Token::Name::ALIAS:
				return *this;
			default:
				Advance();
				break;
		}
	}

	return *this;
}

Parser&& Parser::Synchronize() &&
{
	Synchronize();
	return std::move(*this);
}

const std::vector<CompilerWarning>& Parser::GetWarnings() const
{
	return m_warnings;
}

TypeclassMetadataMap Parser::GetTypeclassMetadata() const
{
	TypeclassMetadataMap result;
	for (const auto& [tc_name, type_params] : m_state.m_typeclass_type_params)
	{
		TypeclassMetadata metadata;
		if (m_state.m_class_methods.contains(tc_name))
		{
			metadata.m_method_names = m_state.m_class_methods.at(tc_name);
		}
		if (m_state.m_typeclass_type_params.contains(tc_name))
		{
			metadata.m_type_param_names = m_state.m_typeclass_type_params.at(tc_name);
		}
		if (m_state.m_typeclass_associated_types.contains(tc_name))
		{
			metadata.m_associated_type_names = m_state.m_typeclass_associated_types.at(tc_name);
		}
		if (m_state.m_class_instances.contains(tc_name))
		{
			metadata.m_instances = m_state.m_class_instances.at(tc_name);
		}
		if (m_state.m_typeclass_method_types.contains(tc_name))
		{
			metadata.m_method_types = m_state.m_typeclass_method_types.at(tc_name);
		}
		result[tc_name] = std::move(metadata);
	}
	return result;
}

bool Parser::IsModuleVisible(std::string_view module_name) const
{
	return m_context.m_imports.ContainsModule(module_name)
		|| (m_context.m_current_module != nullptr && m_context.m_current_module->ModuleName() == module_name);
}
