#include "FunctionLowering.h"
#include "Compiler/TypeTransform/TypeTransform.h"
#include "Bytecode/Intrinsics/IntrinsicTable.h"
#include "Compiler/Operator/Operator.h"
#include "Bytecode/Format/Format.h"
#include "Compiler/Names/SpecialNames.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

namespace
{
	using TypeRef = std::shared_ptr<MarmotType>;

	// The type a closure of a function with this signature has.
	TypeRef ClosureType(const TypeRef& function_type)
	{
		const MarmotType::FunctionType& signature = function_type->GetType<MarmotType::FunctionType>();
		return MarmotType::MakeFunctionType(signature.m_param_types, TypeRef(signature.m_return_type));
	}

	const TypeRef& UnitType()
	{
		return MarmotType::MakeLiteralType<MarmotType::UnitType>();
	}

	const TypeRef& TextType()
	{
		return MarmotType::MakeLiteralType<MarmotType::TextType>();
	}
}

LoweredExport::LoweredExport(std::string name, BytecodeModule::SymbolType kind, std::optional<MarmotIRGlobalSlot> slot, std::optional<MarmotIRFunctionId> function, Token token)
	: m_name(std::move(name)),
	m_kind(kind),
	m_slot(slot),
	m_function(function),
	m_token(std::move(token))
{
}

LoweredImport::LoweredImport(MarmotIRGlobalSlot slot, Token token)
	: m_slot(slot),
	m_token(std::move(token))
{
}

LoweredModule::LoweredModule(MarmotIRModule module)
	: m_module(std::move(module))
{
}

Lowering::Specialization::Specialization(GenericTypes::TypeEnvironment types, MethodResolutionMap methods, std::optional<std::string> source_module)
	: m_types(std::move(types)),
	m_methods(std::move(methods)),
	m_source_module(std::move(source_module))
{
}

Lowering::Lowering(MarmotProgramTree& program, std::string_view file_name, const std::vector<std::string>& source_lines, std::string module_name, const std::unordered_set<std::string>& export_symbols, const LoweringImports& imports)
	: m_program(program),
	m_file_name(file_name),
	m_source_lines(source_lines),
	m_module_name(module_name),
	m_export_symbols(export_symbols),
	m_module(module_name),
	m_generics(module_name, imports.m_generic_functions),
	m_resolver(imports.m_typeclasses)
{
	m_specializations.emplace_back(GenericTypes::TypeEnvironment{}, MethodResolutionMap{}, std::nullopt);
}

MarmotResult::DiagnosticsResult<LoweredModule> Lowering::Lower() &&
{
	const Specialization& module_context = m_specializations.front();
	const MarmotIRFunctionId top_level = NewFunction(std::string(MmcWire::MAIN_PROCEDURE_PREFIX), UnitType(), module_context);
	m_module.m_top_level = top_level;

	std::vector<CompilerError> reservation_errors = ReserveTopLevelNames();
	if (!reservation_errors.empty())
	{
		return std::unexpected(MarmotResult::CompilerDiagnostics(std::move(reservation_errors)));
	}

	FunctionLowering initializer(*this, top_level, module_context, nullptr, nullptr);
	const Emitted lowered = initializer.LowerTopLevel();
	if (!lowered.has_value())
	{
		return std::unexpected(MarmotResult::CompilerDiagnostics(lowered.error()));
	}

	std::ranges::move(m_functions, std::back_inserter(m_module.m_functions));
	LoweredModule result(std::move(m_module));
	result.m_exports = CollectExports();
	result.m_imports = std::move(m_import_tokens);
	result.m_generic_functions = std::move(m_generics).TakeAll();
	result.m_native_imports = std::move(m_native_imports);
	return result;
}

CompilerError Lowering::Unsupported(std::string_view construct, const Token& token) const
{
	return Error(CompilerErrorCode::LoweringUnsupportedConstruct, std::format("The MarmotIR backend cannot lower {} yet.", construct), token);
}

