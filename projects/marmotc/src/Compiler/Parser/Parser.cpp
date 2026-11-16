#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <limits>
#include <queue>
#include <ranges>
#include <sstream>

#include "Bytecode/Format/Format.h"
#include "Compiler/Names/SpecialNames.h"
#include "Compiler/DerivedDeclarationGenerator/DerivedDeclarationGenerator.h"
#include "Compiler/Lexer/Lexer.h"
#include "Parser.h"

using namespace std::string_literals;

namespace
{
	using ParamTuple = std::pair<Token, std::shared_ptr<MarmotType>>;
	using StructMemberTuple = std::tuple<std::shared_ptr<MarmotType>, std::string>;
	using UnionMemberTuple = std::tuple<std::string, std::vector<std::shared_ptr<MarmotType>>, int>;
	using UnionMemberInfo = std::unordered_map<std::string, MarmotType::UnionType::UnionMemberContext>;

	struct ParamSplit
	{
		std::vector<Token> m_params;
		std::vector<std::shared_ptr<MarmotType>> m_types;
	};

	struct StructMemberSplit
	{
		std::vector<std::shared_ptr<MarmotType>> m_types;
		std::vector<std::string> m_names;
	};

	ParamSplit SplitParamTuples(std::vector<ParamTuple>&& tuples)
	{
		ParamSplit result;
		result.m_params.reserve(tuples.size());
		result.m_types.reserve(tuples.size());
		for (ParamTuple& tuple : tuples)
		{
			result.m_params.emplace_back(std::move(tuple.first));
			result.m_types.emplace_back(std::move(tuple.second));
		}
		return result;
	}

	StructMemberSplit SplitStructMemberTuples(std::vector<StructMemberTuple>&& tuples)
	{
		StructMemberSplit result;
		result.m_types.reserve(tuples.size());
		result.m_names.reserve(tuples.size());
		for (StructMemberTuple& tuple : tuples)
		{
			result.m_types.emplace_back(std::move(std::get<0>(tuple)));
			result.m_names.emplace_back(std::move(std::get<1>(tuple)));
		}
		return result;
	}

	// The type a keyword names on its own, such as `Int`.
	std::optional<std::shared_ptr<MarmotType>> KeywordType(Token::Name name)
	{
		switch (name)
		{
		case Token::Name::TEXT:
			return MarmotType::MakeLiteralType<MarmotType::TextType>();
		case Token::Name::FLOAT:
			return MarmotType::MakeLiteralType<MarmotType::FloatType>();
		case Token::Name::INTEGER:
			return MarmotType::MakeLiteralType<MarmotType::IntegerType>();
		case Token::Name::BYTE:
			return MarmotType::MakeLiteralType<MarmotType::ByteType>();
		case Token::Name::WORD:
			return MarmotType::MakeLiteralType<MarmotType::WordType>();
		case Token::Name::BOOL:
			return MarmotType::MakeLiteralType<MarmotType::BoolType>();
		case Token::Name::UNIT:
			return MarmotType::MakeLiteralType<MarmotType::UnitType>();
		case Token::Name::NEVER:
			return MarmotType::MakeLiteralType<MarmotType::NeverType>();
		default:
			return std::nullopt;
		}
	}

	UnionMemberInfo BuildUnionMemberInfo(std::vector<UnionMemberTuple>&& members)
	{
		UnionMemberInfo info;
		info.reserve(members.size());
		for (UnionMemberTuple& member : members)
		{
			std::string& name = std::get<0>(member);
			std::vector<std::shared_ptr<MarmotType>>& types = std::get<1>(member);
			int tag = std::get<2>(member);
			info.emplace(std::move(name), MarmotType::UnionType::UnionMemberContext{ std::move(types), tag });
		}
		return info;
	}

	bool ContainsConstraint(const std::vector<MarmotType::ClassConstraint>& constraints, const MarmotType::ClassConstraint& constraint)
	{
		return std::ranges::any_of
		(
			constraints,
			[&constraint](const MarmotType::ClassConstraint& existing)
			{
				return existing == constraint;
			}
		);
	}

	void AppendUniqueConstraint(std::vector<MarmotType::ClassConstraint>& constraints, MarmotType::ClassConstraint&& constraint)
	{
		if (!ContainsConstraint(constraints, constraint))
		{
			constraints.push_back(std::move(constraint));
		}
	}

	void CollectTypeConstraints(
		const std::shared_ptr<MarmotType>& type,
		std::vector<MarmotType::ClassConstraint>& constraints,
		std::unordered_set<const MarmotType*>& visited
	)
	{
		if (type == nullptr || !visited.insert(type.get()).second)
		{
			return;
		}

		if (type->IsType<MarmotType::ArrayType>())
		{
			CollectTypeConstraints(type->GetType<MarmotType::ArrayType>().m_element_type, constraints, visited);
			return;
		}

		if (type->IsType<MarmotType::CellType>())
		{
			CollectTypeConstraints(type->GetType<MarmotType::CellType>().m_element_type, constraints, visited);
			return;
		}

		if (type->IsType<MarmotType::RangeType>())
		{
			CollectTypeConstraints(type->GetType<MarmotType::RangeType>().m_element_type, constraints, visited);
			return;
		}

		if (type->IsType<MarmotType::TupleType>())
		{
			for (const std::shared_ptr<MarmotType>& element_type : type->GetType<MarmotType::TupleType>().m_element_types)
			{
				CollectTypeConstraints(element_type, constraints, visited);
			}
			return;
		}

		if (type->IsType<MarmotType::FunctionType>())
		{
			const MarmotType::FunctionType& function_type = type->GetType<MarmotType::FunctionType>();
			for (const std::shared_ptr<MarmotType>& param_type : function_type.m_param_types)
			{
				CollectTypeConstraints(param_type, constraints, visited);
			}
			CollectTypeConstraints(function_type.m_return_type, constraints, visited);
			for (const MarmotType::ClassConstraint& constraint : function_type.m_constraints)
			{
				AppendUniqueConstraint(constraints, MarmotType::ClassConstraint(constraint));
			}
			return;
		}

		if (type->IsType<MarmotType::StructType>())
		{
			const MarmotType::StructType& struct_type = type->GetType<MarmotType::StructType>();
			for (const MarmotType::ClassConstraint& constraint : struct_type.m_constraints)
			{
				AppendUniqueConstraint(constraints, MarmotType::ClassConstraint(constraint));
			}
			for (const std::shared_ptr<MarmotType>& member_type : struct_type.m_member_types)
			{
				CollectTypeConstraints(member_type, constraints, visited);
			}
			return;
		}

		if (type->IsType<MarmotType::UnionType>())
		{
			const MarmotType::UnionType& union_type = type->GetType<MarmotType::UnionType>();
			for (const MarmotType::ClassConstraint& constraint : union_type.m_constraints)
			{
				AppendUniqueConstraint(constraints, MarmotType::ClassConstraint(constraint));
			}
			for (const auto& [_, member_ctx] : union_type.m_member_info)
			{
				for (const std::shared_ptr<MarmotType>& member_type : member_ctx.m_member_types)
				{
					CollectTypeConstraints(member_type, constraints, visited);
				}
			}
		}
	}

	std::vector<MarmotType::ClassConstraint> CollectSignatureConstraints(
		const std::vector<std::shared_ptr<MarmotType>>& param_types,
		const std::shared_ptr<MarmotType>& return_type
	)
	{
		std::vector<MarmotType::ClassConstraint> constraints;
		std::unordered_set<const MarmotType*> visited;
		for (const std::shared_ptr<MarmotType>& param_type : param_types)
		{
			CollectTypeConstraints(param_type, constraints, visited);
		}
		CollectTypeConstraints(return_type, constraints, visited);
		return constraints;
	}

	bool IsDecimalIntegerLiteral(const MarmotExpression& expr)
	{
		if (!expr.IsLiteral(MarmotExpression::LiteralKind::Integer))
		{
			return false;
		}

		const std::string& lexeme = expr.GetExpression<MarmotExpression::Literal>().m_token.m_lexeme;
		return !lexeme.empty()
			&& std::isdigit(static_cast<unsigned char>(lexeme[0u])) != 0
			&& !(lexeme.size() >= 2u && lexeme[0u] == '0' && (lexeme[1u] == 'x' || lexeme[1u] == 'X' || lexeme[1u] == 'b' || lexeme[1u] == 'B'));
	}

	std::unique_ptr<MarmotPattern> MakeNumericLiteralPattern(Token token)
	{
		const std::string& lexeme = token.m_lexeme;
		if (lexeme.size() >= 3u && lexeme[0u] == '0' && (lexeme[1u] == 'x' || lexeme[1u] == 'X' || lexeme[1u] == 'b' || lexeme[1u] == 'B'))
		{
			// A lexeme too large for 64 bits is classified as Word; code generation reports it.
			const uint64_t value = ParseUnsignedLiteral(lexeme).value_or(std::numeric_limits<uint64_t>::max());

			if (value <= 0xFF)
			{
				return std::make_unique<MarmotPattern>(MarmotPattern::Literal(token, MarmotPattern::LiteralKind::Byte));
			}
			if (value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
			{
				return std::make_unique<MarmotPattern>(MarmotPattern::Literal(token, MarmotPattern::LiteralKind::Integer));
			}
			return std::make_unique<MarmotPattern>(MarmotPattern::Literal(token, MarmotPattern::LiteralKind::Word));
		}

		return std::make_unique<MarmotPattern>(MarmotPattern::Literal(token, MarmotPattern::LiteralKind::Integer));
	}


}

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

Parser::PendingDefinition::PendingDefinition(std::string name, int function_depth)
	: m_name(std::move(name)),
	m_function_depth(function_depth)
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

Parser::PendingDefinitionGuard::PendingDefinitionGuard(Parser* parser, std::vector<std::string> names)
	: m_parser(parser),
	m_count(names.size())
{
	for (std::string& name : names)
	{
		m_parser->m_state.m_pending_definitions.emplace_back(std::move(name), m_parser->m_state.m_function_depth);
	}
}

Parser::PendingDefinitionGuard::~PendingDefinitionGuard()
{
	std::vector<PendingDefinition>& pending = m_parser->m_state.m_pending_definitions;
	pending.erase(std::prev(pending.end(), static_cast<std::ptrdiff_t>(m_count)), pending.end());
}

Parser::ActiveConstraintGuard::ActiveConstraintGuard(Parser* parser, size_t prev_size)
	: m_parser(parser),
	m_prev_size(prev_size)
{
}

Parser::ActiveConstraintGuard::~ActiveConstraintGuard()
{
	if (m_parser != nullptr)
	{
		m_parser->m_state.m_active_constraints.resize(m_prev_size);
	}
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

MarmotResult::ExpressionResult Parser::ResolveQualifiedName(const Token& name_token, const std::string& mangled_name)
{
	std::string lookup_name = mangled_name;
	std::vector<Scope>::const_reverse_iterator found_scope_it = FindVariableScope(lookup_name);

	if (found_scope_it != m_state.m_scopes.rend())
	{
		Scope::VariableTable::const_iterator find_result = found_scope_it->m_variables.find(lookup_name);

		// Global
		if (IsGlobalName(found_scope_it))
		{
			RecordTopLevelReference(name_token);
			return std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(name_token, MarmotExpression::NameContext::Global()));
		}
		// Local
		else if (IsLocalName(find_result))
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(name_token, MarmotExpression::NameContext::Local(find_result->second.m_relative_index.value())));
		}
		// Cell
		else
		{
			int var_depth = find_result->second.m_function_depth.value();
			int parent_base = (var_depth >= 1) ? m_state.m_function_base_variable_index[static_cast<size_t>(var_depth - 1)] : 0;
			int cell_index = find_result->second.m_absolute_index.value() - parent_base;
			return std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(name_token, MarmotExpression::NameContext::Cell(cell_index)));
		}
	}

	// Defined further down this module.
	if (m_state.m_top_level_names.contains(lookup_name))
	{
		RecordTopLevelReference(name_token);
		return std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(name_token, MarmotExpression::NameContext::Global()));
	}

	// Not found in local scopes - check imported modules (for bare imports)
	const UseImportResolution use_import_resolution = ResolveUseImport(lookup_name);
	if (use_import_resolution.m_status == UseImportResolutionStatus::Ambiguous)
	{
		return std::unexpected(GenerateParserError(BuildAmbiguousUseImportError(lookup_name, use_import_resolution.m_conflicting_modules), name_token));
	}

	if (use_import_resolution.m_status == UseImportResolutionStatus::Resolved)
	{
		const std::string& imported_module_name = use_import_resolution.m_module_name;
		const ImportedSymbolAccess access = ResolveImportedSymbolAccess(imported_module_name, lookup_name);
		if (access == ImportedSymbolAccess::Accessible)
		{
			Token qualified_token = name_token;
			qualified_token.m_lexeme = imported_module_name + NameSeparator.data() + lookup_name;
			return std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(qualified_token, MarmotExpression::NameContext::Global()));
		}

		return std::unexpected(GenerateParserError(BuildImportedSymbolAccessError(imported_module_name, lookup_name, access), name_token));
	}

	if (name_token.m_lexeme == Token::Spelling(Token::Name::REF))
	{
		const Token op(Token::Name::REF, name_token);
		return ParseUnaryLogicalBitwise()
			.transform([&op](std::unique_ptr<MarmotExpression>&& value)
			{
				return std::make_unique<MarmotExpression>(MarmotExpression::UnaryPrefix(op, std::move(value)));
			});
	}

	const std::vector<std::string> exporting_modules = ModulesExporting(lookup_name);
	if (!exporting_modules.empty())
	{
		return std::unexpected(GenerateParserError(BuildUnimportedSymbolError(lookup_name, exporting_modules), name_token));
	}

	std::vector<std::string> matching_typeclasses;
	for (const MarmotType::ClassConstraint& constraint : m_state.m_active_constraints)
	{
		std::unordered_map<std::string, std::unordered_set<std::string>>::const_iterator tc_it = m_state.m_class_methods.find(constraint.m_class_name);
		if (tc_it != m_state.m_class_methods.cend() && tc_it->second.contains(lookup_name))
		{
			matching_typeclasses.emplace_back(constraint.m_class_name);
		}
	}

	if (!matching_typeclasses.empty())
	{
		if (matching_typeclasses.size() == 1u)
		{
			return std::unexpected
			(
				GenerateParserError
				(
					std::format("Unqualified class method '{}'. Use qualified syntax like '{}{}{}'.", lookup_name, matching_typeclasses[0], NameSeparator, lookup_name),
					name_token
				)
			);
		}

		const std::string candidates = matching_typeclasses
			| std::views::transform([&lookup_name](const std::string& class_name) { return std::format("{}{}{}", class_name, NameSeparator, lookup_name); })
			| std::views::join_with(std::string_view(", "))
			| std::ranges::to<std::string>();

		return std::unexpected
		(
			GenerateParserError
			(
				std::format("Unqualified class method '{}'. Use qualified syntax like one of: {}.", lookup_name, candidates),
				name_token
			)
		);
	}

	return std::unexpected(GenerateParserError(CompilerErrorCode::TypeUndefinedName, "Undefined name.", name_token));
}

