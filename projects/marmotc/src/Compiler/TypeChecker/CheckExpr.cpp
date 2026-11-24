#include "TypeChecker.h"
#include "Compiler/Analysis/AbstractSyntaxTreeWalker.h"
#include "Compiler/Constant/Constant.h"
#include "Compiler/Names/SpecialNames.h"
#include "Compiler/Operator/Operator.h"
#include "Compiler/TypeMatching/InstanceTypeMatcher.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <format>
#include <limits>
#include <ranges>
#include <type_traits>

using namespace std::string_literals;

namespace
{
	const MarmotLiteral* ContextualInteger(const MarmotExpression& expression)
	{
		const MarmotExpression* operand = &expression;
		while (operand->IsExpression<MarmotExpression::Group>())
		{
			operand = operand->GetExpression<MarmotExpression::Group>().m_expr_in.get();
		}
		if (operand->IsLiteral(MarmotLiteralKind::Integer))
		{
			const MarmotLiteral& literal = operand->GetExpression<MarmotExpression::Literal>();
			if (IsPrefixedIntegerLiteral(literal.m_token.m_lexeme))
			{
				return &literal;
			}
		}
		return nullptr;
	}
}

const TypeChecker::LocalGenericFunction* TypeChecker::FindLocalGeneric(const CheckingContext& context, const BindingId& binding) const
{
	for (const TypeScope* scope = context.m_scope; scope != nullptr; scope = scope->m_parent)
	{
		const auto found = std::ranges::find(scope->m_generic_functions, binding, &LocalGenericFunction::m_binding);
		if (found != scope->m_generic_functions.end())
		{
			return &*found;
		}
	}
	return nullptr;
}

const std::array<Token::Name, 5u> TypeChecker::kBinaryArithmeticOperators{
	Token::Name::SINGLE_PLUS,
	Token::Name::SINGLE_MINUS,
	Token::Name::STAR,
	Token::Name::SLASH,
	Token::Name::PERCENT
};
const std::array<Token::Name, 1u> TypeChecker::kBinaryConcatenationOperators{ Token::Name::DOUBLE_PLUS };
const std::array<Token::Name, 4u> TypeChecker::kBinaryPartialOrderComparisonOperators{
	Token::Name::LEFT_ANGLE,
	Token::Name::LESS_EQUAL,
	Token::Name::RIGHT_ANGLE,
	Token::Name::GREATER_EQUAL
};
const std::array<Token::Name, 2u> TypeChecker::kBinaryEqualityOperators{
	Token::Name::DOUBLE_EQUAL,
	Token::Name::BANG_EQUAL
};
const std::array<Token::Name, 2u> TypeChecker::kBinaryLogicalOperators{
	Token::Name::DOUBLE_AMPERSAND,
	Token::Name::DOUBLE_BAR
};
const std::array<Token::Name, 5u> TypeChecker::kBinaryBitwiseOperators{
	Token::Name::CARET,
	Token::Name::SINGLE_AMPERSAND,
	Token::Name::SINGLE_BAR,
	Token::Name::RIGHT_SHIFT,
	Token::Name::LEFT_SHIFT
};

MarmotResult::TypeResult TypeChecker::Evaluate(const CheckingContext& context, const std::unique_ptr<MarmotExpression>& expression, NameUse use)
{
	const MarmotOperator* operation = nullptr;
	std::optional<Token> anchor;
	if (expression->IsExpression<MarmotExpression::Binary>())
	{
		const MarmotExpression::Binary& binary = expression->GetExpression<MarmotExpression::Binary>();
		operation = FindOperator(binary.m_op.m_token_name, false);
		anchor = binary.m_op;
	}
	else if (expression->IsExpression<MarmotExpression::UnaryPrefix>())
	{
		const MarmotExpression::UnaryPrefix& unary = expression->GetExpression<MarmotExpression::UnaryPrefix>();
		operation = FindOperator(unary.m_op.m_token_name, true);
		anchor = unary.m_op;
	}
	if (operation != nullptr && m_classes.contains(std::string(operation->m_class_name)))
	{
		std::vector<std::unique_ptr<MarmotExpression>> arguments;
		if (operation->m_unary)
		{
			arguments.emplace_back(std::move(expression->GetExpression<MarmotExpression::UnaryPrefix>().m_expr));
		}
		else
		{
			MarmotExpression::Binary& binary = expression->GetExpression<MarmotExpression::Binary>();
			arguments.emplace_back(std::move(binary.m_left));
			arguments.emplace_back(std::move(binary.m_right));
		}
		std::unique_ptr<MarmotExpression> callee = std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(Token(operation->MethodName(), Token::Name::IDENTIFIER_LITERAL, anchor.value()), MarmotExpression::NameContext::Global{}));
		*expression = MarmotExpression(MarmotExpression::Call(anchor.value(), std::move(callee), std::move(arguments)));
	}
	return VisitNode
	(
		[this, &context, &use]<typename T>(T& expr) -> MarmotResult::TypeResult
		{
			if constexpr (std::is_same_v<std::decay_t<decltype(expr)>, MarmotExpression::NameAccess>)
			{
				return (*this)(context, expr, use);
			}
			else
			{
				return (*this)(context, expr);
			}
		},
		expression
	);
}

void TypeChecker::ResolveRecordedTypes(const CheckingContext& context, MarmotExpression& expression)
{
	struct ExpressionResolver
	{
		TypeChecker& m_self;
		const CheckingContext& m_context;

		void Resolve(std::unique_ptr<MarmotExpression>& child) const
		{
			if (child != nullptr)
			{
				m_self.ResolveRecordedTypes(m_context, *child);
			}
		}

		void Resolve(std::vector<std::unique_ptr<MarmotExpression>>& children) const
		{
			std::ranges::for_each(children, [this](std::unique_ptr<MarmotExpression>& child) { Resolve(child); });
		}

		void Resolve(std::shared_ptr<MarmotType>& type) const
		{
			if (type != nullptr)
			{
				type = m_self.ApplySubstitution(m_context, type);
			}
		}

		void operator()(MarmotExpression::As& node) const { Resolve(node.m_to_type); Resolve(node.m_expr); }
		void operator()(MarmotExpression::Binary& node) const { Resolve(node.m_left); Resolve(node.m_right); }
		void operator()(MarmotExpression::Group& node) const { Resolve(node.m_expr_in); }
		void operator()(MarmotExpression::Tuple& node) const { Resolve(node.m_elements); }
		void operator()(MarmotExpression::Literal&) const {}
		void operator()(MarmotExpression::UnaryPrefix& node) const { Resolve(node.m_expr); }
		void operator()(MarmotExpression::NameAccess&) const {}

		void operator()(MarmotExpression::Call& node) const
		{
			Resolve(node.m_callee);
			Resolve(node.m_arguments);
			m_self.CheckCallTypeDecided(node);
		}

		void operator()(MarmotExpression::Function& node) const { Resolve(node.m_body); }
		void operator()(MarmotExpression::Construct& node) const { Resolve(node.m_params); }
		void operator()(MarmotExpression::IfElse& node) const { Resolve(node.m_condition); Resolve(node.m_true_branch); Resolve(node.m_else_branch); }
		void operator()(MarmotExpression::MemberAccess& node) const { Resolve(node.m_struct); }
		void operator()(MarmotExpression::Array& node) const { Resolve(node.m_elems); }
		void operator()(MarmotExpression::IndexAccess& node) const { Resolve(node.m_arr_var); Resolve(node.m_index); }
		void operator()(MarmotExpression::RangeBinary& node) const { Resolve(node.m_start); Resolve(node.m_end); }
		void operator()(MarmotExpression::RangeTernary& node) const { Resolve(node.m_start); Resolve(node.m_step); Resolve(node.m_end); }
		void operator()(MarmotExpression::Match& node) const { Resolve(node.m_arg_expr); Resolve(node.m_cases); }

		void operator()(MarmotExpression::RecordUpdate& node) const
		{
			Resolve(node.m_source);
			std::ranges::for_each(node.m_updates, [this](MarmotExpression::RecordUpdate::FieldUpdate& update) { Resolve(update.m_value); });
		}

		void operator()(MarmotExpression::ArrayComprehension& node) const
		{
			Resolve(node.m_iterable_item_type);
			Resolve(node.m_iterable_next_type);
			Resolve(node.m_transform_expr);
			Resolve(node.m_range);
		}

		void operator()(MarmotExpression::Block& node) const
		{
			std::ranges::for_each(node.m_stmts, [this](std::unique_ptr<MarmotStatement>& statement) { m_self.ResolveRecordedTypes(m_context, *statement); });
			if (node.m_final_expr.has_value())
			{
				Resolve(node.m_final_expr.value());
			}
		}

		void operator()(MarmotExpression::Case& node) const
		{
			m_self.ResolveRecordedTypes(m_context, *node.m_pattern);
			Resolve(node.m_expr);
			if (node.m_guard.has_value())
			{
				Resolve(node.m_guard.value());
			}
		}

		void operator()(MarmotExpression::For& node) const
		{
			Resolve(node.m_iterable_item_type);
			Resolve(node.m_iterable_next_type);
			Resolve(node.m_range);
			Resolve(node.m_body);
		}
	};

	expression.GetType() = ApplySubstitution(context, expression.GetType());
	VisitNode(ExpressionResolver{ *this, context }, expression);
}

// Only the call that brings a variable in reports it: one whose argument already
// holds it, as `Id(Make(3))` does, is not where the type went missing.
void TypeChecker::CheckCallTypeDecided(const MarmotExpression::Call& call)
{
	std::unordered_set<int> argument_variables;
	for (const std::unique_ptr<MarmotExpression>& argument : call.m_arguments)
	{
		argument_variables.merge(CollectTypeVariableIds(argument->GetType()));
	}
	const bool is_undecided = std::ranges::any_of(CollectTypeVariableIds(call.m_type_data), [this, &argument_variables](int id)
	{
		return !m_generic_parameter_ids.contains(id) && !argument_variables.contains(id);
	});
	if (!is_undecided)
	{
		return;
	}

	const bool names_callee = call.m_callee->IsExpression<MarmotExpression::NameAccess>();
	const Token& token = names_callee ? call.m_callee->GetExpression<MarmotExpression::NameAccess>().m_name : call.m_paren;
	const std::string what = names_callee ? std::format("'{}' returns here", token.m_lexeme) : std::string("this call returns");
	m_undecided_call_errors.push_back(MarmotError::GenerateTypeCheckerErrorWithContext(std::format("Call expression type error: could not infer the type {}; write the type on the definition that holds the result", what), token, m_file_name, m_source_lines));
}

MarmotResult::TypeResult TypeChecker::CheckFunctionBody(const CheckingContext& context, const std::unique_ptr<MarmotExpression>& body, std::shared_ptr<MarmotType>& return_type, const std::vector<MarmotType::ClassConstraint>& constraints, const Token& token)
{
	const CheckingContext result_context = context.WithExpectedType(return_type);
	const CheckingContext body_context = context.ForFunction(return_type, constraints);
	// The body's assumptions cannot justify its result outside the body.
	return Evaluate(body_context, body).and_then([&](std::shared_ptr<MarmotType>&& body_type)
	{
		return Unify(result_context, token, return_type, body_type, UnifyDiagnosticMode::ExpectedActual);
	});
}

void TypeChecker::InferCaptureTransferability(const CheckingContext& context, MarmotExpression::Function& function, const BindingId& self)
{
	class CaptureFinder final : public MarmotAbstractSyntaxTreeWalker
	{
	public:
		std::vector<const MarmotExpression::NameAccess*> Find(MarmotExpression::Function& function)
		{
			(*this)(function);
			return std::move(m_captures);
		}

	private:
		void operator()(MarmotExpression::Function& function) override
		{
			m_local_functions.insert(function.m_params.Function());
			MarmotAbstractSyntaxTreeWalker::operator()(function);
		}

		void operator()(MarmotExpression::NameAccess& name) override
		{
			const auto* capture = std::get_if<MarmotExpression::NameContext::Cell>(&name.m_name_ctx);
			if (capture != nullptr && !m_local_functions.contains(capture->m_binding->m_function) && m_seen.insert(capture->m_binding).second)
			{
				m_captures.push_back(&name);
			}
		}

		std::unordered_set<LexicalFunctionId> m_local_functions;
		std::unordered_set<BindingId> m_seen;
		std::vector<const MarmotExpression::NameAccess*> m_captures;
	};

	MarmotType::FunctionType& signature = function.m_type_data->GetType<MarmotType::FunctionType>();
	signature.m_is_transferable = std::ranges::all_of
	(
		CaptureFinder().Find(function),
		[&](const MarmotExpression::NameAccess* capture)
		{
			const BindingId& binding = std::get<MarmotExpression::NameContext::Cell>(capture->m_name_ctx).m_binding;
			// A recursive self-capture points back to the closure being checked.
			return binding == self || !EnsureTransferable(context, capture->m_name, capture->m_type_data).has_value();
		}
	);
}

