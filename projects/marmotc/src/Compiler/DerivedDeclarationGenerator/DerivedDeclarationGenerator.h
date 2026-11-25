#pragma once

#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"
#include "Support/Expected.h"
#include "Support/Optional.h"

#include <memory>
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
		Convertible,
		Transferable,
		Map,
		Bind,
		Unwrap
	};

	using Declaration = std::variant<const MarmotStatement::Struct*, const MarmotStatement::Union*>;
	using StatementResult = MarmotCompiler::Expected<std::unique_ptr<MarmotStatement>, DerivedDeclarationError>;

	Declaration m_declaration;
	Operation m_operation;
	Token m_target;
	MarmotCompiler::Optional<Token> m_global_name;

	DerivedDeclarationGenerator(Declaration declaration, Operation operation, Token target, MarmotCompiler::Optional<Token> global_name);

	static MarmotCompiler::Optional<Operation> ResolveOperation(std::string_view name);
	static bool IsContainer(Operation operation);

	StatementResult Generate(const MarmotStatement::Struct& declaration, const OrderingType* ordering) const;
	StatementResult Generate(const MarmotStatement::Union& declaration, const OrderingType* ordering) const;

public:
	static MarmotCompiler::Expected<DerivedDeclarationGenerator, DerivedDeclarationError> Prepare(const MarmotStatement::Struct& declaration, const Token& target);
	static MarmotCompiler::Expected<DerivedDeclarationGenerator, DerivedDeclarationError> Prepare(const MarmotStatement::Union& declaration, const Token& target);

	// Registration precedes body validation, so name collisions keep their
	// diagnostic priority. The parser may qualify the name as it registers it.
	Token* NameToReserve();
	StatementResult Generate(const OrderingType* ordering) const;
};
