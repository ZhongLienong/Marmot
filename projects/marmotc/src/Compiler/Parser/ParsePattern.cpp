#include "Parser.h"
#include "Compiler/Names/SpecialNames.h"
#include <algorithm>
#include <format>
#include <limits>
#include <ranges>

using namespace std::string_literals;

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

MarmotResult::PatternResult Parser::ParsePattern()
{
	return FoldWhile
	(
		ParseAlternatives(),
		[this]() { return Match(Token::Name::AS); },
		[this](std::unique_ptr<MarmotPattern>&& pattern) -> MarmotResult::PatternResult
		{
			const Token as = Previous();
			if (!Match(Token::Name::IDENTIFIER_LITERAL) || Previous().IsDiscard())
			{
				return std::unexpected(GenerateParserError("Expected a name after 'as' in a pattern.", as));
			}

			return ParseBindingPattern(Token(Previous())).transform([&](std::unique_ptr<MarmotPattern>&& binding)
			{
				return std::make_unique<MarmotPattern>(MarmotPattern::As(std::move(pattern), std::move(binding)));
			});
		}
	);
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
	const std::unordered_map<std::string, std::optional<BindingId>> locals = alternatives.front()->Bindings(false)
		| std::views::transform([](const MarmotPattern::Binding* binding) { return std::pair{ binding->m_name.m_lexeme, binding->m_binding }; })
		| std::ranges::to<std::unordered_map>();

	while (Match(Token::Name::SINGLE_BAR))
	{
		const Token bar = Previous();
		MarmotResult::PatternResult alternative = InScope([this]() { return ParsePrimaryPattern(); });
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
			binding->m_binding = locals.at(binding->m_name.m_lexeme);
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
		if (identifier.IsDiscard())
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

						return std::unexpected(GenerateParserError("Constructor patterns require parentheses; use Name() for a constructor with no fields.", identifier));
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
		return std::make_unique<MarmotPattern>(MarmotPattern::Literal(literal, MarmotPattern::LiteralKind::Integer));
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
				std::optional<BindingId> binding = RegisterLocalBinding(defined_name.m_lexeme);
				return std::make_unique<MarmotPattern>(MarmotPattern::Binding(defined_name, std::move(binding)));
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
	if (name.IsDiscard())
	{
		return std::make_unique<MarmotPattern>(MarmotPattern::Wildcard(name));
	}
	return ParseBindingPattern(std::move(name));
}
