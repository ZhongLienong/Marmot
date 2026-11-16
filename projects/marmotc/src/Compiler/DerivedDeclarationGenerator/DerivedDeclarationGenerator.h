#pragma once

#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

struct DerivedDeclarationError
{
	std::string m_message;
	Token m_token;

	DerivedDeclarationError(std::string message, const Token& token);
};

class DerivedDeclarationGenerator
{
private:
	enum class Operation
	{
		Equatable,
		Hashable,
		Orderable,
		Convertable,
		Transferable,
		Map,
		Bind,
		Unwrap
	};

	using Declaration = std::variant<const MarmotStatement::Struct*, const MarmotStatement::Union*>;
	using StatementResult = std::expected<std::unique_ptr<MarmotStatement>, DerivedDeclarationError>;

	Declaration m_declaration;
	Operation m_operation;
	Token m_target;
	std::optional<Token> m_global_name;

	DerivedDeclarationGenerator(Declaration declaration, Operation operation, Token target, std::optional<Token> global_name);

	static std::optional<Operation> ResolveOperation(std::string_view name);
	static bool IsContainer(Operation operation);

	StatementResult Generate(const MarmotStatement::Struct& declaration) const;
	StatementResult Generate(const MarmotStatement::Union& declaration) const;

public:
	static std::expected<DerivedDeclarationGenerator, DerivedDeclarationError> Prepare(const MarmotStatement::Struct& declaration, const Token& target);
	static std::expected<DerivedDeclarationGenerator, DerivedDeclarationError> Prepare(const MarmotStatement::Union& declaration, const Token& target);

	// Registration precedes body validation, so name collisions keep their
	// diagnostic priority. The parser may qualify the name as it registers it.
	Token* NameToReserve();
	StatementResult Generate() const;
};