Parser::ConstructorResolutionResult Parser::ResolveConstructorName(const Token& name_token, const std::string& mangled_name)
{
	for (Scopes::const_reverse_iterator scopes_iter = m_state.m_scopes.crbegin(); scopes_iter != m_state.m_scopes.crend(); ++scopes_iter)
	{
		const Scope& scope = *scopes_iter;
		if (scope.m_union_constructors.contains(mangled_name))
		{
			return ConstructorResolution(std::shared_ptr<MarmotType>(scope.m_union_constructors.at(mangled_name)), std::string(mangled_name), false);
		}
		else if (scope.m_struct_constructors.contains(mangled_name))
		{
			return ConstructorResolution(std::shared_ptr<MarmotType>(scope.m_struct_constructors.at(mangled_name)), std::string(mangled_name), true);
		}
	}

	std::string lookup_base = mangled_name;
	size_t separator_pos = mangled_name.find(NameSeparator);
	if (separator_pos != std::string::npos)
	{
		lookup_base = mangled_name.substr(0u, separator_pos);
	}

	const UseImportResolution use_import_resolution = ResolveUseImport(lookup_base);
	if (use_import_resolution.m_status == UseImportResolutionStatus::Ambiguous)
	{
		return std::unexpected(GenerateParserError(BuildAmbiguousUseImportError(lookup_base, use_import_resolution.m_conflicting_modules), name_token));
	}

	if (use_import_resolution.m_status == UseImportResolutionStatus::Resolved)
	{
		const std::string& module_name = use_import_resolution.m_module_name;
		if (m_context.m_imports.TypesByModule().contains(module_name))
		{
			const TypeEnvironment& env = m_context.m_imports.TypesByModule().at(module_name);
			if (env.contains(lookup_base))
			{
				std::shared_ptr<MarmotType> type = env.at(lookup_base);
				// The signatures hold exported values beside types, so a name is a
				// constructor only when its type is the type of that same name.
				if (type->IsType<MarmotType::UnionType>() && separator_pos != std::string::npos && type->GetType<MarmotType::UnionType>().m_name == lookup_base)
				{
					const MarmotType::UnionType& union_type = type->GetType<MarmotType::UnionType>();
					std::string member_key = union_type.m_name + NameSeparator.data() + mangled_name.substr(separator_pos + NameSeparator.length());
					if (union_type.m_member_info.contains(member_key))
					{
						return ConstructorResolution(std::move(type), std::move(member_key), false);
					}
				}
				else if (type->IsType<MarmotType::StructType>() && lookup_base == mangled_name && type->GetType<MarmotType::StructType>().m_name == lookup_base)
				{
					return ConstructorResolution(std::move(type), std::string(mangled_name), true);
				}
			}
		}
	}

	// `Module::Record(...)` and `Module::Union::Member(...)`: the qualifier names
	// an imported module, and the type it exports carries the constructor.
	if (separator_pos != std::string::npos)
	{
		const std::unordered_map<std::string, TypeEnvironment>::const_iterator module_it = m_context.m_imports.TypesByModule().find(lookup_base);
		if (module_it != m_context.m_imports.TypesByModule().cend())
		{
			const std::string symbol_name = mangled_name.substr(separator_pos + NameSeparator.length());
			const size_t member_pos = symbol_name.find(NameSeparator);
			std::string type_name = member_pos == std::string::npos ? symbol_name : symbol_name.substr(0u, member_pos);
			const TypeEnvironment::const_iterator type_it = module_it->second.find(type_name);
			if (type_it != module_it->second.cend())
			{
				const ImportedSymbolAccess access = ResolveImportedSymbolAccess(lookup_base, type_name);
				if (access != ImportedSymbolAccess::Accessible)
				{
					return std::unexpected(GenerateParserError(BuildImportedSymbolAccessError(lookup_base, type_name, access), name_token));
				}

				// The module's signatures hold its exported values beside its types,
				// so the name is a constructor only when it names a type, not a value.
				if (member_pos == std::string::npos && type_it->second->IsType<MarmotType::StructType>() && type_it->second->GetType<MarmotType::StructType>().m_name == type_name)
				{
					return ConstructorResolution(std::shared_ptr<MarmotType>(type_it->second), std::move(type_name), true);
				}

				if (member_pos != std::string::npos && type_it->second->IsType<MarmotType::UnionType>() && type_it->second->GetType<MarmotType::UnionType>().m_name == type_name)
				{
					const MarmotType::UnionType& union_type = type_it->second->GetType<MarmotType::UnionType>();
					std::string member_key = union_type.m_name + NameSeparator.data() + symbol_name.substr(member_pos + NameSeparator.length());
					if (union_type.m_member_info.contains(member_key))
					{
						return ConstructorResolution(std::shared_ptr<MarmotType>(type_it->second), std::move(member_key), false);
					}
				}
			}
		}
	}

	return std::optional<ConstructorResolution>(std::nullopt);
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

std::string Parser::BuildTypeArgumentCountMismatchMessage(const std::string& type_name, bool is_alias, size_t expected_count, size_t actual_count)
{
	if (!is_alias)
	{
		return std::format("Type argument count mismatch: expected {}, got {}", expected_count, actual_count);
	}

	return std::format("Type argument count mismatch for alias '{}': expected {}, got {}", type_name, expected_count, actual_count);
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

// `Qualifier::Name` in a pattern named no constructor. Say which half is wrong,
// as an expression's lookup of the same name does.
std::string Parser::DescribeUnknownConstructor(const std::string& qualified_name)
{
	std::string qualifier = ExtractQualifier(qualified_name);
	const std::string constructor = ExtractSymbolName(qualified_name);
	const std::unordered_map<std::string, TypeEnvironment>::const_iterator module_types = m_context.m_imports.TypesByModule().find(ExtractQualifier(qualifier));
	const bool is_imported_type = (module_types != m_context.m_imports.TypesByModule().cend()) && module_types->second.contains(ExtractSymbolName(qualifier));
	if ((FindTypeScope(qualifier) != m_state.m_scopes.crend()) || is_imported_type || IsModuleVisible(qualifier))
	{
		return std::format("'{}' has no constructor '{}'.", qualifier, constructor);
	}

	const std::vector<std::string> exporting_modules = ModulesExporting(qualifier);
	if (!exporting_modules.empty())
	{
		return BuildUnimportedSymbolError(qualifier, exporting_modules);
	}

	return std::format("'{}' is not a type or an imported module here, so '{}' names no constructor.", qualifier, qualified_name);
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
			return pending.m_name == name && pending.m_function_depth == m_state.m_function_depth;
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
			if (name == Token::Name::IDENTIFIER_LITERAL)
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
			CollectForeignNames(it);
		}
		else if ((depth == 0) && (name == Token::Name::TYPE) && (std::next(it) != end) && (std::next(it)->m_token_name == Token::Name::IDENTIFIER_LITERAL))
		{
			PredeclareType(static_cast<int>(std::distance(m_context.m_tokens.cbegin(), it)));
		}
		else if ((depth == 0) && (name == Token::Name::DEF) && (std::next(it) != end))
		{
			const Token& following = *std::next(it);
			if (following.m_token_name == Token::Name::IDENTIFIER_LITERAL)
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

// `foreign "symbol" Name: ...;` names one function, and
// `foreign "library" { "symbol" Name: ...; ... }` one per entry.
void Parser::CollectForeignNames(TokenStream::const_iterator foreign_keyword)
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
		return;
	}

	if (it->m_token_name != Token::Name::LEFT_BRACE)
	{
		return;
	}

	for (++it; (it != end) && (it->m_token_name != Token::Name::RIGHT_BRACE); ++it)
	{
		if ((it->m_token_name == Token::Name::TEXT_LITERAL) && (std::next(it) != end) && (std::next(it)->m_token_name == Token::Name::IDENTIFIER_LITERAL))
		{
			m_state.m_top_level_names.insert(std::next(it)->m_lexeme);
		}
	}
}

// A type may be named before its declaration, which is how two types refer to
// each other. The kind is read from the body the way ParseTypeDeclaration will
// read it, and the declaration fills this same object in, so what was named
// earlier is the finished type.
void Parser::PredeclareType(int type_keyword_index)
{
	const TokenStream& tokens = m_context.m_tokens;
	const Token& name = tokens[static_cast<size_t>(type_keyword_index + 1)];
	int cursor = type_keyword_index + 2;

	std::vector<std::string> generic_params;
	if (tokens[static_cast<size_t>(cursor)].m_token_name == Token::Name::LEFT_ANGLE)
	{
		for (cursor += 1; tokens[static_cast<size_t>(cursor)].m_token_name == Token::Name::IDENTIFIER_LITERAL; cursor += 2)
		{
			generic_params.emplace_back(tokens[static_cast<size_t>(cursor)].m_lexeme);
			if (tokens[static_cast<size_t>(cursor + 1)].m_token_name != Token::Name::COMMA)
			{
				break;
			}
		}
	}

	// Past any where clause. A declaration without its '=' is left for the
	// declaration's own parse to report.
	while (tokens[static_cast<size_t>(cursor)].m_token_name != Token::Name::SINGLE_EQUAL)
	{
		const Token::Name token_name = tokens[static_cast<size_t>(cursor)].m_token_name;
		if ((token_name == Token::Name::SINGLE_SEMICOLON) || (token_name == Token::Name::END_OF_FILE))
		{
			return;
		}
		cursor += 1;
	}

	const int body_index = cursor + 1;
	Scope& module_scope = m_state.m_scopes.front();
	std::shared_ptr<MarmotType> placeholder;
	if (tokens[static_cast<size_t>(body_index)].m_token_name == Token::Name::LEFT_BRACE)
	{
		placeholder = MarmotType::MakeStructType(name.m_lexeme, CurrentModuleName(), {}, {}, std::move(generic_params));
		module_scope.m_struct_constructors[name.m_lexeme] = placeholder;
	}
	else
	{
		const int resume_index = m_state.m_current_token_index;
		m_state.m_current_token_index = body_index;
		const bool is_union = TypeBodyHasTopLevelBar();
		m_state.m_current_token_index = resume_index;
		if (is_union)
		{
			placeholder = MarmotType::MakeUnionType(name.m_lexeme, CurrentModuleName(), std::move(generic_params));
			PredeclareVariants(placeholder, body_index);
		}
		else
		{
			placeholder = MarmotType::MakeNewType(name.m_lexeme, CurrentModuleName(), MarmotType::MakeLiteralType<MarmotType::UnitType>(), std::move(generic_params));
		}
	}

	module_scope.m_defined_types[name.m_lexeme] = placeholder;
	m_undeclared_types.insert(placeholder.get());
	m_predeclared_types.emplace(name.m_lexeme, std::move(placeholder));
}

// A constructor is resolved, and a construction given its tag, while parsing,
// so the variants' names and tags are registered with the type: the
// declaration replaces them with the same ones and their field types.
void Parser::PredeclareVariants(const std::shared_ptr<MarmotType>& union_type, int body_index)
{
	const TokenStream& tokens = m_context.m_tokens;
	MarmotType::UnionType& union_type_ref = union_type->GetType<MarmotType::UnionType>();
	int cursor = tokens[static_cast<size_t>(body_index)].m_token_name == Token::Name::SINGLE_BAR ? body_index + 1 : body_index;
	for (int tag = 0; tokens[static_cast<size_t>(cursor)].m_token_name == Token::Name::IDENTIFIER_LITERAL; tag += 1)
	{
		const std::string member_name = union_type_ref.m_name + std::string(NameSeparator) + tokens[static_cast<size_t>(cursor)].m_lexeme;
		union_type_ref.m_member_info.emplace(member_name, MarmotType::UnionType::UnionMemberContext{ {}, tag });
		m_state.m_scopes.front().m_union_constructors[member_name] = union_type;

		cursor += 1;
		if (tokens[static_cast<size_t>(cursor)].m_token_name == Token::Name::LEFT_PAREN)
		{
			for (int depth = 0; ; cursor += 1)
			{
				const Token::Name token_name = tokens[static_cast<size_t>(cursor)].m_token_name;
				depth += (token_name == Token::Name::LEFT_PAREN) ? 1 : ((token_name == Token::Name::RIGHT_PAREN) ? -1 : 0);
				if ((depth == 0) || (token_name == Token::Name::END_OF_FILE))
				{
					break;
				}
			}
			cursor += 1;
		}

		if (tokens[static_cast<size_t>(cursor)].m_token_name != Token::Name::SINGLE_BAR)
		{
			return;
		}
		cursor += 1;
	}
}

// The object a declaration fills: the one registered for it before parsing
// began, or, for a type declared inside a block, the one it just made.
std::shared_ptr<MarmotType> Parser::DeclaredTypeFor(const TypeDeclarationHeader& header, std::shared_ptr<MarmotType>&& fresh_type)
{
	if (header.m_predeclared_type == nullptr)
	{
		return std::move(fresh_type);
	}

	header.m_predeclared_type->m_type = std::move(fresh_type->m_type);
	return header.m_predeclared_type;
}

void Parser::CompleteType(const std::shared_ptr<MarmotType>& type)
{
	m_undeclared_types.erase(type.get());
}

// Whether `type` names `newtype` without a record's or union's fields in
// between: those are nominal, and nothing that erases a newtype looks inside them.
bool Parser::RepresentationNames(const std::shared_ptr<MarmotType>& type, const MarmotType* newtype, std::unordered_set<const MarmotType*>& visited)
{
	if (type.get() == newtype)
	{
		return true;
	}
	if (!visited.insert(type.get()).second)
	{
		return false;
	}

	const auto names_any = [newtype, &visited](const std::vector<std::shared_ptr<MarmotType>>& types)
		{
			return std::ranges::any_of(types, [newtype, &visited](const std::shared_ptr<MarmotType>& inner) { return RepresentationNames(inner, newtype, visited); });
		};

	if (type->IsType<MarmotType::NewType>())
	{
		const MarmotType::NewType& inner = type->GetType<MarmotType::NewType>();
		return RepresentationNames(inner.m_representation, newtype, visited) || names_any(inner.m_type_arguments);
	}
	if (type->IsType<MarmotType::StructType>())
	{
		return names_any(type->GetType<MarmotType::StructType>().m_type_arguments);
	}
	if (type->IsType<MarmotType::UnionType>())
	{
		return names_any(type->GetType<MarmotType::UnionType>().m_type_arguments);
	}
	if (type->IsType<MarmotType::ArrayType>())
	{
		return RepresentationNames(type->GetType<MarmotType::ArrayType>().m_element_type, newtype, visited);
	}
	if (type->IsType<MarmotType::CellType>())
	{
		return RepresentationNames(type->GetType<MarmotType::CellType>().m_element_type, newtype, visited);
	}
	if (type->IsType<MarmotType::ChannelType>())
	{
		return RepresentationNames(type->GetType<MarmotType::ChannelType>().m_element_type, newtype, visited);
	}
	if (type->IsType<MarmotType::RangeType>())
	{
		return RepresentationNames(type->GetType<MarmotType::RangeType>().m_element_type, newtype, visited);
	}
	if (type->IsType<MarmotType::WorkerType>())
	{
		return RepresentationNames(type->GetType<MarmotType::WorkerType>().m_result_type, newtype, visited);
	}
	if (type->IsType<MarmotType::TupleType>())
	{
		return names_any(type->GetType<MarmotType::TupleType>().m_element_types);
	}
	if (type->IsType<MarmotType::FunctionType>())
	{
		const MarmotType::FunctionType& function = type->GetType<MarmotType::FunctionType>();
		return names_any(function.m_param_types) || RepresentationNames(function.m_return_type, newtype, visited);
	}
	return false;
}

bool Parser::Reaches(const std::shared_ptr<MarmotType>& type, const std::function<bool(const MarmotType*)>& is_sought, std::unordered_set<const MarmotType*>& visited)
{
	if (is_sought(type.get()))
	{
		return true;
	}
	if (!visited.insert(type.get()).second)
	{
		return false;
	}

	const auto reaches_any = [&is_sought, &visited](const std::vector<std::shared_ptr<MarmotType>>& types)
		{
			return std::ranges::any_of(types, [&is_sought, &visited](const std::shared_ptr<MarmotType>& inner) { return Reaches(inner, is_sought, visited); });
		};

	if (type->IsType<MarmotType::StructType>())
	{
		const MarmotType::StructType& struct_type = type->GetType<MarmotType::StructType>();
		return reaches_any(struct_type.m_member_types) || reaches_any(struct_type.m_type_arguments);
	}
	if (type->IsType<MarmotType::UnionType>())
	{
		const MarmotType::UnionType& union_type = type->GetType<MarmotType::UnionType>();
		return reaches_any(union_type.m_type_arguments) || std::ranges::any_of
		(
			union_type.m_member_info | std::views::values,
			[&reaches_any](const MarmotType::UnionType::UnionMemberContext& member) { return reaches_any(member.m_member_types); }
		);
	}
	if (type->IsType<MarmotType::NewType>())
	{
		const MarmotType::NewType& new_type = type->GetType<MarmotType::NewType>();
		return Reaches(new_type.m_representation, is_sought, visited) || reaches_any(new_type.m_type_arguments);
	}
	if (type->IsType<MarmotType::ArrayType>())
	{
		return Reaches(type->GetType<MarmotType::ArrayType>().m_element_type, is_sought, visited);
	}
	if (type->IsType<MarmotType::CellType>())
	{
		return Reaches(type->GetType<MarmotType::CellType>().m_element_type, is_sought, visited);
	}
	if (type->IsType<MarmotType::ChannelType>())
	{
		return Reaches(type->GetType<MarmotType::ChannelType>().m_element_type, is_sought, visited);
	}
	if (type->IsType<MarmotType::RangeType>())
	{
		return Reaches(type->GetType<MarmotType::RangeType>().m_element_type, is_sought, visited);
	}
	if (type->IsType<MarmotType::WorkerType>())
	{
		return Reaches(type->GetType<MarmotType::WorkerType>().m_result_type, is_sought, visited);
	}
	if (type->IsType<MarmotType::TupleType>())
	{
		return reaches_any(type->GetType<MarmotType::TupleType>().m_element_types);
	}
	if (type->IsType<MarmotType::FunctionType>())
	{
		const MarmotType::FunctionType& function = type->GetType<MarmotType::FunctionType>();
		return reaches_any(function.m_param_types) || Reaches(function.m_return_type, is_sought, visited);
	}
	return false;
}

// Not whole: undeclared, being declared, or an instantiation not yet filled.
bool Parser::ReachesUnfinishedType(const std::shared_ptr<MarmotType>& type) const
{
	if (m_undeclared_types.empty() && m_state.m_types_being_declared.empty() && m_unfilled_instantiations.empty())
	{
		return false;
	}

	std::unordered_set<const MarmotType*> visited;
	return Reaches
	(
		type,
		[this](const MarmotType* candidate)
		{
			return m_undeclared_types.contains(candidate)
				|| m_unfilled_instantiations.contains(candidate)
				|| std::ranges::any_of(m_state.m_types_being_declared, [candidate](const std::shared_ptr<MarmotType>& declaring) { return declaring.get() == candidate; });
		},
		visited
	);
}

// Each instantiation is filled once nothing its template reaches is still
// waiting. Those left waiting on each other are a type instantiated, through
// the types it holds, inside its own instantiation with other arguments, which
// has no finite form.
std::optional<CompilerError> Parser::FillDeferredInstantiations()
{
	std::vector<const DeferredInstantiation*> waiting;
	std::ranges::transform(m_deferred_instantiations, std::back_inserter(waiting), [](const DeferredInstantiation& deferred) { return &deferred; });

	while (!waiting.empty())
	{
		const std::vector<const DeferredInstantiation*>::iterator ready = std::ranges::find_if
		(
			waiting,
			[this](const DeferredInstantiation* deferred)
			{
				std::unordered_set<const MarmotType*> visited;
				return !Reaches(deferred->m_template, [this](const MarmotType* candidate) { return m_unfilled_instantiations.contains(candidate); }, visited);
			}
		);
		if (ready == waiting.end())
		{
			const DeferredInstantiation& stuck = *waiting.front();
			std::vector<std::string> own_parameters;
			if (stuck.m_template->IsType<MarmotType::UnionType>())
			{
				own_parameters = stuck.m_template->GetType<MarmotType::UnionType>().m_generic_params;
			}
			else if (stuck.m_template->IsType<MarmotType::StructType>())
			{
				own_parameters = stuck.m_template->GetType<MarmotType::StructType>().m_generic_params;
			}
			else
			{
				own_parameters = stuck.m_template->GetType<MarmotType::NewType>().m_generic_params;
			}
			const std::string parameter_list = std::ranges::fold_left
			(
				own_parameters | std::views::drop(1),
				own_parameters.front(),
				[](std::string joined, const std::string& parameter) { return std::move(joined) + ", " + parameter; }
			);
			return GenerateParserError(std::format("'{0}' and the types it holds instantiate each other with arguments other than their own type parameters, which cannot be built. Name it here with its own type parameters, {0}<{1}>, and use the same parameter names in the declarations that name each other.", stuck.m_reference.m_lexeme, parameter_list), stuck.m_reference);
		}

		MarmotType::SubstituteTypeParamsInto((*ready)->m_target, (*ready)->m_template, (*ready)->m_substitutions);
		m_unfilled_instantiations.erase((*ready)->m_target.get());
		waiting.erase(ready);
	}

	return std::nullopt;
}

// A type whose body is not parsed yet, or that holds one, cannot be copied
// with its parameters substituted: the copy would keep the unfinished part
// empty. In a type's body, named with its own parameters, it is the template
// itself, as a union names itself. Otherwise the instantiation is made at the
// end of the parse, when every type is whole.
MarmotResult::TypeResult Parser::InstantiateUnfinishedType(const Token& type_name, const std::shared_ptr<MarmotType>& type_template, const std::vector<std::string>& generic_params, std::vector<std::shared_ptr<MarmotType>>&& type_args)
{
	if (!m_state.m_types_being_declared.empty())
	{
		const bool names_own_parameters = std::ranges::equal
		(
			type_args,
			generic_params,
			[](const std::shared_ptr<MarmotType>& type_arg, const std::string& generic_param)
			{
				return type_arg->IsType<MarmotType::GenericParam>() && (type_arg->GetType<MarmotType::GenericParam>().m_name == generic_param);
			}
		);
		if (names_own_parameters)
		{
			return type_template;
		}
	}

	std::unordered_map<std::string, std::shared_ptr<MarmotType>> substitutions;
	for (size_t index = 0uz; index < generic_params.size(); index += 1uz)
	{
		substitutions.emplace(generic_params[index], std::move(type_args[index]));
	}

	std::shared_ptr<MarmotType> instantiation = std::make_shared<MarmotType>(*type_template);
	m_unfilled_instantiations.insert(instantiation.get());
	m_deferred_instantiations.emplace_back(instantiation, type_template, std::move(substitutions), type_name);
	return instantiation;
}

Parser::DeferredInstantiation::DeferredInstantiation(std::shared_ptr<MarmotType> target, std::shared_ptr<MarmotType> type_template, std::unordered_map<std::string, std::shared_ptr<MarmotType>> substitutions, Token reference)
	: m_target(std::move(target)), m_template(std::move(type_template)), m_substitutions(std::move(substitutions)), m_reference(std::move(reference))
{
}

Parser::DeclaringTypeScope::DeclaringTypeScope(std::vector<std::shared_ptr<MarmotType>>& stack, const std::shared_ptr<MarmotType>& type)
	: m_stack(stack)
{
	m_stack.push_back(type);
}

Parser::DeclaringTypeScope::~DeclaringTypeScope()
{
	m_stack.pop_back();
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
std::optional<CompilerError> Parser::CheckDefinitionOrder()
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

	return std::nullopt;
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
	return m_state.m_function_depth == 0 || found_tbl_it->second.m_function_depth == m_state.m_function_depth;
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
		return std::unexpected(GenerateParserError(std::string(message), Peek(0)));
	}
}

MarmotResult::TokenResult Parser::ConsumeTypeRightAngle(std::string_view message)
{
	if (Check(Token::Name::RIGHT_ANGLE, 0))
	{
		return Advance();
	}

	if (Check(Token::Name::RIGHT_SHIFT, 0))
	{
		const Token anchor = Peek(0);
		Token first_right_angle(Token::Name::RIGHT_ANGLE, anchor);
		Token second_right_angle(Token::Name::RIGHT_ANGLE, anchor);

		m_context.m_tokens[m_state.m_current_token_index] = std::move(first_right_angle);

		// The insert shifts later tokens, so the enclosing level closes on the second
		// '>'. TokenStream keeps each Token at a stable address precisely so that the
		// parse frames holding Token& across this call survive the insert.
		TokenStream split_tokens;
		split_tokens.AddToken(std::move(second_right_angle));
		m_context.m_tokens.Insert(m_context.m_tokens.begin() + m_state.m_current_token_index + 1, std::move(split_tokens));

		return Advance();
	}

	return std::unexpected(GenerateParserError(std::string(message), Peek(0)));
}

MarmotResult::TokenResult Parser::ConsumeReturnTypeSeparator(std::string_view message)
{
	if (Check(Token::Name::THIN_ARROW, 0))
	{
		return Advance();
	}

	return std::unexpected(GenerateParserError(std::string(message), Peek(0)));
}

Parser& Parser::BeginScope() &
{
	m_state.m_scopes.emplace_back();
	return *this;
}

Parser&& Parser::BeginScope() &&
{
	m_state.m_scopes.emplace_back();
	return std::move(*this);
}

void Parser::EndScope()
{
	const int scope_local_count = static_cast<int>(m_state.m_scopes.back().m_variables.size());
	m_state.m_total_locals_in_curr_scope -= scope_local_count;
	m_state.m_total_variables -= scope_local_count;
	m_state.m_scopes.pop_back();
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
		return std::unexpected(GenerateParserError("Name already exists in the current scope", name));
	}

	m_state.m_scopes.back().m_defined_names.emplace(name.m_lexeme);
	if (is_variable)
	{
		m_state.m_scopes.back().m_variables.emplace(name.m_lexeme, VariableContext());
	}

	return name;
}

std::optional<int> Parser::RegisterOrUpdateLocalVariable(const std::string& name)
{
	std::optional<int> local_index = std::nullopt;

	if (!IsAtGlobalScope())
	{
		m_state.m_scopes.back().m_variables[name] = VariableContext(m_state.m_total_locals_in_curr_scope++, m_state.m_total_variables++, m_state.m_function_depth);
		local_index.emplace(m_state.m_scopes.back().m_variables[name].m_relative_index.value());
	}

	return local_index;
}

std::optional<int> Parser::BinaryOperatorClass(Token::Name name)
{
	switch (name)
	{
	case Token::Name::STAR:
	case Token::Name::SLASH:
	case Token::Name::PERCENT:
		return 0;
	case Token::Name::SINGLE_PLUS:
	case Token::Name::SINGLE_MINUS:
		return 1;
	case Token::Name::DOUBLE_PLUS:
		return 2;
	case Token::Name::LEFT_SHIFT:
	case Token::Name::RIGHT_SHIFT:
		return 3;
	case Token::Name::LEFT_ANGLE:
	case Token::Name::LESS_EQUAL:
	case Token::Name::RIGHT_ANGLE:
	case Token::Name::GREATER_EQUAL:
		return 4;
	case Token::Name::BANG_EQUAL:
	case Token::Name::DOUBLE_EQUAL:
		return 5;
	case Token::Name::SINGLE_AMPERSAND:
		return 6;
	case Token::Name::CARET:
		return 7;
	case Token::Name::SINGLE_BAR:
		return 8;
	case Token::Name::DOUBLE_AMPERSAND:
		return 9;
	case Token::Name::DOUBLE_BAR:
		return 10;
	default:
		return std::nullopt;
	}
}

std::optional<CompilerError> Parser::CheckOperatorMixing(const Token& op, const MarmotExpression& operand)
{
	// Two operators of different precedence in one expression bind by a table the
	// reader has to remember. Parentheses say it instead, so they are required:
	// `a + b * c` is written `a + (b * c)`. A chain of one operator, or of operators
	// that bind equally like `+` and `-`, needs none.
	if (!operand.IsExpression<MarmotExpression::Binary>())
	{
		return std::nullopt;
	}

	const std::optional<int> outer_class = BinaryOperatorClass(op.m_token_name);
	const Token& inner_op = operand.GetExpression<MarmotExpression::Binary>().m_op;
	const std::optional<int> inner_class = BinaryOperatorClass(inner_op.m_token_name);
	if (!outer_class.has_value() || !inner_class.has_value() || outer_class.value() == inner_class.value())
	{
		return std::nullopt;
	}

	return GenerateParserError(std::format("'{}' and '{}' do not bind equally, so mixing them needs parentheses: write the '{}' operand in its own parentheses.", op.m_lexeme, inner_op.m_lexeme, inner_op.m_lexeme), op);
}

MarmotResult::ExpressionResult Parser::ParseFactor()
{
	return ParseBinary(&Parser::ParseUnaryLogicalBitwise, Token::Name::STAR, Token::Name::SLASH, Token::Name::PERCENT);
}

MarmotResult::ExpressionResult Parser::ParseShift()
{
	return ParseBinary(&Parser::ParseTerm, Token::Name::LEFT_SHIFT, Token::Name::RIGHT_SHIFT);
}

MarmotResult::ExpressionResult Parser::ParseRange()
{
	return ParseShift()
		.and_then
		(
			[this](std::unique_ptr<MarmotExpression>&& start) -> MarmotResult::ExpressionResult
			{
				if (!Match(Token::Name::DOUBLE_DOT))
				{
					return start;
				}

				Token first_range_op = Previous();

				return ParseShift()
					.and_then
					(
						[this, &first_range_op, &start](std::unique_ptr<MarmotExpression>&& middle) -> MarmotResult::ExpressionResult
						{
							if (!Match(Token::Name::DOUBLE_DOT))
							{
								return std::make_unique<MarmotExpression>(MarmotExpression::RangeBinary(first_range_op, std::move(start), std::move(middle)));
							}

							Token second_range_op = Previous();

							return ParseShift()
								.and_then
								(
									[&first_range_op, &second_range_op, &start, &middle](std::unique_ptr<MarmotExpression>&& end) -> MarmotResult::ExpressionResult
									{
										return std::make_unique<MarmotExpression>(MarmotExpression::RangeTernary(first_range_op, second_range_op, std::move(start), std::move(middle), std::move(end)));
									}
								);
						}
					);
			}
		);
}

MarmotResult::ExpressionResult Parser::ParseTerm()
{
	return ParseBinary(&Parser::ParseFactor, Token::Name::SINGLE_PLUS, Token::Name::DOUBLE_PLUS, Token::Name::SINGLE_MINUS);
}

MarmotResult::ExpressionResult Parser::ParseComparison()
{
	return ParseBinary(&Parser::ParseRange, Token::Name::LEFT_ANGLE, Token::Name::LESS_EQUAL, Token::Name::RIGHT_ANGLE, Token::Name::GREATER_EQUAL);
}

MarmotResult::ExpressionResult Parser::ParseEquality()
{
	return ParseBinary(&Parser::ParseComparison, Token::Name::BANG_EQUAL, Token::Name::DOUBLE_EQUAL);
}

MarmotResult::ExpressionResult Parser::ParseBitwiseAnd()
{
	return ParseBinary(&Parser::ParseEquality, Token::Name::SINGLE_AMPERSAND);
}

MarmotResult::ExpressionResult Parser::ParseBitwiseXor()
{
	return ParseBinary(&Parser::ParseBitwiseAnd, Token::Name::CARET);
}

MarmotResult::ExpressionResult Parser::ParseBitwiseOr()
{
	return ParseBinary(&Parser::ParseBitwiseXor, Token::Name::SINGLE_BAR);
}

MarmotResult::ExpressionResult Parser::ParseUnaryLogicalBitwise()
{
	if (Match(Token::Name::BANG, Token::Name::TILDE, Token::Name::HASH, Token::Name::STAR))
	{
		Token& op = Previous();
		return ParseUnaryLogicalBitwise()
			.and_then
			(
				[&op](std::unique_ptr<MarmotExpression>&& right) -> MarmotResult::ExpressionResult
				{
					return std::make_unique<MarmotExpression>(MarmotExpression::UnaryPrefix(op, std::move(right)));
				}
			);
	}

	return ParseUnaryArithmetic();
}

MarmotResult::ExpressionResult Parser::ParseUnaryArithmetic()
{
	if (Match(Token::Name::LEFT_ARROW, Token::Name::SINGLE_MINUS, Token::Name::SINGLE_PLUS))
	{
		Token& op = Previous();
		return ParseUnaryLogicalBitwise()
			.and_then
			(
				[&op](std::unique_ptr<MarmotExpression>&& right) -> MarmotResult::ExpressionResult
				{
					if (op.m_token_name == Token::Name::LEFT_ARROW)
					{
						return std::make_unique<MarmotExpression>(MarmotExpression::Receive(op, std::move(right)));
					}

					// The smallest Int has no positive counterpart, so `-9223372036854775808` is one
					// literal rather than a negation of a literal that is out of range.
					if (op.m_token_name == Token::Name::SINGLE_MINUS && IsDecimalIntegerLiteral(*right))
					{
						MarmotExpression::Literal& literal = right->GetExpression<MarmotExpression::Literal>();
						literal.m_token.m_lexeme.insert(0u, 1u, '-');
						return std::move(right);
					}

					return std::make_unique<MarmotExpression>(MarmotExpression::UnaryPrefix(op, std::move(right)));
				}
			);
	}
	else
	{
		return ParseCall();
	}
}

