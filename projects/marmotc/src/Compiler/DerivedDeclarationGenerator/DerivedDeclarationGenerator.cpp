#include "DerivedDeclarationGenerator.h"
#include "Compiler/TypeTransform/TypeTransform.h"
#include "Bytecode/Format/Format.h"
#include "Compiler/Names/SpecialNames.h"
#include "Compiler/Operator/Ordering.h"
#include "Support/Expected.h"
#include "Support/Optional.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace
{
	bool IsExactGenericParam(const std::shared_ptr<MarmotType>& type, std::string_view name)
	{
		return type->IsType<MarmotType::GenericParam>() && type->GetType<MarmotType::GenericParam>().m_name == name;
	}

	// Each type parameter in the derived class, with the instance's type
	// arguments after the first: Convertible<T, Text> for Convertible<P, Text>.
	std::vector<MarmotType::ClassConstraint> DerivedConstraints(const std::string& class_name, const std::vector<Token>& generic_params, const std::vector<std::shared_ptr<MarmotType>>& type_args)
	{
		return generic_params
			| std::views::transform
			(
				[&class_name, &type_args](const Token& param)
				{
					std::vector<std::shared_ptr<MarmotType>> constraint_args{ MarmotType::MakeGenericType(param.m_lexeme) };
					constraint_args.insert(constraint_args.end(), std::next(type_args.begin()), type_args.end());
					return MarmotType::ClassConstraint(class_name, std::move(constraint_args));
				}
			)
			| std::ranges::to<std::vector>();
	}

	bool IsUnionSelfReference(const std::shared_ptr<MarmotType>& type, std::string_view union_name)
	{
		return type->IsType<MarmotType::UnionType>() && type->GetType<MarmotType::UnionType>().m_name == union_name;
	}

	bool ContainsGenericParam(const std::shared_ptr<MarmotType>& type, std::string_view name, std::unordered_set<const MarmotType*>& visited)
	{
		if (visited.contains(type.get()))
		{
			return false;
		}
		visited.emplace(type.get());

		if (IsExactGenericParam(type, name))
		{
			return true;
		}
		if (type->IsType<MarmotType::ArrayType>())
		{
			return ContainsGenericParam(type->GetType<MarmotType::ArrayType>().m_element_type, name, visited);
		}

		if (type->IsType<MarmotType::CellType>())
		{
			return ContainsGenericParam(type->GetType<MarmotType::CellType>().m_element_type, name, visited);
		}
		if (type->IsType<MarmotType::RangeType>())
		{
			return ContainsGenericParam(type->GetType<MarmotType::RangeType>().m_element_type, name, visited);
		}
		if (type->IsType<MarmotType::TupleType>())
		{
			for (const std::shared_ptr<MarmotType>& elem_type : type->GetType<MarmotType::TupleType>().m_element_types)
			{
				if (ContainsGenericParam(elem_type, name, visited))
				{
					return true;
				}
			}
			return false;
		}
		if (type->IsType<MarmotType::FunctionType>())
		{
			const MarmotType::FunctionType& function_type = type->GetType<MarmotType::FunctionType>();
			for (const std::shared_ptr<MarmotType>& param_type : function_type.m_param_types)
			{
				if (ContainsGenericParam(param_type, name, visited))
				{
					return true;
				}
			}
			return ContainsGenericParam(function_type.m_return_type, name, visited);
		}
		if (type->IsType<MarmotType::StructType>())
		{
			for (const std::shared_ptr<MarmotType>& member_type : type->GetType<MarmotType::StructType>().m_member_types)
			{
				if (ContainsGenericParam(member_type, name, visited))
				{
					return true;
				}
			}
			return false;
		}
		if (type->IsType<MarmotType::UnionType>())
		{
			for (const auto& [member_name, member_ctx] : type->GetType<MarmotType::UnionType>().m_member_info)
			{
				for (const std::shared_ptr<MarmotType>& member_type : member_ctx.m_member_types)
				{
					if (ContainsGenericParam(member_type, name, visited))
					{
						return true;
					}
				}
			}
			return false;
		}
		return false;
	}

	bool ContainsGenericParam(const std::shared_ptr<MarmotType>& type, std::string_view name)
	{
		std::unordered_set<const MarmotType*> visited;
		return ContainsGenericParam(type, name, visited);
	}

	// The syntax a derived instance is made of, every token placed at the
	// declaration it derives from.
	class SyntheticSyntax
	{
	private:
		const Token& m_anchor;
		LexicalFunctionId m_function = std::make_shared<LexicalFunction>();

	public:
		explicit SyntheticSyntax(const Token& anchor)
			: m_anchor(anchor)
		{
		}

		GeneratedBinding Local(int index) const
		{
			return GeneratedBinding(index, m_anchor, m_function);
		}

		std::vector<GeneratedBinding> Locals(int first, size_t count) const
		{
			return std::views::iota(0uz, count)
				| std::views::transform([this, first](size_t index) { return Local(first + static_cast<int>(index)); })
				| std::ranges::to<std::vector>();
		}

		FunctionParameters Parameters(std::vector<GeneratedBinding> bindings) const
		{
			return FunctionParameters::Generated(std::move(bindings), m_function);
		}

		std::unique_ptr<MarmotExpression> LocalName(const GeneratedBinding& binding) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(binding));
		}

		std::unique_ptr<MarmotExpression> GlobalName(const std::string& name) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(Token(name, Token::Name::IDENTIFIER_LITERAL, m_anchor), MarmotExpression::NameContext::Global{}));
		}

		std::unique_ptr<MarmotExpression> Call(std::unique_ptr<MarmotExpression>&& callee, std::vector<std::unique_ptr<MarmotExpression>>&& args) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::Call(Token(Token::Name::LEFT_PAREN, m_anchor), std::move(callee), std::move(args)));
		}

		std::unique_ptr<MarmotExpression> QualifiedCall(const std::string& qualifier, const std::string& method_name, std::vector<std::unique_ptr<MarmotExpression>>&& args) const
		{
			return Call(GlobalName(qualifier + std::string(NameSeparator) + method_name), std::move(args));
		}

		std::unique_ptr<MarmotExpression> Binary(Token::Name token_name, std::unique_ptr<MarmotExpression>&& left, std::unique_ptr<MarmotExpression>&& right) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::Binary(Token(token_name, m_anchor), std::move(left), std::move(right)));
		}

		std::unique_ptr<MarmotExpression> BoolLiteral(bool value) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::Literal(Token(value ? Token::Name::TRUE : Token::Name::FALSE, m_anchor), MarmotExpression::LiteralKind::Bool));
		}

		std::unique_ptr<MarmotExpression> IntLiteral(int value) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::Literal(Token(std::to_string(value), Token::Name::INTEGER_LITERAL, m_anchor), MarmotExpression::LiteralKind::Integer));
		}

		std::unique_ptr<MarmotExpression> Match(std::unique_ptr<MarmotExpression>&& scrutinee, std::vector<std::unique_ptr<MarmotExpression>>&& cases) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::Match(Token(Token::Name::MATCH, m_anchor), std::move(scrutinee), std::move(cases)));
		}

		std::unique_ptr<MarmotExpression> Case(std::unique_ptr<MarmotPattern>&& pattern, std::unique_ptr<MarmotExpression>&& body) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::Case(Token(Token::Name::CASE, m_anchor), std::move(pattern), std::move(body)));
		}

		std::unique_ptr<MarmotPattern> BindingPattern(const GeneratedBinding& binding) const
		{
			return std::make_unique<MarmotPattern>(MarmotPattern::Binding(binding));
		}

		std::unique_ptr<MarmotPattern> WildcardPattern() const
		{
			return std::make_unique<MarmotPattern>(MarmotPattern::Wildcard(Token("_", Token::Name::IDENTIFIER_LITERAL, m_anchor)));
		}

		std::unique_ptr<MarmotPattern> ConstructorPattern(const std::string& name, std::vector<std::unique_ptr<MarmotPattern>>&& fields) const
		{
			return std::make_unique<MarmotPattern>(MarmotPattern::Constructor(Token(name, Token::Name::IDENTIFIER_LITERAL, m_anchor), std::string(name), std::move(fields)));
		}

		std::unique_ptr<MarmotExpression> TextLiteral(const std::string& value) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::Literal(Token(value, Token::Name::TEXT_LITERAL, m_anchor), MarmotExpression::LiteralKind::Text));
		}

		std::unique_ptr<MarmotExpression> Concat(std::unique_ptr<MarmotExpression>&& left, std::unique_ptr<MarmotExpression>&& right) const
		{
			return Binary(Token::Name::DOUBLE_PLUS, std::move(left), std::move(right));
		}

		std::unique_ptr<MarmotExpression> AsText(std::unique_ptr<MarmotExpression>&& value) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::As(Token(Token::Name::AS, m_anchor), MarmotType::MakeLiteralType<MarmotType::TextType>(), std::move(value)));
		}

		std::unique_ptr<MarmotExpression> OrderingValue(const OrderingType& ordering, MarmotOrdering value) const
		{
			const Token constructor(ordering.ConstructorName(value), Token::Name::IDENTIFIER_LITERAL, m_anchor);
			return std::make_unique<MarmotExpression>(MarmotExpression::Construct(constructor, {}, std::shared_ptr<MarmotType>(ordering.m_type), true, MarmotExpression::Construct::Union{ ordering.Tag(value) }));
		}

		// `Orderable::Compare(left, right)`, or `equal` when that is Equal. The
		// comparison is held in local `slot` on its way out.
		std::unique_ptr<MarmotExpression> CompareThen(const OrderingType& ordering, std::unique_ptr<MarmotExpression>&& left, std::unique_ptr<MarmotExpression>&& right, int slot, std::unique_ptr<MarmotExpression>&& equal) const
		{
			std::vector<std::unique_ptr<MarmotExpression>> operands;
			operands.emplace_back(std::move(left));
			operands.emplace_back(std::move(right));
			std::vector<std::unique_ptr<MarmotExpression>> cases;
			cases.emplace_back(Case(ConstructorPattern(ordering.ConstructorName(MarmotOrdering::Equal), {}), std::move(equal)));
			const GeneratedBinding order = Local(slot);
			cases.emplace_back(Case(BindingPattern(order), LocalName(order)));
			return Match(QualifiedCall(std::string(ORDERABLE_CLASS_NAME), std::string(COMPARE_METHOD_NAME), std::move(operands)), std::move(cases));
		}
	};
}