CompilerError Lowering::Error(CompilerErrorCode code, std::string_view message, const Token& token) const
{
	return MarmotError::GenerateLoweringErrorWithContext(code, message, token, m_file_name, m_source_lines);
}

CompilerError Lowering::ResolutionError(const MethodResolutionError& error, const Token& token) const
{
	return MarmotError::GenerateLoweringErrorWithContext(error.m_code, error.m_message, token.m_line, m_file_name, m_source_lines);
}

MarmotIRFunctionId Lowering::NewFunction(std::string name, TypeRef return_type, const Specialization& specialization)
{
	MarmotIRFunction& function = m_functions.emplace_back(std::move(name), std::move(return_type));
	function.m_source_module = specialization.m_source_module.value_or(std::string());
	return MarmotIRFunctionId{ static_cast<uint32_t>(m_functions.size() - 1u) };
}

// A projection such as Iterable::Item<S> means a type once S is one.
Lowering::TypeRef Lowering::Substitute(const TypeRef& type, const GenericTypes::TypeEnvironment& types) const
{
	return TypeTransform::Specialize(type, types, [this](const MarmotType::AssociatedType& projection) { return m_resolver.ResolveAssociatedType(projection); });
}

// A body may name a top-level definition that comes after it, so every one has
// its global, every function its MarmotIR function, every generic its template
// and every class and instance its entry before any body is lowered.
// Every name is reserved even after one fails, so each declaration the module
// cannot lower is reported, not only the first.
std::vector<CompilerError> Lowering::ReserveTopLevelNames()
{
	std::vector<CompilerError> errors;
	for (std::unique_ptr<MarmotStatement>& statement : m_program)
	{
		Emitted reserved = ReserveTopLevelName(*statement);
		if (!reserved.has_value())
		{
			errors.push_back(std::move(reserved.error()));
		}
	}
	return errors;
}

