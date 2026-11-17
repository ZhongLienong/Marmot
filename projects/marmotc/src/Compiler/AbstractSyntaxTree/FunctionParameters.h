#pragma once

#include "Compiler/Token/Token.h"
#include "Compiler/AbstractSyntaxTree/GeneratedBinding.h"

#include <string>
#include <variant>
#include <vector>

// A function's parameters: source names or compiler-generated local identities.
// Callers can obtain diagnostic locations without depending on that choice.
class FunctionParameters
{
private:
	std::variant<std::vector<Token>, std::vector<GeneratedBinding>> m_bindings;
	std::vector<BindingId> m_source_bindings;
	LexicalFunctionId m_function;

	FunctionParameters(std::vector<GeneratedBinding> bindings, LexicalFunctionId function);

public:
	FunctionParameters() = default;
	FunctionParameters(std::vector<Token> names, std::vector<BindingId> bindings, LexicalFunctionId function);
	static FunctionParameters Generated(std::vector<GeneratedBinding> bindings, LexicalFunctionId function);

	size_t Size() const;
	bool Empty() const;
	// The written parameter token, or a generated binding's source anchor.
	const Token& SourceToken(size_t index) const;
	// Null for source parameters. The index must be valid in either representation.
	const GeneratedBinding* GeneratedBindingAt(size_t index) const;
	const BindingId& BindingAt(size_t index) const;
	const LexicalFunctionId& Function() const;
	// Presentation only; generated bindings are resolved by identity.
	std::string DisplayName(size_t index) const;
};
