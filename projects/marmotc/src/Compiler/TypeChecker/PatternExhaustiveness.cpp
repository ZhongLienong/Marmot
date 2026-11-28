#include "TypeChecker.h"
#include "PatternCoverage.h"
#include "Compiler/Constant/Constant.h"
#include "Support/Expected.h"
#include "Support/Optional.h"
#include <algorithm>
#include <format>
#include <ranges>
#include <type_traits>

using namespace std::string_literals;

namespace
{
	template <typename>
	inline constexpr bool AlwaysFalse = false;

	const Token& GetPatternToken(const MarmotPattern& pattern)
	{
		return std::visit
		(
			[]<typename T>(const T& node) -> const Token&
			{
				using Node = std::decay_t<T>;
				if constexpr (std::is_same_v<Node, MarmotPattern::Binding>)
				{
					return node.m_name;
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Wildcard>)
				{
					return node.m_token;
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Literal>)
				{
					return node.m_token;
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Tuple>)
				{
					return node.m_left_paren;
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Array>)
				{
					return node.m_left_bracket;
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Constructor>)
				{
					return node.m_name_token;
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::As>)
				{
					return GetPatternToken(*node.m_pattern);
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Or>)
				{
					return GetPatternToken(*node.m_alternatives.front());
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Range>)
				{
					return node.m_start;
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Record>)
				{
					return node.m_name_token;
				}
				else
				{
					static_assert(AlwaysFalse<Node>, "Unhandled pattern type.");
					return node.m_name_token;
				}
			},
			*pattern
		);
	}
}

