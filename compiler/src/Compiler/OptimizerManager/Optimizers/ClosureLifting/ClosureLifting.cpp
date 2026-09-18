#include "ClosureLifting.h"
#include "Compiler/Analysis/AbstractSyntaxTreeWalker.h"
#include "Compiler/Analysis/SharedAnalysis.h"
#include "Common/BuildConfig/BuildConfig.h"
#include "Common/Constant/Constant.h"
#include "Compiler/Token/Token.h"
#include <format>
#include <type_traits>

namespace
{
	constexpr std::string_view LiftedNamePrefix = "__lifted_";

	bool IsForwardingLiftWrapperCall(const MidoriStatement::FunctionDefinition& defun)
	{
		if (defun.m_body == nullptr || !defun.m_body->IsExpression<MidoriExpression::Call>())
		{
			return false;
		}

		const MidoriExpression::Call& call = defun.m_body->GetExpression<MidoriExpression::Call>();
		if (call.m_callee == nullptr || !call.m_callee->IsExpression<MidoriExpression::NameAccess>())
		{
			return false;
		}

		const MidoriExpression::NameAccess& callee = call.m_callee->GetExpression<MidoriExpression::NameAccess>();
		if (!std::holds_alternative<MidoriExpression::NameContext::Global>(callee.m_name_ctx))
		{
			return false;
		}

		if (!callee.m_name.m_lexeme.starts_with(LiftedNamePrefix) || call.m_arguments.size() != defun.m_params.size())
		{
			return false;
		}

		for (size_t i = 0u; i < call.m_arguments.size(); i += 1u)
		{
			const std::unique_ptr<MidoriExpression>& argument = call.m_arguments[i];
			if (argument == nullptr || !argument->IsExpression<MidoriExpression::NameAccess>())
			{
				return false;
			}

			const MidoriExpression::NameAccess& access = argument->GetExpression<MidoriExpression::NameAccess>();
			if (!std::holds_alternative<MidoriExpression::NameContext::Local>(access.m_name_ctx))
			{
				return false;
			}

			if (std::get<MidoriExpression::NameContext::Local>(access.m_name_ctx).m_index != static_cast<int>(i) || access.m_name.m_lexeme != defun.m_params[i].m_lexeme)
			{
				return false;
			}
		}

		return true;
	}

	Token MakeLiftedToken(const Token& source_token, std::string_view base_name)
	{
		static int s_global_lift_id = 0;
		return Token
		(
			std::format("__lifted_{}_{}", base_name, s_global_lift_id++),
			Token::Name::IDENTIFIER_LITERAL,
			source_token
		);
	}

	class SelfReferencePatcher : protected MidoriAbstractSyntaxTreeWalker
	{
public:
		std::string_view m_self_name;
		std::string m_new_global_name;
		int m_patch_count{ 0 };

		SelfReferencePatcher(std::string_view self_name, std::string new_global_name)
			: m_self_name(self_name), 
			m_new_global_name(std::move(new_global_name)) 
		{
		}

		void Patch(std::unique_ptr<MidoriExpression>& expr)
		{
			VisitExpression(expr);
		}

	protected:
		void RewriteAccess(Token& name, MidoriExpression::NameContext::Tag& context)
		{
			if (name.m_lexeme == m_self_name)
			{
				name.m_lexeme = m_new_global_name;
				context = MidoriExpression::NameContext::Global{};
				m_patch_count++;
			}
		}

		void operator()(MidoriExpression::NameAccess& access) override
		{
			RewriteAccess(access.m_name, access.m_name_ctx);
		}

	};
}

MidoriResult::OptimizerResult ClosureLifting::Optimize(MidoriProgramTree program_tree)
{
	ResetPassState();
	m_visible_globals.clear();
	m_pending_globals.clear();

	MidoriProgramTree rewritten_program;
	rewritten_program.reserve(program_tree.size());

	for (std::unique_ptr<MidoriStatement>& stmt : program_tree)
	{
		m_pending_globals.clear();
		VisitStatement(stmt);

		for (std::unique_ptr<MidoriStatement>& pending_global : m_pending_globals)
		{
			RecordVisibleGlobalNames(*pending_global);
			rewritten_program.emplace_back(std::move(pending_global));
		}

		RecordVisibleGlobalNames(*stmt);
		rewritten_program.emplace_back(std::move(stmt));
	}

	m_pending_globals.clear();
	return std::move(rewritten_program);
}

