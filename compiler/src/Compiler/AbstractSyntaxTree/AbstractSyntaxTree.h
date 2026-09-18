#pragma once

#include <cstdint>
#include <optional>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "Compiler/Token/Token.h"
#include "Type.h"

class MidoriExpression;

enum class MidoriLiteralKind
{
	Bool,
	Integer,
	Byte,
	Word,
	Float,
	Text,
	Unit
};

// Value of an Integer, Byte or Word literal lexeme: decimal, `0x` hexadecimal or `0b`
// binary. Nothing when the lexeme is malformed or does not fit in 64 unsigned bits.
std::optional<uint64_t> ParseUnsignedLiteral(std::string_view lexeme);

// Value of an Integer literal lexeme. Also accepts a leading '-', which the optimizer
// writes when it folds a negative constant back into a literal. Nothing when the value
// does not fit in a signed 64-bit integer.
std::optional<int64_t> ParseIntegerLiteral(std::string_view lexeme);

class MidoriStatement
{
public:

	struct ExpressionStatement
	{
		Token m_semicolon;
		std::unique_ptr<MidoriExpression> m_expr;

		ExpressionStatement(const Token& semicolon, std::unique_ptr<MidoriExpression>&& expr);
	};

	struct VariableDefinition
	{
		Token m_name;
		std::unique_ptr<MidoriExpression> m_value;
		std::optional<std::shared_ptr<MidoriType>> m_annotated_type;
		std::optional<int> m_local_index;
		bool m_is_elided = false;

		VariableDefinition(const Token& name, std::unique_ptr<MidoriExpression>&& value, std::optional<std::shared_ptr<MidoriType>>&& annotated_type, std::optional<int>&& local_index);
	};

	struct TupleDefinition
	{
		std::vector<Token> m_names;
		std::unique_ptr<MidoriExpression> m_value;
		std::vector<std::optional<int>> m_local_indices;

		TupleDefinition(std::vector<Token>&& names, std::unique_ptr<MidoriExpression>&& value, std::vector<std::optional<int>>&& local_indices);
	};

	struct FunctionDefinition
	{
		Token m_name;
		std::vector<Token> m_generic_params;
		std::vector<Token> m_params;
		std::vector<std::shared_ptr<MidoriType>> m_param_types;
		std::vector<MidoriType::ClassConstraint> m_constraints;
		std::shared_ptr<MidoriType> m_return_type;
		std::unique_ptr<MidoriExpression> m_body;
		std::optional<int> m_local_index;
		int m_captured_count;
		bool m_is_lift_wrapper = false;

		FunctionDefinition(const Token& name, std::vector<Token>&& generic_params, std::vector<Token>&& params, std::vector<std::shared_ptr<MidoriType>>&& param_types, std::shared_ptr<MidoriType>&& return_type, std::unique_ptr<MidoriExpression>&& body, std::optional<int>&& local_index, int captured_count = 0, std::vector<MidoriType::ClassConstraint>&& constraints = {});
	};

	struct ForeignDefinition
	{
		Token m_function_name;
		std::string m_foreign_name;
		std::shared_ptr<MidoriType> m_type;
		std::optional<int> m_local_index;

		ForeignDefinition(const Token& function_name, const std::string& foreign_name, std::shared_ptr<MidoriType>&& type, std::optional<int>&& local_index);
	};

	struct Struct
	{
		Token m_name;
		std::vector<Token> m_generic_params;
		std::vector<MidoriType::ClassConstraint> m_constraints;
		std::shared_ptr<MidoriType> m_self_type;

		Struct(const Token& name, std::vector<Token>&& generic_params, std::vector<MidoriType::ClassConstraint>&& constraints, std::shared_ptr<MidoriType>&& self_type);
	};

	struct Union
	{
		Token m_name;
		std::vector<Token> m_generic_params;
		std::vector<Token> m_constructor_names;
		std::vector<MidoriType::ClassConstraint> m_constraints;
		std::shared_ptr<MidoriType> m_self_type;