MarmotResult::TypeResult TypeChecker::CheckGenericLambda(const CheckingContext& context, MarmotExpression::Function& function, const Token& anchor, const BindingId& self, const std::function<void(const std::shared_ptr<MarmotType>&, const std::vector<int>&)>& bind)
{
	// Validate that generic parameter names are unique
	std::unordered_set<std::string> generic_param_names;
	for (const Token& generic_param : function.m_generic_params)
	{
		if (!generic_param_names.insert(generic_param.m_lexeme).second)
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Function expression type error: duplicate generic parameter name", generic_param, m_file_name, m_source_lines));
		}
	}

	// Validate that generic parameters don't conflict with function parameters
	for (size_t index = 0u; index < function.m_params.Size(); index += 1u)
	{
		if (function.m_params.GeneratedBindingAt(index) != nullptr)
		{
			continue;
		}
		const Token& param = function.m_params.SourceToken(index);
		if (generic_param_names.contains(param.m_lexeme))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Function expression type error: generic parameter conflicts with function parameter", param, m_file_name, m_source_lines));
		}
	}

	std::vector<MarmotType::ClassConstraint> propagated_constraints = CollectSignatureConstraints(function.m_param_types, function.m_return_type);
	for (MarmotType::ClassConstraint& propagated_constraint : propagated_constraints)
	{
		AppendUniqueConstraint(function.m_constraints, std::move(propagated_constraint));
	}

	for (const MarmotType::ClassConstraint& constraint : function.m_constraints)
	{
		if (!m_classes.contains(ConstrainedClassName(constraint)))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Function expression type error: undefined class '" + ConstrainedClassName(constraint) + "' in constraint", function.m_function_keyword, m_file_name, m_source_lines));
		}

		// An equality constraint names no class of its own, so the arity check
		// below does not apply to it.
		if (constraint.IsEquality())
		{
			continue;
		}

		const ClassInfo& tc_info = m_classes.at(constraint.m_class_name);

		if (constraint.m_type_args.size() != tc_info.m_type_param_names.size())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Function expression type error: class '" + constraint.m_class_name + "' expects " + std::to_string(tc_info.m_type_param_names.size()) + " type argument(s) but got " + std::to_string(constraint.m_type_args.size()), function.m_function_keyword, m_file_name, m_source_lines));
		}
	}

	TypeTransform::Freshener freshening_context([this]() { return FreshTypeVar(); });
	for (std::shared_ptr<MarmotType>& param_type : function.m_param_types)
	{
		param_type = freshening_context.Apply(param_type);
	}
	function.m_return_type = freshening_context.Apply(function.m_return_type);
	// An equality constraint's types are its operands, which Map freshens with the
	// signature: otherwise the projection would never match the one built at the
	// use site.
	function.m_constraints = MarmotType::MapConstraints(function.m_constraints, [&freshening_context](const std::shared_ptr<MarmotType>& constraint_type) { return freshening_context.Apply(constraint_type); });

	std::shared_ptr<MarmotType> return_type_copy = function.m_return_type;
	std::shared_ptr<MarmotType> function_type = MarmotType::MakeFunctionType(function.m_param_types, std::move(return_type_copy));
	MarmotType::FunctionType& signature = function_type->GetType<MarmotType::FunctionType>();
	signature.m_constraints = function.m_constraints;
	signature.m_is_transferable = context.m_scope->m_parent == nullptr;
	ApplyTransferableParameterConstraints(signature);
	function.m_param_types = signature.m_param_types;
	function.m_constraints = signature.m_constraints;
	function.m_type_data = function_type;
	bind(function_type, freshening_context.Bindings()
		| std::views::values
		| std::views::filter([](const std::shared_ptr<MarmotType>& type) { return type->IsType<MarmotType::TypeVariable>(); })
		| std::views::transform([](const std::shared_ptr<MarmotType>& type) { return type->GetType<MarmotType::TypeVariable>().m_id; })
		| std::ranges::to<std::vector>());

	return InScope
	(
		context,
		[&](const CheckingContext& context) -> MarmotResult::TypeResult
		{
			for (const Token& generic_param : function.m_generic_params)
			{
				const std::string& param_name = generic_param.m_lexeme;
				context.m_scope->m_names.emplace(param_name, freshening_context.Parameter(param_name));
			}

			BindParameters(context, function.m_params, function.m_param_types);

			// Inside its own body, a recursive call is not a fresh instantiation.
			const GenericContext generic = ForGenericParameters(context, freshening_context.Bindings(), function.m_params.Function());
			const size_t pending_start = m_pending_instances.size();
			return CheckFunctionBody(generic.m_context, function.m_body, function.m_return_type, function.m_constraints, function.m_function_keyword)
				.and_then
				(
					[&function, &context, &generic, pending_start, &anchor, &self, this](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
					{
						if (std::optional<CompilerError> error = CheckGenericParametersStayGeneric(generic.m_context, generic.m_parameters, function.m_generic_params, anchor))
						{
							return std::unexpected(std::move(*error));
						}
						if (std::optional<CompilerError> error = CheckDecidedPendingInstances(generic.m_context, pending_start))
						{
							return std::unexpected(std::move(*error));
						}
						InferCaptureTransferability(context, function, self);
						return MarmotType::MakeUndecidedType();
					}
				);
		}
	);
}

