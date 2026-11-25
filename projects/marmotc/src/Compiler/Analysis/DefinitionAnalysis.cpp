#include "DefinitionAnalysis.h"

#include "Compiler/Analysis/AbstractSyntaxTreeWalker.h"
#include "Compiler/Names/SpecialNames.h"
#include "Support/Optional.h"

#include <algorithm>

namespace
{
	SourceSpan Span(const Token& first, MarmotSource::Position end)
	{
		return { first.m_file_name, { first.m_line, static_cast<size_t>(first.m_column.Value()) }, end };
	}

	class DefinitionCollector : public MarmotAbstractSyntaxTreeWalker
	{
	public:
		DefinitionCollector(const std::string& module_name, const std::unordered_map<int, std::string>& generic_parameter_names, const std::vector<int>& doc_comment_lines)
			: m_module_name(module_name), m_generic_parameter_names(generic_parameter_names), m_doc_comment_lines(doc_comment_lines)
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
		const std::unordered_map<int, std::string>& m_generic_parameter_names;
		const std::vector<int>& m_doc_comment_lines;
		ModuleDefinitions m_definitions;
		std::unordered_map<BindingId, SymbolDeclaration> m_locals;
		std::vector<std::pair<BindingId, SourceSpan>> m_local_references;

		std::string GlobalKey(const std::string& name) const
		{
			return m_module_name + NameSeparator.data() + name;
		}

		MarmotCompiler::Optional<int> DocumentationStart(int line) const
		{
			auto first = std::ranges::lower_bound(m_doc_comment_lines, line - 1);
			if (first == m_doc_comment_lines.end() || *first != line - 1)
			{
				return MarmotCompiler::None;
			}
			while (first != m_doc_comment_lines.begin() && *(first - 1) == *first - 1)
			{
				--first;
			}
			return *first;
		}

		void RecordType(const SourceSpan& span, const MarmotType& type, MarmotCompiler::Optional<int> documentation_line = MarmotCompiler::None)
		{
			if (!type.IsType<MarmotType::UndecidedType>())
			{
				m_definitions.m_types.push_back({ span, type.DisplayString(m_generic_parameter_names), documentation_line });
			}
		}

		void Declare(const Token& name, const MarmotCompiler::Optional<BindingId>& binding, const MarmotType& type, bool documentable = false)
		{
			if (name.IsDiscard() || !name.m_column || !name.m_source_length)
			{
				return;
			}
			SourceSpan span = Span(name, { name.m_line, static_cast<size_t>(*name.m_column) + *name.m_source_length });
			const MarmotCompiler::Optional<int> documentation_line = documentable ? DocumentationStart(name.m_line) : MarmotCompiler::None;
			RecordType(span, type, documentation_line);
			SymbolDeclaration declaration{ std::move(span), documentation_line };
			if (binding)
			{
				m_locals.emplace(*binding, std::move(declaration));
			}
			else
			{
				m_definitions.m_globals.emplace(GlobalKey(name.m_lexeme), std::move(declaration));
			}
		}

		void DeclareParameters(const FunctionParameters& params, const std::vector<std::shared_ptr<MarmotType>>& types)
		{
			for (size_t index = 0; index < params.Size(); ++index)
			{
				if (params.GeneratedBindingAt(index) == nullptr)
				{
					Declare(params.SourceToken(index), params.BindingAt(index), *types[index]);
				}
			}
		}

		void operator()(MarmotStatement::VariableDefinition& node) override
		{
			Declare(node.m_name, node.m_binding, *node.m_annotated_type.ValueOr(node.m_value->GetType()), true);
			VisitExpression(node.m_value);
		}

		void operator()(MarmotStatement::TupleDefinition& node) override
		{
			const auto& types = node.m_value->GetType()->GetType<MarmotType::TupleType>().m_element_types;
			for (size_t index = 0; index < node.m_names.size(); ++index)
			{
				Declare(node.m_names[index], node.m_bindings[index], *types[index], true);
			}
			VisitExpression(node.m_value);
		}

		void operator()(MarmotStatement::FunctionDefinition& node) override
		{
			Declare(node.m_name, node.m_binding, *MarmotType::MakeFunctionType(node.m_param_types, std::shared_ptr<MarmotType>(node.m_return_type)), true);
			DeclareParameters(node.m_params, node.m_param_types);
			VisitExpression(node.m_body);
		}

		void operator()(MarmotStatement::ForeignDefinition& node) override
		{
			Declare(node.m_function_name, node.m_binding, *node.m_type, true);
		}

		void operator()(MarmotStatement::Instance& node) override
		{
			for (MarmotStatement::Instance::Method& method : node.m_methods)
			{
				DeclareParameters(method.m_definition.m_params, method.m_definition.m_param_types);
				VisitExpression(method.m_definition.m_body);
			}
		}

		void operator()(MarmotPattern::Binding& node) override
		{
			if (!node.m_is_generated)
			{
				Declare(node.m_name, node.m_binding, *node.m_type_data);
			}
		}

		void operator()(MarmotExpression::Function& node) override
		{
			const auto& signature = node.m_type_data->GetType<MarmotType::FunctionType>();
			DeclareParameters(node.m_params, signature.m_param_types);
			VisitExpression(node.m_body);
		}

		template <typename Iteration>
		void DeclareLoopVariable(const Iteration& node)
		{
			const MarmotType& range = *node.m_range->GetType();
			const std::shared_ptr<MarmotType>& item = node.m_is_iterable_iteration ? node.m_iterable_item_type
				: node.m_is_array_iteration ? range.GetType<MarmotType::ArrayType>().m_element_type
				: range.GetType<MarmotType::RangeType>().m_element_type;
			Declare(node.m_loop_variable, node.m_loop_binding, *item);
		}

		void operator()(MarmotExpression::ArrayComprehension& node) override
		{
			DeclareLoopVariable(node);
			MarmotAbstractSyntaxTreeWalker::operator()(node);
		}

		void operator()(MarmotExpression::For& node) override
		{
			DeclareLoopVariable(node);
			MarmotAbstractSyntaxTreeWalker::operator()(node);
		}

		void operator()(MarmotExpression::NameAccess& node) override
		{
			if (!node.m_value_reference_end)
			{
				return;
			}
			SourceSpan reference = Span(node.m_name, *node.m_value_reference_end);
			RecordType(reference, *node.m_type_data);
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

ModuleDefinitions ModuleDefinitions::Collect(MarmotProgramTree& program, const std::string& module_name, const std::unordered_map<int, std::string>& generic_parameter_names, const std::vector<int>& doc_comment_lines)
{
	return DefinitionCollector(module_name, generic_parameter_names, doc_comment_lines).Collect(program);
}