		Union(const Token& name, std::vector<Token>&& generic_params, std::vector<Token>&& constructor_names, std::vector<MidoriType::ClassConstraint>&& constraints, std::shared_ptr<MidoriType>&& self_type);
	};

	struct Class
	{
		struct AssociatedTypeDeclaration
		{
			Token m_name;

			explicit AssociatedTypeDeclaration(const Token& name);
		};

		Token m_name;
		std::vector<Token> m_type_params;
		std::vector<MidoriType::ClassConstraint> m_superclasses;
		std::vector<AssociatedTypeDeclaration> m_associated_types;
		std::vector<std::unique_ptr<MidoriStatement>> m_methods;

		Class(const Token& name, std::vector<Token>&& type_params, std::vector<MidoriType::ClassConstraint>&& superclasses, std::vector<AssociatedTypeDeclaration>&& associated_types, std::vector<std::unique_ptr<MidoriStatement>>&& methods);
	};

	struct Instance
	{
		struct AssociatedTypeBinding
		{
			Token m_name;
			std::shared_ptr<MidoriType> m_type;

			AssociatedTypeBinding(const Token& name, std::shared_ptr<MidoriType>&& type);
		};

		Token m_class_name;
		std::vector<std::shared_ptr<MidoriType>> m_type_args;
		std::vector<MidoriType::ClassConstraint> m_constraints;
		std::vector<AssociatedTypeBinding> m_associated_types;
		std::vector<std::unique_ptr<MidoriStatement>> m_methods;

		Instance(const Token& class_name, std::vector<std::shared_ptr<MidoriType>>&& type_args, std::vector<MidoriType::ClassConstraint>&& constraints, std::vector<AssociatedTypeBinding>&& associated_types, std::vector<std::unique_ptr<MidoriStatement>>&& methods);
	};

	struct TypeAlias
	{
		Token m_name;
		std::vector<Token> m_generic_params;
		std::shared_ptr<MidoriType> m_aliased_type;

		TypeAlias(const Token& name, std::vector<Token>&& generic_params, std::shared_ptr<MidoriType>&& aliased_type);
	};

private:
	using StatementUnion = std::variant<ExpressionStatement, VariableDefinition, TupleDefinition, FunctionDefinition, ForeignDefinition, Struct, Union, Class, Instance, TypeAlias>;
	StatementUnion m_variant;

public:
	template<typename T>
	MidoriStatement(T&& stmt_data) : m_variant(std::move(stmt_data))
	{
	}

	template<typename T>
	T& GetStatement()
	{
		return std::get<T>(m_variant);
	}

	template<typename T>
	const T& GetStatement() const
	{
		return std::get<T>(m_variant);
	}

	template<typename T>
	constexpr bool IsStatement() const
	{
		return std::holds_alternative<T>(m_variant);
	}

	StatementUnion& operator*();
	const StatementUnion& operator*() const;
};

using MidoriProgramTree = std::vector<std::unique_ptr<MidoriStatement>>;

class MidoriPattern
{
public:
	struct BasePattern
	{
		std::shared_ptr<MidoriType> m_type_data = MidoriType::MakeUndecidedType();
	};

	struct Binding : BasePattern
	{
		Token m_name;
		std::optional<int> m_local_index;

		Binding(const Token& name, std::optional<int> local_index);
	};

	struct Wildcard : BasePattern
	{
		Token m_token;

		Wildcard(const Token& token);
	};

	using LiteralKind = MidoriLiteralKind;

	struct Literal : BasePattern
	{
		Token m_token;
		LiteralKind m_kind;

		Literal(const Token& token, LiteralKind kind);
	};

	struct Tuple : BasePattern
	{
		Token m_left_paren;
		std::vector<std::unique_ptr<MidoriPattern>> m_elements;

		Tuple(const Token& left_paren, std::vector<std::unique_ptr<MidoriPattern>>&& elements);
	};

	struct Array : BasePattern
	{
		Token m_left_bracket;
		std::vector<std::unique_ptr<MidoriPattern>> m_elements;