// A `for` loop and an array comprehension iterate the same things the same way:
// a Range, an Array, or anything with an Iterable instance or constraint. This
// decides which, records it on the node, and gives the element type.
template <typename IterationNode>
MarmotResult::TypeResult TypeChecker::ResolveIteration(const CheckingContext& context, IterationNode& node, const std::shared_ptr<MarmotType>& range_type, std::string_view error_prefix)
{
	const std::shared_ptr<MarmotType> resolved_range = ApplySubstitution(context, range_type);
	const Token& token = node.m_in_keyword;
	const std::function<CompilerError(std::string_view, const std::shared_ptr<MarmotType>&)> fail = [this, &token, error_prefix](std::string_view message, const std::shared_ptr<MarmotType>& shown_type)
	{
		return MarmotError::GenerateTypeCheckerErrorWithContext(std::format("{} type error: {}", error_prefix, message), token, m_file_name, m_source_lines, shown_type);
	};

	if (resolved_range->IsType<MarmotType::RangeType>())
	{
		node.m_is_array_iteration = false;
		node.m_is_iterable_iteration = false;
		return resolved_range->GetType<MarmotType::RangeType>().m_element_type;
	}
	if (resolved_range->IsType<MarmotType::ArrayType>())
	{
		node.m_is_array_iteration = true;
		node.m_is_iterable_iteration = false;
		return resolved_range->GetType<MarmotType::ArrayType>().m_element_type;
	}

	std::unordered_map<std::string, ClassInfo>::iterator class_it = m_classes.find(std::string(ITERABLE_CLASS_NAME));
	if (class_it == m_classes.end())
	{
		return std::unexpected(fail("Iterable class not found", resolved_range));
	}

	const bool uses_associated_item = class_it->second.m_associated_types.contains(std::string(ITEM_ASSOCIATED_TYPE_NAME));
	std::shared_ptr<MarmotType> iterable_item_type;
	bool is_iterable = false;

	// A bare variable stands for a type nothing is known about yet, so only a
	// where clause can say it is iterable; any other type may match an instance,
	// generic or not.
	if (!resolved_range->IsType<MarmotType::TypeVariable>())
	{
		if (uses_associated_item)
		{
			std::optional<ResolvedInstanceMatch> resolved_match = FindMatchingInstance(std::string(ITERABLE_CLASS_NAME), { resolved_range });
			if (resolved_match.has_value())
			{
				AssociatedTypeEnvironment::const_iterator binding_it = resolved_match->m_instance->m_associated_type_bindings.find(std::string(ITEM_ASSOCIATED_TYPE_NAME));
				if (binding_it != resolved_match->m_instance->m_associated_type_bindings.cend())
				{
					iterable_item_type = ApplySubstitution(context, TypeTransform::Instantiate(binding_it->second, resolved_match->m_substitutions));
					is_iterable = true;
				}
			}
		}
		else
		{
			for (const auto& [key, info] : m_instances)
			{
				if (info.m_class_name != ITERABLE_CLASS_NAME || info.m_type_args.size() != 2u)
				{
					continue;
				}

				InstanceTypeMatcher matcher;
				if (!matcher.Match(info.m_type_args[0u], resolved_range))
				{
					continue;
				}

				if (is_iterable)
				{
					return std::unexpected(fail("ambiguous Iterable instance for iterator type", resolved_range));
				}
				iterable_item_type = TypeTransform::Instantiate(info.m_type_args[1u], matcher.Bindings());
				is_iterable = true;
			}
		}
	}

	if (!is_iterable)
	{
		for (const MarmotType::ClassConstraint& constraint : context.m_assumptions->m_constraints)
		{
			if (constraint.m_class_name == ITERABLE_CLASS_NAME && constraint.m_type_args.size() == class_it->second.m_type_param_names.size() && !constraint.m_type_args.empty() && *ApplySubstitution(context, constraint.m_type_args[0u]) == *resolved_range)
			{
				if (uses_associated_item)
				{
					iterable_item_type = ResolveAssociatedType(context, MarmotType::AssociatedType(std::string(ITERABLE_CLASS_NAME), std::string(ITEM_ASSOCIATED_TYPE_NAME), std::vector<std::shared_ptr<MarmotType>>(constraint.m_type_args)));
				}
				else if (constraint.m_type_args.size() >= 2u)
				{
					iterable_item_type = constraint.m_type_args[1u];
				}
				is_iterable = true;
				break;
			}
		}
	}

	if (!is_iterable)
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(std::format("{} type error: expected Range, Array, or Iterable type for iteration", error_prefix), token, m_file_name, m_source_lines, resolved_range, MarmotType::MakeRangeType(MarmotType::MakeLiteralType<MarmotType::IntegerType>())));
	}

	std::unordered_map<std::string, std::shared_ptr<MarmotType>>::iterator method_it = class_it->second.m_method_types.find(std::string(NEXT_METHOD_NAME));
	if (method_it == class_it->second.m_method_types.end())
	{
		return std::unexpected(fail("Iterable::Next not found", resolved_range));
	}

	TypeEnvironment substitutions;
	const std::vector<std::string>& type_params = class_it->second.m_type_param_names;
	if (!type_params.empty())
	{
		substitutions[type_params[0u]] = resolved_range;
		if (!uses_associated_item && type_params.size() >= 2u && iterable_item_type != nullptr)
		{
			substitutions[type_params[1u]] = iterable_item_type;
		}
	}

	std::shared_ptr<MarmotType> next_type = ApplySubstitution(context, TypeTransform::Instantiate(method_it->second, substitutions));
	if (!next_type->IsType<MarmotType::FunctionType>())
	{
		return std::unexpected(fail("Iterable::Next must be a function", resolved_range));
	}

	std::shared_ptr<MarmotType> next_return = ApplySubstitution(context, next_type->GetType<MarmotType::FunctionType>().m_return_type);
	if (!next_return->IsType<MarmotType::UnionType>())
	{
		return std::unexpected(fail("Iterable::Next must return an Option-like union", next_return));
	}

	const MarmotType::UnionType& option_union = next_return->GetType<MarmotType::UnionType>();
	const std::string some_name = option_union.m_name + std::string(NameSeparator) + std::string(ITERABLE_SOME_CONSTRUCTOR_NAME);
	if (!option_union.m_member_info.contains(some_name))
	{
		return std::unexpected(fail("Iterable::Next return type is missing 'Some' constructor", next_return));
	}

	const MarmotType::UnionType::UnionMemberContext& some_ctx = option_union.m_member_info.at(some_name);
	if (some_ctx.m_member_types.size() != 1u)
	{
		return std::unexpected(fail("Iterable::Next 'Some' constructor must contain exactly one value", next_return));
	}

	// Next returns Option<(Item, Iter)>. The payload is a pair whose first
	// element is the item and whose second is the advanced iterator, so the
	// loop variable takes element 0 rather than the payload itself.
	std::shared_ptr<MarmotType> some_payload = ApplySubstitution(context, some_ctx.m_member_types[0u]);
	if (!some_payload->IsType<MarmotType::TupleType>() || some_payload->GetType<MarmotType::TupleType>().m_element_types.size() != 2u)
	{
		return std::unexpected(fail("Iterable::Next must return Option<(Item, Iter)>", some_payload));
	}

	iterable_item_type = ApplySubstitution(context, some_payload->GetType<MarmotType::TupleType>().m_element_types[0u]);
	node.m_is_array_iteration = false;
	node.m_is_iterable_iteration = true;
	node.m_iterable_item_type = iterable_item_type;
	node.m_iterable_some_tag = some_ctx.m_tag;
	node.m_iterable_next_type = next_return;
	return iterable_item_type;
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::For& for_expr)
{
	return Evaluate(context, for_expr.m_range)
		.and_then([&for_expr, this, &context](std::shared_ptr<MarmotType>&& range_type) { return ResolveIteration(context, for_expr, range_type, "For loop expression"); })
		.and_then
		(
			[&for_expr, this, &context](std::shared_ptr<MarmotType>&& element_type) -> MarmotResult::TypeResult
			{
				TypeScope scope(context.m_scope);
				const CheckingContext scope_context = context.WithScope(scope);
				BindValue(scope_context, for_expr.m_loop_variable, for_expr.m_loop_binding, element_type);

				return Evaluate(scope_context, for_expr.m_body)
					.and_then
					(
						[&for_expr](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
						{
							// A `for` loop consumes an iterable for its effects, so it is Unit.
							// It used to take its type from whatever `break` carried out of it.
							for_expr.m_type_data = MarmotType::MakeLiteralType<MarmotType::UnitType>();
							return for_expr.m_type_data;
						}
					);
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::ArrayComprehension& comp)
{
	return Evaluate(context, comp.m_range)
		.and_then([&comp, this, &context](std::shared_ptr<MarmotType>&& range_type) { return ResolveIteration(context, comp, range_type, "Array comprehension"); })
		.and_then
		(
			[&comp, this, &context](std::shared_ptr<MarmotType>&& element_type) -> MarmotResult::TypeResult
			{
				TypeScope scope(context.m_scope);
				const CheckingContext scope_context = context.WithScope(scope);
				BindValue(scope_context, comp.m_loop_variable, comp.m_loop_binding, element_type);

				// The transform expression produces one element, so its expected type is
				// the expected array's element type, never the array type itself. Leaving
				// the ambient type in place pushes Array<T> into a construction here,
				// which then cannot infer its own arguments.
				const std::shared_ptr<MarmotType> ambient_expected_type = scope_context.m_expected_expr_type;
				const CheckingContext transform_context = scope_context.WithExpectedType((ambient_expected_type != nullptr && ambient_expected_type->IsType<MarmotType::ArrayType>()) ? ambient_expected_type->GetType<MarmotType::ArrayType>().m_element_type : std::shared_ptr<MarmotType>{});

				return Evaluate(transform_context, comp.m_transform_expr)
					.and_then
					(
						[&comp, this, &transform_context](std::shared_ptr<MarmotType>&& transform_type) -> MarmotResult::TypeResult
						{
							comp.m_type_data = MarmotType::MakeArrayType(ApplySubstitution(transform_context, transform_type));
							return comp.m_type_data;
						}
					);
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::As& as)
{
	// A cast's operand is typed on its own terms. The ambient expected type
	// describes the result of the cast, not its source, so leaving it in place
	// pushes the target type into a construction on the left of 'as'.
	const CheckingContext operand_context = context.WithExpectedType(std::shared_ptr<MarmotType>{});
	as.m_to_type = InstantiateWrittenType(operand_context, as.m_to_type);

	return Evaluate(operand_context, as.m_expr)
		.and_then
		(
			[&as, this, &operand_context](std::shared_ptr<MarmotType>&& expr_type) ->MarmotResult::TypeResult
			{
				// Instance selection matches the resolved types, so a variable inference
				// has bound casts as its type, and a generic instance is found.
				const std::shared_ptr<MarmotType> from_type = ApplySubstitution(operand_context, expr_type);
				const std::shared_ptr<MarmotType> to_type = ApplySubstitution(operand_context, as.m_to_type);
				const MarmotType::ClassConstraint conversion(std::string(CONVERTABLE_CLASS_NAME), { from_type, to_type });
				const bool has_convertable_instance = !FindMatchingInstances(conversion.m_class_name, conversion.m_type_args).empty();
				const bool has_convertable_constraint = !has_convertable_instance && IsSatisfiedByActiveConstraint(operand_context, conversion);

				// Check if this is a built-in conversion
				bool is_builtin_conversion = false;
				if (to_type->IsType<MarmotType::StructType>() && from_type->IsType<MarmotType::StructType>())
				{
					if (!has_convertable_instance && !has_convertable_constraint)
					{
						const MarmotType::StructType& from_struct_type = from_type->GetType<MarmotType::StructType>();
						const MarmotType::StructType& to_struct_type = to_type->GetType<MarmotType::StructType>();
						if (to_struct_type.m_member_types.size() != from_struct_type.m_member_types.size())
						{
							return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Type cast expression type error: struct member count mismatch", as.m_as_keyword, m_file_name, m_source_lines, from_type, to_type));
						}

						for (size_t i : std::views::iota(0u, to_struct_type.m_member_types.size()))
						{
							if (*from_struct_type.m_member_types[i] != *to_struct_type.m_member_types[i])
							{
								return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Type cast expression type error: struct member type mismatch", as.m_as_keyword, m_file_name, m_source_lines, from_struct_type.m_member_types[i], to_struct_type.m_member_types[i]));
							}
						}
						is_builtin_conversion = true;
					}
				}
				else if (to_type->IsType<MarmotType::StructType>() && !has_convertable_instance && !has_convertable_constraint)
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Type cast expression type error: cannot cast to struct type", as.m_as_keyword, m_file_name, m_source_lines, from_type, to_type));
				}

				// Check for built-in primitive conversions
				bool const is_from_int = from_type->IsType<MarmotType::IntegerType>();
				bool const is_from_float = from_type->IsType<MarmotType::FloatType>();
				bool const is_from_text = from_type->IsType<MarmotType::TextType>();
				bool const is_from_byte = from_type->IsType<MarmotType::ByteType>();
				bool const is_from_word = from_type->IsType<MarmotType::WordType>();
				bool const is_from_bool = from_type->IsType<MarmotType::BoolType>();

				bool const is_to_int = to_type->IsType<MarmotType::IntegerType>();
				bool const is_to_float = to_type->IsType<MarmotType::FloatType>();
				bool const is_to_text = to_type->IsType<MarmotType::TextType>();
				bool const is_to_byte = to_type->IsType<MarmotType::ByteType>();
				bool const is_to_word = to_type->IsType<MarmotType::WordType>();

				if ((is_from_int && (is_to_float || is_to_text || is_to_byte || is_to_word)) ||
					(is_from_float && (is_to_int || is_to_text || is_to_byte || is_to_word)) ||
					(is_from_text && (is_to_int || is_to_float)) ||
					(is_from_byte && (is_to_int || is_to_float || is_to_word || is_to_text)) ||
					(is_from_word && (is_to_int || is_to_float || is_to_byte || is_to_text)) ||
					(is_from_bool && is_to_text))
				{
					is_builtin_conversion = true;
				}

				// `x as T` where x already has type T is a no-op, so it needs no Convertable instance.
				const bool is_identity_conversion = *from_type == *to_type;

				// Verify that either Convertable instance exists, constraint exists, or it's a built-in conversion
				if (!has_convertable_instance && !has_convertable_constraint && !is_builtin_conversion && !is_identity_conversion)
				{
					MarmotType::ClassConstraint constraint(std::string(CONVERTABLE_CLASS_NAME), { from_type, to_type });
					const std::string suggestion = std::format("Define 'instance Convertable<{}, {}>' to enable this conversion.", from_type->DisplayString(), to_type->DisplayString());
					return std::unexpected(MakeConstraintFailureError(operand_context, as.m_as_keyword, constraint, suggestion));
				}

				as.m_type_data = to_type;

				// Prefer built-in code generation for concrete built-in casts so Convertable
				// instances can implement Convert via `as` without recursing back into themselves.
				// Constraints still require Convertable dispatch because the concrete conversion
				// is only known during specialization.
				as.m_uses_convertable = has_convertable_constraint || (has_convertable_instance && !is_builtin_conversion);
				if (has_convertable_instance && !is_builtin_conversion)
				{
					if (std::optional<CompilerError> error = CheckConstraintHolds(operand_context, as.m_as_keyword, conversion, 0u))
					{
						return std::unexpected(std::move(*error));
					}
				}
				return as.m_type_data;
			}
		);
}

MarmotResult::TypeResult TypeChecker::CheckRefNew(const CheckingContext& context, const Token& token, const std::unique_ptr<MarmotExpression>& value)
{
	std::shared_ptr<MarmotType> expected_element = nullptr;
	const std::shared_ptr<MarmotType> expected_type = context.m_expected_expr_type != nullptr ? ApplySubstitution(context, context.m_expected_expr_type) : nullptr;
	if (expected_type != nullptr && expected_type->IsType<MarmotType::CellType>())
	{
		expected_element = expected_type->GetType<MarmotType::CellType>().m_element_type;
	}

	const CheckingContext expected_context = context.WithExpectedType(expected_element);
	return Evaluate(expected_context, value).and_then([&](std::shared_ptr<MarmotType>&& value_type) -> MarmotResult::TypeResult
	{
		if (expected_element != nullptr)
		{
			return Unify(expected_context, token, value_type, expected_element, UnifyDiagnosticMode::ActualExpected)
				.transform([&](const std::shared_ptr<MarmotType>&) { return MarmotType::MakeCellType(ApplySubstitution(expected_context, expected_element)); });
		}
		return MarmotType::MakeCellType(ApplySubstitution(expected_context, value_type));
	});
}

MarmotResult::TypeResult TypeChecker::CheckRefAccess(const CheckingContext& context, const std::unique_ptr<MarmotExpression>& cell, const std::unique_ptr<MarmotExpression>* value, const Token& token, std::string_view error_message)
{
	MarmotResult::TypeResult cell_result = [this, &cell, &context]() -> MarmotResult::TypeResult
	{
		const CheckingContext cell_context = context.WithExpectedType(nullptr);
		return Evaluate(cell_context, cell);
	}();
	if (!cell_result.has_value())
	{
		return cell_result;
	}

	std::shared_ptr<MarmotType> cell_type = ApplySubstitution(context, cell_result.value());
	if (cell_type->IsType<MarmotType::TypeVariable>())
	{
		std::shared_ptr<MarmotType> expected_cell = MarmotType::MakeCellType(FreshTypeVar());
		MarmotResult::TypeResult unified = Unify(context, token, cell_type, expected_cell);
		if (!unified.has_value())
		{
			return unified;
		}
		cell_type = ApplySubstitution(context, cell_type);
	}
	if (!cell_type->IsType<MarmotType::CellType>())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(std::string(error_message), token, m_file_name, m_source_lines, cell_type));
	}

	std::shared_ptr<MarmotType> element_type = cell_type->GetType<MarmotType::CellType>().m_element_type;
	if (value != nullptr)
	{
		const CheckingContext value_context = context.WithExpectedType(element_type);
		MarmotResult::TypeResult value_result = Evaluate(value_context, *value);
		if (!value_result.has_value())
		{
			return value_result;
		}
		MarmotResult::TypeResult unified = Unify(value_context, token, element_type, value_result.value(), UnifyDiagnosticMode::ReferenceWrite);
		if (!unified.has_value())
		{
			return unified;
		}
	}
	return ApplySubstitution(context, element_type);
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::Binary& binary)
{
	if (binary.m_op.m_token_name == Token::Name::COLON_EQUAL)
	{
		return CheckRefAccess(context, binary.m_left, &binary.m_right, binary.m_op, "Reference write operator requires a Ref<T>")
			.transform
			(
				[&binary](const std::shared_ptr<MarmotType>&)
				{
					binary.m_type_data = MarmotType::MakeLiteralType<MarmotType::UnitType>();
					return binary.m_type_data;
				}
			);
	}

	return CheckBinaryOperands(context, binary.m_op.m_token_name, binary.m_left, binary.m_right)
		.and_then([&](BinaryOperandTypes&& operands)
		{
			return CheckBinaryOperator(context, binary, operands[0u], operands[1u]);
		});
}

std::expected<TypeChecker::BinaryOperandTypes, CompilerError> TypeChecker::CheckBinaryOperands(
	const CheckingContext& context, Token::Name op,
	const std::unique_ptr<MarmotExpression>& left, const std::unique_ptr<MarmotExpression>& right)
{
	// Comparisons return Bool independently of their operand types. Shifts
	// always take an Int amount, regardless of the type being shifted.
	const bool compares = op == Token::Name::DOUBLE_EQUAL || op == Token::Name::BANG_EQUAL
		|| op == Token::Name::LEFT_ANGLE || op == Token::Name::LESS_EQUAL
		|| op == Token::Name::RIGHT_ANGLE || op == Token::Name::GREATER_EQUAL;
	const CheckingContext operand_context = compares ? context.WithExpectedType(nullptr) : context;
	const bool shifts = op == Token::Name::LEFT_SHIFT || op == Token::Name::RIGHT_SHIFT;
	const MarmotLiteral* left_literal = ContextualInteger(*left);
	const MarmotLiteral* right_literal = ContextualInteger(*right);
	const auto needs_word = [](const MarmotLiteral& literal)
	{
		return ParseUnsignedLiteral(literal.m_token.m_lexeme).value_or(std::numeric_limits<uint64_t>::max())
			> static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
	};

	// Let the fixed operand, or a literal that requires Word, establish the type.
	const bool right_first = !shifts && left_literal != nullptr
		&& (right_literal == nullptr || (needs_word(*right_literal) && !needs_word(*left_literal)));
	const std::unique_ptr<MarmotExpression>& first = right_first ? right : left;
	const std::unique_ptr<MarmotExpression>& second = right_first ? left : right;
	MarmotResult::TypeResult first_type = Evaluate(operand_context, first);
	if (!first_type.has_value())
	{
		return std::unexpected(std::move(first_type.error()));
	}

	std::shared_ptr<MarmotType> expected = operand_context.m_expected_expr_type;
	if (shifts)
	{
		expected = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	}
	else if (right_first || right_literal != nullptr)
	{
		const std::shared_ptr<MarmotType> peer = ApplySubstitution(context, first_type.value());
		if (peer->IsType<MarmotType::IntegerType>() || peer->IsType<MarmotType::WordType>() || peer->IsType<MarmotType::ByteType>())
		{
			expected = peer;
		}
	}

	MarmotResult::TypeResult second_type = Evaluate(operand_context.WithExpectedType(std::move(expected)), second);
	if (!second_type.has_value())
	{
		return std::unexpected(std::move(second_type.error()));
	}

	BinaryOperandTypes types{ std::move(first_type.value()), std::move(second_type.value()) };
	if (right_first)
	{
		std::swap(types[0u], types[1u]);
	}
	return types;
}

// The class an operand must belong to for an operator to apply. A generic
// parameter belongs to one only where its definition says so, and it is shown
// by the name it was written as, with the where clause that would say so.
std::optional<CompilerError> TypeChecker::RequireOperandClass(const CheckingContext& context, const Token& op, std::string_view class_name, const std::shared_ptr<MarmotType>& resolved_operand)
{
	std::optional<CompilerError> error = CheckConstraintHolds(context, op, MarmotType::ClassConstraint(std::string(class_name), { resolved_operand }), 0u);
	const std::optional<std::string> param_name = RigidParameterName(context, resolved_operand);
	if (!error.has_value() || !param_name.has_value())
	{
		return error;
	}

	const std::string_view declaring_file = class_name;
	const std::string hint = m_classes.contains(std::string(class_name))
		? std::format("'{0}' is a generic parameter, so it is only known to be one when the definition says 'where {1}<{0}>'.", param_name.value(), class_name)
		: std::format("'{0}' is a generic parameter. Import MarmotPrelude/{2}.mmt, which declares {1}, and say 'where {1}<{0}>' on the definition.", param_name.value(), class_name, declaring_file);
	return MakeConstraintFailureError(context, op, MarmotType::ClassConstraint(std::string(class_name), { MarmotType::MakeGenericType(param_name.value()) }), hint);
}

// An operand whose type is still a variable is one inference has not decided
// yet, and an operator may decide it. A generic parameter is a variable too, but
// it stands for whatever type a caller picks, so an operator applies to it only
// where a where clause names the class that provides the operator.
MarmotResult::TypeResult TypeChecker::CheckBinaryOperator(const CheckingContext& context, MarmotExpression::Binary& binary, std::shared_ptr<MarmotType>& left_type, std::shared_ptr<MarmotType>& right_type)
{
	const Token& op = binary.m_op;
	if (op.m_token_name == Token::Name::LEFT_SHIFT || op.m_token_name == Token::Name::RIGHT_SHIFT)
	{
		const std::shared_ptr<MarmotType> resolved_left = ApplySubstitution(context, left_type);
		const std::shared_ptr<MarmotType> resolved_right = ApplySubstitution(context, right_type);

		if (resolved_right->IsType<MarmotType::TypeVariable>() && !RigidParameterName(context, resolved_right).has_value())
		{
			std::shared_ptr<MarmotType> int_type = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
			MarmotResult::TypeResult unified = Unify(context, op, right_type, int_type, UnifyDiagnosticMode::ActualExpected);
			if (!unified.has_value())
			{
				return unified;
			}
		}
		else if (const std::optional<std::string> amount_param = RigidParameterName(context, resolved_right))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Shift operator type error: shift amount must be Int", op, m_file_name, m_source_lines, MarmotType::MakeGenericType(amount_param.value()), MarmotType::MakeLiteralType<MarmotType::IntegerType>()));
		}
		else if (!resolved_right->IsType<MarmotType::IntegerType>())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Shift operator type error: shift amount must be Int", op, m_file_name, m_source_lines, resolved_right, MarmotType::MakeLiteralType<MarmotType::IntegerType>()));
		}

		if (RigidParameterName(context, resolved_left).has_value())
		{
			if (std::optional<CompilerError> error = CheckConstraintHolds(context, op, MarmotType::ClassConstraint(std::string(FindOperator(op.m_token_name, false)->m_class_name), { resolved_left, resolved_right }), 0u))
			{
				return std::unexpected(std::move(*error));
			}
		}
		else if (!resolved_left->IsType<MarmotType::IntegerType>() && !resolved_left->IsType<MarmotType::ByteType>() && !resolved_left->IsType<MarmotType::WordType>() && !resolved_left->IsType<MarmotType::TypeVariable>())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Shift operator type error: can only shift Int, Byte, or Word types", op, m_file_name, m_source_lines, resolved_left, MarmotType::MakeLiteralType<MarmotType::IntegerType>(), MarmotType::MakeLiteralType<MarmotType::ByteType>(), MarmotType::MakeLiteralType<MarmotType::WordType>()));
		}

		binary.m_type_data = left_type;
		return binary.m_type_data;
	}

	MarmotResult::TypeResult unified = Unify(context, op, left_type, right_type);
	if (!unified.has_value())
	{
		return unified;
	}

	const std::shared_ptr<MarmotType> resolved = ApplySubstitution(context, left_type);
	const std::optional<std::string> param_name = RigidParameterName(context, resolved);
	const bool is_undecided = resolved->IsType<MarmotType::TypeVariable>() && !param_name.has_value();
	binary.m_type_data = left_type;

	if (std::ranges::contains(kBinaryPartialOrderComparisonOperators, op.m_token_name))
	{
		if (!resolved->IsNumericType() && !is_undecided)
		{
			if (std::optional<CompilerError> error = RequireOperandClass(context, op, ORDERABLE_CLASS_NAME, resolved))
			{
				return std::unexpected(std::move(*error));
			}
			binary.m_uses_orderable = true;
		}

		binary.m_type_data = MarmotType::MakeLiteralType<MarmotType::BoolType>();
		return binary.m_type_data;
	}

	if (std::ranges::contains(kBinaryArithmeticOperators, op.m_token_name))
	{
		if (param_name.has_value())
		{
			if (std::optional<CompilerError> error = CheckConstraintHolds(context, op, MarmotType::ClassConstraint(std::string(FindOperator(op.m_token_name, false)->m_class_name), { resolved, resolved }), 0u, "Import MarmotPrelude/Numeric.mmt and constrain the operation used by the generic body."))
			{
				return std::unexpected(std::move(*error));
			}
		}
		else if (!resolved->IsNumericType() && !is_undecided)
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Binary expression type error: expected numeric type", op, m_file_name, m_source_lines, resolved, MarmotType::MakeLiteralType<MarmotType::IntegerType>(), MarmotType::MakeLiteralType<MarmotType::FloatType>()));
		}
		return binary.m_type_data;
	}

	if (std::ranges::contains(kBinaryBitwiseOperators, op.m_token_name))
	{
		if (param_name.has_value())
		{
			if (std::optional<CompilerError> error = CheckConstraintHolds(context, op, MarmotType::ClassConstraint(std::string(FindOperator(op.m_token_name, false)->m_class_name), { resolved, resolved }), 0u))
			{
				return std::unexpected(std::move(*error));
			}
		}
		else if (!resolved->IsType<MarmotType::IntegerType>() && !resolved->IsType<MarmotType::ByteType>() && !resolved->IsType<MarmotType::WordType>() && !is_undecided)
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Binary expression type error: expected integer, byte, or word type", op, m_file_name, m_source_lines, resolved, MarmotType::MakeLiteralType<MarmotType::IntegerType>(), MarmotType::MakeLiteralType<MarmotType::ByteType>(), MarmotType::MakeLiteralType<MarmotType::WordType>()));
		}
		return binary.m_type_data;
	}

	if (std::ranges::contains(kBinaryEqualityOperators, op.m_token_name))
	{
		const bool is_builtin = resolved->IsNumericType() || resolved->IsType<MarmotType::TextType>() || resolved->IsType<MarmotType::BoolType>() || is_undecided;
		if (!is_builtin)
		{
			if (std::optional<CompilerError> error = RequireOperandClass(context, op, EQUATABLE_CLASS_NAME, resolved))
			{
				return std::unexpected(std::move(*error));
			}
			binary.m_uses_equatable = true;
		}

		binary.m_type_data = MarmotType::MakeLiteralType<MarmotType::BoolType>();
		return binary.m_type_data;
	}

	if (std::ranges::contains(kBinaryLogicalOperators, op.m_token_name))
	{
		std::shared_ptr<MarmotType> bool_type = MarmotType::MakeLiteralType<MarmotType::BoolType>();
		if (is_undecided)
		{
			MarmotResult::TypeResult bool_result = Unify(context, op, left_type, bool_type, UnifyDiagnosticMode::ActualExpected);
			if (!bool_result.has_value())
			{
				return bool_result;
			}
		}
		else if (!resolved->IsType<MarmotType::BoolType>())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Binary expression type error: expected boolean type", op, m_file_name, m_source_lines, resolved, bool_type));
		}

		binary.m_type_data = bool_type;
		return binary.m_type_data;
	}

	if (std::ranges::contains(kBinaryConcatenationOperators, op.m_token_name))
	{
		if (!resolved->IsType<MarmotType::TextType>() && !resolved->IsType<MarmotType::ArrayType>())
		{
			if (std::optional<CompilerError> error = RequireOperandClass(context, op, CONCATENABLE_CLASS_NAME, resolved))
			{
				return std::unexpected(std::move(*error));
			}
			binary.m_uses_concatenable = true;
		}
	}

	return binary.m_type_data;
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::Group& group)
{
	return Evaluate(context, group.m_expr_in)
		.and_then
		(
			[&group](std::shared_ptr<MarmotType>&& actual_type) ->MarmotResult::TypeResult
			{
				group.m_type_data = std::move(actual_type);
				return group.m_type_data;
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::Tuple& tuple)
{
	if (tuple.m_elements.empty())
	{
		// Empty tuple is Unit type
		tuple.m_type_data = MarmotType::MakeLiteralType<MarmotType::UnitType>();
		return tuple.m_type_data;
	}

	std::vector<std::shared_ptr<MarmotType>> element_types;
	element_types.reserve(tuple.m_elements.size());

	const std::shared_ptr<MarmotType> ambient_expected_type = context.m_expected_expr_type;
	const bool expected_tuple_matches = ambient_expected_type != nullptr
		&& ambient_expected_type->IsType<MarmotType::TupleType>()
		&& ambient_expected_type->GetType<MarmotType::TupleType>().m_element_types.size() == tuple.m_elements.size();

	for (size_t index : std::views::iota(0u, tuple.m_elements.size()))
	{
		// An element's expected type is its own slot in the expected tuple, never
		// the whole tuple. Leaving the ambient type in place leaks it into the
		// element, so a construction there would infer against the tuple type.
		const CheckingContext element_context = context.WithExpectedType(expected_tuple_matches ? ambient_expected_type->GetType<MarmotType::TupleType>().m_element_types[index] : std::shared_ptr<MarmotType>{});

		MarmotResult::TypeResult result = Evaluate(element_context, tuple.m_elements[index]);
		if (!result.has_value())
		{
			return result;
		}

		element_types.emplace_back(std::move(result.value()));
	}

	tuple.m_type_data = MarmotType::MakeTupleType(std::move(element_types));
	return tuple.m_type_data;
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::UnaryPrefix& unary)
{
	if (unary.m_op.m_token_name == Token::Name::REF || unary.m_op.m_token_name == Token::Name::STAR)
	{
		MarmotResult::TypeResult result = unary.m_op.m_token_name == Token::Name::REF
			? CheckRefNew(context, unary.m_op, unary.m_expr)
			: CheckRefAccess(context, unary.m_expr, nullptr, unary.m_op, "Dereference operator requires a Ref<T>");
		return result.transform
		(
			[&unary](std::shared_ptr<MarmotType> type)
			{
				unary.m_type_data = std::move(type);
				return unary.m_type_data;
			}
		);
	}

	return Evaluate(context, unary.m_expr)
		.and_then
		(
			[this, &unary, &context](std::shared_ptr<MarmotType>&& operand_type) -> MarmotResult::TypeResult
			{
				std::shared_ptr<MarmotType> actual_type = ApplySubstitution(context, operand_type);
				if (unary.m_op.m_token_name == Token::Name::SINGLE_MINUS || unary.m_op.m_token_name == Token::Name::SINGLE_PLUS)
				{
					if (RigidParameterName(context, actual_type).has_value())
					{
						if (std::optional<CompilerError> error = CheckConstraintHolds(context, unary.m_op, MarmotType::ClassConstraint(std::string(FindOperator(unary.m_op.m_token_name, true)->m_class_name), { actual_type }), 0u))
						{
							return std::unexpected(std::move(*error));
						}
					}
					else if (!actual_type->IsNumericType())
					{
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Unary operator requires numeric type", unary.m_op, m_file_name, m_source_lines, actual_type, MarmotType::MakeLiteralType<MarmotType::IntegerType>(), MarmotType::MakeLiteralType<MarmotType::FloatType>()));
					}
				}
				else if (unary.m_op.m_token_name == Token::Name::BANG)
				{
					if (!actual_type->IsType<MarmotType::BoolType>())
					{
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Logical NOT operator requires boolean type", unary.m_op, m_file_name, m_source_lines, actual_type, MarmotType::MakeLiteralType<MarmotType::BoolType>()));
					}
				}
				else if (unary.m_op.m_token_name == Token::Name::TILDE)
				{
					if (RigidParameterName(context, actual_type).has_value())
					{
						if (std::optional<CompilerError> error = CheckConstraintHolds(context, unary.m_op, MarmotType::ClassConstraint(std::string(FindOperator(unary.m_op.m_token_name, true)->m_class_name), { actual_type }), 0u))
						{
							return std::unexpected(std::move(*error));
						}
					}
					else if (!actual_type->IsType<MarmotType::IntegerType>() && !actual_type->IsType<MarmotType::ByteType>() && !actual_type->IsType<MarmotType::WordType>())
					{
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Bitwise NOT operator requires an integer, byte, or word type", unary.m_op, m_file_name, m_source_lines, actual_type, MarmotType::MakeLiteralType<MarmotType::IntegerType>(), MarmotType::MakeLiteralType<MarmotType::ByteType>(), MarmotType::MakeLiteralType<MarmotType::WordType>()));
					}
				}
				else if (unary.m_op.m_token_name == Token::Name::HASH)
				{
					if (actual_type->IsType<MarmotType::ArrayType>())
					{
						unary.m_type_data = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
						return unary.m_type_data;
					}

					const std::string suggestion = m_classes.contains(std::string(COUNTABLE_CLASS_NAME))
						? std::format("Define 'instance Countable<{}>' to give it a length.", actual_type->DisplayString())
						: actual_type->IsType<MarmotType::TextType>()
						? std::string("Import MarmotPrelude/Countable.mmt: it declares Countable and its instance for Text.")
						: std::format("Import MarmotPrelude/Countable.mmt, which declares Countable, and define 'instance Countable<{}>'.", actual_type->DisplayString());
					if (std::optional<CompilerError> error = CheckConstraintHolds(context, unary.m_op, MarmotType::ClassConstraint(std::string(COUNTABLE_CLASS_NAME), { actual_type }), 0u, suggestion))
					{
						return std::unexpected(std::move(*error));
					}

					unary.m_uses_countable = true;
					unary.m_type_data = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
					return unary.m_type_data;
				}

				unary.m_type_data = std::move(actual_type);
				return unary.m_type_data;
			}
		);
}

// Spawn(argument, F) runs F in a new worker. Its one argument stands for all of
// F's parameters, which no signature can say, and both what F takes and what it
// gives back must be able to cross.
MarmotResult::TypeResult TypeChecker::CheckSpawnCall(const CheckingContext& context, MarmotExpression::Call& call)
{
	const MarmotExpression::NameAccess& spawn_name = call.m_callee->GetExpression<MarmotExpression::NameAccess>();
	const Token& spawn_token = spawn_name.m_name;
	if (call.m_arguments.size() != 2u)
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, std::format("Call expression type error: incorrect arity: expected 2 argument(s), got {}", call.m_arguments.size()), call.m_paren, m_file_name, m_source_lines));
	}

	// The ambient expected type belongs to the spawn's own result, Worker<T>, and
	// would otherwise be pushed onto a lambda written in callee position.
	std::unique_ptr<MarmotExpression>& function = call.m_arguments[1u];
	MarmotResult::TypeResult callee_result;
	{
		const CheckingContext expected_context = context.WithExpectedType(nullptr);
		callee_result = Evaluate(expected_context, call.m_callee, NameUse::Callee);
		callee_result = std::move(callee_result).and_then([this, &function, &expected_context](std::shared_ptr<MarmotType>&&) { return Evaluate(expected_context, function, NameUse::Callee); });
	}

	if (!callee_result.has_value())
	{
		return callee_result;
	}

	std::shared_ptr<MarmotType> callee_type = ApplySubstitution(context, callee_result.value());
	if (!callee_type->IsType<MarmotType::FunctionType>())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeNotCallable, std::format("Spawn expression type error: the second argument to {} must be a function", spawn_token.m_lexeme), spawn_token, m_file_name, m_source_lines, callee_type));
	}

	MarmotType::FunctionType& function_type = callee_type->GetType<MarmotType::FunctionType>();
	if (function_type.m_is_foreign)
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Spawn expression type error: foreign functions cannot be spawned", spawn_token, m_file_name, m_source_lines));
	}

	if (std::optional<CompilerError> error = CheckSpawnArgument(context, call, function_type))
	{
		return std::unexpected(std::move(*error));
	}

	if (std::optional<CompilerError> error = EnsureTransferable(context, spawn_token, callee_type))
	{
		return std::unexpected(std::move(*error));
	}

	std::shared_ptr<MarmotType> resolved_return_type = ApplySubstitution(context, function_type.m_return_type);
	if (std::optional<CompilerError> error = EnsureTransferable(context, spawn_token, resolved_return_type))
	{
		return std::unexpected(std::move(*error));
	}

	call.m_type_data = MarmotType::MakeWorkerType(resolved_return_type);
	return call.m_type_data;
}

