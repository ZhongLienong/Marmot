#include "AbstractSyntaxTreePrinter.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

namespace
{
	std::string JoinNames(const std::vector<Token>& names)
	{
		return names
			| std::views::transform([](const Token& name) { return name.m_lexeme; })
			| std::views::join_with(std::string_view(", "))
			| std::ranges::to<std::string>();
	}

	std::string DescribeConstraint(const MarmotType::ClassConstraint& constraint)
	{
		return constraint.IsEquality()
			? std::format("{} = {}", constraint.m_equality_lhs->ToString(), constraint.m_equality_rhs->ToString())
			: std::format("{}<{}>", constraint.m_class_name, MarmotType::JoinToStrings(constraint.m_type_args));
	}

	struct PrintAbstractSyntaxTree
	{
		std::string& m_output;

		explicit PrintAbstractSyntaxTree(std::string& output);

		void PrintWithIndentation(int depth, std::string_view text) const;

		void PrintVariableSemantic(int depth, const MarmotExpression::NameContext::Tag& tag) const;

		void operator()(const MarmotStatement::ExpressionStatement& simple, int depth = 0) const;

		void operator()(const MarmotStatement::VariableDefinition& def, int depth = 0) const;

		void operator()(const MarmotStatement::TupleDefinition& def_tuple, int depth = 0) const;

		void operator()(const MarmotStatement::FunctionDefinition& defun, int depth = 0) const;

		void operator()(const MarmotStatement::ForeignDefinition& foreign, int depth = 0) const;

		void operator()(const MarmotStatement::Struct& struct_stmt, int depth = 0) const;

		void operator()(const MarmotStatement::Union& union_stmt, int depth = 0) const;

		void operator()(const MarmotStatement::Class& class_stmt, int depth = 0) const;

		void operator()(const MarmotStatement::Instance& instance, int depth = 0) const;

		void operator()(const MarmotStatement::TypeAlias& alias, int depth = 0) const;

		void operator()(const MarmotExpression::As& as, int depth = 0) const;

		void operator()(const MarmotExpression::Binary& binary, int depth = 0) const;

		void operator()(const MarmotExpression::Group& group, int depth = 0) const;

		void operator()(const MarmotExpression::Tuple& tuple, int depth = 0) const;

		void operator()(const MarmotExpression::UnaryPrefix& unary, int depth = 0) const;

		void operator()(const MarmotExpression::Call& call, int depth = 0) const;

		void operator()(const MarmotExpression::MemberAccess& get, int depth = 0) const;

		void operator()(const MarmotExpression::NameAccess& variable, int depth = 0) const;

		void operator()(const MarmotExpression::Literal& literal, int depth = 0) const;

		void operator()(const MarmotExpression::Function& closure, int depth = 0) const;

		void operator()(const MarmotExpression::Construct& construct, int depth = 0) const;

		void operator()(const MarmotExpression::RecordUpdate& record_update, int depth = 0) const;

		void operator()(const MarmotExpression::Array& array, int depth = 0) const;

		void operator()(const MarmotExpression::IndexAccess& array_get, int depth = 0) const;

		void operator()(const MarmotExpression::ArrayComprehension& comprehension, int depth = 0) const;

		void operator()(const MarmotExpression::RangeBinary& range_binary, int depth = 0) const;

		void operator()(const MarmotExpression::RangeTernary& range_ternary, int depth = 0) const;

		void operator()(const MarmotExpression::IfElse& if_else, int depth = 0) const;

		void operator()(const MarmotExpression::Block& block, int depth = 0) const;

		void operator()(const MarmotExpression::Match& match, int depth = 0) const;

		void operator()(const MarmotExpression::Case& case_expr, int depth = 0) const;

		void operator()(const MarmotExpression::For& for_expr, int depth = 0) const;

		void operator()(const MarmotPattern::Binding& binding, int depth = 0) const;

		void operator()(const MarmotPattern::Wildcard& wildcard, int depth = 0) const;