		Array(const Token& left_bracket, std::vector<std::unique_ptr<MidoriPattern>>&& elements);
	};

	struct Constructor : BasePattern
	{
		Token m_name_token;
		std::string m_name;
		std::vector<std::unique_ptr<MidoriPattern>> m_args;
		bool m_is_union = false;
		int m_tag = -1;

		Constructor(const Token& name_token, std::string&& name, std::vector<std::unique_ptr<MidoriPattern>>&& args, bool is_union);
	};

private:
	using PatternUnion = std::variant<Binding, Wildcard, Literal, Tuple, Array, Constructor>;
	PatternUnion m_variant;

public:
	template<typename T>
	MidoriPattern(T&& pattern_data) : m_variant(std::move(pattern_data))
	{
	}

	template<typename T>
	T& GetPattern()
	{
		return std::get<T>(m_variant);
	}

	template<typename T>
	const T& GetPattern() const
	{
		return std::get<T>(m_variant);
	}

	template<typename T>
	constexpr bool IsPattern() const
	{
		return std::holds_alternative<T>(m_variant);
	}

	PatternUnion& operator*();
	const PatternUnion& operator*() const;

	std::shared_ptr<MidoriType>& GetType();
	const std::shared_ptr<MidoriType>& GetType() const;
};

class MidoriExpression
{
public:
	struct NameContext
	{
		struct Local
		{
			int m_index = 0;
		};
		struct Cell
		{
			int m_index = 0;
		};
		struct Global {};

		using Tag = std::variant<Local, Cell, Global>;
	};

	enum class ConditionOperandType
	{
		INTEGER,
		FLOAT,
		OTHER
	};

	struct BaseExpression
	{
		std::shared_ptr<MidoriType> m_type_data = MidoriType::MakeUndecidedType();
	};

	struct As : BaseExpression
	{
		Token m_as_keyword;
		std::weak_ptr<MidoriType> m_from_type;
		std::shared_ptr<MidoriType> m_to_type;
		std::unique_ptr<MidoriExpression> m_expr;
		bool m_uses_convertable = false;

		As(const Token& as_keyword, std::shared_ptr<MidoriType> to_type, std::unique_ptr<MidoriExpression>&& expr);
	};

	struct Binary : BaseExpression
	{
		Token m_op;
		std::unique_ptr<MidoriExpression> m_left;
		std::unique_ptr<MidoriExpression> m_right;
		bool m_uses_concatenable = false; // True if using Concatenable typeclass for ++
		bool m_uses_equatable = false;   // True if using Equatable typeclass for == or !=
		bool m_uses_orderable = false;   // True if using Orderable typeclass for <, >, <=, >=

		Binary(const Token& op, std::unique_ptr<MidoriExpression>&& left, std::unique_ptr<MidoriExpression>&& right);
	};

	struct Group : BaseExpression
	{
		std::unique_ptr<MidoriExpression> m_expr_in;

		Group(std::unique_ptr<MidoriExpression>&& expr_in);
	};

	struct Tuple : BaseExpression
	{
		Token m_op;
		std::vector<std::unique_ptr<MidoriExpression>> m_elements;

		Tuple(const Token& op, std::vector<std::unique_ptr<MidoriExpression>>&& elements);
	};

	using LiteralKind = MidoriLiteralKind;

	struct Literal : BaseExpression
	{
		Token m_token;
		LiteralKind m_kind;

		Literal(const Token& token, LiteralKind kind);
	};

	struct UnaryPrefix : BaseExpression
	{
		Token m_op;
		std::unique_ptr<MidoriExpression> m_expr;
		bool m_uses_countable = false;

		UnaryPrefix(const Token& op, std::unique_ptr<MidoriExpression>&& expr);
	};

	struct UnarySuffix : BaseExpression
	{
		Token m_op;
		std::unique_ptr<MidoriExpression> m_expr;

