#include "FunctionParameters.h"

#include <utility>

FunctionParameters::FunctionParameters(std::vector<Token> names, std::vector<BindingId> bindings, LexicalFunctionId function)
	: m_bindings(std::move(names)),
	m_source_bindings(std::move(bindings)),
	m_function(std::move(function))
{
}

FunctionParameters::FunctionParameters(std::vector<GeneratedBinding> bindings, LexicalFunctionId function)
	: m_bindings(std::move(bindings)),
	m_function(std::move(function))
{
}

FunctionParameters FunctionParameters::Generated(std::vector<GeneratedBinding> bindings, LexicalFunctionId function)
{
	return FunctionParameters(std::move(bindings), std::move(function));
}

const BindingId& FunctionParameters::BindingAt(size_t index) const
{
	if (const GeneratedBinding* binding = GeneratedBindingAt(index))
	{
		return binding->m_binding;
	}
	return m_source_bindings[index];
}

const LexicalFunctionId& FunctionParameters::Function() const
{
	return m_function;
}

size_t FunctionParameters::Size() const
{
	return std::visit([](const auto& bindings) { return bindings.size(); }, m_bindings);
}

bool FunctionParameters::Empty() const
{
	return Size() == 0u;
}

const Token& FunctionParameters::SourceToken(size_t index) const
{
	if (const GeneratedBinding* binding = GeneratedBindingAt(index))
	{
		return binding->m_anchor;
	}
	return std::get<std::vector<Token>>(m_bindings)[index];
}

const GeneratedBinding* FunctionParameters::GeneratedBindingAt(size_t index) const
{
	const std::vector<GeneratedBinding>* bindings = std::get_if<std::vector<GeneratedBinding>>(&m_bindings);
	return bindings != nullptr ? &(*bindings)[index] : nullptr;
}

std::string FunctionParameters::DisplayName(size_t index) const
{
	if (const GeneratedBinding* binding = GeneratedBindingAt(index))
	{
		return binding->DisplayName();
	}
	return SourceToken(index).m_lexeme;
}