std::optional<CompilerError> TypeChecker::CheckSpawnArgument(const CheckingContext& context, MarmotExpression::Call& call, const MarmotType::FunctionType& function_type)
{
	const Token& spawn_token = call.m_callee->GetExpression<MarmotExpression::NameAccess>().m_name;
	const size_t arity = function_type.m_param_types.size();
	std::shared_ptr<MarmotType> expected_type;
	if (arity == 0u)
	{
		expected_type = MarmotType::MakeLiteralType<MarmotType::UnitType>();
	}
	else if (arity == 1u)
	{
		expected_type = function_type.m_param_types[0u];
	}
	else
	{
		std::vector<std::shared_ptr<MarmotType>> element_types(function_type.m_param_types.begin(), function_type.m_param_types.end());
		expected_type = MarmotType::MakeTupleType(std::move(element_types));
	}

	const CheckingContext expected_context = context.WithExpectedType(expected_type);
	MarmotResult::TypeResult argument_result = Evaluate(expected_context, call.m_arguments[0u]);
	if (!argument_result.has_value())
	{
		return std::move(argument_result.error());
	}

	std::shared_ptr<MarmotType> actual_type = ApplySubstitution(expected_context, argument_result.value());
	if (arity >= 2u)
	{
		const bool is_matching_tuple = actual_type->IsType<MarmotType::TupleType>() && actual_type->GetType<MarmotType::TupleType>().m_element_types.size() == arity;
		if (!is_matching_tuple)
		{
			return MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, std::format("Spawn expression type error: the spawned function takes {} parameters, so pass them to {} as one {}-element tuple", arity, spawn_token.m_lexeme, arity), spawn_token, m_file_name, m_source_lines, actual_type, expected_type);
		}
	}

	MarmotResult::TypeResult unify_result = Unify(expected_context, spawn_token, actual_type, expected_type, UnifyDiagnosticMode::ActualExpected);
	if (!unify_result.has_value())
	{
		return std::move(unify_result.error());
	}

	return EnsureTransferable(expected_context, spawn_token, actual_type);
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::Call& call)
{
	const size_t pending = m_pending_instances.size();
	call.m_primitive = CalledPrimitive(context, call);
	MarmotResult::TypeResult checked = call.m_primitive == MarmotPrimitive::Spawn ? CheckSpawnCall(context, call) : CheckCall(context, call);
	return std::move(checked)
		.and_then
		(
			[this, pending, &context](std::shared_ptr<MarmotType>&& type) -> MarmotResult::TypeResult
			{
				if (std::optional<CompilerError> error = CheckDecidedPendingInstances(context, pending))
				{
					return std::unexpected(std::move(*error));
				}
				return type;
			}
		);
}