		UnarySuffix(const Token& op, std::unique_ptr<MidoriExpression>&& expr);
	};

	struct Spawn : BaseExpression
	{
		Token m_spawn_keyword;
		// Any expression of function type, evaluated on the spawning side and
		// transferred to the worker: a name, a lambda, or a parameter holding one.
		std::unique_ptr<MidoriExpression> m_callee;
		std::vector<std::unique_ptr<MidoriExpression>> m_arguments;
		// Concurrency::Spawn(argument, F): m_arguments holds the one argument, which
		// stands for all of F's parameters - the value itself for one parameter, a
		// tuple spread across them for several, and a Unit that is discarded for none.
		// The type checker records F's arity so the code generator knows which.
		int m_callee_arity = -1;

		Spawn(const Token& spawn_keyword, std::unique_ptr<MidoriExpression>&& callee, std::vector<std::unique_ptr<MidoriExpression>>&& arguments);
	};

	struct Join : BaseExpression
	{
		Token m_join_keyword;
		std::unique_ptr<MidoriExpression> m_worker;
		// `join w` has type Result<T, WorkerError>. The parser resolves both
		// declarations by name where the join is written; the type checker
		// instantiates Result and records the constructor tags the VM builds.
		std::shared_ptr<MidoriType> m_result_type;
		std::shared_ptr<MidoriType> m_worker_error_type;
		int m_ok_tag = 0;
		int m_err_tag = 0;
		int m_cancelled_tag = 0;
		int m_failed_tag = 0;

		Join(const Token& join_keyword, std::unique_ptr<MidoriExpression>&& worker, std::shared_ptr<MidoriType>&& result_type, std::shared_ptr<MidoriType>&& worker_error_type);
	};

	struct ChannelCreate : BaseExpression
	{
		Token m_channel_keyword;
		std::shared_ptr<MidoriType> m_element_type;
		std::unique_ptr<MidoriExpression> m_capacity;

		ChannelCreate(const Token& channel_keyword, std::shared_ptr<MidoriType>&& element_type, std::unique_ptr<MidoriExpression>&& capacity);
	};

	struct Send : BaseExpression
	{
		Token m_arrow;
		std::unique_ptr<MidoriExpression> m_channel;
		std::unique_ptr<MidoriExpression> m_value;

		Send(const Token& arrow, std::unique_ptr<MidoriExpression>&& channel, std::unique_ptr<MidoriExpression>&& value);
	};

	struct Receive : BaseExpression
	{
		Token m_arrow;
		std::unique_ptr<MidoriExpression> m_channel;

		Receive(const Token& arrow, std::unique_ptr<MidoriExpression>&& channel);
	};

	struct NameAccess : BaseExpression
	{
		Token m_name;
		NameContext::Tag m_name_ctx;

		NameAccess(const Token& name, NameContext::Tag&& name_ctx);
	};

	struct Call : BaseExpression
	{
		Token m_paren;
		std::unique_ptr<MidoriExpression> m_callee;
		std::vector<std::unique_ptr<MidoriExpression>> m_arguments;
		bool m_is_foreign;
		bool m_is_tail_call = false;

		Call(const Token& paren, std::unique_ptr<MidoriExpression>&& callee, std::vector<std::unique_ptr<MidoriExpression>>&& arguments, bool is_foreign = false);
	};

	struct Function : BaseExpression
	{
		Token m_function_keyword;
		std::vector<Token> m_generic_params;
		std::vector<Token> m_params;
		std::vector<std::shared_ptr<MidoriType>> m_param_types;
		std::vector<MidoriType::ClassConstraint> m_constraints;
		std::shared_ptr<MidoriType> m_return_type;
		std::unique_ptr<MidoriExpression> m_body;
		int m_captured_count;

		Function(const Token& function_keyword, std::vector<Token>&& generic_params, std::vector<Token>&& params, std::vector<std::shared_ptr<MidoriType>>&& param_types, std::shared_ptr<MidoriType>&& return_type, std::unique_ptr<MidoriExpression>&& body, int captured_count = 0, std::vector<MidoriType::ClassConstraint>&& constraints = {});
	};