Lowering::Emitted Lowering::ReserveTopLevelName(MarmotStatement& statement)
{
	struct Reserver
	{
		Lowering& m_self;

		Emitted operator()(MarmotStatement::VariableDefinition& definition) const
		{
			if (definition.m_is_elided || definition.m_name.IsDiscard())
			{
				return {};
			}

			const std::string& name = definition.m_name.m_lexeme;
			if (!definition.m_value->IsExpression<MarmotExpression::Function>())
			{
				m_self.m_top_level_names.emplace(name, TopLevelName{ m_self.m_module.ReserveGlobal(name, definition.m_value->GetType()), std::nullopt });
				return {};
			}

			MarmotExpression::Function& function = definition.m_value->GetExpression<MarmotExpression::Function>();
			if (!function.m_generic_params.empty())
			{
				m_self.RegisterGeneric(name, function.m_params, function.m_param_types, function.m_generic_params, function.m_constraints, function.m_return_type, function.m_body);
				return {};
			}
			return m_self.ReserveFunction(name, ClosureType(definition.m_value->GetType()));
		}

		Emitted operator()(MarmotStatement::TupleDefinition& definition) const
		{
			const std::vector<TypeRef>& element_types = GenericTypes::RepresentationOf(definition.m_value->GetType())->GetType<MarmotType::TupleType>().m_element_types;
			for (size_t index = 0u; index < definition.m_names.size(); index += 1u)
			{
				if (definition.m_names[index].IsDiscard())
				{
					continue;
				}
				const std::string& name = definition.m_names[index].m_lexeme;
				m_self.m_top_level_names.emplace(name, TopLevelName{ m_self.m_module.ReserveGlobal(name, element_types[index]), std::nullopt });
			}
			return {};
		}

		Emitted operator()(MarmotStatement::FunctionDefinition& definition) const
		{
			if (!definition.m_generic_params.empty())
			{
				m_self.RegisterGeneric(definition.m_name.m_lexeme, definition.m_params, definition.m_param_types, definition.m_generic_params, definition.m_constraints, definition.m_return_type, definition.m_body);
				return {};
			}
			return m_self.ReserveFunction(definition.m_name.m_lexeme, MarmotType::MakeFunctionType(definition.m_param_types, TypeRef(definition.m_return_type)));
		}

		Emitted operator()(MarmotStatement::ForeignDefinition& foreign) const
		{
			if (foreign.DeclaredPrimitive().has_value())
			{
				return {};
			}
			return m_self.ReserveForeign(foreign);
		}

		Emitted operator()(MarmotStatement::Class& class_statement) const
		{
			std::unordered_set<std::string> method_names = class_statement.m_methods
				| std::views::filter([](const std::unique_ptr<MarmotStatement>& method) { return method->IsStatement<MarmotStatement::FunctionDefinition>(); })
				| std::views::transform([](const std::unique_ptr<MarmotStatement>& method) { return method->GetStatement<MarmotStatement::FunctionDefinition>().m_name.m_lexeme; })
				| std::ranges::to<std::unordered_set<std::string>>();
			m_self.m_resolver.AddClass(class_statement.m_name.m_lexeme, std::move(method_names));
			return {};
		}

		// A method of an instance over a generic head is a generic, specialized
		// where it is called; any other is an ordinary function.
		Emitted operator()(MarmotStatement::Instance& instance) const
		{
			TypeclassInstance::AssociatedTypes associated_types = instance.m_associated_types
				| std::views::transform([](const MarmotStatement::Instance::AssociatedTypeBinding& binding) { return std::pair{ binding.m_name.m_lexeme, binding.m_type }; })
				| std::ranges::to<TypeclassInstance::AssociatedTypes>();
			std::vector<std::string> methods = instance.m_methods
				| std::views::transform([](const MarmotStatement::Instance::Method& method) { return method.m_definition.m_name.m_lexeme; })
				| std::ranges::to<std::vector<std::string>>();
			m_self.m_resolver.AddInstance(instance.m_class_name.m_lexeme, TypeclassInstance(instance.m_type_args, std::move(associated_types), instance.m_constraints, std::move(methods), m_self.m_module_name));
			for (MarmotStatement::Instance::Method& instance_method : instance.m_methods)
			{
				MarmotStatement::FunctionDefinition& method = instance_method.m_definition;
				if (m_self.IsGenericMethod(instance, method))
				{
					m_self.RegisterGeneric(method.m_name.m_lexeme, method.m_params, method.m_param_types, method.m_generic_params, method.m_constraints, method.m_return_type, method.m_body);
					continue;
				}

				const Emitted reserved = m_self.ReserveFunction(method.m_name.m_lexeme, MarmotType::MakeFunctionType(method.m_param_types, TypeRef(method.m_return_type)));
				if (!reserved.has_value())
				{
					return reserved;
				}
			}
			return {};
		}

		Emitted operator()(MarmotStatement::ExpressionStatement&) const
		{
			return {};
		}

		Emitted operator()(MarmotStatement::Struct&) const
		{
			return {};
		}

		Emitted operator()(MarmotStatement::Union&) const
		{
			return {};
		}

		Emitted operator()(MarmotStatement::TypeAlias&) const
		{
			return {};
		}
	};

	return VisitNode(Reserver{ *this }, statement);
}

Lowering::Emitted Lowering::ReserveFunction(const std::string& name, const TypeRef& closure_type)
{
	const MarmotIRFunctionId function = NewFunction(name, closure_type->GetType<MarmotType::FunctionType>().m_return_type, m_specializations.front());
	m_top_level_names.emplace(name, TopLevelName{ m_module.ReserveGlobal(name, closure_type), function });
	return {};
}

// A foreign function's global holds the name the VM looks it up by, which is
// how another module calls it.
Lowering::Emitted Lowering::ReserveForeign(const MarmotStatement::ForeignDefinition& foreign)
{
	return ForeignName(foreign)
		.transform
		(
			[this, &foreign](const std::string&)
			{
				const std::string& name = foreign.m_function_name.m_lexeme;
				m_top_level_names.emplace(name, TopLevelName{ m_module.ReserveGlobal(name, TextType()), std::nullopt });
			}
		);
}