MarmotResult::ExpressionResult Parser::RejectExponent(std::unique_ptr<MarmotExpression>&& literal)
{
	const Token& number = literal->GetExpression<MarmotExpression::Literal>().m_token;
	const Token& next = Peek(0);
	const bool adjacent = next.m_token_name == Token::Name::IDENTIFIER_LITERAL
		&& next.m_line == number.m_line
		&& number.m_column.has_value() && next.m_column.has_value()
		&& next.m_column.value() == number.m_column.value() + static_cast<int>(number.m_source_length.value_or(number.m_lexeme.size()));
	const bool looks_like_exponent = !next.m_lexeme.empty()
		&& (next.m_lexeme[0u] == 'e' || next.m_lexeme[0u] == 'E')
		&& std::all_of(next.m_lexeme.begin() + 1, next.m_lexeme.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; });
	if (adjacent && looks_like_exponent)
	{
		return std::unexpected(GenerateParserError("A number has no exponent form: write the digits out, as in 1000000.0 or 0.000001, or compute it with Math::Pow.", next));
	}

	return std::move(literal);
}

MarmotResult::ExpressionResult Parser::ParseExpression()
{
	return ParseAs()
		.and_then([this](std::unique_ptr<MarmotExpression>&& cell) -> MarmotResult::ExpressionResult
		{
			if (!Match(Token::Name::COLON_EQUAL))
			{
				return std::move(cell);
			}
			const Token& op = Previous();
			return ParseExpression()
				.transform([&cell, &op](std::unique_ptr<MarmotExpression>&& value)
				{
					return std::make_unique<MarmotExpression>(MarmotExpression::Binary(op, std::move(cell), std::move(value)));
				});
		});
}

MarmotResult::ExpressionResult Parser::ParseAs()
{
	return ParseLogicalOr()
		.and_then
		(
			[this](std::unique_ptr<MarmotExpression>&& expr) ->MarmotResult::ExpressionResult
			{
				while (Match(Token::Name::AS))
				{
					Token& as = Previous();
					MarmotResult::TypeResult type = ParseType();
					if (!type.has_value())
					{
						return std::unexpected(type.error());
					}

					expr = std::make_unique<MarmotExpression>(MarmotExpression::As(as, std::move(type.value()), std::move(expr)));
				}

				return expr;
			}
		);
}

MarmotResult::ExpressionResult Parser::ParseArrayAccessHelper(std::unique_ptr<MarmotExpression>&& arr_var)
{
	Token& op = Previous();

	return Consume(Token::Name::LEFT_BRACKET, "Expected '[' before index.")
		.and_then
		(
			[&op, &arr_var, this](Token&&) ->MarmotResult::ExpressionResult
			{
				return ParseLogicalOr()
					.and_then
					(
						[&op, &arr_var, this](std::unique_ptr<MarmotExpression>&& index) ->MarmotResult::ExpressionResult
						{
							return Consume(Token::Name::RIGHT_BRACKET, "Expected ']' after index.")
								.and_then
								(
									[&op, &arr_var, index = std::move(index)](Token&&) mutable ->MarmotResult::ExpressionResult
									{
										return std::make_unique<MarmotExpression>(MarmotExpression::IndexAccess(op, std::move(index), std::move(arr_var)));
									}
								);
						}
					);
			}
		);
}

MarmotResult::ExpressionResult Parser::ParseArrayAccess()
{
	return ParsePrimary()
		.and_then
		(
			[this](std::unique_ptr<MarmotExpression>&& arr_var) -> MarmotResult::ExpressionResult
			{
				// Check if we can do array access: next token is '['.
				bool has_bracket_for_access = Check(Token::Name::LEFT_BRACKET, 0);

				return has_bracket_for_access
					? ParseArrayAccessHelper(std::move(arr_var))
					: std::move(arr_var);
			}
		);
}

MarmotResult::ExpressionResult Parser::ParsePostfixChain(std::unique_ptr<MarmotExpression>&& expr)
{
	if (Match(Token::Name::LEFT_PAREN))
	{
		return FinishCall(std::move(expr))
			.and_then
			(
				[this](std::unique_ptr<MarmotExpression>&& called) -> MarmotResult::ExpressionResult
				{
					return ParsePostfixChain(std::move(called));
				}
			);
	}
	else if (Match(Token::Name::SINGLE_DOT))
	{
		return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected identifier after '.'.")
			.and_then
			(
				[&expr, this](Token&& name) -> MarmotResult::ExpressionResult
				{
					return ParsePostfixChain(std::make_unique<MarmotExpression>(MarmotExpression::MemberAccess(name, std::move(expr))));
				}
			);
	}
	else if (Check(Token::Name::LEFT_BRACKET, 0))
	{
		return ParseArrayAccessHelper(std::move(expr))
			.and_then
			(
				[this](std::unique_ptr<MarmotExpression>&& indexed) -> MarmotResult::ExpressionResult
				{
					return ParsePostfixChain(std::move(indexed));
				}
			);
	}
	else
	{
		return expr;
	}
}

MarmotResult::ExpressionResult Parser::ParseCall()
{
	return ParseArrayAccess()
		.and_then
		(
			[this](std::unique_ptr<MarmotExpression>&& expr)
			{
				return ParsePostfixChain(std::move(expr));
			}
		);
}

MarmotResult::ExpressionResult Parser::FinishCall(std::unique_ptr<MarmotExpression>&& callee)
{
	return ParseDelimitedZeroOrMoreLimited<std::unique_ptr<MarmotExpression>>
		(
			[this]() { return ParseExpression(); },
			[this]() { return Consume(Token::Name::COMMA, "Expected ',' after expression."); },
			[this]() { return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after arguments."); }
		)
		.and_then
		(
			[&callee, this](std::vector<std::unique_ptr<MarmotExpression>>&& arguments) ->MarmotResult::ExpressionResult
			{
				return std::make_unique<MarmotExpression>(MarmotExpression::Call(Previous(), std::move(callee), std::move(arguments)));
			}
		);
}

MarmotResult::ExpressionResult Parser::FinishConstruct(Token&& constructor_token, std::shared_ptr<MarmotType>&& constructed_type, bool is_struct)
{
	MarmotResult::TokenResult left_paren = Consume(Token::Name::LEFT_PAREN, "Expected '(' after type.");
	if (!left_paren.has_value())
	{
		return std::unexpected(std::move(left_paren.error()));
	}

	MarmotResult::Result<std::vector<std::unique_ptr<MarmotExpression>>> arguments = ParseDelimitedZeroOrMoreLimited<std::unique_ptr<MarmotExpression>>
		(
			[this]() { return ParseExpression(); },
			[this]() { return Consume(Token::Name::COMMA, "Expected ',' after expression."); },
			[this]() { return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after arguments."); }
		);
	if (!arguments.has_value())
	{
		return std::unexpected(std::move(arguments.error()));
	}

	if (is_struct)
	{
		return std::make_unique<MarmotExpression>(MarmotExpression::Construct(constructor_token, std::move(arguments.value()), std::move(constructed_type), false, MarmotExpression::Construct::Struct{}));
	}

	const int tag = constructed_type->GetType<MarmotType::UnionType>().m_member_info.at(constructor_token.m_lexeme).m_tag;
	return std::make_unique<MarmotExpression>(MarmotExpression::Construct(constructor_token, std::move(arguments.value()), std::move(constructed_type), false, MarmotExpression::Construct::Union(tag)));
}

// The record's fields by name, in any order, each exactly once. The type
// checker places them, since a record declared further down has no fields yet.
MarmotResult::ExpressionResult Parser::FinishNamedConstruct(Token&& constructor_token, std::shared_ptr<MarmotType>&& constructed_type)
{
	MarmotResult::TokenResult left_brace = Consume(Token::Name::LEFT_BRACE, "Expected '{' after the record type.");
	if (!left_brace.has_value())
	{
		return std::unexpected(std::move(left_brace.error()));
	}

	std::vector<Token> names;
	std::vector<std::unique_ptr<MarmotExpression>> values;
	do
	{
		if (Check(Token::Name::RIGHT_BRACE, 0))
		{
			break;
		}

		MarmotResult::TokenResult name = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected a field name.");
		if (!name.has_value())
		{
			return std::unexpected(std::move(name.error()));
		}
		if (std::ranges::contains(names, name->m_lexeme, &Token::m_lexeme))
		{
			return std::unexpected(GenerateParserError(std::format("Field '{}' is given more than once in this construction.", name->m_lexeme), name.value()));
		}

		MarmotResult::TokenResult equal = Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after the field name.");
		if (!equal.has_value())
		{
			return std::unexpected(std::move(equal.error()));
		}
		MarmotResult::ExpressionResult value = ParseExpression();
		if (!value.has_value())
		{
			return std::unexpected(std::move(value.error()));
		}
		names.push_back(std::move(name.value()));
		values.push_back(std::move(value.value()));
	} while (Match(Token::Name::COMMA));

	MarmotResult::TokenResult right_brace = Consume(Token::Name::RIGHT_BRACE, "Expected '}' after the fields.");
	if (!right_brace.has_value())
	{
		return std::unexpected(std::move(right_brace.error()));
	}

	std::unique_ptr<MarmotExpression> construct = std::make_unique<MarmotExpression>(MarmotExpression::Construct(constructor_token, std::move(values), std::move(constructed_type), false, MarmotExpression::Construct::Struct{}));
	construct->GetExpression<MarmotExpression::Construct>().m_field_names = std::move(names);
	return construct;
}

MarmotResult::ExpressionResult Parser::ParsePrimary()
{
	if (Match(Token::Name::LEFT_BRACE))
	{
		return ProbeRecordUpdate()
			? ParseRecordUpdate()
			: ParseBlockExpression();
	}
	else if (Match(Token::Name::LEFT_PAREN))
	{
		Token left_paren = Previous();

		if (Match(Token::Name::RIGHT_PAREN))
		{
			// Empty tuple is unit
			return std::make_unique<MarmotExpression>(MarmotExpression::Literal(Previous(), MarmotExpression::LiteralKind::Unit));
		}

		return ParseExpression()
			.and_then
			(
				[this, left_paren](std::unique_ptr<MarmotExpression>&& first_expr) -> MarmotResult::ExpressionResult
				{
					if (Match(Token::Name::COMMA))
					{
						// It's a tuple - parse remaining elements
						std::vector<std::unique_ptr<MarmotExpression>> elements;
						elements.push_back(std::move(first_expr));

						// Parse remaining tuple elements
						do
						{
							MarmotResult::ExpressionResult elem_result = ParseExpression();
							if (!elem_result)
							{
								return elem_result;
							}
							elements.push_back(std::move(elem_result.value()));
						} while (Match(Token::Name::COMMA));

						return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after tuple elements.")
							.and_then
							(
								[&elements, left_paren](Token&&) -> MarmotResult::ExpressionResult
								{
									return std::make_unique<MarmotExpression>(MarmotExpression::Tuple(left_paren, std::move(elements)));
								}
							);
					}
					else
					{
						return Consume(Token::Name::RIGHT_PAREN, "Expected right parentheses.")
							.and_then
							(
								[&first_expr](Token&&) -> MarmotResult::ExpressionResult
								{
									return std::make_unique<MarmotExpression>(MarmotExpression::Group(std::move(first_expr)));
								}
							);
					}
				}
			);
	}
	else if (Match(Token::Name::IDENTIFIER_LITERAL))
	{
		return MatchNameResolution()
			.and_then
			(
				[this](Token&& variable) -> MarmotResult::ExpressionResult
				{
					std::string mangled_name = Mangle(variable.m_lexeme);
					std::string symbol_name = ExtractSymbolName(variable.m_lexeme);
					std::string qualifier = ExtractQualifier(variable.m_lexeme);

					if (qualifier.empty() && (IsBeingDefined(symbol_name) || IsBeingDefined(mangled_name)))
					{
						return std::unexpected
						(
							GenerateParserError
							(
								std::format("'{}' has no value here: a definition cannot read the name it is defining. Rename one of the two, or read it inside a function, which runs after the definition finishes.", symbol_name),
								variable
							)
						);
					}

					// A constructor may be written without 'new': `Point(1, 2)` and
					// `Option::Some(5)` build the same Construct node `new` builds. A name
					// bound to a variable still wins, so this only adds spellings that used
					// to be errors. The check precedes import resolution because a union
					// constructor is qualified by its union, not by a module, and import
					// resolution would report a missing module instead.
					std::string variable_lookup_name = mangled_name;
					if (FindVariableScope(variable_lookup_name) == m_state.m_scopes.crend())
					{
						ConstructorResolutionResult resolution = ResolveConstructorName(variable, mangled_name);
						if (!resolution.has_value())
						{
							return std::unexpected(std::move(resolution.error()));
						}

						if (resolution.value().has_value())
						{
							ConstructorResolution& constructor = resolution.value().value();
							Token constructor_token = variable;
							constructor_token.m_lexeme = constructor.m_constructor_name;

							// `Point { x = 1 }`: a block cannot start with `name =`, since
							// nothing assigns, so this is never a type before a block.
							if (constructor.m_is_struct && Check(Token::Name::LEFT_BRACE, 0) && Check(Token::Name::IDENTIFIER_LITERAL, 1) && Check(Token::Name::SINGLE_EQUAL, 2))
							{
								return FinishNamedConstruct(std::move(constructor_token), std::move(constructor.m_type));
							}

							if (!Check(Token::Name::LEFT_PAREN, 0))
							{
								return std::unexpected
								(
									GenerateParserError
									(
										std::format("Constructor '{}' cannot be used as a value; it is monomorphised at each construction site and has no single procedure to pass around. Write '{}(...)' to construct, or wrap it in a lambda.", constructor.m_constructor_name, constructor.m_constructor_name),
										constructor_token
									)
								);
							}

							return FinishConstruct(std::move(constructor_token), std::move(constructor.m_type), constructor.m_is_struct);
						}
					}

					// Check if this is a module-qualified name
					if (!qualifier.empty())
					{
						// Qualified class method call (e.g., Show::show)
						for (const MarmotType::ClassConstraint& constraint : m_state.m_active_constraints)
						{
							if (constraint.m_class_name != qualifier)
							{
								continue;
							}

							std::unordered_map<std::string, std::unordered_set<std::string>>::const_iterator tc_it = m_state.m_class_methods.find(constraint.m_class_name);
							if (tc_it != m_state.m_class_methods.cend() && tc_it->second.contains(symbol_name))
							{
								return std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(variable, MarmotExpression::NameContext::Global()));
							}
						}

						std::unordered_map<std::string, std::unordered_set<std::string>>::const_iterator concrete_tc_it = m_state.m_class_methods.find(qualifier);
						if (concrete_tc_it != m_state.m_class_methods.cend() && concrete_tc_it->second.contains(symbol_name))
						{
							return std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(variable, MarmotExpression::NameContext::Global()));
						}

						const ImportedSymbolAccess access = ResolveImportedSymbolAccess(qualifier, symbol_name);
						if (access == ImportedSymbolAccess::Accessible)
						{
							// Keep the fully qualified name so lowering can identify imports
							return std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(variable, MarmotExpression::NameContext::Global()));
						}

						return std::unexpected(GenerateParserError(BuildImportedSymbolAccessError(qualifier, symbol_name, access), variable));
					}

					return ResolveQualifiedName(variable, mangled_name);
				}
			);
	}
	else if (Match(Token::Name::FUNCTION))
	{
		return ParseFunctionExpression();
	}
	else if (Match(Token::Name::TRUE, Token::Name::FALSE))
	{
		return std::make_unique<MarmotExpression>(MarmotExpression::Literal(Previous(), MarmotExpression::LiteralKind::Bool));
	}
	else if (Match(Token::Name::FLOAT_LITERAL))
	{
		return RejectExponent(std::make_unique<MarmotExpression>(MarmotExpression::Literal(Previous(), MarmotExpression::LiteralKind::Float)));
	}
	else if (Match(Token::Name::INTEGER_LITERAL))
	{
		Token& token = Previous();
		const std::string& lexeme = token.m_lexeme;

		// Check if it's a hex or binary literal
		if (lexeme.size() >= 3 && lexeme[0u] == '0' && (lexeme[1u] == 'x' || lexeme[1u] == 'X' || lexeme[1u] == 'b' || lexeme[1u] == 'B'))
		{
			// A lexeme too large for 64 bits is classified as Word; code generation reports it.
			const uint64_t value = ParseUnsignedLiteral(lexeme).value_or(std::numeric_limits<uint64_t>::max());

			// Determine type based on value
			if (value <= 0xFF)
			{
				// Fits in Byte (0-255)
				return std::make_unique<MarmotExpression>(MarmotExpression::Literal(token, MarmotExpression::LiteralKind::Byte));
			}
			else if (value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
			{
				// Fits in signed Int
				return std::make_unique<MarmotExpression>(MarmotExpression::Literal(token, MarmotExpression::LiteralKind::Integer));
			}
			else
			{
				// Needs Word (unsigned 64-bit)
				return std::make_unique<MarmotExpression>(MarmotExpression::Literal(token, MarmotExpression::LiteralKind::Word));
			}
		}
		else
		{
			// Decimal literal - an Integer literal
			return RejectExponent(std::make_unique<MarmotExpression>(MarmotExpression::Literal(token, MarmotExpression::LiteralKind::Integer)));
		}
	}
	else if (Match(Token::Name::TEXT_LITERAL))
	{
		return std::make_unique<MarmotExpression>(MarmotExpression::Literal(Previous(), MarmotExpression::LiteralKind::Text));
	}
	else if (Match(Token::Name::LEFT_BRACKET))
	{
		Token& op = Previous();

		// Check for empty array first
		if (Match(Token::Name::RIGHT_BRACKET))
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::Array(op, {}));
		}

		// Probe for comprehension intent before parsing the first element.
		// This lets malformed comprehensions route into the dedicated diagnostics path
		// without stealing arrays whose first element is itself a `for` expression.
		ArrayComprehensionProbe comprehension_probe = ProbeArrayComprehension();
		if (comprehension_probe.m_is_candidate)
		{
			return ParseArrayComprehension(op, comprehension_probe);
		}

		// Otherwise, parse as normal array literal
		return ParseExpression()
			.and_then
			(
				[&op, this](std::unique_ptr<MarmotExpression>&& first_expr) -> MarmotResult::ExpressionResult
				{
					if (Match(Token::Name::COMMA))
					{
						// Parse remaining elements
						return ParseDelimitedZeroOrMoreLimited<std::unique_ptr<MarmotExpression>>
							(
								[this]() { return ParseExpression(); },
								[this]() { return Consume(Token::Name::COMMA, "Expected ',' after expression."); },
								[this]() -> MarmotResult::TokenResult
								{
									if (Match(Token::Name::RIGHT_BRACKET))
									{
										return Previous();
									}
									else
									{
										return std::unexpected(GenerateParserError("Expected ']' for array expression.", Peek(0)));
									}
								}
							)
							.and_then
							(
								[&op, first_expr = std::move(first_expr)](std::vector<std::unique_ptr<MarmotExpression>>&& expressions) mutable -> MarmotResult::ExpressionResult
								{
									expressions.insert(expressions.begin(), std::move(first_expr));
									return std::make_unique<MarmotExpression>(MarmotExpression::Array(op, std::move(expressions)));
								}
							);
					}
					else if (Match(Token::Name::RIGHT_BRACKET))
					{
						// Single element array
						std::vector<std::unique_ptr<MarmotExpression>> expressions;
						expressions.emplace_back(std::move(first_expr));
						return std::make_unique<MarmotExpression>(MarmotExpression::Array(op, std::move(expressions)));
					}
					else
					{
						return std::unexpected(GenerateParserError("Expected ',' or ']' after array element.", Peek(0)));
					}
				}
			);
	}
	else if (Match(Token::Name::IF))
	{
		return ParseIfElseExpression();
	}
	else if (Match(Token::Name::MATCH))
	{
		return ParseMatchExpression();
	}
	else if (Match(Token::Name::FOR))
	{
		return ParseForExpression();
	}
	else
	{
		return std::unexpected(GenerateParserError("Expected expression.", Peek(0)));
	}
}

MarmotResult::ExpressionResult Parser::ParseLogicalAnd()
{
	return ParseBinary(&Parser::ParseSend, Token::Name::DOUBLE_AMPERSAND);
}

MarmotResult::ExpressionResult Parser::ParseLogicalOr()
{
	return ParseBinary(&Parser::ParseLogicalAnd, Token::Name::DOUBLE_BAR);
}

MarmotResult::ExpressionResult Parser::ParseSend()
{
	return ParsePipe()
		.and_then
		(
			[this](std::unique_ptr<MarmotExpression>&& left_expr) -> MarmotResult::ExpressionResult
			{
				while (Match(Token::Name::THIN_ARROW))
				{
					Token& arrow = Previous();
					MarmotResult::ExpressionResult right = ParsePipe();
					if (!right.has_value())
					{
						return std::unexpected(std::move(right.error()));
					}

					left_expr = std::make_unique<MarmotExpression>(MarmotExpression::Send(arrow, std::move(left_expr), std::move(right.value())));
				}

				return left_expr;
			}
		);
}

MarmotResult::ExpressionResult Parser::ParsePipe()
{
	return ParseBitwiseOr()
		.and_then
		(
			[this](std::unique_ptr<MarmotExpression>&& left_expr) -> MarmotResult::ExpressionResult
			{
				while (Match(Token::Name::BAR_BRACKET))
				{
					Token& pipe_op = Previous();

					if (Match(Token::Name::MATCH))
					{
						Token& match_keyword = Previous();
						MarmotResult::ExpressionResult right = ParseMatchExpressionWithScrutinee(match_keyword, std::move(left_expr));
						if (!right.has_value())
						{
							return std::unexpected(std::move(right.error()));
						}

						left_expr = std::move(right.value());
						continue;
					}

					m_state.m_next_function_is_pipe_stage = Peek(0).m_token_name == Token::Name::FUNCTION;
					MarmotResult::ExpressionResult right = ParseBitwiseOr();
					if (!right.has_value())
					{
						return std::unexpected(std::move(right.error()));
					}

					// Prepend the piped value into whichever argument list the right-hand side
					// already owns, and fall back to a call when it owns none.
					// x |> f(y) becomes f(x, y)
					// x |> Point(y) becomes Point(x, y)
					// x |> Concurrency::Spawn(F) becomes Concurrency::Spawn(x, F)
					// x |> f becomes f(x)
					// A construction is not a Call node, so it needs its own branch.
					// Prepending at parse time is also what lets the piped value take part in generic
					// inference on the same footing as a written argument.
					if (right.value()->IsExpression<MarmotExpression::Call>())
					{
						MarmotExpression::Call& call_expr = right.value()->GetExpression<MarmotExpression::Call>();
						call_expr.m_arguments.insert(call_expr.m_arguments.begin(), std::move(left_expr));
						left_expr = std::move(right.value());
					}
					else if (right.value()->IsExpression<MarmotExpression::Construct>())
					{
						MarmotExpression::Construct& construct_expr = right.value()->GetExpression<MarmotExpression::Construct>();
						construct_expr.m_params.insert(construct_expr.m_params.begin(), std::move(left_expr));
						left_expr = std::move(right.value());
					}
					else
					{
						std::vector<std::unique_ptr<MarmotExpression>> arguments;
						arguments.emplace_back(std::move(left_expr));
						left_expr = std::make_unique<MarmotExpression>(MarmotExpression::Call(pipe_op, std::move(right.value()), std::move(arguments)));
					}
				}

				return left_expr;
			}
		);
}

