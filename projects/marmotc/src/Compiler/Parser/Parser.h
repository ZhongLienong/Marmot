#pragma once

#include <expected>
#include <optional>
#include <queue>
#include <string_view>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "Compiler/Error/CompilerError.h"
#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"
#include "Compiler/Module/ModuleImports.h"
#include "Compiler/Module/Module.h"
#include "Compiler/Result/Result.h"

class Parser
{
private:
	struct Scope
	{
		using VariableTable = std::unordered_map<std::string, BindingId>;
		using StructConstructorTable = std::unordered_map<std::string, std::shared_ptr<MarmotType>>;
		using UnionConstructorTable = std::unordered_map<std::string, std::shared_ptr<MarmotType>>;
		using DefinedTypeTable = std::unordered_map<std::string, std::shared_ptr<MarmotType>>;
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
		std::vector<std::shared_ptr<MarmotType>> m_generic_param_types;
		std::vector<MarmotType::ClassConstraint> m_constraints;
		// The type CollectTopLevelNames registered for this declaration, which
		// earlier declarations and signatures may already name.
		std::shared_ptr<MarmotType> m_predeclared_type;
		size_t m_scope_index;
		bool m_has_generic_params = false;

		TypeDeclarationHeader(Token&& name, std::string&& name_before_mangle, size_t scope_index)
			: m_name(std::move(name)), m_name_before_mangle(std::move(name_before_mangle)), m_scope_index(scope_index)
		{
		}
	};

	struct ParsedTypeDeclaration
	{
		std::unique_ptr<MarmotStatement> m_statement;
		std::vector<Token> m_deriving_targets;

		explicit ParsedTypeDeclaration(std::unique_ptr<MarmotStatement> statement, std::vector<Token> deriving_targets = {});
	};

	using ParsedTypeResult = std::expected<ParsedTypeDeclaration, CompilerError>;
	using TypeBodyParser = std::function<ParsedTypeResult(TypeDeclarationHeader&&)>;

	// The result of matching a name against the struct and union constructor tables.
	// A construction and an ordinary call are told apart by this lookup rather than by a
	// leading keyword, so the lookup lives on its own.
	struct ConstructorResolution
	{
		std::shared_ptr<MarmotType> m_type;
		std::string m_constructor_name;
		bool m_is_struct;

		ConstructorResolution(std::shared_ptr<MarmotType>&& type, std::string&& constructor_name, bool is_struct)
			: m_type(std::move(type)), m_constructor_name(std::move(constructor_name)), m_is_struct(is_struct)
		{
		}
	};

	using ConstructorResolutionResult = MarmotResult::Result<std::optional<ConstructorResolution>>;

	using Scopes = std::vector<Scope>;

	using TypeclassMethodMap = std::unordered_map<std::string, std::unordered_set<std::string>>;
	using TypeclassInstanceMap = std::unordered_map<std::string, std::vector<std::string>>;
	using TypeclassAssociatedTypeMap = std::unordered_map<std::string, std::vector<std::string>>;
	using TypeEnvironment = std::unordered_map<std::string, std::shared_ptr<MarmotType>>;
	using TypeclassMethodTypeMap = std::unordered_map<std::string, std::unordered_map<std::string, std::shared_ptr<MarmotType>>>;

	struct ParseContext
	{
		const ModuleImports& m_imports;
		TokenStream m_tokens;
		std::string m_file_name;
		const ModuleDeclaration* m_current_module = nullptr;
		const std::vector<std::string>* m_source_lines = nullptr;

		ParseContext(TokenStream&& tokens, std::string_view file_name, const std::vector<std::string>& source_lines, const ModuleImports& imports, const ModuleDeclaration* module_decl);
	};

	// A `def` whose initializer is being parsed. Until it finishes, the name has
	// no value, so a read at the same function depth would read nothing; a read
	// deeper than that is inside a function body, which runs later, and is how a
	// definition calls itself.
	struct PendingDefinition
	{
		std::string m_name;
		LexicalFunctionId m_function;

		PendingDefinition(std::string name, LexicalFunctionId function);
	};

	// A top-level definition: which statement makes it, and whether it is a
	// function, whose body runs only when called.
	struct TopLevelDefinition
	{
		int m_statement = 0;
		int m_line = 0;
		bool m_is_function = false;

		TopLevelDefinition(int statement, int line, bool is_function);
	};

	// A read of one of this module's top-level names, and the statement it sits
	// in. Checked once the whole module is parsed, so a definition may name one
	// that comes after it.
	struct TopLevelReference
	{
		int m_statement = 0;
		Token m_name;

