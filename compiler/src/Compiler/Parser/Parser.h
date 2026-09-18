#pragma once

#include <expected>
#include <queue>
#include <string_view>
#include <unordered_map>

#include "Common/Error/Error.h"
#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"
#include "Compiler/Module/CompiledModule.h"
#include "Compiler/Module/Module.h"
#include "Compiler/Result/Result.h"

class Parser
{
private:
	struct VariableContext
	{
		std::optional<int> m_relative_index = std::nullopt;
		std::optional<int> m_absolute_index = std::nullopt;
		std::optional<int> m_function_depth = std::nullopt;

		VariableContext() = default;

		VariableContext(int relative_index, int absolute_index, int function_depth);
	};

	struct Scope
	{
		using VariableTable = std::unordered_map<std::string, VariableContext>;
		using StructConstructorTable = std::unordered_map<std::string, std::shared_ptr<MidoriType>>;
		using UnionConstructorTable = std::unordered_map<std::string, std::shared_ptr<MidoriType>>;
		using DefinedTypeTable = std::unordered_map<std::string, std::shared_ptr<MidoriType>>;
		using AliasGenericParamTable = std::unordered_map<std::string, std::vector<std::string>>;
		using DefinedNames = std::unordered_set<std::string>;

		VariableTable m_variables;
		StructConstructorTable m_struct_constructors;
		UnionConstructorTable m_union_constructors;
		DefinedTypeTable m_defined_types;
		// An alias resolves to its expansion at parse time, and substitution clears the
		// expansion's own m_generic_params. The parameters a parameterised alias binds
		// therefore belong to the alias, not to the type it expands to, and are kept here
		// beside m_defined_types under the same name. Only parameterised aliases get an
		// entry, so an unparameterised one resolves exactly as it always has.
		AliasGenericParamTable m_alias_generic_params;
		DefinedNames m_defined_names;
	};

	// The prologue every nominal type declaration shares: name, optional generic
	// parameters, optional 'where' constraints. `type` and `alias` differ only in what
	// follows it, and `type` dispatches further on the shape of its body, so all of
	// them parse the prologue through one function and hand the result to a body parser.
	struct TypeDeclarationHeader
	{
		Token m_name;
		std::string m_name_before_mangle;
		std::vector<Token> m_generic_params;
		std::vector<std::shared_ptr<MidoriType>> m_generic_param_types;
		std::vector<MidoriType::ClassConstraint> m_constraints;
		bool m_has_generic_params = false;

		TypeDeclarationHeader(Token&& name, std::string&& name_before_mangle)
			: m_name(std::move(name)), m_name_before_mangle(std::move(name_before_mangle))
		{
		}
	};

	// The result of matching a name against the struct and union constructor tables.
	// A construction and an ordinary call are told apart by this lookup rather than by a
	// leading keyword, so the lookup lives on its own.
	struct ConstructorResolution
	{
		std::shared_ptr<MidoriType> m_type;
		std::string m_constructor_name;
		bool m_is_struct;

		ConstructorResolution(std::shared_ptr<MidoriType>&& type, std::string&& constructor_name, bool is_struct)
			: m_type(std::move(type)), m_constructor_name(std::move(constructor_name)), m_is_struct(is_struct)
		{
		}
	};

	using ConstructorResolutionResult = MidoriResult::Result<std::optional<ConstructorResolution>>;

	using Scopes = std::vector<Scope>;

	using TypeclassMethodMap = std::unordered_map<std::string, std::unordered_set<std::string>>;
	using TypeclassInstanceMap = std::unordered_map<std::string, std::vector<std::string>>;
	using TypeclassInstanceTypeMap = std::unordered_map<std::string, std::vector<std::vector<std::shared_ptr<MidoriType>>>>;
	using TypeclassAssociatedTypeMap = std::unordered_map<std::string, std::vector<std::string>>;
	using TypeclassInstanceAssociatedTypeBindingMap = std::unordered_map<std::string, std::vector<std::unordered_map<std::string, std::shared_ptr<MidoriType>>>>;
	using TypeEnvironment = std::unordered_map<std::string, std::shared_ptr<MidoriType>>;
	using TypeclassMethodTypeMap = std::unordered_map<std::string, std::unordered_map<std::string, std::shared_ptr<MidoriType>>>;