bool Parser::ProbeRecordUpdate()
{
	// Called with the opening '{' already consumed, so Peek(0) is the first token inside.
	//
	// '{' opens a block today. A record update `{ source with f = v }` is told apart by
	// scanning at nesting depth 0 for the first of ';' (block), '}' (block) or 'with'
	// (record update). The scan is bounded by the enclosing brace, in the same way
	// ProbeArrayComprehension is bounded by its bracket.
	//
	// The pending-match counter is load-bearing, not defensive padding: `{ match x with
	// case ... }` is a block whose 'with' sits at depth 0. Simulating this probe over the
	// 352 .mmt files in test/, MarmotPrelude/, benchmarks/, examples/ and the unit tests
	// misclassifies 0 braces with the counter and 51 without it - most of
	// MarmotPrelude/Prelude/Result.mmt and Option.mmt among them.
	int offset = 0;
	int depth = 0;
	int pending_match = 0;

	while (true)
	{
		Token::Name current = Peek(offset).m_token_name;

		if (current == Token::Name::END_OF_FILE)
		{
			return false;
		}

		if (current == Token::Name::LEFT_PAREN || current == Token::Name::LEFT_BRACKET || current == Token::Name::LEFT_BRACE)
		{
			depth += 1;
		}
		else if (current == Token::Name::RIGHT_PAREN || current == Token::Name::RIGHT_BRACKET)
		{
			depth -= 1;
		}
		else if (current == Token::Name::RIGHT_BRACE)
		{
			if (depth == 0)
			{
				return false;
			}
			depth -= 1;
		}
		else if (depth == 0)
		{
			if (current == Token::Name::SINGLE_SEMICOLON)
			{
				return false;
			}
			else if (current == Token::Name::MATCH)
			{
				pending_match += 1;
			}
			else if (current == Token::Name::WITH)
			{
				if (pending_match == 0)
				{
					return true;
				}
				pending_match -= 1;
			}
		}

		offset += 1;

		// Safety limit to prevent infinite loop
		if (offset > MAX_ARRAY_SIZE)
		{
			return false;
		}
	}
}

MarmotResult::ExpressionResult Parser::ParseRecordUpdate()
{
	// Called with the opening '{' already consumed and ProbeRecordUpdate() having said
	// there is a 'with' at depth 0 ahead of any ';' or '}'.
	MarmotResult::ExpressionResult source_result = ParseExpression();
	if (!source_result.has_value())
	{
		return source_result;
	}

	MarmotResult::TokenResult with_result = Consume(Token::Name::WITH, "Expected 'with' in record update.");
	if (!with_result.has_value())
	{
		return std::unexpected(std::move(with_result.error()));
	}
	Token with_keyword = with_result.value();

	std::vector<MarmotExpression::RecordUpdate::FieldUpdate> updates;
	std::vector<std::string> seen_paths;

	while (true)
	{
		std::vector<Token> path;
		do
		{
			MarmotResult::TokenResult name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected field name in record update.");
			if (!name_result.has_value())
			{
				return std::unexpected(std::move(name_result.error()));
			}
			path.push_back(std::move(name_result.value()));
		} while (Match(Token::Name::SINGLE_DOT));

		// A field set twice, directly or through a path, is an error, not last-one-wins.
		const std::string joined = path | std::views::transform(&Token::m_lexeme) | std::views::join_with('.') | std::ranges::to<std::string>();
		const bool overlaps = std::ranges::any_of(seen_paths, [&joined](const std::string& seen)
		{
			return seen == joined || joined.starts_with(seen + '.') || seen.starts_with(joined + '.');
		});
		if (overlaps)
		{
			return std::unexpected(GenerateParserError(std::format("Field '{}' is assigned more than once in this record update.", joined), path.back()));
		}
		seen_paths.push_back(joined);

		MarmotResult::TokenResult equal_result = Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after field name in record update.");
		if (!equal_result.has_value())
		{
			return std::unexpected(std::move(equal_result.error()));
		}

		MarmotResult::ExpressionResult value_result = ParseExpression();
		if (!value_result.has_value())
		{
			return value_result;
		}

		updates.emplace_back(std::move(path), std::move(value_result.value()));

		if (!Match(Token::Name::COMMA))
		{
			break;
		}

		// Allow a trailing comma before '}'.
		if (Check(Token::Name::RIGHT_BRACE, 0))
		{
			break;
		}
	}

	return Consume(Token::Name::RIGHT_BRACE, "Expected '}' after record update fields.")
		.and_then
		(
			[&with_keyword, &source_result, &updates](Token&&) -> MarmotResult::ExpressionResult
			{
				return std::make_unique<MarmotExpression>(MarmotExpression::RecordUpdate(with_keyword, std::move(source_result.value()), std::move(updates)));
			}
		);
}

MarmotResult::ExpressionResult Parser::ParseBlockExpression()
{
	std::function<MarmotResult::ExpressionResult(std::vector<std::unique_ptr<MarmotStatement>>&&, std::unique_ptr<MarmotExpression>&&)> build_block = [this](std::vector<std::unique_ptr<MarmotStatement>>&& stmts, std::unique_ptr<MarmotExpression>&& final_expr) -> MarmotResult::ExpressionResult
		{
			return Consume(Token::Name::RIGHT_BRACE, "Expected '}' after block expression.")
				.and_then
				(
					[&stmts, &final_expr, this](Token&& right_brace)
					{
						EndScope();
						return final_expr != nullptr
							? MarmotResult::ExpressionResult(std::make_unique<MarmotExpression>(MarmotExpression::Block(right_brace, std::move(stmts), std::move(final_expr))))
							: MarmotResult::ExpressionResult(std::make_unique<MarmotExpression>(MarmotExpression::Block(right_brace, std::move(stmts))));
					}
				);
		};

	BeginScope();
	return ParseZeroOrMoreUnlimited<std::unique_ptr<MarmotStatement>>([this]() { return ParseDeclaration(); })
		.and_then
		(
			[&build_block, this](std::vector<std::unique_ptr<MarmotStatement>>&& stmts) ->MarmotResult::ExpressionResult
			{
				return TryParser<std::unique_ptr<MarmotExpression>>
					(
						[this]() -> MarmotResult::ExpressionResult
						{
							// Peek ahead: if next token is '}', do NOT try to parse an expression
							if (Check(Token::Name::RIGHT_BRACE, 0))
							{
								return std::unexpected(CompilerError::NoMatch());  // Signal: no expression expected
							}
							else
							{
								return ParseExpression();
							}
						}
					)
					.and_then
					(
						[&stmts, &build_block](std::unique_ptr<MarmotExpression>&& final_expr) -> MarmotResult::ExpressionResult
						{
							return build_block(std::move(stmts), std::move(final_expr));
						}
					)
					.or_else
					(
						[&stmts, &build_block](CompilerError&& err) -> MarmotResult::ExpressionResult
						{
							// Only fall back if this was an intentional absence
							if (err.IsNoMatch())
							{
								return build_block(std::move(stmts), nullptr);
							}

							return std::unexpected(std::move(err));
						}
					);
			}
		);
}

MarmotResult::ExpressionResult Parser::ParseForExpression()
{
	Token& for_keyword = Previous();

	if (!Match(Token::Name::IDENTIFIER_LITERAL))
	{
		return std::unexpected(GenerateParserError("Expected identifier after 'for'.", Peek(0)));
	}
	Token loop_variable = Previous();

	if (!Match(Token::Name::IN))
	{
		return std::unexpected(GenerateParserError("Expected 'in' after loop variable.", Peek(0)));
	}
	Token in_keyword = Previous();

	return ParseExpression()
		.and_then
		(
			[&for_keyword, &loop_variable, &in_keyword, this](std::unique_ptr<MarmotExpression>&& range)->MarmotResult::ExpressionResult
			{
				BeginScope();

				// Add loop variable to scope. Every index here is the slot within this
				// function's frame: the running m_total_variables count is absolute
				// across nested functions, so using it addressed slots past the frame
				// once a loop sat inside a nested lambda.
				std::string var_name(loop_variable.m_lexeme);
				std::optional<int> local_index = RegisterOrUpdateLocalVariable(var_name);
				int var_index = local_index.value_or(m_state.m_total_variables - 1);

				return ParseExpression()
					.and_then
					(
						[&for_keyword, &loop_variable, &in_keyword, range = std::move(range), var_index, this](std::unique_ptr<MarmotExpression>&& body) mutable ->MarmotResult::ExpressionResult
						{
							EndScope();

							std::unique_ptr<MarmotExpression> for_expr = std::make_unique<MarmotExpression>(MarmotExpression::For(for_keyword, loop_variable, in_keyword, std::move(range), std::move(body)));
							MarmotExpression::For& for_expr_ref = for_expr->GetExpression<MarmotExpression::For>();
							for_expr_ref.m_loop_variable_index = var_index;
							return for_expr;
						}
					);
			}
		);
}

Parser::ArrayComprehensionProbe Parser::ProbeArrayComprehension()
{
	// Look ahead for a likely comprehension boundary at bracket depth 0.
	// Returning a candidate here means the array should be parsed through the
	// comprehension path so syntax errors stay comprehension-specific.

	int offset = 0;
	int bracket_depth = 0;
	int paren_depth = 0;
	int brace_depth = 0;

	while (true)
	{
		Token::Name current = Peek(offset).m_token_name;
		if (current == Token::Name::END_OF_FILE)
		{
			return {};
		}

		const bool at_top_level = bracket_depth == 0 && paren_depth == 0 && brace_depth == 0;

		// Track nesting
		if (current == Token::Name::LEFT_BRACKET)
		{
			bracket_depth += 1;
		}
		else if (current == Token::Name::RIGHT_BRACKET)
		{
			if (at_top_level)
			{
				return {};
			}
			bracket_depth -= 1;
		}
		else if (current == Token::Name::LEFT_PAREN)
		{
			paren_depth += 1;
		}
		else if (current == Token::Name::RIGHT_PAREN)
		{
			paren_depth -= 1;
		}
		else if (current == Token::Name::LEFT_BRACE)
		{
			brace_depth += 1;
		}
		else if (current == Token::Name::RIGHT_BRACE)
		{
			brace_depth -= 1;
		}
		else if (current == Token::Name::COMMA && at_top_level)
		{
			return {};
		}
		else if (current == Token::Name::FOR && at_top_level)
		{
			// `[for ...]` is an array whose first element is a for-expression, not a comprehension.
			if (offset == 0)
			{
				return {};
			}

			ArrayComprehensionProbe probe;
			probe.m_is_candidate = true;
			if (Peek(offset + 1).m_token_name == Token::Name::IDENTIFIER_LITERAL)
			{
				probe.m_loop_variable_offset = offset + 1;
			}
			return probe;
		}
		else if (current == Token::Name::IDENTIFIER_LITERAL && at_top_level && offset > 0 && Peek(offset + 1).m_token_name == Token::Name::IN)
		{
			return ArrayComprehensionProbe{ true, offset };
		}

		offset += 1;

		// Safety limit to prevent infinite loop
		if (offset > MAX_ARRAY_SIZE)
		{
			return {};
		}
	}
}

MarmotResult::ExpressionResult Parser::ParseArrayComprehension(Token& bracket, const ArrayComprehensionProbe& probe)
{
	bool scope_open = false;
	auto close_scope = [this, &scope_open]()
		{
			if (scope_open)
			{
				EndScope();
				scope_open = false;
			}
		};

	int var_index = -1;

	if (probe.m_loop_variable_offset.has_value())
	{
		Token loop_variable = Peek(probe.m_loop_variable_offset.value());
		BeginScope();
		scope_open = true;

		// Register the loop variable before parsing the transform expression so
		// comprehensions like `[i for i in range]` resolve `i` correctly. Each index is
		// the slot within this function's frame; m_total_variables counts across nested
		// functions, so using it addressed slots past the frame from inside a lambda.
		var_index = RegisterOrUpdateLocalVariable(std::string(loop_variable.m_lexeme)).value_or(m_state.m_total_variables - 1);
	}

	return ParseExpression()
		.and_then
		(
			[&bracket, &close_scope, var_index, this](std::unique_ptr<MarmotExpression>&& transform_expr) -> MarmotResult::ExpressionResult
			{
				if (!Match(Token::Name::FOR))
				{
					close_scope();
					return std::unexpected(GenerateParserError("Expected 'for' in array comprehension. Use '[expr for item in range]' syntax.", Peek(0)));
				}

				if (!Match(Token::Name::IDENTIFIER_LITERAL))
				{
					close_scope();
					return std::unexpected(GenerateParserError("Expected identifier after 'for' in array comprehension.", Peek(0)));
				}
				Token actual_loop_var = Previous();

				if (!Match(Token::Name::IN))
				{
					close_scope();
					return std::unexpected(GenerateParserError("Expected 'in' after loop variable in array comprehension. Use '[expr for item in range]' syntax.", Peek(0)));
				}
				Token in_keyword = Previous();

				return ParseExpression()
					.and_then
					(
						[&bracket, &actual_loop_var, &in_keyword, &close_scope, var_index, transform_expr = std::move(transform_expr), this](std::unique_ptr<MarmotExpression>&& range) mutable -> MarmotResult::ExpressionResult
						{
							if (!Match(Token::Name::RIGHT_BRACKET))
							{
								close_scope();
								return std::unexpected(GenerateParserError("Expected ']' after array comprehension.", Peek(0)));
							}

							close_scope();
							if (var_index < 0)
							{
								return std::unexpected(GenerateParserError("Internal error: array comprehension loop binding was not initialized.", actual_loop_var));
							}

							std::unique_ptr<MarmotExpression> comp_expr = std::make_unique<MarmotExpression>(MarmotExpression::ArrayComprehension(bracket, actual_loop_var, in_keyword, std::move(transform_expr), std::move(range)));

							MarmotExpression::ArrayComprehension& comp_ref = comp_expr->GetExpression<MarmotExpression::ArrayComprehension>();
							comp_ref.m_loop_variable_index = var_index;

							return comp_expr;
						}
					)
					.or_else
					(
						[&close_scope](CompilerError&& error) -> MarmotResult::ExpressionResult
						{
							close_scope();
							return std::unexpected(std::move(error));
						}
					);
			}
		)
		.or_else
		(
			[&close_scope](CompilerError&& error) -> MarmotResult::ExpressionResult
			{
				close_scope();
				return std::unexpected(std::move(error));
			}
		);
}

MarmotResult::StatementResult Parser::ParseDefineStatement()
{
	if (Match(Token::Name::LEFT_PAREN))
	{
		std::vector<Token> names;
		std::vector<std::optional<int>> local_indices;

		do
		{
			MarmotResult::TokenResult name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected identifier in tuple pattern.");
			if (!name_result)
			{
				return std::unexpected(name_result.error());
			}

			Token var_name = std::move(name_result.value());
			constexpr bool is_variable = true;

			MarmotResult::TokenResult defined_name_result = DefineName(var_name, is_variable);
			if (!defined_name_result)
			{
				return std::unexpected(defined_name_result.error());
			}

			Token defined_name = std::move(defined_name_result.value());
			std::optional<int> local_index = RegisterOrUpdateLocalVariable(defined_name.m_lexeme);

			names.emplace_back(std::move(defined_name));
			local_indices.emplace_back(std::move(local_index));

		} while (Match(Token::Name::COMMA));

		return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after tuple pattern.")
			.and_then
			(
				[&names, &local_indices, this](Token&&) -> MarmotResult::StatementResult
				{
					return Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after tuple pattern.")
						.and_then
						(
							[&names, &local_indices, this](Token&&) -> MarmotResult::StatementResult
							{
								std::vector<std::string> pending_names;
								pending_names.reserve(names.size());
								std::ranges::transform(names, std::back_inserter(pending_names), [](const Token& name) { return name.m_lexeme; });
								const PendingDefinitionGuard pending(this, std::move(pending_names));

								return ParseExpression()
									.and_then
									(
										[&names, &local_indices, this](std::unique_ptr<MarmotExpression>&& expr) -> MarmotResult::StatementResult
										{
											return Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after tuple binding.")
												.and_then
												(
													[&names, &local_indices, &expr](Token&&) -> MarmotResult::StatementResult
													{
														return std::make_unique<MarmotStatement>(MarmotStatement::TupleDefinition(std::move(names), std::move(expr), std::move(local_indices)));
													}
												);
										}
									);
							}
						);
				}
			);
	}

	// Single variable: def x = ...
	return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected name.")
		.and_then
		(
			[this](Token&& var_name) -> MarmotResult::StatementResult
			{
				constexpr bool is_variable = true;
				return DefineName(var_name, is_variable)
					.and_then
					(
						[this](Token&& define_name) -> MarmotResult::StatementResult
						{
							std::function<MarmotResult::StatementResult(std::optional<std::shared_ptr<MarmotType>>)> def_aux_func = [&define_name, this](std::optional<std::shared_ptr<MarmotType>> type_annotation)
								{
									std::optional<int> local_index = RegisterOrUpdateLocalVariable(define_name.m_lexeme);

									return Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after defining a name.")
										.and_then
										(
											[&define_name, &type_annotation, &local_index, this](Token&&) -> MarmotResult::StatementResult
											{
												const PendingDefinitionGuard pending(this, { define_name.m_lexeme });
												return ParseExpression()
													.and_then
													(
														[&define_name, &type_annotation, &local_index, this](std::unique_ptr<MarmotExpression>&& expr) -> MarmotResult::StatementResult
														{
															return Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after name binding.")
																.and_then
																(
																	[&define_name, &expr, &type_annotation, &local_index](Token&&) -> MarmotResult::StatementResult
																	{
																		return std::make_unique<MarmotStatement>(MarmotStatement::VariableDefinition(define_name, std::move(expr), std::move(type_annotation), std::move(local_index)));
																	}
																);
														}
													);
											}
										);
								};

							if (Match(Token::Name::SINGLE_COLON))
							{
								return ParseType()
									.and_then
									(
										[&def_aux_func](std::shared_ptr<MarmotType>&& type)->MarmotResult::StatementResult
										{
											return def_aux_func(std::move(type));
										}
									);
							}
							else
							{
								return def_aux_func(std::nullopt);
							}
						}
					);
			}
		);
}

std::expected<Parser::TypeDeclarationHeader, CompilerError> Parser::ParseTypeDeclarationHeader(std::string_view noun, std::string_view capitalized_noun)
{
	MarmotResult::TokenResult name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected "s + std::string(noun) + " name."s);
	if (!name_result.has_value())
	{
		return std::unexpected(name_result.error());
	}

	Token declaration_name = std::move(name_result.value());
	std::string name_before_mangle = declaration_name.m_lexeme;
	declaration_name.m_lexeme = Mangle(declaration_name.m_lexeme);

	if (declaration_name.m_lexeme[0u] != std::toupper(declaration_name.m_lexeme[0u]))
	{
		return std::unexpected(GenerateParserError(std::string(capitalized_noun) + " name must start with a capital letter."s, declaration_name));
	}

	constexpr bool is_variable = false;
	MarmotResult::TokenResult defined_name_result = DefineName(declaration_name, is_variable);
	if (!defined_name_result.has_value())
	{
		return std::unexpected(defined_name_result.error());
	}

	TypeDeclarationHeader header(std::move(defined_name_result.value()), std::move(name_before_mangle));

	// Generic parameters open a scope before they are parsed, so that DefineName()
	// inside ParseGenericParameters() adds them to it. The body parsers close it.
	if (Match(Token::Name::LEFT_ANGLE))
	{
		header.m_has_generic_params = true;
		BeginScope();

		MarmotResult::TokenListResult generic_parse_result = ParseGenericParameters(&header.m_generic_param_types);
		if (!generic_parse_result.has_value())
		{
			EndScope();
			return std::unexpected(generic_parse_result.error());
		}

		header.m_generic_params = std::move(generic_parse_result.value());
	}

	if (Match(Token::Name::WHERE))
	{
		if (!header.m_has_generic_params)
		{
			return std::unexpected(GenerateParserError(std::string(capitalized_noun) + " constraints require at least one type parameter."s, header.m_name));
		}

		std::expected<std::vector<MarmotType::ClassConstraint>, CompilerError> constraints_result = ParseClassConstraints(header.m_name);
		if (!constraints_result.has_value())
		{
			return std::unexpected(constraints_result.error());
		}

		header.m_constraints = std::move(constraints_result.value());
	}

	return header;
}

MarmotResult::StatementResult Parser::ParseStructBody(TypeDeclarationHeader&& header)
{
	MarmotResult::TokenResult brace_result = Consume(Token::Name::LEFT_BRACE, "Expected '{' before struct body.");
	if (!brace_result.has_value())
	{
		return std::unexpected(brace_result.error());
	}

	std::vector<std::string> generic_param_names;
	std::ranges::transform(header.m_generic_params, std::back_inserter(generic_param_names), [](const Token& tok) { return tok.m_lexeme; });

	// Registered before the fields are parsed, as a union is before its variants,
	// so a field can name the record it belongs to.
	std::shared_ptr<MarmotType> struct_type = DeclaredTypeFor(header, MarmotType::MakeStructType(header.m_name.m_lexeme, CurrentModuleName(), {}, {}, std::move(generic_param_names)));
	struct_type->GetType<MarmotType::StructType>().m_constraints = header.m_constraints;
	const size_t type_scope_idx = header.m_has_generic_params ? m_state.m_scopes.size() - 2uz : m_state.m_scopes.size() - 1uz;
	m_state.m_scopes[type_scope_idx].m_defined_types[header.m_name.m_lexeme] = struct_type;

	DeclaringTypeScope declaring_scope(m_state.m_types_being_declared, struct_type);
	std::expected<std::vector<StructMemberTuple>, CompilerError> members_result = ParseDelimitedZeroOrMoreLimited<StructMemberTuple>
	(
		[this]() -> std::expected<StructMemberTuple, CompilerError>
		{
			MarmotResult::TokenResult member_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected struct member name.");
			if (!member_name_result.has_value())
			{
				return std::unexpected(member_name_result.error());
			}

			Token member_name = std::move(member_name_result.value());

			MarmotResult::TokenResult colon_result = Consume(Token::Name::SINGLE_COLON, "Expected ':' before struct member type token.");
			if (!colon_result.has_value())
			{
				return std::unexpected(colon_result.error());
			}

			MarmotResult::TypeResult member_type_result = ParseType();
			if (!member_type_result.has_value())
			{
				return std::unexpected(member_type_result.error());
			}

			return std::make_tuple(std::move(member_type_result.value()), member_name.m_lexeme);
		},
		[this]() { return Consume(Token::Name::COMMA, "Expected ',' struct member."); },
		[this]() { return Consume(Token::Name::RIGHT_BRACE, "Expected '}' struct members."); }
	);
	if (!members_result.has_value())
	{
		return std::unexpected(members_result.error());
	}

	std::vector<Token> deriving_targets;
	if (Match(Token::Name::DERIVING))
	{
		std::expected<std::vector<Token>, CompilerError> deriving_result = ParseDerivingTargets(header.m_name);
		if (!deriving_result.has_value())
		{
			return std::unexpected(std::move(deriving_result.error()));
		}

		deriving_targets = std::move(deriving_result.value());
	}

	MarmotResult::TokenResult semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after struct body.");
	if (!semicolon_result.has_value())
	{
		return std::unexpected(semicolon_result.error());
	}

	StructMemberSplit member_split = SplitStructMemberTuples(std::move(members_result.value()));
	struct_type->GetType<MarmotType::StructType>().m_member_types = std::move(member_split.m_types);
	struct_type->GetType<MarmotType::StructType>().m_member_names = std::move(member_split.m_names);
	CompleteType(struct_type);

	// End the generic param scope if it was created
	if (header.m_has_generic_params)
	{
		EndScope();
	}

	m_state.m_scopes.back().m_struct_constructors[header.m_name.m_lexeme] = struct_type;

	MarmotStatement::Struct struct_stmt(header.m_name, std::vector<Token>(header.m_generic_params), std::vector<MarmotType::ClassConstraint>(header.m_constraints), std::shared_ptr<MarmotType>(struct_type));
	if (!deriving_targets.empty())
	{
		std::expected<void, CompilerError> derive_result = QueueDerivedStatements(struct_stmt, deriving_targets);
		if (!derive_result.has_value())
		{
			return std::unexpected(std::move(derive_result.error()));
		}
	}

	return std::make_unique<MarmotStatement>(MarmotStatement::Struct(std::move(header.m_name), std::move(header.m_generic_params), std::move(header.m_constraints), std::move(struct_type)));
}

