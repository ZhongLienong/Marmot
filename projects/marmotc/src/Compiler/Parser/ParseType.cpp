#include "Parser.h"
#include "Compiler/Names/SpecialNames.h"
#include "Compiler/TypeTransform/TypeTransform.h"
#include "Support/Expected.h"
#include "Support/Optional.h"
#include <algorithm>
#include <format>
#include <ranges>

using namespace std::string_literals;

namespace
{
	// The type a keyword names on its own, such as `Int`.
	MarmotCompiler::Optional<std::shared_ptr<MarmotType>> KeywordType(Token::Name name)
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
			return MarmotCompiler::None;
		}
	}
}

std::string Parser::BuildTypeArgumentCountMismatchMessage(const std::string& type_name, bool is_alias, size_t expected_count, size_t actual_count)
{
	if (!is_alias)
	{
		return std::format("Type argument count mismatch: expected {}, got {}", expected_count, actual_count);
	}

	return std::format("Type argument count mismatch for alias '{}': expected {}, got {}", type_name, expected_count, actual_count);
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
				if (token_name == Token::Name::END_OF_FILE)
				{
					return;
				}
				if (depth == 0)
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
MarmotCompiler::Optional<CompilerError> Parser::FillDeferredInstantiations()
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

		TypeTransform::InstantiateInto((*ready)->m_target, (*ready)->m_template, (*ready)->m_substitutions);
		m_unfilled_instantiations.erase((*ready)->m_target.get());
		waiting.erase(ready);
	}

	return MarmotCompiler::None;
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

MarmotResult::TokenResult Parser::ConsumeTypeRightAngle(std::string_view message)
{
	Token& current = Peek(0);
	if (current.m_token_name == Token::Name::RIGHT_SHIFT)
	{
		// Keep the current token's address and leave a second '>' for the
		// enclosing type argument list.
		TokenStream remaining;
		remaining.AddToken(Token(Token::Name::RIGHT_ANGLE, current));
		current = Token(Token::Name::RIGHT_ANGLE, current);
		m_context.m_tokens.Insert(m_context.m_tokens.begin() + m_state.m_current_token_index + 1, std::move(remaining));
	}
	if (current.m_token_name == Token::Name::RIGHT_ANGLE)
	{
		return Advance();
	}

	return MarmotCompiler::Failure(GenerateParserError(std::string(message), current));
}

MarmotResult::TokenResult Parser::ConsumeReturnTypeSeparator(std::string_view message)
{
	if (Check(Token::Name::THIN_ARROW, 0))
	{
		return Advance();
	}

	return MarmotCompiler::Failure(GenerateParserError(std::string(message), Peek(0)));
}

MarmotResult::TypeResult Parser::ParseTypeOfOne(Token::Name keyword, std::string_view argument_noun, MakeTypeOfOne make)
{
	return ParseWhen<std::shared_ptr<MarmotType>>
	(
		m_state,
		keyword,
		[this, keyword, argument_noun, make](Token&&) -> MarmotResult::TypeResult
		{
			return Consume(Token::Name::LEFT_ANGLE, std::format("Expected '<' after '{}'.", Token::Spelling(keyword)))
				.AndThen([this](Token&&) { return ParseType(); })
				.AndThen
				(
					[this, argument_noun, make](std::shared_ptr<MarmotType>&& type) -> MarmotResult::TypeResult
					{
						return ConsumeTypeRightAngle(std::format("Expected '>' after {}.", argument_noun))
							.Transform([&type, make](Token&&) { return make(type); });
					}
				);
		}
	);
}

