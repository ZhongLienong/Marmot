#include "AbstractSyntaxTree.h"

#include <charconv>
#include <limits>

std::optional<uint64_t> ParseUnsignedLiteral(std::string_view lexeme)
{
	int base = 10;
	if (lexeme.size() >= 3u && lexeme[0u] == '0' && (lexeme[1u] == 'x' || lexeme[1u] == 'X'))
	{
		base = 16;
		lexeme.remove_prefix(2u);
	}
	else if (lexeme.size() >= 3u && lexeme[0u] == '0' && (lexeme[1u] == 'b' || lexeme[1u] == 'B'))
	{
		base = 2;
		lexeme.remove_prefix(2u);
	}

	uint64_t value = 0u;
	const std::from_chars_result result = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), value, base);
	if (lexeme.empty() || result.ec != std::errc() || result.ptr != lexeme.data() + lexeme.size())
	{
		return std::nullopt;
	}

	return value;
}

std::optional<int64_t> ParseIntegerLiteral(std::string_view lexeme)
{
	if (!lexeme.empty() && lexeme[0u] == '-')
	{
		int64_t value = 0;
		const std::from_chars_result result = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), value);
		if (result.ec != std::errc() || result.ptr != lexeme.data() + lexeme.size())
		{
			return std::nullopt;
		}

		return value;
	}

	const std::optional<uint64_t> value = ParseUnsignedLiteral(lexeme);
	if (!value.has_value() || value.value() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
	{
		return std::nullopt;
	}

	return static_cast<int64_t>(value.value());
}

std::optional<double> ParseFloatLiteral(std::string_view lexeme)
{
	double value = 0.0;
	const std::from_chars_result result = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), value);
	if (lexeme.empty() || result.ec != std::errc() || result.ptr != lexeme.data() + lexeme.size())
	{
		return std::nullopt;
	}

	return value;
}

namespace
{
	struct TypeDataAccessor
	{
		template<typename T>
		std::shared_ptr<MarmotType>& operator()(T& arg) const
		{
			return arg.m_type_data;
		}
	};

	struct ConstTypeDataAccessor
	{
		template<typename T>
		const std::shared_ptr<MarmotType>& operator()(const T& arg) const
		{
			return arg.m_type_data;
		}
	};

	std::optional<std::unique_ptr<MarmotExpression>> MakeOptionalExpression(std::unique_ptr<MarmotExpression>&& expr)
	{
		if (expr == nullptr)
		{
			return std::nullopt;
		}

		return std::optional<std::unique_ptr<MarmotExpression>>(std::move(expr));
	}
}

MarmotPattern::Binding::Binding(const Token& name, std::optional<BindingId> binding)
	: m_name(name),
	m_binding(std::move(binding))
{
}

MarmotPattern::Binding::Binding(const GeneratedBinding& binding)
	: m_name(binding.m_anchor),
	m_binding(binding.m_binding),
	m_is_generated(true)
{
}

MarmotPattern::Wildcard::Wildcard(const Token& token)
	: m_token(token)
{
}

MarmotPattern::Literal::Literal(const Token& token, LiteralKind kind)
	: m_token(token),
	m_kind(kind)
{
}

MarmotPattern::Tuple::Tuple(const Token& left_paren, std::vector<std::unique_ptr<MarmotPattern>>&& elements)
	: m_left_paren(left_paren),
	m_elements(std::move(elements))
{
}

MarmotPattern::Array::Rest::Rest(size_t position, std::unique_ptr<MarmotPattern>&& pattern)
	: m_position(position),
	m_pattern(std::move(pattern))
{
}

MarmotPattern::Array::Array(const Token& left_bracket, std::vector<std::unique_ptr<MarmotPattern>>&& elements, std::optional<Rest>&& rest)
	: m_left_bracket(left_bracket),
	m_elements(std::move(elements)),
	m_rest(std::move(rest))
{
}

MarmotPattern::Constructor::Constructor(const Token& name_token, std::string&& name, std::vector<std::unique_ptr<MarmotPattern>>&& args, bool is_union)
	: m_name_token(name_token),
	m_name(std::move(name)),
	m_args(std::move(args)),
	m_is_union(is_union)
{
}

