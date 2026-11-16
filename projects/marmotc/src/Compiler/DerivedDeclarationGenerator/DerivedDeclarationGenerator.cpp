#include "DerivedDeclarationGenerator.h"
#include "Bytecode/Format/Format.h"
#include "Compiler/Constant/Constant.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace
{
	bool IsStructuralTarget(std::string_view name)
	{
		return name == EQUATABLE_CLASS_NAME || name == HASHABLE_CLASS_NAME || name == ORDERABLE_CLASS_NAME || name == CONVERTABLE_CLASS_NAME;
	}

	bool IsExactGenericParam(const std::shared_ptr<MarmotType>& type, std::string_view name)
	{
		return type->IsType<MarmotType::GenericParam>() && type->GetType<MarmotType::GenericParam>().m_name == name;
	}

	// Each type parameter in the derived class, with the instance's type
	// arguments after the first: Convertable<T, Text> for Convertable<P, Text>.
	std::vector<MarmotType::ClassConstraint> DerivedConstraints(const std::string& class_name, const std::vector<Token>& generic_params, const std::vector<std::shared_ptr<MarmotType>>& type_args)
	{
		return generic_params
			| std::views::transform([&class_name, &type_args](const Token& param)
			{
				std::vector<std::shared_ptr<MarmotType>> constraint_args{ MarmotType::MakeGenericType(param.m_lexeme) };
				constraint_args.insert(constraint_args.end(), std::next(type_args.begin()), type_args.end());
				return MarmotType::ClassConstraint(class_name, std::move(constraint_args));
			})
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

	public:
		explicit SyntheticSyntax(const Token& anchor)
			: m_anchor(anchor)
		{
		}

		std::unique_ptr<MarmotExpression> LocalName(const std::string& name, int index) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(Token(name, Token::Name::IDENTIFIER_LITERAL, m_anchor), MarmotExpression::NameContext::Local{ index }));
		}

		std::unique_ptr<MarmotExpression> GlobalName(const std::string& name) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(Token(name, Token::Name::IDENTIFIER_LITERAL, m_anchor), MarmotExpression::NameContext::Global{}));
		}

		std::unique_ptr<MarmotExpression> Call(std::unique_ptr<MarmotExpression>&& callee, std::vector<std::unique_ptr<MarmotExpression>>&& args) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::Call(Token("(", Token::Name::LEFT_PAREN, m_anchor), std::move(callee), std::move(args)));
		}

		std::unique_ptr<MarmotExpression> QualifiedCall(const std::string& qualifier, const std::string& method_name, std::vector<std::unique_ptr<MarmotExpression>>&& args) const
		{
			return Call(GlobalName(qualifier + std::string(NameSeparator) + method_name), std::move(args));
		}

		std::unique_ptr<MarmotExpression> Binary(Token::Name token_name, const std::string& lexeme, std::unique_ptr<MarmotExpression>&& left, std::unique_ptr<MarmotExpression>&& right) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::Binary(Token(lexeme, token_name, m_anchor), std::move(left), std::move(right)));
		}

		std::unique_ptr<MarmotExpression> BoolLiteral(bool value) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::Literal(Token(value ? "true" : "false", value ? Token::Name::TRUE : Token::Name::FALSE, m_anchor), MarmotExpression::LiteralKind::Bool));
		}

		std::unique_ptr<MarmotExpression> IntLiteral(int value) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::Literal(Token(std::to_string(value), Token::Name::INTEGER_LITERAL, m_anchor), MarmotExpression::LiteralKind::Integer));
		}

		std::unique_ptr<MarmotExpression> Match(std::unique_ptr<MarmotExpression>&& scrutinee, std::vector<std::unique_ptr<MarmotExpression>>&& cases) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::Match(Token("match", Token::Name::MATCH, m_anchor), std::move(scrutinee), std::move(cases)));
		}

		std::unique_ptr<MarmotExpression> Case(std::unique_ptr<MarmotPattern>&& pattern, std::unique_ptr<MarmotExpression>&& body) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::Case(Token("case", Token::Name::CASE, m_anchor), std::move(pattern), std::move(body)));
		}

		std::unique_ptr<MarmotPattern> BindingPattern(const std::string& name, int index) const
		{
			return std::make_unique<MarmotPattern>(MarmotPattern::Binding(Token(name, Token::Name::IDENTIFIER_LITERAL, m_anchor), index));
		}

		std::unique_ptr<MarmotPattern> WildcardPattern() const
		{
			return std::make_unique<MarmotPattern>(MarmotPattern::Wildcard(Token("_", Token::Name::IDENTIFIER_LITERAL, m_anchor)));
		}

		std::unique_ptr<MarmotPattern> ConstructorPattern(const std::string& name, std::vector<std::unique_ptr<MarmotPattern>>&& fields) const
		{
			return std::make_unique<MarmotPattern>(MarmotPattern::Constructor(Token(name, Token::Name::IDENTIFIER_LITERAL, m_anchor), std::string(name), std::move(fields), true));
		}

		std::unique_ptr<MarmotExpression> TextLiteral(const std::string& value) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::Literal(Token(value, Token::Name::TEXT_LITERAL, m_anchor), MarmotExpression::LiteralKind::Text));
		}

		std::unique_ptr<MarmotExpression> Concat(std::unique_ptr<MarmotExpression>&& left, std::unique_ptr<MarmotExpression>&& right) const
		{
			return Binary(Token::Name::DOUBLE_PLUS, "++", std::move(left), std::move(right));
		}

		std::unique_ptr<MarmotExpression> AsText(std::unique_ptr<MarmotExpression>&& value) const
		{
			return std::make_unique<MarmotExpression>(MarmotExpression::As(Token("as", Token::Name::AS, m_anchor), MarmotType::MakeLiteralType<MarmotType::TextType>(), std::move(value)));
		}

		// `Orderable::Compare(left, right)`, or `equal` when that is 0. The
		// comparison is held in local `slot` on its way out.
		std::unique_ptr<MarmotExpression> CompareThen(std::unique_ptr<MarmotExpression>&& left, std::unique_ptr<MarmotExpression>&& right, int slot, std::unique_ptr<MarmotExpression>&& equal) const
		{
			std::vector<std::unique_ptr<MarmotExpression>> operands;
			operands.emplace_back(std::move(left));
			operands.emplace_back(std::move(right));
			std::vector<std::unique_ptr<MarmotExpression>> cases;
			cases.emplace_back(Case(std::make_unique<MarmotPattern>(MarmotPattern::Literal(Token("0", Token::Name::INTEGER_LITERAL, m_anchor), MarmotPattern::LiteralKind::Integer)), std::move(equal)));
			cases.emplace_back(Case(BindingPattern("order", slot), LocalName("order", slot)));
			return Match(QualifiedCall(std::string(ORDERABLE_CLASS_NAME), std::string(COMPARE_METHOD_NAME), std::move(operands)), std::move(cases));
		}
	};
}