	struct ParseContext
	{
		std::unordered_map<std::string, CompiledModule::SymbolTable> m_imported_symbols;
		std::unordered_map<std::string, TypeEnvironment> m_imported_type_signatures;
		TokenStream m_tokens;
		std::string m_file_name;
		const ModuleDeclaration* m_current_module = nullptr;
		const std::unordered_map<std::string, ModuleDeclaration>* m_module_declarations = nullptr;
		const std::unordered_map<std::string, std::vector<UseImport>>* m_use_imports = nullptr;
		const std::vector<std::string>* m_source_lines = nullptr;

		ParseContext(TokenStream&& tokens, std::string_view file_name, const std::vector<std::string>& source_lines, const std::unordered_map<std::string, CompiledModule::SymbolTable>& imports, const std::unordered_map<std::string, TypeEnvironment>& imported_type_signatures, const ModuleDeclaration* module_decl);
	};

	struct ParseState
	{
		TypeclassMethodMap m_class_methods;
		TypeclassInstanceMap m_typeclass_type_params;
		TypeclassAssociatedTypeMap m_typeclass_associated_types;
		TypeclassInstanceMap m_class_instances;
		TypeclassInstanceTypeMap m_class_instance_type_args;
		TypeclassInstanceAssociatedTypeBindingMap m_class_instance_associated_type_bindings;
		TypeclassMethodTypeMap m_typeclass_method_types;
		Scopes m_scopes{ Scope() };
		std::vector<int> m_function_base_variable_index{0};
		std::vector<UseImport> m_current_use_imports;
		std::vector<std::string> m_namespaces;
		std::vector<MidoriType::ClassConstraint> m_active_constraints;
		std::vector<std::shared_ptr<MidoriType>> m_active_union_types;
		int m_function_depth = 0;
		int m_current_token_index = 0;
		int m_total_locals_in_curr_scope = 0;
		int m_total_variables = 0;
		bool m_allow_implicit_generic_params = false;

		ParseState(const std::vector<UseImport>& use_imports);
	};

	struct ActiveConstraintGuard
	{
		Parser* m_parser = nullptr;
		size_t m_prev_size = 0u;

		ActiveConstraintGuard(Parser* parser, size_t prev_size);

		~ActiveConstraintGuard();

		ActiveConstraintGuard(const ActiveConstraintGuard&) = delete;
		ActiveConstraintGuard& operator=(const ActiveConstraintGuard&) = delete;
	};

	struct ArrayComprehensionProbe
	{
		bool m_is_candidate = false;
		std::optional<int> m_loop_variable_offset = std::nullopt;
	};

	enum class ImportedSymbolAccess
	{
		Accessible,
		ModuleNotFound,
		SymbolNotExported,
		PrivateInaccessible
	};

	enum class UseImportResolutionStatus
	{
		NotImported,
		Resolved,
		Ambiguous
	};

	struct UseImportResolution
	{
		UseImportResolutionStatus m_status = UseImportResolutionStatus::NotImported;
		std::string m_module_name;
		std::vector<std::string> m_conflicting_modules;
	};

	ParseContext m_context;
	ParseState m_state;
	std::vector<CompilerWarning> m_warnings;
	std::queue<std::unique_ptr<MidoriStatement>> m_pending_statements;

	friend struct ParserTestAccess;

public:
	Parser(TokenStream&& tokens, std::string_view file_name, const std::vector<std::string>& source_lines, const std::unordered_map<std::string, CompiledModule::SymbolTable>& imports, const std::unordered_map<std::string, TypeEnvironment>& imported_type_signatures, const std::vector<UseImport>& use_imports, const ModuleDeclaration* module_decl, const CompiledModule::TypeclassMetadataMap& imported_typeclass_metadata = {});

	MidoriResult::ParserResult Parse();

	const TypeclassMethodMap& GetTypeclassMethods() const;
	const std::vector<CompilerWarning>& GetWarnings() const;

	CompiledModule::TypeclassMetadataMap GetTypeclassMetadata() const;

private:
	
	template <typename... T>
		requires (std::is_same_v<T, Token::Name> && ...)
	bool Match(T... tokens)
	{
		if ((... || Check(tokens, 0)))
		{
			Advance();
			return true;
		}
		return false;
	}
	