		TopLevelReference(int statement, const Token& name);
	};

	struct ParseState
	{
		TypeclassMethodMap m_class_methods;
		TypeclassInstanceMap m_typeclass_type_params;
		TypeclassAssociatedTypeMap m_typeclass_associated_types;
		std::unordered_map<std::string, std::vector<TypeclassInstance>> m_class_instances;
		TypeclassMethodTypeMap m_typeclass_method_types;
		Scopes m_scopes{ Scope() };
		LexicalFunctionId m_function;
		std::vector<UseImport> m_current_use_imports;
		std::vector<std::string> m_namespaces;
		std::vector<MarmotType::ClassConstraint> m_active_constraints;
		// The types whose bodies are being parsed, innermost last.
		std::vector<std::shared_ptr<MarmotType>> m_types_being_declared;
		std::vector<PendingDefinition> m_pending_definitions;
		// Set by `|>` when its stage is a lambda, whose body then ends where a pipe
		// operand does, so the next `|>` is the pipeline's next stage.
		bool m_next_function_is_pipe_stage = false;
		std::unordered_set<std::string> m_top_level_names;
		std::unordered_map<std::string, TopLevelDefinition> m_top_level_definitions;
		std::vector<TopLevelReference> m_top_level_references;
		int m_statement_index = 0;
		int m_current_token_index = 0;
		int m_next_binding_index = 0;
		bool m_allow_implicit_generic_params = false;

		ParseState(const std::vector<UseImport>& use_imports);
	};

	// Restore lexical context without rewinding tokens or undoing declarations.
	class ParseLifetime
	{
	private:
		ParseState& m_state;
		size_t m_scope_count;
		size_t m_namespace_count;
		size_t m_constraint_count;
		size_t m_declaring_type_count;
		size_t m_pending_definition_count;
		LexicalFunctionId m_function;
		bool m_allow_implicit_generic_params;

	public:
		explicit ParseLifetime(ParseState& state);
		~ParseLifetime();
		ParseLifetime(const ParseLifetime&) = delete;
		ParseLifetime& operator=(const ParseLifetime&) = delete;

	};

	struct ArrayComprehensionProbe
	{
		bool m_is_candidate = false;
		std::optional<int> m_loop_variable_offset = std::nullopt;
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
	std::queue<std::unique_ptr<MarmotStatement>> m_pending_statements;
	// By name: a module's top-level type names are unique.
	std::unordered_map<std::string, std::shared_ptr<MarmotType>> m_predeclared_types;
	// Registered, and not declared yet.
	std::unordered_set<const MarmotType*> m_undeclared_types;

	// An instantiation of a type that was not whole when it was named: filled
	// in place once every declaration is parsed.
	struct DeferredInstantiation
	{
		std::shared_ptr<MarmotType> m_target;
		std::shared_ptr<MarmotType> m_template;
		std::unordered_map<std::string, std::shared_ptr<MarmotType>> m_substitutions;
		Token m_reference;

		DeferredInstantiation(std::shared_ptr<MarmotType> target, std::shared_ptr<MarmotType> type_template, std::unordered_map<std::string, std::shared_ptr<MarmotType>> substitutions, Token reference);
	};
	std::vector<DeferredInstantiation> m_deferred_instantiations;
	std::unordered_set<const MarmotType*> m_unfilled_instantiations;

	friend struct ParserTestAccess;

public:
	Parser(TokenStream&& tokens, std::string_view file_name, const std::vector<std::string>& source_lines, const ModuleImports& imports, const std::vector<UseImport>& use_imports, const ModuleDeclaration* module_decl);

	MarmotResult::ParserResult Parse();

	const std::vector<CompilerWarning>& GetWarnings() const;

	TypeclassMetadataMap GetTypeclassMetadata() const;

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
	
	template<typename Value, typename Condition, typename Step>
	static std::expected<Value, CompilerError> FoldWhile(std::expected<Value, CompilerError> value, Condition&& condition, Step&& step)
	{
		while (value.has_value() && condition())
		{
			value = step(std::move(value.value()));
		}
		return value;
	}