DerivedDeclarationError::DerivedDeclarationError(std::string message, const Token& token)
	: m_message(std::move(message)),
	m_token(token)
{
}

DerivedDeclarationGenerator::DerivedDeclarationGenerator(Declaration declaration, Token target, std::optional<Token> global_name)
	: m_declaration(declaration),
	m_target(std::move(target)),
	m_global_name(std::move(global_name))
{
}

std::expected<DerivedDeclarationGenerator, DerivedDeclarationError> DerivedDeclarationGenerator::Prepare(const MarmotStatement::Struct& declaration, const Token& target)
{
	if (!IsStructuralTarget(target.m_lexeme) && target.m_lexeme != TRANSFERABLE_CLASS_NAME)
	{
		return std::unexpected(DerivedDeclarationError("Unsupported deriving target for struct.", target));
	}
	return DerivedDeclarationGenerator(&declaration, target, std::nullopt);
}

std::expected<DerivedDeclarationGenerator, DerivedDeclarationError> DerivedDeclarationGenerator::Prepare(const MarmotStatement::Union& declaration, const Token& target)
{
	const bool is_container = target.m_lexeme == "Map" || target.m_lexeme == "Bind" || target.m_lexeme == "Unwrap";
	if (!IsStructuralTarget(target.m_lexeme) && target.m_lexeme != TRANSFERABLE_CLASS_NAME && !is_container)
	{
		return std::unexpected(DerivedDeclarationError("Unsupported deriving target for union.", target));
	}
	std::optional<Token> global_name;
	if (is_container)
	{
		if (declaration.m_self_type->GetType<MarmotType::UnionType>().m_generic_params.empty())
		{
			return std::unexpected(DerivedDeclarationError("Container deriving requires at least one union type parameter.", declaration.m_name));
		}
		global_name.emplace(declaration.m_name.m_lexeme + target.m_lexeme, Token::Name::IDENTIFIER_LITERAL, declaration.m_name);
	}
	return DerivedDeclarationGenerator(&declaration, target, std::move(global_name));
}

Token* DerivedDeclarationGenerator::NameToReserve()
{
	return m_global_name ? &*m_global_name : nullptr;
}

DerivedDeclarationGenerator::StatementResult DerivedDeclarationGenerator::Generate() const
{
	return std::visit([this](const auto* declaration) { return Generate(*declaration); }, m_declaration);
}

