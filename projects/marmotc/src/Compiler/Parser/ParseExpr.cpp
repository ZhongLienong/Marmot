#include "Parser.h"
#include "Bytecode/Format/Format.h"
#include "Compiler/Names/SpecialNames.h"
#include <algorithm>
#include <cctype>
#include <format>
#include <limits>
#include <ranges>

using namespace std::string_literals;

namespace
{
	using ParamTuple = std::pair<Token, std::shared_ptr<MarmotType>>;

	void AppendUniqueConstraint(std::vector<MarmotType::ClassConstraint>& constraints, MarmotType::ClassConstraint&& constraint)
	{
		if (!std::ranges::contains(constraints, constraint))
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
			&& !IsPrefixedIntegerLiteral(lexeme);
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
			return std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(name_token, MarmotExpression::NameContext::Global(), &name_token));
		}
		// Local
		else if (IsLocalName(find_result))
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(name_token, MarmotExpression::NameContext::Local(find_result->second), &name_token));
		}
		// Cell
		else
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(name_token, MarmotExpression::NameContext::Cell(find_result->second), &name_token));
		}
	}

	// Defined further down this module.
	if (m_state.m_top_level_names.contains(lookup_name))
	{
		RecordTopLevelReference(name_token);
		return std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(name_token, MarmotExpression::NameContext::Global(), &name_token));
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
			return std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(qualified_token, MarmotExpression::NameContext::Global(), &name_token));
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