	template <typename... T>
		requires (std::is_same_v<T, Token::Name> && ...)
	MarmotResult::ExpressionResult ParseBinary(MarmotResult::ExpressionResult(Parser::* operand)(), T... tokens)
	{
		return FoldWhile
		(
			(this->*operand)(),
			[this, ...tokens = tokens]() { return Match(tokens...); },
			[this, operand](std::unique_ptr<MarmotExpression>&& left) -> MarmotResult::ExpressionResult
			{
				const Token op = Previous();
				return (this->*operand)().and_then
				(
					[&](std::unique_ptr<MarmotExpression>&& right) -> MarmotResult::ExpressionResult
					{
						return std::make_unique<MarmotExpression>(MarmotExpression::Binary(op, std::move(left), std::move(right)));
					}
				);
			}
		);
	}

	template<typename Parse>
	std::invoke_result_t<Parse> InContext(Parse&& parse)
	{
		const ParseLifetime lifetime(m_state);
		return std::forward<Parse>(parse)();
	}

	template<typename Parse>
	std::invoke_result_t<Parse> InScope(Parse&& parse)
	{
		return InContext
		(
			[&]()
			{
				m_state.m_scopes.emplace_back();
				return std::forward<Parse>(parse)();
			}
		);
	}

	template<typename Parse>
	std::invoke_result_t<Parse, std::vector<Token>, std::vector<std::shared_ptr<MarmotType>>> WithGenericParameters(Parse&& parse)
	{
		using Result = std::invoke_result_t<Parse, std::vector<Token>, std::vector<std::shared_ptr<MarmotType>>>;
		const auto parse_with_parameters = [&]() -> Result
		{
			std::vector<Token> names;
			std::vector<std::shared_ptr<MarmotType>> types;
			if (Match(Token::Name::LEFT_ANGLE))
			{
				MarmotResult::TokenListResult parameters = ParseGenericParameters(&types);
				if (!parameters.has_value())
				{
					return std::unexpected(std::move(parameters.error()));
				}
				names = std::move(parameters.value());
			}
			return std::forward<Parse>(parse)(std::move(names), std::move(types));
		};

		return Check(Token::Name::LEFT_ANGLE, 0) ? InScope(parse_with_parameters) : parse_with_parameters();
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

	// Every caller needs at least one element, and an element that fails after a
	// delimiter is an error in that element: swallowing it left the list's caller
	// to report a missing terminator at the element's first token.
	template<typename OutputType, typename ParseFunc, typename Delim>
	std::expected<std::vector<OutputType>, CompilerError> ParseDelimitedOneOrMoreUnlimited(ParseFunc&& func, Delim&& delim, std::vector<OutputType>&& acc = {})
	{
		std::expected<OutputType, CompilerError> elem = func();
		if (!elem.has_value())
		{
			return std::unexpected(std::move(elem.error()));
		}

		acc.emplace_back(std::move(elem.value()));
		if (!delim().has_value())
		{
			return std::move(acc);
		}

		return ParseDelimitedOneOrMoreUnlimited(std::forward<ParseFunc>(func), std::forward<Delim>(delim), std::move(acc));
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

	FunctionParameters ParametersFor(std::vector<Token> names) const;
	
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

	UseImportResolution ResolveUseImport(const std::string& symbol_name) const;

	// The imported modules that export `symbol_name` and would let this module
	// see it, in name order.
	std::vector<std::string> ModulesExporting(const std::string& symbol_name) const;

	std::string BuildUnimportedSymbolError(const std::string& symbol_name, const std::vector<std::string>& module_names) const;

	std::string DescribeUnknownConstructor(const std::string& qualified_name);

	std::string BuildAmbiguousUseImportError(const std::string& symbol_name, const std::vector<std::string>& module_names) const;

	static std::string BuildTypeArgumentCountMismatchMessage(const std::string& type_name, bool is_alias, size_t expected_count, size_t actual_count);

	std::string_view DeclaredModuleName() const;
	
	ImportedSymbolAccess ResolveImportedSymbolAccess(const std::string& module_name, const std::string& symbol_name) const;

	// True when this file imports the module, or is the module.
	bool IsModuleVisible(std::string_view module_name) const;

	// Parses the '<T>' after the built-in type name Cell.
	MarmotResult::TypeResult ParseRefTypeArguments();

	std::string BuildImportedSymbolAccessError(const std::string& module_name, const std::string& symbol_name, ImportedSymbolAccess access) const;

	std::string CurrentModuleName() const;

	bool IsBeingDefined(const std::string& name) const;

	void CollectTopLevelNames();

	void CollectForeignNames(TokenStream::const_iterator foreign_keyword);

	void PredeclareType(int type_keyword_index);

	void PredeclareVariants(const std::shared_ptr<MarmotType>& union_type, int body_index);

	std::shared_ptr<MarmotType> DeclaredTypeFor(const TypeDeclarationHeader& header, std::shared_ptr<MarmotType>&& fresh_type);

	void CompleteType(const std::shared_ptr<MarmotType>& type);

	static bool RepresentationNames(const std::shared_ptr<MarmotType>& type, const MarmotType* newtype, std::unordered_set<const MarmotType*>& visited);

	static bool Reaches(const std::shared_ptr<MarmotType>& type, const std::function<bool(const MarmotType*)>& is_sought, std::unordered_set<const MarmotType*>& visited);

	bool ReachesUnfinishedType(const std::shared_ptr<MarmotType>& type) const;

	std::optional<CompilerError> FillDeferredInstantiations();

	MarmotResult::TypeResult InstantiateUnfinishedType(const Token& type_name, const std::shared_ptr<MarmotType>& type_template, const std::vector<std::string>& generic_params, std::vector<std::shared_ptr<MarmotType>>&& type_args);

	void RecordTopLevelDefinition(const MarmotStatement& statement, int statement_index);

	void RecordTopLevelReference(const Token& name);

	std::optional<CompilerError> CheckDefinitionOrder();

	std::string ExtractSymbolName(const std::string& qualified_name) const;

	std::string ExtractQualifier(const std::string& qualified_name) const;

	MarmotResult::ExpressionResult ResolveQualifiedName(const Token& name_token, const std::string& mangled_name);

	ConstructorResolutionResult ResolveConstructorName(const Token& name_token, const std::string& mangled_name);

	Token& Peek(int offset);

	Token& Previous();

	Token& Advance();

	MarmotResult::TokenResult Consume(Token::Name type, std::string_view message);

	MarmotResult::TokenResult ConsumeTypeRightAngle(std::string_view message);

	MarmotResult::TokenResult ConsumeReturnTypeSeparator(std::string_view message);

	MarmotResult::TokenResult MatchNameResolution();

	MarmotResult::TokenListResult ParseGenericParameters(std::vector<std::shared_ptr<MarmotType>>* out_types = nullptr);

	MarmotResult::FunctionParamsResult ParseFunctionParameters(bool allow_inferred_types = false);

	std::string Mangle(std::string_view name);

	MarmotResult::TokenResult DefineName(Token& name, bool is_variable);

	std::optional<BindingId> RegisterLocalBinding(const std::string& name);

	ArrayComprehensionProbe ProbeArrayComprehension();

	MarmotResult::TypeResult ParseType(bool is_foreign = false);

	using MakeTypeOfOne = std::shared_ptr<MarmotType> (*)(const std::shared_ptr<MarmotType>&);

	// `Keyword<T>`: a type made from one other, such as `Array<Int>`.
	MarmotResult::TypeResult ParseTypeOfOne(Token::Name keyword, std::string_view argument_noun, MakeTypeOfOne make);

	MarmotResult::ExpressionResult ParseExpression();

	MarmotResult::ExpressionResult ParseFactor();

	MarmotResult::ExpressionResult ParseShift();

	MarmotResult::ExpressionResult ParseRange();

	MarmotResult::ExpressionResult ParseTerm();

	MarmotResult::ExpressionResult ParseComparison();

	MarmotResult::ExpressionResult ParseEquality();

	MarmotResult::ExpressionResult ParseBitwiseAnd();

	MarmotResult::ExpressionResult ParseBitwiseXor();

	MarmotResult::ExpressionResult ParseBitwiseOr();

	MarmotResult::ExpressionResult ParseUnaryLogicalBitwise();

	MarmotResult::ExpressionResult ParseUnaryArithmetic();

	MarmotResult::ExpressionResult ParseArrayAccessHelper(std::unique_ptr<MarmotExpression>&& arr_var);

	MarmotResult::ExpressionResult ParseCall();

	MarmotResult::ExpressionResult ParseAs();

	MarmotResult::ExpressionResult FinishCall(std::unique_ptr<MarmotExpression>&& callee);

	// A parsed construction never carries explicit type arguments: `new Name<T>(...)` was the
	// only form that could state them and it is gone. The AST node keeps the flag, because the
	// deriving synthesiser builds Construct nodes directly with an already-resolved return type.
	MarmotResult::ExpressionResult FinishConstruct(Token&& constructor_token, std::shared_ptr<MarmotType>&& constructed_type, bool is_struct);

	MarmotResult::ExpressionResult FinishNamedConstruct(Token&& constructor_token, std::shared_ptr<MarmotType>&& constructed_type);

	MarmotResult::ExpressionResult ParsePrimary();

	// `1e300` lexes as the number 1 and the name e300; say that a number has no exponent
	// form rather than asking for the comma the parser would otherwise want.
	MarmotResult::ExpressionResult RejectExponent(std::unique_ptr<MarmotExpression>&& literal);

	MarmotResult::ExpressionResult ParseLogicalAnd();

	MarmotResult::ExpressionResult ParseLogicalOr();

	MarmotResult::ExpressionResult ParseSend();

	MarmotResult::ExpressionResult ParsePipe();

	// The type declared under `name` in this module or any module it imports, or
	// nullptr. For compiler-required declarations such as `Result`.

	MarmotResult::ExpressionResult ParseBlockExpression();

	bool ProbeRecordUpdate();

	MarmotResult::ExpressionResult ParseRecordUpdate();

	MarmotResult::ExpressionResult ParsePostfixChain(std::unique_ptr<MarmotExpression>&& expr);

	MarmotResult::ExpressionResult ParseForExpression();

	MarmotResult::ExpressionResult ParseArrayComprehension(Token& bracket, const ArrayComprehensionProbe& probe);

	MarmotResult::ExpressionResult ParseMatchExpressionWithScrutinee(Token& match_keyword, std::unique_ptr<MarmotExpression>&& expr);

	MarmotResult::ExpressionResult ParseMatchExpression();

	MarmotResult::ExpressionResult ParseIfElseExpression();

	MarmotResult::ExpressionResult ParseFunctionExpression();

	MarmotResult::ExpressionResult ParseCaseExpression(Token& keyword);

	MarmotResult::PatternResult ParsePattern();

	MarmotResult::PatternResult ParseAlternatives();

	MarmotResult::PatternResult ParsePrimaryPattern();

	MarmotResult::PatternResult ParseBindingPattern(Token&& name);

	MarmotResult::PatternResult ParseRecordPattern(Token&& name);

	MarmotResult::PatternResult ParseNumericLiteralPattern();

	MarmotResult::PatternResult ParseRangePattern(std::unique_ptr<MarmotPattern>&& start);

	MarmotResult::PatternResult ParseArrayRest(Token&& dots);

	MarmotResult::StatementResult ParseDeclaration();

	MarmotResult::StatementResult ParseDefineStatement();

	ParsedTypeResult WithTypeDeclarationHeader(std::string_view noun, std::string_view capitalized_noun, const TypeBodyParser& parse_body);

	MarmotResult::StatementResult FinishTypeDeclaration(ParsedTypeDeclaration declaration);

	std::expected<MarmotStatement::FunctionDefinition, CompilerError> ParseInstanceMethodBody(const Token& method_name);

	ParsedTypeResult ParseStructBody(TypeDeclarationHeader&& header);

	ParsedTypeResult ParseUnionBody(TypeDeclarationHeader&& header);

	bool TypeBodyHasTopLevelBar();

	ParsedTypeResult ParseNewTypeBody(TypeDeclarationHeader&& header);

	MarmotResult::StatementResult ParseClassDeclaration();

	MarmotResult::StatementResult ParseInstanceDeclaration();

	MarmotResult::StatementResult ParseTypeDeclaration();

	MarmotResult::StatementResult ParseAliasDeclaration();

	ParsedTypeResult ParseAliasBody(TypeDeclarationHeader&& header);

	MarmotResult::StatementResult ParseSimpleStatement();

	MarmotResult::StatementResult ParseForeignStatement();

	MarmotResult::StatementResult ParseForeignBlock(const Token& library);

	MarmotResult::StatementResult ParseForeignDeclaration(const Token& foreign_name, const std::optional<std::string>& block_library);

	std::expected<std::vector<MarmotType::ClassConstraint>, CompilerError> ParseClassConstraints(const Token& context_token);

	void PushActiveConstraints(const std::vector<MarmotType::ClassConstraint>& constraints);

	std::expected<std::vector<Token>, CompilerError> ParseDerivingTargets(const Token& context_token);

	void RegisterSyntheticInstanceMetadata(const std::string& class_name, const std::vector<std::shared_ptr<MarmotType>>& type_args, const std::vector<MarmotType::ClassConstraint>& constraints, const std::vector<std::string>& mangled_method_names);

	template<typename Declaration>
	std::expected<void, CompilerError> QueueDerivedStatements(const Declaration& declaration, const std::vector<Token>& deriving_targets);

	MarmotResult::StatementResult ParseStatement();
};