DerivedDeclarationGenerator::StatementResult DerivedDeclarationGenerator::Generate(const MarmotStatement::Struct& struct_stmt) const
{
	const Token& derive_target = m_target;
	const MarmotType::StructType& struct_type = struct_stmt.m_self_type->GetType<MarmotType::StructType>();

	const SyntheticSyntax syntax(struct_stmt.m_name);

	auto make_member_access = [&struct_stmt, &syntax](const std::string& base_name, int base_index, const std::string& member_name, int member_index) -> std::unique_ptr<MarmotExpression>
	{
		Token member_token(member_name, Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name);
		return std::make_unique<MarmotExpression>(MarmotExpression::MemberAccess(member_token, syntax.LocalName(base_name, base_index), member_index));
	};

	std::vector<std::shared_ptr<MarmotType>> type_args{ std::shared_ptr<MarmotType>(struct_stmt.m_self_type) };
	if (derive_target.m_lexeme == CONVERTABLE_CLASS_NAME)
	{
		type_args.emplace_back(MarmotType::MakeLiteralType<MarmotType::TextType>());
	}
	Token class_token(derive_target.m_lexeme, Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name);
	std::vector<std::unique_ptr<MarmotStatement>> methods;

	if (derive_target.m_lexeme == TRANSFERABLE_CLASS_NAME)
	{
		return std::make_unique<MarmotStatement>(MarmotStatement::Instance(class_token, std::move(type_args), {}, {}, std::move(methods)));
	}

	if (derive_target.m_lexeme == EQUATABLE_CLASS_NAME)
	{
		const std::string mangled_name = MarmotType::MangleInstanceMethodName(std::string(EQUALS_METHOD_NAME), derive_target.m_lexeme, type_args);
		Token method_token(mangled_name, Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name);
		std::vector<Token> params
		{
			Token("a", Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name),
			Token("b", Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name)
		};
		std::vector<std::shared_ptr<MarmotType>> param_types{ std::shared_ptr<MarmotType>(struct_stmt.m_self_type), std::shared_ptr<MarmotType>(struct_stmt.m_self_type) };

		std::unique_ptr<MarmotExpression> body = syntax.BoolLiteral(true);
		for (size_t i = 0u; i < struct_type.m_member_types.size(); i += 1u)
		{
			std::vector<std::unique_ptr<MarmotExpression>> eq_args;
			eq_args.emplace_back(make_member_access("a", 0, struct_type.m_member_names[i], static_cast<int>(i)));
			eq_args.emplace_back(make_member_access("b", 1, struct_type.m_member_names[i], static_cast<int>(i)));

			std::unique_ptr<MarmotExpression> compare_expr = syntax.QualifiedCall(std::string(EQUATABLE_CLASS_NAME), std::string(EQUALS_METHOD_NAME), std::move(eq_args));
			body = syntax.Binary(Token::Name::DOUBLE_AMPERSAND, "&&", std::move(body), std::move(compare_expr));
		}

		methods.emplace_back
		(
			std::make_unique<MarmotStatement>
			(
				MarmotStatement::FunctionDefinition(method_token, {}, std::move(params), std::move(param_types), std::shared_ptr<MarmotType>(MarmotType::MakeLiteralType<MarmotType::BoolType>()), std::move(body), std::nullopt, 0, {})
			)
		);
	}
	else if (derive_target.m_lexeme == CONVERTABLE_CLASS_NAME)
	{
		const std::string mangled_name = MarmotType::MangleInstanceMethodName(std::string(CONVERT_METHOD_NAME), derive_target.m_lexeme, type_args);
		Token method_token(mangled_name, Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name);
		std::vector<Token> params{ Token("value", Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name) };
		std::vector<std::shared_ptr<MarmotType>> param_types{ std::shared_ptr<MarmotType>(struct_stmt.m_self_type) };

		// `Name { field = value, ... }`, each field as its own `as Text` gives it.
		std::unique_ptr<MarmotExpression> body = syntax.TextLiteral(struct_type.m_name + " { ");
		for (size_t i = 0u; i < struct_type.m_member_types.size(); i += 1u)
		{
			body = syntax.Concat(std::move(body), syntax.TextLiteral((i == 0u ? "" : ", ") + struct_type.m_member_names[i] + " = "));
			body = syntax.Concat(std::move(body), syntax.AsText(make_member_access("value", 0, struct_type.m_member_names[i], static_cast<int>(i))));
		}
		body = syntax.Concat(std::move(body), syntax.TextLiteral(" }"));

		methods.emplace_back
		(
			std::make_unique<MarmotStatement>
			(
				MarmotStatement::FunctionDefinition(method_token, {}, std::move(params), std::move(param_types), std::shared_ptr<MarmotType>(MarmotType::MakeLiteralType<MarmotType::TextType>()), std::move(body), std::nullopt, 0, {})
			)
		);
	}
	else if (derive_target.m_lexeme == ORDERABLE_CLASS_NAME)
	{
		const std::string mangled_name = MarmotType::MangleInstanceMethodName(std::string(COMPARE_METHOD_NAME), derive_target.m_lexeme, type_args);
		Token method_token(mangled_name, Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name);
		std::vector<Token> params
		{
			Token("a", Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name),
			Token("b", Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name)
		};
		std::vector<std::shared_ptr<MarmotType>> param_types{ std::shared_ptr<MarmotType>(struct_stmt.m_self_type), std::shared_ptr<MarmotType>(struct_stmt.m_self_type) };

		// The first field that differs, in declaration order, decides.
		std::unique_ptr<MarmotExpression> body = syntax.IntLiteral(0);
		for (size_t i = struct_type.m_member_types.size(); i > 0u; i -= 1u)
		{
			const size_t field = i - 1u;
			body = syntax.CompareThen(
				make_member_access("a", 0, struct_type.m_member_names[field], static_cast<int>(field)),
				make_member_access("b", 1, struct_type.m_member_names[field], static_cast<int>(field)),
				2 + static_cast<int>(field),
				std::move(body));
		}

		methods.emplace_back
		(
			std::make_unique<MarmotStatement>
			(
				MarmotStatement::FunctionDefinition(method_token, {}, std::move(params), std::move(param_types), std::shared_ptr<MarmotType>(MarmotType::MakeLiteralType<MarmotType::IntegerType>()), std::move(body), std::nullopt, 0, {})
			)
		);
	}
	else
	{
		const std::string mangled_name = MarmotType::MangleInstanceMethodName(std::string(HASH_METHOD_NAME), derive_target.m_lexeme, type_args);
		Token method_token(mangled_name, Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name);
		std::vector<Token> params{ Token("value", Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name) };
		std::vector<std::shared_ptr<MarmotType>> param_types{ std::shared_ptr<MarmotType>(struct_stmt.m_self_type) };

		std::unique_ptr<MarmotExpression> body = syntax.IntLiteral(0);
		for (size_t i = 0u; i < struct_type.m_member_types.size(); i += 1u)
		{
			std::vector<std::unique_ptr<MarmotExpression>> hash_args;
			hash_args.emplace_back(make_member_access("value", 0, struct_type.m_member_names[i], static_cast<int>(i)));

			std::unique_ptr<MarmotExpression> member_hash = syntax.QualifiedCall(std::string(HASHABLE_CLASS_NAME), std::string(HASH_METHOD_NAME), std::move(hash_args));
			std::unique_ptr<MarmotExpression> scaled = syntax.Binary(Token::Name::STAR, "*", std::move(body), syntax.IntLiteral(31));
			body = syntax.Binary(Token::Name::SINGLE_PLUS, "+", std::move(scaled), std::move(member_hash));
		}

		methods.emplace_back
		(
			std::make_unique<MarmotStatement>
			(
				MarmotStatement::FunctionDefinition(method_token, {}, std::move(params), std::move(param_types), std::shared_ptr<MarmotType>(MarmotType::MakeLiteralType<MarmotType::IntegerType>()), std::move(body), std::nullopt, 0, {})
			)
		);
	}

	std::vector<MarmotType::ClassConstraint> constraints = DerivedConstraints(derive_target.m_lexeme, struct_stmt.m_generic_params, type_args);

	return std::make_unique<MarmotStatement>(MarmotStatement::Instance(class_token, std::move(type_args), std::move(constraints), {}, std::move(methods)));
}