// The primitive whose declaration a call's callee names. Only a name can name
// one, since a primitive is never a value.
std::optional<MarmotPrimitive> TypeChecker::CalledPrimitive(const CheckingContext& context, const MarmotExpression::Call& call) const
{
	if (!call.m_callee->IsExpression<MarmotExpression::NameAccess>())
	{
		return std::nullopt;
	}

	const std::shared_ptr<MarmotType>* binding = FindValueType(context, call.m_callee->GetExpression<MarmotExpression::NameAccess>());
	return binding != nullptr && (*binding)->IsType<MarmotType::FunctionType>() ? (*binding)->GetType<MarmotType::FunctionType>().m_primitive : std::nullopt;
}

MarmotResult::TypeResult TypeChecker::CheckCall(const CheckingContext& context, MarmotExpression::Call& call)
{
	if (call.m_callee->IsExpression<MarmotExpression::NameAccess>())
	{
		MarmotExpression::NameAccess& callee_name = call.m_callee->GetExpression<MarmotExpression::NameAccess>();
		const std::string& full_name = callee_name.m_name.m_lexeme;
		size_t separator_pos = full_name.rfind(NameSeparator.data());
		if (separator_pos != std::string::npos)
		{
			const std::string qualifier = full_name.substr(0u, separator_pos);
			const std::string method_name = full_name.substr(separator_pos + NameSeparator.length());

			std::unordered_map<std::string, ClassInfo>::const_iterator tc_it = m_classes.find(qualifier);
			if (tc_it != m_classes.cend())
			{
				const ClassInfo& tc_info = tc_it->second;
				TypeEnvironment::const_iterator method_it = tc_info.m_method_types.find(method_name);
				if (method_it != tc_info.m_method_types.cend())
				{
					std::vector<std::shared_ptr<MarmotType>> arg_results;
					const size_t arity = call.m_arguments.size();
					if (arity == 2u && FindOperator(call.m_paren.m_token_name, false) != nullptr
						&& (ContextualInteger(*call.m_arguments[0u]) != nullptr || ContextualInteger(*call.m_arguments[1u]) != nullptr))
					{
						std::expected<BinaryOperandTypes, CompilerError> operands = CheckBinaryOperands(context, call.m_paren.m_token_name, call.m_arguments[0u], call.m_arguments[1u]);
						if (!operands.has_value())
						{
							return std::unexpected(std::move(operands.error()));
						}
						arg_results = { std::move((*operands)[0u]), std::move((*operands)[1u]) };
					}
					else if (arity == 1u && FindOperator(call.m_paren.m_token_name, true) != nullptr && ContextualInteger(*call.m_arguments[0u]) != nullptr)
					{
						MarmotResult::TypeResult operand = Evaluate(context, call.m_arguments[0u]);
						if (!operand.has_value())
						{
							return operand;
						}
						arg_results.push_back(std::move(operand.value()));
					}
					const std::vector<const MarmotType::ClassConstraint*> matching_constraints = context.m_assumptions->m_constraints
						| std::views::filter([&qualifier](const MarmotType::ClassConstraint& constraint) { return constraint.m_class_name == qualifier; })
						| std::views::transform([](const MarmotType::ClassConstraint& constraint) { return &constraint; })
						| std::ranges::to<std::vector>();

					if (!matching_constraints.empty())
					{
						arg_results.reserve(call.m_arguments.size());
						for (std::unique_ptr<MarmotExpression>& call_arg : call.m_arguments | std::views::drop(arg_results.size()))
						{
							MarmotResult::TypeResult arg_result = Evaluate(context.WithExpectedType(nullptr), call_arg);
							if (!arg_result.has_value())
							{
								return arg_result;
							}
							arg_results.emplace_back(std::move(arg_result.value()));
						}

						// A where clause in force answers for the type it names. The method is
						// picked by its first argument, so a call on any other type - a concrete
						// one with an instance of its own - is selected from the instances.
						const std::vector<const MarmotType::ClassConstraint*> selected_constraints = MatchingMethodConstraints(context, qualifier, tc_info, method_it->second, arg_results);

						if (selected_constraints.size() > 1u)
						{
							const std::string first_arg_name = arg_results.empty() ? std::string("no arguments") : ApplySubstitution(context, arg_results[0u])->DisplayString();
							return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Call expression type error: ambiguous class method '" + qualifier + NameSeparator.data() + method_name + "' for argument type '" + first_arg_name + "'", call.m_paren, m_file_name, m_source_lines));
						}
						if (selected_constraints.size() == 1u)
						{
							return CheckConstrainedMethod(context, call.m_paren, tc_info, method_it->second, *selected_constraints.front(), arg_results).transform
							(
								[&call, this, &context](std::shared_ptr<MarmotType> signature)
								{
									const MarmotType::FunctionType& function = signature->GetType<MarmotType::FunctionType>();
									call.m_is_foreign = function.m_is_foreign;
									call.m_type_data = ApplySubstitution(context, function.m_return_type);
									return call.m_type_data;
								}
							);
						}
					}

					return CheckInstanceMethodCall(context, call, qualifier, method_name, tc_info, method_it->second, std::move(arg_results));
				}
			}
		}
	}

	// A lambda is called directly only from inside parentheses: `fn(x) => x(3)`
	// would put the call in its body.
	const MarmotExpression* callee_inside_groups = call.m_callee.get();
	while (callee_inside_groups->IsExpression<MarmotExpression::Group>())
	{
		callee_inside_groups = callee_inside_groups->GetExpression<MarmotExpression::Group>().m_expr_in.get();
	}

	if (callee_inside_groups->IsExpression<MarmotExpression::Function>())
	{
		std::vector<std::shared_ptr<MarmotType>> arg_results;
		arg_results.reserve(call.m_arguments.size());
		for (std::unique_ptr<MarmotExpression>& call_arg : call.m_arguments)
		{
			MarmotResult::TypeResult arg_result = Evaluate(context, call_arg);
			if (!arg_result.has_value())
			{
				return arg_result;
			}
			arg_results.emplace_back(std::move(arg_result.value()));
		}

		std::shared_ptr<MarmotType> expected_return_type = context.m_expected_expr_type != nullptr ? std::shared_ptr<MarmotType>(context.m_expected_expr_type) : FreshTypeVar();
		std::shared_ptr<MarmotType> expected_callee_type = MarmotType::MakeFunctionType(arg_results, std::move(expected_return_type));
		const CheckingContext callee_context = context.WithExpectedType(expected_callee_type);

		return Evaluate(callee_context, call.m_callee)
			.and_then
			(
				[&call, &arg_results, this, &callee_context](std::shared_ptr<MarmotType>&& actual_type) -> MarmotResult::TypeResult
				{
					std::shared_ptr<MarmotType> resolved_type = ApplySubstitution(callee_context, actual_type);
					if (!resolved_type->IsType<MarmotType::FunctionType>())
					{
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeNotCallable, "Call expression type error: not a callable", call.m_paren, m_file_name, m_source_lines, resolved_type));
					}

					MarmotType::FunctionType& function_type = resolved_type->GetType<MarmotType::FunctionType>();
					if (function_type.m_param_types.size() != call.m_arguments.size())
					{
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, std::format("Call expression type error: incorrect arity: expected {} argument(s), got {}", function_type.m_param_types.size(), call.m_arguments.size()), call.m_paren, m_file_name, m_source_lines));
					}

					for (size_t idx : std::views::iota(0u, arg_results.size()))
					{
						std::shared_ptr<MarmotType>& actual_param_type = arg_results[idx];
						std::shared_ptr<MarmotType>& param_type = function_type.m_param_types[idx];
						MarmotResult::TypeResult result = Unify(callee_context, call.m_paren, actual_param_type, param_type, UnifyDiagnosticMode::ActualExpected);
						if (!result.has_value())
						{
							return result;
						}
					}

					call.m_is_foreign = function_type.m_is_foreign;
					call.m_type_data = ApplySubstitution(callee_context, function_type.m_return_type);
					return call.m_type_data;
				}
			);
	}

	// The type expected of the call is its result's, not the callee's.
	MarmotResult::TypeResult callee_result = Evaluate(context.WithExpectedType(nullptr), call.m_callee, NameUse::Callee);

	return std::move(callee_result)
		.and_then
		(
			[&call, this, &context](std::shared_ptr<MarmotType>&& actual_type) ->MarmotResult::TypeResult
			{
				std::shared_ptr<MarmotType> resolved_type = ApplySubstitution(context, actual_type);

				if (!resolved_type->IsType<MarmotType::FunctionType>())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeNotCallable, "Call expression type error: not a callable", call.m_paren, m_file_name, m_source_lines, resolved_type));
				}

				MarmotType::FunctionType& function_type = resolved_type->GetType<MarmotType::FunctionType>();
				if (function_type.m_param_types.size() != call.m_arguments.size())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, std::format("Call expression type error: incorrect arity: expected {} argument(s), got {}", function_type.m_param_types.size(), call.m_arguments.size()), call.m_paren, m_file_name, m_source_lines));
				}

				for (size_t idx : std::views::iota(0u, call.m_arguments.size()))
				{
					// Each argument is unified against its parameter before the next one is checked, so a
					// parameter type that an earlier argument decides is already resolved by the time it
					// becomes the expected type. Deferring every unification to a second pass left the
					// callee's freshened variables unbound, so `Append(buckets, Slot::Empty())` on
					// `fn<T>(Array<T>, T) -> Unit` checked the construction against a bare T and rejected it.
					const CheckingContext argument_context = context.WithExpectedType(function_type.m_param_types[idx]);

					MarmotResult::TypeResult arg_result = Evaluate(argument_context, call.m_arguments[idx]);
					if (!arg_result.has_value())
					{
						return arg_result;
					}

					MarmotResult::TypeResult unify_result = Unify(argument_context, call.m_paren, arg_result.value(), function_type.m_param_types[idx], UnifyDiagnosticMode::ActualExpected);
					if (!unify_result.has_value())
					{
						return unify_result;
					}
				}

				// A where clause may be on a type only the result names, which where the
				// result goes decides, as it does for a generic used as a value.
				if (!function_type.m_constraints.empty() && context.m_expected_expr_type != nullptr && !IsDecided(context, ApplySubstitution(context, function_type.m_return_type)))
				{
					MarmotResult::TypeResult expected_result = Unify(context, call.m_paren, function_type.m_return_type, context.m_expected_expr_type, UnifyDiagnosticMode::ActualExpected);
					if (!expected_result.has_value())
					{
						return expected_result;
					}
				}

				MarmotResult::TypeResult constraint_result = ValidateFunctionConstraints(context, call.m_paren, function_type);
				if (!constraint_result.has_value())
				{
					return constraint_result;
				}

				call.m_is_foreign = function_type.m_is_foreign;
				call.m_type_data = ApplySubstitution(context, function_type.m_return_type);

				return call.m_type_data;
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::MemberAccess& get)
{
	// The type the context demands of `a.b` describes the member, not the object: `a`'s type is
	// settled by `a` alone, and the member name then picks a field out of it. Carrying the
	// demand into the object let a construction there unify its own struct type with whatever
	// the surrounding call wanted, so `IO::PrintLine((new Point(7, 8)).x as Text)` was rejected
	// for building a Point where Text was expected.
	const CheckingContext object_context = context.WithExpectedType(std::shared_ptr<MarmotType>{});

	return Evaluate(object_context, get.m_struct)
		.and_then
		(
			[this, &get, &object_context](std::shared_ptr<MarmotType>&& object_type) -> MarmotResult::TypeResult
			{
				const std::shared_ptr<MarmotType> actual_type = ApplySubstitution(object_context, object_type);
				if (!actual_type->IsType<MarmotType::StructType>())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Cannot access member on non-struct type", get.m_member_name, m_file_name, m_source_lines));
				}

				const MarmotType::StructType& struct_type = actual_type->GetType<MarmotType::StructType>();
				std::vector<std::string>::const_iterator find_result = std::find(struct_type.m_member_names.cbegin(), struct_type.m_member_names.cend(), get.m_member_name.m_lexeme);
				if (find_result == struct_type.m_member_names.cend())
				{
					std::string suggestion = std::format("Struct '{}' does not have a member named '{}'", struct_type.m_name, get.m_member_name.m_lexeme);
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Unknown struct member", get.m_member_name, m_file_name, m_source_lines, suggestion));
				}

				get.m_index = static_cast<int>(find_result - struct_type.m_member_names.cbegin());

				get.m_type_data = struct_type.m_member_types[static_cast<size_t>(get.m_index)];
				return get.m_type_data;
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::NameAccess& variable, NameUse use)
{
	const BindingId* identity = nullptr;
	if (const MarmotExpression::NameContext::Local* local = std::get_if<MarmotExpression::NameContext::Local>(&variable.m_name_ctx))
	{
		identity = &local->m_binding;
	}
	else if (const MarmotExpression::NameContext::Cell* captured = std::get_if<MarmotExpression::NameContext::Cell>(&variable.m_name_ctx))
	{
		identity = &captured->m_binding;
	}
	size_t separator_pos = identity == nullptr ? variable.m_name.m_lexeme.rfind(NameSeparator.data()) : std::string::npos;
	if (separator_pos != std::string::npos)
	{
		std::string qualifier = variable.m_name.m_lexeme.substr(0u, separator_pos);
		std::string symbol_name = variable.m_name.m_lexeme.substr(separator_pos + NameSeparator.length());

		if (const std::unordered_map<std::string, ClassInfo>::const_iterator class_it = m_classes.find(qualifier); class_it != m_classes.cend() && class_it->second.m_method_types.contains(symbol_name))
		{
			const ClassInfo& info = class_it->second;
			const std::shared_ptr<MarmotType>& method = info.m_method_types.at(symbol_name);
			if (context.m_expected_expr_type == nullptr || !context.m_expected_expr_type->IsType<MarmotType::FunctionType>())
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Class method value needs an expected function type", variable.m_name, m_file_name, m_source_lines));
			}
			const MarmotType::FunctionType& expected = context.m_expected_expr_type->GetType<MarmotType::FunctionType>();
			std::vector<std::shared_ptr<MarmotType>> arguments = expected.m_param_types;
			const std::vector<const MarmotType::ClassConstraint*> constraints = MatchingMethodConstraints(context, qualifier, info, method, arguments);
			if (constraints.size() > 1u)
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Ambiguous class method value", variable.m_name, m_file_name, m_source_lines));
			}
			const CheckingContext method_context = context.WithExpectedType(expected.m_return_type);
			MarmotResult::TypeResult checked = constraints.empty()
				? CheckInstanceMethod(method_context, variable.m_name, qualifier, symbol_name, info, method, std::move(arguments))
				: CheckConstrainedMethod(method_context, variable.m_name, info, method, *constraints.front(), arguments);
			if (!checked.has_value())
			{
				return checked;
			}
			variable.m_type_data = std::move(checked.value());
			return Unify(context, variable.m_name, variable.m_type_data, context.m_expected_expr_type, UnifyDiagnosticMode::ActualExpected);
		}
	}

	const std::shared_ptr<MarmotType>* binding = FindValueType(context, variable);
	const LocalGenericFunction* local_generic = identity != nullptr ? FindLocalGeneric(context, *identity) : nullptr;
	const bool is_defining = identity != nullptr
		? local_generic != nullptr && std::ranges::contains(context.m_assumptions->m_defining_generic_functions, local_generic->m_function)
		: m_generic_functions.contains(variable.m_name.m_lexeme) && std::ranges::contains(context.m_assumptions->m_defining_generic_functions, m_generic_functions.at(variable.m_name.m_lexeme));
	const bool is_value = use == NameUse::Value;
	const auto freshen_name = [this, &variable, is_value](const std::shared_ptr<MarmotType>& type)
	{
		std::shared_ptr<MarmotType> fresh = Freshen(type);
		if (type->IsType<MarmotType::FunctionType>())
		{
			variable.m_intrinsic = type->GetType<MarmotType::FunctionType>().m_intrinsic;
			if (is_value && variable.m_intrinsic.has_value())
			{
				fresh->GetType<MarmotType::FunctionType>().m_is_foreign = false;
			}
		}
		return fresh;
	};
	if (is_value && binding != nullptr && (*binding)->IsType<MarmotType::FunctionType>() && (*binding)->GetType<MarmotType::FunctionType>().m_primitive.has_value())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(std::format("Name access expression type error: '{}' is a primitive, an instruction rather than a function, so it can only be called", variable.m_name.m_lexeme), variable.m_name, m_file_name, m_source_lines));
	}
	const std::function<MarmotResult::TypeResult()> use_as_value = [this, &variable, &context]()
	{
		return UseGenericInstance(context, variable.m_name, variable.m_type_data, std::format("Name access expression type error: could not infer which types '{}' is used at; pass it where a function type is expected, or write the type on the definition that holds it", variable.m_name.m_lexeme))
			.transform([&variable](std::shared_ptr<MarmotType>&& instance) { return variable.m_type_data = std::move(instance); });
	};
	if (binding != nullptr && !is_defining && local_generic != nullptr)
	{
		variable.m_type_data = Instantiate(context, *binding, local_generic->m_parameters);
		return is_value ? use_as_value() : variable.m_type_data;
	}
	if (binding != nullptr)
	{
		const bool binding_is_generic_function =
			identity == nullptr && m_generic_functions.contains(variable.m_name.m_lexeme)
			&& !is_defining
			&& binding->get()->IsType<MarmotType::FunctionType>();
		if (binding_is_generic_function
			|| (identity == nullptr && variable.m_name.m_lexeme.find(NameSeparator) != std::string::npos)
			|| (binding->get()->IsType<MarmotType::FunctionType>() && binding->get()->GetType<MarmotType::FunctionType>().m_is_foreign))
		{
			variable.m_type_data = freshen_name(*binding);
			if (is_value && variable.m_type_data->IsType<MarmotType::FunctionType>() && !CollectTypeVariableIds(variable.m_type_data).empty())
			{
				return use_as_value();
			}
		}
		else
		{
			// Apply substitution to get the most up-to-date type
			// This handles cases where the type contains type variables that have been unified
			variable.m_type_data = ApplySubstitution(context, *binding);
		}
		return variable.m_type_data;
	}

	for (const MarmotType::ClassConstraint& constraint : context.m_assumptions->m_constraints)
	{
		std::unordered_map<std::string, ClassInfo>::iterator tc_it = m_classes.find(constraint.m_class_name);
		if (tc_it != m_classes.end())
		{
			const ClassInfo& tc_info = tc_it->second;
			TypeEnvironment::const_iterator method_it = tc_info.m_method_types.find(variable.m_name.m_lexeme);
			if (method_it != tc_info.m_method_types.cend())
			{
				// This is a valid class method - return its type
				// The actual name resolution to the mangled method happens in code generation
				variable.m_type_data = Freshen(method_it->second);
				return variable.m_type_data;
			}
		}
	}

	// A top-level definition the checker has not reached yet: its type is the
	// one written on it.
	const TypeEnvironment::const_iterator later = m_later_definitions.find(variable.m_name.m_lexeme);
	if (later != m_later_definitions.cend())
	{
		variable.m_type_data = freshen_name(later->second);
		return variable.m_type_data;
	}

	if (m_failed_definitions.contains(variable.m_name.m_lexeme))
	{
		m_used_failed_definition = true;
		variable.m_type_data = FreshTypeVar();
		return variable.m_type_data;
	}

	if (m_later_unannotated.contains(variable.m_name.m_lexeme))
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUndefinedName, std::format("'{}' is defined further down, and its type is not written on it, so it is not known here. Give it a type -- its parameter and return types, for a function -- or move it above this.", variable.m_name.m_lexeme), variable.m_name, m_file_name, m_source_lines));
	}

	return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUndefinedName, "Name access expression type error: variable not found", variable.m_name, m_file_name, m_source_lines));
}