std::string_view ClosureLifting::GetName() const
{
	return "ClosureLifting";
}

void ClosureLifting::operator()(MidoriStatement::VariableDefinition& def)
{
	if (def.m_is_elided)
	{
		return;
	}

	// A global binding whose initializer is a lambda is already a named top-level
	// procedure. The code generator emits it under the binding's own name, so
	// lifting it here would only add a second global and an indirection, and would
	// leave the binding's name attached to no procedure at all.
	if (!def.m_local_index.has_value() && def.m_value != nullptr && def.m_value->IsExpression<MidoriExpression::Function>())
	{
		VisitAndReplace(def.m_value->GetExpression<MidoriExpression::Function>().m_body);
		return;
	}

	VisitAndReplace(def.m_value);
}

void ClosureLifting::operator()(MidoriExpression::Function& function)
{
	VisitAndReplace(function.m_body);

	// A generic lambda is registered for specialization under the name it is bound
	// to, so it must reach the code generator still attached to that binding.
	// Lifting it here would replace the initializer with a reference to the lifted
	// name and strand every call site on a name that is not registered as generic.
	if (!function.m_generic_params.empty())
	{
		return;
	}

	const MidoriAnalysis::LiftSafetySummary lift_safety = MidoriAnalysis::AnalyzeLiftSafety(*function.m_body, m_visible_globals);
	if (!lift_safety.m_is_safe)
	{
		return;
	}

	Token lifted_token = MakeLiftedToken(function.m_function_keyword, "lambda");

	std::unique_ptr<MidoriStatement> global_def = std::make_unique<MidoriStatement>
	(
		MidoriStatement::FunctionDefinition
		(
			lifted_token,
			std::move(function.m_generic_params),
			std::move(function.m_params),
			std::move(function.m_param_types),
			std::move(function.m_return_type),
			std::move(function.m_body),
			std::nullopt,
			0,
			std::move(function.m_constraints)
		)
	);
	m_pending_globals.emplace_back(std::move(global_def));
	m_visible_globals.insert(lifted_token.m_lexeme);

	m_pending_replacement = std::make_unique<MidoriExpression>
	(
		MidoriExpression::NameAccess(lifted_token, MidoriExpression::NameContext::Global{})
	);
	m_pending_replacement->GetType() = function.m_type_data;
}