DerivedDeclarationGenerator::StatementResult DerivedDeclarationGenerator::Generate(const MarmotStatement::Union& union_stmt) const
{
	const Token& derive_target = m_target;
	const MarmotType::UnionType& union_type = union_stmt.m_self_type->GetType<MarmotType::UnionType>();

	const SyntheticSyntax syntax(union_stmt.m_name);

	auto make_constructor_pattern = [&syntax](const std::string& ctor_name, int first_local_index, size_t field_count, const std::string& prefix) -> std::unique_ptr<MarmotPattern>
	{
		std::vector<std::unique_ptr<MarmotPattern>> args;
		for (size_t i = 0u; i < field_count; i += 1u)
		{
			args.emplace_back(syntax.BindingPattern(prefix + std::to_string(i), first_local_index + static_cast<int>(i)));
		}
		return syntax.ConstructorPattern(ctor_name, std::move(args));
	};

	auto make_union_construct = [&union_stmt](const std::string& ctor_name, int tag, std::vector<std::unique_ptr<MarmotExpression>>&& args, const std::shared_ptr<MarmotType>& return_type) -> std::unique_ptr<MarmotExpression>
	{
		Token ctor_token(ctor_name, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
		return std::make_unique<MarmotExpression>(MarmotExpression::Construct(ctor_token, std::move(args), std::shared_ptr<MarmotType>(return_type), true, MarmotExpression::Construct::Union(tag)));
	};

	const bool is_structural = IsStructuralTarget(derive_target.m_lexeme);
	if (derive_target.m_lexeme == TRANSFERABLE_CLASS_NAME)
	{
		std::vector<std::shared_ptr<MarmotType>> type_args{ std::shared_ptr<MarmotType>(union_stmt.m_self_type) };
		Token class_token(derive_target.m_lexeme, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
		std::vector<std::unique_ptr<MarmotStatement>> methods;

		return std::make_unique<MarmotStatement>(MarmotStatement::Instance(class_token, std::move(type_args), {}, {}, std::move(methods)));
	}

	if (is_structural)
	{
		std::vector<std::shared_ptr<MarmotType>> type_args{ std::shared_ptr<MarmotType>(union_stmt.m_self_type) };
		if (derive_target.m_lexeme == CONVERTABLE_CLASS_NAME)
		{
			type_args.emplace_back(MarmotType::MakeLiteralType<MarmotType::TextType>());
		}
		Token class_token(derive_target.m_lexeme, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
		std::vector<std::unique_ptr<MarmotStatement>> methods;

		if (derive_target.m_lexeme == EQUATABLE_CLASS_NAME)
		{
			const std::string mangled_name = MarmotType::MangleInstanceMethodName(std::string(EQUALS_METHOD_NAME), derive_target.m_lexeme, type_args);
			Token method_token(mangled_name, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
			std::vector<Token> params
			{
				Token("a", Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name),
				Token("b", Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name)
			};
			std::vector<std::shared_ptr<MarmotType>> param_types{ std::shared_ptr<MarmotType>(union_stmt.m_self_type), std::shared_ptr<MarmotType>(union_stmt.m_self_type) };

			std::vector<std::unique_ptr<MarmotExpression>> outer_cases;
			for (const Token& ctor_name : union_stmt.m_constructor_names)
			{
				const MarmotType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(ctor_name.m_lexeme);
				const int outer_binding_start = 2;
				const int inner_binding_start = outer_binding_start + static_cast<int>(member_ctx.m_member_types.size());

				std::vector<std::unique_ptr<MarmotExpression>> inner_cases;
				std::unique_ptr<MarmotExpression> inner_body = syntax.BoolLiteral(true);
				for (size_t i = 0u; i < member_ctx.m_member_types.size(); i += 1u)
				{
					std::vector<std::unique_ptr<MarmotExpression>> eq_args;
					eq_args.emplace_back(syntax.LocalName("lhs" + std::to_string(i), outer_binding_start + static_cast<int>(i)));
					eq_args.emplace_back(syntax.LocalName("rhs" + std::to_string(i), inner_binding_start + static_cast<int>(i)));

					std::unique_ptr<MarmotExpression> compare_expr = syntax.QualifiedCall(std::string(EQUATABLE_CLASS_NAME), std::string(EQUALS_METHOD_NAME), std::move(eq_args));
					inner_body = syntax.Binary(Token::Name::DOUBLE_AMPERSAND, "&&", std::move(inner_body), std::move(compare_expr));
				}

				inner_cases.emplace_back
				(
					syntax.Case
					(
						make_constructor_pattern(ctor_name.m_lexeme, inner_binding_start, member_ctx.m_member_types.size(), "rhs"),
						std::move(inner_body)
					)
				);
				inner_cases.emplace_back(syntax.Case(syntax.WildcardPattern(), syntax.BoolLiteral(false)));

				outer_cases.emplace_back
				(
					syntax.Case
					(
						make_constructor_pattern(ctor_name.m_lexeme, outer_binding_start, member_ctx.m_member_types.size(), "lhs"),
						syntax.Match(syntax.LocalName("b", 1), std::move(inner_cases))
					)
				);
			}

			std::unique_ptr<MarmotExpression> body = syntax.Match(syntax.LocalName("a", 0), std::move(outer_cases));
			methods.emplace_back
			(
				std::make_unique<MarmotStatement>
				(
					MarmotStatement::FunctionDefinition(method_token, {}, std::move(params), std::move(param_types), std::shared_ptr<MarmotType>(MarmotType::MakeLiteralType<MarmotType::BoolType>()), std::move(body), std::nullopt, 0, {})
				)
			);
		}
		else if (derive_target.m_lexeme == CONVERTABLE_CLASS_NAME)
		{
			const std::string mangled_name = MarmotType::MangleInstanceMethodName(std::string(CONVERT_METHOD_NAME), derive_target.m_lexeme, type_args);
			Token method_token(mangled_name, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
			std::vector<Token> params{ Token("value", Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name) };
			std::vector<std::shared_ptr<MarmotType>> param_types{ std::shared_ptr<MarmotType>(union_stmt.m_self_type) };

			// `Union::Variant(field, ...)`, as a variant is written.
			std::vector<std::unique_ptr<MarmotExpression>> cases;
			for (const Token& ctor_name : union_stmt.m_constructor_names)
			{
				const MarmotType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(ctor_name.m_lexeme);
				std::unique_ptr<MarmotExpression> case_body = syntax.TextLiteral(ctor_name.m_lexeme + "(");
				for (size_t i = 0u; i < member_ctx.m_member_types.size(); i += 1u)
				{
					if (i != 0u)
					{
						case_body = syntax.Concat(std::move(case_body), syntax.TextLiteral(", "));
					}
					case_body = syntax.Concat(std::move(case_body), syntax.AsText(syntax.LocalName("field" + std::to_string(i), 1 + static_cast<int>(i))));
				}
				case_body = syntax.Concat(std::move(case_body), syntax.TextLiteral(")"));
				cases.emplace_back(syntax.Case(make_constructor_pattern(ctor_name.m_lexeme, 1, member_ctx.m_member_types.size(), "field"), std::move(case_body)));
			}

			std::unique_ptr<MarmotExpression> body = syntax.Match(syntax.LocalName("value", 0), std::move(cases));
			methods.emplace_back
			(
				std::make_unique<MarmotStatement>
				(
					MarmotStatement::FunctionDefinition(method_token, {}, std::move(params), std::move(param_types), std::shared_ptr<MarmotType>(MarmotType::MakeLiteralType<MarmotType::TextType>()), std::move(body), std::nullopt, 0, {})
				)
			);
		}
		else if (derive_target.m_lexeme == ORDERABLE_CLASS_NAME)
		{
			const std::string mangled_name = MarmotType::MangleInstanceMethodName(std::string(COMPARE_METHOD_NAME), derive_target.m_lexeme, type_args);
			Token method_token(mangled_name, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
			std::vector<Token> params
			{
				Token("a", Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name),
				Token("b", Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name)
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

				std::vector<std::unique_ptr<MarmotExpression>> inner_cases;
				for (size_t right = 0u; right < variants.size(); right += 1u)
				{
					if (right == left)
					{
						std::unique_ptr<MarmotExpression> same = syntax.IntLiteral(0);
						for (size_t i = field_count; i > 0u; i -= 1u)
						{
							const int field = static_cast<int>(i - 1u);
							same = syntax.CompareThen(syntax.LocalName("lhs" + std::to_string(field), left_start + field), syntax.LocalName("rhs" + std::to_string(field), right_start + field), order_start + field, std::move(same));
						}
						inner_cases.emplace_back(syntax.Case(make_constructor_pattern(variants[right].m_lexeme, right_start, field_count, "rhs"), std::move(same)));
						continue;
					}

					std::vector<std::unique_ptr<MarmotPattern>> wildcards;
					for (size_t i = 0u; i < union_type.m_member_info.at(variants[right].m_lexeme).m_member_types.size(); i += 1u)
					{
						wildcards.emplace_back(syntax.WildcardPattern());
					}
					std::unique_ptr<MarmotExpression> order = right < left
						? syntax.IntLiteral(1)
						: syntax.Binary(Token::Name::SINGLE_MINUS, "-", syntax.IntLiteral(0), syntax.IntLiteral(1));
					inner_cases.emplace_back(syntax.Case(syntax.ConstructorPattern(variants[right].m_lexeme, std::move(wildcards)), std::move(order)));
				}

				outer_cases.emplace_back(syntax.Case(make_constructor_pattern(variants[left].m_lexeme, left_start, field_count, "lhs"), syntax.Match(syntax.LocalName("b", 1), std::move(inner_cases))));
			}

			std::unique_ptr<MarmotExpression> body = syntax.Match(syntax.LocalName("a", 0), std::move(outer_cases));
			methods.emplace_back
			(
				std::make_unique<MarmotStatement>
				(
					MarmotStatement::FunctionDefinition(method_token, {}, std::move(params), std::move(param_types), std::shared_ptr<MarmotType>(MarmotType::MakeLiteralType<MarmotType::IntegerType>()), std::move(body), std::nullopt, 0, {})
				)
			);
		}
		else
		{
			const std::string mangled_name = MarmotType::MangleInstanceMethodName(std::string(HASH_METHOD_NAME), derive_target.m_lexeme, type_args);
			Token method_token(mangled_name, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
			std::vector<Token> params{ Token("value", Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name) };
			std::vector<std::shared_ptr<MarmotType>> param_types{ std::shared_ptr<MarmotType>(union_stmt.m_self_type) };

			std::vector<std::unique_ptr<MarmotExpression>> cases;
			for (const Token& ctor_name : union_stmt.m_constructor_names)
			{
				const MarmotType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(ctor_name.m_lexeme);
				std::unique_ptr<MarmotExpression> case_body = syntax.IntLiteral(member_ctx.m_tag);
				for (size_t i = 0u; i < member_ctx.m_member_types.size(); i += 1u)
				{
					std::vector<std::unique_ptr<MarmotExpression>> hash_args;
					hash_args.emplace_back(syntax.LocalName("field" + std::to_string(i), 1 + static_cast<int>(i)));

					std::unique_ptr<MarmotExpression> member_hash = syntax.QualifiedCall(std::string(HASHABLE_CLASS_NAME), std::string(HASH_METHOD_NAME), std::move(hash_args));
					std::unique_ptr<MarmotExpression> scaled = syntax.Binary(Token::Name::STAR, "*", std::move(case_body), syntax.IntLiteral(31));
					case_body = syntax.Binary(Token::Name::SINGLE_PLUS, "+", std::move(scaled), std::move(member_hash));
				}

				cases.emplace_back
				(
					syntax.Case
					(
						make_constructor_pattern(ctor_name.m_lexeme, 1, member_ctx.m_member_types.size(), "field"),
						std::move(case_body)
					)
				);
			}

			std::unique_ptr<MarmotExpression> body = syntax.Match(syntax.LocalName("value", 0), std::move(cases));
			methods.emplace_back
			(
				std::make_unique<MarmotStatement>
				(
					MarmotStatement::FunctionDefinition(method_token, {}, std::move(params), std::move(param_types), std::shared_ptr<MarmotType>(MarmotType::MakeLiteralType<MarmotType::IntegerType>()), std::move(body), std::nullopt, 0, {})
				)
			);
		}

		std::vector<MarmotType::ClassConstraint> constraints = DerivedConstraints(derive_target.m_lexeme, union_stmt.m_generic_params, type_args);

		return std::make_unique<MarmotStatement>(MarmotStatement::Instance(class_token, std::move(type_args), std::move(constraints), {}, std::move(methods)));
	}

	const std::string& first_type_param = union_type.m_generic_params[0u];

	// Bind and Unwrap take each variant as it is or by its one field of the
	// first type parameter: the index of that field, or -1 for none.
	auto mapped_field_of = [&union_stmt, &union_type, &first_type_param, &derive_target](const MarmotType::UnionType::UnionMemberContext& member_ctx) -> std::expected<int, DerivedDeclarationError>
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
					return std::unexpected(DerivedDeclarationError(std::format("{} deriving requires variants to contain at most one direct field of the first type parameter.", derive_target.m_lexeme), union_stmt.m_name));
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
			return std::unexpected(DerivedDeclarationError(std::format("{} deriving only supports pass-through variants or single-value variants.", derive_target.m_lexeme), union_stmt.m_name));
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
	if (derive_target.m_lexeme != "Unwrap")
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
	std::shared_ptr<MarmotType> output_union_type = MarmotType::SubstituteTypeParams(union_stmt.m_self_type, output_substitutions);

	std::vector<Token> params;
	std::vector<std::shared_ptr<MarmotType>> param_types;
	params.emplace_back(Token("value", Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name));
	param_types.emplace_back(input_union_type);

	if (derive_target.m_lexeme == "Map")
	{
		params.emplace_back(Token("f", Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name));
		param_types.emplace_back(MarmotType::MakeFunctionType(std::vector<std::shared_ptr<MarmotType>>{ source_type }, std::shared_ptr<MarmotType>(mapped_type)));
		const int case_binding_base_index = static_cast<int>(params.size());

		std::vector<std::unique_ptr<MarmotExpression>> cases;
		for (const Token& ctor_name : union_stmt.m_constructor_names)
		{
			const MarmotType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(ctor_name.m_lexeme);
			std::vector<std::unique_ptr<MarmotExpression>> ctor_args;
			for (size_t i = 0u; i < member_ctx.m_member_types.size(); i += 1u)
			{
				const std::shared_ptr<MarmotType>& member_type = member_ctx.m_member_types[i];
				const int local_index = case_binding_base_index + static_cast<int>(i);
				const std::string binding_name = "field" + std::to_string(i);

				if (IsExactGenericParam(member_type, first_type_param))
				{
					std::vector<std::unique_ptr<MarmotExpression>> call_args;
					call_args.emplace_back(syntax.LocalName(binding_name, local_index));
					ctor_args.emplace_back(syntax.Call(syntax.LocalName("f", 1), std::move(call_args)));
				}
				else if (IsUnionSelfReference(member_type, union_type.m_name))
				{
					std::vector<std::unique_ptr<MarmotExpression>> call_args;
					call_args.emplace_back(syntax.LocalName(binding_name, local_index));
					call_args.emplace_back(syntax.LocalName("f", 1));
					ctor_args.emplace_back(syntax.Call(syntax.GlobalName(function_name.m_lexeme), std::move(call_args)));
				}
				else
				{
					if (ContainsGenericParam(member_type, first_type_param))
					{
						return std::unexpected(DerivedDeclarationError("Map deriving only supports direct occurrences of the first type parameter or recursive self fields.", union_stmt.m_name));
					}
					ctor_args.emplace_back(syntax.LocalName(binding_name, local_index));
				}
			}

			cases.emplace_back
			(
					syntax.Case
					(
						make_constructor_pattern(ctor_name.m_lexeme, case_binding_base_index, member_ctx.m_member_types.size(), "field"),
						make_union_construct(ctor_name.m_lexeme, member_ctx.m_tag, std::move(ctor_args), output_union_type)
					)
				);
		}

		std::unique_ptr<MarmotExpression> body = syntax.Match(syntax.LocalName("value", 0), std::move(cases));
		return std::make_unique<MarmotStatement>
		(
			MarmotStatement::FunctionDefinition(function_name, std::move(generic_params), std::move(params), std::move(param_types), std::move(output_union_type), std::move(body), std::nullopt, 0, {})
		);
	}

	if (derive_target.m_lexeme == "Bind")
	{
		params.emplace_back(Token("f", Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name));
		param_types.emplace_back(MarmotType::MakeFunctionType(std::vector<std::shared_ptr<MarmotType>>{ source_type }, std::shared_ptr<MarmotType>(output_union_type)));
		const int case_binding_base_index = static_cast<int>(params.size());

		std::vector<std::unique_ptr<MarmotExpression>> cases;
		for (const Token& ctor_name : union_stmt.m_constructor_names)
		{
			const MarmotType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(ctor_name.m_lexeme);
			const std::expected<int, DerivedDeclarationError> mapped_field = mapped_field_of(member_ctx);
			if (!mapped_field.has_value())
			{
				return std::unexpected(mapped_field.error());
			}
			const int mapped_field_index = mapped_field.value();

			std::unique_ptr<MarmotExpression> case_body;
			if (mapped_field_index >= 0)
			{
				std::vector<std::unique_ptr<MarmotExpression>> call_args;
				call_args.emplace_back(syntax.LocalName("field" + std::to_string(mapped_field_index), case_binding_base_index + mapped_field_index));
				case_body = syntax.Call(syntax.LocalName("f", 1), std::move(call_args));
			}
			else
			{
				std::vector<std::unique_ptr<MarmotExpression>> ctor_args;
				for (size_t i = 0u; i < member_ctx.m_member_types.size(); i += 1u)
				{
					ctor_args.emplace_back(syntax.LocalName("field" + std::to_string(i), case_binding_base_index + static_cast<int>(i)));
				}
				case_body = make_union_construct(ctor_name.m_lexeme, member_ctx.m_tag, std::move(ctor_args), output_union_type);
			}

			cases.emplace_back
			(
					syntax.Case
					(
						make_constructor_pattern(ctor_name.m_lexeme, case_binding_base_index, member_ctx.m_member_types.size(), "field"),
						std::move(case_body)
					)
				);
		}

		std::unique_ptr<MarmotExpression> body = syntax.Match(syntax.LocalName("value", 0), std::move(cases));
		return std::make_unique<MarmotStatement>
		(
			MarmotStatement::FunctionDefinition(function_name, std::move(generic_params), std::move(params), std::move(param_types), std::move(output_union_type), std::move(body), std::nullopt, 0, {})
		);
	}

	params.emplace_back(Token("default_value", Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name));
	param_types.emplace_back(source_type);
	const int case_binding_base_index = static_cast<int>(params.size());

	std::vector<std::unique_ptr<MarmotExpression>> cases;
	for (const Token& ctor_name : union_stmt.m_constructor_names)
	{
		const MarmotType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(ctor_name.m_lexeme);
		const std::expected<int, DerivedDeclarationError> mapped_field = mapped_field_of(member_ctx);
		if (!mapped_field.has_value())
		{
			return std::unexpected(mapped_field.error());
		}
		const int mapped_field_index = mapped_field.value();

		std::unique_ptr<MarmotExpression> case_body =
			mapped_field_index >= 0
			? syntax.LocalName("field" + std::to_string(mapped_field_index), case_binding_base_index + mapped_field_index)
			: syntax.LocalName("default_value", 1);

		// A variant that falls back to the default reads none of its fields; the
		// leading underscore is what keeps the unused-binding warning off code the
		// program never wrote.
		cases.emplace_back
		(
			syntax.Case
			(
				make_constructor_pattern(ctor_name.m_lexeme, case_binding_base_index, member_ctx.m_member_types.size(), mapped_field_index >= 0 ? "field" : "_field"),
				std::move(case_body)
			)
		);
	}

	std::unique_ptr<MarmotExpression> body = syntax.Match(syntax.LocalName("value", 0), std::move(cases));
	return std::make_unique<MarmotStatement>
	(
		MarmotStatement::FunctionDefinition(function_name, std::move(generic_params), std::move(params), std::move(param_types), std::move(source_type), std::move(body), std::nullopt, 0, {})
	);
}