MarmotPattern::As::As(std::unique_ptr<MarmotPattern>&& pattern, std::unique_ptr<MarmotPattern>&& binding)
	: m_pattern(std::move(pattern)),
	m_binding(std::move(binding))
{
}

MarmotPattern::Or::Or(std::vector<std::unique_ptr<MarmotPattern>>&& alternatives)
	: m_alternatives(std::move(alternatives))
{
}

MarmotPattern::Range::Range(const Token& start, int64_t low, int64_t high)
	: m_start(start),
	m_low(low),
	m_high(high)
{
}

MarmotPattern::Record::Field::Field(const Token& name, std::unique_ptr<MarmotPattern>&& pattern)
	: m_name(name),
	m_pattern(std::move(pattern))
{
}

MarmotPattern::Record::Record(const Token& name_token, std::string&& name, std::vector<Field>&& fields, std::optional<Token>&& rest)
	: m_name_token(name_token),
	m_name(std::move(name)),
	m_fields(std::move(fields)),
	m_rest(std::move(rest))
{
}

MarmotExpression::As::As(const Token& as_keyword, std::shared_ptr<MarmotType> to_type, std::unique_ptr<MarmotExpression>&& expr)
	: m_as_keyword(as_keyword),
	m_to_type(std::move(to_type)),
	m_expr(std::move(expr))
{
}

MarmotExpression::Binary::Binary(const Token& op, std::unique_ptr<MarmotExpression>&& left, std::unique_ptr<MarmotExpression>&& right)
	: m_op(op),
	m_left(std::move(left)),
	m_right(std::move(right))
{
}

MarmotExpression::Group::Group(std::unique_ptr<MarmotExpression>&& expr_in)
	: m_expr_in(std::move(expr_in))
{
}

MarmotExpression::Tuple::Tuple(const Token& op, std::vector<std::unique_ptr<MarmotExpression>>&& elements)
	: m_op(op), m_elements(std::move(elements))
{
}

MarmotExpression::Literal::Literal(const Token& token, LiteralKind kind)
	: m_token(token),
	m_kind(kind)
{
}

MarmotExpression::UnaryPrefix::UnaryPrefix(const Token& op, std::unique_ptr<MarmotExpression>&& expr)
	: m_op(op),
	m_expr(std::move(expr))
{
}

MarmotExpression::NameAccess::NameAccess(const Token& name, NameContext::Tag&& name_ctx)
	: m_name(name),
	m_name_ctx(std::move(name_ctx))
{
}

MarmotExpression::NameAccess::NameAccess(const GeneratedBinding& binding)
	: m_name(binding.m_anchor),
	m_name_ctx(NameContext::Local{ binding.m_binding, true })
{
}

MarmotExpression::Call::Call(const Token& paren, std::unique_ptr<MarmotExpression>&& callee, std::vector<std::unique_ptr<MarmotExpression>>&& arguments, bool is_foreign)
	: m_paren(paren),
	m_callee(std::move(callee)),
	m_arguments(std::move(arguments)),
	m_is_foreign(is_foreign)
{
}

MarmotExpression::Function::Function(const Token& function_keyword, std::vector<Token>&& generic_params, FunctionParameters&& params, std::vector<std::shared_ptr<MarmotType>>&& param_types, std::shared_ptr<MarmotType>&& return_type, std::unique_ptr<MarmotExpression>&& body, std::vector<MarmotType::ClassConstraint>&& constraints)
	: m_function_keyword(function_keyword),
	m_generic_params(std::move(generic_params)),
	m_params(std::move(params)),
	m_param_types(std::move(param_types)),
	m_constraints(std::move(constraints)),
	m_return_type(std::move(return_type)),
	m_body(std::move(body))
{
}

MarmotExpression::Construct::Construct(const Token& data_name, std::vector<std::unique_ptr<MarmotExpression>>&& params, std::shared_ptr<MarmotType>&& return_type, bool has_explicit_type_args, ConstructContext&& construct_ctx)
	: m_data_name(data_name),
	m_params(std::move(params)),
	m_return_type(std::move(return_type)),
	m_has_explicit_type_args(has_explicit_type_args),
	m_construct_ctx(std::move(construct_ctx))
{
}