	struct Construct : BaseExpression
	{
		struct Union
		{
			int m_index;
		};
		struct Struct {};
		using ConstructContext = std::variant<Union, Struct>;

		Token m_data_name;
		std::vector<std::unique_ptr<MidoriExpression>> m_params;
		std::shared_ptr<MidoriType> m_return_type;
		bool m_has_explicit_type_args = false;
		ConstructContext m_construct_ctx;

		Construct(const Token& data_name, std::vector<std::unique_ptr<MidoriExpression>>&& params, std::shared_ptr<MidoriType>&& return_type, bool has_explicit_type_args, ConstructContext&& construct_ctx);

		template<typename T>
		constexpr bool IsConstructTypeOf() const
		{
			return std::holds_alternative<T>(m_construct_ctx);
		}
	};

	// `{ source with field = value, other = value }` - a copy of `source` with those
	// fields replaced. Kept as its own node because it names only the fields it changes:
	// lowering it to a Construct would mean rebuilding the full argument list, and every
	// unmentioned field would have to be re-read from the source by name.
	// A record update takes its type from the already-resolved source.
	struct RecordUpdate : BaseExpression
	{
		struct FieldUpdate
		{
			Token m_name;
			std::unique_ptr<MidoriExpression> m_value;
			// Slot in the struct's declared member order; filled by the type checker.
			int m_index = -1;

			FieldUpdate(const Token& name, std::unique_ptr<MidoriExpression>&& value);
		};

		Token m_with_keyword;
		std::unique_ptr<MidoriExpression> m_source;
		std::vector<FieldUpdate> m_updates;
		// One entry per declared member slot, in declared order. Holds the index into
		// m_updates that supplies that slot, or -1 when the slot is copied from the
		// source. Filled by the type checker, consumed by the code generator.
		std::vector<int> m_slot_sources;

		RecordUpdate(const Token& with_keyword, std::unique_ptr<MidoriExpression>&& source, std::vector<FieldUpdate>&& updates);
	};

	struct IfElse : BaseExpression
	{
		Token m_if_token;
		Token m_then_token;
		Token m_else_token;
		std::unique_ptr<MidoriExpression> m_condition;
		std::unique_ptr<MidoriExpression> m_true_branch;
		std::unique_ptr<MidoriExpression> m_else_branch;
		ConditionOperandType m_condition_operand_type;

		IfElse(const Token& if_token, const Token& then_token, const Token& else_token, std::unique_ptr<MidoriExpression>&& condition, std::unique_ptr<MidoriExpression>&& true_branch, std::unique_ptr<MidoriExpression>&& else_branch, ConditionOperandType condition_operand_type);
	};

	struct MemberAccess : BaseExpression
	{
		Token m_member_name;
		std::unique_ptr<MidoriExpression> m_struct;
		int m_index;

		MemberAccess(const Token& member_name, std::unique_ptr<MidoriExpression>&& struct_expr, int index = -1);
	};

	struct Array : BaseExpression
	{
		Token m_op;
		std::vector<std::unique_ptr<MidoriExpression>> m_elems;

		Array(const Token& op, std::vector<std::unique_ptr<MidoriExpression>>&& elems);
	};

	struct IndexAccess : BaseExpression
	{
		Token m_op;
		std::unique_ptr<MidoriExpression> m_index;
		std::unique_ptr<MidoriExpression> m_arr_var;
		bool m_uses_indexable = false;

		IndexAccess(const Token& op, std::unique_ptr<MidoriExpression>&& index, std::unique_ptr<MidoriExpression>&& arr_var);
	};