MarmotResult::StatementResult Parser::ParseUnionBody(TypeDeclarationHeader&& header)
{
	// The caller has already consumed the '=': ParseTypeDeclaration takes it before
	// dispatching on the shape of the body that follows.
	std::vector<std::string> generic_param_names;
	std::ranges::transform(header.m_generic_params, std::back_inserter(generic_param_names), [](const Token& tok) { return tok.m_lexeme; });

	std::shared_ptr<MarmotType> union_type = DeclaredTypeFor(header, MarmotType::MakeUnionType(header.m_name.m_lexeme, CurrentModuleName(), std::move(generic_param_names)));
	MarmotType::UnionType& union_type_ref = union_type->GetType<MarmotType::UnionType>();
	union_type_ref.m_constraints = header.m_constraints;

	// Registered before the body is parsed so that a variant can name the union it
	// belongs to, as `type List = Nil | Cons(Int, List)` does.
	size_t type_scope_idx = header.m_has_generic_params ? m_state.m_scopes.size() - 2uz : m_state.m_scopes.size() - 1uz;
	m_state.m_scopes[type_scope_idx].m_defined_types[header.m_name.m_lexeme] = union_type;
	m_state.m_namespaces.emplace_back(header.m_name_before_mangle);

	std::vector<Token> constructor_names;
	int tag = 0;

	std::expected<std::vector<UnionMemberTuple>, CompilerError> members_result = [&constructor_names, &tag, &union_type, this]()
	{
		DeclaringTypeScope scope(m_state.m_types_being_declared, union_type);

		return ParseDelimitedOneOrMoreUnlimited<UnionMemberTuple>
		(
			[&constructor_names, &tag, this]() -> std::expected<UnionMemberTuple, CompilerError>
			{
				MarmotResult::TokenResult member_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected union member name.");
				if (!member_name_result.has_value())
				{
					return std::unexpected(member_name_result.error());
				}

				Token member_name = std::move(member_name_result.value());
				member_name.m_lexeme = Mangle(member_name.m_lexeme);

				constexpr bool is_variable = false;
				MarmotResult::TokenResult defined_member_result = DefineName(member_name, is_variable);
				if (!defined_member_result.has_value())
				{
					return std::unexpected(defined_member_result.error());
				}

				member_name = std::move(defined_member_result.value());
				constructor_names.push_back(member_name);

				std::vector<std::shared_ptr<MarmotType>> member_types;
				if (Match(Token::Name::LEFT_PAREN))
				{
					MarmotResult::TypeListResult member_types_result = ParseDelimitedZeroOrMoreLimited<std::shared_ptr<MarmotType>>
					(
						[this]() { return ParseType(); },
						[this]() { return Consume(Token::Name::COMMA, "Expected ',' after type."); },
						[this]() { return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after union constructor."); }
					);
					if (!member_types_result.has_value())
					{
						return std::unexpected(member_types_result.error());
					}

					member_types = std::move(member_types_result.value());
				}

				UnionMemberTuple member = std::make_tuple(member_name.m_lexeme, std::move(member_types), tag);
				tag += 1;
				return member;
			},
			[this]() { return Consume(Token::Name::SINGLE_BAR, "Expected '|' after a union member."); }
		);
	}();

	if (!members_result.has_value())
	{
		return std::unexpected(members_result.error());
	}

	union_type_ref.m_member_info = BuildUnionMemberInfo(std::move(members_result.value()));
	CompleteType(union_type);

	// Store constructors in the parent scope (where the union is declared)
	// If we have generic params, we're one scope level deeper, so go back one
	Scope& constructor_scope = header.m_has_generic_params
		? m_state.m_scopes[m_state.m_scopes.size() - 2]
		: m_state.m_scopes.back();

	for (const std::pair<const std::string, MarmotType::UnionType::UnionMemberContext>& member_info_entry : union_type_ref.m_member_info)
	{
		constructor_scope.m_union_constructors[member_info_entry.first] = union_type;
	}

	std::vector<Token> deriving_targets;
	if (Match(Token::Name::DERIVING))
	{
		std::expected<std::vector<Token>, CompilerError> deriving_result = ParseDerivingTargets(header.m_name);
		if (!deriving_result.has_value())
		{
			return std::unexpected(std::move(deriving_result.error()));
		}

		deriving_targets = std::move(deriving_result.value());
	}

	MarmotResult::TokenResult semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after union body.");
	if (!semicolon_result.has_value())
	{
		return std::unexpected(semicolon_result.error());
	}

	m_state.m_namespaces.pop_back();

	if (header.m_has_generic_params)
	{
		EndScope();
	}

	MarmotStatement::Union union_stmt(header.m_name, std::vector<Token>(header.m_generic_params), std::vector<Token>(constructor_names), std::vector<MarmotType::ClassConstraint>(header.m_constraints), std::shared_ptr<MarmotType>(union_type));
	if (!deriving_targets.empty())
	{
		std::expected<void, CompilerError> derive_result = QueueDerivedStatements(union_stmt, deriving_targets);
		if (!derive_result.has_value())
		{
			return std::unexpected(std::move(derive_result.error()));
		}
	}

	return std::make_unique<MarmotStatement>(MarmotStatement::Union(std::move(header.m_name), std::move(header.m_generic_params), std::move(constructor_names), std::move(header.m_constraints), std::move(union_type)));
}

bool Parser::TypeBodyHasTopLevelBar()
{
	// Bounded scan from the current token to the depth-0 ';' that ends the
	// declaration, capped by MAX_ARRAY_SIZE and bailing out as soon as any
	// depth counter would go negative, following the ProbeArrayComprehension
	// and ProbeRecordUpdate precedent: unlike those, this scan has no closing
	// delimiter of its own to bound it, so both guards are needed to keep a
	// malformed declaration from running this to true EOF. A depth-0 '|'
	// means a sum; anything else is a newtype over a type expression.
	//
	// The lexer merges adjacent '>' characters into one RIGHT_SHIFT token
	// (e.g. the '>>' closing Array<Array<Int>>), so RIGHT_SHIFT closes two
	// angle-bracket levels here, mirroring how ConsumeTypeRightAngle later
	// splits that same token into two synthetic RIGHT_ANGLE tokens when the
	// real type parser consumes it. LEFT_SHIFT gets no symmetric treatment:
	// two '<' are never adjacent in well-formed type syntax (every '<' that
	// opens a type argument list is preceded by a type name), and ParseType
	// has no LEFT_SHIFT-splitting counterpart to ConsumeTypeRightAngle, so
	// treating '<<' as two opens here would only desynchronize this probe
	// from what the real parser does with it.
	int paren_depth = 0;
	int angle_depth = 0;
	int brace_depth = 0;
	int bracket_depth = 0;

	for (int offset = 0; !Check(Token::Name::END_OF_FILE, offset); offset += 1)
	{
		const Token::Name token_name = Peek(offset).m_token_name;

		if (token_name == Token::Name::LEFT_PAREN)
		{
			paren_depth += 1;
		}
		else if (token_name == Token::Name::RIGHT_PAREN)
		{
			paren_depth -= 1;
		}
		else if (token_name == Token::Name::LEFT_ANGLE)
		{
			angle_depth += 1;
		}
		else if (token_name == Token::Name::RIGHT_ANGLE)
		{
			angle_depth -= 1;
		}
		else if (token_name == Token::Name::RIGHT_SHIFT)
		{
			angle_depth -= 2;
		}
		else if (token_name == Token::Name::LEFT_BRACE)
		{
			brace_depth += 1;
		}
		else if (token_name == Token::Name::RIGHT_BRACE)
		{
			brace_depth -= 1;
		}
		else if (token_name == Token::Name::LEFT_BRACKET)
		{
			bracket_depth += 1;
		}
		else if (token_name == Token::Name::RIGHT_BRACKET)
		{
			bracket_depth -= 1;
		}

		if (paren_depth < 0 || angle_depth < 0 || brace_depth < 0 || bracket_depth < 0)
		{
			return false;
		}

		const bool at_top_level = paren_depth == 0 && angle_depth == 0 && brace_depth == 0 && bracket_depth == 0;

		if (at_top_level && token_name == Token::Name::SINGLE_SEMICOLON)
		{
			return false;
		}

		if (at_top_level && token_name == Token::Name::SINGLE_BAR)
		{
			return true;
		}

		if (offset > MAX_ARRAY_SIZE)
		{
			return false;
		}
	}

	return false;
}

MarmotResult::StatementResult Parser::ParseNewTypeBody(TypeDeclarationHeader&& header)
{
	std::vector<std::string> generic_param_names;
	std::ranges::transform(header.m_generic_params, std::back_inserter(generic_param_names), [](const Token& generic_param) { return generic_param.m_lexeme; });

	std::shared_ptr<MarmotType> new_type = DeclaredTypeFor(header, MarmotType::MakeNewType(header.m_name.m_lexeme, CurrentModuleName(), MarmotType::MakeLiteralType<MarmotType::UnitType>(), std::move(generic_param_names)));
	new_type->GetType<MarmotType::NewType>().m_constraints = header.m_constraints;

	MarmotResult::TypeResult representation_result = [&new_type, this]()
		{
			DeclaringTypeScope declaring_scope(m_state.m_types_being_declared, new_type);
			return ParseType();
		}();
	if (!representation_result.has_value())
	{
		return std::unexpected(representation_result.error());
	}

	new_type->GetType<MarmotType::NewType>().m_representation = std::move(representation_result.value());
	CompleteType(new_type);

	// A newtype is its representation wherever it is erased, so one that
	// contains itself, directly or through another newtype, never ends.
	std::unordered_set<const MarmotType*> visited;
	if (RepresentationNames(new_type->GetType<MarmotType::NewType>().m_representation, new_type.get(), visited))
	{
		return std::unexpected(GenerateParserError(std::format("A newtype cannot contain itself: '{}' is represented by {}.", header.m_name.m_lexeme, new_type->GetType<MarmotType::NewType>().m_representation->DisplayString()), header.m_name));
	}

	MarmotResult::TokenResult semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after newtype definition.");
	if (!semicolon_result.has_value())
	{
		return std::unexpected(semicolon_result.error());
	}

	if (header.m_has_generic_params)
	{
		EndScope();
	}

	// Registered in the enclosing scope so uses of the name resolve to the
	// nominal type rather than to its representation.
	m_state.m_scopes.back().m_defined_types[header.m_name.m_lexeme] = new_type;

	return std::make_unique<MarmotStatement>(MarmotStatement::TypeAlias(std::move(header.m_name), std::move(header.m_generic_params), std::move(new_type)));
}

MarmotResult::StatementResult Parser::ParseTypeDeclaration()
{
	const bool is_top_level = m_state.m_scopes.size() == 1uz;
	std::expected<TypeDeclarationHeader, CompilerError> header_result = ParseTypeDeclarationHeader("type", "Type");
	if (!header_result.has_value())
	{
		return std::unexpected(header_result.error());
	}

	MarmotResult::TokenResult equal_result = Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after type name.");
	if (!equal_result.has_value())
	{
		return std::unexpected(equal_result.error());
	}

	if (is_top_level)
	{
		header_result.value().m_predeclared_type = m_predeclared_types.at(header_result.value().m_name.m_lexeme);
	}

	// The token after '=' decides the kind. '{' opens a record body and ParseType
	// has no LEFT_BRACE branch, so the two shapes cannot be confused.
	//
	// This '{' is not the record-update probe's '{'. ProbeRecordUpdate runs at
	// exactly two sites, ParsePrimary and the function-body fast path, both in
	// expression position; a type declaration never reaches expression parsing.
	// Were it somehow fed a record body, it would scan to the depth-0 '}' before
	// finding any 'with' and answer "block", so it fails safe either way.
	if (Check(Token::Name::LEFT_BRACE, 0))
	{
		return ParseStructBody(std::move(header_result.value()));
	}

	// A depth-0 '|' anywhere before the terminating ';' means a sum. Without one,
	// the right-hand side is a type expression and this declares a newtype. A
	// single-variant sum therefore needs an explicit leading bar, which is what
	// frees the bare-name spelling for newtypes.
	if (!TypeBodyHasTopLevelBar())
	{
		return ParseNewTypeBody(std::move(header_result.value()));
	}

	Match(Token::Name::SINGLE_BAR);
	return ParseUnionBody(std::move(header_result.value()));
}

MarmotResult::StatementResult Parser::ParseAliasDeclaration()
{
	// The header gives an alias the same prologue every nominal declaration has, and
	// with it the scope that binds `T` while `= Box<T>` is parsed.
	std::expected<TypeDeclarationHeader, CompilerError> header_result = ParseTypeDeclarationHeader("alias", "Alias");
	if (!header_result.has_value())
	{
		return std::unexpected(header_result.error());
	}

	TypeDeclarationHeader header = std::move(header_result.value());

	// An alias is transparent: it disappears into its expansion before anything can
	// discharge a constraint written on it. Rejecting is the only honest answer.
	if (!header.m_constraints.empty())
	{
		return std::unexpected(GenerateParserError("Alias declarations cannot carry 'where' constraints. Constrain the type the alias expands to instead.", header.m_name));
	}

	MarmotResult::TokenResult equal_result = Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after alias name.");
	if (!equal_result.has_value())
	{
		return std::unexpected(equal_result.error());
	}

	MarmotResult::TypeResult aliased_type_result = ParseType();
	if (!aliased_type_result.has_value())
	{
		return std::unexpected(aliased_type_result.error());
	}

	std::shared_ptr<MarmotType> aliased_type = std::move(aliased_type_result.value());

	MarmotResult::TokenResult semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after alias definition.");
	if (!semicolon_result.has_value())
	{
		return std::unexpected(semicolon_result.error());
	}

	if (header.m_has_generic_params)
	{
		EndScope();
	}

	// aliased_type is already the template a use site needs: substituting Box's own
	// parameters with the alias's left every occurrence of `T` in it standing for the
	// alias's `T`, so applying the alias is one more substitution over the same body.
	// Only the parameter names and their order do not survive that rewrite, so those
	// are what the scope records.
	std::vector<std::string> generic_param_names;
	std::ranges::transform(header.m_generic_params, std::back_inserter(generic_param_names), [](const Token& generic_param) { return generic_param.m_lexeme; });

	m_state.m_scopes.back().m_defined_types[header.m_name.m_lexeme] = aliased_type;
	if (!generic_param_names.empty())
	{
		m_state.m_scopes.back().m_alias_generic_params[header.m_name.m_lexeme] = std::move(generic_param_names);
	}

	return std::make_unique<MarmotStatement>(MarmotStatement::TypeAlias(std::move(header.m_name), std::move(header.m_generic_params), std::move(aliased_type)));
}

MarmotResult::StatementResult Parser::ParseClassDeclaration()
{
	MarmotResult::TokenResult class_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected class name.");
	if (!class_name_result.has_value())
	{
		return std::unexpected(class_name_result.error());
	}

	Token typeclass_name = std::move(class_name_result.value());
	typeclass_name.m_lexeme = Mangle(typeclass_name.m_lexeme);
	if (typeclass_name.m_lexeme[0u] != std::toupper(typeclass_name.m_lexeme[0u]))
	{
		return std::unexpected(GenerateParserError("Class name must start with a capital letter.", typeclass_name));
	}

	MarmotResult::TokenResult defined_name_result = DefineName(typeclass_name, false);
	if (!defined_name_result.has_value())
	{
		return std::unexpected(defined_name_result.error());
	}
	typeclass_name = std::move(defined_name_result.value());

	if (!Match(Token::Name::LEFT_ANGLE))
	{
		return std::unexpected(GenerateParserError("Class must have at least one type parameter.", typeclass_name));
	}

	BeginScope();
	struct ScopeGuard
	{
		Parser* m_parser;

		explicit ScopeGuard(Parser* parser)
			: m_parser(parser)
		{
		}

		~ScopeGuard()
		{
			if (m_parser != nullptr)
			{
				m_parser->EndScope();
			}
		}
	} scope_guard(this);

	std::vector<Token> type_params;
	std::vector<std::shared_ptr<MarmotType>> type_param_types;
	MarmotResult::TokenListResult generic_parse_result = ParseGenericParameters(&type_param_types);
	if (!generic_parse_result.has_value())
	{
		return std::unexpected(generic_parse_result.error());
	}
	type_params = std::move(generic_parse_result.value());

	std::vector<std::string> type_param_names;
	std::ranges::transform(type_params, std::back_inserter(type_param_names), [](const Token& tok) { return tok.m_lexeme; });
	m_state.m_typeclass_type_params[typeclass_name.m_lexeme] = type_param_names;
	std::vector<std::string>& associated_type_names = m_state.m_typeclass_associated_types[typeclass_name.m_lexeme];
	associated_type_names.clear();

	MarmotResult::TokenResult brace_result = Consume(Token::Name::LEFT_BRACE, "Expected '{' before class body.");
	if (!brace_result.has_value())
	{
		return std::unexpected(brace_result.error());
	}

	std::vector<MarmotStatement::Class::AssociatedTypeDeclaration> associated_types;
	std::vector<std::unique_ptr<MarmotStatement>> methods;
	while (!Match(Token::Name::RIGHT_BRACE))
	{
		if (IsAtEnd())
		{
			return std::unexpected(GenerateParserError("Expected '}' after class body.", Previous()));
		}

		if (Match(Token::Name::TYPE))
		{
			MarmotResult::TokenResult assoc_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected associated type name.");
			if (!assoc_name_result.has_value())
			{
				return std::unexpected(assoc_name_result.error());
			}

			Token assoc_name = std::move(assoc_name_result.value());
			assoc_name.m_lexeme = Mangle(assoc_name.m_lexeme);
			if (assoc_name.m_lexeme[0u] != std::toupper(assoc_name.m_lexeme[0u]))
			{
				return std::unexpected(GenerateParserError("Associated type name must start with a capital letter.", assoc_name));
			}

			MarmotResult::TokenResult defined_assoc_result = DefineName(assoc_name, false);
			if (!defined_assoc_result.has_value())
			{
				return std::unexpected(defined_assoc_result.error());
			}
			assoc_name = std::move(defined_assoc_result.value());

			MarmotResult::TokenResult assoc_semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after associated type declaration.");
			if (!assoc_semicolon_result.has_value())
			{
				return std::unexpected(assoc_semicolon_result.error());
			}

			associated_type_names.push_back(assoc_name.m_lexeme);
			std::vector<std::shared_ptr<MarmotType>> associated_type_args = type_param_types;
			m_state.m_scopes.back().m_defined_types[assoc_name.m_lexeme] = MarmotType::MakeAssociatedType(typeclass_name.m_lexeme, assoc_name.m_lexeme, std::move(associated_type_args));
			associated_types.emplace_back(assoc_name);
			continue;
		}

		MarmotResult::TokenResult method_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected method name.");
		if (!method_name_result.has_value())
		{
			return std::unexpected(method_name_result.error());
		}

		Token method_name = std::move(method_name_result.value());
		std::string method_name_str = method_name.m_lexeme;

		MarmotResult::TokenResult colon_result = Consume(Token::Name::SINGLE_COLON, "Expected ':' after method name.");
		if (!colon_result.has_value())
		{
			return std::unexpected(colon_result.error());
		}

		MarmotResult::TokenResult function_result = Consume(Token::Name::FUNCTION, "Expected 'fn' in method signature.");
		if (!function_result.has_value())
		{
			return std::unexpected(function_result.error());
		}

		MarmotResult::TokenResult left_paren_result = Consume(Token::Name::LEFT_PAREN, "Expected '(' before method parameters.");
		if (!left_paren_result.has_value())
		{
			return std::unexpected(left_paren_result.error());
		}

		using MethodParam = std::tuple<Token, std::shared_ptr<MarmotType>>;
		std::expected<std::vector<MethodParam>, CompilerError> params_result = ParseDelimitedZeroOrMoreLimited<MethodParam>
		(
			[this]() -> std::expected<MethodParam, CompilerError>
			{
				MarmotResult::TokenResult param_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected parameter name.");
				if (!param_name_result.has_value())
				{
					return std::unexpected(param_name_result.error());
				}

				Token param_name = std::move(param_name_result.value());
				MarmotResult::TokenResult param_colon_result = Consume(Token::Name::SINGLE_COLON, "Expected ':' after parameter name.");
				if (!param_colon_result.has_value())
				{
					return std::unexpected(param_colon_result.error());
				}

				MarmotResult::TypeResult param_type_result = ParseType();
				if (!param_type_result.has_value())
				{
					return std::unexpected(param_type_result.error());
				}

				return std::make_tuple(std::move(param_name), std::move(param_type_result.value()));
			},
			[this]() { return Consume(Token::Name::COMMA, "Expected ',' between parameters."); },
			[this]() { return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after parameters."); }
		);
		if (!params_result.has_value())
		{
			return std::unexpected(std::move(params_result.error()));
		}

		MarmotResult::TokenResult arrow_result = Consume(Token::Name::THIN_ARROW, "Expected '->' before return type.");
		if (!arrow_result.has_value())
		{
			return std::unexpected(arrow_result.error());
		}

		MarmotResult::TypeResult return_type_result = ParseType();
		if (!return_type_result.has_value())
		{
			return std::unexpected(return_type_result.error());
		}
		std::shared_ptr<MarmotType> return_type = std::move(return_type_result.value());

		MarmotResult::TokenResult method_semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after method signature.");
		if (!method_semicolon_result.has_value())
		{
			return std::unexpected(method_semicolon_result.error());
		}

		std::vector<std::shared_ptr<MarmotType>> param_types;
		std::vector<Token> param_tokens;
		param_types.reserve(params_result->size());
		param_tokens.reserve(params_result->size());
		for (MethodParam& tuple : params_result.value())
		{
			param_types.emplace_back(std::get<1>(tuple));
			param_tokens.emplace_back(std::move(std::get<0>(tuple)));
		}

		std::vector<std::shared_ptr<MarmotType>> param_types_copy = param_types;
		std::shared_ptr<MarmotType> return_type_copy = return_type;
		std::shared_ptr<MarmotType> method_type = MarmotType::MakeFunctionType(std::move(param_types_copy), std::move(return_type_copy));

		m_state.m_class_methods[typeclass_name.m_lexeme].insert(method_name_str);
		m_state.m_typeclass_method_types[typeclass_name.m_lexeme][method_name_str] = method_type;
		methods.emplace_back
		(
			std::make_unique<MarmotStatement>
			(
				MarmotStatement::FunctionDefinition(method_name, std::vector<Token>(), std::move(param_tokens), std::move(param_types), std::move(return_type), nullptr, std::nullopt, 0, std::vector<MarmotType::ClassConstraint>())
			)
		);
	}

	MarmotResult::TokenResult class_semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after class body.");
	if (!class_semicolon_result.has_value())
	{
		return std::unexpected(class_semicolon_result.error());
	}

	scope_guard.m_parser = nullptr;
	EndScope();

	return std::make_unique<MarmotStatement>
	(
		MarmotStatement::Class(std::move(typeclass_name), std::move(type_params), std::move(associated_types), std::move(methods))
	);
}

MarmotResult::StatementResult Parser::ParseInstanceDeclaration()
{
	MarmotResult::TokenResult class_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected class name.");
	if (!class_result.has_value())
	{
		return std::unexpected(class_result.error());
	}

	Token typeclass_name = std::move(class_result.value());
	if (!m_state.m_typeclass_type_params.contains(typeclass_name.m_lexeme))
	{
		return std::unexpected(GenerateParserError("Unknown class '" + typeclass_name.m_lexeme + "'.", typeclass_name));
	}

	MarmotResult::TokenResult angle_result = Consume(Token::Name::LEFT_ANGLE, "Expected '<' before type arguments.");
	if (!angle_result.has_value())
	{
		return std::unexpected(angle_result.error());
	}

	BeginScope();
	struct ScopeGuard
	{
		Parser* m_parser;

		explicit ScopeGuard(Parser* parser)
			: m_parser(parser)
		{
		}

		~ScopeGuard()
		{
			if (m_parser != nullptr)
			{
				m_parser->EndScope();
			}
		}

		ScopeGuard(const ScopeGuard&) = delete;
		ScopeGuard& operator=(const ScopeGuard&) = delete;
	} scope_guard(this);

	struct ImplicitGenericParamGuard
	{
		Parser* m_parser;
		bool m_prev_value;

		explicit ImplicitGenericParamGuard(Parser* parser)
			: m_parser(parser),
			m_prev_value(parser->m_state.m_allow_implicit_generic_params)
		{
			m_parser->m_state.m_allow_implicit_generic_params = true;
		}

		~ImplicitGenericParamGuard()
		{
			if (m_parser != nullptr)
			{
				m_parser->m_state.m_allow_implicit_generic_params = m_prev_value;
			}
		}

		ImplicitGenericParamGuard(const ImplicitGenericParamGuard&) = delete;
		ImplicitGenericParamGuard& operator=(const ImplicitGenericParamGuard&) = delete;
	};

	std::vector<std::shared_ptr<MarmotType>> type_args;
	{
		ImplicitGenericParamGuard implicit_guard(this);
		MarmotResult::TypeListResult type_args_result = ParseDelimitedZeroOrMoreLimited<std::shared_ptr<MarmotType>>
			(
				[this]() { return ParseType(); },
				[this]() { return Consume(Token::Name::COMMA, "Expected ',' between type arguments."); },
				[this]() { return ConsumeTypeRightAngle("Expected '>' after type arguments."); }
			);
		if (!type_args_result.has_value())
		{
			return std::unexpected(type_args_result.error());
		}
		type_args = std::move(type_args_result.value());
	}

	if (type_args.empty())
	{
		return std::unexpected(GenerateParserError("Instance must have at least one type argument.", typeclass_name));
	}

	std::vector<MarmotType::ClassConstraint> constraints;
	if (Match(Token::Name::WHERE))
	{
		std::expected<std::vector<MarmotType::ClassConstraint>, CompilerError> constraints_result = ParseClassConstraints(typeclass_name);
		if (!constraints_result.has_value())
		{
			return std::unexpected(constraints_result.error());
		}

		constraints = std::move(constraints_result.value());
	}

	size_t prev_constraints_size = m_state.m_active_constraints.size();
	PushActiveConstraints(constraints);
	ActiveConstraintGuard constraint_guard(this, prev_constraints_size);

	MarmotResult::TokenResult brace_result = Consume(Token::Name::LEFT_BRACE, "Expected '{' before instance methods.");
	if (!brace_result.has_value())
	{
		return std::unexpected(brace_result.error());
	}

	const std::vector<std::string>& declared_associated_types = m_state.m_typeclass_associated_types[typeclass_name.m_lexeme];
	std::vector<MarmotStatement::Instance::AssociatedTypeBinding> associated_types;
	std::unordered_set<std::string> bound_associated_type_names;
	std::vector<MarmotStatement::Instance::Method> methods;

	while (!Match(Token::Name::RIGHT_BRACE))
	{
		if (IsAtEnd())
		{
			return std::unexpected(GenerateParserError("Expected '}' after instance body.", Previous()));
		}

		if (Match(Token::Name::TYPE))
		{
			MarmotResult::TokenResult assoc_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected associated type name.");
			if (!assoc_name_result.has_value())
			{
				return std::unexpected(assoc_name_result.error());
			}

			Token assoc_name = std::move(assoc_name_result.value());
			assoc_name.m_lexeme = Mangle(assoc_name.m_lexeme);
			if (!std::ranges::contains(declared_associated_types, assoc_name.m_lexeme))
			{
				return std::unexpected(GenerateParserError("Unknown associated type '" + assoc_name.m_lexeme + "' for class '" + typeclass_name.m_lexeme + "'.", assoc_name));
			}
			if (!bound_associated_type_names.insert(assoc_name.m_lexeme).second)
			{
				return std::unexpected(GenerateParserError("Duplicate associated type binding '" + assoc_name.m_lexeme + "'.", assoc_name));
			}

			MarmotResult::TokenResult defined_assoc_result = DefineName(assoc_name, false);
			if (!defined_assoc_result.has_value())
			{
				return std::unexpected(defined_assoc_result.error());
			}
			assoc_name = std::move(defined_assoc_result.value());

			MarmotResult::TokenResult equals_result = Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after associated type name.");
			if (!equals_result.has_value())
			{
				return std::unexpected(equals_result.error());
			}

			MarmotResult::TypeResult assoc_type_result = ParseType();
			if (!assoc_type_result.has_value())
			{
				return std::unexpected(assoc_type_result.error());
			}
			std::shared_ptr<MarmotType> assoc_type = std::move(assoc_type_result.value());

			MarmotResult::TokenResult assoc_semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after associated type binding.");
			if (!assoc_semicolon_result.has_value())
			{
				return std::unexpected(assoc_semicolon_result.error());
			}

			m_state.m_scopes.back().m_defined_types[assoc_name.m_lexeme] = assoc_type;
			associated_types.emplace_back(assoc_name, std::shared_ptr<MarmotType>(assoc_type));
			continue;
		}

		// An instance method is spelled `def show = fn(...) -> R => body;`, the same
		// binding form a module-level function uses.
		if (!Match(Token::Name::DEF))
		{
			return std::unexpected(GenerateParserError("Expected 'def' or associated type binding in instance body.", Peek(0)));
		}

		MarmotResult::TokenResult method_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected method name.");
		if (!method_name_result.has_value())
		{
			return std::unexpected(method_name_result.error());
		}
		Token method_name = std::move(method_name_result.value());

		MarmotResult::TokenResult method_equal_result = Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after instance method name.");
		if (!method_equal_result.has_value())
		{
			return std::unexpected(method_equal_result.error());
		}

		MarmotResult::TokenResult method_function_result = Consume(Token::Name::FUNCTION, "Expected 'fn' after '=' in an instance method binding.");
		if (!method_function_result.has_value())
		{
			return std::unexpected(method_function_result.error());
		}

		std::vector<Token> generic_params;
		bool has_generic_params = false;
		if (Match(Token::Name::LEFT_ANGLE))
		{
			has_generic_params = true;
			BeginScope();

			MarmotResult::TokenListResult generic_parse_result = ParseGenericParameters(nullptr);
			if (!generic_parse_result.has_value())
			{
				EndScope();
				return std::unexpected(generic_parse_result.error());
			}

			generic_params = std::move(generic_parse_result.value());
		}

		MarmotResult::TokenResult left_paren_result = Consume(Token::Name::LEFT_PAREN, "Expected '(' before method parameters.");
		if (!left_paren_result.has_value())
		{
			if (has_generic_params)
			{
				EndScope();
			}
			return std::unexpected(left_paren_result.error());
		}

		m_state.m_function_depth += 1;
		m_state.m_function_base_variable_index.push_back(m_state.m_total_variables);
		int prev_total_locals = m_state.m_total_locals_in_curr_scope;
		m_state.m_total_locals_in_curr_scope = 0;
		BeginScope();

		MarmotResult::FunctionParamsResult params_parse_result = ParseFunctionParameters();
		if (!params_parse_result.has_value())
		{
			EndScope();
			m_state.m_total_locals_in_curr_scope = prev_total_locals;
			m_state.m_function_base_variable_index.pop_back();
			m_state.m_function_depth -= 1;
			if (has_generic_params)
			{
				EndScope();
			}
			return std::unexpected(params_parse_result.error());
		}

		std::vector<std::pair<Token, std::shared_ptr<MarmotType>>> param_tuples = std::move(params_parse_result.value());
		ParamSplit split = SplitParamTuples(std::move(param_tuples));
		std::vector<Token> params = std::move(split.m_params);
		std::vector<std::shared_ptr<MarmotType>> param_types = std::move(split.m_types);

		MarmotResult::TokenResult return_colon_result = ConsumeReturnTypeSeparator("Expected '->' before return type.");
		if (!return_colon_result.has_value())
		{
			EndScope();
			m_state.m_total_locals_in_curr_scope = prev_total_locals;
			m_state.m_function_base_variable_index.pop_back();
			m_state.m_function_depth -= 1;
			if (has_generic_params)
			{
				EndScope();
			}
			return std::unexpected(return_colon_result.error());
		}

		MarmotResult::TypeResult return_type_result = ParseType();
		if (!return_type_result.has_value())
		{
			EndScope();
			m_state.m_total_locals_in_curr_scope = prev_total_locals;
			m_state.m_function_base_variable_index.pop_back();
			m_state.m_function_depth -= 1;
			if (has_generic_params)
			{
				EndScope();
			}
			return std::unexpected(return_type_result.error());
		}
		std::shared_ptr<MarmotType> return_type = std::move(return_type_result.value());

		MarmotResult::TokenResult fat_arrow_result = Consume(Token::Name::FAT_ARROW, "Expected '=>' before method body.");
		if (!fat_arrow_result.has_value())
		{
			EndScope();
			m_state.m_total_locals_in_curr_scope = prev_total_locals;
			m_state.m_function_base_variable_index.pop_back();
			m_state.m_function_depth -= 1;
			if (has_generic_params)
			{
				EndScope();
			}
			return std::unexpected(fat_arrow_result.error());
		}

		MarmotResult::ExpressionResult body_result = ParseExpression();
		if (!body_result.has_value())
		{
			EndScope();
			m_state.m_total_locals_in_curr_scope = prev_total_locals;
			m_state.m_function_base_variable_index.pop_back();
			m_state.m_function_depth -= 1;
			if (has_generic_params)
			{
				EndScope();
			}
			return std::unexpected(body_result.error());
		}
		std::unique_ptr<MarmotExpression> body = std::move(body_result.value());

		MarmotResult::TokenResult method_semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after method body.");
		if (!method_semicolon_result.has_value())
		{
			EndScope();
			m_state.m_total_locals_in_curr_scope = prev_total_locals;
			m_state.m_function_base_variable_index.pop_back();
			m_state.m_function_depth -= 1;
			if (has_generic_params)
			{
				EndScope();
			}
			return std::unexpected(method_semicolon_result.error());
		}

		EndScope();
		m_state.m_total_locals_in_curr_scope = prev_total_locals;
		m_state.m_function_base_variable_index.pop_back();
		m_state.m_function_depth -= 1;
		if (has_generic_params)
		{
			EndScope();
		}

		methods.emplace_back
		(
			method_name,
			MarmotStatement::FunctionDefinition(
				method_name,
				std::move(generic_params),
				std::move(params),
				std::move(param_types),
				std::move(return_type),
				std::move(body),
				std::nullopt,
				0,
				std::vector<MarmotType::ClassConstraint>()
			)
		);
	}

	MarmotResult::TokenResult instance_semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after instance body.");
	if (!instance_semicolon_result.has_value())
	{
		return std::unexpected(instance_semicolon_result.error());
	}

	std::unordered_map<std::string, std::shared_ptr<MarmotType>> associated_type_bindings;
	for (const MarmotStatement::Instance::AssociatedTypeBinding& binding : associated_types)
	{
		associated_type_bindings.emplace(binding.m_name.m_lexeme, binding.m_type);
	}
	std::vector<std::string> mangled_method_names;
	for (MarmotStatement::Instance::Method& method : methods)
	{
		MarmotStatement::FunctionDefinition& defun = method.m_definition;
		std::string mangled_name = MarmotType::MangleInstanceMethodName(method.m_name.m_lexeme, typeclass_name.m_lexeme, type_args);
		mangled_method_names.push_back(mangled_name);
		defun.m_name.m_lexeme = mangled_name;
	}

	const std::string declaring_module = m_context.m_current_module && m_context.m_current_module->HasModuleDeclaration() ? m_context.m_current_module->ModuleName() : std::string();
	m_state.m_class_instances[typeclass_name.m_lexeme].emplace_back(type_args, std::move(associated_type_bindings), constraints, std::move(mangled_method_names), declaring_module);

	scope_guard.m_parser = nullptr;
	EndScope();

	return std::make_unique<MarmotStatement>
	(
		MarmotStatement::Instance(std::move(typeclass_name), std::move(type_args), std::move(constraints), std::move(associated_types), std::move(methods))
	);
}

MarmotResult::StatementResult Parser::ParseSimpleStatement()
{
	ParseState checkpoint = m_state;

	MarmotResult::ExpressionResult expr_result = ParseExpression();
	if (!expr_result.has_value())
	{
		return std::unexpected(std::move(expr_result.error()));
	}

	std::unique_ptr<MarmotExpression> expr = std::move(expr_result.value());
	MarmotResult::TokenResult semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after expression.");
	if (!semicolon_result.has_value())
	{
		if (Check(Token::Name::RIGHT_BRACE, 0))
		{
			m_state = std::move(checkpoint);
			return NoMatch<std::unique_ptr<MarmotStatement>>();
		}

		// A for loop ends in a block, so it reads as finished, but it is an
		// expression like any other: the missing ';' belongs after its '}',
		// not before the line that follows.
		if (expr->IsExpression<MarmotExpression::For>())
		{
			return std::unexpected(GenerateParserError("Expected ';' after the for loop: it is an expression, and as a statement it ends with ';'.", Previous()));
		}

		return std::unexpected(std::move(semicolon_result.error()));
	}

	Token semi_colon = std::move(semicolon_result.value());
	return std::make_unique<MarmotStatement>(MarmotStatement::ExpressionStatement(semi_colon, std::move(expr)));
}

MarmotResult::StatementResult Parser::ParseForeignStatement()
{
	return Consume(Token::Name::TEXT_LITERAL, "Expected the foreign symbol, or the library of a foreign block.")
		.and_then
		(
			[this](Token&& name) -> MarmotResult::StatementResult
			{
				if (Match(Token::Name::LEFT_BRACE))
				{
					return ParseForeignBlock(name);
				}

				return ParseForeignDeclaration(name, std::nullopt);
			}
		);
}

// foreign "library" { "symbol" Name : Type; ... } declares each function as if
// it were written `foreign "symbol" Name : Type from "library";`.
MarmotResult::StatementResult Parser::ParseForeignBlock(const Token& library)
{
	std::vector<std::unique_ptr<MarmotStatement>> declarations;
	while (!Check(Token::Name::RIGHT_BRACE, 0) && !IsAtEnd())
	{
		MarmotResult::TokenResult symbol = Consume(Token::Name::TEXT_LITERAL, "Expected the symbol of a foreign function in the foreign block.");
		if (!symbol.has_value())
		{
			return std::unexpected(std::move(symbol.error()));
		}

		MarmotResult::StatementResult declaration = ParseForeignDeclaration(symbol.value(), library.m_lexeme);
		if (!declaration.has_value())
		{
			return declaration;
		}

		if (declaration.value()->GetStatement<MarmotStatement::ForeignDefinition>().m_local_index.has_value())
		{
			return std::unexpected(GenerateParserError("A foreign block can only appear at the top level of a module.", library));
		}

		declarations.emplace_back(std::move(declaration.value()));
	}

	MarmotResult::TokenResult closing = Consume(Token::Name::RIGHT_BRACE, "Expected '}' after the foreign block.");
	if (!closing.has_value())
	{
		return std::unexpected(std::move(closing.error()));
	}

	if (declarations.empty())
	{
		return std::unexpected(GenerateParserError(std::format("The foreign block for \"{}\" declares no functions.", library.m_lexeme), library));
	}

	for (size_t index = 1uz; index < declarations.size(); index += 1uz)
	{
		m_pending_statements.push(std::move(declarations[index]));
	}
	return std::move(declarations.front());
}

MarmotResult::StatementResult Parser::ParseForeignDeclaration(const Token& foreign_name, const std::optional<std::string>& block_library)
{
	return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected foreign function name.")
		.and_then
		(
			[this, &foreign_name, &block_library](Token&& function_name) ->MarmotResult::StatementResult
			{
				function_name.m_lexeme = Mangle(function_name.m_lexeme);

				return Consume(Token::Name::SINGLE_COLON, "Expected ':' before foreign function type.")
					.and_then
					(
						[&foreign_name, &function_name, &block_library, this](Token&&) ->MarmotResult::StatementResult
						{
							constexpr bool is_variable = true;
							return DefineName(function_name, is_variable)
								.and_then
								(
									[&foreign_name, &function_name, &block_library, this](Token&& name) ->MarmotResult::StatementResult
									{
										std::optional<int> local_index = RegisterOrUpdateLocalVariable(name.m_lexeme);
										constexpr bool is_foreign = true;
										BeginScope();
										struct ForeignTypeScopeGuard
										{
											Parser* m_parser;
											bool m_prev_allow_implicit_generic_params;

											explicit ForeignTypeScopeGuard(Parser* parser)
												: m_parser(parser),
												  m_prev_allow_implicit_generic_params(parser->m_state.m_allow_implicit_generic_params)
											{
												m_parser->m_state.m_allow_implicit_generic_params = true;
											}

											~ForeignTypeScopeGuard()
											{
												m_parser->m_state.m_allow_implicit_generic_params = m_prev_allow_implicit_generic_params;
												m_parser->EndScope();
											}
										} foreign_type_scope_guard(this);

										return ParseType(is_foreign)
											.and_then
											(
												[&foreign_name, &function_name, &local_index, &block_library, this](std::shared_ptr<MarmotType>&& type)->MarmotResult::StatementResult
												{
													if (!type->IsType<MarmotType::FunctionType>())
													{
														return std::unexpected(GenerateParserError("'foreign' only applies to function types.", function_name));
													}

													std::optional<std::string> library = block_library;
													if (!block_library.has_value() && Check(Token::Name::IDENTIFIER_LITERAL, 0) && Peek(0).m_lexeme == "from")
													{
														Advance();
														MarmotResult::TokenResult library_name = Consume(Token::Name::TEXT_LITERAL, "Expected the library name after 'from'.");
														if (!library_name.has_value())
														{
															return std::unexpected(std::move(library_name.error()));
														}
														library = library_name->m_lexeme;
													}

													return Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after foreign function type.")
														.and_then
														(
															[&foreign_name, &function_name, &type, &local_index, &library](Token&&) ->MarmotResult::StatementResult
															{
																if (!library.has_value())
																{
																	type->GetType<MarmotType::FunctionType>().m_primitive = FindPrimitive(foreign_name.m_lexeme);
																}
																return std::make_unique<MarmotStatement>(MarmotStatement::ForeignDefinition(function_name, foreign_name.m_lexeme, std::move(type), std::move(local_index), std::move(library)));
															}
														);
												}
											);
									}
								);
						}
					);
			}
		);
}

MarmotResult::ExpressionResult Parser::ParseMatchExpressionWithScrutinee(Token& match_keyword, std::unique_ptr<MarmotExpression>&& expr)
{
	return Consume(Token::Name::WITH, "Expected 'with' after match expression.")
		.and_then
		(
			[expr = std::move(expr), &match_keyword, this](Token&&) mutable ->MarmotResult::ExpressionResult
			{
				std::vector<std::unique_ptr<MarmotExpression>> cases;

				while (Match(Token::Name::CASE))
				{
					Token& case_keyword = Previous();
					MarmotResult::ExpressionResult case_result = ParseCaseExpression(case_keyword);
					if (!case_result.has_value())
					{
						return std::unexpected(std::move(case_result.error()));
					}
					cases.emplace_back(std::move(case_result.value()));
				}

				if (cases.empty())
				{
					return std::unexpected(GenerateParserError("Expected at least one case.", match_keyword));
				}

				return std::make_unique<MarmotExpression>(MarmotExpression::Match(match_keyword, std::move(expr), std::move(cases)));
			}
		);
}

MarmotResult::ExpressionResult Parser::ParseMatchExpression()
{
	Token& match_keyword = Previous();
	return ParseExpression()
		.and_then
		(
			[&match_keyword, this](std::unique_ptr<MarmotExpression>&& expr) ->MarmotResult::ExpressionResult
			{
				return ParseMatchExpressionWithScrutinee(match_keyword, std::move(expr));
			}
		);
}

MarmotResult::ExpressionResult Parser::ParseIfElseExpression()
{
	Token& if_token = Previous();
	return ParseExpression()
		.and_then
		(
			[&if_token, this](std::unique_ptr<MarmotExpression>&& condition) -> MarmotResult::ExpressionResult
			{
				return Consume(Token::Name::THEN, "Expected 'then'.")
					.and_then
					(
						[&if_token, &condition, this](Token&& then_token) -> MarmotResult::ExpressionResult
						{
							return ParseExpression()
								.and_then
								(
									[&if_token, &condition, &then_token, this](std::unique_ptr<MarmotExpression>&& true_branch) -> MarmotResult::ExpressionResult
									{
										return Consume(Token::Name::ELSE, "Expected 'else'.")
											.and_then
											(
												[&if_token, &condition, &true_branch, &then_token, this](Token&& else_token) -> MarmotResult::ExpressionResult
												{
													return ParseExpression()
														.and_then
														(
															[&if_token, &condition, &true_branch, &then_token, &else_token](std::unique_ptr<MarmotExpression>&& else_branch) -> MarmotResult::ExpressionResult
															{
																return std::make_unique<MarmotExpression>(MarmotExpression::IfElse(if_token, then_token, else_token, std::move(condition), std::move(true_branch), std::move(else_branch)));
															}
														);
												}
											);
									}
								);
						}
					);
			}
		);
}

MarmotResult::ExpressionResult Parser::ParseFunctionExpression()
{
	Token& keyword = Previous();
	// In `x |> fn(a) => f(a) |> g` the lambda is one stage: its body stops before the
	// next `|>`, as any pipe operand does, rather than taking the rest of the pipeline.
	const bool is_pipe_stage = std::exchange(m_state.m_next_function_is_pipe_stage, false);

	// Parse optional generic parameters <T, U, ...>
	// Create scope BEFORE parsing so DefineName() in ParseGenericParameters adds them to this scope
	std::vector<Token> generic_params;
	std::vector<std::shared_ptr<MarmotType>> generic_param_types;
	bool has_generic_params = false;

	if (Match(Token::Name::LEFT_ANGLE))
	{
		has_generic_params = true;
		BeginScope();  // Create scope for generic parameters

		MarmotResult::TokenListResult generic_parse_result = ParseGenericParameters(&generic_param_types);
		if (!generic_parse_result.has_value())
		{
			EndScope();  // Clean up scope on error
			return std::unexpected(generic_parse_result.error());
		}

		generic_params = std::move(generic_parse_result.value());
	}

	MarmotResult::TokenResult left_paren_result = Consume(Token::Name::LEFT_PAREN, "Expected '(' before function parameters.");
	if (!left_paren_result.has_value())
	{
		if (has_generic_params)
		{
			EndScope();
		}
		return std::unexpected(left_paren_result.error());
	}

	m_state.m_function_depth += 1;
	m_state.m_function_base_variable_index.push_back(m_state.m_total_variables);
	int prev_total_locals = m_state.m_total_locals_in_curr_scope;
	m_state.m_total_locals_in_curr_scope = 0;
	BeginScope();

	// Unwinds the function scope (and the generic parameter scope, if one was opened).
	auto unwind_function_state = [prev_total_locals, has_generic_params, this]()
	{
		EndScope();
		m_state.m_total_locals_in_curr_scope = prev_total_locals;
		m_state.m_function_base_variable_index.pop_back();
		m_state.m_function_depth -= 1;
		if (has_generic_params)
		{
			EndScope();
		}
	};

	MarmotResult::FunctionParamsResult params_parse_result = ParseFunctionParameters(true);
	if (!params_parse_result.has_value())
	{
		unwind_function_state();
		return std::unexpected(params_parse_result.error());
	}

	std::vector<std::pair<Token, std::shared_ptr<MarmotType>>> param_tuples = std::move(params_parse_result.value());
	ParamSplit split = SplitParamTuples(std::move(param_tuples));
	std::vector<Token> params = std::move(split.m_params);
	std::vector<std::shared_ptr<MarmotType>> param_types = std::move(split.m_types);

	std::shared_ptr<MarmotType> return_type = MarmotType::MakeUndecidedType();
	if (Match(Token::Name::THIN_ARROW))
	{
		MarmotResult::TypeResult return_type_result = ParseType();
		if (!return_type_result.has_value())
		{
			unwind_function_state();
			return std::unexpected(return_type_result.error());
		}

		return_type = std::move(return_type_result.value());
	}

	// Parse optional 'where' clauses. A where clause may name constraints the
	// signature never mentions, so they must be recorded explicitly.
	std::vector<MarmotType::ClassConstraint> constraints;
	if (Match(Token::Name::WHERE))
	{
		std::expected<std::vector<MarmotType::ClassConstraint>, CompilerError> constraints_result = ParseClassConstraints(keyword);
		if (!constraints_result.has_value())
		{
			unwind_function_state();
			return std::unexpected(constraints_result.error());
		}

		constraints = std::move(constraints_result.value());
	}

	MarmotResult::TokenResult fat_arrow_result = Consume(Token::Name::FAT_ARROW, "Expected '=>' before function body.");
	if (!fat_arrow_result.has_value())
	{
		unwind_function_state();
		return std::unexpected(fat_arrow_result.error());
	}

	size_t prev_constraints_size = m_state.m_active_constraints.size();
	std::vector<MarmotType::ClassConstraint> propagated_constraints = CollectSignatureConstraints(param_types, return_type);
	for (MarmotType::ClassConstraint& propagated_constraint : propagated_constraints)
	{
		AppendUniqueConstraint(constraints, std::move(propagated_constraint));
	}
	PushActiveConstraints(constraints);
	ActiveConstraintGuard constraint_guard(this, prev_constraints_size);

	// If body is a block, parse just the block without continuing to parse calls
	// This prevents `fn() => {}()` from parsing `()` as part of the function body.
	//
	// A record update is not a block: its value is a struct you may well want to project
	// or call straight away, so `=> { c with p = 1 }.port` must behave exactly as it does
	// in expression position. It therefore does continue through the postfix chain. The
	// asymmetry is deliberate - the two constructs genuinely differ.
	MarmotResult::ExpressionResult body_result = [this, is_pipe_stage]() -> MarmotResult::ExpressionResult
		{
			if (!Match(Token::Name::LEFT_BRACE))
			{
				return is_pipe_stage ? ParseBitwiseOr() : ParseExpression();
			}

			if (!ProbeRecordUpdate())
			{
				return ParseBlockExpression();
			}

			return ParseRecordUpdate()
				.and_then
				(
					[this](std::unique_ptr<MarmotExpression>&& record_update) -> MarmotResult::ExpressionResult
					{
						return ParsePostfixChain(std::move(record_update));
					}
				);
		}();

	if (!body_result.has_value())
	{
		unwind_function_state();
		return std::unexpected(body_result.error());
	}

	unwind_function_state();

	return std::make_unique<MarmotExpression>(MarmotExpression::Function(keyword, std::move(generic_params), std::move(params), std::move(param_types), std::move(return_type), std::move(body_result.value()), m_state.m_total_variables, std::move(constraints)));
}

MarmotResult::ExpressionResult Parser::ParseCaseExpression(Token& keyword)
{
	BeginScope();
	MarmotResult::PatternResult pattern_result = ParsePattern();
	if (!pattern_result.has_value())
	{
		EndScope();
		return std::unexpected(std::move(pattern_result.error()));
	}

	std::unique_ptr<MarmotPattern> pattern = std::move(pattern_result.value());

	std::unique_ptr<MarmotExpression> guard = nullptr;
	if (Match(Token::Name::IF))
	{
		MarmotResult::ExpressionResult guard_result = ParseExpression();
		if (!guard_result.has_value())
		{
			EndScope();
			return std::unexpected(std::move(guard_result.error()));
		}
		guard = std::move(guard_result.value());
	}

	return Consume(Token::Name::FAT_ARROW, "Expected '=>' after case.")
		.and_then
		(
			[&keyword, &pattern, &guard, this](Token&&)->MarmotResult::ExpressionResult
			{
				return ParseExpression()
					.and_then
					(
						[&keyword, &pattern, &guard, this](std::unique_ptr<MarmotExpression>&& case_expr)->MarmotResult::ExpressionResult
						{
							EndScope();
							return std::make_unique<MarmotExpression>(MarmotExpression::Case(keyword, std::move(pattern), std::move(case_expr), std::move(guard)));
						}
					);
			}
		);
}

MarmotResult::PatternResult Parser::ParsePattern()
{
	MarmotResult::PatternResult pattern = ParseAlternatives();
	while (pattern.has_value() && Match(Token::Name::AS))
	{
		const Token as = Previous();
		if (!Match(Token::Name::IDENTIFIER_LITERAL) || Previous().m_lexeme == "_")
		{
			return std::unexpected(GenerateParserError("Expected a name after 'as' in a pattern.", as));
		}

		pattern = ParseBindingPattern(Token(Previous()))
			.transform([&pattern](std::unique_ptr<MarmotPattern>&& binding)
			{
				return std::make_unique<MarmotPattern>(MarmotPattern::As(std::move(pattern.value()), std::move(binding)));
			});
	}
	return pattern;
}

// An alternative after the first is parsed in a scope of its own, so it may bind
// the names the first does, and its bindings then take the first's locals: the
// case sees one set of names, whichever alternative matched.
MarmotResult::PatternResult Parser::ParseAlternatives()
{
	MarmotResult::PatternResult first = ParsePrimaryPattern();
	if (!first.has_value() || !Check(Token::Name::SINGLE_BAR, 0))
	{
		return first;
	}

	std::vector<std::unique_ptr<MarmotPattern>> alternatives;
	alternatives.push_back(std::move(first.value()));
	const std::unordered_map<std::string, std::optional<int>> locals = alternatives.front()->Bindings(false)
		| std::views::transform([](const MarmotPattern::Binding* binding) { return std::pair{ binding->m_name.m_lexeme, binding->m_local_index }; })
		| std::ranges::to<std::unordered_map>();

	while (Match(Token::Name::SINGLE_BAR))
	{
		const Token bar = Previous();
		BeginScope();
		MarmotResult::PatternResult alternative = ParsePrimaryPattern();
		EndScope();
		if (!alternative.has_value())
		{
			return alternative;
		}

		const std::vector<MarmotPattern::Binding*> bound = alternative.value()->Bindings(false);
		for (const MarmotPattern::Binding* binding : bound)
		{
			if (!locals.contains(binding->m_name.m_lexeme))
			{
				return std::unexpected(GenerateParserError(std::format("'{}' is bound here but not by the first alternative; every alternative of '|' binds the same names.", binding->m_name.m_lexeme), binding->m_name));
			}
		}
		for (const std::string& name : locals | std::views::keys)
		{
			if (std::ranges::none_of(bound, [&name](const MarmotPattern::Binding* binding) { return binding->m_name.m_lexeme == name; }))
			{
				return std::unexpected(GenerateParserError(std::format("The first alternative binds '{}' and this one does not; every alternative of '|' binds the same names.", name), bar));
			}
		}

		for (MarmotPattern::Binding* binding : alternative.value()->Bindings(true))
		{
			binding->m_local_index = locals.at(binding->m_name.m_lexeme);
		}
		alternatives.push_back(std::move(alternative.value()));
	}

	return std::make_unique<MarmotPattern>(MarmotPattern::Or(std::move(alternatives)));
}

MarmotResult::PatternResult Parser::ParsePrimaryPattern()
{
	if (Match(Token::Name::LEFT_PAREN))
	{
		Token left_paren = Previous();

		if (Match(Token::Name::RIGHT_PAREN))
		{
			return std::make_unique<MarmotPattern>(MarmotPattern::Literal(Previous(), MarmotPattern::LiteralKind::Unit));
		}

		return ParsePattern()
			.and_then
			(
				[this, left_paren](std::unique_ptr<MarmotPattern>&& first_pattern) -> MarmotResult::PatternResult
				{
					if (Match(Token::Name::COMMA))
					{
						std::vector<std::unique_ptr<MarmotPattern>> elements;
						elements.push_back(std::move(first_pattern));

						do
						{
							MarmotResult::PatternResult elem_result = ParsePattern();
							if (!elem_result)
							{
								return std::unexpected(elem_result.error());
							}
							elements.push_back(std::move(elem_result.value()));
						} while (Match(Token::Name::COMMA));

						return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after tuple pattern.")
							.and_then
							(
								[&elements, left_paren](Token&&) -> MarmotResult::PatternResult
								{
									return std::make_unique<MarmotPattern>(MarmotPattern::Tuple(left_paren, std::move(elements)));
								}
							);
					}

					return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after pattern.")
						.and_then
						(
							[pattern = std::move(first_pattern)](Token&&) mutable -> MarmotResult::PatternResult
							{
								return std::move(pattern);
							}
						);
				}
			);
	}

	if (Match(Token::Name::LEFT_BRACKET))
	{
		Token left_bracket = Previous();
		std::vector<std::unique_ptr<MarmotPattern>> elements;
		std::optional<MarmotPattern::Array::Rest> rest;
		if (!Check(Token::Name::RIGHT_BRACKET, 0))
		{
			do
			{
				if (Match(Token::Name::DOUBLE_DOT))
				{
					Token dots = Previous();
					if (rest.has_value())
					{
						return std::unexpected(GenerateParserError("An array pattern takes at most one '..'.", dots));
					}

					MarmotResult::PatternResult rest_result = ParseArrayRest(std::move(dots));
					if (!rest_result)
					{
						return std::unexpected(rest_result.error());
					}
					rest.emplace(elements.size(), std::move(rest_result.value()));
				}
				else
				{
					MarmotResult::PatternResult elem_result = ParsePattern();
					if (!elem_result)
					{
						return std::unexpected(elem_result.error());
					}
					elements.push_back(std::move(elem_result.value()));
				}
			} while (Match(Token::Name::COMMA));
		}

		return Consume(Token::Name::RIGHT_BRACKET, "Expected ']' after array pattern.")
			.and_then
			(
				[&elements, &rest, left_bracket](Token&&) -> MarmotResult::PatternResult
				{
					return std::make_unique<MarmotPattern>(MarmotPattern::Array(left_bracket, std::move(elements), std::move(rest)));
				}
			);
	}

	if (Match(Token::Name::TRUE, Token::Name::FALSE))
	{
		return std::make_unique<MarmotPattern>(MarmotPattern::Literal(Previous(), MarmotPattern::LiteralKind::Bool));
	}

	if (Check(Token::Name::FLOAT_LITERAL, 0) || Check(Token::Name::INTEGER_LITERAL, 0) || Check(Token::Name::SINGLE_MINUS, 0) || Check(Token::Name::SINGLE_PLUS, 0))
	{
		return ParseNumericLiteralPattern()
			.and_then([this](std::unique_ptr<MarmotPattern>&& literal) -> MarmotResult::PatternResult
			{
				return Check(Token::Name::DOUBLE_DOT, 0) ? ParseRangePattern(std::move(literal)) : std::move(literal);
			});
	}

	if (Match(Token::Name::TEXT_LITERAL))
	{
		return std::make_unique<MarmotPattern>(MarmotPattern::Literal(Previous(), MarmotPattern::LiteralKind::Text));
	}

	if (Match(Token::Name::IDENTIFIER_LITERAL))
	{
		Token identifier = Previous();
		if (identifier.m_lexeme == "_")
		{
			return std::make_unique<MarmotPattern>(MarmotPattern::Wildcard(identifier));
		}

		return MatchNameResolution()
			.and_then
			(
				[&identifier, this](Token&& resolved) -> MarmotResult::PatternResult
				{
					resolved.m_lexeme = Mangle(resolved.m_lexeme);

					// A pattern names a constructor by the rule an expression does: in scope,
					// through a `use`, or as Module::Record and Module::Union::Member.
					ConstructorResolutionResult resolution = ResolveConstructorName(resolved, resolved.m_lexeme);
					if (!resolution.has_value())
					{
						return std::unexpected(resolution.error());
					}

					if (resolution.value().has_value())
					{
						const bool is_union = !resolution.value()->m_is_struct;
						resolved.m_lexeme = resolution.value()->m_constructor_name;
						if (Check(Token::Name::LEFT_BRACE, 0))
						{
							if (is_union)
							{
								return std::unexpected(GenerateParserError("A variant's fields have no names; match it with Name(...).", Peek(0)));
							}
							Advance();
							return ParseRecordPattern(std::move(resolved));
						}
						if (Match(Token::Name::LEFT_PAREN))
						{
							std::vector<std::unique_ptr<MarmotPattern>> args;
							if (!Match(Token::Name::RIGHT_PAREN))
							{
								do
								{
									MarmotResult::PatternResult arg_result = ParsePattern();
									if (!arg_result)
									{
										return std::unexpected(arg_result.error());
									}
									args.push_back(std::move(arg_result.value()));
								} while (Match(Token::Name::COMMA));

								if (!Match(Token::Name::RIGHT_PAREN))
								{
									return std::unexpected(GenerateParserError("Expected ')' after constructor pattern.", Peek(0)));
								}
							}

							return std::make_unique<MarmotPattern>(MarmotPattern::Constructor(resolved, std::string(resolved.m_lexeme), std::move(args), is_union));
						}

						return std::make_unique<MarmotPattern>(MarmotPattern::Constructor(resolved, std::string(resolved.m_lexeme), {}, is_union));
					}

					if (resolved.m_lexeme.find(NameSeparator) != std::string::npos)
					{
						return std::unexpected(GenerateParserError(DescribeUnknownConstructor(resolved.m_lexeme), resolved));
					}

					return ParseBindingPattern(std::move(identifier));
				}
			);
	}

	return std::unexpected(GenerateParserError("Expected pattern.", Peek(0)));
}

MarmotResult::PatternResult Parser::ParseNumericLiteralPattern()
{
	std::string sign;
	if (Match(Token::Name::SINGLE_MINUS, Token::Name::SINGLE_PLUS))
	{
		sign = Previous().m_token_name == Token::Name::SINGLE_MINUS ? "-" : "";
		if (!Check(Token::Name::FLOAT_LITERAL, 0) && !Check(Token::Name::INTEGER_LITERAL, 0))
		{
			return std::unexpected(GenerateParserError("Expected numeric literal after unary sign in pattern.", Previous()));
		}
	}

	if (Match(Token::Name::FLOAT_LITERAL))
	{
		Token literal = Previous();
		literal.m_lexeme = sign + literal.m_lexeme;
		return std::make_unique<MarmotPattern>(MarmotPattern::Literal(literal, MarmotPattern::LiteralKind::Float));
	}
	if (Match(Token::Name::INTEGER_LITERAL))
	{
		Token literal = Previous();
		literal.m_lexeme = sign + literal.m_lexeme;
		return MakeNumericLiteralPattern(std::move(literal));
	}
	return std::unexpected(GenerateParserError("Expected numeric literal in pattern.", Peek(0)));
}

// `start..end` and `start..step..end` match the Ints the same range visits. The
// step is 1 or -1, which keeps what a case covers an interval; other steps belong
// in a guard.
MarmotResult::PatternResult Parser::ParseRangePattern(std::unique_ptr<MarmotPattern>&& start)
{
	std::vector<std::unique_ptr<MarmotPattern>> parts;
	parts.push_back(std::move(start));
	while (parts.size() < 3uz && Match(Token::Name::DOUBLE_DOT))
	{
		MarmotResult::PatternResult part = ParseNumericLiteralPattern();
		if (!part.has_value())
		{
			return part;
		}
		parts.push_back(std::move(part.value()));
	}

	std::vector<int64_t> values;
	for (const std::unique_ptr<MarmotPattern>& part : parts)
	{
		const MarmotPattern::Literal& literal = part->GetPattern<MarmotPattern::Literal>();
		if (literal.m_kind != MarmotPattern::LiteralKind::Integer)
		{
			return std::unexpected(GenerateParserError("A range pattern's start, step and end are Int literals.", literal.m_token));
		}
		const std::optional<int64_t> value = ParseIntegerLiteral(literal.m_token.m_lexeme);
		if (!value.has_value())
		{
			return std::unexpected(GenerateParserError("This literal does not fit in an Int.", literal.m_token));
		}
		values.push_back(value.value());
	}

	const Token& start_token = parts.front()->GetPattern<MarmotPattern::Literal>().m_token;
	const int64_t first = values.front();
	const int64_t step = values.size() == 3uz ? values[1uz] : 1;
	const int64_t end = values.back();
	if (step != 1 && step != -1)
	{
		return std::unexpected(GenerateParserError("A range pattern steps by 1 or -1; test other steps with a guard, as in 'case n if n % 2 == 0'.", parts[1uz]->GetPattern<MarmotPattern::Literal>().m_token));
	}
	if (step == 1 ? first >= end : first <= end)
	{
		return std::unexpected(GenerateParserError("This range visits no value, so the pattern can never match.", start_token));
	}
	return step == 1
		? std::make_unique<MarmotPattern>(MarmotPattern::Range(start_token, first, end - 1))
		: std::make_unique<MarmotPattern>(MarmotPattern::Range(start_token, end + 1, first));
}

// Every field is named with `=`, and `..`, last, says the fields not named are
// left out on purpose; which ones exist is the type checker's to say.
MarmotResult::PatternResult Parser::ParseRecordPattern(Token&& name)
{
	std::vector<MarmotPattern::Record::Field> fields;
	std::optional<Token> rest;
	do
	{
		if (Match(Token::Name::DOUBLE_DOT))
		{
			rest = Previous();
			break;
		}
		MarmotResult::TokenResult field = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected a field name, or '..' to leave the rest out, in a record pattern.");
		if (!field.has_value())
		{
			return std::unexpected(field.error());
		}
		MarmotResult::TokenResult equal = Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after the field name; a record pattern names each field as 'field = pattern'.");
		if (!equal.has_value())
		{
			return std::unexpected(equal.error());
		}
		MarmotResult::PatternResult pattern = ParsePattern();
		if (!pattern.has_value())
		{
			return pattern;
		}
		fields.emplace_back(field.value(), std::move(pattern.value()));
	} while (Match(Token::Name::COMMA));

	return Consume(Token::Name::RIGHT_BRACE, rest.has_value() ? "Expected '}' after '..'; it comes last in a record pattern." : "Expected '}' after record pattern.")
		.transform([&name, &fields, &rest](Token&&)
		{
			return std::make_unique<MarmotPattern>(MarmotPattern::Record(name, std::string(name.m_lexeme), std::move(fields), std::move(rest)));
		});
}

MarmotResult::PatternResult Parser::ParseBindingPattern(Token&& name)
{
	constexpr bool is_variable = true;
	return DefineName(name, is_variable)
		.and_then
		(
			[this](Token&& defined_name) -> MarmotResult::PatternResult
			{
				std::optional<int> local_index = RegisterOrUpdateLocalVariable(defined_name.m_lexeme);
				return std::make_unique<MarmotPattern>(MarmotPattern::Binding(defined_name, std::move(local_index)));
			}
		);
}

MarmotResult::PatternResult Parser::ParseArrayRest(Token&& dots)
{
	if (!Match(Token::Name::IDENTIFIER_LITERAL))
	{
		return std::make_unique<MarmotPattern>(MarmotPattern::Wildcard(dots));
	}

	Token name = Previous();
	if (name.m_lexeme == "_")
	{
		return std::make_unique<MarmotPattern>(MarmotPattern::Wildcard(name));
	}
	return ParseBindingPattern(std::move(name));
}

MarmotResult::StatementResult Parser::ParseStatement()
{
	return ParseSimpleStatement();
}

MarmotResult::TypeResult Parser::ParseTypeOfOne(Token::Name keyword, std::string_view argument_noun, MakeTypeOfOne make)
{
	return ParseWhen<std::shared_ptr<MarmotType>>(m_state,
		keyword,
		[this, keyword, argument_noun, make](Token&&) -> MarmotResult::TypeResult
		{
			return Consume(Token::Name::LEFT_ANGLE, std::format("Expected '<' after '{}'.", Token::Spelling(keyword)))
				.and_then([this](Token&&) { return ParseType(); })
				.and_then
				(
					[this, argument_noun, make](std::shared_ptr<MarmotType>&& type) -> MarmotResult::TypeResult
					{
						return ConsumeTypeRightAngle(std::format("Expected '>' after {}.", argument_noun))
							.transform([&type, make](Token&&) { return make(type); });
					}
				);
		}
	);
}

MarmotResult::TypeResult Parser::ParseType(bool is_foreign)
{
	return ParseChoice<std::shared_ptr<MarmotType>>(m_state,
		[this]() -> MarmotResult::TypeResult
		{
			const std::optional<std::shared_ptr<MarmotType>> keyword_type = KeywordType(Peek(m_state, 0).m_token_name);
			if (!keyword_type.has_value())
			{
				return NoMatch<std::shared_ptr<MarmotType>>();
			}

			Advance(m_state);
			return *keyword_type;
		},
		[this]() -> MarmotResult::TypeResult
		{
			return ParseTypeOfOne(Token::Name::ARRAY, "array type", &MarmotType::MakeArrayType);
		},
		[this]() -> MarmotResult::TypeResult
		{
			return ParseTypeOfOne(Token::Name::RANGE, "range element type", &MarmotType::MakeRangeType);
		},
		[this]() -> MarmotResult::TypeResult
		{
			return ParseTypeOfOne(Token::Name::WORKER, "worker result type", &MarmotType::MakeWorkerType);
		},
		[this]() -> MarmotResult::TypeResult
		{
			return ParseTypeOfOne(Token::Name::CHANNEL_TYPE, "channel element type", &MarmotType::MakeChannelType);
		},
		[this, is_foreign]() -> MarmotResult::TypeResult
		{
			return ParseWhen<std::shared_ptr<MarmotType>>(m_state,
				Token::Name::FUNCTION,
				[this, is_foreign](Token&&) -> MarmotResult::TypeResult
				{
					std::function<MarmotResult::TypeResult(std::vector<std::shared_ptr<MarmotType>>&&)> func_type_aux_func = [is_foreign, this](std::vector<std::shared_ptr<MarmotType>>&& types) ->MarmotResult::TypeResult
						{
							return Consume(Token::Name::THIN_ARROW, "Expected '->' before return type token.")
								.and_then
								(
									[&types, is_foreign, this](Token&&) ->MarmotResult::TypeResult
									{
										return ParseType()
											.and_then
											(
												[&types, is_foreign](std::shared_ptr<MarmotType>&& return_type) ->MarmotResult::TypeResult
												{
													return MarmotType::MakeFunctionType(std::move(types), std::move(return_type), is_foreign);
												}
											);
									}
								);
						};
					return Consume(Token::Name::LEFT_PAREN, "Expected '(' before function argument types.")
						.and_then
						(
							[&func_type_aux_func, this](Token&&)
							{
								if (!Match(Token::Name::RIGHT_PAREN))
								{
									return ParseDelimitedZeroOrMoreLimited<std::shared_ptr<MarmotType>>
										(
											[this]() { return ParseType(); },
											[this]() { return Consume(Token::Name::COMMA, "Expected ',' after argument type"); },
											[this]() { return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after argument types."); }
										)
										.and_then
										(
											[&func_type_aux_func](std::vector<std::shared_ptr<MarmotType>>&& types) ->MarmotResult::TypeResult
											{
												return func_type_aux_func(std::move(types));
											}
										);
								}
								else
								{
									return func_type_aux_func({});
								}
							}
						);
				}
			);
		},
		[this]() -> MarmotResult::TypeResult
		{
			return ParseWhen<std::shared_ptr<MarmotType>>(m_state,
				Token::Name::LEFT_PAREN,
				[this](Token&&) -> MarmotResult::TypeResult
				{
					if (Match(Token::Name::RIGHT_PAREN))
					{
						return MarmotType::MakeLiteralType<MarmotType::UnitType>();
					}

					return ParseType()
						.and_then
						(
							[this](std::shared_ptr<MarmotType>&& first_type) -> MarmotResult::TypeResult
							{
								if (Match(Token::Name::COMMA))
								{
									std::vector<std::shared_ptr<MarmotType>> element_types;
									element_types.push_back(std::move(first_type));

									do
									{
										MarmotResult::TypeResult elem_result = ParseType();
										if (!elem_result)
										{
											return elem_result;
										}
										element_types.push_back(std::move(elem_result.value()));
									} while (Match(Token::Name::COMMA));

									return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after tuple types.")
										.and_then
										(
											[&element_types](Token&&) -> MarmotResult::TypeResult
											{
												return MarmotType::MakeTupleType(std::move(element_types));
											}
										);
								}
								else
								{
									return Consume
									(
										Token::Name::RIGHT_PAREN, "Expected ')' after type.")
										.and_then([&first_type](Token&&) -> MarmotResult::TypeResult
											{
												return first_type;
											}
										);
								}
							}
						);
				}
			);
		},
		[this]() -> MarmotResult::TypeResult
		{
			return ParseWhen<std::shared_ptr<MarmotType>>(m_state,
				Token::Name::IDENTIFIER_LITERAL,
				[this](Token&&) -> MarmotResult::TypeResult
				{
					return MatchNameResolution()
						.and_then
						(
							[this](Token&& type_name) ->MarmotResult::TypeResult
							{
								std::string mangled_name = Mangle(type_name.m_lexeme);
								std::vector<Scope>::const_reverse_iterator found_scope_it = FindTypeScope(type_name.m_lexeme);

								// A declared type named Ref takes precedence over the built-in reference type.
								if (found_scope_it == m_state.m_scopes.crend() && type_name.m_lexeme == "Ref")
								{
									return ParseRefTypeArguments();
								}

								std::shared_ptr<MarmotType> base_type = nullptr;
								const std::vector<std::string>* alias_generic_params = nullptr;
								auto try_parse_associated_type = [this, &type_name]() -> MarmotResult::TypeResult
								{
									std::string associated_type_qualifier = ExtractQualifier(type_name.m_lexeme);
									if (associated_type_qualifier.empty())
									{
										return std::unexpected(GenerateParserError("Undefined struct or union.", type_name));
									}

									std::string associated_type_name = ExtractSymbolName(type_name.m_lexeme);
									TypeclassAssociatedTypeMap::const_iterator assoc_it = m_state.m_typeclass_associated_types.find(associated_type_qualifier);
									if (assoc_it == m_state.m_typeclass_associated_types.cend()
										|| !std::ranges::contains(assoc_it->second, associated_type_name))
									{
										return std::unexpected(GenerateParserError("Undefined struct or union.", type_name));
									}

									if (!Match(Token::Name::LEFT_ANGLE))
									{
										return std::unexpected(GenerateParserError("Expected '<' after associated type name.", type_name));
									}

									MarmotResult::TypeListResult type_args_result = ParseDelimitedZeroOrMoreLimited<std::shared_ptr<MarmotType>>
									(
										[this]() { return ParseType(); },
										[this]() { return Consume(Token::Name::COMMA, "Expected ',' after associated type argument."); },
										[this]() { return ConsumeTypeRightAngle("Expected '>' after associated type arguments."); }
									);
									if (!type_args_result.has_value())
									{
										return std::unexpected(type_args_result.error());
									}

									const size_t expected_arg_count = m_state.m_typeclass_type_params.contains(associated_type_qualifier)
										? m_state.m_typeclass_type_params.at(associated_type_qualifier).size()
										: 0u;
									std::vector<std::shared_ptr<MarmotType>> type_args = std::move(type_args_result.value());
									if (type_args.size() != expected_arg_count)
									{
										return std::unexpected
										(
											GenerateParserError
											(
												"Associated type argument count mismatch: expected " + std::to_string(expected_arg_count) +
												", got " + std::to_string(type_args.size()),
												type_name
											)
										);
									}

									return MarmotType::MakeAssociatedType(associated_type_qualifier, associated_type_name, std::move(type_args));
								};

								if (found_scope_it != m_state.m_scopes.crend())
								{
									base_type = found_scope_it->m_defined_types.at(type_name.m_lexeme);

									Scope::AliasGenericParamTable::const_iterator alias_params_it = found_scope_it->m_alias_generic_params.find(type_name.m_lexeme);
									if (alias_params_it != found_scope_it->m_alias_generic_params.cend())
									{
										alias_generic_params = std::addressof(alias_params_it->second);
									}
								}
								else
								{
									const UseImportResolution use_import_resolution = ResolveUseImport(type_name.m_lexeme);
									if (use_import_resolution.m_status == UseImportResolutionStatus::Ambiguous)
									{
										return std::unexpected(GenerateParserError(BuildAmbiguousUseImportError(type_name.m_lexeme, use_import_resolution.m_conflicting_modules), type_name));
									}

									if (use_import_resolution.m_status == UseImportResolutionStatus::Resolved)
									{
										std::unordered_map<std::string, TypeEnvironment>::const_iterator module_it = m_context.m_imports.TypesByModule().find(use_import_resolution.m_module_name);
										if (module_it != m_context.m_imports.TypesByModule().cend())
										{
											TypeEnvironment::const_iterator type_it = module_it->second.find(type_name.m_lexeme);
											if (type_it != module_it->second.cend())
											{
												base_type = type_it->second;
											}
										}
									}

									if (base_type == nullptr)
									{
										std::string associated_type_qualifier = ExtractQualifier(type_name.m_lexeme);
										if (!associated_type_qualifier.empty())
										{
											std::string associated_type_name = ExtractSymbolName(type_name.m_lexeme);
											TypeclassAssociatedTypeMap::const_iterator assoc_it = m_state.m_typeclass_associated_types.find(associated_type_qualifier);
											if (assoc_it != m_state.m_typeclass_associated_types.cend()
												&& std::ranges::contains(assoc_it->second, associated_type_name)
												&& Check(Token::Name::LEFT_ANGLE, 0))
											{
												return try_parse_associated_type();
											}
										}
									}

									if (base_type == nullptr)
									{
										std::string qualifier = ExtractQualifier(type_name.m_lexeme);
										if (!qualifier.empty())
										{
											std::string symbol_name = ExtractSymbolName(type_name.m_lexeme);
											std::unordered_map<std::string, TypeEnvironment>::const_iterator module_it = m_context.m_imports.TypesByModule().find(qualifier);
											if (module_it != m_context.m_imports.TypesByModule().cend())
											{
												TypeEnvironment::const_iterator type_it = module_it->second.find(symbol_name);
												if (type_it != module_it->second.cend())
												{
													base_type = type_it->second;
												}
												else
												{
													if (m_context.m_imports.ContainsModule(qualifier))
													{
														return std::unexpected(GenerateParserError("Type '" + symbol_name + "' is not exported by module '" + qualifier + "'.", type_name));
													}
												}
											}
										}
									}

									if (base_type == nullptr)
									{
										std::string associated_type_qualifier = ExtractQualifier(type_name.m_lexeme);
										if (!associated_type_qualifier.empty())
										{
											std::string associated_type_name = ExtractSymbolName(type_name.m_lexeme);
											TypeclassAssociatedTypeMap::const_iterator assoc_it = m_state.m_typeclass_associated_types.find(associated_type_qualifier);
											if (assoc_it != m_state.m_typeclass_associated_types.cend() && std::ranges::contains(assoc_it->second, associated_type_name))
											{
												return try_parse_associated_type();
											}
										}

										const std::vector<std::string> exporting_modules = ModulesExporting(type_name.m_lexeme);
										if (!exporting_modules.empty())
										{
											return std::unexpected(GenerateParserError(BuildUnimportedSymbolError(type_name.m_lexeme, exporting_modules), type_name));
										}

										if (m_state.m_allow_implicit_generic_params && type_name.m_lexeme.find(NameSeparator) == std::string::npos)
										{
											std::shared_ptr<MarmotType> generic_type = MarmotType::MakeGenericType(type_name.m_lexeme);
											m_state.m_scopes.back().m_defined_types[type_name.m_lexeme] = generic_type;
											base_type = generic_type;
										}
										else
										{
											return std::unexpected(GenerateParserError("Undefined struct or union.", type_name));
										}
									}
								}

								if (Match(Token::Name::LEFT_ANGLE))
								{
									MarmotResult::TypeListResult type_args_result = ParseDelimitedZeroOrMoreLimited<std::shared_ptr<MarmotType>>
										(
											[this]() { return ParseType(); },
											[this]() { return Consume(Token::Name::COMMA, "Expected ',' after type argument."); },
											[this]() { return ConsumeTypeRightAngle("Expected '>' after type arguments."); }
										);

									if (!type_args_result.has_value())
									{
										return std::unexpected(type_args_result.error());
									}

									std::vector<std::shared_ptr<MarmotType>> type_args = std::move(type_args_result.value());

									// An alias binds its own parameters, and its expansion no longer
									// carries any, so the alias is asked first and the expansion
									// only when the name is not one.
									std::vector<std::string> generic_params;
									if (alias_generic_params != nullptr)
									{
										generic_params = *alias_generic_params;
									}
									else if (base_type->IsType<MarmotType::StructType>())
									{
										generic_params = base_type->GetType<MarmotType::StructType>().m_generic_params;
									}
									else if (base_type->IsType<MarmotType::UnionType>())
									{
										generic_params = base_type->GetType<MarmotType::UnionType>().m_generic_params;
									}
									else if (base_type->IsType<MarmotType::NewType>())
									{
										// A newtype is not transparent like an alias: it carries its own
										// m_generic_params (Task 2's substitution arm deliberately
										// preserves them), spelled with the newtype's own parameter
										// names, so the base type can be read directly here exactly as
										// for struct and union. No alias_generic_params side table is
										// needed because there is no separate "expansion" whose params
										// could differ from the declaration's own.
										generic_params = base_type->GetType<MarmotType::NewType>().m_generic_params;
									}

									if (type_args.size() != generic_params.size())
									{
										return std::unexpected(GenerateParserError(
											BuildTypeArgumentCountMismatchMessage(type_name.m_lexeme, alias_generic_params != nullptr, generic_params.size(), type_args.size()), type_name));
									}

									if (ReachesUnfinishedType(base_type))
									{
										return InstantiateUnfinishedType(type_name, base_type, generic_params, std::move(type_args));
									}

									std::unordered_map<std::string, std::shared_ptr<MarmotType>> substitutions;
									for (size_t i = 0; i < generic_params.size(); ++i)
									{
										substitutions[generic_params[i]] = type_args[i];
									}

									return MarmotType::SubstituteTypeParams(base_type, substitutions);
								}

								// Without arguments a parameterised alias would hand back a template
								// whose parameters nothing binds.
								if (alias_generic_params != nullptr)
								{
									return std::unexpected(GenerateParserError(
										BuildTypeArgumentCountMismatchMessage(type_name.m_lexeme, true, alias_generic_params->size(), 0u), type_name));
								}

								return base_type;
							}
						);
				}
			);
		},
		[this]() -> MarmotResult::TypeResult
		{
			return std::unexpected(GenerateParserError("Expected type token.", Peek(0)));
		}
	);
}

MarmotResult::StatementResult Parser::ParseDeclaration()
{
	// End of current block: no further declarations to parse here.
	if (Check(Token::Name::RIGHT_BRACE, 0))
	{
		return NoMatch<std::unique_ptr<MarmotStatement>>();
	}

	return ParseChoice<std::unique_ptr<MarmotStatement>>(m_state,
		[this]() -> MarmotResult::StatementResult
		{
			return ParseWhen<std::unique_ptr<MarmotStatement>>(m_state,
				Token::Name::DEF,
				[this](Token&&) -> MarmotResult::StatementResult
				{
					return ParseDefineStatement();
				}
			);
		},
		[this]() -> MarmotResult::StatementResult
		{
			return ParseWhen<std::unique_ptr<MarmotStatement>>(m_state,
				Token::Name::CLASS,
				[this](Token&&) -> MarmotResult::StatementResult
				{
					return ParseClassDeclaration();
				}
			);
		},
		[this]() -> MarmotResult::StatementResult
		{
			return ParseWhen<std::unique_ptr<MarmotStatement>>(m_state,
				Token::Name::INSTANCE,
				[this](Token&&) -> MarmotResult::StatementResult
				{
					return ParseInstanceDeclaration();
				}
			);
		},
		[this]() -> MarmotResult::StatementResult
		{
			return ParseWhen<std::unique_ptr<MarmotStatement>>(m_state,
				Token::Name::FOREIGN,
				[this](Token&&) -> MarmotResult::StatementResult
				{
					return ParseForeignStatement();
				}
			);
		},
		[this]() -> MarmotResult::StatementResult
		{
			return ParseWhen<std::unique_ptr<MarmotStatement>>(m_state,
				Token::Name::TYPE,
				[this](Token&&) -> MarmotResult::StatementResult
				{
					return ParseTypeDeclaration();
				}
			);
		},
		[this]() -> MarmotResult::StatementResult
		{
			return ParseWhen<std::unique_ptr<MarmotStatement>>(m_state,
				Token::Name::ALIAS,
				[this](Token&&) -> MarmotResult::StatementResult
				{
					return ParseAliasDeclaration();
				}
			);
		},
		[this]() -> MarmotResult::StatementResult
		{
			return ParseStatement();
		}
	);
}

MarmotResult::ParserResult Parser::Parse()
{
	MarmotProgramTree programTree;
	CollectTopLevelNames();

	while (!IsAtEnd() || !m_pending_statements.empty())
	{
		if (!m_pending_statements.empty())
		{
			programTree.emplace_back(std::move(m_pending_statements.front()));
			m_pending_statements.pop();
			continue;
		}

		m_state.m_statement_index = static_cast<int>(programTree.size());
		MarmotResult::StatementResult result = ParseDeclaration();
		if (result.has_value())
		{
			RecordTopLevelDefinition(*result.value(), m_state.m_statement_index);
			programTree.emplace_back(std::move(result.value()));
		}
		else
		{
			// Phase 2 recovery policy: Parse() is the single recovery boundary for hard parser errors.
			// The parser still stops at the first hard error after synchronizing to the next declaration starter.
			Synchronize();
			return std::unexpected(MarmotResult::CompilerDiagnostics(std::move(result.error())));
		}
	}

	if (std::optional<CompilerError> instantiation_error = FillDeferredInstantiations())
	{
		return std::unexpected(MarmotResult::CompilerDiagnostics(std::move(instantiation_error.value())));
	}

	if (std::optional<CompilerError> order_error = CheckDefinitionOrder())
	{
		return std::unexpected(MarmotResult::CompilerDiagnostics(std::move(order_error.value())));
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
			return std::unexpected(GenerateParserError(std::format("Expected identifier after '{}'.", NameSeparator), Previous()));
		}

		resolved_name_str.append(NameSeparator).append(Previous().m_lexeme);
	}

	return resolved_name;
}

MarmotResult::TokenListResult Parser::ParseGenericParameters(std::vector<std::shared_ptr<MarmotType>>* out_types)
{
	return ParseDelimitedZeroOrMoreLimited<Token>
		(
			[this, out_types]() -> MarmotResult::TokenResult
			{
				return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected generic parameter name.")
					.and_then
					(
						[this, out_types](Token&& param_name) -> MarmotResult::TokenResult
						{
							constexpr bool is_variable = false;
							return DefineName(param_name, is_variable)
								.and_then
								(
									[this, out_types](Token&& param_name) -> MarmotResult::TokenResult
									{
										std::shared_ptr<MarmotType> param_type = MarmotType::MakeGenericType(param_name.m_lexeme);
										m_state.m_scopes.back().m_defined_types[param_name.m_lexeme] = param_type;

										// Keep the type alive if requested
										if (out_types != nullptr)
										{
											out_types->push_back(param_type);
										}

										return param_name;
									}
								);
						}
					);
			},
			[this]() { return Consume(Token::Name::COMMA, "Expected ',' between generic parameters."); },
			[this]() { return ConsumeTypeRightAngle("Expected '>' after generic parameters."); }
		);
}

void Parser::PushActiveConstraints(const std::vector<MarmotType::ClassConstraint>& constraints)
{
	for (const MarmotType::ClassConstraint& constraint : constraints)
	{
		if (!ContainsConstraint(m_state.m_active_constraints, constraint))
		{
			m_state.m_active_constraints.push_back(constraint);
		}
	}
}

std::expected<std::vector<MarmotType::ClassConstraint>, CompilerError> Parser::ParseClassConstraints(const Token& context_token)
{
	// An equality constraint reads 'Class::Assoc<S> ~ Type'. The '::' one token
	// past the leading identifier is what distinguishes it from a class
	// constraint, which reads 'Class<Type,...>'.
	std::function<std::expected<MarmotType::ClassConstraint, CompilerError>()> parse_equality_constraint = [this]() -> std::expected<MarmotType::ClassConstraint, CompilerError>
		{
			Token projection_token = Peek(0);
			return ParseType()
				.and_then
				(
					[&projection_token, this](std::shared_ptr<MarmotType>&& equality_lhs) -> std::expected<MarmotType::ClassConstraint, CompilerError>
					{
						if (!equality_lhs->IsType<MarmotType::AssociatedType>())
						{
							return std::unexpected(GenerateParserError("The left side of a '~' constraint must be an associated type projection, such as 'Stepper::Item<S>'.", projection_token));
						}

						return Consume(Token::Name::TILDE, "Expected '~' after the associated type projection in an equality constraint.")
							.and_then
							(
								[&equality_lhs, this](Token&&) -> std::expected<MarmotType::ClassConstraint, CompilerError>
								{
									return ParseType()
										.and_then
										(
											[&equality_lhs](std::shared_ptr<MarmotType>&& equality_rhs) -> std::expected<MarmotType::ClassConstraint, CompilerError>
											{
												return MarmotType::ClassConstraint{ std::move(equality_lhs), std::move(equality_rhs) };
											}
										);
								}
							);
					}
				);
		};

	std::function<std::expected<MarmotType::ClassConstraint, CompilerError>()> parse_constraint = [&parse_equality_constraint, this]() -> std::expected<MarmotType::ClassConstraint, CompilerError>
		{
			// A leading '~' after the identifier routes here too, so that 'T ~ Int'
			// is rejected by the rule it actually breaks rather than by a missing '<'.
			if (Check(Token::Name::IDENTIFIER_LITERAL, 0) && (Check(Token::Name::DOUBLE_COLON, 1) || Check(Token::Name::TILDE, 1)))
			{
				return parse_equality_constraint();
			}

			return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected class name in constraint.")
				.and_then
				(
					[this](Token&& first_token) -> std::expected<MarmotType::ClassConstraint, CompilerError>
					{
						Token typeclass_name = std::move(first_token);
						return Consume(Token::Name::LEFT_ANGLE, "Expected '<' after class name in constraint (e.g., 'Show<T>').")
							.and_then
							(
								[&typeclass_name, this](Token&&) -> std::expected<MarmotType::ClassConstraint, CompilerError>
								{
									return ParseDelimitedZeroOrMoreLimited<std::shared_ptr<MarmotType>>
										(
											[this]() { return ParseType(); },
											[this]() { return Consume(Token::Name::COMMA, "Expected ',' between type arguments."); },
											[this]() { return ConsumeTypeRightAngle("Expected '>' after type arguments."); }
										)
										.and_then
										(
											[&typeclass_name](std::vector<std::shared_ptr<MarmotType>>&& type_args) -> std::expected<MarmotType::ClassConstraint, CompilerError>
											{
												return MarmotType::ClassConstraint{ typeclass_name.m_lexeme, std::move(type_args) };
											}
										);
								}
							);
					}
				);
		};

	std::expected<std::vector<MarmotType::ClassConstraint>, CompilerError> constraints_result = ParseDelimitedOneOrMoreUnlimited<MarmotType::ClassConstraint>
	(
		parse_constraint,
		[this]() { return Consume(Token::Name::COMMA, "Expected ',' between constraints."); }
	);

	if (!constraints_result.has_value())
	{
		return std::unexpected(std::move(constraints_result.error()));
	}

	return std::move(constraints_result.value());
}

std::expected<std::vector<Token>, CompilerError> Parser::ParseDerivingTargets(const Token& context_token)
{
	return Consume(Token::Name::LEFT_PAREN, "Expected '(' after 'deriving'.")
		.and_then
		(
			[&context_token, this](Token&&) -> std::expected<std::vector<Token>, CompilerError>
			{
				std::unordered_set<std::string> seen_targets;
				return ParseDelimitedZeroOrMoreLimited<Token>
					(
						[&seen_targets, this]() -> MarmotResult::TokenResult
						{
							return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected deriving target.")
								.and_then
								(
									[&seen_targets, this](Token&& target) -> MarmotResult::TokenResult
									{
										if (!seen_targets.insert(target.m_lexeme).second)
										{
											return std::unexpected(GenerateParserError("Duplicate deriving target.", target));
										}

										return target;
									}
								);
						},
						[this]() { return Consume(Token::Name::COMMA, "Expected ',' after deriving target."); },
						[this]() { return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after deriving targets."); }
					)
					.and_then
					(
						[&context_token, this](std::vector<Token>&& targets) -> std::expected<std::vector<Token>, CompilerError>
						{
							if (targets.empty())
							{
								return std::unexpected(GenerateParserError("Expected at least one deriving target.", context_token));
							}

							return std::move(targets);
						}
					);
			}
		);
}

void Parser::RegisterSyntheticInstanceMetadata(const std::string& class_name, const std::vector<std::shared_ptr<MarmotType>>& type_args, const std::vector<MarmotType::ClassConstraint>& constraints, const std::vector<std::string>& mangled_method_names)
{
	const std::string declaring_module = m_context.m_current_module && m_context.m_current_module->HasModuleDeclaration() ? m_context.m_current_module->ModuleName() : std::string();
	if (m_state.m_class_methods.contains(class_name))
	{
		m_state.m_class_instances[class_name].emplace_back(type_args, TypeclassInstance::AssociatedTypes(), constraints, mangled_method_names, declaring_module);
	}
}

MarmotResult::FunctionParamsResult Parser::ParseFunctionParameters(bool allow_inferred_types)
{
	return ParseDelimitedZeroOrMoreLimited<std::pair<Token, std::shared_ptr<MarmotType>>>
		(
			[this, allow_inferred_types]() -> MarmotResult::FunctionParamResult
			{
				return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected parameter name.")
					.and_then
					(
						[this, allow_inferred_types](Token&& param_name) -> MarmotResult::FunctionParamResult
						{
							return DefineName(param_name, true)
								.and_then
								(
									[this, allow_inferred_types](Token&& param_name) -> MarmotResult::FunctionParamResult
									{
										auto finish_param = [&param_name, this](std::shared_ptr<MarmotType>&& type) -> MarmotResult::FunctionParamResult
										{
											RegisterOrUpdateLocalVariable(param_name.m_lexeme);
											return std::make_pair(std::move(param_name), std::move(type));
										};

										if (Match(Token::Name::SINGLE_COLON))
										{
											return ParseType()
												.and_then
												(
													[&finish_param](std::shared_ptr<MarmotType>&& type) -> MarmotResult::FunctionParamResult
													{
														return finish_param(std::move(type));
													}
												);
										}

										if (!allow_inferred_types)
										{
											return std::unexpected(GenerateParserError("Expected ':' after parameter name.", param_name));
										}

										return finish_param(std::shared_ptr<MarmotType>(MarmotType::MakeUndecidedType()));
									}
								);
						}
					);
			},
			[this]() { return Consume(Token::Name::COMMA, "Expected ',' after function parameter."); },
			[this]() { return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after function parameters."); }
		);
}

template<typename Declaration>
std::expected<void, CompilerError> Parser::QueueDerivedStatements(const Declaration& declaration, const std::vector<Token>& deriving_targets)
{
	if (!IsAtGlobalScope())
	{
		return std::unexpected(GenerateParserError("Deriving declarations are only supported at global scope.", declaration.m_name));
	}
	for (const Token& target : deriving_targets)
	{
		std::expected<DerivedDeclarationGenerator, DerivedDeclarationError> derivation = DerivedDeclarationGenerator::Prepare(declaration, target);
		if (!derivation.has_value())
		{
			return std::unexpected(GenerateParserError(std::move(derivation.error().m_message), derivation.error().m_token));
		}
		if (Token* name = derivation->NameToReserve())
		{
			MarmotResult::TokenResult registered = DefineName(*name, true);
			if (!registered.has_value())
			{
				return std::unexpected(std::move(registered.error()));
			}
		}
		std::expected<std::unique_ptr<MarmotStatement>, DerivedDeclarationError> generated = derivation->Generate();
		if (!generated.has_value())
		{
			return std::unexpected(GenerateParserError(std::move(generated.error().m_message), generated.error().m_token));
		}
		if (generated.value()->IsStatement<MarmotStatement::Instance>())
		{
			const MarmotStatement::Instance& instance = generated.value()->GetStatement<MarmotStatement::Instance>();
			std::vector<std::string> methods = instance.m_methods
				| std::views::transform([](const MarmotStatement::Instance::Method& method) { return method.m_definition.m_name.m_lexeme; })
				| std::ranges::to<std::vector<std::string>>();
			RegisterSyntheticInstanceMetadata(instance.m_class_name.m_lexeme, instance.m_type_args, instance.m_constraints, methods);
		}
		m_pending_statements.push(std::move(generated.value()));
	}
	return {};
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

Parser::VariableContext::VariableContext(int relative_index, int absolute_index, int function_depth)
	: m_relative_index(relative_index),
	m_absolute_index(absolute_index),
	m_function_depth(function_depth)
{
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

MarmotResult::TypeResult Parser::ParseRefTypeArguments()
{
	return Consume(Token::Name::LEFT_ANGLE, "Expected '<' after 'Ref'.")
		.and_then
		(
			[this](Token&&) -> MarmotResult::TypeResult
			{
				return ParseType()
					.and_then
					(
						[this](std::shared_ptr<MarmotType>&& element_type) -> MarmotResult::TypeResult
						{
							return ConsumeTypeRightAngle("Expected '>' after reference element type.")
								.and_then
								(
									[&element_type](Token&&) -> MarmotResult::TypeResult
									{
										return MarmotType::MakeCellType(element_type);
									}
								);
						}
					);
			}
		);
}