std::expected<std::string, CompilerError> Lowering::ForeignName(const MarmotStatement::ForeignDefinition& foreign)
{
	const Token& name = foreign.m_function_name;
	const MarmotType::FunctionType& signature = foreign.m_type->GetType<MarmotType::FunctionType>();
	const TypeRef& return_type = signature.m_return_type;
	const bool is_supported_return = return_type->IsType<MarmotType::IntegerType>() || return_type->IsType<MarmotType::FloatType>() || return_type->IsType<MarmotType::BoolType>() || return_type->IsType<MarmotType::UnitType>() || return_type->IsType<MarmotType::TextType>() || return_type->IsType<MarmotType::ArrayType>() || return_type->IsType<MarmotType::ByteType>() || return_type->IsType<MarmotType::WordType>();
	if (!is_supported_return && !(signature.m_intrinsic.has_value() && return_type->IsType<MarmotType::NeverType>()))
	{
		return std::unexpected(Error(CompilerErrorCode::LoweringUnsupportedConstruct, "Unsupported return type for foreign function", name));
	}

	// Native code builds an array out of bits, so it can return one of texts or of
	// scalars; one of other objects would hand the VM bits to take as pointers.
	if (foreign.m_library.has_value() && return_type->IsType<MarmotType::ArrayType>())
	{
		const TypeRef& element = return_type->GetType<MarmotType::ArrayType>().m_element_type;
		if (!element->IsType<MarmotType::TextType>() && GenericTypes::IsPointer(element))
		{
			return std::unexpected(Error(CompilerErrorCode::LoweringUnsupportedConstruct, std::format("A native function cannot return {}: native code can return an array of Text or of scalars, not of other values", return_type->DisplayString()), name));
		}
	}

	if (foreign.m_library.has_value())
	{
		m_native_imports[foreign.m_library.value()].insert(foreign.m_foreign_name);
		return foreign.m_library.value() + NATIVE_SYMBOL_SEPARATOR + foreign.m_foreign_name;
	}

	if (!signature.m_intrinsic.has_value())
	{
		return std::unexpected(Error(CompilerErrorCode::LoweringUnknownForeignFunction, std::format("Unknown foreign function '{}': it is not a Marmot builtin. Name the library that exports it: foreign \"{}\" ... from \"library\";", foreign.m_foreign_name, foreign.m_foreign_name), name));
	}
	const MarmotIntrinsicSignature& intrinsic = MarmotIntrinsics::At(signature.m_intrinsic.value());
	const MarmotType* element_type = nullptr;
	const auto matches = [&element_type](const TypeRef& actual, std::string_view expected)
	{
		constexpr std::string_view array_prefix = "Array<";
		const MarmotType* type = actual.get();
		while (expected.starts_with(array_prefix))
		{
			if (!type->IsType<MarmotType::ArrayType>())
			{
				return false;
			}
			type = type->GetType<MarmotType::ArrayType>().m_element_type.get();
			expected.remove_prefix(array_prefix.size());
			expected.remove_suffix(1u);
		}
		if (expected == "T")
		{
			if (element_type == nullptr)
			{
				element_type = type;
			}
			return *type == *element_type;
		}
		return type->ToString() == expected;
	};
	bool matches_signature = signature.m_param_types.size() == intrinsic.m_parameters.size();
	for (size_t index = 0; matches_signature && index < intrinsic.m_parameters.size(); index += 1u)
	{
		matches_signature = matches(signature.m_param_types[index], intrinsic.m_parameters[index]);
	}
	if (!matches_signature || !matches(return_type, intrinsic.m_result))
	{
		const std::string parameters = intrinsic.m_parameters | std::views::join_with(std::string_view(", ")) | std::ranges::to<std::string>();
		return std::unexpected(Error(CompilerErrorCode::LoweringUnsupportedConstruct, std::format("Intrinsic '{}' requires fn({}) -> {}", intrinsic.m_name, parameters, intrinsic.m_result), name));
	}
	return foreign.m_foreign_name;
}