		void operator()(const MarmotPattern::Literal& literal, int depth = 0) const;

		void operator()(const MarmotPattern::Tuple& tuple, int depth = 0) const;

		void operator()(const MarmotPattern::Array& array, int depth = 0) const;

		void operator()(const MarmotPattern::Constructor& constructor, int depth = 0) const;
		void operator()(const MarmotPattern::As& as, int depth = 0) const;
		void operator()(const MarmotPattern::Or& either, int depth = 0) const;
		void operator()(const MarmotPattern::Range& range, int depth = 0) const;
		void operator()(const MarmotPattern::Record& record, int depth = 0) const;

		void Visit(const std::unique_ptr<MarmotStatement>& statement, int depth) const;

		void Visit(const std::unique_ptr<MarmotExpression>& expression, int depth) const;

		void Visit(const std::unique_ptr<MarmotPattern>& pattern, int depth) const;
	};
}

PrintAbstractSyntaxTree::PrintAbstractSyntaxTree(std::string& output)
	: m_output(output)
{
}

void PrintAbstractSyntaxTree::PrintWithIndentation(int depth, std::string_view text) const
{
	m_output.append(static_cast<size_t>(depth), ' ');
	m_output += text;
	m_output.push_back('\n');
}

void PrintAbstractSyntaxTree::PrintVariableSemantic(int depth, const MarmotExpression::NameContext::Tag& tag) const
{
	std::visit
	(
		[depth, this](auto&& arg) -> void
		{
			using T = std::decay_t<decltype(arg)>;

			if constexpr (std::is_same_v<T, MarmotExpression::NameContext::Local>)
			{
				PrintWithIndentation(depth, "Local");
				PrintWithIndentation(depth, "Binding: " + std::to_string(arg.m_binding->m_index));
			}
			else if constexpr (std::is_same_v<T, MarmotExpression::NameContext::Global>)
			{
				PrintWithIndentation(depth, "Global");
			}
			else if constexpr (std::is_same_v<T, MarmotExpression::NameContext::Cell>)
			{
				PrintWithIndentation(depth, "Cell");
				PrintWithIndentation(depth, "Binding: " + std::to_string(arg.m_binding->m_index));
			}
		}, tag
	);
}

void PrintAbstractSyntaxTree::Visit(const std::unique_ptr<MarmotStatement>& statement, int depth) const
{
	VisitNode([this, depth](auto&& arg) { (*this)(arg, depth); }, statement);
}

void PrintAbstractSyntaxTree::Visit(const std::unique_ptr<MarmotExpression>& expression, int depth) const
{
	VisitNode([this, depth](auto&& arg) { (*this)(arg, depth); }, expression);
}

void PrintAbstractSyntaxTree::Visit(const std::unique_ptr<MarmotPattern>& pattern, int depth) const
{
	VisitNode([this, depth](auto&& arg) { (*this)(arg, depth); }, pattern);
}