Parser::ParamSplit Parser::SplitParamTuples(std::vector<std::pair<Token, std::shared_ptr<MarmotType>>>&& tuples)
{
	ParamSplit result;
	result.m_params.reserve(tuples.size());
	result.m_types.reserve(tuples.size());
	for (std::pair<Token, std::shared_ptr<MarmotType>>& tuple : tuples)
	{
		result.m_params.emplace_back(std::move(tuple.first));
		result.m_types.emplace_back(std::move(tuple.second));
	}
	return result;
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
	if (Match(Token::Name::SINGLE_MINUS, Token::Name::SINGLE_PLUS))
	{
		Token& op = Previous();
		return ParseUnaryLogicalBitwise()
			.and_then
			(
				[&op](std::unique_ptr<MarmotExpression>&& right) -> MarmotResult::ExpressionResult
				{
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
		.and_then
		(
			[this](std::unique_ptr<MarmotExpression>&& cell) -> MarmotResult::ExpressionResult
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
			}
		);
}

MarmotResult::ExpressionResult Parser::ParseAs()
{
	return FoldWhile
	(
		ParseLogicalOr(),
		[this]() { return Match(Token::Name::AS); },
		[this](std::unique_ptr<MarmotExpression>&& expression) -> MarmotResult::ExpressionResult
		{
			const Token as = Previous();
			return ParseType().transform([&](std::shared_ptr<MarmotType>&& type)
			{
				return std::make_unique<MarmotExpression>(MarmotExpression::As(as, std::move(type), std::move(expression)));
			});
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

MarmotResult::ExpressionResult Parser::ParsePostfixChain(std::unique_ptr<MarmotExpression>&& expression)
{
	return FoldWhile
	(
		MarmotResult::ExpressionResult(std::move(expression)),
		[this]()
		{
			return Check(Token::Name::LEFT_PAREN, 0) || Check(Token::Name::SINGLE_DOT, 0) || Check(Token::Name::LEFT_BRACKET, 0);
		},
		[this](std::unique_ptr<MarmotExpression>&& value) -> MarmotResult::ExpressionResult
		{
			if (Match(Token::Name::LEFT_PAREN))
			{
				return FinishCall(std::move(value));
			}
			if (Match(Token::Name::SINGLE_DOT))
			{
				return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected identifier after '.'.")
					.transform([&](Token&& name)
					{
						return std::make_unique<MarmotExpression>(MarmotExpression::MemberAccess(name, std::move(value)));
					});
			}
			return ParseArrayAccessHelper(std::move(value));
		}
	);
}

MarmotResult::ExpressionResult Parser::ParseCall()
{
	return ParsePrimary()
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
		if (Previous().IsDiscard())
		{
			return std::unexpected(GenerateParserError("'_' discards a value and cannot be read.", Previous()));
		}
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
							return std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(variable, MarmotExpression::NameContext::Global(), &Previous()));
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
		std::unique_ptr<MarmotExpression> literal = std::make_unique<MarmotExpression>(MarmotExpression::Literal(token, MarmotExpression::LiteralKind::Integer));
		if (IsPrefixedIntegerLiteral(token.m_lexeme))
		{
			return literal;
		}
		return RejectExponent(std::move(literal));
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
	return ParseBinary(&Parser::ParsePipe, Token::Name::DOUBLE_AMPERSAND);
}

MarmotResult::ExpressionResult Parser::ParseLogicalOr()
{
	return ParseBinary(&Parser::ParseLogicalAnd, Token::Name::DOUBLE_BAR);
}

MarmotResult::ExpressionResult Parser::ParsePipe()
{
	return FoldWhile
	(
		ParseBitwiseOr(),
		[this]() { return Match(Token::Name::BAR_BRACKET); },
		[this](std::unique_ptr<MarmotExpression>&& left) -> MarmotResult::ExpressionResult
		{
			const Token pipe_op = Previous();
			if (Match(Token::Name::MATCH))
			{
				Token& match_keyword = Previous();
				return ParseMatchExpressionWithScrutinee(match_keyword, std::move(left));
			}

			m_state.m_next_function_is_pipe_stage = Check(Token::Name::FUNCTION, 0);
			return ParseBitwiseOr().transform
			(
				[&](std::unique_ptr<MarmotExpression>&& right)
				{
					// Prepending at parse time lets the piped value participate in generic
					// inference alongside written arguments. Constructions own a parameter
					// list separate from a call's.
					if (right->IsExpression<MarmotExpression::Call>())
					{
						MarmotExpression::Call& call = right->GetExpression<MarmotExpression::Call>();
						call.m_arguments.insert(call.m_arguments.begin(), std::move(left));
						return std::move(right);
					}
					if (right->IsExpression<MarmotExpression::Construct>())
					{
						MarmotExpression::Construct& construct = right->GetExpression<MarmotExpression::Construct>();
						construct.m_params.insert(construct.m_params.begin(), std::move(left));
						return std::move(right);
					}

					std::vector<std::unique_ptr<MarmotExpression>> arguments;
					arguments.emplace_back(std::move(left));
					return std::make_unique<MarmotExpression>(MarmotExpression::Call(pipe_op, std::move(right), std::move(arguments)));
				}
			);
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
	// 352 .mmt files in test/, projects/marmotstdlib/MarmotPrelude/, benchmarks/, examples/ and the unit tests
	// misclassifies 0 braces with the counter and 51 without it - most of
	// projects/marmotstdlib/MarmotPrelude/Prelude/Result.mmt and Option.mmt among them.
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
		if (offset > MmcWire::MAX_ARRAY_SIZE)
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
	return InScope
	(
		[&]() -> MarmotResult::ExpressionResult
		{
			std::function<MarmotResult::ExpressionResult(std::vector<std::unique_ptr<MarmotStatement>>&&, std::unique_ptr<MarmotExpression>&&)> build_block = [this](std::vector<std::unique_ptr<MarmotStatement>>&& stmts, std::unique_ptr<MarmotExpression>&& final_expr) -> MarmotResult::ExpressionResult
				{
					return Consume(Token::Name::RIGHT_BRACE, "Expected '}' after block expression.")
						.and_then
						(
							[&stmts, &final_expr](Token&& right_brace)
							{
								return final_expr != nullptr
									? MarmotResult::ExpressionResult(std::make_unique<MarmotExpression>(MarmotExpression::Block(right_brace, std::move(stmts), std::move(final_expr))))
									: MarmotResult::ExpressionResult(std::make_unique<MarmotExpression>(MarmotExpression::Block(right_brace, std::move(stmts))));
							}
						);
				};

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
				return InScope
				(
					[&]() -> MarmotResult::ExpressionResult
					{
						const BindingId loop_binding = RegisterLocalBinding(loop_variable.m_lexeme).value_or(nullptr);

						return ParseExpression()
							.and_then
							(
								[&for_keyword, &loop_variable, &in_keyword, range = std::move(range), loop_binding](std::unique_ptr<MarmotExpression>&& body) mutable ->MarmotResult::ExpressionResult
								{
									std::unique_ptr<MarmotExpression> for_expr = std::make_unique<MarmotExpression>(MarmotExpression::For(for_keyword, loop_variable, in_keyword, std::move(range), std::move(body)));
									MarmotExpression::For& for_expr_ref = for_expr->GetExpression<MarmotExpression::For>();
									for_expr_ref.m_loop_binding = loop_binding;
									return for_expr;
								}
							);
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
		if (offset > MmcWire::MAX_ARRAY_SIZE)
		{
			return {};
		}
	}
}

MarmotResult::ExpressionResult Parser::ParseArrayComprehension(Token& bracket, const ArrayComprehensionProbe& probe)
{
	const auto parse = [&]() -> MarmotResult::ExpressionResult
	{
		BindingId loop_binding;

		if (probe.m_loop_variable_offset.has_value())
		{
			Token loop_variable = Peek(probe.m_loop_variable_offset.value());

			// The transform is parsed first, so its loop binding must already exist.
			loop_binding = RegisterLocalBinding(loop_variable.m_lexeme).value_or(nullptr);
		}

		return ParseExpression()
			.and_then
			(
				[&bracket, loop_binding, this](std::unique_ptr<MarmotExpression>&& transform_expr) -> MarmotResult::ExpressionResult
				{
					if (!Match(Token::Name::FOR))
					{
						return std::unexpected(GenerateParserError("Expected 'for' in array comprehension. Use '[expr for item in range]' syntax.", Peek(0)));
					}

					if (!Match(Token::Name::IDENTIFIER_LITERAL))
					{
						return std::unexpected(GenerateParserError("Expected identifier after 'for' in array comprehension.", Peek(0)));
					}
					Token actual_loop_var = Previous();

					if (!Match(Token::Name::IN))
					{
						return std::unexpected(GenerateParserError("Expected 'in' after loop variable in array comprehension. Use '[expr for item in range]' syntax.", Peek(0)));
					}
					Token in_keyword = Previous();

					return ParseExpression()
						.and_then
						(
							[&bracket, &actual_loop_var, &in_keyword, loop_binding, transform_expr = std::move(transform_expr), this](std::unique_ptr<MarmotExpression>&& range) mutable -> MarmotResult::ExpressionResult
							{
								if (!Match(Token::Name::RIGHT_BRACKET))
								{
									return std::unexpected(GenerateParserError("Expected ']' after array comprehension.", Peek(0)));
								}

								std::unique_ptr<MarmotExpression> comp_expr = std::make_unique<MarmotExpression>(MarmotExpression::ArrayComprehension(bracket, actual_loop_var, in_keyword, std::move(transform_expr), std::move(range)));

								MarmotExpression::ArrayComprehension& comp_ref = comp_expr->GetExpression<MarmotExpression::ArrayComprehension>();
								comp_ref.m_loop_binding = loop_binding;

								return comp_expr;
							}
						);
				}
			);
	};
	return probe.m_loop_variable_offset.has_value() ? InScope(parse) : parse();
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
	return WithGenericParameters
	(
		[&](std::vector<Token> generic_params, std::vector<std::shared_ptr<MarmotType>>) -> MarmotResult::ExpressionResult
		{
			return InScope
			(
				[&]() -> MarmotResult::ExpressionResult
				{
					MarmotResult::TokenResult left_paren_result = Consume(Token::Name::LEFT_PAREN, "Expected '(' before function parameters.");
					if (!left_paren_result.has_value())
					{
						return std::unexpected(left_paren_result.error());
					}

					m_state.m_function = std::make_shared<LexicalFunction>();

					MarmotResult::FunctionParamsResult params_parse_result = ParseFunctionParameters(true);
					if (!params_parse_result.has_value())
					{
						return std::unexpected(params_parse_result.error());
					}

					std::vector<std::pair<Token, std::shared_ptr<MarmotType>>> param_tuples = std::move(params_parse_result.value());
					ParamSplit split = SplitParamTuples(std::move(param_tuples));
					FunctionParameters params = ParametersFor(std::move(split.m_params));
					std::vector<std::shared_ptr<MarmotType>> param_types = std::move(split.m_types);

					std::shared_ptr<MarmotType> return_type = MarmotType::MakeUndecidedType();
					if (Match(Token::Name::THIN_ARROW))
					{
						MarmotResult::TypeResult return_type_result = ParseType();
						if (!return_type_result.has_value())
						{
							return std::unexpected(return_type_result.error());
						}

						return_type = std::move(return_type_result.value());
					}

					// Parse optional 'where' clauses. A where clause may name constraints the
					// signature never mentions, so they must be recorded explicitly.
					std::vector<MarmotType::ClassConstraint> constraints;
					if (Match(Token::Name::WHERE))
					{
						std::expected<std::vector<MarmotType::ClassConstraint>, CompilerError> constraints_result = ParseClassConstraints();
						if (!constraints_result.has_value())
						{
							return std::unexpected(constraints_result.error());
						}

						constraints = std::move(constraints_result.value());
					}

					MarmotResult::TokenResult fat_arrow_result = Consume(Token::Name::FAT_ARROW, "Expected '=>' before function body.");
					if (!fat_arrow_result.has_value())
					{
						return std::unexpected(fat_arrow_result.error());
					}

					std::vector<MarmotType::ClassConstraint> propagated_constraints = CollectSignatureConstraints(param_types, return_type);
					for (MarmotType::ClassConstraint& propagated_constraint : propagated_constraints)
					{
						AppendUniqueConstraint(constraints, std::move(propagated_constraint));
					}
					PushActiveConstraints(constraints);

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
						return std::unexpected(body_result.error());
					}

					return std::make_unique<MarmotExpression>(MarmotExpression::Function(keyword, std::move(generic_params), std::move(params), std::move(param_types), std::move(return_type), std::move(body_result.value()), std::move(constraints)));
				}
			);
		}
	);
}

MarmotResult::ExpressionResult Parser::ParseCaseExpression(Token& keyword)
{
	return InScope
	(
		[&]() -> MarmotResult::ExpressionResult
		{
			MarmotResult::PatternResult pattern_result = ParsePattern();
			if (!pattern_result.has_value())
			{
				return std::unexpected(std::move(pattern_result.error()));
			}

			std::unique_ptr<MarmotPattern> pattern = std::move(pattern_result.value());

			std::unique_ptr<MarmotExpression> guard = nullptr;
			if (Match(Token::Name::IF))
			{
				MarmotResult::ExpressionResult guard_result = ParseExpression();
				if (!guard_result.has_value())
				{
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
								[&keyword, &pattern, &guard](std::unique_ptr<MarmotExpression>&& case_expr)->MarmotResult::ExpressionResult
								{
									return std::make_unique<MarmotExpression>(MarmotExpression::Case(keyword, std::move(pattern), std::move(case_expr), std::move(guard)));
								}
							);
					}
				);
		}
	);
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
							return DefineBinding(param_name)
								.and_then
								(
									[this, allow_inferred_types](Token&& param_name) -> MarmotResult::FunctionParamResult
									{
										auto finish_param = [&param_name, this](std::shared_ptr<MarmotType>&& type) -> MarmotResult::FunctionParamResult
										{
											RegisterLocalBinding(param_name.m_lexeme);
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