MarmotResult::TypeResult TypeChecker::CheckLiteral(MarmotLiteral& literal, const std::shared_ptr<MarmotType>& expected) const
{
	if (literal.m_kind == MarmotLiteralKind::Integer && IsPrefixedIntegerLiteral(literal.m_token.m_lexeme))
	{
		const std::optional<uint64_t> value = ParseUnsignedLiteral(literal.m_token.m_lexeme);
		constexpr uint64_t max_int = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
		if (expected != nullptr && expected->IsType<MarmotType::ByteType>())
		{
			if (value.value_or(max_int) > 0xFF)
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Integer literal is out of range for Byte (0 to 255).", literal.m_token, m_file_name, m_source_lines));
			}
			literal.m_kind = MarmotLiteralKind::Byte;
		}
		else if (expected != nullptr && expected->IsType<MarmotType::IntegerType>())
		{
			if (value.value_or(std::numeric_limits<uint64_t>::max()) > max_int)
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Integer literal is out of range for Int.", literal.m_token, m_file_name, m_source_lines));
			}
		}
		else if ((expected != nullptr && expected->IsType<MarmotType::WordType>()) || !value.has_value() || value.value() > max_int)
		{
			literal.m_kind = MarmotLiteralKind::Word;
		}
	}
	switch (literal.m_kind)
	{
	case MarmotLiteralKind::Text:
		return MarmotType::MakeLiteralType<MarmotType::TextType>();
	case MarmotLiteralKind::Bool:
		return MarmotType::MakeLiteralType<MarmotType::BoolType>();
	case MarmotLiteralKind::Float:
		literal.m_float_value = ParseFloatLiteral(literal.m_token.m_lexeme);
		if (!literal.m_float_value.has_value())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Float literal is out of range.", literal.m_token, m_file_name, m_source_lines));
		}
		return MarmotType::MakeLiteralType<MarmotType::FloatType>();
	case MarmotLiteralKind::Integer:
		return MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	case MarmotLiteralKind::Byte:
		return MarmotType::MakeLiteralType<MarmotType::ByteType>();
	case MarmotLiteralKind::Word:
		return MarmotType::MakeLiteralType<MarmotType::WordType>();
	case MarmotLiteralKind::Unit:
		return MarmotType::MakeLiteralType<MarmotType::UnitType>();
	}
	std::unreachable();
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::Literal& literal)
{
	const std::shared_ptr<MarmotType> expected = context.m_expected_expr_type != nullptr
		? ApplySubstitution(context, context.m_expected_expr_type)
		: nullptr;
	return CheckLiteral(literal, expected).transform([&literal](std::shared_ptr<MarmotType>&& type)
	{
		literal.m_type_data = std::move(type);
		return literal.m_type_data;
	});
}

MarmotResult::TypeResult TypeChecker::ResolveFunctionExpressionSignature(const CheckingContext& context, MarmotExpression::Function& function, const std::unordered_set<int>& outer_visible_type_vars)
{
	function.m_type_data = ApplySubstitution(context, function.m_type_data);
	const MarmotType::FunctionType& resolved_type = function.m_type_data->GetType<MarmotType::FunctionType>();
	function.m_param_types = resolved_type.m_param_types;
	function.m_return_type = resolved_type.m_return_type;

	if (HasTypeVariables(function.m_type_data))
	{
		std::unordered_set<int> unresolved_type_vars = CollectTypeVariableIds(function.m_type_data);
		bool only_outer_type_vars_remain = std::ranges::all_of
		(
			unresolved_type_vars,
			[&outer_visible_type_vars](int type_var_id) { return outer_visible_type_vars.contains(type_var_id); }
		);

		if (!only_outer_type_vars_remain)
		{
			return std::unexpected
			(
				MarmotError::GenerateTypeCheckerErrorWithContext
				(
					"Function expression type error: could not infer all lambda parameter or return types",
					function.m_function_keyword,
					m_file_name,
					m_source_lines
				)
			);
		}
	}

	return function.m_type_data;
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::Function& function)
{
	return CheckLambda(context, function, nullptr);
}