MarmotExpression::RecordUpdate::FieldUpdate::FieldUpdate(std::vector<Token>&& path, std::unique_ptr<MarmotExpression>&& value)
	: m_path(std::move(path)),
	m_value(std::move(value))
{
}

MarmotExpression::RecordUpdate::RecordUpdate(const Token& with_keyword, std::unique_ptr<MarmotExpression>&& source, std::vector<FieldUpdate>&& updates)
	: m_with_keyword(with_keyword),
	m_source(std::move(source)),
	m_updates(std::move(updates))
{
}

MarmotExpression::IfElse::IfElse(const Token& if_token, const Token& then_token, const Token& else_token, std::unique_ptr<MarmotExpression>&& condition, std::unique_ptr<MarmotExpression>&& true_branch, std::unique_ptr<MarmotExpression>&& else_branch)
	:
	m_if_token(if_token),
	m_then_token(then_token),
	m_else_token(else_token),
	m_condition(std::move(condition)),
	m_true_branch(std::move(true_branch)),
	m_else_branch(std::move(else_branch))
{
}

MarmotExpression::MemberAccess::MemberAccess(const Token& member_name, std::unique_ptr<MarmotExpression>&& struct_expr, int index)
	: m_member_name(member_name),
	m_struct(std::move(struct_expr)),
	m_index(index)
{
}

MarmotExpression::Array::Array(const Token& op, std::vector<std::unique_ptr<MarmotExpression>>&& elems)
	: m_op(op),
	m_elems(std::move(elems))
{
}

MarmotExpression::IndexAccess::IndexAccess(const Token& op, std::unique_ptr<MarmotExpression>&& index, std::unique_ptr<MarmotExpression>&& arr_var)
	: m_op(op),
	m_index(std::move(index)),
	m_arr_var(std::move(arr_var))
{
}

MarmotExpression::ArrayComprehension::ArrayComprehension(const Token& bracket, const Token& loop_variable, const Token& in_keyword,
	std::unique_ptr<MarmotExpression>&& transform_expr, std::unique_ptr<MarmotExpression>&& range)
	: m_bracket(bracket),
	m_loop_variable(loop_variable),
	m_in_keyword(in_keyword),
	m_transform_expr(std::move(transform_expr)),
	m_range(std::move(range))
{
}

MarmotExpression::RangeBinary::RangeBinary(const Token& range_op, std::unique_ptr<MarmotExpression>&& start, std::unique_ptr<MarmotExpression>&& end)
	: m_range_op(range_op),
	m_start(std::move(start)),
	m_end(std::move(end))
{
}

MarmotExpression::RangeTernary::RangeTernary(const Token& first_op, const Token& second_op, std::unique_ptr<MarmotExpression>&& start, std::unique_ptr<MarmotExpression>&& step, std::unique_ptr<MarmotExpression>&& end)
	: m_first_range_op(first_op),
	m_second_range_op(second_op),
	m_start(std::move(start)),
	m_step(std::move(step)),
	m_end(std::move(end))
{
}

MarmotExpression::Block::Block(const Token& right_brace, std::vector<std::unique_ptr<MarmotStatement>>&& stmts, std::unique_ptr<MarmotExpression>&& final_expr)
	: m_right_brace(right_brace),
	m_stmts(std::move(stmts)),
	m_final_expr(MakeOptionalExpression(std::move(final_expr)))
{
}

MarmotExpression::Match::Match(const Token& switch_keyword, std::unique_ptr<MarmotExpression>&& arg_expr, std::vector<std::unique_ptr<MarmotExpression>>&& cases)
	: m_match_keyword(switch_keyword),
	m_arg_expr(std::move(arg_expr)),
	m_cases(std::move(cases))
{
}

MarmotExpression::Case::Case(const Token& keyword, std::unique_ptr<MarmotPattern>&& pattern, std::unique_ptr<MarmotExpression>&& expr, std::unique_ptr<MarmotExpression>&& guard)
	: m_keyword(keyword),
	m_pattern(std::move(pattern)),
	m_expr(std::move(expr)),
	m_guard(MakeOptionalExpression(std::move(guard)))
{
}

bool MarmotExpression::Case::HasGuard() const
{
	return m_guard.has_value();
}