	template <typename... T>
		requires (std::is_same_v<T, Token::Name> && ...)
	MidoriResult::ExpressionResult ParseBinary(MidoriResult::ExpressionResult(Parser::* operand)(), T... tokens)
	{
		return (this->*operand)()
			.and_then
			(
				[operand, ...tokens = std::move(tokens), this](std::unique_ptr<MidoriExpression>&& lower_expr) ->MidoriResult::ExpressionResult
				{
					while (Match(tokens...))
					{
						Token& op = Previous();
						MidoriResult::ExpressionResult right = (this->*operand)();
						if (!right.has_value())
						{
							return std::unexpected(std::move(right.error()));
						}

						if (std::optional<CompilerError> error = CheckOperatorMixing(op, *lower_expr))
						{
							return std::unexpected(std::move(error.value()));
						}
						if (std::optional<CompilerError> error = CheckOperatorMixing(op, *right.value()))
						{
							return std::unexpected(std::move(error.value()));
						}

						lower_expr = std::make_unique<MidoriExpression>(MidoriExpression::Binary(op, std::move(lower_expr), std::move(right.value())));
					}

					return lower_expr;
				}
			);
	}

	template<typename OutputType, typename ParseFunc>
	std::expected<OutputType, CompilerError> TryParser(ParseFunc&& func)
	{
		ParseState checkpoint = m_state;
		return func()
			.or_else
			(
				[&checkpoint, this](CompilerError&& error) -> std::expected<OutputType, CompilerError> 
				{
					m_state = std::move(checkpoint);
					return std::unexpected(std::move(error));
				}
			);
	}

	template<typename OutputType>
	std::expected<OutputType, CompilerError> NoMatch()
	{
		return std::unexpected(CompilerError::NoMatch());
	}

	template<typename OutputType, typename ParseFunc>
	std::expected<OutputType, CompilerError> ParseWhen(ParseState& state, Token::Name token, ParseFunc&& func)
	{
		if (!Check(state, token, 0))
		{
			return NoMatch<OutputType>();
		}

		Token consumed = Advance(state);
		return func(std::move(consumed));
	}

	template<typename OutputType, typename First, typename... Rest>
	std::expected<OutputType, CompilerError> ParseChoice(ParseState& state, First&& first, Rest&&... rest)
	{
		static_cast<void>(state);
		std::expected<OutputType, CompilerError> result = first();
		if (result.has_value())
		{
			return result;
		}

		if (!IsNoMatchError(result.error()))
		{
			return result;
		}

		if constexpr (sizeof...(Rest) == 0)
		{
			return result;
		}
		else
		{
			return ParseChoice<OutputType>(state, std::forward<Rest>(rest)...);
		}
	}

	template<typename OutputType, typename ParseFunc, typename Delim, typename EndCond>
	std::expected<std::vector<OutputType>, CompilerError> ParseDelimitedZeroOrMoreLimited(ParseFunc&& func, Delim&& delim, EndCond&& end_cond, std::vector<OutputType>&& acc = {})
	{
		return TryParser<OutputType>(std::forward<ParseFunc>(func))
			.and_then
			(
				[&func, &end_cond, &delim, &acc, this](OutputType&& elem)
				{
					acc.emplace_back(std::move(elem));
					return delim()
						.and_then
						(
							[&func, &end_cond, &delim, &acc, this](Token&&) -> std::expected<std::vector<OutputType>, CompilerError>
							{
								return ParseDelimitedZeroOrMoreLimited(std::forward<ParseFunc>(func), std::forward<Delim>(delim), std::forward<EndCond>(end_cond), std::move(acc));
							}
						)
						.or_else(
							[&end_cond, &acc](CompilerError&& delim_error) -> std::expected<std::vector<OutputType>, CompilerError>
							{
								return end_cond()
									.and_then
									(
										[&acc](Token&&) -> std::expected<std::vector<OutputType>, CompilerError>
										{
											return std::move(acc);
										}
									)
									.or_else
									(
										[&delim_error](CompilerError&&) -> std::expected<std::vector<OutputType>, CompilerError>
										{
											return std::unexpected(std::move(delim_error));
										}
									);
							}
						);
				}
			)
			.or_else
			(
				[&acc, &end_cond](CompilerError&& try_parser_error)
				{
					return end_cond()
						.and_then
						(
							[&acc](Token&&) -> std::expected<std::vector<OutputType>, CompilerError>
							{
								return std::move(acc);
							}
						)
						.or_else
						(
							[&try_parser_error](CompilerError&&) -> std::expected<std::vector<OutputType>, CompilerError>
							{
								return std::unexpected(std::move(try_parser_error));
							}
						);
				}
			);
	}