void ClosureLifting::operator()(MidoriExpression::Block& block)
{
	for (std::unique_ptr<MidoriStatement>& stmt : block.m_stmts)
	{
		if (stmt->IsStatement<MidoriStatement::FunctionDefinition>())
		{
			MidoriStatement::FunctionDefinition& defun = stmt->GetStatement<MidoriStatement::FunctionDefinition>();

			VisitAndReplace(defun.m_body);

			bool is_local_nested = defun.m_local_index.has_value();

			if (is_local_nested && !defun.m_is_lift_wrapper && !IsForwardingLiftWrapperCall(defun))
			{
				const MidoriAnalysis::LiftSafetySummary lift_safety = MidoriAnalysis::AnalyzeLiftSafety(*defun.m_body, m_visible_globals, defun.m_name.m_lexeme);
				if (lift_safety.m_is_safe)
				{
					Token lifted_token = MakeLiftedToken(defun.m_name, defun.m_name.m_lexeme);

					SelfReferencePatcher patcher(defun.m_name.m_lexeme, lifted_token.m_lexeme);
					patcher.Patch(defun.m_body);

					MarkOptimization();

					// Reset captured count to 0. 
					// This is crucial: it tells CodeGenerator to emit MAKE_FUNCTION instead of MAKE_CLOSURE,
					// and to NOT emit BIND_CAPTURES. Since we verified no captures are used, this is safe.
					defun.m_captured_count = 0;

					// Save parameters for the wrapper function BEFORE moving them
					std::vector<Token> wrapper_params = defun.m_params;
					std::vector<Token> wrapper_generic = defun.m_generic_params;
					std::vector<std::shared_ptr<MidoriType>> wrapper_param_types = defun.m_param_types;
					std::shared_ptr<MidoriType> wrapper_return_type = defun.m_return_type;

					std::unique_ptr<MidoriStatement> global_def = std::make_unique<MidoriStatement>
					(
						MidoriStatement::FunctionDefinition
						(
							lifted_token,
							std::move(defun.m_generic_params),
							std::move(defun.m_params),
							std::move(defun.m_param_types),
							std::move(defun.m_return_type),
							std::move(defun.m_body),
							std::nullopt,
							0,
							std::move(defun.m_constraints)
						)
					);
					m_pending_globals.emplace_back(std::move(global_def));
					m_visible_globals.insert(lifted_token.m_lexeme);

					// Create arguments for the wrapper call
					std::vector<std::unique_ptr<MidoriExpression>> call_args;
					call_args.reserve(wrapper_params.size());
					for (size_t i = 0uz; i < wrapper_params.size(); i += 1uz) 
					{
						call_args.emplace_back(std::make_unique<MidoriExpression>(MidoriExpression::NameAccess(wrapper_params[i], MidoriExpression::NameContext::Local{static_cast<int>(i)})));
					}

					MidoriExpression::NameContext::Tag context = MidoriExpression::NameContext::Global{};
					Token callee_token = lifted_token;
					std::unique_ptr<MidoriExpression> callee_expr = std::make_unique<MidoriExpression>(MidoriExpression::NameAccess(callee_token, std::move(context)));

					Token paren_token(std::string("("), Token::Name::LEFT_PAREN, defun.m_name.m_line, defun.m_name.m_file_name);
					std::unique_ptr<MidoriExpression> call_expr = std::make_unique<MidoriExpression>
						(
							MidoriExpression::Call
							(
								paren_token,
								std::move(callee_expr),
								std::move(call_args)
							)
						);

					std::unique_ptr<MidoriStatement> wrapper_def = std::make_unique<MidoriStatement>
					(
						MidoriStatement::FunctionDefinition
						(
							defun.m_name,
							std::move(wrapper_generic),
							std::move(wrapper_params),
							std::move(wrapper_param_types),
							std::move(wrapper_return_type),
							std::move(call_expr),
							std::move(defun.m_local_index)
						)
					);
					wrapper_def->GetStatement<MidoriStatement::FunctionDefinition>().m_is_lift_wrapper = true;

					stmt = std::move(wrapper_def);
				}
			}
		}
		else
		{
			VisitStatement(stmt);
		}
	}

	if (block.m_final_expr.has_value())
	{
		VisitAndReplace(block.m_final_expr.value());
	}
}

void ClosureLifting::RecordVisibleGlobalNames(const MidoriStatement& stmt)
{
	std::visit
	(
		[this](const auto& node)
		{
			using T = std::decay_t<decltype(node)>;

			if constexpr (std::is_same_v<T, MidoriStatement::VariableDefinition>)
			{
				if (!node.m_local_index.has_value())
				{
					m_visible_globals.insert(node.m_name.m_lexeme);
				}
			}
			else if constexpr (std::is_same_v<T, MidoriStatement::TupleDefinition>)
			{
				for (size_t i = 0u; i < node.m_names.size(); i += 1u)
				{
					if (!node.m_local_indices[i].has_value())
					{
						m_visible_globals.insert(node.m_names[i].m_lexeme);
					}
				}
			}
			else if constexpr (std::is_same_v<T, MidoriStatement::FunctionDefinition>)
			{
				if (!node.m_local_index.has_value())
				{
					m_visible_globals.insert(node.m_name.m_lexeme);
				}
			}
			else if constexpr (std::is_same_v<T, MidoriStatement::ForeignDefinition>)
			{
				if (!node.m_local_index.has_value())
				{
					m_visible_globals.insert(node.m_function_name.m_lexeme);
				}
			}
		},
		*stmt
	);
}