	struct ArrayComprehension : BaseExpression
	{
		Token m_bracket;                
		Token m_loop_variable;                         
		Token m_in_keyword;                                 
		std::unique_ptr<MidoriExpression> m_transform_expr;
		std::unique_ptr<MidoriExpression> m_range;    
		std::shared_ptr<MidoriType> m_iterable_item_type = nullptr;
		int m_loop_variable_index = -1;
		int m_hidden_step_index = -1;    
		int m_hidden_end_index = -1;  
		int m_hidden_array_index = -1;
		int m_result_array_index = -1;  
		int m_iterable_some_tag = -1;
		bool m_is_array_iteration = false;
		bool m_is_iterable_iteration = false;

		ArrayComprehension(const Token& bracket, const Token& loop_variable, const Token& in_keyword, std::unique_ptr<MidoriExpression>&& transform_expr, std::unique_ptr<MidoriExpression>&& range);
	};

	struct RangeBinary : BaseExpression
	{
		Token m_range_op;
		std::unique_ptr<MidoriExpression> m_start;
		std::unique_ptr<MidoriExpression> m_end;

		RangeBinary(const Token& range_op, std::unique_ptr<MidoriExpression>&& start, std::unique_ptr<MidoriExpression>&& end);
	};

	struct RangeTernary : BaseExpression
	{
		Token m_first_range_op;
		Token m_second_range_op;
		std::unique_ptr<MidoriExpression> m_start;
		std::unique_ptr<MidoriExpression> m_step;
		std::unique_ptr<MidoriExpression> m_end;

		RangeTernary(const Token& first_op, const Token& second_op, std::unique_ptr<MidoriExpression>&& start, std::unique_ptr<MidoriExpression>&& step, std::unique_ptr<MidoriExpression>&& end);
	};

	struct Block : BaseExpression
	{
		Token m_right_brace;
		std::vector<std::unique_ptr<MidoriStatement>> m_stmts;
		std::optional<std::unique_ptr<MidoriExpression>> m_final_expr = std::nullopt;
		int m_local_count = 0;

		Block(const Token& right_brace, std::vector<std::unique_ptr<MidoriStatement>>&& stmts, int local_count, std::unique_ptr<MidoriExpression>&& final_expr = nullptr);

		bool HasDefine() const;
	};

	struct Match : BaseExpression
	{
		Token m_match_keyword;
		std::unique_ptr<MidoriExpression> m_arg_expr;
		std::vector<std::unique_ptr<MidoriExpression>> m_cases;
		int m_match_value_index = -1;

		Match(const Token& match_keyword, std::unique_ptr<MidoriExpression>&& arg_expr, std::vector<std::unique_ptr<MidoriExpression>>&& cases);
	};

	struct Case : BaseExpression
	{
		Token m_keyword;
		std::unique_ptr<MidoriPattern> m_pattern;
		std::unique_ptr<MidoriExpression> m_expr;
		std::optional<std::unique_ptr<MidoriExpression>> m_guard = std::nullopt;
		int m_binding_count = 0;

		Case(const Token& keyword, std::unique_ptr<MidoriPattern>&& pattern, std::unique_ptr<MidoriExpression>&& expr, int binding_count, std::unique_ptr<MidoriExpression>&& guard = nullptr);

		bool HasGuard() const;
	};

	struct For : BaseExpression
	{
		Token m_for_keyword;
		Token m_loop_variable;
		Token m_in_keyword;
		std::unique_ptr<MidoriExpression> m_range;
		std::unique_ptr<MidoriExpression> m_body;
		std::shared_ptr<MidoriType> m_iterable_item_type = nullptr;
		int m_loop_variable_index = -1;
		int m_hidden_step_index = -1;   // For range: step; For array: current index
		int m_hidden_end_index = -1;    // For range: end; For array: length
		int m_hidden_array_index = -1;  // For array iteration: stores array reference
		int m_iterable_some_tag = -1;
		bool m_is_array_iteration = false;
		bool m_is_iterable_iteration = false;