	template<typename OutputType, typename ParseFunc, typename Delim>
	std::expected<std::vector<OutputType>, CompilerError> ParseDelimitedZeroOrMoreUnlimited(ParseFunc&& func, Delim&& delim, std::vector<OutputType>&& acc = {})
	{
		return TryParser<OutputType>(std::forward<ParseFunc>(func))
			.and_then
			(
				[&func, &delim, &acc, this](OutputType&& elem)
				{
					acc.emplace_back(std::move(elem));
					return delim()
						.and_then
						(
							[&func, &delim, &acc, this](Token&&) -> std::expected<std::vector<OutputType>, CompilerError>
							{
								return ParseDelimitedZeroOrMoreUnlimited(std::forward<ParseFunc>(func), std::forward<Delim>(delim), std::move(acc));
							}
						)
						.or_else
						(
							[&acc](CompilerError&&) -> std::expected<std::vector<OutputType>, CompilerError>
							{
								return std::move(acc);
							}
						);
				}
			)
			.or_else
			(
				[&acc](CompilerError&&) -> std::expected<std::vector<OutputType>, CompilerError>
				{
					return std::move(acc);
				}
			);
	}

	template<typename OutputType, typename ParseFunc, typename EndCond>
	std::expected<std::vector<OutputType>, CompilerError> ParseZeroOrMoreLimited(ParseFunc&& func, EndCond&& end_cond, std::vector<OutputType>&& acc = {})
	{
		return TryParser<OutputType>(std::forward<ParseFunc>(func))
			.and_then
			(
				[&acc, &func, &end_cond, this](OutputType&& elem)
				{
					acc.emplace_back(std::move(elem));
					return ParseZeroOrMoreLimited(std::forward<ParseFunc>(func), std::forward<EndCond>(end_cond), std::move(acc))
						.or_else
						(
							[&end_cond, &acc](CompilerError&& error) -> std::expected<std::vector<OutputType>, CompilerError>
							{
								return end_cond()
									.and_then
									(
										[&acc](Token&&) -> std::expected<std::vector<OutputType>, CompilerError>
										{
											return std::move(acc);
										}
									)
									.or_else
									(
										[&error](CompilerError&&) ->std::expected<std::vector<OutputType>, CompilerError>
										{
											return std::unexpected(std::move(error));
										}
									);
							}
						);
				}
			)
			.or_else
			(
				[&acc, &end_cond](CompilerError&& try_parser_error)
				{
					return end_cond()
						.and_then
						(
							[&acc](Token&&) -> std::expected<std::vector<OutputType>, CompilerError>
							{
								return std::move(acc);
							}
						)
						.or_else
						(
							[&try_parser_error](CompilerError&&) -> std::expected<std::vector<OutputType>, CompilerError>
							{
								return std::unexpected(std::move(try_parser_error));
							}
						);
				}
			);
	}

	template<typename OutputType, typename ParseFunc>
	std::expected<std::vector<OutputType>, CompilerError> ParseZeroOrMoreUnlimited(ParseFunc&& func, std::vector<OutputType>&& acc = {})
	{
		return TryParser<OutputType>(std::forward<ParseFunc>(func))
			.and_then
			(
				[&func, &acc, this](OutputType&& elem) mutable
				{
					acc.emplace_back(std::move(elem));
					return ParseZeroOrMoreUnlimited<OutputType>(std::forward<ParseFunc>(func), std::move(acc));
				}
			)
			.or_else
			(
				[&acc, this](CompilerError&& try_parser_error) mutable -> std::expected<std::vector<OutputType>, CompilerError>
				{
					if (IsNoMatchError(try_parser_error))
					{
						return std::move(acc);
					}

					return std::unexpected(std::move(try_parser_error));
				}
			);
	}

	bool IsGlobalName(const std::vector<Scope>::const_reverse_iterator& found_scope_it) const;

	bool IsLocalName(const Scope::VariableTable::const_iterator& found_scope_it) const;
	
	Parser& Synchronize() &;

	Parser&& Synchronize() &&;

	bool IsNoMatchError(const CompilerError& error) const;

	CompilerError GenerateParserError(std::string&& message, const Token& token);

	CompilerError GenerateParserError(CompilerErrorCode code, std::string&& message, const Token& token);

	bool IsAtEnd(ParseState& state);

	bool Check(ParseState& state, Token::Name type, int offset);

	Token& Peek(ParseState& state, int offset);

	Token& Previous(ParseState& state);

	Token& Advance(ParseState& state);

	bool IsAtEnd();

	bool Check(Token::Name type, int offset);

	bool IsAtGlobalScope() const;

