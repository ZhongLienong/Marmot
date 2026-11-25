#include "DefinitionAnalysis.h"

#include "Compiler/Analysis/AbstractSyntaxTreeWalker.h"
#include "Compiler/Names/SpecialNames.h"

namespace
{
	SourceSpan Span(const Token& first, MarmotSource::Position end)
	{
		return { first.m_file_name, { first.m_line, static_cast<size_t>(first.m_column.value()) }, end };
	}

	class DefinitionCollector : public MarmotAbstractSyntaxTreeWalker
	{
	public:
		explicit DefinitionCollector(const std::string& module_name)
			: m_module_name(module_name)
		{
		}

		ModuleDefinitions Collect(MarmotProgramTree& program)
		{
			VisitProgram(program);
			for (auto& [binding, reference] : m_local_references)
			{
				const auto declaration = m_locals.find(binding);
				if (declaration != m_locals.end())
				{
					m_definitions.m_local_links.push_back({ std::move(reference), declaration->second });
				}
			}
			return std::move(m_definitions);
		}

	private:
		const std::string& m_module_name;
		ModuleDefinitions m_definitions;
		std::unordered_map<BindingId, SourceSpan> m_locals;
		std::vector<std::pair<BindingId, SourceSpan>> m_local_references;

		std::string GlobalKey(const std::string& name) const
		{
			return m_module_name + NameSeparator.data() + name;
		}

		void Declare(const Token& name, const std::optional<BindingId>& binding)
		{
			if (name.IsDiscard() || !name.m_column || !name.m_source_length)
			{
				return;
			}
			SourceSpan span = Span(name, { name.m_line, static_cast<size_t>(*name.m_column) + *name.m_source_length });
			if (binding)
			{
				m_locals.emplace(*binding, std::move(span));
			}
			else
			{
				m_definitions.m_globals.emplace(GlobalKey(name.m_lexeme), std::move(span));
			}
		}

		void DeclareParameters(const FunctionParameters& params)
		{
			for (size_t index = 0; index < params.Size(); ++index)
			{
				if (params.GeneratedBindingAt(index) == nullptr)
				{
					Declare(params.SourceToken(index), params.BindingAt(index));
				}
			}
		}

		void operator()(MarmotStatement::VariableDefinition& node) override
		{
			Declare(node.m_name, node.m_binding);
			VisitExpression(node.m_value);
		}

		void operator()(MarmotStatement::TupleDefinition& node) override
		{
			for (size_t index = 0; index < node.m_names.size(); ++index)
			{
				Declare(node.m_names[index], node.m_bindings[index]);
			}
			VisitExpression(node.m_value);
		}

		void operator()(MarmotStatement::FunctionDefinition& node) override
		{
			Declare(node.m_name, node.m_binding);
			DeclareParameters(node.m_params);
			VisitExpression(node.m_body);
		}

		void operator()(MarmotStatement::ForeignDefinition& node) override
		{
			Declare(node.m_function_name, node.m_binding);
		}

		void operator()(MarmotStatement::Instance& node) override
		{
			for (MarmotStatement::Instance::Method& method : node.m_methods)
			{
				DeclareParameters(method.m_definition.m_params);
				VisitExpression(method.m_definition.m_body);
			}
		}

		void operator()(MarmotPattern::Binding& node) override
		{
			if (!node.m_is_generated)
			{
				Declare(node.m_name, node.m_binding);
			}
		}

		void operator()(MarmotExpression::Function& node) override
		{
			DeclareParameters(node.m_params);
			VisitExpression(node.m_body);
		}

		void operator()(MarmotExpression::ArrayComprehension& node) override
		{
			Declare(node.m_loop_variable, node.m_loop_binding);
			MarmotAbstractSyntaxTreeWalker::operator()(node);
		}

		void operator()(MarmotExpression::For& node) override
		{
			Declare(node.m_loop_variable, node.m_loop_binding);
			MarmotAbstractSyntaxTreeWalker::operator()(node);
		}

		void operator()(MarmotExpression::NameAccess& node) override
		{
			if (!node.m_value_reference_end)
			{
				return;
			}
			SourceSpan reference = Span(node.m_name, *node.m_value_reference_end);
			if (const auto* local = std::get_if<MarmotExpression::NameContext::Local>(&node.m_name_ctx))
			{
				m_local_references.emplace_back(local->m_binding, std::move(reference));
			}
			else if (const auto* cell = std::get_if<MarmotExpression::NameContext::Cell>(&node.m_name_ctx))
			{
				m_local_references.emplace_back(cell->m_binding, std::move(reference));
			}
			else
			{
				const std::string& name = node.m_name.m_lexeme;
				m_definitions.m_global_references.emplace_back
				(
					name.contains(NameSeparator) ? name : GlobalKey(name),
					std::move(reference)
				);
			}
		}
	};
}

ModuleDefinitions ModuleDefinitions::Collect(MarmotProgramTree& program, const std::string& module_name)
{
	return DefinitionCollector(module_name).Collect(program);
}