DerivedDeclarationError::DerivedDeclarationError(std::string message, const Token& token)
	: m_message(std::move(message)),
	m_token(token)
{
}

DerivedDeclarationGenerator::DerivedDeclarationGenerator(Declaration declaration, Operation operation, Token target, MarmotCompiler::Optional<Token> global_name)
	: m_declaration(declaration),
	m_operation(operation),
	m_target(std::move(target)),
	m_global_name(std::move(global_name))
{
}

MarmotCompiler::Optional<DerivedDeclarationGenerator::Operation> DerivedDeclarationGenerator::ResolveOperation(std::string_view name)
{
	constexpr std::pair<std::string_view, Operation> operations[]
	{
		{ EQUATABLE_CLASS_NAME, Operation::Equatable },
		{ HASHABLE_CLASS_NAME, Operation::Hashable },
		{ ORDERABLE_CLASS_NAME, Operation::Orderable },
		{ CONVERTIBLE_CLASS_NAME, Operation::Convertible },
		{ TRANSFERABLE_CLASS_NAME, Operation::Transferable },
		{ MAP_DERIVING_NAME, Operation::Map },
		{ BIND_DERIVING_NAME, Operation::Bind },
		{ UNWRAP_DERIVING_NAME, Operation::Unwrap }
	};
	for (const auto& [spelling, operation] : operations)
	{
		if (spelling == name)
		{
			return operation;
		}
	}
	return MarmotCompiler::None;
}