MarmotResult::TypeResult Parser::ParseType(bool is_foreign)
{
	return ParseChoice<std::shared_ptr<MarmotType>>
	(
		m_state,
		[this]() -> MarmotResult::TypeResult
		{
			const MarmotCompiler::Optional<std::shared_ptr<MarmotType>> keyword_type = KeywordType(Peek(m_state, 0).m_token_name);
			if (!keyword_type.HasValue())
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
			return ParseWhen<std::shared_ptr<MarmotType>>
			(
				m_state,
				Token::Name::FUNCTION,
				[this, is_foreign](Token&&) -> MarmotResult::TypeResult
				{
					std::function<MarmotResult::TypeResult(std::vector<std::shared_ptr<MarmotType>>&&)> func_type_aux_func = [is_foreign, this](std::vector<std::shared_ptr<MarmotType>>&& types) ->MarmotResult::TypeResult
						{
							return Consume(Token::Name::THIN_ARROW, "Expected '->' before return type token.")
								.AndThen
								(
									[&types, is_foreign, this](Token&&) ->MarmotResult::TypeResult
									{
										return ParseType()
											.AndThen
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
						.AndThen
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
										.AndThen
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
			return ParseWhen<std::shared_ptr<MarmotType>>
			(
				m_state,
				Token::Name::LEFT_PAREN,
				[this](Token&&) -> MarmotResult::TypeResult
				{
					if (Match(Token::Name::RIGHT_PAREN))
					{
						return MarmotType::MakeLiteralType<MarmotType::UnitType>();
					}

					return ParseType()
						.AndThen
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
										element_types.push_back(std::move(elem_result.Value()));
									} while (Match(Token::Name::COMMA));

									return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after tuple types.")
										.AndThen
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
										.AndThen([&first_type](Token&&) -> MarmotResult::TypeResult
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
			return ParseWhen<std::shared_ptr<MarmotType>>
			(
				m_state,
				Token::Name::IDENTIFIER_LITERAL,
				[this](Token&&) -> MarmotResult::TypeResult
				{
					return MatchNameResolution()
						.AndThen
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
										return MarmotCompiler::Failure(GenerateParserError("Undefined struct or union.", type_name));
									}

									std::string associated_type_name = ExtractSymbolName(type_name.m_lexeme);
									TypeclassAssociatedTypeMap::const_iterator assoc_it = m_state.m_typeclass_associated_types.find(associated_type_qualifier);
									if (assoc_it == m_state.m_typeclass_associated_types.cend()
										|| !std::ranges::contains(assoc_it->second, associated_type_name))
									{
										return MarmotCompiler::Failure(GenerateParserError("Undefined struct or union.", type_name));
									}

									if (!Match(Token::Name::LEFT_ANGLE))
									{
										return MarmotCompiler::Failure(GenerateParserError("Expected '<' after associated type name.", type_name));
									}

									MarmotResult::TypeListResult type_args_result = ParseDelimitedZeroOrMoreLimited<std::shared_ptr<MarmotType>>
									(
										[this]() { return ParseType(); },
										[this]() { return Consume(Token::Name::COMMA, "Expected ',' after associated type argument."); },
										[this]() { return ConsumeTypeRightAngle("Expected '>' after associated type arguments."); }
									);
									if (!type_args_result.HasValue())
									{
										return MarmotCompiler::Failure(type_args_result.Error());
									}

									const size_t expected_arg_count = m_state.m_typeclass_type_params.contains(associated_type_qualifier)
										? m_state.m_typeclass_type_params.at(associated_type_qualifier).size()
										: 0u;
									std::vector<std::shared_ptr<MarmotType>> type_args = std::move(type_args_result.Value());
									if (type_args.size() != expected_arg_count)
									{
										return MarmotCompiler::Failure
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
										return MarmotCompiler::Failure(GenerateParserError(BuildAmbiguousUseImportError(type_name.m_lexeme, use_import_resolution.m_conflicting_modules), type_name));
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
														return MarmotCompiler::Failure(GenerateParserError("Type '" + symbol_name + "' is not exported by module '" + qualifier + "'.", type_name));
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
											return MarmotCompiler::Failure(GenerateParserError(BuildUnimportedSymbolError(type_name.m_lexeme, exporting_modules), type_name));
										}

										if (m_state.m_allow_implicit_generic_params && type_name.m_lexeme.find(NameSeparator) == std::string::npos)
										{
											std::shared_ptr<MarmotType> generic_type = MarmotType::MakeGenericType(type_name.m_lexeme);
											m_state.m_scopes.back().m_defined_types[type_name.m_lexeme] = generic_type;
											base_type = generic_type;
										}
										else
										{
											return MarmotCompiler::Failure(GenerateParserError("Undefined struct or union.", type_name));
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

									if (!type_args_result.HasValue())
									{
										return MarmotCompiler::Failure(type_args_result.Error());
									}

									std::vector<std::shared_ptr<MarmotType>> type_args = std::move(type_args_result.Value());

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
										return MarmotCompiler::Failure(GenerateParserError(
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

									return TypeTransform::Instantiate(base_type, substitutions);
								}

								// Without arguments a parameterised alias would hand back a template
								// whose parameters nothing binds.
								if (alias_generic_params != nullptr)
								{
									return MarmotCompiler::Failure(GenerateParserError(
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
			return MarmotCompiler::Failure(GenerateParserError("Expected type token.", Peek(0)));
		}
	);
}

MarmotResult::TokenListResult Parser::ParseGenericParameters(std::vector<std::shared_ptr<MarmotType>>* out_types)
{
	return ParseDelimitedZeroOrMoreLimited<Token>
		(
			[this, out_types]() -> MarmotResult::TokenResult
			{
				return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected generic parameter name.")
					.AndThen
					(
						[this, out_types](Token&& param_name) -> MarmotResult::TokenResult
						{
							constexpr bool is_variable = false;
							return DefineName(param_name, is_variable)
								.AndThen
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
		if (!std::ranges::contains(m_state.m_active_constraints, constraint))
		{
			m_state.m_active_constraints.push_back(constraint);
		}
	}
}

Parser::ClassConstraintListResult Parser::ParseClassConstraints()
{
	using ClassConstraintResult = MarmotResult::Result<MarmotType::ClassConstraint>;

	// An equality constraint reads 'Class::Assoc<S> ~ Type'. The '::' one token
	// past the leading identifier is what distinguishes it from a class
	// constraint, which reads 'Class<Type,...>'.
	std::function<ClassConstraintResult()> parse_equality_constraint = [this]() -> ClassConstraintResult
		{
			Token projection_token = Peek(0);
			return ParseType()
				.AndThen
				(
					[&projection_token, this](std::shared_ptr<MarmotType>&& equality_lhs) -> ClassConstraintResult
					{
						if (!equality_lhs->IsType<MarmotType::AssociatedType>())
						{
							return MarmotCompiler::Failure(GenerateParserError("The left side of a '~' constraint must be an associated type projection, such as 'Stepper::Item<S>'.", projection_token));
						}

						return Consume(Token::Name::TILDE, "Expected '~' after the associated type projection in an equality constraint.")
							.AndThen
							(
								[&equality_lhs, this](Token&&) -> ClassConstraintResult
								{
									return ParseType()
										.AndThen
										(
											[&equality_lhs](std::shared_ptr<MarmotType>&& equality_rhs) -> ClassConstraintResult
											{
												return MarmotType::ClassConstraint{ std::move(equality_lhs), std::move(equality_rhs) };
											}
										);
								}
							);
					}
				);
		};

	std::function<ClassConstraintResult()> parse_constraint = [&parse_equality_constraint, this]() -> ClassConstraintResult
		{
			// A leading '~' after the identifier routes here too, so that 'T ~ Int'
			// is rejected by the rule it actually breaks rather than by a missing '<'.
			if (Check(Token::Name::IDENTIFIER_LITERAL, 0) && (Check(Token::Name::DOUBLE_COLON, 1) || Check(Token::Name::TILDE, 1)))
			{
				return parse_equality_constraint();
			}

			return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected class name in constraint.")
				.AndThen
				(
					[this](Token&& first_token) -> ClassConstraintResult
					{
						Token typeclass_name = std::move(first_token);
						return Consume(Token::Name::LEFT_ANGLE, "Expected '<' after class name in constraint (e.g., 'Show<T>').")
							.AndThen
							(
								[&typeclass_name, this](Token&&) -> ClassConstraintResult
								{
									return ParseDelimitedZeroOrMoreLimited<std::shared_ptr<MarmotType>>
										(
											[this]() { return ParseType(); },
											[this]() { return Consume(Token::Name::COMMA, "Expected ',' between type arguments."); },
											[this]() { return ConsumeTypeRightAngle("Expected '>' after type arguments."); }
										)
										.AndThen
										(
											[&typeclass_name](std::vector<std::shared_ptr<MarmotType>>&& type_args) -> ClassConstraintResult
											{
												return MarmotType::ClassConstraint{ typeclass_name.m_lexeme, std::move(type_args) };
											}
										);
								}
							);
					}
				);
		};

	ClassConstraintListResult constraints_result = ParseDelimitedOneOrMoreUnlimited<MarmotType::ClassConstraint>
	(
		parse_constraint,
		[this]() { return Consume(Token::Name::COMMA, "Expected ',' between constraints."); }
	);

	if (!constraints_result.HasValue())
	{
		return MarmotCompiler::Failure(std::move(constraints_result.Error()));
	}

	return std::move(constraints_result.Value());
}

MarmotResult::TypeResult Parser::ParseRefTypeArguments()
{
	return Consume(Token::Name::LEFT_ANGLE, "Expected '<' after 'Ref'.")
		.AndThen
		(
			[this](Token&&) -> MarmotResult::TypeResult
			{
				return ParseType()
					.AndThen
					(
						[this](std::shared_ptr<MarmotType>&& element_type) -> MarmotResult::TypeResult
						{
							return ConsumeTypeRightAngle("Expected '>' after reference element type.")
								.AndThen
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
