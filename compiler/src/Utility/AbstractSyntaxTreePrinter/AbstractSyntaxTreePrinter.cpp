#if MIDORI_ENABLE_AST_DUMP

#include <algorithm>
#include "Common/BuildConfig/BuildConfig.h"
#include <ranges>

#include "AbstractSyntaxTreePrinter.h"
#include "Common\Printer\Printer.h"

void PrintAbstractSyntaxTree::PrintWithIndentation(int depth, std::string_view text) const
{
	Printer::Print(std::string(depth, ' '));
	Printer::Print(text);
	Printer::Print("\n");
}

void PrintAbstractSyntaxTree::PrintVariableSemantic(int depth, const MidoriExpression::NameContext::Tag& tag) const
{
	std::visit
	(
		[depth, this](auto&& arg) -> void
		{
			using T = std::decay_t<decltype(arg)>;

			if constexpr (std::is_same_v<T, MidoriExpression::NameContext::Local>)
			{
				PrintWithIndentation(depth, "Local");
				PrintWithIndentation(depth, "Index: " + std::to_string(arg.m_index));
			}
			else if constexpr (std::is_same_v<T, MidoriExpression::NameContext::Global>)
			{
				PrintWithIndentation(depth, "Global");
			}
			else if constexpr (std::is_same_v<T, MidoriExpression::NameContext::Cell>)
			{
				PrintWithIndentation(depth, "Cell");
				PrintWithIndentation(depth, "Index: " + std::to_string(arg.m_index));
			}
		}, tag
	);
}

void PrintAbstractSyntaxTree::Visit(const std::unique_ptr<MidoriStatement>& statement, int depth) const
{
	VisitNode([this, depth](auto&& arg) { (*this)(arg, depth); }, statement);
}

void PrintAbstractSyntaxTree::Visit(const std::unique_ptr<MidoriExpression>& expression, int depth) const
{
	VisitNode([this, depth](auto&& arg) { (*this)(arg, depth); }, expression);
}

void PrintAbstractSyntaxTree::Visit(const std::unique_ptr<MidoriPattern>& pattern, int depth) const
{
	VisitNode([this, depth](auto&& arg) { (*this)(arg, depth); }, pattern);
}

