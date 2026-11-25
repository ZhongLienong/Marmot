#include "Parser.h"
#include "Bytecode/Format/Format.h"
#include "Bytecode/Intrinsics/IntrinsicTable.h"
#include "Compiler/Names/SpecialNames.h"
#include "Support/Expected.h"
#include "Support/Optional.h"
#include <algorithm>
#include <format>
#include <ranges>

using namespace std::string_literals;

namespace
{
	using StructMemberTuple = std::tuple<std::shared_ptr<MarmotType>, std::string>;
	using UnionMemberTuple = std::tuple<std::string, std::vector<std::shared_ptr<MarmotType>>, int>;
	using UnionMemberInfo = std::unordered_map<std::string, MarmotType::UnionType::UnionMemberContext>;

	struct StructMemberSplit
	{
		std::vector<std::shared_ptr<MarmotType>> m_types;
		std::vector<std::string> m_names;
	};

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
}

Parser::ParsedTypeDeclaration::ParsedTypeDeclaration(std::unique_ptr<MarmotStatement> statement, std::vector<Token> deriving_targets)
	: m_statement(std::move(statement)),
	m_deriving_targets(std::move(deriving_targets))
{
}

MarmotResult::StatementResult Parser::ParseDefineStatement()
{
	if (Match(Token::Name::LEFT_PAREN))
	{
		std::vector<Token> names;
		std::vector<MarmotCompiler::Optional<BindingId>> bindings;

		do
		{
			MarmotResult::TokenResult name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected identifier in tuple pattern.");
			if (!name_result)
			{
				return MarmotCompiler::Failure(name_result.Error());
			}

			Token var_name = std::move(name_result.Value());
			MarmotResult::TokenResult defined_name_result = DefineBinding(var_name);
			if (!defined_name_result)
			{
				return MarmotCompiler::Failure(defined_name_result.Error());
			}

			Token defined_name = std::move(defined_name_result.Value());
			MarmotCompiler::Optional<BindingId> binding = RegisterLocalBinding(defined_name.m_lexeme);

			names.emplace_back(std::move(defined_name));
			bindings.emplace_back(std::move(binding));

		} while (Match(Token::Name::COMMA));

		return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after tuple pattern.")
			.AndThen
			(
				[&names, &bindings, this](Token&&) -> MarmotResult::StatementResult
				{
					return Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after tuple pattern.")
						.AndThen
						(
							[&names, &bindings, this](Token&&) -> MarmotResult::StatementResult
							{
								return InContext
								(
									[&]() -> MarmotResult::StatementResult
									{
										for (const Token& name : names)
										{
											if (!name.IsDiscard())
											{
												m_state.m_pending_definitions.emplace_back(name.m_lexeme, m_state.m_function);
											}
										}

										return ParseExpression()
											.AndThen
											(
												[&names, &bindings, this](std::unique_ptr<MarmotExpression>&& expr) -> MarmotResult::StatementResult
												{
													return Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after tuple binding.")
														.AndThen
														(
															[&names, &bindings, &expr](Token&&) -> MarmotResult::StatementResult
															{
																return std::make_unique<MarmotStatement>(MarmotStatement::TupleDefinition(std::move(names), std::move(expr), std::move(bindings)));
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

	// Single variable: def x = ...
	return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected name.")
		.AndThen
		(
			[this](Token&& var_name) -> MarmotResult::StatementResult
			{
				return DefineBinding(var_name)
					.AndThen
					(
						[this](Token&& define_name) -> MarmotResult::StatementResult
						{
							std::function<MarmotResult::StatementResult(MarmotCompiler::Optional<std::shared_ptr<MarmotType>>)> def_aux_func = [&define_name, this](MarmotCompiler::Optional<std::shared_ptr<MarmotType>> type_annotation)
								{
									MarmotCompiler::Optional<BindingId> binding = RegisterLocalBinding(define_name.m_lexeme);

									return Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after defining a name.")
										.AndThen
										(
											[&define_name, &type_annotation, &binding, this](Token&&) -> MarmotResult::StatementResult
											{
												return InContext
												(
													[&]() -> MarmotResult::StatementResult
													{
														if (!define_name.IsDiscard())
														{
															m_state.m_pending_definitions.emplace_back(define_name.m_lexeme, m_state.m_function);
														}
														return ParseExpression()
															.AndThen
															(
																[&define_name, &type_annotation, &binding, this](std::unique_ptr<MarmotExpression>&& expr) -> MarmotResult::StatementResult
																{
																	return Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after name binding.")
																		.AndThen
																		(
																			[&define_name, &expr, &type_annotation, &binding](Token&&) -> MarmotResult::StatementResult
																			{
																				return std::make_unique<MarmotStatement>(MarmotStatement::VariableDefinition(define_name, std::move(expr), std::move(type_annotation), std::move(binding)));
																			}
																		);
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
									.AndThen
									(
										[&def_aux_func](std::shared_ptr<MarmotType>&& type)->MarmotResult::StatementResult
										{
											return def_aux_func(std::move(type));
										}
									);
							}
							else
							{
								return def_aux_func(MarmotCompiler::None);
							}
						}
					);
			}
		);
}

Parser::ParsedTypeResult Parser::WithTypeDeclarationHeader(std::string_view noun, std::string_view capitalized_noun, const TypeBodyParser& parse_body)
{
	MarmotResult::TokenResult name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected "s + std::string(noun) + " name."s);
	if (!name_result.HasValue())
	{
		return MarmotCompiler::Failure(name_result.Error());
	}

	Token declaration_name = std::move(name_result.Value());
	std::string name_before_mangle = declaration_name.m_lexeme;
	declaration_name.m_lexeme = Mangle(declaration_name.m_lexeme);

	if (declaration_name.m_lexeme[0u] != std::toupper(declaration_name.m_lexeme[0u]))
	{
		return MarmotCompiler::Failure(GenerateParserError(std::string(capitalized_noun) + " name must start with a capital letter."s, declaration_name));
	}

	constexpr bool is_variable = false;
	MarmotResult::TokenResult defined_name_result = DefineName(declaration_name, is_variable);
	if (!defined_name_result.HasValue())
	{
		return MarmotCompiler::Failure(defined_name_result.Error());
	}

	TypeDeclarationHeader header(std::move(defined_name_result.Value()), std::move(name_before_mangle), m_state.m_scopes.size() - 1u);
	return WithGenericParameters
	(
		[&](std::vector<Token> parameters, std::vector<std::shared_ptr<MarmotType>> types) -> ParsedTypeResult
		{
			header.m_generic_params = std::move(parameters);
			header.m_generic_param_types = std::move(types);
			header.m_has_generic_params = !header.m_generic_params.empty();

			if (Match(Token::Name::WHERE))
			{
				if (!header.m_has_generic_params)
				{
					return MarmotCompiler::Failure(GenerateParserError(std::string(capitalized_noun) + " constraints require at least one type parameter."s, header.m_name));
				}

				ClassConstraintListResult constraints_result = ParseClassConstraints();
				if (!constraints_result.HasValue())
				{
					return MarmotCompiler::Failure(constraints_result.Error());
				}

				header.m_constraints = std::move(constraints_result.Value());
			}

			return parse_body(std::move(header));
		}
	);
}

Parser::ParsedTypeResult Parser::ParseStructBody(TypeDeclarationHeader&& header)
{
	MarmotResult::TokenResult brace_result = Consume(Token::Name::LEFT_BRACE, "Expected '{' before struct body.");
	if (!brace_result.HasValue())
	{
		return MarmotCompiler::Failure(brace_result.Error());
	}

	std::vector<std::string> generic_param_names;
	std::ranges::transform(header.m_generic_params, std::back_inserter(generic_param_names), [](const Token& tok) { return tok.m_lexeme; });

	// Registered before the fields are parsed, as a union is before its variants,
	// so a field can name the record it belongs to.
	std::shared_ptr<MarmotType> struct_type = DeclaredTypeFor(header, MarmotType::MakeStructType(header.m_name.m_lexeme, CurrentModuleName(), {}, {}, std::move(generic_param_names)));
	struct_type->GetType<MarmotType::StructType>().m_constraints = header.m_constraints;
	m_state.m_scopes[header.m_scope_index].m_defined_types[header.m_name.m_lexeme] = struct_type;

	MarmotResult::Result<std::vector<StructMemberTuple>> members_result = InContext
	(
		[this, &struct_type]()
		{
			m_state.m_types_being_declared.push_back(struct_type);
			return ParseDelimitedZeroOrMoreLimited<StructMemberTuple>
			(
				[this]() -> MarmotResult::Result<StructMemberTuple>
				{
					MarmotResult::TokenResult member_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected struct member name.");
					if (!member_name_result.HasValue())
					{
						return MarmotCompiler::Failure(member_name_result.Error());
					}

					Token member_name = std::move(member_name_result.Value());

					MarmotResult::TokenResult colon_result = Consume(Token::Name::SINGLE_COLON, "Expected ':' before struct member type token.");
					if (!colon_result.HasValue())
					{
						return MarmotCompiler::Failure(colon_result.Error());
					}

					MarmotResult::TypeResult member_type_result = ParseType();
					if (!member_type_result.HasValue())
					{
						return MarmotCompiler::Failure(member_type_result.Error());
					}

					return std::make_tuple(std::move(member_type_result.Value()), member_name.m_lexeme);
				},
				[this]() { return Consume(Token::Name::COMMA, "Expected ',' struct member."); },
				[this]() { return Consume(Token::Name::RIGHT_BRACE, "Expected '}' struct members."); }
			);
		}
	);
	if (!members_result.HasValue())
	{
		return MarmotCompiler::Failure(members_result.Error());
	}

	std::vector<Token> deriving_targets;
	if (Match(Token::Name::DERIVING))
	{
		MarmotResult::TokenListResult deriving_result = ParseDerivingTargets(header.m_name);
		if (!deriving_result.HasValue())
		{
			return MarmotCompiler::Failure(std::move(deriving_result.Error()));
		}

		deriving_targets = std::move(deriving_result.Value());
	}

	MarmotResult::TokenResult semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after struct body.");
	if (!semicolon_result.HasValue())
	{
		return MarmotCompiler::Failure(semicolon_result.Error());
	}

	StructMemberSplit member_split = SplitStructMemberTuples(std::move(members_result.Value()));
	struct_type->GetType<MarmotType::StructType>().m_member_types = std::move(member_split.m_types);
	struct_type->GetType<MarmotType::StructType>().m_member_names = std::move(member_split.m_names);
	CompleteType(struct_type);

	m_state.m_scopes[header.m_scope_index].m_struct_constructors[header.m_name.m_lexeme] = struct_type;

	return ParsedTypeDeclaration(std::make_unique<MarmotStatement>(MarmotStatement::Struct(std::move(header.m_name), std::move(header.m_generic_params), std::move(header.m_constraints), std::move(struct_type))), std::move(deriving_targets));
}

Parser::ParsedTypeResult Parser::ParseUnionBody(TypeDeclarationHeader&& header)
{
	return InContext
	(
		[&]() -> ParsedTypeResult
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
			m_state.m_scopes[header.m_scope_index].m_defined_types[header.m_name.m_lexeme] = union_type;
			m_state.m_namespaces.emplace_back(header.m_name_before_mangle);

			std::vector<Token> constructor_names;
			int tag = 0;

			MarmotResult::Result<std::vector<UnionMemberTuple>> members_result = InContext
			(
				[&constructor_names, &tag, &union_type, this]()
				{
					m_state.m_types_being_declared.push_back(union_type);

					return ParseDelimitedOneOrMoreUnlimited<UnionMemberTuple>
					(
						[&constructor_names, &tag, this]() -> MarmotResult::Result<UnionMemberTuple>
						{
							MarmotResult::TokenResult member_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected union member name.");
							if (!member_name_result.HasValue())
							{
								return MarmotCompiler::Failure(member_name_result.Error());
							}

							Token member_name = std::move(member_name_result.Value());
							member_name.m_lexeme = Mangle(member_name.m_lexeme);

							constexpr bool is_variable = false;
							MarmotResult::TokenResult defined_member_result = DefineName(member_name, is_variable);
							if (!defined_member_result.HasValue())
							{
								return MarmotCompiler::Failure(defined_member_result.Error());
							}

							member_name = std::move(defined_member_result.Value());
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
								if (!member_types_result.HasValue())
								{
									return MarmotCompiler::Failure(member_types_result.Error());
								}

								member_types = std::move(member_types_result.Value());
							}

							UnionMemberTuple member = std::make_tuple(member_name.m_lexeme, std::move(member_types), tag);
							tag += 1;
							return member;
						},
						[this]() { return Consume(Token::Name::SINGLE_BAR, "Expected '|' after a union member."); }
					);
				}
			);

			if (!members_result.HasValue())
			{
				return MarmotCompiler::Failure(members_result.Error());
			}

			union_type_ref.m_member_info = BuildUnionMemberInfo(std::move(members_result.Value()));
			CompleteType(union_type);

			Scope& constructor_scope = m_state.m_scopes[header.m_scope_index];

			for (const std::pair<const std::string, MarmotType::UnionType::UnionMemberContext>& member_info_entry : union_type_ref.m_member_info)
			{
				constructor_scope.m_union_constructors[member_info_entry.first] = union_type;
			}

			std::vector<Token> deriving_targets;
			if (Match(Token::Name::DERIVING))
			{
				MarmotResult::TokenListResult deriving_result = ParseDerivingTargets(header.m_name);
				if (!deriving_result.HasValue())
				{
					return MarmotCompiler::Failure(std::move(deriving_result.Error()));
				}

				deriving_targets = std::move(deriving_result.Value());
			}

			MarmotResult::TokenResult semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after union body.");
			if (!semicolon_result.HasValue())
			{
				return MarmotCompiler::Failure(semicolon_result.Error());
			}

			return ParsedTypeDeclaration(std::make_unique<MarmotStatement>(MarmotStatement::Union(std::move(header.m_name), std::move(header.m_generic_params), std::move(constructor_names), std::move(header.m_constraints), std::move(union_type))), std::move(deriving_targets));
		}
	);
}

bool Parser::TypeBodyHasTopLevelBar()
{
	// Bounded scan from the current token to the depth-0 ';' that ends the
	// declaration, capped by MmcWire::MAX_ARRAY_SIZE and bailing out as soon as any
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

		if (offset > MmcWire::MAX_ARRAY_SIZE)
		{
			return false;
		}
	}

	return false;
}

Parser::ParsedTypeResult Parser::ParseNewTypeBody(TypeDeclarationHeader&& header)
{
	std::vector<std::string> generic_param_names;
	std::ranges::transform(header.m_generic_params, std::back_inserter(generic_param_names), [](const Token& generic_param) { return generic_param.m_lexeme; });

	std::shared_ptr<MarmotType> new_type = DeclaredTypeFor(header, MarmotType::MakeNewType(header.m_name.m_lexeme, CurrentModuleName(), MarmotType::MakeLiteralType<MarmotType::UnitType>(), std::move(generic_param_names)));
	new_type->GetType<MarmotType::NewType>().m_constraints = header.m_constraints;

	MarmotResult::TypeResult representation_result = InContext
	(
		[&new_type, this]()
			{
				m_state.m_types_being_declared.push_back(new_type);
				return ParseType();
			}
	);
	if (!representation_result.HasValue())
	{
		return MarmotCompiler::Failure(representation_result.Error());
	}

	new_type->GetType<MarmotType::NewType>().m_representation = std::move(representation_result.Value());
	CompleteType(new_type);

	// A newtype is its representation wherever it is erased, so one that
	// contains itself, directly or through another newtype, never ends.
	std::unordered_set<const MarmotType*> visited;
	if (RepresentationNames(new_type->GetType<MarmotType::NewType>().m_representation, new_type.get(), visited))
	{
		return MarmotCompiler::Failure(GenerateParserError(std::format("A newtype cannot contain itself: '{}' is represented by {}.", header.m_name.m_lexeme, new_type->GetType<MarmotType::NewType>().m_representation->DisplayString()), header.m_name));
	}

	MarmotResult::TokenResult semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after newtype definition.");
	if (!semicolon_result.HasValue())
	{
		return MarmotCompiler::Failure(semicolon_result.Error());
	}

	// Registered in the enclosing scope so uses of the name resolve to the
	// nominal type rather than to its representation.
	m_state.m_scopes[header.m_scope_index].m_defined_types[header.m_name.m_lexeme] = new_type;

	return ParsedTypeDeclaration(std::make_unique<MarmotStatement>(MarmotStatement::TypeAlias(std::move(header.m_name), std::move(header.m_generic_params), std::move(new_type))));
}

MarmotResult::StatementResult Parser::FinishTypeDeclaration(ParsedTypeDeclaration declaration)
{
	if (!declaration.m_deriving_targets.empty())
	{
		const MarmotResult::VoidResult generated = declaration.m_statement->IsStatement<MarmotStatement::Struct>()
			? QueueDerivedStatements(declaration.m_statement->GetStatement<MarmotStatement::Struct>(), declaration.m_deriving_targets)
			: QueueDerivedStatements(declaration.m_statement->GetStatement<MarmotStatement::Union>(), declaration.m_deriving_targets);
		if (!generated.HasValue())
		{
			return MarmotCompiler::Failure(std::move(generated.Error()));
		}
	}
	return std::move(declaration.m_statement);
}

MarmotResult::StatementResult Parser::ParseTypeDeclaration()
{
	const bool is_top_level = IsAtGlobalScope();
	return WithTypeDeclarationHeader
	(
		"type",
		"Type",
		[&](TypeDeclarationHeader&& header) -> ParsedTypeResult
		{
			MarmotResult::TokenResult equal_result = Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after type name.");
			if (!equal_result.HasValue())
			{
				return MarmotCompiler::Failure(equal_result.Error());
			}

			if (is_top_level)
			{
				header.m_predeclared_type = m_predeclared_types.at(header.m_name.m_lexeme);
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
				return ParseStructBody(std::move(header));
			}

			// A depth-0 '|' anywhere before the terminating ';' means a sum. Without one,
			// the right-hand side is a type expression and this declares a newtype. A
			// single-variant sum therefore needs an explicit leading bar, which is what
			// frees the bare-name spelling for newtypes.
			if (!TypeBodyHasTopLevelBar())
			{
				return ParseNewTypeBody(std::move(header));
			}

			Match(Token::Name::SINGLE_BAR);
			return ParseUnionBody(std::move(header));
		}
	).AndThen([this](ParsedTypeDeclaration&& declaration) { return FinishTypeDeclaration(std::move(declaration)); });
}

MarmotResult::StatementResult Parser::ParseAliasDeclaration()
{
	return WithTypeDeclarationHeader("alias", "Alias", [this](TypeDeclarationHeader&& header) { return ParseAliasBody(std::move(header)); })
		.AndThen([this](ParsedTypeDeclaration&& declaration) { return FinishTypeDeclaration(std::move(declaration)); });
}

Parser::ParsedTypeResult Parser::ParseAliasBody(TypeDeclarationHeader&& header)
{
	// An alias is transparent: it disappears into its expansion before anything can
	// discharge a constraint written on it. Rejecting is the only honest answer.
	if (!header.m_constraints.empty())
	{
		return MarmotCompiler::Failure(GenerateParserError("Alias declarations cannot carry 'where' constraints. Constrain the type the alias expands to instead.", header.m_name));
	}

	MarmotResult::TokenResult equal_result = Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after alias name.");
	if (!equal_result.HasValue())
	{
		return MarmotCompiler::Failure(equal_result.Error());
	}

	MarmotResult::TypeResult aliased_type_result = ParseType();
	if (!aliased_type_result.HasValue())
	{
		return MarmotCompiler::Failure(aliased_type_result.Error());
	}

	std::shared_ptr<MarmotType> aliased_type = std::move(aliased_type_result.Value());

	MarmotResult::TokenResult semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after alias definition.");
	if (!semicolon_result.HasValue())
	{
		return MarmotCompiler::Failure(semicolon_result.Error());
	}

	// aliased_type is already the template a use site needs: substituting Box's own
	// parameters with the alias's left every occurrence of `T` in it standing for the
	// alias's `T`, so applying the alias is one more substitution over the same body.
	// Only the parameter names and their order do not survive that rewrite, so those
	// are what the scope records.
	std::vector<std::string> generic_param_names;
	std::ranges::transform(header.m_generic_params, std::back_inserter(generic_param_names), [](const Token& generic_param) { return generic_param.m_lexeme; });

	m_state.m_scopes[header.m_scope_index].m_defined_types[header.m_name.m_lexeme] = aliased_type;
	if (!generic_param_names.empty())
	{
		m_state.m_scopes[header.m_scope_index].m_alias_generic_params[header.m_name.m_lexeme] = std::move(generic_param_names);
	}

	return ParsedTypeDeclaration(std::make_unique<MarmotStatement>(MarmotStatement::TypeAlias(std::move(header.m_name), std::move(header.m_generic_params), std::move(aliased_type))));
}

MarmotResult::StatementResult Parser::ParseClassDeclaration()
{
	MarmotResult::TokenResult class_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected class name.");
	if (!class_name_result.HasValue())
	{
		return MarmotCompiler::Failure(class_name_result.Error());
	}

	Token typeclass_name = std::move(class_name_result.Value());
	typeclass_name.m_lexeme = Mangle(typeclass_name.m_lexeme);
	if (typeclass_name.m_lexeme[0u] != std::toupper(typeclass_name.m_lexeme[0u]))
	{
		return MarmotCompiler::Failure(GenerateParserError("Class name must start with a capital letter.", typeclass_name));
	}

	MarmotResult::TokenResult defined_name_result = DefineName(typeclass_name, false);
	if (!defined_name_result.HasValue())
	{
		return MarmotCompiler::Failure(defined_name_result.Error());
	}
	typeclass_name = std::move(defined_name_result.Value());

	if (!Match(Token::Name::LEFT_ANGLE))
	{
		return MarmotCompiler::Failure(GenerateParserError("Class must have at least one type parameter.", typeclass_name));
	}

	return InScope
	(
		[&]() -> MarmotResult::StatementResult
		{

			std::vector<Token> type_params;
			std::vector<std::shared_ptr<MarmotType>> type_param_types;
			MarmotResult::TokenListResult generic_parse_result = ParseGenericParameters(&type_param_types);
			if (!generic_parse_result.HasValue())
			{
				return MarmotCompiler::Failure(generic_parse_result.Error());
			}
			type_params = std::move(generic_parse_result.Value());

			std::vector<std::string> type_param_names;
			std::ranges::transform(type_params, std::back_inserter(type_param_names), [](const Token& tok) { return tok.m_lexeme; });
			m_state.m_typeclass_type_params[typeclass_name.m_lexeme] = type_param_names;
			std::vector<std::string>& associated_type_names = m_state.m_typeclass_associated_types[typeclass_name.m_lexeme];
			associated_type_names.clear();

			MarmotResult::TokenResult brace_result = Consume(Token::Name::LEFT_BRACE, "Expected '{' before class body.");
			if (!brace_result.HasValue())
			{
				return MarmotCompiler::Failure(brace_result.Error());
			}

			std::vector<MarmotStatement::Class::AssociatedTypeDeclaration> associated_types;
			std::vector<std::unique_ptr<MarmotStatement>> methods;
			while (!Match(Token::Name::RIGHT_BRACE))
			{
				if (IsAtEnd())
				{
					return MarmotCompiler::Failure(GenerateParserError("Expected '}' after class body.", Previous()));
				}

				if (Match(Token::Name::TYPE))
				{
					MarmotResult::TokenResult assoc_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected associated type name.");
					if (!assoc_name_result.HasValue())
					{
						return MarmotCompiler::Failure(assoc_name_result.Error());
					}

					Token assoc_name = std::move(assoc_name_result.Value());
					assoc_name.m_lexeme = Mangle(assoc_name.m_lexeme);
					if (assoc_name.m_lexeme[0u] != std::toupper(assoc_name.m_lexeme[0u]))
					{
						return MarmotCompiler::Failure(GenerateParserError("Associated type name must start with a capital letter.", assoc_name));
					}

					MarmotResult::TokenResult defined_assoc_result = DefineName(assoc_name, false);
					if (!defined_assoc_result.HasValue())
					{
						return MarmotCompiler::Failure(defined_assoc_result.Error());
					}
					assoc_name = std::move(defined_assoc_result.Value());

					MarmotResult::TokenResult assoc_semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after associated type declaration.");
					if (!assoc_semicolon_result.HasValue())
					{
						return MarmotCompiler::Failure(assoc_semicolon_result.Error());
					}

					associated_type_names.push_back(assoc_name.m_lexeme);
					std::vector<std::shared_ptr<MarmotType>> associated_type_args = type_param_types;
					m_state.m_scopes.back().m_defined_types[assoc_name.m_lexeme] = MarmotType::MakeAssociatedType(typeclass_name.m_lexeme, assoc_name.m_lexeme, std::move(associated_type_args));
					associated_types.emplace_back(assoc_name);
					continue;
				}

				MarmotResult::TokenResult method_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected method name.");
				if (!method_name_result.HasValue())
				{
					return MarmotCompiler::Failure(method_name_result.Error());
				}

				Token method_name = std::move(method_name_result.Value());
				std::string method_name_str = method_name.m_lexeme;

				MarmotResult::TokenResult colon_result = Consume(Token::Name::SINGLE_COLON, "Expected ':' after method name.");
				if (!colon_result.HasValue())
				{
					return MarmotCompiler::Failure(colon_result.Error());
				}

				MarmotResult::TokenResult function_result = Consume(Token::Name::FUNCTION, "Expected 'fn' in method signature.");
				if (!function_result.HasValue())
				{
					return MarmotCompiler::Failure(function_result.Error());
				}

				MarmotResult::TokenResult left_paren_result = Consume(Token::Name::LEFT_PAREN, "Expected '(' before method parameters.");
				if (!left_paren_result.HasValue())
				{
					return MarmotCompiler::Failure(left_paren_result.Error());
				}

				using MethodParam = std::tuple<Token, std::shared_ptr<MarmotType>>;
				MarmotResult::Result<std::vector<MethodParam>> params_result = ParseDelimitedZeroOrMoreLimited<MethodParam>
				(
					[this]() -> MarmotResult::Result<MethodParam>
					{
						MarmotResult::TokenResult param_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected parameter name.");
						if (!param_name_result.HasValue())
						{
							return MarmotCompiler::Failure(param_name_result.Error());
						}

						Token param_name = std::move(param_name_result.Value());
						MarmotResult::TokenResult param_colon_result = Consume(Token::Name::SINGLE_COLON, "Expected ':' after parameter name.");
						if (!param_colon_result.HasValue())
						{
							return MarmotCompiler::Failure(param_colon_result.Error());
						}

						MarmotResult::TypeResult param_type_result = ParseType();
						if (!param_type_result.HasValue())
						{
							return MarmotCompiler::Failure(param_type_result.Error());
						}

						return std::make_tuple(std::move(param_name), std::move(param_type_result.Value()));
					},
					[this]() { return Consume(Token::Name::COMMA, "Expected ',' between parameters."); },
					[this]() { return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after parameters."); }
				);
				if (!params_result.HasValue())
				{
					return MarmotCompiler::Failure(std::move(params_result.Error()));
				}

				MarmotResult::TokenResult arrow_result = Consume(Token::Name::THIN_ARROW, "Expected '->' before return type.");
				if (!arrow_result.HasValue())
				{
					return MarmotCompiler::Failure(arrow_result.Error());
				}

				MarmotResult::TypeResult return_type_result = ParseType();
				if (!return_type_result.HasValue())
				{
					return MarmotCompiler::Failure(return_type_result.Error());
				}
				std::shared_ptr<MarmotType> return_type = std::move(return_type_result.Value());

				MarmotResult::TokenResult method_semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after method signature.");
				if (!method_semicolon_result.HasValue())
				{
					return MarmotCompiler::Failure(method_semicolon_result.Error());
				}

				std::vector<std::shared_ptr<MarmotType>> param_types;
				std::vector<Token> param_tokens;
				std::vector<BindingId> param_bindings;
				const LexicalFunctionId method_function = std::make_shared<LexicalFunction>();
				param_types.reserve(params_result->size());
				param_tokens.reserve(params_result->size());
				for (MethodParam& tuple : params_result.Value())
				{
					param_types.emplace_back(std::get<1>(tuple));
					param_tokens.emplace_back(std::move(std::get<0>(tuple)));
					param_bindings.push_back(std::make_shared<BindingIdentity>(method_function, m_state.m_next_binding_index++));
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
						MarmotStatement::FunctionDefinition(method_name, std::vector<Token>(), FunctionParameters(std::move(param_tokens), std::move(param_bindings), method_function), std::move(param_types), std::move(return_type), nullptr, MarmotCompiler::None, std::vector<MarmotType::ClassConstraint>())
					)
				);
			}

			MarmotResult::TokenResult class_semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after class body.");
			if (!class_semicolon_result.HasValue())
			{
				return MarmotCompiler::Failure(class_semicolon_result.Error());
			}

			return std::make_unique<MarmotStatement>
			(
				MarmotStatement::Class(std::move(typeclass_name), std::move(type_params), std::move(associated_types), std::move(methods))
			);
		}
	);
}

MarmotResult::Result<MarmotStatement::FunctionDefinition> Parser::ParseInstanceMethodBody(const Token& method_name)
{
	return WithGenericParameters([&](std::vector<Token> generic_params, std::vector<std::shared_ptr<MarmotType>>) -> MarmotResult::Result<MarmotStatement::FunctionDefinition>
	{
		return InScope([&]() -> MarmotResult::Result<MarmotStatement::FunctionDefinition>
		{
			MarmotResult::TokenResult left_paren_result = Consume(Token::Name::LEFT_PAREN, "Expected '(' before method parameters.");
			if (!left_paren_result.HasValue())
			{
				return MarmotCompiler::Failure(left_paren_result.Error());
			}

			m_state.m_function = std::make_shared<LexicalFunction>();

			MarmotResult::FunctionParamsResult params_parse_result = ParseFunctionParameters();
			if (!params_parse_result.HasValue())
			{
				return MarmotCompiler::Failure(params_parse_result.Error());
			}

			std::vector<std::pair<Token, std::shared_ptr<MarmotType>>> param_tuples = std::move(params_parse_result.Value());
			ParamSplit split = SplitParamTuples(std::move(param_tuples));
			FunctionParameters params = ParametersFor(std::move(split.m_params));
			std::vector<std::shared_ptr<MarmotType>> param_types = std::move(split.m_types);

			MarmotResult::TokenResult return_colon_result = ConsumeReturnTypeSeparator("Expected '->' before return type.");
			if (!return_colon_result.HasValue())
			{
				return MarmotCompiler::Failure(return_colon_result.Error());
			}

			MarmotResult::TypeResult return_type_result = ParseType();
			if (!return_type_result.HasValue())
			{
				return MarmotCompiler::Failure(return_type_result.Error());
			}
			std::shared_ptr<MarmotType> return_type = std::move(return_type_result.Value());

			MarmotResult::TokenResult fat_arrow_result = Consume(Token::Name::FAT_ARROW, "Expected '=>' before method body.");
			if (!fat_arrow_result.HasValue())
			{
				return MarmotCompiler::Failure(fat_arrow_result.Error());
			}

			MarmotResult::ExpressionResult body_result = ParseExpression();
			if (!body_result.HasValue())
			{
				return MarmotCompiler::Failure(body_result.Error());
			}
			std::unique_ptr<MarmotExpression> body = std::move(body_result.Value());

			MarmotResult::TokenResult method_semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after method body.");
			if (!method_semicolon_result.HasValue())
			{
				return MarmotCompiler::Failure(method_semicolon_result.Error());
			}
			return MarmotStatement::FunctionDefinition(method_name, std::move(generic_params), std::move(params), std::move(param_types), std::move(return_type), std::move(body), MarmotCompiler::None, std::vector<MarmotType::ClassConstraint>());
		});
	});
}

MarmotResult::StatementResult Parser::ParseInstanceDeclaration()
{
	MarmotResult::TokenResult class_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected class name.");
	if (!class_result.HasValue())
	{
		return MarmotCompiler::Failure(class_result.Error());
	}

	Token typeclass_name = std::move(class_result.Value());
	if (!m_state.m_typeclass_type_params.contains(typeclass_name.m_lexeme))
	{
		return MarmotCompiler::Failure(GenerateParserError("Unknown class '" + typeclass_name.m_lexeme + "'.", typeclass_name));
	}

	MarmotResult::TokenResult angle_result = Consume(Token::Name::LEFT_ANGLE, "Expected '<' before type arguments.");
	if (!angle_result.HasValue())
	{
		return MarmotCompiler::Failure(angle_result.Error());
	}

	return InScope
	(
		[&]() -> MarmotResult::StatementResult
		{

			MarmotResult::TypeListResult type_args_result = InContext
			(
				[this]()
				{
					m_state.m_allow_implicit_generic_params = true;
					return ParseDelimitedZeroOrMoreLimited<std::shared_ptr<MarmotType>>
					(
						[this]() { return ParseType(); },
						[this]() { return Consume(Token::Name::COMMA, "Expected ',' between type arguments."); },
						[this]() { return ConsumeTypeRightAngle("Expected '>' after type arguments."); }
					);
				}
			);
			if (!type_args_result.HasValue())
			{
				return MarmotCompiler::Failure(type_args_result.Error());
			}
			std::vector<std::shared_ptr<MarmotType>> type_args = std::move(type_args_result.Value());

			if (type_args.empty())
			{
				return MarmotCompiler::Failure(GenerateParserError("Instance must have at least one type argument.", typeclass_name));
			}

			std::vector<MarmotType::ClassConstraint> constraints;
			if (Match(Token::Name::WHERE))
			{
				ClassConstraintListResult constraints_result = ParseClassConstraints();
				if (!constraints_result.HasValue())
				{
					return MarmotCompiler::Failure(constraints_result.Error());
				}

				constraints = std::move(constraints_result.Value());
			}

			PushActiveConstraints(constraints);

			MarmotResult::TokenResult brace_result = Consume(Token::Name::LEFT_BRACE, "Expected '{' before instance methods.");
			if (!brace_result.HasValue())
			{
				return MarmotCompiler::Failure(brace_result.Error());
			}

			const std::vector<std::string>& declared_associated_types = m_state.m_typeclass_associated_types[typeclass_name.m_lexeme];
			std::vector<MarmotStatement::Instance::AssociatedTypeBinding> associated_types;
			std::unordered_set<std::string> bound_associated_type_names;
			std::vector<MarmotStatement::Instance::Method> methods;

			while (!Match(Token::Name::RIGHT_BRACE))
			{
				if (IsAtEnd())
				{
					return MarmotCompiler::Failure(GenerateParserError("Expected '}' after instance body.", Previous()));
				}

				if (Match(Token::Name::TYPE))
				{
					MarmotResult::TokenResult assoc_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected associated type name.");
					if (!assoc_name_result.HasValue())
					{
						return MarmotCompiler::Failure(assoc_name_result.Error());
					}

					Token assoc_name = std::move(assoc_name_result.Value());
					assoc_name.m_lexeme = Mangle(assoc_name.m_lexeme);
					if (!std::ranges::contains(declared_associated_types, assoc_name.m_lexeme))
					{
						return MarmotCompiler::Failure(GenerateParserError("Unknown associated type '" + assoc_name.m_lexeme + "' for class '" + typeclass_name.m_lexeme + "'.", assoc_name));
					}
					if (!bound_associated_type_names.insert(assoc_name.m_lexeme).second)
					{
						return MarmotCompiler::Failure(GenerateParserError("Duplicate associated type binding '" + assoc_name.m_lexeme + "'.", assoc_name));
					}

					MarmotResult::TokenResult defined_assoc_result = DefineName(assoc_name, false);
					if (!defined_assoc_result.HasValue())
					{
						return MarmotCompiler::Failure(defined_assoc_result.Error());
					}
					assoc_name = std::move(defined_assoc_result.Value());

					MarmotResult::TokenResult equals_result = Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after associated type name.");
					if (!equals_result.HasValue())
					{
						return MarmotCompiler::Failure(equals_result.Error());
					}

					MarmotResult::TypeResult assoc_type_result = ParseType();
					if (!assoc_type_result.HasValue())
					{
						return MarmotCompiler::Failure(assoc_type_result.Error());
					}
					std::shared_ptr<MarmotType> assoc_type = std::move(assoc_type_result.Value());

					MarmotResult::TokenResult assoc_semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after associated type binding.");
					if (!assoc_semicolon_result.HasValue())
					{
						return MarmotCompiler::Failure(assoc_semicolon_result.Error());
					}

					m_state.m_scopes.back().m_defined_types[assoc_name.m_lexeme] = assoc_type;
					associated_types.emplace_back(assoc_name, std::shared_ptr<MarmotType>(assoc_type));
					continue;
				}

				// An instance method is spelled `def show = fn(...) -> R => body;`, the same
				// binding form a module-level function uses.
				if (!Match(Token::Name::DEF))
				{
					return MarmotCompiler::Failure(GenerateParserError("Expected 'def' or associated type binding in instance body.", Peek(0)));
				}

				MarmotResult::TokenResult method_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected method name.");
				if (!method_name_result.HasValue())
				{
					return MarmotCompiler::Failure(method_name_result.Error());
				}
				Token method_name = std::move(method_name_result.Value());

				MarmotResult::TokenResult method_equal_result = Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after instance method name.");
				if (!method_equal_result.HasValue())
				{
					return MarmotCompiler::Failure(method_equal_result.Error());
				}

				MarmotResult::TokenResult method_function_result = Consume(Token::Name::FUNCTION, "Expected 'fn' after '=' in an instance method binding.");
				if (!method_function_result.HasValue())
				{
					return MarmotCompiler::Failure(method_function_result.Error());
				}

				MarmotResult::Result<MarmotStatement::FunctionDefinition> method = ParseInstanceMethodBody(method_name);
				if (!method.HasValue())
				{
					return MarmotCompiler::Failure(std::move(method.Error()));
				}
				methods.emplace_back(method_name, std::move(method.Value()));
			}

			MarmotResult::TokenResult instance_semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after instance body.");
			if (!instance_semicolon_result.HasValue())
			{
				return MarmotCompiler::Failure(instance_semicolon_result.Error());
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

			return std::make_unique<MarmotStatement>
			(
				MarmotStatement::Instance(std::move(typeclass_name), std::move(type_args), std::move(constraints), std::move(associated_types), std::move(methods))
			);
		}
	);
}

MarmotResult::StatementResult Parser::ParseSimpleStatement()
{
	ParseState checkpoint = m_state;

	MarmotResult::ExpressionResult expr_result = ParseExpression();
	if (!expr_result.HasValue())
	{
		return MarmotCompiler::Failure(std::move(expr_result.Error()));
	}

	std::unique_ptr<MarmotExpression> expr = std::move(expr_result.Value());
	MarmotResult::TokenResult semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after expression.");
	if (!semicolon_result.HasValue())
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
			return MarmotCompiler::Failure(GenerateParserError("Expected ';' after the for loop: it is an expression, and as a statement it ends with ';'.", Previous()));
		}

		return MarmotCompiler::Failure(std::move(semicolon_result.Error()));
	}

	Token semi_colon = std::move(semicolon_result.Value());
	return std::make_unique<MarmotStatement>(MarmotStatement::ExpressionStatement(semi_colon, std::move(expr)));
}

MarmotResult::StatementResult Parser::ParseForeignStatement()
{
	return Consume(Token::Name::TEXT_LITERAL, "Expected the foreign symbol, or the library of a foreign block.")
		.AndThen
		(
			[this](Token&& name) -> MarmotResult::StatementResult
			{
				if (Match(Token::Name::LEFT_BRACE))
				{
					return ParseForeignBlock(name);
				}

				return ParseForeignDeclaration(name, MarmotCompiler::None);
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
		if (!symbol.HasValue())
		{
			return MarmotCompiler::Failure(std::move(symbol.Error()));
		}

		MarmotResult::StatementResult declaration = ParseForeignDeclaration(symbol.Value(), library.m_lexeme);
		if (!declaration.HasValue())
		{
			return declaration;
		}

		if (declaration.Value()->GetStatement<MarmotStatement::ForeignDefinition>().m_binding.HasValue())
		{
			return MarmotCompiler::Failure(GenerateParserError("A foreign block can only appear at the top level of a module.", library));
		}

		declarations.emplace_back(std::move(declaration.Value()));
	}

	MarmotResult::TokenResult closing = Consume(Token::Name::RIGHT_BRACE, "Expected '}' after the foreign block.");
	if (!closing.HasValue())
	{
		return MarmotCompiler::Failure(std::move(closing.Error()));
	}

	if (declarations.empty())
	{
		return MarmotCompiler::Failure(GenerateParserError(std::format("The foreign block for \"{}\" declares no functions.", library.m_lexeme), library));
	}

	for (size_t index = 1uz; index < declarations.size(); index += 1uz)
	{
		m_pending_statements.push(std::move(declarations[index]));
	}
	return std::move(declarations.front());
}

MarmotResult::StatementResult Parser::ParseForeignDeclaration(const Token& foreign_name, const MarmotCompiler::Optional<std::string>& block_library)
{
	return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected foreign function name.")
		.AndThen
		(
			[this, &foreign_name, &block_library](Token&& function_name) ->MarmotResult::StatementResult
			{
				function_name.m_lexeme = Mangle(function_name.m_lexeme);

				return Consume(Token::Name::SINGLE_COLON, "Expected ':' before foreign function type.")
					.AndThen
					(
						[&foreign_name, &function_name, &block_library, this](Token&&) ->MarmotResult::StatementResult
						{
							constexpr bool is_variable = true;
							return DefineName(function_name, is_variable)
								.AndThen
								(
									[&foreign_name, &function_name, &block_library, this](Token&& name) ->MarmotResult::StatementResult
									{
										MarmotCompiler::Optional<BindingId> binding = RegisterLocalBinding(name.m_lexeme);
										return InScope
										(
											[&]() -> MarmotResult::StatementResult
											{
												m_state.m_allow_implicit_generic_params = true;

												constexpr bool is_foreign = true;
												return ParseType(is_foreign)
													.AndThen
													(
														[&foreign_name, &function_name, &binding, &block_library, this](std::shared_ptr<MarmotType>&& type)->MarmotResult::StatementResult
														{
															if (!type->IsType<MarmotType::FunctionType>())
															{
																return MarmotCompiler::Failure(GenerateParserError("'foreign' only applies to function types.", function_name));
															}

															MarmotCompiler::Optional<std::string> library = block_library;
															if (!block_library.HasValue() && Check(Token::Name::IDENTIFIER_LITERAL, 0) && Peek(0).m_lexeme == "from")
															{
																Advance();
																MarmotResult::TokenResult library_name = Consume(Token::Name::TEXT_LITERAL, "Expected the library name after 'from'.");
																if (!library_name.HasValue())
																{
																	return MarmotCompiler::Failure(std::move(library_name.Error()));
																}
																library = library_name->m_lexeme;
															}

															return Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after foreign function type.")
																.AndThen
																(
																	[&foreign_name, &function_name, &type, &binding, &library](Token&&) ->MarmotResult::StatementResult
																	{
																		if (!library.HasValue())
																		{
																			MarmotType::FunctionType& signature = type->GetType<MarmotType::FunctionType>();
																			signature.m_primitive = FindPrimitive(foreign_name.m_lexeme);
																			signature.m_intrinsic = MarmotIntrinsics::FindIndex(foreign_name.m_lexeme);
																		}
																		return std::make_unique<MarmotStatement>(MarmotStatement::ForeignDefinition(function_name, foreign_name.m_lexeme, std::move(type), std::move(binding), std::move(library)));
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
		);
}

MarmotResult::StatementResult Parser::ParseStatement()
{
	return ParseSimpleStatement();
}

MarmotResult::StatementResult Parser::ParseDeclaration()
{
	// End of current block: no further declarations to parse here.
	if (Check(Token::Name::RIGHT_BRACE, 0))
	{
		return NoMatch<std::unique_ptr<MarmotStatement>>();
	}

	return ParseChoice<std::unique_ptr<MarmotStatement>>
	(
		m_state,
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

MarmotResult::TokenListResult Parser::ParseDerivingTargets(const Token& context_token)
{
	return Consume(Token::Name::LEFT_PAREN, "Expected '(' after 'deriving'.")
		.AndThen
		(
			[&context_token, this](Token&&) -> MarmotResult::TokenListResult
			{
				std::unordered_set<std::string> seen_targets;
				return ParseDelimitedZeroOrMoreLimited<Token>
					(
						[&seen_targets, this]() -> MarmotResult::TokenResult
						{
							return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected deriving target.")
								.AndThen
								(
									[&seen_targets, this](Token&& target) -> MarmotResult::TokenResult
									{
										if (!seen_targets.insert(target.m_lexeme).second)
										{
											return MarmotCompiler::Failure(GenerateParserError("Duplicate deriving target.", target));
										}

										return target;
									}
								);
						},
						[this]() { return Consume(Token::Name::COMMA, "Expected ',' after deriving target."); },
						[this]() { return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after deriving targets."); }
					)
					.AndThen
					(
						[&context_token, this](std::vector<Token>&& targets) -> MarmotResult::TokenListResult
						{
							if (targets.empty())
							{
								return MarmotCompiler::Failure(GenerateParserError("Expected at least one deriving target.", context_token));
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

MarmotResult::StatementResult Parser::GenerateDerivedStatement(const DerivedDeclarationGenerator& generator, const OrderingType* ordering)
{
	MarmotCompiler::Expected<std::unique_ptr<MarmotStatement>, DerivedDeclarationError> generated = generator.Generate(ordering);
	if (!generated.HasValue())
	{
		return MarmotCompiler::Failure(GenerateParserError(std::move(generated.Error().m_message), generated.Error().m_token));
	}
	if (generated.Value()->IsStatement<MarmotStatement::Instance>())
	{
		const MarmotStatement::Instance& instance = generated.Value()->GetStatement<MarmotStatement::Instance>();
		std::vector<std::string> methods = instance.m_methods
			| std::views::transform([](const MarmotStatement::Instance::Method& method) { return method.m_definition.m_name.m_lexeme; })
			| std::ranges::to<std::vector<std::string>>();
		RegisterSyntheticInstanceMetadata(instance.m_class_name.m_lexeme, instance.m_type_args, instance.m_constraints, methods);
	}
	return std::move(generated.Value());
}

template<typename Declaration>
MarmotResult::VoidResult Parser::QueueDerivedStatements(const Declaration& declaration, const std::vector<Token>& deriving_targets)
{
	if (!IsAtGlobalScope())
	{
		return MarmotCompiler::Failure(GenerateParserError("Deriving declarations are only supported at global scope.", declaration.m_name));
	}
	for (const Token& target : deriving_targets)
	{
		MarmotCompiler::Expected<DerivedDeclarationGenerator, DerivedDeclarationError> derivation = DerivedDeclarationGenerator::Prepare(declaration, target);
		if (!derivation.HasValue())
		{
			return MarmotCompiler::Failure(GenerateParserError(std::move(derivation.Error().m_message), derivation.Error().m_token));
		}
		if (Token* name = derivation->NameToReserve())
		{
			MarmotResult::TokenResult registered = DefineName(*name, true);
			if (!registered.HasValue())
			{
				return MarmotCompiler::Failure(std::move(registered.Error()));
			}
		}
		if (target.m_lexeme == ORDERABLE_CLASS_NAME)
		{
			m_pending_statements.emplace(DeferredOrderingDerivation{ std::move(derivation.Value()), target });
			continue;
		}
		MarmotResult::StatementResult generated = GenerateDerivedStatement(derivation.Value(), nullptr);
		if (!generated.HasValue())
		{
			return MarmotCompiler::Failure(std::move(generated.Error()));
		}
		m_pending_statements.push(std::move(generated.Value()));
	}
	return {};
}