// Without higher-kinded type parameters, a class cannot abstract over F<T>
// for a container constructor F. Map and Bind therefore derive per-union
// functions; Unwrap uses the same function-generating path.
bool DerivedDeclarationGenerator::IsContainer(Operation operation)
{
	return operation == Operation::Map || operation == Operation::Bind || operation == Operation::Unwrap;
}

MarmotCompiler::Expected<DerivedDeclarationGenerator, DerivedDeclarationError> DerivedDeclarationGenerator::Prepare(const MarmotStatement::Struct& declaration, const Token& target)
{
	const MarmotCompiler::Optional<Operation> operation = ResolveOperation(target.m_lexeme);
	if (!operation.HasValue() || IsContainer(operation.Value()))
	{
		return MarmotCompiler::Failure(DerivedDeclarationError("Unsupported deriving target for struct.", target));
	}
	return DerivedDeclarationGenerator(&declaration, operation.Value(), target, MarmotCompiler::None);
}

MarmotCompiler::Expected<DerivedDeclarationGenerator, DerivedDeclarationError> DerivedDeclarationGenerator::Prepare(const MarmotStatement::Union& declaration, const Token& target)
{
	const MarmotCompiler::Optional<Operation> operation = ResolveOperation(target.m_lexeme);
	if (!operation.HasValue())
	{
		return MarmotCompiler::Failure(DerivedDeclarationError("Unsupported deriving target for union.", target));
	}
	MarmotCompiler::Optional<Token> global_name;
	if (IsContainer(operation.Value()))
	{
		if (declaration.m_self_type->GetType<MarmotType::UnionType>().m_generic_params.empty())
		{
			return MarmotCompiler::Failure(DerivedDeclarationError("Container deriving requires at least one union type parameter.", declaration.m_name));
		}
		global_name.Emplace(declaration.m_name.m_lexeme + target.m_lexeme, Token::Name::IDENTIFIER_LITERAL, declaration.m_name);
	}
	return DerivedDeclarationGenerator(&declaration, operation.Value(), target, std::move(global_name));
}

Token* DerivedDeclarationGenerator::NameToReserve()
{
	return m_global_name ? &*m_global_name : nullptr;
}

DerivedDeclarationGenerator::StatementResult DerivedDeclarationGenerator::Generate(const OrderingType* ordering) const
{
	return std::visit([this, ordering](const auto* declaration) { return Generate(*declaration, ordering); }, m_declaration);
}