void PrintAbstractSyntaxTree::operator()(const MidoriStatement::ExpressionStatement& simple, int depth) const
{
	PrintWithIndentation(depth, "Simple {");
	Visit(simple.m_expr, depth + 1);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriStatement::VariableDefinition& def, int depth) const
{
	PrintWithIndentation(depth, "Define {");
	PrintWithIndentation(depth + 1, "Name: " + def.m_name.m_lexeme);
	PrintWithIndentation(depth + 1, "Value: ");
	Visit(def.m_value, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriStatement::TupleDefinition& def_tuple, int depth) const
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

void PrintAbstractSyntaxTree::operator()(const MidoriStatement::FunctionDefinition& defun, int depth) const
{
	PrintWithIndentation(depth, "DefineFunction {");
	PrintWithIndentation(depth + 1, "Name: " + defun.m_name.m_lexeme);
	PrintWithIndentation(depth + 1, "Parameters: ");
	std::ranges::for_each
	(
		std::ranges::views::zip(defun.m_params, defun.m_param_types),
		[depth, this](const auto& param)
		{
			PrintWithIndentation(depth + 2, "ParameterName: " + std::get<0>(param).m_lexeme);
			PrintWithIndentation(depth + 2, "ParameterType: " + std::get<1>(param)->ToString());
		}
	);
	PrintWithIndentation(depth + 1, "ReturnType: " + defun.m_return_type->ToString());
	PrintWithIndentation(depth + 1, "Body: ");
	Visit(defun.m_body, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriStatement::ForeignDefinition& foreign, int depth) const
{
	PrintWithIndentation(depth, "ForeignFunctionInterface {");
	PrintWithIndentation(depth + 1, "Name: " + foreign.m_function_name.m_lexeme);
	PrintWithIndentation(depth + 1, "ForeignName: " + foreign.m_foreign_name);
	PrintWithIndentation(depth + 1, "Type: " + foreign.m_type->ToString());
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriStatement::Struct& struct_stmt, int depth) const
{
	PrintWithIndentation(depth, "Struct {");
	PrintWithIndentation(depth + 1, "Name: " + struct_stmt.m_name.m_lexeme);
	const MidoriType::StructType& struct_type = struct_stmt.m_self_type->GetType<MidoriType::StructType>();
	for (size_t i = 0u; i < struct_type.m_member_names.size(); i += 1u)
	{
		PrintWithIndentation(depth + 1, "MemberName: ");
		PrintWithIndentation(depth + 2, struct_type.m_member_names[i]);
		PrintWithIndentation(depth + 1, "MemberType: ");
		PrintWithIndentation(depth + 2, struct_type.m_member_types[i]->ToString());
	}
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriStatement::Union& union_stmt, int depth) const
{
	PrintWithIndentation(depth, "Union {");
	PrintWithIndentation(depth + 1, "Name: " + union_stmt.m_name.m_lexeme);
	PrintWithIndentation(depth + 1, "Constructors: ");

	const MidoriType::UnionType& union_type = union_stmt.m_self_type->GetType<MidoriType::UnionType>();

	for (const auto& [name, info] : union_type.m_member_info)
	{
		PrintWithIndentation(depth + 2, "{");
		std::string constructor = name;
		constructor.push_back('(');
		for (size_t i = 0u; i < info.m_member_types.size(); i += 1u)
		{
			constructor.append(info.m_member_types[i]->ToString());
			if (i != info.m_member_types.size() - 1u)
			{
				constructor.append(", ");
			}
		}
		constructor.push_back(')');
		PrintWithIndentation(depth + 3, constructor);
		PrintWithIndentation(depth + 3, "Tag: " + std::to_string(info.m_tag));
		PrintWithIndentation(depth + 2, "}");
	}
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::As& as, int depth) const
{
	PrintWithIndentation(depth, "As {");
	PrintWithIndentation(depth + 1, "Value: ");
	Visit(as.m_expr, depth + 2);
	PrintWithIndentation(depth + 1, "ToType: ");
	PrintWithIndentation(depth + 2, as.m_to_type->ToString());
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::Binary& binary, int depth) const
{
	PrintWithIndentation(depth, "Binary {");
	PrintWithIndentation(depth + 1, "Operator: " + binary.m_op.m_lexeme);
	PrintWithIndentation(depth + 1, "Left: ");
	Visit(binary.m_left, depth + 2);
	PrintWithIndentation(depth + 1, "Right: ");
	Visit(binary.m_right, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::Group& group, int depth) const
{
	PrintWithIndentation(depth, "Group {");
	Visit(group.m_expr_in, depth + 1);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::Tuple& tuple, int depth) const
{
	PrintWithIndentation(depth, "Tuple {");
	PrintWithIndentation(depth + 1, "Elements: ");
	std::ranges::for_each
	(
		tuple.m_elements,
		[depth, this](const std::unique_ptr<MidoriExpression>& elem)
		{
			Visit(elem, depth + 2);
		}
	);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::UnaryPrefix& unary, int depth) const
{
	PrintWithIndentation(depth, "UnaryPrefix {");
	PrintWithIndentation(depth + 1, "Operator: " + unary.m_op.m_lexeme);
	PrintWithIndentation(depth + 1, "Operand: ");
	Visit(unary.m_expr, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::UnarySuffix& unary, int depth) const
{
	PrintWithIndentation(depth, "UnarySuffix {");
	PrintWithIndentation(depth + 1, "Operator: " + unary.m_op.m_lexeme);
	PrintWithIndentation(depth + 1, "Operand: ");
	Visit(unary.m_expr, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::Spawn& spawn, int depth) const
{
	PrintWithIndentation(depth, "Spawn {");
	PrintWithIndentation(depth + 1, "Callee: ");
	Visit(spawn.m_callee, depth + 2);
	PrintWithIndentation(depth + 1, "Args: ");
	std::ranges::for_each
	(
		spawn.m_arguments,
		[depth, this](const std::unique_ptr<MidoriExpression>& argument)
		{
			Visit(argument, depth + 2);
		}
	);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::Join& join, int depth) const
{
	PrintWithIndentation(depth, "Join {");
	PrintWithIndentation(depth + 1, "Worker: ");
	Visit(join.m_worker, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::ChannelCreate& channel_create, int depth) const
{
	PrintWithIndentation(depth, "ChannelCreate {");
	PrintWithIndentation(depth + 1, "ElementType: " + channel_create.m_element_type->ToString());
	PrintWithIndentation(depth + 1, "Capacity: ");
	Visit(channel_create.m_capacity, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::Send& send, int depth) const
{
	PrintWithIndentation(depth, "Send {");
	PrintWithIndentation(depth + 1, "Channel: ");
	Visit(send.m_channel, depth + 2);
	PrintWithIndentation(depth + 1, "Value: ");
	Visit(send.m_value, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::Receive& receive, int depth) const
{
	PrintWithIndentation(depth, "Receive {");
	PrintWithIndentation(depth + 1, "Channel: ");
	Visit(receive.m_channel, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::Call& call, int depth) const
{
	PrintWithIndentation(depth, "Call {");
	PrintWithIndentation(depth + 1, "Callee: ");
	Visit(call.m_callee, depth + 2);
	PrintWithIndentation(depth + 1, "Args: ");
	std::ranges::for_each
	(
		call.m_arguments,
		[depth, this](const std::unique_ptr<MidoriExpression>& arg)
		{
			Visit(arg, depth + 2);
		}
	);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::MemberAccess& get, int depth) const
{
	PrintWithIndentation(depth, "Get {");
	PrintWithIndentation(depth + 1, "MidoriStruct: ");
	Visit(get.m_struct, depth + 2);
	PrintWithIndentation(depth + 1, "Name: " + get.m_member_name.m_lexeme);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::NameAccess& variable, int depth) const
{
	PrintWithIndentation(depth, "BoundedName {");
	PrintWithIndentation(depth + 1, "Name: " + variable.m_name.m_lexeme);
	PrintWithIndentation(depth + 1, "NameContext {");
	PrintVariableSemantic(depth + 2, variable.m_name_ctx);
	PrintWithIndentation(depth + 1, "}");
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::Literal& literal, int depth) const
{
	switch (literal.m_kind)
	{
	case MidoriExpression::LiteralKind::Text:
		PrintWithIndentation(depth, "Text {");
		PrintWithIndentation(depth + 1, "Value: \"" + literal.m_token.m_lexeme + "\"");
		PrintWithIndentation(depth, "}");
		return;
	case MidoriExpression::LiteralKind::Unit:
		PrintWithIndentation(depth, "()");
		return;
	case MidoriExpression::LiteralKind::Bool:
		PrintWithIndentation(depth, "Bool {");
		break;
	case MidoriExpression::LiteralKind::Float:
		PrintWithIndentation(depth, "Float {");
		break;
	case MidoriExpression::LiteralKind::Integer:
		PrintWithIndentation(depth, "Integer {");
		break;
	case MidoriExpression::LiteralKind::Byte:
		PrintWithIndentation(depth, "Byte {");
		break;
	case MidoriExpression::LiteralKind::Word:
		PrintWithIndentation(depth, "Word {");
		break;
	}
	PrintWithIndentation(depth + 1, "Value: " + literal.m_token.m_lexeme);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::Function& closure, int depth) const
{
	PrintAbstractSyntaxTree::PrintWithIndentation(depth, "Function {");
	PrintWithIndentation(depth + 1, "Parameters: ");
	std::ranges::for_each
	(
		std::ranges::views::zip(closure.m_params, closure.m_param_types),
		[depth, this](const auto& param)
		{
			PrintWithIndentation(depth + 2, "ParameterName: " + std::get<0>(param).m_lexeme);
			PrintWithIndentation(depth + 2, "ParameterType: " + std::get<1>(param)->ToString());
		}
	);
	PrintWithIndentation(depth + 1, "ReturnType: " + closure.m_return_type->ToString());
	PrintWithIndentation(depth + 1, "Body: ");
	Visit(closure.m_body, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::Construct& construct, int depth) const
{
	PrintWithIndentation(depth, "Construct {");
	PrintWithIndentation(depth + 1, "Type: " + construct.m_return_type->ToString());
	PrintWithIndentation(depth + 1, "Params: ");
	std::ranges::for_each
	(
		construct.m_params,
		[depth, this](const std::unique_ptr<MidoriExpression>& expr)
		{
			Visit(expr, depth + 2);
		}
	);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::RecordUpdate& record_update, int depth) const
{
	PrintWithIndentation(depth, "RecordUpdate {");
	PrintWithIndentation(depth + 1, "Source: ");
	Visit(record_update.m_source, depth + 2);
	PrintWithIndentation(depth + 1, "Fields: ");
	std::ranges::for_each
	(
		record_update.m_updates,
		[depth, this](const MidoriExpression::RecordUpdate::FieldUpdate& update)
		{
			PrintWithIndentation(depth + 2, update.m_name.m_lexeme + " = ");
			Visit(update.m_value, depth + 3);
		}
	);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::Array& array, int depth) const
{
	PrintWithIndentation(depth, "Array {");
	PrintWithIndentation(depth + 1, "Elements: ");
	std::ranges::for_each
	(
		array.m_elems,
		[depth, this](const std::unique_ptr<MidoriExpression>& element)
		{
			Visit(element, depth + 2);
		}
	);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::IndexAccess& array_get, int depth) const
{
	PrintWithIndentation(depth, "ArrayGet {");
	PrintWithIndentation(depth + 1, "Array: ");
	Visit(array_get.m_arr_var, depth + 2);
	PrintWithIndentation(depth + 1, "Index: ");
	Visit(array_get.m_index, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::PrintAbstractSyntaxTree::operator()(const MidoriExpression::RangeBinary& range_binary, int depth) const
{
	PrintWithIndentation(depth, "RangeBinary {");
	PrintWithIndentation(depth + 1, "Start: ");
	Visit(range_binary.m_start, depth + 2);
	PrintWithIndentation(depth + 1, "End: ");
	Visit(range_binary.m_end, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::PrintAbstractSyntaxTree::operator()(const MidoriExpression::RangeTernary& range_ternary, int depth) const
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

void PrintAbstractSyntaxTree::PrintAbstractSyntaxTree::operator()(const MidoriExpression::IfElse& if_else, int depth) const
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

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::Block& block, int depth) const
{
	PrintWithIndentation(depth, "Block {");
	PrintWithIndentation(depth + 1, "Statements: ");
	std::ranges::for_each
	(
		block.m_stmts, [depth, this](const std::unique_ptr<MidoriStatement>& stmt)
		{
			Visit(stmt, depth + 2);
		}
	);
	if (block.m_final_expr.has_value())
	{
		PrintWithIndentation(depth + 1, "Expression: ");
		Visit(*block.m_final_expr, depth + 2);
	}
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::Match& match, int depth) const
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

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::Case& case_expr, int depth) const
{
	PrintWithIndentation(depth, "Case {");
	PrintWithIndentation(depth + 1, "Pattern: ");
	Visit(case_expr.m_pattern, depth + 2);
	if (case_expr.HasGuard())
	{
		PrintWithIndentation(depth + 1, "Guard: ");
		Visit(case_expr.m_guard.value(), depth + 2);
	}
	PrintWithIndentation(depth + 1, "Value {");
	Visit(case_expr.m_expr, depth + 2);
	PrintWithIndentation(depth + 1, "}");
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriExpression::For& for_expr, int depth) const
{
	PrintWithIndentation(depth, "For {");
	PrintWithIndentation(depth + 1, "Variable: " + std::string(for_expr.m_loop_variable.m_lexeme));
	PrintWithIndentation(depth + 1, "Range: ");
	Visit(for_expr.m_range, depth + 2);
	PrintWithIndentation(depth + 1, "Body: ");
	Visit(for_expr.m_body, depth + 2);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriPattern::Binding& binding, int depth) const
{
	PrintWithIndentation(depth, "BindingPattern {");
	PrintWithIndentation(depth + 1, binding.m_name.m_lexeme);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriPattern::Wildcard& wildcard, int depth) const
{
	PrintWithIndentation(depth, "WildcardPattern {");
	PrintWithIndentation(depth + 1, wildcard.m_token.m_lexeme);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriPattern::Literal& literal, int depth) const
{
	PrintWithIndentation(depth, "LiteralPattern {");
	PrintWithIndentation(depth + 1, literal.m_token.m_lexeme);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriPattern::Tuple& tuple, int depth) const
{
	PrintWithIndentation(depth, "TuplePattern {");
	std::ranges::for_each
	(
		tuple.m_elements,
		[depth, this](const std::unique_ptr<MidoriPattern>& elem)
		{
			Visit(elem, depth + 1);
		}
	);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriPattern::Array& array, int depth) const
{
	PrintWithIndentation(depth, "ArrayPattern {");
	std::ranges::for_each
	(
		array.m_elements,
		[depth, this](const std::unique_ptr<MidoriPattern>& elem)
		{
			Visit(elem, depth + 1);
		}
	);
	PrintWithIndentation(depth, "}");
}

void PrintAbstractSyntaxTree::operator()(const MidoriPattern::Constructor& constructor, int depth) const
{
	PrintWithIndentation(depth, "ConstructorPattern {");
	PrintWithIndentation(depth + 1, constructor.m_name);
	std::ranges::for_each
	(
		constructor.m_args,
		[depth, this](const std::unique_ptr<MidoriPattern>& arg)
		{
			Visit(arg, depth + 1);
		}
	);
	PrintWithIndentation(depth, "}");
}

#endif