void PrintAbstractSyntaxTree::operator()(const MarmotStatement::ExpressionStatement& simple, int depth) const
{
	PrintWithIndentation(depth, "Simple {");
	Visit(simple.m_expr, depth + 1);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotStatement::VariableDefinition& def, int depth) const
{
	PrintWithIndentation(depth, "Define {");
	PrintWithIndentation(depth + 1, "Name: " + def.m_name.m_lexeme);
	PrintWithIndentation(depth + 1, "Value: ");
	Visit(def.m_value, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotStatement::TupleDefinition& def_tuple, int depth) const
{
	PrintWithIndentation(depth, "DefineTuple {");
	PrintWithIndentation(depth + 1, "Names: ");
	std::ranges::for_each
	(
		def_tuple.m_names,
		[depth, this](const Token& name)
		{
			PrintWithIndentation(depth + 2, name.m_lexeme);
		}
	);
	PrintWithIndentation(depth + 1, "Value: ");
	Visit(def_tuple.m_value, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotStatement::FunctionDefinition& defun, int depth) const
{
	PrintWithIndentation(depth, "DefineFunction {");
	PrintWithIndentation(depth + 1, "Name: " + defun.m_name.m_lexeme);
	PrintWithIndentation(depth + 1, "Parameters: ");
	for (size_t index = 0u; index < defun.m_params.Size(); index += 1u)
	{
		PrintWithIndentation(depth + 2, "ParameterName: " + defun.m_params.DisplayName(index));
		PrintWithIndentation(depth + 2, "ParameterType: " + defun.m_param_types[index]->ToString());
	}
	PrintWithIndentation(depth + 1, "ReturnType: " + defun.m_return_type->ToString());
	// A class declares its methods' signatures only.
	if (defun.m_body != nullptr)
	{
		PrintWithIndentation(depth + 1, "Body: ");
		Visit(defun.m_body, depth + 2);
	}
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotStatement::ForeignDefinition& foreign, int depth) const
{
	PrintWithIndentation(depth, "ForeignFunctionInterface {");
	PrintWithIndentation(depth + 1, "Name: " + foreign.m_function_name.m_lexeme);
	PrintWithIndentation(depth + 1, "ForeignName: " + foreign.m_foreign_name);
	PrintWithIndentation(depth + 1, "Type: " + foreign.m_type->ToString());
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotStatement::Struct& struct_stmt, int depth) const
{
	PrintWithIndentation(depth, "Struct {");
	PrintWithIndentation(depth + 1, "Name: " + struct_stmt.m_name.m_lexeme);
	const MarmotType::StructType& struct_type = struct_stmt.m_self_type->GetType<MarmotType::StructType>();
	for (size_t i = 0u; i < struct_type.m_member_names.size(); i += 1u)
	{
		PrintWithIndentation(depth + 1, "MemberName: ");
		PrintWithIndentation(depth + 2, struct_type.m_member_names[i]);
		PrintWithIndentation(depth + 1, "MemberType: ");
		PrintWithIndentation(depth + 2, struct_type.m_member_types[i]->ToString());
	}
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotStatement::Union& union_stmt, int depth) const
{
	PrintWithIndentation(depth, "Union {");
	PrintWithIndentation(depth + 1, "Name: " + union_stmt.m_name.m_lexeme);
	PrintWithIndentation(depth + 1, "Constructors: ");

	const MarmotType::UnionType& union_type = union_stmt.m_self_type->GetType<MarmotType::UnionType>();

	for (const auto& [name, info] : union_type.m_member_info)
	{
		PrintWithIndentation(depth + 2, "{");
		PrintWithIndentation(depth + 3, std::format("{}({})", name, MarmotType::JoinToStrings(info.m_member_types)));
		PrintWithIndentation(depth + 3, "Tag: " + std::to_string(info.m_tag));
		PrintWithIndentation(depth + 2, "}");
	}
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotExpression::As& as, int depth) const
{
	PrintWithIndentation(depth, "As {");
	PrintWithIndentation(depth + 1, "Value: ");
	Visit(as.m_expr, depth + 2);
	PrintWithIndentation(depth + 1, "ToType: ");
	PrintWithIndentation(depth + 2, as.m_to_type->ToString());
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotExpression::Binary& binary, int depth) const
{
	PrintWithIndentation(depth, "Binary {");
	PrintWithIndentation(depth + 1, "Operator: " + binary.m_op.m_lexeme);
	PrintWithIndentation(depth + 1, "Left: ");
	Visit(binary.m_left, depth + 2);
	PrintWithIndentation(depth + 1, "Right: ");
	Visit(binary.m_right, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotExpression::Group& group, int depth) const
{
	PrintWithIndentation(depth, "Group {");
	Visit(group.m_expr_in, depth + 1);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotExpression::Tuple& tuple, int depth) const
{
	PrintWithIndentation(depth, "Tuple {");
	PrintWithIndentation(depth + 1, "Elements: ");
	std::ranges::for_each
	(
		tuple.m_elements,
		[depth, this](const std::unique_ptr<MarmotExpression>& elem)
		{
			Visit(elem, depth + 2);
		}
	);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotExpression::UnaryPrefix& unary, int depth) const
{
	PrintWithIndentation(depth, "UnaryPrefix {");
	PrintWithIndentation(depth + 1, "Operator: " + unary.m_op.m_lexeme);
	PrintWithIndentation(depth + 1, "Operand: ");
	Visit(unary.m_expr, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotExpression::Call& call, int depth) const
{
	PrintWithIndentation(depth, "Call {");
	PrintWithIndentation(depth + 1, "Callee: ");
	Visit(call.m_callee, depth + 2);
	PrintWithIndentation(depth + 1, "Args: ");
	std::ranges::for_each
	(
		call.m_arguments,
		[depth, this](const std::unique_ptr<MarmotExpression>& arg)
		{
			Visit(arg, depth + 2);
		}
	);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotExpression::MemberAccess& get, int depth) const
{
	PrintWithIndentation(depth, "Get {");
	PrintWithIndentation(depth + 1, "MarmotStruct: ");
	Visit(get.m_struct, depth + 2);
	PrintWithIndentation(depth + 1, "Name: " + get.m_member_name.m_lexeme);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotExpression::NameAccess& variable, int depth) const
{
	PrintWithIndentation(depth, "BoundedName {");
	const MarmotExpression::NameContext::Local* local = std::get_if<MarmotExpression::NameContext::Local>(&variable.m_name_ctx);
	const std::string name = local != nullptr && local->m_is_generated
		? std::format("%local{}", local->m_binding->m_index)
		: variable.m_name.m_lexeme;
	PrintWithIndentation(depth + 1, "Name: " + name);
	PrintWithIndentation(depth + 1, "NameContext {");
	PrintVariableSemantic(depth + 2, variable.m_name_ctx);
	PrintWithIndentation(depth + 1, "}");
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotExpression::Literal& literal, int depth) const
{
	switch (literal.m_kind)
	{
	case MarmotExpression::LiteralKind::Text:
		PrintWithIndentation(depth, "Text {");
		PrintWithIndentation(depth + 1, "Value: \"" + literal.m_token.m_lexeme + "\"");
		PrintWithIndentation(depth, "}");
		return;
	case MarmotExpression::LiteralKind::Unit:
		PrintWithIndentation(depth, "()");
		return;
	case MarmotExpression::LiteralKind::Bool:
		PrintWithIndentation(depth, "Bool {");
		break;
	case MarmotExpression::LiteralKind::Float:
		PrintWithIndentation(depth, "Float {");
		break;
	case MarmotExpression::LiteralKind::Integer:
		PrintWithIndentation(depth, "Integer {");
		break;
	case MarmotExpression::LiteralKind::Byte:
		PrintWithIndentation(depth, "Byte {");
		break;
	case MarmotExpression::LiteralKind::Word:
		PrintWithIndentation(depth, "Word {");
		break;
	}
	PrintWithIndentation(depth + 1, "Value: " + literal.m_token.m_lexeme);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotExpression::Function& closure, int depth) const
{
	PrintAbstractSyntaxTree::PrintWithIndentation(depth, "Function {");
	PrintWithIndentation(depth + 1, "Parameters: ");
	for (size_t index = 0u; index < closure.m_params.Size(); index += 1u)
	{
		PrintWithIndentation(depth + 2, "ParameterName: " + closure.m_params.DisplayName(index));
		PrintWithIndentation(depth + 2, "ParameterType: " + closure.m_param_types[index]->ToString());
	}
	PrintWithIndentation(depth + 1, "ReturnType: " + closure.m_return_type->ToString());
	PrintWithIndentation(depth + 1, "Body: ");
	Visit(closure.m_body, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotExpression::Construct& construct, int depth) const
{
	PrintWithIndentation(depth, "Construct {");
	PrintWithIndentation(depth + 1, "Type: " + construct.m_return_type->ToString());
	PrintWithIndentation(depth + 1, "Params: ");
	std::ranges::for_each
	(
		construct.m_params,
		[depth, this](const std::unique_ptr<MarmotExpression>& expr)
		{
			Visit(expr, depth + 2);
		}
	);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotExpression::RecordUpdate& record_update, int depth) const
{
	PrintWithIndentation(depth, "RecordUpdate {");
	PrintWithIndentation(depth + 1, "Source: ");
	Visit(record_update.m_source, depth + 2);
	PrintWithIndentation(depth + 1, "Fields: ");
	std::ranges::for_each
	(
		record_update.m_updates,
		[depth, this](const MarmotExpression::RecordUpdate::FieldUpdate& update)
		{
			PrintWithIndentation(depth + 2, std::format("{} = ", update.m_path | std::views::transform(&Token::m_lexeme) | std::views::join_with('.') | std::ranges::to<std::string>()));
			Visit(update.m_value, depth + 3);
		}
	);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotExpression::Array& array, int depth) const
{
	PrintWithIndentation(depth, "Array {");
	PrintWithIndentation(depth + 1, "Elements: ");
	std::ranges::for_each
	(
		array.m_elems,
		[depth, this](const std::unique_ptr<MarmotExpression>& element)
		{
			Visit(element, depth + 2);
		}
	);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotExpression::IndexAccess& array_get, int depth) const
{
	PrintWithIndentation(depth, "ArrayGet {");
	PrintWithIndentation(depth + 1, "Array: ");
	Visit(array_get.m_arr_var, depth + 2);
	PrintWithIndentation(depth + 1, "Index: ");
	Visit(array_get.m_index, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::PrintAbstractSyntaxTree::operator()(const MarmotExpression::RangeBinary& range_binary, int depth) const
{
	PrintWithIndentation(depth, "RangeBinary {");
	PrintWithIndentation(depth + 1, "Start: ");
	Visit(range_binary.m_start, depth + 2);
	PrintWithIndentation(depth + 1, "End: ");
	Visit(range_binary.m_end, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::PrintAbstractSyntaxTree::operator()(const MarmotExpression::RangeTernary& range_ternary, int depth) const
{
	PrintWithIndentation(depth, "RangeTernary {");
	PrintWithIndentation(depth + 1, "Start: ");
	Visit(range_ternary.m_start, depth + 2);
	PrintWithIndentation(depth + 1, "Step: ");
	Visit(range_ternary.m_step, depth + 2);
	PrintWithIndentation(depth + 1, "End: ");
	Visit(range_ternary.m_end, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::PrintAbstractSyntaxTree::operator()(const MarmotExpression::IfElse& if_else, int depth) const
{
	PrintWithIndentation(depth, "IfElse {");
	PrintWithIndentation(depth + 1, "Condition: ");
	Visit(if_else.m_condition, depth + 2);
	PrintWithIndentation(depth + 1, "Then: ");
	Visit(if_else.m_true_branch, depth + 2);
	PrintWithIndentation(depth + 1, "Else: ");
	Visit(if_else.m_else_branch, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotExpression::Block& block, int depth) const
{
	PrintWithIndentation(depth, "Block {");
	PrintWithIndentation(depth + 1, "Statements: ");
	std::ranges::for_each
	(
		block.m_stmts, [depth, this](const std::unique_ptr<MarmotStatement>& stmt)
		{
			Visit(stmt, depth + 2);
		}
	);
	if (block.m_final_expr.HasValue())
	{
		PrintWithIndentation(depth + 1, "Expression: ");
		Visit(*block.m_final_expr, depth + 2);
	}
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotExpression::Match& match, int depth) const
{
	PrintWithIndentation(depth, "Match {");
	PrintWithIndentation(depth + 1, "Value: ");
	Visit(match.m_arg_expr, depth + 2);
	PrintWithIndentation(depth + 1, "Cases: ");
	std::ranges::for_each
	(
		match.m_cases,
		[depth, this](auto&& case_expr)
		{
			Visit(case_expr, depth + 2);
		}
	);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotExpression::Case& case_expr, int depth) const
{
	PrintWithIndentation(depth, "Case {");
	PrintWithIndentation(depth + 1, "Pattern: ");
	Visit(case_expr.m_pattern, depth + 2);
	if (case_expr.HasGuard())
	{
		PrintWithIndentation(depth + 1, "Guard: ");
		Visit(case_expr.m_guard.Value(), depth + 2);
	}
	PrintWithIndentation(depth + 1, "Value {");
	Visit(case_expr.m_expr, depth + 2);
	PrintWithIndentation(depth + 1, "}");
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotExpression::For& for_expr, int depth) const
{
	PrintWithIndentation(depth, "For {");
	PrintWithIndentation(depth + 1, "Variable: " + std::string(for_expr.m_loop_variable.m_lexeme));
	PrintWithIndentation(depth + 1, "Range: ");
	Visit(for_expr.m_range, depth + 2);
	PrintWithIndentation(depth + 1, "Body: ");
	Visit(for_expr.m_body, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotPattern::Binding& binding, int depth) const
{
	PrintWithIndentation(depth, "BindingPattern {");
	PrintWithIndentation(depth + 1, binding.m_is_generated ? std::format("%local{}", binding.m_binding.Value()->m_index) : binding.m_name.m_lexeme);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotPattern::Wildcard& wildcard, int depth) const
{
	PrintWithIndentation(depth, "WildcardPattern {");
	PrintWithIndentation(depth + 1, wildcard.m_token.m_lexeme);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotPattern::Literal& literal, int depth) const
{
	PrintWithIndentation(depth, "LiteralPattern {");
	PrintWithIndentation(depth + 1, literal.m_token.m_lexeme);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotPattern::Tuple& tuple, int depth) const
{
	PrintWithIndentation(depth, "TuplePattern {");
	std::ranges::for_each
	(
		tuple.m_elements,
		[depth, this](const std::unique_ptr<MarmotPattern>& elem)
		{
			Visit(elem, depth + 1);
		}
	);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotPattern::Array& array, int depth) const
{
	PrintWithIndentation(depth, "ArrayPattern {");
	std::ranges::for_each
	(
		array.m_elements,
		[depth, this](const std::unique_ptr<MarmotPattern>& elem)
		{
			Visit(elem, depth + 1);
		}
	);
	if (array.m_rest.HasValue())
	{
		PrintWithIndentation(depth + 1, std::format("Rest after {} element(s) {{", array.m_rest->m_position));
		Visit(array.m_rest->m_pattern, depth + 2);
		PrintWithIndentation(depth + 1, "}");
	}
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotPattern::Constructor& constructor, int depth) const
{
	PrintWithIndentation(depth, "ConstructorPattern {");
	PrintWithIndentation(depth + 1, constructor.m_name);
	std::ranges::for_each
	(
		constructor.m_args,
		[depth, this](const std::unique_ptr<MarmotPattern>& arg)
		{
			Visit(arg, depth + 1);
		}
	);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotPattern::As& as, int depth) const
{
	PrintWithIndentation(depth, "AsPattern {");
	Visit(as.m_pattern, depth + 1);
	Visit(as.m_binding, depth + 1);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotPattern::Or& either, int depth) const
{
	PrintWithIndentation(depth, "OrPattern {");
	std::ranges::for_each(either.m_alternatives, [depth, this](const std::unique_ptr<MarmotPattern>& alternative) { Visit(alternative, depth + 1); });
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotPattern::Range& range, int depth) const
{
	PrintWithIndentation(depth, std::format("RangePattern {{ {} to {} }}", range.m_low, range.m_high));
}

void PrintAbstractSyntaxTree::operator()(const MarmotPattern::Record& record, int depth) const
{
	PrintWithIndentation(depth, std::format("RecordPattern {} {{", record.m_name));
	for (const MarmotPattern::Record::Field& field : record.m_fields)
	{
		PrintWithIndentation(depth + 1, std::format("{} =", field.m_name.m_lexeme));
		Visit(field.m_pattern, depth + 2);
	}
	if (record.m_rest.HasValue())
	{
		PrintWithIndentation(depth + 1, "..");
	}
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotStatement::Class& class_stmt, int depth) const
{
	PrintWithIndentation(depth, "Class {");
	PrintWithIndentation(depth + 1, "Name: " + class_stmt.m_name.m_lexeme);
	PrintWithIndentation(depth + 1, "TypeParameters: " + JoinNames(class_stmt.m_type_params));
	std::ranges::for_each(class_stmt.m_associated_types, [depth, this](const MarmotStatement::Class::AssociatedTypeDeclaration& associated) { PrintWithIndentation(depth + 1, "AssociatedType: " + associated.m_name.m_lexeme); });
	PrintWithIndentation(depth + 1, "Methods: ");
	std::ranges::for_each(class_stmt.m_methods, [depth, this](const std::unique_ptr<MarmotStatement>& method) { Visit(method, depth + 2); });
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotStatement::Instance& instance, int depth) const
{
	PrintWithIndentation(depth, "Instance {");
	PrintWithIndentation(depth + 1, "Class: " + instance.m_class_name.m_lexeme);
	PrintWithIndentation(depth + 1, "TypeArguments: " + MarmotType::JoinToStrings(instance.m_type_args));
	std::ranges::for_each(instance.m_constraints, [depth, this](const MarmotType::ClassConstraint& constraint) { PrintWithIndentation(depth + 1, "Where: " + DescribeConstraint(constraint)); });
	std::ranges::for_each(instance.m_associated_types, [depth, this](const MarmotStatement::Instance::AssociatedTypeBinding& binding) { PrintWithIndentation(depth + 1, std::format("AssociatedType: {} = {}", binding.m_name.m_lexeme, binding.m_type->ToString())); });
	PrintWithIndentation(depth + 1, "Methods: ");
	std::ranges::for_each(instance.m_methods, [depth, this](const MarmotStatement::Instance::Method& method) { (*this)(method.m_definition, depth + 2); });
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotStatement::TypeAlias& alias, int depth) const
{
	PrintWithIndentation(depth, "TypeAlias {");
	PrintWithIndentation(depth + 1, "Name: " + alias.m_name.m_lexeme);
	PrintWithIndentation(depth + 1, "GenericParameters: " + JoinNames(alias.m_generic_params));
	PrintWithIndentation(depth + 1, "AliasedType: " + alias.m_aliased_type->ToString());
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MarmotExpression::ArrayComprehension& comprehension, int depth) const
{
	PrintWithIndentation(depth, "ArrayComprehension {");
	PrintWithIndentation(depth + 1, "Variable: " + comprehension.m_loop_variable.m_lexeme);
	PrintWithIndentation(depth + 1, "Range: ");
	Visit(comprehension.m_range, depth + 2);
	PrintWithIndentation(depth + 1, "Transform: ");
	Visit(comprehension.m_transform_expr, depth + 2);
	PrintWithIndentation(depth, "}");
}

AbstractSyntaxTreePrinter::AbstractSyntaxTreePrinter(std::string_view module_name, const MarmotProgramTree& program)
	: m_module_name(module_name),
	m_program(program)
{
}

std::string AbstractSyntaxTreePrinter::Print() const
{
	std::string output = std::format("module {}\n", m_module_name);
	const PrintAbstractSyntaxTree visitor(output);
	std::ranges::for_each(m_program, [&visitor](const std::unique_ptr<MarmotStatement>& statement) { visitor.Visit(statement, 0); });
	return output;
}
