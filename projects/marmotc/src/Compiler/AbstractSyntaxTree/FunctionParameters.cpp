#include "FunctionParameters.h"

#include <utility>

FunctionParameters::FunctionParameters(std::vector<Token> names)
	: m_bindings(std::move(names))
{
}

FunctionParameters::FunctionParameters(std::vector<GeneratedBinding> bindings)
	: m_bindings(std::move(bindings))
{
}

FunctionParameters FunctionParameters::Generated(std::vector<GeneratedBinding> bindings)
{
	return FunctionParameters(std::move(bindings));
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