		For(const Token& for_keyword, const Token& loop_variable, const Token& in_keyword, std::unique_ptr<MidoriExpression>&& range, std::unique_ptr<MidoriExpression>&& body);
	};

private:
	using ExpressionUnion = std::variant<As, Binary, Group, Tuple, Literal, UnaryPrefix, UnarySuffix, Spawn, Join, ChannelCreate, Send, Receive, NameAccess, Call, Function, Construct, RecordUpdate, IfElse, MemberAccess, Array, IndexAccess, ArrayComprehension, RangeBinary, RangeTernary, Block, Match, Case, For>;
	ExpressionUnion m_variant;

public:
	template<typename T>
	MidoriExpression(T&& expr_data) : m_variant(std::move(expr_data))
	{
	}

	template<typename T>
	T& GetExpression()
	{
		return std::get<T>(m_variant);
	}

	template<typename T>
	const T& GetExpression() const
	{
		return std::get<T>(m_variant);
	}

	template<typename T>
	constexpr bool IsExpression() const
	{
		return std::holds_alternative<T>(m_variant);
	}

	bool IsLiteral(LiteralKind kind) const
	{
		return IsExpression<Literal>() && GetExpression<Literal>().m_kind == kind;
	}

	ExpressionUnion& operator*();
	const ExpressionUnion& operator*() const;

	std::shared_ptr<MidoriType>& GetType();
	const std::shared_ptr<MidoriType>& GetType() const;

	template<typename Kind>
	struct ContainsVisitor
	{
		template<typename T>
		bool operator()(const T& node) const
		{
			if constexpr (std::is_same_v<T, Kind>)
			{
				return true;
			}
			else if constexpr (std::is_same_v<T, MidoriExpression::As>)
			{
				return node.m_expr->template Contains<Kind>();
			}
			else if constexpr (std::is_same_v<T, MidoriExpression::Binary>)
			{
				return node.m_left->template Contains<Kind>() || node.m_right->template Contains<Kind>();
			}
			else if constexpr (std::is_same_v<T, MidoriExpression::UnaryPrefix>)
			{
				return node.m_expr->template Contains<Kind>();
			}
			else if constexpr (std::is_same_v<T, MidoriExpression::UnarySuffix>)
			{
				return node.m_expr->template Contains<Kind>();
			}
			else if constexpr (std::is_same_v<T, MidoriExpression::Spawn>)
			{
				return std::ranges::any_of
				(
					node.m_arguments,
					[](const std::unique_ptr<MidoriExpression>& arg)
					{
						return arg->template Contains<Kind>();
					}
				);
			}
			else if constexpr (std::is_same_v<T, MidoriExpression::Join>)
			{
				return node.m_worker->template Contains<Kind>();
			}
			else if constexpr (std::is_same_v<T, MidoriExpression::ChannelCreate>)
			{
				return node.m_capacity->template Contains<Kind>();
			}
			else if constexpr (std::is_same_v<T, MidoriExpression::Send>)
			{
				return node.m_channel->template Contains<Kind>() || node.m_value->template Contains<Kind>();
			}
			else if constexpr (std::is_same_v<T, MidoriExpression::Receive>)
			{
				return node.m_channel->template Contains<Kind>();
			}
			else if constexpr (std::is_same_v<T, MidoriExpression::Group>)
			{
				return node.m_expr_in->template Contains<Kind>();
			}
			else if constexpr (std::is_same_v<T, MidoriExpression::IfElse>)
			{
				return node.m_condition->template Contains<Kind>()
					|| node.m_true_branch->template Contains<Kind>()
					|| node.m_else_branch->template Contains<Kind>();
			}
			else if constexpr (std::is_same_v<T, MidoriExpression::Block>)
			{
				const bool has_statement = std::ranges::any_of
				(
					node.m_stmts,
					[](const std::unique_ptr<MidoriStatement>& stmt)
					{
						return stmt->template IsStatement<MidoriStatement::ExpressionStatement>()
							&& stmt->template GetStatement<MidoriStatement::ExpressionStatement>().m_expr->template Contains<Kind>();
					}
				);

				const bool has_final_expr = node.m_final_expr.has_value()
					&& node.m_final_expr.value()->template Contains<Kind>();

				return has_statement || has_final_expr;
			}
			else if constexpr (std::is_same_v<T, MidoriExpression::Function>)
			{
				return node.m_body->template Contains<Kind>();
			}
			else if constexpr (std::is_same_v<T, MidoriExpression::Call>)
			{
				const bool has_arg = std::ranges::any_of
				(
					node.m_arguments,
					[](const std::unique_ptr<MidoriExpression>& arg)
					{
						return arg->template Contains<Kind>();
					}
				);

				return node.m_callee->template Contains<Kind>() || has_arg;
			}
			else if constexpr (std::is_same_v<T, MidoriExpression::ArrayComprehension>)
			{
				return node.m_transform_expr->template Contains<Kind>()
					|| node.m_range->template Contains<Kind>();
			}
			else
			{
				return false;
			}
		}
	};