MarmotExpression::For::For(const Token& for_keyword, const Token& loop_variable, const Token& in_keyword,
	std::unique_ptr<MarmotExpression>&& range, std::unique_ptr<MarmotExpression>&& body)
	: m_for_keyword(for_keyword),
	m_loop_variable(loop_variable),
	m_in_keyword(in_keyword),
	m_range(std::move(range)),
	m_body(std::move(body))
{
}

MarmotPattern::PatternUnion& MarmotPattern::operator*()
{
	return m_variant;
}

const MarmotPattern::PatternUnion& MarmotPattern::operator*() const
{
	return m_variant;
}

std::shared_ptr<MarmotType>& MarmotPattern::GetType()
{
	return std::visit(TypeDataAccessor{}, m_variant);
}

const std::shared_ptr<MarmotType>& MarmotPattern::GetType() const
{
	return std::visit(ConstTypeDataAccessor{}, m_variant);
}

namespace
{
	template<typename Pattern, typename Binding>
	void CollectBindings(Pattern& pattern, bool every_alternative, std::vector<Binding*>& bindings)
	{
		const auto children = [every_alternative, &bindings](auto& patterns)
			{
				for (auto& child : patterns)
				{
					CollectBindings(*child, every_alternative, bindings);
				}
			};

		std::visit
		(
			[&]<typename T>(T& node)
			{
				using Node = std::remove_const_t<T>;
				if constexpr (std::is_same_v<Node, MarmotPattern::Binding>)
				{
					bindings.push_back(&node);
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Tuple>)
				{
					children(node.m_elements);
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Array>)
				{
					children(node.m_elements);
					if (node.m_rest.has_value())
					{
						CollectBindings(*node.m_rest->m_pattern, every_alternative, bindings);
					}
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Constructor>)
				{
					children(node.m_args);
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Record>)
				{
					for (auto& field : node.m_fields)
					{
						CollectBindings(*field.m_pattern, every_alternative, bindings);
					}
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::As>)
				{
					CollectBindings(*node.m_pattern, every_alternative, bindings);
					CollectBindings(*node.m_binding, every_alternative, bindings);
				}
				else if constexpr (std::is_same_v<Node, MarmotPattern::Or>)
				{
					if (every_alternative)
					{
						children(node.m_alternatives);
					}
					else
					{
						CollectBindings(*node.m_alternatives.front(), every_alternative, bindings);
					}
				}
			},
			*pattern
		);
	}
}

std::vector<MarmotPattern::Binding*> MarmotPattern::Bindings(bool every_alternative)
{
	std::vector<Binding*> bindings;
	CollectBindings(*this, every_alternative, bindings);
	return bindings;
}

std::vector<const MarmotPattern::Binding*> MarmotPattern::Bindings() const
{
	std::vector<const Binding*> bindings;
	CollectBindings(*this, false, bindings);
	return bindings;
}

MarmotExpression::ExpressionUnion& MarmotExpression::operator*()
{
	return m_variant;
}

const MarmotExpression::ExpressionUnion& MarmotExpression::operator*() const
{
	return m_variant;
}

std::shared_ptr<MarmotType>& MarmotExpression::GetType()
{
	return std::visit(TypeDataAccessor{}, m_variant);
}

const std::shared_ptr<MarmotType>& MarmotExpression::GetType() const
{
	return std::visit(ConstTypeDataAccessor{}, m_variant);
}

MarmotStatement::StatementUnion& MarmotStatement::operator*()
{
	return m_variant;
}

const MarmotStatement::StatementUnion& MarmotStatement::operator*() const
{
	return m_variant;
}

MarmotStatement::ExpressionStatement::ExpressionStatement(const Token& semicolon, std::unique_ptr<MarmotExpression>&& expr)
	: m_semicolon(semicolon), m_expr(std::move(expr))
{
}

MarmotStatement::VariableDefinition::VariableDefinition(const Token& name, std::unique_ptr<MarmotExpression>&& value, std::optional<std::shared_ptr<MarmotType>>&& annotated_type, std::optional<BindingId>&& binding)
	: m_name(name),
	m_value(std::move(value)),
	m_annotated_type(std::move(annotated_type)),
	m_binding(std::move(binding))
{
}