MarmotResult::TypeResult TypeChecker::CheckLambda(const CheckingContext& context, MarmotExpression::Function& function, const BindingId& self)
{
	// An anonymous generic lambda is one value, so it takes one instance of its
	// generic type, which the context decides. Its parameter and return types
	// stay generic for lowering to read the instance against.
	if (!function.m_generic_params.empty())
	{
		std::vector<int> parameters;
		return CheckGenericLambda(context, function, function.m_function_keyword, self, [&parameters](const std::shared_ptr<MarmotType>&, const std::vector<int>& own) { parameters = own; })
			.and_then([&](std::shared_ptr<MarmotType>&&) { return UseGenericInstance(context, function.m_function_keyword, Instantiate(context, function.m_type_data, parameters), "Function expression type error: could not infer which types this generic lambda is used at"); })
			.transform([&function](std::shared_ptr<MarmotType>&& instance) { return function.m_type_data = std::move(instance); });
	}

	// Preserve visible generic type variables when a lambda annotation refers to them.
	TypeTransform::Freshener freshening_context = MakeLambdaFresheningContext(context);

	// Freshen any UndecidedType parameters to TypeVariables
	for (std::shared_ptr<MarmotType>& param_type : function.m_param_types)
	{
		param_type = freshening_context.Apply(param_type);
	}
	function.m_return_type = freshening_context.Apply(function.m_return_type);

	// Create the function type and store it so it can be returned
	std::shared_ptr<MarmotType> return_type_copy = function.m_return_type;
	function.m_type_data = MarmotType::MakeFunctionType(function.m_param_types, std::move(return_type_copy));
	// Match the signature before checking the body that decides its guarantee.
	MarmotType::FunctionType& signature = function.m_type_data->GetType<MarmotType::FunctionType>();
	signature.m_is_transferable = true;
	signature.m_constraints = MarmotType::MapConstraints(function.m_constraints, [&freshening_context](const std::shared_ptr<MarmotType>& constraint_type) { return freshening_context.Apply(constraint_type); });
	ApplyTransferableParameterConstraints(signature);
	function.m_param_types = signature.m_param_types;
	if (context.m_expected_expr_type != nullptr)
	{
		std::shared_ptr<MarmotType> expected_type = ApplySubstitution(context, context.m_expected_expr_type);
		// An inference variable needs the final type, once captures have been checked.
		if (!expected_type->IsType<MarmotType::TypeVariable>() && !expected_type->IsType<MarmotType::UndecidedType>())
		{
			MarmotResult::TypeResult expected_result = Unify(context, function.m_function_keyword, function.m_type_data, expected_type, UnifyDiagnosticMode::ActualExpected);
			if (!expected_result.has_value())
			{
				return expected_result;
			}

			function.m_type_data = ApplySubstitution(context, function.m_type_data);
			const MarmotType::FunctionType& inferred_type = function.m_type_data->GetType<MarmotType::FunctionType>();
			function.m_param_types = inferred_type.m_param_types;
			function.m_return_type = inferred_type.m_return_type;
		}
	}

	// Collected before the lambda's parameters enter the context: they are
	// the variables this check must infer, rather than preserve.
	std::unordered_set<int> outer_visible_type_vars = CollectEnclosingTypeVariableIds(context);
	if (context.m_expected_expr_type != nullptr)
	{
		std::unordered_set<int> expected_type_vars = CollectTypeVariableIds(ApplySubstitution(context, context.m_expected_expr_type));
		outer_visible_type_vars.insert(expected_type_vars.cbegin(), expected_type_vars.cend());
	}

	std::vector<MarmotType::ClassConstraint>& function_constraints = function.m_type_data->GetType<MarmotType::FunctionType>().m_constraints;
	for (MarmotType::ClassConstraint& constraint : CollectSignatureConstraints(function.m_param_types, function.m_return_type))
	{
		AppendUniqueConstraint(function_constraints, std::move(constraint));
	}

	return InScope
	(
		context,
		[&](const CheckingContext& context) -> MarmotResult::TypeResult
		{
			BindParameters(context, function.m_params, function.m_param_types);

			return CheckFunctionBody(context, function.m_body, function.m_return_type, function_constraints, function.m_function_keyword)
				.and_then
				(
					[&function, &outer_visible_type_vars, this, &context, &self](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
					{
						InferCaptureTransferability(context, function, self);
						return ResolveFunctionExpressionSignature(context, function, outer_visible_type_vars);
					}
				);
		}
	);
}

// Each value moved to its field's position, which makes a named construction
// the positional one.
std::optional<CompilerError> TypeChecker::PlaceNamedFields(MarmotExpression::Construct& construct, const MarmotType::StructType& struct_type)
{
	std::vector<std::unique_ptr<MarmotExpression>> placed(struct_type.m_member_names.size());
	for (size_t written = 0u; written < construct.m_field_names.size(); written += 1u)
	{
		const Token& field = construct.m_field_names[written];
		const std::vector<std::string>::const_iterator found = std::ranges::find(struct_type.m_member_names, field.m_lexeme);
		if (found == struct_type.m_member_names.cend())
		{
			return MarmotError::GenerateTypeCheckerErrorWithContext(std::format("Construct expression type error: '{}' has no field named '{}'", struct_type.m_name, field.m_lexeme), field, m_file_name, m_source_lines);
		}
		placed[static_cast<size_t>(std::distance(struct_type.m_member_names.cbegin(), found))] = std::move(construct.m_params[written]);
	}

	const std::vector<std::string> missing = std::views::iota(0u, placed.size())
		| std::views::filter([&placed](size_t index) { return placed[index] == nullptr; })
		| std::views::transform([&struct_type](size_t index) { return std::format("'{}'", struct_type.m_member_names[index]); })
		| std::ranges::to<std::vector>();
	if (!missing.empty())
	{
		return MarmotError::GenerateTypeCheckerErrorWithContext(std::format("Construct expression type error: '{}' needs a value for {}", struct_type.m_name, missing | std::views::join_with(std::string_view(", ")) | std::ranges::to<std::string>()), construct.m_data_name, m_file_name, m_source_lines);
	}

	construct.m_params = std::move(placed);
	construct.m_field_names.clear();
	return std::nullopt;
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::Construct& construct)
{
	const std::shared_ptr<MarmotType>& return_type = construct.m_return_type;
	std::string constructor_name;
	std::string actual_type_name;
	std::string declaring_module_name;

	if (construct.IsConstructTypeOf<MarmotExpression::Construct::Struct>())
	{
		const MarmotType::StructType& struct_type = return_type->GetType<MarmotType::StructType>();
		actual_type_name = struct_type.m_name;
		constructor_name = actual_type_name;
		declaring_module_name = struct_type.m_module_name;
	}
	else
	{
		const MarmotType::UnionType& union_type = return_type->GetType<MarmotType::UnionType>();
		constructor_name = construct.m_data_name.m_lexeme;
		actual_type_name = union_type.m_name;
		declaring_module_name = union_type.m_module_name;
	}

	const std::shared_ptr<MarmotType>* constructor_type_ptr = FindNameType(context, ConstructorKey(declaring_module_name, constructor_name));
	if (constructor_type_ptr == nullptr)
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Construct expression type error: struct or union not found", construct.m_data_name, m_file_name, m_source_lines));
	}

	if (!construct.m_field_names.empty())
	{
		const TypeDefinitionMap::const_iterator definition = m_struct_type_definitions.find(ConstructorKey(declaring_module_name, actual_type_name));
		if (std::optional<CompilerError> error = PlaceNamedFields(construct, definition->second->GetType<MarmotType::StructType>()))
		{
			return std::unexpected(std::move(*error));
		}
	}

	bool is_generic = false;
	if (construct.IsConstructTypeOf<MarmotExpression::Construct::Struct>())
	{
		is_generic = m_generic_structs.contains(ConstructorKey(declaring_module_name, actual_type_name));
	}
	else
	{
		is_generic = m_generic_unions.contains(ConstructorKey(declaring_module_name, actual_type_name));
	}

	std::shared_ptr<MarmotType> constructor_type_shared = is_generic ? Freshen(*constructor_type_ptr) : *constructor_type_ptr;

	// The name must resolve to the constructor's function type. It does not when a
	// struct is imported into another module, where the name binds to the type
	// itself, and reading it as a function would be undefined rather than an error.
	if (!constructor_type_shared->IsType<MarmotType::FunctionType>())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Construct expression type error: '" + constructor_name + "' does not name a constructor here (it resolved to '" + constructor_type_shared->DisplayString() + "')", construct.m_data_name, m_file_name, m_source_lines));
	}

	MarmotType::FunctionType& constructor_type = constructor_type_shared->GetType<MarmotType::FunctionType>();

	if (construct.m_has_explicit_type_args)
	{
		construct.m_return_type = InstantiateWrittenType(context, construct.m_return_type);
		MarmotResult::TypeResult explicit_type_result = Unify(context, construct.m_data_name, constructor_type.m_return_type, construct.m_return_type, UnifyDiagnosticMode::ActualExpected);
		if (!explicit_type_result.has_value())
		{
			return explicit_type_result;
		}
	}
	else if (context.m_expected_expr_type != nullptr)
	{
		std::shared_ptr<MarmotType> expected_type = context.m_expected_expr_type;
		MarmotResult::TypeResult expected_type_result = Unify(context, construct.m_data_name, constructor_type.m_return_type, expected_type, UnifyDiagnosticMode::ActualExpected);
		if (!expected_type_result.has_value())
		{
			return expected_type_result;
		}
	}

	if (constructor_type.m_param_types.size() != construct.m_params.size())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeIncorrectArity, std::format("Construct expression type error: incorrect arity: expected {} argument(s), got {}", constructor_type.m_param_types.size(), construct.m_params.size()), construct.m_data_name, m_file_name, m_source_lines));
	}

	for (size_t idx : std::views::iota(0u, construct.m_params.size()))
	{
		std::unique_ptr<MarmotExpression>& param = construct.m_params[idx];
		const CheckingContext expected_context = context.WithExpectedType(constructor_type.m_param_types[idx]);
		MarmotResult::TypeResult param_result = Evaluate(expected_context, param);
		if (!param_result.has_value())
		{
			return param_result;
		}

		std::shared_ptr<MarmotType> param_type = constructor_type.m_param_types[idx];
		MarmotResult::TypeResult unify_result = Unify(expected_context, construct.m_data_name, param_result.value(), param_type, UnifyDiagnosticMode::ActualExpected);
		if (!unify_result.has_value())
		{
			return unify_result;
		}
	}

	MarmotResult::TypeResult constraint_result = ValidateFunctionConstraints(context, construct.m_data_name, constructor_type);
	if (!constraint_result.has_value())
	{
		return constraint_result;
	}

	// For generic structs, apply substitution to the return type to get the monomorphized type
	// After unifying parameters, type variables have been substituted with concrete types
	construct.m_type_data = ApplySubstitution(context, constructor_type.m_return_type);

	if (HasTypeVariables(construct.m_type_data))
	{
		// Inside a generic definition the declared types have already been freshened, so
		// `new Bag(xs, t)` in `def MakeBag = fn<T>(xs : Array<T>, t : Int) -> Bag<T>` infers
		// Bag<T0> for a type variable T0 that stands for T. That is fully inferred, not
		// ambiguous - the enclosing definition owns T0 and monomorphisation decides it at
		// each call site. Only a variable that no enclosing definition owns is ambiguous.
		std::unordered_set<int> enclosing_type_vars = CollectEnclosingTypeVariableIds(context);
		std::unordered_set<int> unresolved_type_vars = CollectTypeVariableIds(construct.m_type_data);

		bool only_enclosing_type_vars_remain = std::ranges::all_of
		(
			unresolved_type_vars,
			[&enclosing_type_vars](int type_var_id) { return enclosing_type_vars.contains(type_var_id); }
		);

		if (!only_enclosing_type_vars_remain)
		{
			return std::unexpected(
				MarmotError::GenerateTypeCheckerErrorWithContext
				(
					std::format("Construct expression type error: could not infer all type arguments for '{}'", actual_type_name),
					construct.m_data_name,
					m_file_name,
					m_source_lines
				)
			);
		}
	}

	// Mark as generic instantiation if this was a generic struct/union
	if (construct.IsConstructTypeOf<MarmotExpression::Construct::Struct>())
	{
		if (is_generic && construct.m_type_data->IsType<MarmotType::StructType>())
		{
			construct.m_type_data->GetType<MarmotType::StructType>().m_is_generic_instantiation = true;
		}
	}
	else if (is_generic && construct.m_type_data->IsType<MarmotType::UnionType>())
	{
		construct.m_type_data->GetType<MarmotType::UnionType>().m_is_generic_instantiation = true;
	}

	return construct.m_type_data;
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::RecordUpdate& record_update)
{
	MarmotResult::TypeResult source_result = Evaluate(context, record_update.m_source);
	if (!source_result.has_value())
	{
		return source_result;
	}

	std::shared_ptr<MarmotType> source_type = ApplySubstitution(context, source_result.value());

	for (MarmotExpression::RecordUpdate::FieldUpdate& update : record_update.m_updates)
	{
		std::shared_ptr<MarmotType> member_type = source_type;
		update.m_indices.clear();
		for (const Token& step : update.m_path)
		{
			// A union's variants hold their fields by position, so there is no name to set.
			if (member_type->IsType<MarmotType::UnionType>())
			{
				std::string suggestion = std::format("'{}' is a union, whose variants' fields have no names; match on it and construct the variant instead", member_type->GetType<MarmotType::UnionType>().m_name);
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Record update expression type error: not a record", step, m_file_name, m_source_lines, std::optional<std::string_view>(suggestion)));
			}

			if (!member_type->IsType<MarmotType::StructType>())
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Record update expression type error: not a struct", step, m_file_name, m_source_lines, member_type));
			}

			const MarmotType::StructType& struct_type = member_type->GetType<MarmotType::StructType>();
			std::vector<std::string>::const_iterator find_result = std::ranges::find(struct_type.m_member_names, step.m_lexeme);
			if (find_result == struct_type.m_member_names.cend())
			{
				std::string suggestion = std::format("Struct '{}' does not have a member named '{}'", struct_type.m_name, step.m_lexeme);
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Unknown struct member in record update", step, m_file_name, m_source_lines, std::optional<std::string_view>(suggestion)));
			}

			const size_t index = static_cast<size_t>(find_result - struct_type.m_member_names.cbegin());
			update.m_indices.push_back(static_cast<int>(index));
			member_type = ApplySubstitution(context, struct_type.m_member_types[index]);
		}

		const CheckingContext expected_context = context.WithExpectedType(member_type);
		MarmotResult::TypeResult value_result = Evaluate(expected_context, update.m_value);
		if (!value_result.has_value())
		{
			return value_result;
		}

		MarmotResult::TypeResult unify_result = Unify(expected_context, update.m_path.back(), member_type, value_result.value(), UnifyDiagnosticMode::ExpectedActual);
		if (!unify_result.has_value())
		{
			return unify_result;
		}
	}

	// The result is the source's own already-resolved type, so no inference is needed here
	// at all. Construct instead looks the constructor function up by name, freshens it and
	// infers the type arguments from the supplied members; that also works inside a generic
	// function - `new Bag(b.items, t)` in `def Retag = fn<T>(b : Bag<T>, t : Int) -> Bag<T>`
	// infers Bag<T0>, a type variable the enclosing function owns. See
	// test/generics/success/generic_construction_in_generic_function.mmt.
	record_update.m_type_data = source_type;
	return record_update.m_type_data;
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::Array& array)
{
	std::shared_ptr<MarmotType> expected_array_type;
	std::shared_ptr<MarmotType> expected_element_type;
	if (context.m_expected_expr_type != nullptr)
	{
		std::shared_ptr<MarmotType> resolved_expected_type = ApplySubstitution(context, context.m_expected_expr_type);
		if (resolved_expected_type->IsType<MarmotType::ArrayType>())
		{
			expected_array_type = resolved_expected_type;
			expected_element_type = resolved_expected_type->GetType<MarmotType::ArrayType>().m_element_type;
		}
	}

	if (array.m_elems.empty())
	{
		array.m_type_data = expected_array_type != nullptr ? expected_array_type : MarmotType::MakeArrayType(MarmotType::MakeUndecidedType());
		return array.m_type_data;
	}

	const CheckingContext element_context = context.WithExpectedType(expected_element_type);

	std::vector<std::shared_ptr<MarmotType>> element_results;
	element_results.reserve(array.m_elems.size());

	for (std::unique_ptr<MarmotExpression>& element : array.m_elems)
	{
		MarmotResult::TypeResult result = Evaluate(element_context, element);
		if (!result.has_value())
		{
			return result;
		}

		element_results.emplace_back(std::move(result.value()));
	}

	// The elements share one type, which each may tell more of: `[[], [1]]` is
	// Array<Array<Int>>, and an element that never finishes takes the others' type.
	std::shared_ptr<MarmotType> element_type = element_results[0u];
	for (std::shared_ptr<MarmotType>& element_result : element_results | std::views::drop(1))
	{
		std::shared_ptr<MarmotType> resolved_element_type = ApplySubstitution(element_context, element_type);
		MarmotResult::TypeResult unified = Unify(element_context, array.m_op, resolved_element_type, element_result, UnifyDiagnosticMode::CommonType);
		if (!unified.has_value())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Array expression type error: inconsistent element types", array.m_op, m_file_name, m_source_lines, ApplySubstitution(element_context, element_result), ApplySubstitution(element_context, element_type)));
		}
		element_type = unified.value();
	}

	array.m_type_data = MarmotType::MakeArrayType(ApplySubstitution(element_context, element_type));
	return array.m_type_data;
}