	std::vector<Scope>::const_reverse_iterator FindVariableScope(std::string& name);

	std::vector<Scope>::const_reverse_iterator FindTypeScope(std::string& name);

	bool CanAccessSymbol(const std::string& symbol_name) const;

	bool IsInUseImports(const std::string& symbol_name, std::string& out_module_name) const;

	UseImportResolution ResolveUseImport(const std::string& symbol_name) const;

	std::string BuildAmbiguousUseImportError(const std::string& symbol_name, const std::vector<std::string>& module_names) const;

	static std::string BuildTypeArgumentCountMismatchMessage(const std::string& type_name, bool is_alias, size_t expected_count, size_t actual_count);

	ImportedSymbolAccess ResolveImportedSymbolAccess(const std::string& module_name, const std::string& symbol_name) const;

	// True when this file imports the module, or is the module: the rule that makes
	// compiler-provided names such as Concurrency::Spawn and Cell::New available.
	bool IsModuleVisible(std::string_view module_name) const;

	// Parses the '<T>' after the built-in type name Cell.
	MidoriResult::TypeResult ParseCellTypeArguments();

	std::string BuildImportedSymbolAccessError(const std::string& module_name, const std::string& symbol_name, ImportedSymbolAccess access) const;

	bool ResolveQualifiedSymbol(const std::string& module_name, const std::string& symbol_name) const;

	bool SharesNamespace(const std::string& namespace1, const std::string& namespace2) const;

	std::string ExtractSymbolName(const std::string& qualified_name) const;

	std::string ExtractQualifier(const std::string& qualified_name) const;

	MidoriResult::ExpressionResult ResolveQualifiedName(const Token& name_token, const std::string& mangled_name);

	ConstructorResolutionResult ResolveConstructorName(const Token& name_token, const std::string& mangled_name);

	Token& Peek(int offset);

	Token& Previous();

	Token& Advance();

	MidoriResult::TokenResult Consume(Token::Name type, std::string_view message);

	MidoriResult::TokenResult ConsumeTypeRightAngle(std::string_view message);

	CompilerError GenerateRemovedReturnTypeColonError();

	MidoriResult::TokenResult ConsumeReturnTypeSeparator(std::string_view message);

	MidoriResult::TokenResult MatchNameResolution();

	MidoriResult::TokenListResult ParseGenericParameters(std::vector<std::shared_ptr<MidoriType>>* out_types = nullptr);

	MidoriResult::FunctionParamsResult ParseFunctionParameters(bool allow_inferred_types = false);

	Parser& BeginScope() &;

	Parser&& BeginScope() &&;

	int EndScope();

	std::string Mangle(std::string_view name);

	MidoriResult::TokenResult DefineName(Token& name, bool is_variable);

	std::optional<int> RegisterOrUpdateLocalVariable(const std::string& name);

	std::optional<int> RegisterHiddenLocal(const std::string&);

	ArrayComprehensionProbe ProbeArrayComprehension();

	MidoriResult::TypeResult ParseType(bool is_foreign = false);

	MidoriResult::ExpressionResult ParseExpression();

	MidoriResult::ExpressionResult ParseFactor();

	MidoriResult::ExpressionResult ParseShift();

	MidoriResult::ExpressionResult ParseRange();

	MidoriResult::ExpressionResult ParseTerm();

	MidoriResult::ExpressionResult ParseComparison();

	MidoriResult::ExpressionResult ParseEquality();

	MidoriResult::ExpressionResult ParseBitwiseAnd();

	MidoriResult::ExpressionResult ParseBitwiseXor();

	MidoriResult::ExpressionResult ParseBitwiseOr();

	MidoriResult::ExpressionResult ParseBind();

	MidoriResult::ExpressionResult ParseUnaryLogicalBitwise();

	MidoriResult::ExpressionResult ParseUnaryArithmetic();

	MidoriResult::ExpressionResult ParseArrayAccessHelper(std::unique_ptr<MidoriExpression>&& arr_var);

	MidoriResult::ExpressionResult ParseArrayAccess();

	MidoriResult::ExpressionResult ParseCall();

	MidoriResult::ExpressionResult ParseAs();

	MidoriResult::ExpressionResult ParseConstruct();

	MidoriResult::ExpressionResult FinishCall(std::unique_ptr<MidoriExpression>&& callee);

	MidoriResult::ExpressionResult LowerConcurrencyIntrinsic(std::unique_ptr<MidoriExpression>&& expr);