	template<typename Kind>
	bool Contains() const 
	{
		return std::visit(ContainsVisitor<Kind>{}, m_variant);
	}
};

namespace MidoriAstDetail
{
	template<typename Visitor, typename Node>
	using VisitNodeRefResult = decltype(std::visit(std::declval<Visitor>(), *std::declval<Node>()));

	template<typename Visitor, typename Node>
	using VisitNodePtrResult = decltype(std::visit(std::declval<Visitor>(), **std::declval<Node>()));
}

template<typename Visitor>
MidoriAstDetail::VisitNodeRefResult<Visitor, MidoriStatement&> VisitNode(Visitor&& visitor, MidoriStatement& statement)
{
	return std::visit(std::forward<Visitor>(visitor), *statement);
}

template<typename Visitor>
MidoriAstDetail::VisitNodePtrResult<Visitor, std::unique_ptr<MidoriStatement>&> VisitNode(Visitor&& visitor, std::unique_ptr<MidoriStatement>& statement)
{
	return std::visit(std::forward<Visitor>(visitor), **statement);
}

template<typename Visitor>
MidoriAstDetail::VisitNodePtrResult<Visitor, const std::unique_ptr<MidoriStatement>&> VisitNode(Visitor&& visitor, const std::unique_ptr<MidoriStatement>& statement)
{
	return std::visit(std::forward<Visitor>(visitor), **statement);
}

template<typename Visitor>
MidoriAstDetail::VisitNodeRefResult<Visitor, MidoriExpression&> VisitNode(Visitor&& visitor, MidoriExpression& expression)
{
	return std::visit(std::forward<Visitor>(visitor), *expression);
}

template<typename Visitor>
MidoriAstDetail::VisitNodePtrResult<Visitor, std::unique_ptr<MidoriExpression>&> VisitNode(Visitor&& visitor, std::unique_ptr<MidoriExpression>& expression)
{
	return std::visit(std::forward<Visitor>(visitor), **expression);
}

template<typename Visitor>
MidoriAstDetail::VisitNodePtrResult<Visitor, const std::unique_ptr<MidoriExpression>&> VisitNode(Visitor&& visitor, const std::unique_ptr<MidoriExpression>& expression)
{
	return std::visit(std::forward<Visitor>(visitor), **expression);
}

template<typename Visitor>
MidoriAstDetail::VisitNodeRefResult<Visitor, MidoriPattern&> VisitNode(Visitor&& visitor, MidoriPattern& pattern)
{
	return std::visit(std::forward<Visitor>(visitor), *pattern);
}

template<typename Visitor>
MidoriAstDetail::VisitNodePtrResult<Visitor, std::unique_ptr<MidoriPattern>&> VisitNode(Visitor&& visitor, std::unique_ptr<MidoriPattern>& pattern)
{
	return std::visit(std::forward<Visitor>(visitor), **pattern);
}

template<typename Visitor>
MidoriAstDetail::VisitNodePtrResult<Visitor, const std::unique_ptr<MidoriPattern>&> VisitNode(Visitor&& visitor, const std::unique_ptr<MidoriPattern>& pattern)
{
	return std::visit(std::forward<Visitor>(visitor), **pattern);
}