DerivedDeclarationGenerator::StatementResult DerivedDeclarationGenerator::Generate(const MarmotStatement::Struct& struct_stmt, const OrderingType* ordering) const
{
	const Token& derive_target = m_target;
	const MarmotType::StructType& struct_type = struct_stmt.m_self_type->GetType<MarmotType::StructType>();

	const SyntheticSyntax syntax(struct_stmt.m_name);

	auto make_member_access = [&struct_stmt, &syntax](const GeneratedBinding& base, const std::string& member_name, int member_index) -> std::unique_ptr<MarmotExpression>
	{
		Token member_token(member_name, Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name);
		return std::make_unique<MarmotExpression>(MarmotExpression::MemberAccess(member_token, syntax.LocalName(base), member_index));
	};

	std::vector<std::shared_ptr<MarmotType>> type_args{ std::shared_ptr<MarmotType>(struct_stmt.m_self_type) };
	if (m_operation == Operation::Convertible)
	{
		type_args.emplace_back(MarmotType::MakeLiteralType<MarmotType::TextType>());
	}
	Token class_token(derive_target.m_lexeme, Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name);
	std::vector<MarmotStatement::Instance::Method> methods;

	if (m_operation == Operation::Transferable)
	{
		return std::make_unique<MarmotStatement>(MarmotStatement::Instance(class_token, std::move(type_args), {}, {}, std::move(methods)));
	}

	if (m_operation == Operation::Equatable)
	{
		const std::string mangled_name = MarmotType::MangleInstanceMethodName(std::string(EQUALS_METHOD_NAME), derive_target.m_lexeme, type_args);
		Token method_token(mangled_name, Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name);
		std::vector<GeneratedBinding> params
		{
			syntax.Local(0),
			syntax.Local(1)
		};
		std::vector<std::shared_ptr<MarmotType>> param_types{ std::shared_ptr<MarmotType>(struct_stmt.m_self_type), std::shared_ptr<MarmotType>(struct_stmt.m_self_type) };

		std::unique_ptr<MarmotExpression> body = syntax.BoolLiteral(true);
		for (size_t i = 0u; i < struct_type.m_member_types.size(); i += 1u)
		{
			std::vector<std::unique_ptr<MarmotExpression>> eq_args;
			eq_args.emplace_back(make_member_access(params[0], struct_type.m_member_names[i], static_cast<int>(i)));
			eq_args.emplace_back(make_member_access(params[1], struct_type.m_member_names[i], static_cast<int>(i)));

			std::unique_ptr<MarmotExpression> compare_expr = syntax.QualifiedCall(std::string(EQUATABLE_CLASS_NAME), std::string(EQUALS_METHOD_NAME), std::move(eq_args));
			body = syntax.Binary(Token::Name::DOUBLE_AMPERSAND, std::move(body), std::move(compare_expr));
		}

		methods.emplace_back
		(
			Token(std::string(EQUALS_METHOD_NAME), Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name),
			MarmotStatement::FunctionDefinition(method_token, {}, syntax.Parameters(std::move(params)), std::move(param_types), std::shared_ptr<MarmotType>(MarmotType::MakeLiteralType<MarmotType::BoolType>()), std::move(body), MarmotCompiler::None, {})
		);
	}
	else if (m_operation == Operation::Convertible)
	{
		const std::string mangled_name = MarmotType::MangleInstanceMethodName(std::string(CONVERT_METHOD_NAME), derive_target.m_lexeme, type_args);
		Token method_token(mangled_name, Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name);
		std::vector<GeneratedBinding> params{ syntax.Local(0) };
		std::vector<std::shared_ptr<MarmotType>> param_types{ std::shared_ptr<MarmotType>(struct_stmt.m_self_type) };

		// `Name { field = value, ... }`, each field as its own `as Text` gives it.
		std::unique_ptr<MarmotExpression> body = syntax.TextLiteral(struct_type.m_name + " { ");
		for (size_t i = 0u; i < struct_type.m_member_types.size(); i += 1u)
		{
			body = syntax.Concat(std::move(body), syntax.TextLiteral((i == 0u ? "" : ", ") + struct_type.m_member_names[i] + " = "));
			body = syntax.Concat(std::move(body), syntax.AsText(make_member_access(params[0], struct_type.m_member_names[i], static_cast<int>(i))));
		}
		body = syntax.Concat(std::move(body), syntax.TextLiteral(" }"));

		methods.emplace_back
		(
			Token(std::string(CONVERT_METHOD_NAME), Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name),
			MarmotStatement::FunctionDefinition(method_token, {}, syntax.Parameters(std::move(params)), std::move(param_types), std::shared_ptr<MarmotType>(MarmotType::MakeLiteralType<MarmotType::TextType>()), std::move(body), MarmotCompiler::None, {})
		);
	}
	else if (m_operation == Operation::Orderable)
	{
		const std::string mangled_name = MarmotType::MangleInstanceMethodName(std::string(COMPARE_METHOD_NAME), derive_target.m_lexeme, type_args);
		Token method_token(mangled_name, Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name);
		std::vector<GeneratedBinding> params
		{
			syntax.Local(0),
			syntax.Local(1)
		};
		std::vector<std::shared_ptr<MarmotType>> param_types{ std::shared_ptr<MarmotType>(struct_stmt.m_self_type), std::shared_ptr<MarmotType>(struct_stmt.m_self_type) };

		// The first field that differs, in declaration order, decides.
		std::unique_ptr<MarmotExpression> body = syntax.OrderingValue(*ordering, MarmotOrdering::Equal);
		for (size_t i = struct_type.m_member_types.size(); i > 0u; i -= 1u)
		{
			const size_t field = i - 1u;
			body = syntax.CompareThen
			(
				*ordering,
				make_member_access(params[0], struct_type.m_member_names[field], static_cast<int>(field)),
				make_member_access(params[1], struct_type.m_member_names[field], static_cast<int>(field)),
				2 + static_cast<int>(field),
				std::move(body)
			);
		}

		methods.emplace_back
		(
			Token(std::string(COMPARE_METHOD_NAME), Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name),
			MarmotStatement::FunctionDefinition(method_token, {}, syntax.Parameters(std::move(params)), std::move(param_types), std::shared_ptr<MarmotType>(ordering->m_type), std::move(body), MarmotCompiler::None, {})
		);
	}
	else
	{
		const std::string mangled_name = MarmotType::MangleInstanceMethodName(std::string(HASH_METHOD_NAME), derive_target.m_lexeme, type_args);
		Token method_token(mangled_name, Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name);
		std::vector<GeneratedBinding> params{ syntax.Local(0) };
		std::vector<std::shared_ptr<MarmotType>> param_types{ std::shared_ptr<MarmotType>(struct_stmt.m_self_type) };

		std::unique_ptr<MarmotExpression> body = syntax.IntLiteral(0);
		for (size_t i = 0u; i < struct_type.m_member_types.size(); i += 1u)
		{
			std::vector<std::unique_ptr<MarmotExpression>> hash_args;
			hash_args.emplace_back(make_member_access(params[0], struct_type.m_member_names[i], static_cast<int>(i)));

			std::unique_ptr<MarmotExpression> member_hash = syntax.QualifiedCall(std::string(HASHABLE_CLASS_NAME), std::string(HASH_METHOD_NAME), std::move(hash_args));
			std::unique_ptr<MarmotExpression> scaled = syntax.Binary(Token::Name::STAR, std::move(body), syntax.IntLiteral(31));
			body = syntax.Binary(Token::Name::SINGLE_PLUS, std::move(scaled), std::move(member_hash));
		}

		methods.emplace_back
		(
			Token(std::string(HASH_METHOD_NAME), Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name),
			MarmotStatement::FunctionDefinition(method_token, {}, syntax.Parameters(std::move(params)), std::move(param_types), std::shared_ptr<MarmotType>(MarmotType::MakeLiteralType<MarmotType::IntegerType>()), std::move(body), MarmotCompiler::None, {})
		);
	}

	std::vector<MarmotType::ClassConstraint> constraints = DerivedConstraints(derive_target.m_lexeme, struct_stmt.m_generic_params, type_args);

	return std::make_unique<MarmotStatement>(MarmotStatement::Instance(class_token, std::move(type_args), std::move(constraints), {}, std::move(methods)));
}