	// Operators of different precedence bind by a table the reader has to remember,
	// so mixing them in one expression needs parentheses instead. Returns the class
	// an operator belongs to, or nullopt for one the rule does not cover.
	static std::optional<int> BinaryOperatorClass(Token::Name name);

	std::optional<CompilerError> CheckOperatorMixing(const Token& op, const MidoriExpression& operand);

	// A parsed construction never carries explicit type arguments: `new Name<T>(...)` was the
	// only form that could state them and it is gone. The AST node keeps the flag, because the
	// deriving synthesiser builds Construct nodes directly with an already-resolved return type.
	MidoriResult::ExpressionResult FinishConstruct(Token&& constructor_token, std::shared_ptr<MidoriType>&& constructed_type, bool is_struct);

	MidoriResult::ExpressionResult ParsePrimary();

	MidoriResult::ExpressionResult ParseLogicalAnd();

	MidoriResult::ExpressionResult ParseLogicalOr();

	MidoriResult::ExpressionResult ParseSend();

	MidoriResult::ExpressionResult ParsePipe();

	// The type declared under `name` in this module or any module it imports, or
	// nullptr. For compiler-required declarations such as `Result`.
	std::shared_ptr<MidoriType> FindDeclaredTypeByName(const std::string& name);

	MidoriResult::ExpressionResult ParseBlockExpression();

	bool ProbeRecordUpdate();

	MidoriResult::ExpressionResult ParseRecordUpdate();

	MidoriResult::ExpressionResult ParsePostfixChain(std::unique_ptr<MidoriExpression>&& expr);


	MidoriResult::ExpressionResult ParseForExpression();

	MidoriResult::ExpressionResult ParseArrayComprehension(Token& bracket, const ArrayComprehensionProbe& probe);



	MidoriResult::ExpressionResult ParseMatchExpressionWithScrutinee(Token& match_keyword, std::unique_ptr<MidoriExpression>&& expr);

	MidoriResult::ExpressionResult ParseMatchExpression();

	MidoriResult::ExpressionResult ParseIfElseExpression();

	MidoriResult::ExpressionResult ParseFunctionExpression();

	MidoriResult::ExpressionResult ParseCaseExpression(std::unordered_set<std::string>& visited_members, Token& keyword);

	MidoriResult::PatternResult ParsePattern();


	MidoriResult::StatementResult ParseDeclaration();

	MidoriResult::StatementResult ParseDefineStatement();

	std::expected<TypeDeclarationHeader, CompilerError> ParseTypeDeclarationHeader(std::string_view noun, std::string_view capitalized_noun);

	MidoriResult::StatementResult ParseStructBody(TypeDeclarationHeader&& header);

	MidoriResult::StatementResult ParseUnionBody(TypeDeclarationHeader&& header);

	bool TypeBodyHasTopLevelBar();

	MidoriResult::StatementResult ParseNewTypeBody(TypeDeclarationHeader&& header);

	MidoriResult::StatementResult ParseClassDeclaration();

	MidoriResult::StatementResult ParseInstanceDeclaration();

	MidoriResult::StatementResult ParseTypeDeclaration();

	MidoriResult::StatementResult ParseAliasDeclaration();


	MidoriResult::StatementResult ParseSimpleStatement();

	MidoriResult::StatementResult ParseForeignStatement();

	std::expected<std::vector<MidoriType::ClassConstraint>, CompilerError> ParseClassConstraints(const Token& context_token);

	void PushActiveConstraints(const std::vector<MidoriType::ClassConstraint>& constraints);

	std::expected<std::vector<Token>, CompilerError> ParseDerivingTargets(const Token& context_token);

	Token MakeSyntheticToken(std::string lexeme, Token::Name token_name, const Token& anchor) const;

	std::string AppendSuffixToQualifiedName(std::string_view qualified_name, std::string_view suffix) const;

	MidoriResult::TokenResult RegisterSyntheticGlobalName(const std::string& name, const Token& anchor);

	void RegisterSyntheticInstanceMetadata(const std::string& class_name, const std::vector<std::shared_ptr<MidoriType>>& type_args, const std::vector<std::string>& mangled_method_names);

	std::expected<void, CompilerError> QueueDerivedStructStatements(const MidoriStatement::Struct& struct_stmt, const std::vector<Token>& deriving_targets);

	std::expected<void, CompilerError> QueueDerivedUnionStatements(const MidoriStatement::Union& union_stmt, const std::vector<Token>& deriving_targets);

	MidoriResult::StatementResult ParseStatement();
};