MarmotStatement::TupleDefinition::TupleDefinition(std::vector<Token>&& names, std::unique_ptr<MarmotExpression>&& value, std::vector<std::optional<BindingId>>&& bindings)
	: m_names(std::move(names)),
	m_value(std::move(value)),
	m_bindings(std::move(bindings))
{
}

MarmotStatement::ForeignDefinition::ForeignDefinition(const Token& function_name, const std::string& foreign_name, std::shared_ptr<MarmotType>&& type, std::optional<BindingId>&& binding, std::optional<std::string> library)
	: m_function_name(function_name),
	m_foreign_name(foreign_name),
	m_type(std::move(type)),
	m_binding(std::move(binding)),
	m_library(std::move(library))
{
}

std::optional<MarmotPrimitive> MarmotStatement::ForeignDefinition::DeclaredPrimitive() const
{
	return m_type->GetType<MarmotType::FunctionType>().m_primitive;
}

MarmotStatement::Struct::Struct(const Token& name, std::vector<Token>&& generic_params, std::vector<MarmotType::ClassConstraint>&& constraints, std::shared_ptr<MarmotType>&& self_type)
	: m_name(name),
	m_generic_params(std::move(generic_params)),
	m_constraints(std::move(constraints)),
	m_self_type(std::move(self_type))
{
}

MarmotStatement::Union::Union(const Token& name, std::vector<Token>&& generic_params, std::vector<Token>&& constructor_names, std::vector<MarmotType::ClassConstraint>&& constraints, std::shared_ptr<MarmotType>&& self_type)
	: m_name(name),
	m_generic_params(std::move(generic_params)),
	m_constructor_names(std::move(constructor_names)),
	m_constraints(std::move(constraints)),
	m_self_type(std::move(self_type))
{
}

MarmotStatement::FunctionDefinition::FunctionDefinition(const Token& name, std::vector<Token>&& generic_params, FunctionParameters&& params, std::vector<std::shared_ptr<MarmotType>>&& param_types, std::shared_ptr<MarmotType>&& return_type, std::unique_ptr<MarmotExpression>&& body, std::optional<BindingId>&& binding, std::vector<MarmotType::ClassConstraint>&& constraints)
	: m_name(name),
	m_generic_params(std::move(generic_params)),
	m_params(std::move(params)),
	m_param_types(std::move(param_types)),
	m_constraints(std::move(constraints)),
	m_return_type(std::move(return_type)),
	m_body(std::move(body)),
	m_binding(std::move(binding))
{
}

MarmotStatement::Class::AssociatedTypeDeclaration::AssociatedTypeDeclaration(const Token& name)
	: m_name(name)
{
}

MarmotStatement::Class::Class(const Token& name, std::vector<Token>&& type_params, std::vector<AssociatedTypeDeclaration>&& associated_types, std::vector<std::unique_ptr<MarmotStatement>>&& methods)
	: m_name(name),
	m_type_params(std::move(type_params)),
	m_associated_types(std::move(associated_types)),
	m_methods(std::move(methods))
{
}

MarmotStatement::Instance::Method::Method(const Token& name, FunctionDefinition&& definition)
	: m_name(name),
	m_definition(std::move(definition))
{
}

MarmotStatement::Instance::AssociatedTypeBinding::AssociatedTypeBinding(const Token& name, std::shared_ptr<MarmotType>&& type)
	: m_name(name),
	m_type(std::move(type))
{
}

MarmotStatement::Instance::Instance(const Token& class_name, std::vector<std::shared_ptr<MarmotType>>&& type_args, std::vector<MarmotType::ClassConstraint>&& constraints, std::vector<AssociatedTypeBinding>&& associated_types, std::vector<Method>&& methods)
	: m_class_name(class_name),
	m_type_args(std::move(type_args)),
	m_constraints(std::move(constraints)),
	m_associated_types(std::move(associated_types)),
	m_methods(std::move(methods))
{
}

MarmotStatement::TypeAlias::TypeAlias(const Token& name, std::vector<Token>&& generic_params, std::shared_ptr<MarmotType>&& aliased_type)
	: m_name(name),
	m_generic_params(std::move(generic_params)),
	m_aliased_type(std::move(aliased_type))
{
}