MarmotResult::TypeResult TypeChecker::CheckPattern(const CheckingContext& context, MarmotPattern& pattern, const std::shared_ptr<MarmotType>& expected_type)
{
	std::shared_ptr<MarmotType> resolved_expected = ApplySubstitution(context, expected_type);

	return std::visit
	(
		[&]<typename T>(T&& node) -> MarmotResult::TypeResult
		{
			using Node = std::decay_t<T>;
			if constexpr (std::is_same_v<Node, MarmotPattern::Binding>)
			{
				node.m_type_data = resolved_expected;
				BindValue(context, node.m_name, node.m_binding, resolved_expected);
				return node.m_type_data;
			}
			else if constexpr (std::is_same_v<Node, MarmotPattern::Wildcard>)
			{
				node.m_type_data = resolved_expected;
				return node.m_type_data;
			}
			else if constexpr (std::is_same_v<Node, MarmotPattern::Literal>)
			{
				MarmotResult::TypeResult type = CheckLiteral(node, resolved_expected);
				if (!type.HasValue())
				{
					return type;
				}
				node.m_type_data = std::move(*type);

				return Unify(context, node.m_token, node.m_type_data, resolved_expected, UnifyDiagnosticMode::ActualExpected)
					.AndThen([&node](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult { return node.m_type_data; });
			}
			else if constexpr (std::is_same_v<Node, MarmotPattern::Tuple>)
			{
				if (!resolved_expected->IsType<MarmotType::TupleType>())
				{
					return MarmotCompiler::Failure(MarmotError::GenerateTypeCheckerErrorWithContext("Pattern type error: expected tuple type", node.m_left_paren, m_file_name, m_source_lines, resolved_expected));
				}

				const MarmotType::TupleType& tuple_type = resolved_expected->GetType<MarmotType::TupleType>();
				if (tuple_type.m_element_types.size() != node.m_elements.size())
				{
					return MarmotCompiler::Failure(MarmotError::GenerateTypeCheckerErrorWithContext("Pattern type error: tuple arity mismatch", node.m_left_paren, m_file_name, m_source_lines));
				}

				for (size_t i = 0u; i < node.m_elements.size(); i += 1u)
				{
					MarmotResult::TypeResult elem_result = CheckPattern(context, *node.m_elements[i], tuple_type.m_element_types[i]);
					if (!elem_result.HasValue())
					{
						return MarmotCompiler::Failure(std::move(elem_result.Error()));
					}
				}

				node.m_type_data = resolved_expected;
				return node.m_type_data;
			}
			else if constexpr (std::is_same_v<Node, MarmotPattern::Array>)
			{
				if (!resolved_expected->IsType<MarmotType::ArrayType>())
				{
					return MarmotCompiler::Failure(MarmotError::GenerateTypeCheckerErrorWithContext("Pattern type error: expected array type", node.m_left_bracket, m_file_name, m_source_lines, resolved_expected));
				}

				const MarmotType::ArrayType& array_type = resolved_expected->GetType<MarmotType::ArrayType>();
				for (const std::unique_ptr<MarmotPattern>& elem : node.m_elements)
				{
					MarmotResult::TypeResult elem_result = CheckPattern(context, *elem, array_type.m_element_type);
					if (!elem_result.HasValue())
					{
						return MarmotCompiler::Failure(std::move(elem_result.Error()));
					}
				}

				if (node.m_rest.HasValue())
				{
					MarmotResult::TypeResult rest_result = CheckPattern(context, *node.m_rest->m_pattern, resolved_expected);
					if (!rest_result.HasValue())
					{
						return MarmotCompiler::Failure(std::move(rest_result.Error()));
					}
				}

				node.m_type_data = resolved_expected;
				return node.m_type_data;
			}
			else if constexpr (std::is_same_v<Node, MarmotPattern::Constructor>)
			{
				if (!resolved_expected->IsType<MarmotType::UnionType>())
				{
					return MarmotCompiler::Failure(MarmotError::GenerateTypeCheckerErrorWithContext("Pattern type error: expected union type", node.m_name_token, m_file_name, m_source_lines, resolved_expected));
				}

				const MarmotType::UnionType& union_type = resolved_expected->GetType<MarmotType::UnionType>();
				if (!union_type.m_member_info.contains(node.m_name))
				{
					return MarmotCompiler::Failure(MarmotError::GenerateTypeCheckerErrorWithContext(std::format("Pattern type error: unrecognized union member '{}'", node.m_name), node.m_name_token, m_file_name, m_source_lines));
				}

				const MarmotType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(node.m_name);
				if (member_ctx.m_member_types.size() != node.m_args.size())
				{
					return MarmotCompiler::Failure(MarmotError::GenerateTypeCheckerErrorWithContext("Pattern type error: incorrect union arity", node.m_name_token, m_file_name, m_source_lines));
				}

				node.m_tag = member_ctx.m_tag;
				for (size_t i = 0u; i < node.m_args.size(); i += 1u)
				{
					MarmotResult::TypeResult arg_result = CheckPattern(context, *node.m_args[i], member_ctx.m_member_types[i]);
					if (!arg_result.HasValue())
					{
						return MarmotCompiler::Failure(std::move(arg_result.Error()));
					}
				}

				node.m_type_data = resolved_expected;
				return node.m_type_data;
			}
			else if constexpr (std::is_same_v<Node, MarmotPattern::As>)
			{
				return CheckPattern(context, *node.m_pattern, resolved_expected)
					.AndThen([&](std::shared_ptr<MarmotType>&&) { return CheckPattern(context, *node.m_binding, resolved_expected); })
					.Transform
					(
						[&node, &resolved_expected](std::shared_ptr<MarmotType>&&)
						{
							node.m_type_data = resolved_expected;
							return node.m_type_data;
						}
					);
			}
			else if constexpr (std::is_same_v<Node, MarmotPattern::Record>)
			{
				if (!resolved_expected->IsType<MarmotType::StructType>() || resolved_expected->GetType<MarmotType::StructType>().m_name != node.m_name)
				{
					return MarmotCompiler::Failure(MarmotError::GenerateTypeCheckerErrorWithContext("Pattern type error: expected struct type", node.m_name_token, m_file_name, m_source_lines, resolved_expected));
				}

				const MarmotType::StructType& struct_type = resolved_expected->GetType<MarmotType::StructType>();
				std::vector<bool> is_matched(struct_type.m_member_names.size(), false);
				for (MarmotPattern::Record::Field& field : node.m_fields)
				{
					const std::vector<std::string>::const_iterator found = std::ranges::find(struct_type.m_member_names, field.m_name.m_lexeme);
					if (found == struct_type.m_member_names.cend())
					{
						const std::string suggestion = std::format("Struct '{}' does not have a member named '{}'", struct_type.m_name, field.m_name.m_lexeme);
						return MarmotCompiler::Failure(MarmotError::GenerateTypeCheckerErrorWithContext("Unknown struct member in record pattern", field.m_name, m_file_name, m_source_lines, MarmotCompiler::Optional<std::string_view>(suggestion)));
					}

					field.m_index = static_cast<int>(std::distance(struct_type.m_member_names.cbegin(), found));
					if (is_matched[static_cast<size_t>(field.m_index)])
					{
						return MarmotCompiler::Failure(MarmotError::GenerateTypeCheckerErrorWithContext(std::format("Pattern type error: '{}' is matched twice in this record pattern", field.m_name.m_lexeme), field.m_name, m_file_name, m_source_lines));
					}
					is_matched[static_cast<size_t>(field.m_index)] = true;

					MarmotResult::TypeResult field_result = CheckPattern(context, *field.m_pattern, struct_type.m_member_types[static_cast<size_t>(field.m_index)]);
					if (!field_result.HasValue())
					{
						return field_result;
					}
				}

				const std::vector<std::string> left_out = std::views::zip(struct_type.m_member_names, is_matched)
					| std::views::filter([](const std::tuple<const std::string&, bool>& member) { return !std::get<1>(member); })
					| std::views::transform([](const std::tuple<const std::string&, bool>& member) { return std::format("'{}'", std::get<0>(member)); })
					| std::ranges::to<std::vector>();
				if (!node.m_rest.HasValue() && !left_out.empty())
				{
					return MarmotCompiler::Failure(MarmotError::GenerateTypeCheckerErrorWithContext(std::format("Pattern type error: this record pattern leaves out {}; match each, as in 'field = _', or end with '..' to leave them out", left_out | std::views::join_with(std::string_view(", ")) | std::ranges::to<std::string>()), node.m_name_token, m_file_name, m_source_lines));
				}
				if (node.m_rest.HasValue() && left_out.empty())
				{
					return MarmotCompiler::Failure(MarmotError::GenerateTypeCheckerErrorWithContext("Pattern type error: every field is matched already, so '..' leaves nothing out", node.m_rest.Value(), m_file_name, m_source_lines));
				}

				node.m_type_data = resolved_expected;
				return node.m_type_data;
			}
			else if constexpr (std::is_same_v<Node, MarmotPattern::Range>)
			{
				node.m_type_data = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
				return Unify(context, node.m_start, node.m_type_data, resolved_expected, UnifyDiagnosticMode::ActualExpected)
					.AndThen([&node](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult { return node.m_type_data; });
			}
			else if constexpr (std::is_same_v<Node, MarmotPattern::Or>)
			{
				// Each alternative binds the same names; the first decides their types and
				// every later one must agree with it.
				MarmotResult::TypeResult first = CheckPattern(context, *node.m_alternatives.front(), resolved_expected);
				if (!first.HasValue())
				{
					return first;
				}
				std::unordered_map<BindingId, std::shared_ptr<MarmotType>>& names = context.m_scope->m_bindings;
				const std::vector<std::tuple<BindingId, std::string, std::shared_ptr<MarmotType>>> bound = node.m_alternatives.front()->Bindings(false)
					| std::views::transform([&names](const MarmotPattern::Binding* binding) { return std::tuple{ binding->m_binding.Value(), binding->m_name.m_lexeme, names.at(binding->m_binding.Value()) }; })
					| std::ranges::to<std::vector>();

				for (std::unique_ptr<MarmotPattern>& alternative : node.m_alternatives | std::views::drop(1))
				{
					MarmotResult::TypeResult checked = CheckPattern(context, *alternative, resolved_expected);
					if (!checked.HasValue())
					{
						return checked;
					}
					for (const auto& [binding, name, type] : bound)
					{
						std::shared_ptr<MarmotType> resolved_type = ApplySubstitution(context, type);
						std::shared_ptr<MarmotType> alternative_type = ApplySubstitution(context, names.at(binding));
						if (!Unify(context, GetPatternToken(*alternative), resolved_type, alternative_type).HasValue())
						{
							return MarmotCompiler::Failure(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, std::format("Pattern type error: '{}' has a different type in this alternative than in the first", name), GetPatternToken(*alternative), m_file_name, m_source_lines, alternative_type, resolved_type));
						}
						names[binding] = type;
					}
				}

				node.m_type_data = resolved_expected;
				return node.m_type_data;
			}
			else
			{
				return MarmotCompiler::Failure(MarmotError::GenerateTypeCheckerErrorWithContext("Pattern type error: unsupported pattern", GetPatternToken(pattern), m_file_name, m_source_lines));
			}
		},
		*pattern
	);
}

void TypeChecker::ResolveRecordedTypes(const CheckingContext& context, MarmotPattern& pattern)
{
	pattern.GetType() = ApplySubstitution(context, pattern.GetType());
	const auto resolve_all = [this, &context](std::vector<std::unique_ptr<MarmotPattern>>& children)
	{
		std::ranges::for_each(children, [this, &context](std::unique_ptr<MarmotPattern>& child) { ResolveRecordedTypes(context, *child); });
	};
	if (pattern.IsPattern<MarmotPattern::Tuple>())
	{
		resolve_all(pattern.GetPattern<MarmotPattern::Tuple>().m_elements);
	}
	else if (pattern.IsPattern<MarmotPattern::Array>())
	{
		MarmotPattern::Array& array = pattern.GetPattern<MarmotPattern::Array>();
		resolve_all(array.m_elements);
		if (array.m_rest.HasValue())
		{
			ResolveRecordedTypes(context, *array.m_rest->m_pattern);
		}
	}
	else if (pattern.IsPattern<MarmotPattern::Constructor>())
	{
		resolve_all(pattern.GetPattern<MarmotPattern::Constructor>().m_args);
	}
	else if (pattern.IsPattern<MarmotPattern::As>())
	{
		MarmotPattern::As& as = pattern.GetPattern<MarmotPattern::As>();
		ResolveRecordedTypes(context, *as.m_pattern);
		ResolveRecordedTypes(context, *as.m_binding);
	}
	else if (pattern.IsPattern<MarmotPattern::Or>())
	{
		resolve_all(pattern.GetPattern<MarmotPattern::Or>().m_alternatives);
	}
	else if (pattern.IsPattern<MarmotPattern::Record>())
	{
		std::ranges::for_each(pattern.GetPattern<MarmotPattern::Record>().m_fields, [this, &context](MarmotPattern::Record::Field& field) { ResolveRecordedTypes(context, *field.m_pattern); });
	}
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::Match& match)
{
	// The type demanded of the match is its cases' type, not the scrutinee's.

	return Evaluate(context.WithExpectedType(nullptr), match.m_arg_expr)
		.AndThen
		(
			[&match, this, &context](std::shared_ptr<MarmotType>&& arg_type) -> MarmotResult::TypeResult
			{
				std::shared_ptr<MarmotType> resolved_arg_type = ApplySubstitution(context, arg_type);
				std::shared_ptr<MarmotType> prev_case_type = nullptr;
				PatternCoverage coverage;

				for (const std::unique_ptr<MarmotExpression>& case_expr : match.m_cases)
				{
					TypeScope scope(context.m_scope);
					const CheckingContext scope_context = context.WithScope(scope);
					MarmotExpression::Case& match_case = case_expr->GetExpression<MarmotExpression::Case>();

					// Before the case's own pattern is checked: a match in an arm without
					// braces takes the enclosing match's later cases, whose patterns are
					// likely of another type, and this is the error that says why.
					if (&case_expr != &match.m_cases.front() && coverage.IsExhaustive())
					{
						return MarmotCompiler::Failure(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnreachableCase, "The cases above already match every value, so this one can never run. If it belongs to an enclosing match, put the match above it in braces.", match_case.m_keyword, m_file_name, m_source_lines));
					}

					MarmotResult::TypeResult case_result = CheckPattern(scope_context, *match_case.m_pattern, resolved_arg_type)
						.AndThen
						(
							[&](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
							{
								const MarmotCompiler::Optional<CompilerError> error = CheckCaseReachable(coverage, match_case);
								if (error.HasValue())
								{
									return MarmotCompiler::Failure(error.Value());
								}
								return {};
							}
						)
						.AndThen
						(
							[&](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
							{
								const MarmotCompiler::Optional<CompilerError> error = match_case.HasGuard() ? CheckCaseGuard(scope_context, match_case) : MarmotCompiler::None;
								if (error.HasValue())
								{
									return MarmotCompiler::Failure(error.Value());
								}
								return Evaluate(scope_context, match_case.m_expr);
							}
						);

					if (!case_result.HasValue())
					{
						return MarmotCompiler::Failure(std::move(case_result.Error()));
					}

					// A guarded case may fail its guard, so it covers nothing.
					if (!match_case.HasGuard())
					{
						coverage.Add(*match_case.m_pattern);
					}

					std::shared_ptr<MarmotType> resolved_case_type = ApplySubstitution(scope_context, case_result.Value());
					if (prev_case_type == nullptr)
					{
						prev_case_type = resolved_case_type;
					}
					else
					{
						std::shared_ptr<MarmotType> resolved_prev_case_type = ApplySubstitution(scope_context, prev_case_type);
						MarmotResult::TypeResult unify_result = Unify(scope_context, match.m_match_keyword, resolved_prev_case_type, resolved_case_type, UnifyDiagnosticMode::CommonType);
						if (!unify_result.HasValue())
						{
							return MarmotCompiler::Failure(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, "Match expression type error: case types do not match", match.m_match_keyword, m_file_name, m_source_lines, resolved_prev_case_type, resolved_case_type));
						}

						// What unification gives back, not the previous arm's type: an arm
						// that never finishes takes the type of the arms that do.
						prev_case_type = ApplySubstitution(scope_context, unify_result.Value());
					}
				}

				const std::vector<std::string> unmatched = coverage.FindUnmatched();

				if (unmatched == std::vector<std::string>{ "_" })
				{
					return MarmotCompiler::Failure(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeNonExhaustiveMatch, "Match expression type error: non-union matches need a catch-all arm such as 'case _ =>'", match.m_match_keyword, m_file_name, m_source_lines));
				}
				if (!unmatched.empty())
				{
					const std::string message = std::format("Match expression type error: non-exhaustive match: no case matches {}", unmatched | std::views::join_with(std::string_view(", ")) | std::ranges::to<std::string>());
					return MarmotCompiler::Failure(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeNonExhaustiveMatch, message, match.m_match_keyword, m_file_name, m_source_lines));
				}

				match.m_type_data = prev_case_type;
				return match.m_type_data;
			}
		);
}