MarmotResult::TypeResult TypeChecker::ResolveIndexableElementType(const CheckingContext& context, const Token& op, const std::shared_ptr<MarmotType>& container_type, const std::shared_ptr<MarmotType>& index_type)
{
	const MarmotType::ClassConstraint constraint(std::string(INDEXABLE_CLASS_NAME), { container_type, index_type });

	std::optional<ResolvedInstanceMatch> resolved_match = FindMatchingInstance(std::string(INDEXABLE_CLASS_NAME), constraint.m_type_args);
	if (resolved_match.has_value())
	{
		AssociatedTypeEnvironment::const_iterator binding_it = resolved_match->m_instance->m_associated_type_bindings.find(std::string(ELEMENT_ASSOCIATED_TYPE_NAME));
		if (binding_it == resolved_match->m_instance->m_associated_type_bindings.cend())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Index expression type error: the matching Indexable instance does not bind Element", op, m_file_name, m_source_lines, container_type));
		}

		return ApplySubstitution(context, TypeTransform::Instantiate(binding_it->second, resolved_match->m_substitutions));
	}

	if (IsSatisfiedByActiveConstraint(context, constraint))
	{
		return ApplySubstitution(context, MarmotType::MakeAssociatedType(std::string(INDEXABLE_CLASS_NAME), std::string(ELEMENT_ASSOCIATED_TYPE_NAME), { container_type, index_type }));
	}

	return std::unexpected(MakeConstraintFailureError(context, op, constraint));
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::IndexAccess& array_get)
{
	return Evaluate(context, array_get.m_arr_var)
		.and_then
		(
			[&array_get, this, &context](std::shared_ptr<MarmotType>&& container_type) ->MarmotResult::TypeResult
			{
				return Evaluate(context, array_get.m_index)
					.and_then
					(
						[&array_get, &container_type, this, &context](std::shared_ptr<MarmotType>&& raw_index_type) ->MarmotResult::TypeResult
						{
							std::shared_ptr<MarmotType> index_type = ApplySubstitution(context, raw_index_type);
							container_type = ApplySubstitution(context, container_type);

							if (container_type->IsType<MarmotType::ArrayType>())
							{
								if (!index_type->IsType<MarmotType::IntegerType>())
								{
									return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Array get expression type error: index must be integer", array_get.m_op, m_file_name, m_source_lines, index_type, MarmotType::MakeLiteralType<MarmotType::IntegerType>()));
								}

								array_get.m_type_data = ApplySubstitution(context, container_type->GetType<MarmotType::ArrayType>().m_element_type);
								return array_get.m_type_data;
							}

							return ResolveIndexableElementType(context, array_get.m_op, container_type, index_type)
								.and_then
								(
									[&array_get, this, &context](std::shared_ptr<MarmotType>&& element_type) ->MarmotResult::TypeResult
									{
										array_get.m_uses_indexable = true;
										array_get.m_type_data = ApplySubstitution(context, element_type);
										return array_get.m_type_data;
									}
								);
						}
					);
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::RangeBinary& range_binary)
{
	return Evaluate(context, range_binary.m_start)
		.and_then
		(
			[&range_binary, this, &context](std::shared_ptr<MarmotType>&& start_type) -> MarmotResult::TypeResult
			{
				return Evaluate(context, range_binary.m_end)
					.and_then
					(
						[&start_type, &range_binary, this, &context](std::shared_ptr<MarmotType>&& end_type) -> MarmotResult::TypeResult
						{
							return Unify(context, range_binary.m_range_op, start_type, end_type)
								.and_then
								(
									[&range_binary, &start_type, this, &context](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
									{
										std::shared_ptr<MarmotType> resolved_type = ApplySubstitution(context, start_type);

										if (RigidParameterName(context, resolved_type).has_value())
										{
											if (std::optional<CompilerError> error = RequireOperandClass(context, range_binary.m_range_op, NUMERIC_CLASS_NAME, resolved_type))
											{
												return std::unexpected(std::move(*error));
											}
										}
										else if (!resolved_type->IsNumericType() && !resolved_type->IsType<MarmotType::TypeVariable>())
										{
											return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Range expression type error: expected numeric type", range_binary.m_range_op, m_file_name, m_source_lines, resolved_type, MarmotType::MakeLiteralType<MarmotType::IntegerType>(), MarmotType::MakeLiteralType<MarmotType::FloatType>()));
										}

										range_binary.m_type_data = MarmotType::MakeRangeType(start_type);
										return range_binary.m_type_data;
									}
								);
						}
					);
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::RangeTernary& range_ternary)
{
	return Evaluate(context, range_ternary.m_start)
		.and_then
		(
			[&range_ternary, this, &context](std::shared_ptr<MarmotType>&& start_type) -> MarmotResult::TypeResult
			{
				return Evaluate(context, range_ternary.m_step)
					.and_then
					(
						[&start_type, &range_ternary, this, &context](std::shared_ptr<MarmotType>&& step_type) -> MarmotResult::TypeResult
						{
							return Unify(context, range_ternary.m_first_range_op, start_type, step_type)
								.and_then
								(
									[&start_type, &range_ternary, this, &context](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
									{
										return Evaluate(context, range_ternary.m_end)
											.and_then
											(
												[&start_type, &range_ternary, this, &context](std::shared_ptr<MarmotType>&& end_type) -> MarmotResult::TypeResult
												{
													return Unify(context, range_ternary.m_second_range_op, start_type, end_type)
														.and_then
														(
															[&range_ternary, &start_type, this, &context](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
															{
																std::shared_ptr<MarmotType> resolved_type = ApplySubstitution(context, start_type);

																if (RigidParameterName(context, resolved_type).has_value())
																{
																	if (std::optional<CompilerError> error = RequireOperandClass(context, range_ternary.m_first_range_op, NUMERIC_CLASS_NAME, resolved_type))
																	{
																		return std::unexpected(std::move(*error));
																	}
																}
																else if (!resolved_type->IsNumericType() && !resolved_type->IsType<MarmotType::TypeVariable>())
																{
																	return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Range expression type error: expected numeric type", range_ternary.m_first_range_op, m_file_name, m_source_lines, resolved_type, MarmotType::MakeLiteralType<MarmotType::IntegerType>(), MarmotType::MakeLiteralType<MarmotType::FloatType>()));
																}

																range_ternary.m_type_data = MarmotType::MakeRangeType(start_type);
																return range_ternary.m_type_data;
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

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::IfElse& if_else)
{
	// The type demanded of the if is the type of its branches, not of its
	// condition: `C(x) == C(1)` as a condition was checked against Option<C>.

	return Evaluate(context.WithExpectedType(nullptr), if_else.m_condition)
		.and_then
		(
			[&if_else, this, &context](std::shared_ptr<MarmotType>&& actual_type) ->MarmotResult::TypeResult
			{
				std::shared_ptr<MarmotType> bool_type = MarmotType::MakeLiteralType<MarmotType::BoolType>();
				return Unify(context, if_else.m_if_token, bool_type, actual_type, UnifyDiagnosticMode::ExpectedActual)
					.and_then
					(
						[&if_else, this, &context](std::shared_ptr<MarmotType>&& type)->MarmotResult::TypeResult
						{
							if (!type->IsType<MarmotType::BoolType>())
							{
								return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("IfElse expression type error: condition must be boolean", if_else.m_if_token, m_file_name, m_source_lines, type, MarmotType::MakeLiteralType<MarmotType::BoolType>()));
							}


							return Evaluate(context, if_else.m_true_branch)
								.and_then
								(
									[&if_else, this, &context](std::shared_ptr<MarmotType>&& true_branch_type) ->MarmotResult::TypeResult
									{
										// With nothing demanded of the whole if, the else branch is
										// still known to need the then branch's type, which lets
										// `if c then Some(x) else None()` decide None's type.
										const CheckingContext else_context = context.m_expected_expr_type == nullptr
											? context.WithExpectedType(ApplySubstitution(context, true_branch_type))
											: context;
										return Evaluate(else_context, if_else.m_else_branch)
											.and_then
											(
												[&true_branch_type, &if_else, this, &context](std::shared_ptr<MarmotType>&& else_branch_type)->MarmotResult::TypeResult
												{
													return Unify(context, if_else.m_else_token, true_branch_type, else_branch_type, UnifyDiagnosticMode::CommonType)
														.and_then
														(
															[&if_else](std::shared_ptr<MarmotType>&& type)->MarmotResult::TypeResult
															{
																if_else.m_type_data = type;
																return if_else.m_type_data;
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

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotExpression::Block& block)
{
	return InScope
	(
		context,
		[&](const CheckingContext& context) -> MarmotResult::TypeResult
		{
			for (const std::unique_ptr<MarmotStatement>& stmt : block.m_stmts)
			{
				const CheckingContext statement_context = context.WithExpectedType(std::shared_ptr<MarmotType>{});
				MarmotResult::TypeResult result = Evaluate(statement_context, stmt);
				if (!result.has_value())
				{
					return result;
				}
				// Note: Statement types are checked but do NOT contribute to the block's type
				// Only the final expression (if present) determines the block type
			}

			if (block.m_final_expr.has_value())
			{
				return Evaluate(context, *block.m_final_expr)
					.and_then
					(
						[&block, this, &context](std::shared_ptr<MarmotType>&& final_value) -> MarmotResult::TypeResult
						{
							return Unify(context, block.m_right_brace, block.m_type_data, final_value, UnifyDiagnosticMode::ExpectedActual);
						}
					);
			}

			// Blocks without final expressions have Unit type
			if (block.m_type_data->IsType<MarmotType::UndecidedType>())
			{
				block.m_type_data = MarmotType::MakeLiteralType<MarmotType::UnitType>();
			}
			return block.m_type_data;
		}
	);
}