DerivedDeclarationGenerator::StatementResult DerivedDeclarationGenerator::Generate(const MarmotStatement::Union& union_stmt, const OrderingType* ordering) const
{
	const Token& derive_target = m_target;
	const MarmotType::UnionType& union_type = union_stmt.m_self_type->GetType<MarmotType::UnionType>();

	const SyntheticSyntax syntax(union_stmt.m_name);

	auto make_constructor_pattern = [&syntax](const std::string& ctor_name, const std::vector<GeneratedBinding>& bindings) -> std::unique_ptr<MarmotPattern>
	{
		std::vector<std::unique_ptr<MarmotPattern>> args;
		for (const GeneratedBinding& binding : bindings)
		{
			args.emplace_back(syntax.BindingPattern(binding));
		}
		return syntax.ConstructorPattern(ctor_name, std::move(args));
	};

	auto make_union_construct = [&union_stmt](const std::string& ctor_name, int tag, std::vector<std::unique_ptr<MarmotExpression>>&& args, const std::shared_ptr<MarmotType>& return_type) -> std::unique_ptr<MarmotExpression>
	{
		Token ctor_token(ctor_name, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
		return std::make_unique<MarmotExpression>(MarmotExpression::Construct(ctor_token, std::move(args), std::shared_ptr<MarmotType>(return_type), true, MarmotExpression::Construct::Union(tag)));
	};

	if (m_operation == Operation::Transferable)
	{
		std::vector<std::shared_ptr<MarmotType>> type_args{ std::shared_ptr<MarmotType>(union_stmt.m_self_type) };
		Token class_token(derive_target.m_lexeme, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
		std::vector<MarmotStatement::Instance::Method> methods;

		return std::make_unique<MarmotStatement>(MarmotStatement::Instance(class_token, std::move(type_args), {}, {}, std::move(methods)));
	}

	if (!IsContainer(m_operation))
	{
		std::vector<std::shared_ptr<MarmotType>> type_args{ std::shared_ptr<MarmotType>(union_stmt.m_self_type) };
		if (m_operation == Operation::Convertible)
		{
			type_args.emplace_back(MarmotType::MakeLiteralType<MarmotType::TextType>());
		}
		Token class_token(derive_target.m_lexeme, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
		std::vector<MarmotStatement::Instance::Method> methods;

		if (m_operation == Operation::Equatable)
		{
			const std::string mangled_name = MarmotType::MangleInstanceMethodName(std::string(EQUALS_METHOD_NAME), derive_target.m_lexeme, type_args);
			Token method_token(mangled_name, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
			std::vector<GeneratedBinding> params
			{
				syntax.Local(0),
				syntax.Local(1)
			};
			std::vector<std::shared_ptr<MarmotType>> param_types{ std::shared_ptr<MarmotType>(union_stmt.m_self_type), std::shared_ptr<MarmotType>(union_stmt.m_self_type) };

			std::vector<std::unique_ptr<MarmotExpression>> outer_cases;
			for (const Token& ctor_name : union_stmt.m_constructor_names)
			{
				const MarmotType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(ctor_name.m_lexeme);
				const int outer_binding_start = 2;
				const int inner_binding_start = outer_binding_start + static_cast<int>(member_ctx.m_member_types.size());
				const std::vector<GeneratedBinding> outer_bindings = syntax.Locals(outer_binding_start, member_ctx.m_member_types.size());
				const std::vector<GeneratedBinding> inner_bindings = syntax.Locals(inner_binding_start, member_ctx.m_member_types.size());

				std::vector<std::unique_ptr<MarmotExpression>> inner_cases;
				std::unique_ptr<MarmotExpression> inner_body = syntax.BoolLiteral(true);
				for (size_t i = 0u; i < member_ctx.m_member_types.size(); i += 1u)
				{
					std::vector<std::unique_ptr<MarmotExpression>> eq_args;
					eq_args.emplace_back(syntax.LocalName(outer_bindings[i]));
					eq_args.emplace_back(syntax.LocalName(inner_bindings[i]));

					std::unique_ptr<MarmotExpression> compare_expr = syntax.QualifiedCall(std::string(EQUATABLE_CLASS_NAME), std::string(EQUALS_METHOD_NAME), std::move(eq_args));
					inner_body = syntax.Binary(Token::Name::DOUBLE_AMPERSAND, std::move(inner_body), std::move(compare_expr));
				}

				inner_cases.emplace_back
				(
					syntax.Case
					(
						make_constructor_pattern(ctor_name.m_lexeme, inner_bindings),
						std::move(inner_body)
					)
				);
				inner_cases.emplace_back(syntax.Case(syntax.WildcardPattern(), syntax.BoolLiteral(false)));

				outer_cases.emplace_back
				(
					syntax.Case
					(
						make_constructor_pattern(ctor_name.m_lexeme, outer_bindings),
						syntax.Match(syntax.LocalName(params[1]), std::move(inner_cases))
					)
				);
			}

			std::unique_ptr<MarmotExpression> body = syntax.Match(syntax.LocalName(params[0]), std::move(outer_cases));
			methods.emplace_back
			(
				Token(std::string(EQUALS_METHOD_NAME), Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name),
				MarmotStatement::FunctionDefinition(method_token, {}, syntax.Parameters(std::move(params)), std::move(param_types), std::shared_ptr<MarmotType>(MarmotType::MakeLiteralType<MarmotType::BoolType>()), std::move(body), MarmotCompiler::None, {})
			);
		}
		else if (m_operation == Operation::Convertible)
		{
			const std::string mangled_name = MarmotType::MangleInstanceMethodName(std::string(CONVERT_METHOD_NAME), derive_target.m_lexeme, type_args);
			Token method_token(mangled_name, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
			std::vector<GeneratedBinding> params{ syntax.Local(0) };
			std::vector<std::shared_ptr<MarmotType>> param_types{ std::shared_ptr<MarmotType>(union_stmt.m_self_type) };

			// `Union::Variant(field, ...)`, as a variant is written.
			std::vector<std::unique_ptr<MarmotExpression>> cases;
			for (const Token& ctor_name : union_stmt.m_constructor_names)
			{
				const MarmotType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(ctor_name.m_lexeme);
				const std::vector<GeneratedBinding> fields = syntax.Locals(1, member_ctx.m_member_types.size());
				std::unique_ptr<MarmotExpression> case_body = syntax.TextLiteral(ctor_name.m_lexeme + "(");
				for (size_t i = 0u; i < member_ctx.m_member_types.size(); i += 1u)
				{
					if (i != 0u)
					{
						case_body = syntax.Concat(std::move(case_body), syntax.TextLiteral(", "));
					}
					case_body = syntax.Concat(std::move(case_body), syntax.AsText(syntax.LocalName(fields[i])));
				}
				case_body = syntax.Concat(std::move(case_body), syntax.TextLiteral(")"));
				cases.emplace_back(syntax.Case(make_constructor_pattern(ctor_name.m_lexeme, fields), std::move(case_body)));
			}

			std::unique_ptr<MarmotExpression> body = syntax.Match(syntax.LocalName(params[0]), std::move(cases));
			methods.emplace_back
			(
				Token(std::string(CONVERT_METHOD_NAME), Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name),
				MarmotStatement::FunctionDefinition(method_token, {}, syntax.Parameters(std::move(params)), std::move(param_types), std::shared_ptr<MarmotType>(MarmotType::MakeLiteralType<MarmotType::TextType>()), std::move(body), MarmotCompiler::None, {})
			);
		}
		else if (m_operation == Operation::Orderable)
		{
			const std::string mangled_name = MarmotType::MangleInstanceMethodName(std::string(COMPARE_METHOD_NAME), derive_target.m_lexeme, type_args);
			Token method_token(mangled_name, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
			std::vector<GeneratedBinding> params
			{
				syntax.Local(0),
				syntax.Local(1)
			};
			std::vector<std::shared_ptr<MarmotType>> param_types{ std::shared_ptr<MarmotType>(union_stmt.m_self_type), std::shared_ptr<MarmotType>(union_stmt.m_self_type) };

			// A variant declared earlier is the smaller; the same variant
			// compares its fields in order.
			const std::vector<Token>& variants = union_stmt.m_constructor_names;
			std::vector<std::unique_ptr<MarmotExpression>> outer_cases;
			for (size_t left = 0u; left < variants.size(); left += 1u)
			{
				const size_t field_count = union_type.m_member_info.at(variants[left].m_lexeme).m_member_types.size();
				const int left_start = 2;
				const int right_start = left_start + static_cast<int>(field_count);
				const int order_start = right_start + static_cast<int>(field_count);
				const std::vector<GeneratedBinding> left_bindings = syntax.Locals(left_start, field_count);
				const std::vector<GeneratedBinding> right_bindings = syntax.Locals(right_start, field_count);

				std::vector<std::unique_ptr<MarmotExpression>> inner_cases;
				for (size_t right = 0u; right < variants.size(); right += 1u)
				{
					if (right == left)
					{
						std::unique_ptr<MarmotExpression> same = syntax.OrderingValue(*ordering, MarmotOrdering::Equal);
						for (size_t i = field_count; i > 0u; i -= 1u)
						{
							const int field = static_cast<int>(i - 1u);
							same = syntax.CompareThen(*ordering, syntax.LocalName(left_bindings[field]), syntax.LocalName(right_bindings[field]), order_start + field, std::move(same));
						}
						inner_cases.emplace_back(syntax.Case(make_constructor_pattern(variants[right].m_lexeme, right_bindings), std::move(same)));
						continue;
					}

					std::vector<std::unique_ptr<MarmotPattern>> wildcards;
					for (size_t i = 0u; i < union_type.m_member_info.at(variants[right].m_lexeme).m_member_types.size(); i += 1u)
					{
						wildcards.emplace_back(syntax.WildcardPattern());
					}
					std::unique_ptr<MarmotExpression> order = syntax.OrderingValue(*ordering, right < left ? MarmotOrdering::Greater : MarmotOrdering::Less);
					inner_cases.emplace_back(syntax.Case(syntax.ConstructorPattern(variants[right].m_lexeme, std::move(wildcards)), std::move(order)));
				}

				outer_cases.emplace_back(syntax.Case(make_constructor_pattern(variants[left].m_lexeme, left_bindings), syntax.Match(syntax.LocalName(params[1]), std::move(inner_cases))));
			}

			std::unique_ptr<MarmotExpression> body = syntax.Match(syntax.LocalName(params[0]), std::move(outer_cases));
			methods.emplace_back
			(
				Token(std::string(COMPARE_METHOD_NAME), Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name),
				MarmotStatement::FunctionDefinition(method_token, {}, syntax.Parameters(std::move(params)), std::move(param_types), std::shared_ptr<MarmotType>(ordering->m_type), std::move(body), MarmotCompiler::None, {})
			);
		}
		else
		{
			const std::string mangled_name = MarmotType::MangleInstanceMethodName(std::string(HASH_METHOD_NAME), derive_target.m_lexeme, type_args);
			Token method_token(mangled_name, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
			std::vector<GeneratedBinding> params{ syntax.Local(0) };
			std::vector<std::shared_ptr<MarmotType>> param_types{ std::shared_ptr<MarmotType>(union_stmt.m_self_type) };

			std::vector<std::unique_ptr<MarmotExpression>> cases;
			for (const Token& ctor_name : union_stmt.m_constructor_names)
			{
				const MarmotType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(ctor_name.m_lexeme);
				const std::vector<GeneratedBinding> fields = syntax.Locals(1, member_ctx.m_member_types.size());
				std::unique_ptr<MarmotExpression> case_body = syntax.IntLiteral(member_ctx.m_tag);
				for (size_t i = 0u; i < member_ctx.m_member_types.size(); i += 1u)
				{
					std::vector<std::unique_ptr<MarmotExpression>> hash_args;
					hash_args.emplace_back(syntax.LocalName(fields[i]));

					std::unique_ptr<MarmotExpression> member_hash = syntax.QualifiedCall(std::string(HASHABLE_CLASS_NAME), std::string(HASH_METHOD_NAME), std::move(hash_args));
					std::unique_ptr<MarmotExpression> scaled = syntax.Binary(Token::Name::STAR, std::move(case_body), syntax.IntLiteral(31));
					case_body = syntax.Binary(Token::Name::SINGLE_PLUS, std::move(scaled), std::move(member_hash));
				}

				cases.emplace_back
				(
					syntax.Case
					(
						make_constructor_pattern(ctor_name.m_lexeme, fields),
						std::move(case_body)
					)
				);
			}

			std::unique_ptr<MarmotExpression> body = syntax.Match(syntax.LocalName(params[0]), std::move(cases));
			methods.emplace_back
			(
				Token(std::string(HASH_METHOD_NAME), Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name),
				MarmotStatement::FunctionDefinition(method_token, {}, syntax.Parameters(std::move(params)), std::move(param_types), std::shared_ptr<MarmotType>(MarmotType::MakeLiteralType<MarmotType::IntegerType>()), std::move(body), MarmotCompiler::None, {})
			);
		}

		std::vector<MarmotType::ClassConstraint> constraints = DerivedConstraints(derive_target.m_lexeme, union_stmt.m_generic_params, type_args);

		return std::make_unique<MarmotStatement>(MarmotStatement::Instance(class_token, std::move(type_args), std::move(constraints), {}, std::move(methods)));
	}

	const std::string& first_type_param = union_type.m_generic_params[0u];

	// Bind and Unwrap take each variant as it is or by its one field of the
	// first type parameter: the index of that field, or -1 for none.
	auto mapped_field_of = [&union_stmt, &union_type, &first_type_param, &derive_target](const MarmotType::UnionType::UnionMemberContext& member_ctx) -> MarmotCompiler::Expected<int, DerivedDeclarationError>
	{
		int mapped_field_index = -1;
		bool has_other_generic_field = false;
		for (size_t i = 0u; i < member_ctx.m_member_types.size(); i += 1u)
		{
			const std::shared_ptr<MarmotType>& member_type = member_ctx.m_member_types[i];
			if (IsExactGenericParam(member_type, first_type_param))
			{
				if (mapped_field_index != -1)
				{
					return MarmotCompiler::Failure(DerivedDeclarationError(std::format("{} deriving requires variants to contain at most one direct field of the first type parameter.", derive_target.m_lexeme), union_stmt.m_name));
				}
				mapped_field_index = static_cast<int>(i);
			}
			else if (IsUnionSelfReference(member_type, union_type.m_name) || ContainsGenericParam(member_type, first_type_param))
			{
				has_other_generic_field = true;
			}
		}

		if (has_other_generic_field || (mapped_field_index >= 0 && member_ctx.m_member_types.size() != 1u))
		{
			return MarmotCompiler::Failure(DerivedDeclarationError(std::format("{} deriving only supports pass-through variants or single-value variants.", derive_target.m_lexeme), union_stmt.m_name));
		}
		return mapped_field_index;
	};

	std::string mapped_type_param_name = "B";
	while (std::ranges::any_of(union_stmt.m_generic_params, [&mapped_type_param_name](const Token& token) { return token.m_lexeme == mapped_type_param_name; }))
	{
		mapped_type_param_name.append("Result");
	}

	std::vector<Token> generic_params;
	generic_params.reserve(union_stmt.m_generic_params.size() + 1u);
	generic_params.push_back(union_stmt.m_generic_params[0u]);
	if (m_operation != Operation::Unwrap)
	{
		generic_params.push_back(Token(mapped_type_param_name, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name));
	}
	for (size_t i = 1u; i < union_stmt.m_generic_params.size(); i += 1u)
	{
		generic_params.push_back(union_stmt.m_generic_params[i]);
	}

	const Token& function_name = *m_global_name;

	std::shared_ptr<MarmotType> input_union_type = std::shared_ptr<MarmotType>(union_stmt.m_self_type);
	std::shared_ptr<MarmotType> mapped_type = MarmotType::MakeGenericType(mapped_type_param_name);
	std::shared_ptr<MarmotType> source_type = MarmotType::MakeGenericType(first_type_param);
	std::unordered_map<std::string, std::shared_ptr<MarmotType>> output_substitutions;
	output_substitutions.emplace(first_type_param, mapped_type);
	std::shared_ptr<MarmotType> output_union_type = TypeTransform::Instantiate(union_stmt.m_self_type, output_substitutions);

	std::vector<GeneratedBinding> params;
	std::vector<std::shared_ptr<MarmotType>> param_types;
	params.emplace_back(syntax.Local(0));
	param_types.emplace_back(input_union_type);

	if (m_operation == Operation::Map)
	{
		params.emplace_back(syntax.Local(1));
		param_types.emplace_back(MarmotType::MakeFunctionType(std::vector<std::shared_ptr<MarmotType>>{ source_type }, std::shared_ptr<MarmotType>(mapped_type)));
		const int case_binding_base_index = static_cast<int>(params.size());

		std::vector<std::unique_ptr<MarmotExpression>> cases;
		for (const Token& ctor_name : union_stmt.m_constructor_names)
		{
			const MarmotType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(ctor_name.m_lexeme);
			const std::vector<GeneratedBinding> fields = syntax.Locals(case_binding_base_index, member_ctx.m_member_types.size());
			std::vector<std::unique_ptr<MarmotExpression>> ctor_args;
			for (size_t i = 0u; i < member_ctx.m_member_types.size(); i += 1u)
			{
				const std::shared_ptr<MarmotType>& member_type = member_ctx.m_member_types[i];
				const GeneratedBinding& local = fields[i];

				if (IsExactGenericParam(member_type, first_type_param))
				{
					std::vector<std::unique_ptr<MarmotExpression>> call_args;
					call_args.emplace_back(syntax.LocalName(local));
					ctor_args.emplace_back(syntax.Call(syntax.LocalName(params[1]), std::move(call_args)));
				}
				else if (IsUnionSelfReference(member_type, union_type.m_name))
				{
					std::vector<std::unique_ptr<MarmotExpression>> call_args;
					call_args.emplace_back(syntax.LocalName(local));
					call_args.emplace_back(syntax.LocalName(params[1]));
					ctor_args.emplace_back(syntax.Call(syntax.GlobalName(function_name.m_lexeme), std::move(call_args)));
				}
				else
				{
					if (ContainsGenericParam(member_type, first_type_param))
					{
						return MarmotCompiler::Failure(DerivedDeclarationError("Map deriving only supports direct occurrences of the first type parameter or recursive self fields.", union_stmt.m_name));
					}
					ctor_args.emplace_back(syntax.LocalName(local));
				}
			}

			cases.emplace_back
			(
					syntax.Case
					(
						make_constructor_pattern(ctor_name.m_lexeme, fields),
						make_union_construct(ctor_name.m_lexeme, member_ctx.m_tag, std::move(ctor_args), output_union_type)
					)
				);
		}

		std::unique_ptr<MarmotExpression> body = syntax.Match(syntax.LocalName(params[0]), std::move(cases));
		return std::make_unique<MarmotStatement>
		(
			MarmotStatement::FunctionDefinition(function_name, std::move(generic_params), syntax.Parameters(std::move(params)), std::move(param_types), std::move(output_union_type), std::move(body), MarmotCompiler::None, {})
		);
	}

	if (m_operation == Operation::Bind)
	{
		params.emplace_back(syntax.Local(1));
		param_types.emplace_back(MarmotType::MakeFunctionType(std::vector<std::shared_ptr<MarmotType>>{ source_type }, std::shared_ptr<MarmotType>(output_union_type)));
		const int case_binding_base_index = static_cast<int>(params.size());

		std::vector<std::unique_ptr<MarmotExpression>> cases;
		for (const Token& ctor_name : union_stmt.m_constructor_names)
		{
			const MarmotType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(ctor_name.m_lexeme);
			const MarmotCompiler::Expected<int, DerivedDeclarationError> mapped_field = mapped_field_of(member_ctx);
			if (!mapped_field.HasValue())
			{
				return MarmotCompiler::Failure(mapped_field.Error());
			}
			const int mapped_field_index = mapped_field.Value();
			const std::vector<GeneratedBinding> fields = syntax.Locals(case_binding_base_index, member_ctx.m_member_types.size());

			std::unique_ptr<MarmotExpression> case_body;
			if (mapped_field_index >= 0)
			{
				std::vector<std::unique_ptr<MarmotExpression>> call_args;
				call_args.emplace_back(syntax.LocalName(fields[mapped_field_index]));
				case_body = syntax.Call(syntax.LocalName(params[1]), std::move(call_args));
			}
			else
			{
				std::vector<std::unique_ptr<MarmotExpression>> ctor_args;
				for (size_t i = 0u; i < member_ctx.m_member_types.size(); i += 1u)
				{
					ctor_args.emplace_back(syntax.LocalName(fields[i]));
				}
				case_body = make_union_construct(ctor_name.m_lexeme, member_ctx.m_tag, std::move(ctor_args), output_union_type);
			}

			cases.emplace_back
			(
					syntax.Case
					(
						make_constructor_pattern(ctor_name.m_lexeme, fields),
						std::move(case_body)
					)
				);
		}

		std::unique_ptr<MarmotExpression> body = syntax.Match(syntax.LocalName(params[0]), std::move(cases));
		return std::make_unique<MarmotStatement>
		(
			MarmotStatement::FunctionDefinition(function_name, std::move(generic_params), syntax.Parameters(std::move(params)), std::move(param_types), std::move(output_union_type), std::move(body), MarmotCompiler::None, {})
		);
	}

	params.emplace_back(syntax.Local(1));
	param_types.emplace_back(source_type);
	const int case_binding_base_index = static_cast<int>(params.size());

	std::vector<std::unique_ptr<MarmotExpression>> cases;
	for (const Token& ctor_name : union_stmt.m_constructor_names)
	{
		const MarmotType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(ctor_name.m_lexeme);
		const MarmotCompiler::Expected<int, DerivedDeclarationError> mapped_field = mapped_field_of(member_ctx);
		if (!mapped_field.HasValue())
		{
			return MarmotCompiler::Failure(mapped_field.Error());
		}
		const int mapped_field_index = mapped_field.Value();
		const std::vector<GeneratedBinding> fields = syntax.Locals(case_binding_base_index, member_ctx.m_member_types.size());

		std::unique_ptr<MarmotExpression> case_body =
			mapped_field_index >= 0
			? syntax.LocalName(fields[mapped_field_index])
			: syntax.LocalName(params[1]);

		cases.emplace_back
		(
			syntax.Case
			(
				make_constructor_pattern(ctor_name.m_lexeme, fields),
				std::move(case_body)
			)
		);
	}

	std::unique_ptr<MarmotExpression> body = syntax.Match(syntax.LocalName(params[0]), std::move(cases));
	return std::make_unique<MarmotStatement>
	(
		MarmotStatement::FunctionDefinition(function_name, std::move(generic_params), syntax.Parameters(std::move(params)), std::move(param_types), std::move(source_type), std::move(body), MarmotCompiler::None, {})
	);
}