// A case, and each alternative of every or-pattern in it, must match some value
// nothing before it does.
MarmotCompiler::Optional<CompilerError> TypeChecker::CheckCaseReachable(const PatternCoverage& coverage, const MarmotExpression::Case& match_case)
{
	if (!coverage.IsUseful(*match_case.m_pattern))
	{
		return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnreachableCase, "The cases above already match every value this one does, so it can never run.", match_case.m_keyword, m_file_name, m_source_lines);
	}

	const MarmotPattern* alternative = coverage.FindUnreachableAlternative(*match_case.m_pattern);
	if (alternative != nullptr)
	{
		return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnreachableCase, "The cases and alternatives before this one already match every value it does, so it can never match.", GetPatternToken(*alternative), m_file_name, m_source_lines);
	}
	return MarmotCompiler::None;
}

MarmotCompiler::Optional<CompilerError> TypeChecker::CheckCaseGuard(const CheckingContext& context, MarmotExpression::Case& case_expr)
{
	MarmotResult::TypeResult guard_result = Evaluate(context, case_expr.m_guard.Value());
	if (!guard_result.HasValue())
	{
		return std::move(guard_result.Error());
	}

	std::shared_ptr<MarmotType> bool_type = MarmotType::MakeLiteralType<MarmotType::BoolType>();
	std::shared_ptr<MarmotType> resolved_guard_type = ApplySubstitution(context, guard_result.Value());
	if (!Unify(context, case_expr.m_keyword, bool_type, resolved_guard_type).HasValue())
	{
		return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeMismatch, "Match expression type error: case guard must be of type Bool", case_expr.m_keyword, m_file_name, m_source_lines, resolved_guard_type, bool_type);
	}

	return MarmotCompiler::None;
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::Case& case_expr)
{
	if (case_expr.HasGuard())
	{
		MarmotCompiler::Optional<CompilerError> guard_error = CheckCaseGuard(context, case_expr);
		if (guard_error.HasValue())
		{
			return MarmotCompiler::Failure(std::move(guard_error.Value()));
		}
	}

	return Evaluate(context, case_expr.m_expr)
		.AndThen
		(
			[&case_expr, this, &context](std::shared_ptr<MarmotType>&& expr_type) ->MarmotResult::TypeResult
			{
				return Unify(context, case_expr.m_keyword, case_expr.m_type_data, expr_type);
			}
		);
}