// A generic's body moves out of this module's AST into its template, which the
// modules that import it specialize too.
void Lowering::RegisterGeneric(const std::string& name, const FunctionParameters& params, const std::vector<TypeRef>& param_types, const std::vector<Token>& generic_params, const std::vector<MarmotType::ClassConstraint>& constraints, const TypeRef& return_type, std::unique_ptr<MarmotExpression>& body)
{
	m_generics.Add(name, GenericFunctionInfo(name, params, param_types, generic_params, constraints, return_type, std::shared_ptr<const MarmotExpression>(std::move(body)), m_module_name));
}

// Every value in MarmotIR has one type, so a method of an instance whose head
// names a type parameter, `instance Indexable<Array<T>, Int>`, is specialized
// for each set of argument types it is called with, as a generic is.
bool Lowering::IsGenericMethod(const MarmotStatement::Instance& instance, const MarmotStatement::FunctionDefinition& method) const
{
	const bool intrinsic = method.m_body->IsExpression<MarmotExpression::Call>()
		&& method.m_body->GetExpression<MarmotExpression::Call>().m_primitive.has_value()
		&& PrimitiveOperator(method.m_body->GetExpression<MarmotExpression::Call>().m_primitive.value()) != nullptr;
	return intrinsic || !method.m_generic_params.empty() || GenericTypes::IsGenericInstanceHead(instance.m_type_args);
}

std::vector<LoweredExport> Lowering::CollectExports() const
{
	std::vector<LoweredExport> exports;
	const auto export_name = [&](const Token& token, BytecodeModule::SymbolType kind)
	{
		if (!m_export_symbols.contains(token.m_lexeme))
		{
			return;
		}
		const std::unordered_map<std::string, TopLevelName>::const_iterator name = m_top_level_names.find(token.m_lexeme);
		const std::optional<MarmotIRGlobalSlot> slot = name == m_top_level_names.cend() ? std::nullopt : std::optional<MarmotIRGlobalSlot>(name->second.m_slot);
		const std::optional<MarmotIRFunctionId> function = name == m_top_level_names.cend() ? std::nullopt : name->second.m_function;
		exports.emplace_back(token.m_lexeme, kind, slot, function, token);
	};

	for (const std::unique_ptr<MarmotStatement>& statement : m_program)
	{
		if (statement->IsStatement<MarmotStatement::VariableDefinition>())
		{
			const Token& name = statement->GetStatement<MarmotStatement::VariableDefinition>().m_name;
			if (!name.IsDiscard())
			{
				export_name(name, BytecodeModule::SymbolType::GLOBAL_VARIABLE);
			}
		}
		else if (statement->IsStatement<MarmotStatement::ForeignDefinition>() && !statement->GetStatement<MarmotStatement::ForeignDefinition>().DeclaredPrimitive().has_value())
		{
			export_name(statement->GetStatement<MarmotStatement::ForeignDefinition>().m_function_name, BytecodeModule::SymbolType::FOREIGN_FUNCTION);
		}
		else if (statement->IsStatement<MarmotStatement::FunctionDefinition>())
		{
			export_name(statement->GetStatement<MarmotStatement::FunctionDefinition>().m_name, BytecodeModule::SymbolType::FUNCTION);
		}
		else if (statement->IsStatement<MarmotStatement::Instance>())
		{
			for (const MarmotStatement::Instance::Method& method : statement->GetStatement<MarmotStatement::Instance>().m_methods)
			{
				export_name(method.m_definition.m_name, BytecodeModule::SymbolType::FUNCTION);
			}
		}
		else if (statement->IsStatement<MarmotStatement::Struct>())
		{
			export_name(statement->GetStatement<MarmotStatement::Struct>().m_name, BytecodeModule::SymbolType::STRUCT_TYPE);
		}
		else if (statement->IsStatement<MarmotStatement::Union>())
		{
			export_name(statement->GetStatement<MarmotStatement::Union>().m_name, BytecodeModule::SymbolType::UNION_TYPE);
		}
	}
	return exports;
}
