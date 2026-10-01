#include "Lowering.h"
#include "Bytecode/Builtins/BuiltinTable.h"
#include "Bytecode/Format/Format.h"
#include "Compiler/Constant/Constant.h"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <utility>

namespace
{
	using TypeRef = std::shared_ptr<MidoriType>;

	// Called like functions, but each is one instruction.
	constexpr std::array<std::string_view, 3u> s_intrinsic_calls =
	{
		"Concurrency::Close",
		"Concurrency::IsDone",
		"Concurrency::Cancel"
	};

	// The type a closure of a function with this signature has.
	TypeRef ClosureType(const TypeRef& function_type)
	{
		const MidoriType::FunctionType& signature = function_type->GetType<MidoriType::FunctionType>();
		return MidoriType::MakeFunctionType(signature.m_param_types, TypeRef(signature.m_return_type));
	}

	const TypeRef& UnitType()
	{
		return MidoriType::MakeLiteralType<MidoriType::UnitType>();
	}

	const TypeRef& TextType()
	{
		return MidoriType::MakeLiteralType<MidoriType::TextType>();
	}

	bool IsNever(const TypeRef& type)
	{
		return type->IsType<MidoriType::NeverType>();
	}

	const MidoriExpression::NameAccess* GlobalName(const MidoriExpression& expression)
	{
		if (!expression.IsExpression<MidoriExpression::NameAccess>())
		{
			return nullptr;
		}
		const MidoriExpression::NameAccess& name = expression.GetExpression<MidoriExpression::NameAccess>();
		return std::holds_alternative<MidoriExpression::NameContext::Global>(name.m_name_ctx) ? &name : nullptr;
	}
}

LoweredExport::LoweredExport(std::string name, BytecodeModule::SymbolType kind, std::optional<MidoriIRGlobalSlot> slot, std::optional<MidoriIRFunctionId> function, Token token)
	: m_name(std::move(name)),
	m_kind(kind),
	m_slot(slot),
	m_function(function),
	m_token(std::move(token))
{
}

LoweredImport::LoweredImport(MidoriIRGlobalSlot slot, Token token)
	: m_slot(slot),
	m_token(std::move(token))
{
}

LoweredModule::LoweredModule(MidoriIRModule module)
	: m_module(std::move(module))
{
}

Lowering::Specialization::Specialization(GenericTypes::TypeEnvironment types, MethodResolutionMap methods, std::optional<std::string> source_module, std::shared_ptr<const ForeignIndices> foreign_indices)
	: m_types(std::move(types)),
	m_methods(std::move(methods)),
	m_source_module(std::move(source_module)),
	m_foreign_indices(std::move(foreign_indices))
{
}

Lowering::JoinPoint::JoinPoint(MidoriIRBlockId block, TypeRef type)
	: m_block(block),
	m_type(std::move(type))
{
}

Lowering::FunctionScope::FunctionScope(MidoriIRFunctionId id, MidoriIRFunction& function, const Specialization& specialization, FunctionScope* parent, int environment_length)
	: m_id(id),
	m_function(function),
	m_builder(function),
	m_specialization(specialization),
	m_parent(parent),
	m_environment_length(environment_length),
	m_local_frames(1u),
	m_live_blocks(1u, true)
{
}

Lowering::Lowering(MidoriProgramTree& program, std::string_view file_name, const std::vector<std::string>& source_lines, std::string module_name, const std::unordered_set<std::string>& export_symbols, const LoweringImports& imports)
	: m_program(program),
	m_file_name(file_name),
	m_source_lines(source_lines),
	m_module_name(module_name),
	m_export_symbols(export_symbols),
	m_module(module_name),
	m_foreign_indices(std::make_shared<ForeignIndices>()),
	m_generics(module_name, imports.m_generic_functions),
	m_resolver(imports.m_class_methods, imports.m_class_instances, imports.m_class_instance_type_args, imports.m_class_instance_associated_types, [this](const std::string& name) { return m_top_level_names.contains(name); })
{
	m_specializations.emplace_back(GenericTypes::TypeEnvironment{}, MethodResolutionMap{}, std::nullopt, m_foreign_indices);
}

MidoriResult::DiagnosticsResult<LoweredModule> Lowering::Lower() &&
{
	const Specialization& module_context = m_specializations.front();
	const MidoriIRFunctionId top_level = NewFunction(std::string(MAIN_PROCEDURE_PREFIX), UnitType(), module_context);
	m_module.m_top_level = top_level;
	FunctionScope scope(top_level, m_functions[top_level.m_index], module_context, nullptr, 0);
	m_scopes.push_back(&scope);

	std::vector<CompilerError> reservation_errors = ReserveTopLevelNames();
	if (!reservation_errors.empty())
	{
		m_scopes.pop_back();
		return std::unexpected(MidoriResult::CompilerDiagnostics(std::move(reservation_errors)));
	}

	const Emitted lowered = [this]() -> Emitted
	{
		for (std::unique_ptr<MidoriStatement>& statement : m_program)
		{
			const Emitted statement_lowered = LowerTopLevelStatement(*statement);
			if (!statement_lowered.has_value())
			{
				return statement_lowered;
			}
		}
		Builder().AtLine(0).Return(Builder().ConstUnit());
		return {};
	}();
	m_scopes.pop_back();

	if (!lowered.has_value())
	{
		return std::unexpected(MidoriResult::CompilerDiagnostics(lowered.error()));
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
	return Error(CompilerErrorCode::LoweringUnsupportedConstruct, std::format("The MidoriIR backend cannot lower {} yet.", construct), token);
}

CompilerError Lowering::Error(CompilerErrorCode code, std::string_view message, const Token& token) const
{
	return MidoriError::GenerateLoweringErrorWithContext(code, message, token, m_file_name, m_source_lines);
}

CompilerError Lowering::ResolutionError(const MethodResolutionError& error, const Token& token) const
{
	return MidoriError::GenerateLoweringErrorWithContext(error.m_code, error.m_message, token.m_line, m_file_name, m_source_lines);
}

Lowering::FunctionScope& Lowering::Scope()
{
	return *m_scopes.back();
}

MidoriIRBuilder& Lowering::Builder()
{
	return Scope().m_builder;
}

MidoriIRFunctionId Lowering::NewFunction(std::string name, TypeRef return_type, const Specialization& specialization)
{
	MidoriIRFunction& function = m_functions.emplace_back(std::move(name), std::move(return_type));
	function.m_source_module = specialization.m_source_module.value_or(std::string());
	return MidoriIRFunctionId{ static_cast<uint32_t>(m_functions.size() - 1u) };
}

Lowering::TypeRef Lowering::Concrete(const TypeRef& type)
{
	const GenericTypes::TypeEnvironment& types = Scope().m_specialization.m_types;
	return types.empty() ? type : Substitute(type, types);
}

// A projection such as Iterable::Item<S> means a type once S is one.
Lowering::TypeRef Lowering::Substitute(const TypeRef& type, const GenericTypes::TypeEnvironment& types) const
{
	return GenericTypes::Substitute(type, types, [this](const MidoriType::AssociatedType& projection) { return m_resolver.ResolveAssociatedType(projection); });
}

Lowering::TypeRef Lowering::TypeOf(const MidoriExpression& expression)
{
	return Concrete(expression.GetType());
}

Lowering::TypeRef Lowering::NominalType(const MidoriExpression& expression, MidoriIRValueId value)
{
	const TypeRef type = TypeOf(expression);
	return !IsNever(type) && GenericTypes::IsConcrete(type) ? type : Scope().m_function.TypeOf(value);
}

MidoriIRBlockId Lowering::NewBlock()
{
	Scope().m_live_blocks.push_back(false);
	return Builder().CreateBlock();
}

bool Lowering::IsDead()
{
	return !Scope().m_live_blocks[Builder().CurrentBlock().m_index];
}

void Lowering::PositionAt(MidoriIRBlockId block)
{
	Builder().PositionAt(block);
}

void Lowering::Jump(MidoriIRBlockId target, std::vector<MidoriIRValueId> arguments)
{
	if (!IsDead())
	{
		Scope().m_live_blocks[target.m_index] = true;
	}
	Builder().Jump(target, std::move(arguments));
}

void Lowering::Branch(MidoriIRValueId condition, MidoriIRSuccessor if_true, MidoriIRSuccessor if_false)
{
	if (!IsDead())
	{
		Scope().m_live_blocks[if_true.m_block.m_index] = true;
		Scope().m_live_blocks[if_false.m_block.m_index] = true;
	}
	Builder().Branch(condition, std::move(if_true), std::move(if_false));
}

MidoriIRValueId Lowering::Placeholder(const TypeRef& type)
{
	return Builder().Emit(MidoriIROp::Const, type);
}

void Lowering::AfterNever()
{
	Builder().Unreachable();
	PositionAt(NewBlock());
}

// A body may name a top-level definition that comes after it, so every one has
// its global, every function its MidoriIR function, every generic its template
// and every class and instance its entry before any body is lowered.
// Every name is reserved even after one fails, so each declaration the module
// cannot lower is reported, not only the first.
std::vector<CompilerError> Lowering::ReserveTopLevelNames()
{
	std::vector<CompilerError> errors;
	for (std::unique_ptr<MidoriStatement>& statement : m_program)
	{
		Emitted reserved = ReserveTopLevelName(*statement);
		if (!reserved.has_value())
		{
			errors.push_back(std::move(reserved.error()));
		}
	}
	return errors;
}

Lowering::Emitted Lowering::ReserveTopLevelName(MidoriStatement& statement)
{
	struct Reserver
	{
		Lowering& m_self;

		Emitted operator()(MidoriStatement::VariableDefinition& definition) const
		{
			if (definition.m_is_elided)
			{
				return {};
			}

			const std::string& name = definition.m_name.m_lexeme;
			if (!definition.m_value->IsExpression<MidoriExpression::Function>())
			{
				m_self.m_top_level_names.emplace(name, TopLevelName{ m_self.m_module.ReserveGlobal(name, definition.m_value->GetType()), std::nullopt });
				return {};
			}

			MidoriExpression::Function& function = definition.m_value->GetExpression<MidoriExpression::Function>();
			if (!function.m_generic_params.empty())
			{
				m_self.RegisterGeneric(name, function.m_params, function.m_param_types, function.m_generic_params, function.m_constraints, function.m_return_type, function.m_body, function.m_captured_count);
				return {};
			}
			return m_self.ReserveFunction(name, ClosureType(definition.m_value->GetType()));
		}

		Emitted operator()(MidoriStatement::TupleDefinition& definition) const
		{
			const std::vector<TypeRef>& element_types = GenericTypes::RepresentationOf(definition.m_value->GetType())->GetType<MidoriType::TupleType>().m_element_types;
			for (size_t index = 0u; index < definition.m_names.size(); index += 1u)
			{
				const std::string& name = definition.m_names[index].m_lexeme;
				m_self.m_top_level_names.emplace(name, TopLevelName{ m_self.m_module.ReserveGlobal(name, element_types[index]), std::nullopt });
			}
			return {};
		}

		Emitted operator()(MidoriStatement::FunctionDefinition& definition) const
		{
			if (!definition.m_generic_params.empty())
			{
				m_self.RegisterGeneric(definition.m_name.m_lexeme, definition.m_params, definition.m_param_types, definition.m_generic_params, definition.m_constraints, definition.m_return_type, definition.m_body, definition.m_captured_count);
				return {};
			}
			return m_self.ReserveFunction(definition.m_name.m_lexeme, MidoriType::MakeFunctionType(definition.m_param_types, TypeRef(definition.m_return_type)));
		}

		Emitted operator()(MidoriStatement::ForeignDefinition& foreign) const
		{
			return m_self.ReserveForeign(foreign);
		}

		Emitted operator()(MidoriStatement::Class& class_statement) const
		{
			std::unordered_set<std::string> method_names = class_statement.m_methods
				| std::views::filter([](const std::unique_ptr<MidoriStatement>& method) { return method->IsStatement<MidoriStatement::FunctionDefinition>(); })
				| std::views::transform([](const std::unique_ptr<MidoriStatement>& method) { return method->GetStatement<MidoriStatement::FunctionDefinition>().m_name.m_lexeme; })
				| std::ranges::to<std::unordered_set<std::string>>();
			m_self.m_resolver.AddClass(class_statement.m_name.m_lexeme, std::move(method_names));
			return {};
		}

		// A method of an instance over a generic head is a generic, specialized
		// where it is called; any other is an ordinary function.
		Emitted operator()(MidoriStatement::Instance& instance) const
		{
			m_self.m_resolver.AddInstanceTypeArgs(instance.m_class_name.m_lexeme, instance.m_type_args);
			m_self.m_resolver.AddAssociatedTypes(instance.m_class_name.m_lexeme, instance.m_type_args, instance.m_associated_types
				| std::views::transform([](const MidoriStatement::Instance::AssociatedTypeBinding& binding) { return std::pair{ binding.m_name.m_lexeme, binding.m_type }; })
				| std::ranges::to<InstanceResolver::AssociatedTypeBindings>());
			for (std::unique_ptr<MidoriStatement>& statement : instance.m_methods)
			{
				if (!statement->IsStatement<MidoriStatement::FunctionDefinition>())
				{
					continue;
				}

				MidoriStatement::FunctionDefinition& method = statement->GetStatement<MidoriStatement::FunctionDefinition>();
				m_self.m_resolver.AddInstanceMethod(instance.m_class_name.m_lexeme, method.m_name.m_lexeme);
				if (m_self.IsGenericMethod(instance, method))
				{
					m_self.RegisterGeneric(method.m_name.m_lexeme, method.m_params, method.m_param_types, method.m_generic_params, method.m_constraints, method.m_return_type, method.m_body, method.m_captured_count);
					continue;
				}

				const Emitted reserved = m_self.ReserveFunction(method.m_name.m_lexeme, MidoriType::MakeFunctionType(method.m_param_types, TypeRef(method.m_return_type)));
				if (!reserved.has_value())
				{
					return reserved;
				}
			}
			return {};
		}

		Emitted operator()(MidoriStatement::ExpressionStatement&) const
		{
			return {};
		}

		Emitted operator()(MidoriStatement::Struct&) const
		{
			return {};
		}

		Emitted operator()(MidoriStatement::Union&) const
		{
			return {};
		}

		Emitted operator()(MidoriStatement::TypeAlias&) const
		{
			return {};
		}
	};

	return VisitNode(Reserver{ *this }, statement);
}

Lowering::Emitted Lowering::ReserveFunction(const std::string& name, const TypeRef& closure_type)
{
	const MidoriIRFunctionId function = NewFunction(name, closure_type->GetType<MidoriType::FunctionType>().m_return_type, m_specializations.front());
	m_top_level_names.emplace(name, TopLevelName{ m_module.ReserveGlobal(name, closure_type), function });
	return {};
}

// A foreign function's global holds the name the VM looks it up by, which is
// how another module calls it.
Lowering::Emitted Lowering::ReserveForeign(const MidoriStatement::ForeignDefinition& foreign)
{
	return ForeignName(foreign)
		.transform([this, &foreign](const std::string&)
		{
			const std::string& name = foreign.m_function_name.m_lexeme;
			m_top_level_names.emplace(name, TopLevelName{ m_module.ReserveGlobal(name, TextType()), std::nullopt });
		});
}

// A builtin's own name, or its library's and its symbol's. A builtin is also
// recorded by the name the module declares it as, so a call of it is one by
// index.
std::expected<std::string, CompilerError> Lowering::ForeignName(const MidoriStatement::ForeignDefinition& foreign)
{
	const Token& name = foreign.m_function_name;
	const TypeRef& return_type = foreign.m_type->GetType<MidoriType::FunctionType>().m_return_type;
	const bool is_supported_return = return_type->IsType<MidoriType::IntegerType>() || return_type->IsType<MidoriType::FloatType>() || return_type->IsType<MidoriType::BoolType>() || return_type->IsType<MidoriType::UnitType>() || return_type->IsType<MidoriType::TextType>() || return_type->IsType<MidoriType::ArrayType>() || return_type->IsType<MidoriType::ByteType>() || return_type->IsType<MidoriType::WordType>();
	if (!is_supported_return)
	{
		return std::unexpected(Error(CompilerErrorCode::LoweringUnsupportedConstruct, "Unsupported return type for foreign function", name));
	}

	if (foreign.m_library.has_value())
	{
		m_native_imports[foreign.m_library.value()].insert(foreign.m_foreign_name);
		return foreign.m_library.value() + NATIVE_SYMBOL_SEPARATOR + foreign.m_foreign_name;
	}

	const std::optional<size_t> builtin = MarmotBuiltins::FindIndex(foreign.m_foreign_name);
	if (!builtin.has_value())
	{
		return std::unexpected(Error(CompilerErrorCode::LoweringUnknownForeignFunction, std::format("Unknown foreign function '{}': it is not a Marmot builtin. Name the library that exports it: foreign \"{}\" ... from \"library\";", foreign.m_foreign_name, foreign.m_foreign_name), name));
	}
	m_foreign_indices->insert_or_assign(name.m_lexeme, builtin.value());
	return foreign.m_foreign_name;
}

// A generic's body moves out of this module's AST into its template, which the
// modules that import it specialize too.
void Lowering::RegisterGeneric(const std::string& name, const std::vector<Token>& params, const std::vector<TypeRef>& param_types, const std::vector<Token>& generic_params, const std::vector<MidoriType::ClassConstraint>& constraints, const TypeRef& return_type, std::unique_ptr<MidoriExpression>& body, int captured_count)
{
	m_generics.Add(name, GenericFunctionInfo(name, params, param_types, generic_params, constraints, return_type, std::shared_ptr<MidoriExpression>(std::move(body)), captured_count, m_module_name, m_foreign_indices));
}

// Every value in MidoriIR has one type, so a method of an instance whose head
// names a type parameter, `instance Indexable<Array<T>, Int>`, is specialized
// for each set of argument types it is called with, as a generic is.
bool Lowering::IsGenericMethod(const MidoriStatement::Instance& instance, const MidoriStatement::FunctionDefinition& method) const
{
	return !method.m_generic_params.empty() || GenericTypes::IsGenericInstanceHead(instance.m_type_args);
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
		const std::optional<MidoriIRGlobalSlot> slot = name == m_top_level_names.cend() ? std::nullopt : std::optional<MidoriIRGlobalSlot>(name->second.m_slot);
		const std::optional<MidoriIRFunctionId> function = name == m_top_level_names.cend() ? std::nullopt : name->second.m_function;
		exports.emplace_back(token.m_lexeme, kind, slot, function, token);
	};

	for (const std::unique_ptr<MidoriStatement>& statement : m_program)
	{
		if (statement->IsStatement<MidoriStatement::VariableDefinition>())
		{
			export_name(statement->GetStatement<MidoriStatement::VariableDefinition>().m_name, BytecodeModule::SymbolType::GLOBAL_VARIABLE);
		}
		else if (statement->IsStatement<MidoriStatement::ForeignDefinition>())
		{
			export_name(statement->GetStatement<MidoriStatement::ForeignDefinition>().m_function_name, BytecodeModule::SymbolType::FOREIGN_FUNCTION);
		}
		else if (statement->IsStatement<MidoriStatement::FunctionDefinition>())
		{
			export_name(statement->GetStatement<MidoriStatement::FunctionDefinition>().m_name, BytecodeModule::SymbolType::FUNCTION);
		}
		else if (statement->IsStatement<MidoriStatement::Instance>())
		{
			for (const std::unique_ptr<MidoriStatement>& method : statement->GetStatement<MidoriStatement::Instance>().m_methods)
			{
				if (method->IsStatement<MidoriStatement::FunctionDefinition>())
				{
					export_name(method->GetStatement<MidoriStatement::FunctionDefinition>().m_name, BytecodeModule::SymbolType::FUNCTION);
				}
			}
		}
		else if (statement->IsStatement<MidoriStatement::Struct>())
		{
			export_name(statement->GetStatement<MidoriStatement::Struct>().m_name, BytecodeModule::SymbolType::STRUCT_TYPE);
		}
		else if (statement->IsStatement<MidoriStatement::Union>())
		{
			export_name(statement->GetStatement<MidoriStatement::Union>().m_name, BytecodeModule::SymbolType::UNION_TYPE);
		}
	}
	return exports;
}

Lowering::Emitted Lowering::LowerTopLevelStatement(MidoriStatement& statement)
{
	if (statement.IsStatement<MidoriStatement::ForeignDefinition>())
	{
		const MidoriStatement::ForeignDefinition& foreign = statement.GetStatement<MidoriStatement::ForeignDefinition>();
		const std::string value = foreign.m_library.has_value() ? foreign.m_library.value() + NATIVE_SYMBOL_SEPARATOR + foreign.m_foreign_name : foreign.m_foreign_name;
		Builder().AtLine(foreign.m_function_name.m_line);
		Builder().Emit(MidoriIROp::GlobalDefine, UnitType(), { Builder().ConstText(value) }, m_top_level_names.at(foreign.m_function_name.m_lexeme).m_slot);
		return {};
	}

	if (statement.IsStatement<MidoriStatement::FunctionDefinition>())
	{
		MidoriStatement::FunctionDefinition& definition = statement.GetStatement<MidoriStatement::FunctionDefinition>();
		if (!definition.m_generic_params.empty())
		{
			return {};
		}
		return LowerTopLevelFunction(definition.m_name.m_lexeme, definition.m_params, *definition.m_body, definition.m_name.m_line);
	}

	if (statement.IsStatement<MidoriStatement::Instance>())
	{
		MidoriStatement::Instance& instance = statement.GetStatement<MidoriStatement::Instance>();
		for (std::unique_ptr<MidoriStatement>& method_statement : instance.m_methods)
		{
			if (!method_statement->IsStatement<MidoriStatement::FunctionDefinition>())
			{
				continue;
			}
			MidoriStatement::FunctionDefinition& method = method_statement->GetStatement<MidoriStatement::FunctionDefinition>();
			if (IsGenericMethod(instance, method))
			{
				continue;
			}
			const Emitted lowered = LowerTopLevelFunction(method.m_name.m_lexeme, method.m_params, *method.m_body, method.m_name.m_line);
			if (!lowered.has_value())
			{
				return lowered;
			}
		}
		return {};
	}

	if (statement.IsStatement<MidoriStatement::Class>())
	{
		return {};
	}

	if (statement.IsStatement<MidoriStatement::TupleDefinition>())
	{
		return LowerTupleDefinition(statement.GetStatement<MidoriStatement::TupleDefinition>());
	}

	if (!statement.IsStatement<MidoriStatement::VariableDefinition>())
	{
		return LowerStatement(statement);
	}

	MidoriStatement::VariableDefinition& definition = statement.GetStatement<MidoriStatement::VariableDefinition>();
	if (definition.m_is_elided)
	{
		return {};
	}

	const std::unordered_map<std::string, TopLevelName>::const_iterator name = m_top_level_names.find(definition.m_name.m_lexeme);
	if (name == m_top_level_names.cend())
	{
		return {};
	}

	const int line = definition.m_name.m_line;
	if (name->second.m_function.has_value())
	{
		MidoriExpression::Function& function = definition.m_value->GetExpression<MidoriExpression::Function>();
		return LowerTopLevelFunction(definition.m_name.m_lexeme, function.m_params, *function.m_body, line);
	}

	const MidoriIRGlobalSlot slot = name->second.m_slot;
	return Lower(*definition.m_value)
		.transform([this, slot, line](MidoriIRValueId value)
		{
			Builder().AtLine(line).Emit(MidoriIROp::GlobalDefine, UnitType(), { value }, slot);
		});
}

Lowering::Emitted Lowering::LowerTopLevelFunction(const std::string& name, const std::vector<Token>& params, MidoriExpression& body, int line)
{
	const TopLevelName& top_level_name = m_top_level_names.at(name);
	const TypeRef closure_type = m_module.m_globals[top_level_name.m_slot.m_value].m_type;
	const MidoriIRFunctionId id = top_level_name.m_function.value();
	FunctionScope scope(id, m_functions[id.m_index], m_specializations.front(), &Scope(), 0);
	return LowerFunctionBody(scope, closure_type->GetType<MidoriType::FunctionType>().m_param_types, params, body)
		.and_then([&]() { return MakeClosure(scope, closure_type, line, name); })
		.transform([this, &top_level_name, line](MidoriIRValueId closure)
		{
			Builder().AtLine(line).Emit(MidoriIROp::GlobalDefine, UnitType(), { closure }, top_level_name.m_slot);
		});
}

Lowering::Emitted Lowering::LowerStatement(MidoriStatement& statement)
{
	struct StatementLowering
	{
		Lowering& m_self;

		Emitted operator()(MidoriStatement::ExpressionStatement& expression_statement) const
		{
			return m_self.Lower(*expression_statement.m_expr).transform([](MidoriIRValueId) {});
		}

		Emitted operator()(MidoriStatement::VariableDefinition& definition) const
		{
			return m_self.LowerLocalDefinition(definition);
		}

		Emitted operator()(MidoriStatement::TupleDefinition& definition) const
		{
			return m_self.LowerTupleDefinition(definition);
		}

		Emitted operator()(MidoriStatement::FunctionDefinition& definition) const
		{
			return std::unexpected(m_self.Unsupported("a local function definition", definition.m_name));
		}

		Emitted operator()(MidoriStatement::ForeignDefinition& foreign) const
		{
			return m_self.ForeignName(foreign)
				.transform([this, &foreign](const std::string& value)
				{
					m_self.DefineLocal(foreign.m_local_index.value(), m_self.Builder().AtLine(foreign.m_function_name.m_line).ConstText(value));
				});
		}

		// Only at the top level.
		Emitted operator()(MidoriStatement::Class&) const
		{
			std::unreachable();
		}

		Emitted operator()(MidoriStatement::Instance&) const
		{
			std::unreachable();
		}

		Emitted operator()(MidoriStatement::Struct&) const
		{
			return {};
		}

		Emitted operator()(MidoriStatement::Union&) const
		{
			return {};
		}

		Emitted operator()(MidoriStatement::TypeAlias&) const
		{
			return {};
		}
	};

	return VisitNode(StatementLowering{ *this }, statement);
}

Lowering::Emitted Lowering::LowerLocalDefinition(MidoriStatement::VariableDefinition& definition)
{
	if (definition.m_is_elided)
	{
		return {};
	}

	const int local_index = definition.m_local_index.value();
	return Lower(*definition.m_value)
		.transform([this, local_index](MidoriIRValueId value)
		{
			DefineLocal(local_index, value);
		});
}

// Each name takes its element of the tuple, as a local or, at the top level,
// as a global.
Lowering::Emitted Lowering::LowerTupleDefinition(MidoriStatement::TupleDefinition& definition)
{
	const int line = definition.m_names.front().m_line;
	return Lower(*definition.m_value)
		.transform([this, &definition, line](MidoriIRValueId tuple)
		{
			const bool is_dead = IsDead();
			Builder().AtLine(line);
			for (size_t index = 0u; index < definition.m_names.size(); index += 1u)
			{
				const MidoriIRValueId element = is_dead
					? Placeholder(UnitType())
					: Builder().Emit(MidoriIROp::TupleGet, GenericTypes::RepresentationOf(Scope().m_function.TypeOf(tuple))->GetType<MidoriType::TupleType>().m_element_types[index], { tuple }, MidoriIRIndex{ static_cast<uint32_t>(index) }, definition.m_names[index].m_lexeme);
				const std::optional<int>& local_index = definition.m_local_indices[index];
				if (local_index.has_value())
				{
					DefineLocal(local_index.value(), element);
				}
				else if (!is_dead)
				{
					Builder().Emit(MidoriIROp::GlobalDefine, UnitType(), { element }, m_top_level_names.at(definition.m_names[index].m_lexeme).m_slot);
				}
			}
		});
}

void Lowering::PushLocalFrame()
{
	Scope().m_local_frames.emplace_back();
}

void Lowering::PopLocalFrame()
{
	FunctionScope& scope = Scope();
	for (const int index : scope.m_local_frames.back())
	{
		scope.m_locals.erase(index);
	}
	scope.m_local_frames.pop_back();
}

// A closure made before this local, which captures it, gets it now.
void Lowering::DefineLocal(int index, MidoriIRValueId value)
{
	FunctionScope& scope = Scope();
	scope.m_locals.insert_or_assign(index, value);
	scope.m_local_frames.back().push_back(index);

	std::erase_if(scope.m_pending_captures, [&](const PendingCapture& pending)
	{
		if (pending.m_local != index)
		{
			return false;
		}
		Builder().Emit(MidoriIROp::BindCaptures, UnitType(), { pending.m_closure, value }, MidoriIRIndex{ pending.m_capture });
		return true;
	});
}

// The parser numbers what a lambda captures as the VM used to lay out its
// environment: its parent's environment first, then its parent's locals.
Lowering::LocalKey Lowering::KeyForCell(const FunctionScope& scope, int cell_index) const
{
	const FunctionScope& parent = *scope.m_parent;
	return cell_index < parent.m_environment_length
		? KeyForCell(parent, cell_index)
		: LocalKey{ &parent, cell_index - parent.m_environment_length };
}

// A capture is read once, at the top of the function, so its value dominates
// every use.
MidoriIRValueId Lowering::CaptureFor(FunctionScope& scope, const LocalKey& key, const TypeRef& type)
{
	const std::vector<LocalKey>::const_iterator found = std::ranges::find(scope.m_captures, key);
	if (found != scope.m_captures.cend())
	{
		return scope.m_capture_values[static_cast<size_t>(std::distance(scope.m_captures.cbegin(), found))];
	}

	const uint32_t index = static_cast<uint32_t>(scope.m_captures.size());
	MidoriIRFunction& function = scope.m_function;
	function.m_values.emplace_back(type, std::string());
	const MidoriIRValueId value{ static_cast<uint32_t>(function.m_values.size() - 1u) };
	std::vector<MidoriIRInstruction>& entry = function.Block(MidoriIRFunction::s_entry_block).m_instructions;
	entry.emplace(entry.begin() + index, MidoriIRInstruction(MidoriIROp::GetCapture, value, type, {}, MidoriIRIndex{ index }, {}, 0));

	function.m_capture_types.push_back(type);
	scope.m_captures.push_back(key);
	scope.m_capture_values.push_back(value);
	return value;
}

// Parameters are the parser's first locals, so parameter i is local i.
Lowering::Emitted Lowering::LowerFunctionBody(FunctionScope& scope, const std::vector<TypeRef>& param_types, const std::vector<Token>& params, MidoriExpression& body)
{
	for (size_t index = 0u; index < params.size(); index += 1u)
	{
		const MidoriIRValueId parameter = scope.m_builder.AddParameter(MidoriIRFunction::s_entry_block, param_types[index], params[index].m_lexeme);
		scope.m_locals.emplace(static_cast<int>(index), parameter);
		scope.m_local_frames.back().push_back(static_cast<int>(index));
	}

	m_scopes.push_back(&scope);
	const Emitted lowered = LowerReturn(body);
	m_scopes.pop_back();
	return lowered;
}

// Each capture is a value of the enclosing function: its local, or one it
// captures in turn. A local not defined yet, which a local function that names
// itself or a later one captures, goes last, and is bound once it is defined.
Lowering::Lowered Lowering::MakeClosure(FunctionScope& child, const TypeRef& closure_type, int line, std::string name)
{
	FunctionScope& parent = Scope();
	std::vector<size_t> order;
	std::vector<size_t> late;
	std::vector<MidoriIRValueId> operands;
	for (size_t index = 0u; index < child.m_captures.size(); index += 1u)
	{
		const LocalKey& key = child.m_captures[index];
		if (key.m_owner != &parent)
		{
			operands.push_back(CaptureFor(parent, key, child.m_function.m_capture_types[index]));
			order.push_back(index);
			continue;
		}

		const std::unordered_map<int, MidoriIRValueId>::const_iterator local = parent.m_locals.find(key.m_index);
		if (local == parent.m_locals.cend())
		{
			late.push_back(index);
			continue;
		}
		operands.push_back(local->second);
		order.push_back(index);
	}
	order.insert(order.end(), late.begin(), late.end());

	std::vector<uint32_t> position(order.size());
	for (size_t index = 0u; index < order.size(); index += 1u)
	{
		position[order[index]] = static_cast<uint32_t>(index);
	}
	child.m_function.m_capture_types = order
		| std::views::transform([&child](size_t index) { return child.m_function.m_capture_types[index]; })
		| std::ranges::to<std::vector>();
	for (MidoriIRInstruction& instruction : child.m_function.Block(MidoriIRFunction::s_entry_block).m_instructions)
	{
		if (instruction.m_op == MidoriIROp::GetCapture)
		{
			MidoriIRIndex& capture = std::get<MidoriIRIndex>(instruction.m_immediate);
			capture.m_value = position[capture.m_value];
		}
	}

	const MidoriIRValueId closure = Builder().AtLine(line).Emit(MidoriIROp::MakeClosure, closure_type, std::move(operands), child.m_id, std::move(name));
	for (const size_t index : late)
	{
		parent.m_pending_captures.push_back(PendingCapture{ closure, position[index], child.m_captures[index].m_index });
	}
	return closure;
}

Lowering::Lowered Lowering::LowerFunction(MidoriExpression::Function& function)
{
	const int line = function.m_function_keyword.m_line;
	const TypeRef closure_type = ClosureType(Concrete(function.m_type_data));
	const MidoriType::FunctionType& signature = closure_type->GetType<MidoriType::FunctionType>();
	const MidoriIRFunctionId id = NewFunction(std::format("Anonymous Function at line: {}", line), signature.m_return_type, Scope().m_specialization);
	FunctionScope scope(id, m_functions[id.m_index], Scope().m_specialization, &Scope(), function.m_captured_count);
	return LowerFunctionBody(scope, signature.m_param_types, function.m_params, *function.m_body)
		.and_then([&]() { return MakeClosure(scope, closure_type, line); });
}

// One function per generic and set of argument types. Its types come from the
// arguments', and from the call's where only the result names a type parameter.
std::expected<MidoriIRFunctionId, CompilerError> Lowering::Specialize(const std::string& key, const std::vector<TypeRef>& argument_types, const TypeRef& call_type)
{
	const GenericFunctionInfo& info = m_generics.At(key);
	GenericTypes::TypeEnvironment types;
	GenericTypes::Deduce(info.m_generic_return_type, call_type, types);
	for (size_t index = 0u; index < info.m_param_types.size() && index < argument_types.size(); index += 1u)
	{
		GenericTypes::Deduce(info.m_param_types[index], argument_types[index], types);
	}
	// `where Iterable::Item<S> ~ A` is what says A, once S is known.
	for (const MidoriType::ClassConstraint& constraint : info.m_constraints)
	{
		if (!constraint.IsEquality())
		{
			continue;
		}
		const TypeRef projected = Substitute(constraint.m_equality_lhs, types);
		if (GenericTypes::IsConcrete(projected))
		{
			GenericTypes::Deduce(constraint.m_equality_rhs, projected, types);
		}
	}

	const std::string signature = std::format("{}<{}>", key, argument_types
		| std::views::transform([](const TypeRef& type) { return type->ToString(); })
		| std::views::join_with(',')
		| std::ranges::to<std::string>());
	const TypeRef declared_return = Substitute(info.m_generic_return_type, types);
	const TypeRef return_type = GenericTypes::IsConcrete(declared_return) ? declared_return : call_type;
	const std::string cache_key = std::format("{}->{}", signature, return_type->ToString());
	const std::unordered_map<std::string, MidoriIRFunctionId>::const_iterator existing = m_specialized_functions.find(cache_key);
	if (existing != m_specialized_functions.cend())
	{
		return existing->second;
	}
	const std::unordered_map<std::string, MidoriIRFunctionId>::const_iterator by_arguments = m_specializations_by_arguments.find(signature);
	if (!GenericTypes::IsConcrete(return_type) && by_arguments != m_specializations_by_arguments.cend())
	{
		return by_arguments->second;
	}

	const bool is_foreign = !info.m_defining_module.empty() && info.m_defining_module != m_module_name;
	const std::optional<std::string> source_module = is_foreign ? std::optional<std::string>(info.m_defining_module) : std::nullopt;
	const std::shared_ptr<const ForeignIndices> foreign_indices = is_foreign ? info.m_builtin_foreign_indices : m_foreign_indices;
	MethodResolutionMap methods = m_resolver.ResolveConstraints(info.m_constraints, types);
	const Specialization& specialization = m_specializations.emplace_back(std::move(types), std::move(methods), source_module, foreign_indices);

	const MidoriIRFunctionId id = NewFunction(signature, return_type, specialization);
	m_specialized_functions.emplace(cache_key, id);
	m_specializations_by_arguments.emplace(signature, id);
	FunctionScope scope(id, m_functions[id.m_index], specialization, nullptr, info.m_captured_count);
	const std::shared_ptr<MidoriExpression> body = info.m_body;
	const std::vector<Token> params = info.m_params;
	return LowerFunctionBody(scope, argument_types, params, *body)
		.transform([id]() { return id; });
}

bool Lowering::IsForeignFunction(const TypeRef& type) const
{
	return type->IsType<MidoriType::FunctionType>() && type->GetType<MidoriType::FunctionType>().m_is_foreign;
}

// A foreign declaration's global holds its name: this module's, one another
// module declared, or, inside a specialization, the defining module's.
bool Lowering::IsForeignDeclaration(const std::string& name)
{
	const std::unordered_map<std::string, TopLevelName>::const_iterator own = m_top_level_names.find(name);
	const bool is_own = !Scope().m_specialization.m_source_module.has_value() && own != m_top_level_names.cend();
	return !is_own || m_module.m_globals[own->second.m_slot.m_value].m_type->IsType<MidoriType::TextType>();
}

// A foreign function used as a value is a function that calls it.
Lowering::Lowered Lowering::LowerForeignValue(const Token& name, const TypeRef& type)
{
	const TypeRef closure_type = ClosureType(type);
	const MidoriType::FunctionType& signature = closure_type->GetType<MidoriType::FunctionType>();
	const MidoriIRFunctionId id = NewFunction(std::format("foreign {}", name.m_lexeme), signature.m_return_type, Scope().m_specialization);
	FunctionScope scope(id, m_functions[id.m_index], Scope().m_specialization, &Scope(), 0);
	std::vector<MidoriIRValueId> arguments;
	for (const TypeRef& parameter_type : signature.m_param_types)
	{
		arguments.push_back(scope.m_builder.AddParameter(MidoriIRFunction::s_entry_block, parameter_type));
	}

	m_scopes.push_back(&scope);
	Builder().AtLine(name.m_line);
	const ForeignIndices::const_iterator builtin = Scope().m_specialization.m_foreign_indices->find(name.m_lexeme);
	const Emitted lowered = builtin != Scope().m_specialization.m_foreign_indices->cend()
		? Emitted()
		: GlobalFor(name, TextType()).transform([this, &arguments](MidoriIRGlobalSlot slot)
		{
			arguments.insert(arguments.begin(), Builder().Emit(MidoriIROp::GlobalGet, TextType(), {}, slot));
		});
	if (lowered.has_value())
	{
		const MidoriIRImmediate foreign = builtin != Scope().m_specialization.m_foreign_indices->cend() ? MidoriIRImmediate(MidoriIRForeign{ std::string(MarmotBuiltins::At(builtin->second)) }) : MidoriIRImmediate();
		const MidoriIRValueId result = Builder().Emit(MidoriIROp::CallForeign, signature.m_return_type, std::move(arguments), foreign);
		Builder().Return(result);
	}
	m_scopes.pop_back();
	if (!lowered.has_value())
	{
		return std::unexpected(lowered.error());
	}
	return MakeClosure(scope, closure_type, name.m_line);
}

Lowering::Lowered Lowering::LowerName(const MidoriExpression::NameAccess& name)
{
	const Token& token = name.m_name;
	if (std::holds_alternative<MidoriExpression::NameContext::Local>(name.m_name_ctx))
	{
		return Scope().m_locals.at(std::get<MidoriExpression::NameContext::Local>(name.m_name_ctx).m_index);
	}
	if (std::holds_alternative<MidoriExpression::NameContext::Cell>(name.m_name_ctx))
	{
		FunctionScope& scope = Scope();
		const LocalKey key = KeyForCell(scope, std::get<MidoriExpression::NameContext::Cell>(name.m_name_ctx).m_index);
		return CaptureFor(scope, key, Concrete(name.m_type_data));
	}

	const std::string& lexeme = token.m_lexeme;
	const Specialization& specialization = Scope().m_specialization;
	if (specialization.m_methods.contains(lexeme))
	{
		const MethodResolution<std::string> resolved = m_resolver.ResolveConstrainedValue(specialization.m_methods, lexeme);
		if (!resolved.has_value())
		{
			return std::unexpected(ResolutionError(resolved.error(), token));
		}
		return LoadResolved(resolved.value(), Concrete(name.m_type_data), token);
	}

	if (!m_top_level_names.contains(lexeme) && m_generics.FindKey(lexeme, specialization.m_source_module).has_value())
	{
		return std::unexpected(Error(CompilerErrorCode::LoweringUnresolvedMethodResolution, std::format("Generic function '{}' is monomorphised at each call site, so it has no single procedure to use as a value. Call it directly, or wrap it in a non-generic lambda that pins its type parameters.", lexeme), token));
	}
	if (IsForeignFunction(name.m_type_data))
	{
		return LowerForeignValue(token, Concrete(name.m_type_data));
	}
	if (m_resolver.IsClassMethod(lexeme))
	{
		return std::unexpected(Unsupported("a class method used as a value", token));
	}

	return GlobalFor(token, Concrete(name.m_type_data))
		.transform([this, &token](MidoriIRGlobalSlot slot)
		{
			const MidoriIRGlobal& global = m_module.m_globals[slot.m_value];
			return Builder().AtLine(token.m_line).Emit(MidoriIROp::GlobalGet, global.m_type, {}, slot, global.m_name);
		});
}

// A bare name inside a specialization of another module's generic is one of
// that module's, whatever this module defines.
std::expected<MidoriIRGlobalSlot, CompilerError> Lowering::GlobalFor(const Token& name, const TypeRef& type)
{
	const std::string& lexeme = name.m_lexeme;
	const TypeRef import_type = type->IsType<MidoriType::FunctionType>() ? ClosureType(type) : type;
	const size_t separator = lexeme.find(NameSeparator);
	const std::optional<std::string>& source_module = Scope().m_specialization.m_source_module;
	if (source_module.has_value() && separator == std::string::npos)
	{
		return ImportGlobal(source_module.value(), lexeme, import_type, name);
	}

	const std::unordered_map<std::string, TopLevelName>::const_iterator own = m_top_level_names.find(lexeme);
	if (own != m_top_level_names.cend())
	{
		return own->second.m_slot;
	}
	if (separator == std::string::npos)
	{
		return std::unexpected(Unsupported(std::format("'{}'", lexeme), name));
	}
	return ImportGlobal(lexeme.substr(0u, separator), lexeme.substr(separator + NameSeparator.length()), import_type, name);
}

// A resolved instance method is `name`, a global of this module, or
// `name@Module`, one of Module's.
std::expected<MidoriIRGlobalSlot, CompilerError> Lowering::GlobalForResolved(const std::string& resolved_name, const TypeRef& type, const Token& at)
{
	const ResolvedInstanceName name(resolved_name);
	if (name.m_module.has_value())
	{
		if (m_generics.FindKey(resolved_name, Scope().m_specialization.m_source_module).has_value())
		{
			return std::unexpected(Error(CompilerErrorCode::LoweringUnresolvedMethodResolution, std::format("Constrained instance method '{}' from module '{}' is monomorphised at each call site, so it has no address to take. Call it directly instead of using it through an operator or as a value.", name.m_symbol, name.m_module.value()), at));
		}
		return ImportGlobal(name.m_module.value(), name.m_symbol, type, at);
	}

	const std::unordered_map<std::string, TopLevelName>::const_iterator own = m_top_level_names.find(name.m_symbol);
	if (own == m_top_level_names.cend())
	{
		return std::unexpected(Error(CompilerErrorCode::None, std::format("Resolved symbol '{}' not found in globals.", resolved_name), at));
	}
	return own->second.m_slot;
}

MidoriIRGlobalSlot Lowering::ImportGlobal(const std::string& module, const std::string& symbol, const TypeRef& type, const Token& at)
{
	const std::string key = module + std::string(NameSeparator) + symbol;
	const std::unordered_map<std::string, MidoriIRGlobalSlot>::const_iterator imported = m_import_slots.find(key);
	if (imported != m_import_slots.cend())
	{
		return imported->second;
	}

	const MidoriIRGlobalSlot slot = m_module.ReserveImport(module, symbol, type);
	m_import_slots.emplace(key, slot);
	m_import_tokens.emplace_back(slot, at);
	return slot;
}

Lowering::Lowered Lowering::LoadResolved(const std::string& resolved_name, const TypeRef& type, const Token& at)
{
	return GlobalForResolved(resolved_name, type, at)
		.transform([this, &at](MidoriIRGlobalSlot slot)
		{
			const MidoriIRGlobal& global = m_module.m_globals[slot.m_value];
			return Builder().AtLine(at.m_line).Emit(MidoriIROp::GlobalGet, global.m_type, {}, slot, global.m_name);
		});
}

// A constrained instance's method is a generic, specialized for these
// arguments; this module's own is called directly; another module's through
// its global.
Lowering::Lowered Lowering::CallResolved(const std::string& resolved_name, std::vector<MidoriIRValueId> arguments, const std::vector<TypeRef>& argument_types, const TypeRef& result_type, const Token& at)
{
	const std::optional<std::string> generic_key = m_generics.FindKey(resolved_name, Scope().m_specialization.m_source_module);
	if (generic_key.has_value())
	{
		return Specialize(generic_key.value(), argument_types, result_type)
			.transform([&](MidoriIRFunctionId function)
			{
				return Builder().AtLine(at.m_line).Emit(MidoriIROp::Call, m_functions[function.m_index].m_return_type, std::move(arguments), function);
			});
	}

	const ResolvedInstanceName name(resolved_name);
	if (!name.m_module.has_value())
	{
		const std::unordered_map<std::string, TopLevelName>::const_iterator own = m_top_level_names.find(name.m_symbol);
		if (own != m_top_level_names.cend() && own->second.m_function.has_value())
		{
			const MidoriIRFunctionId function = own->second.m_function.value();
			return Builder().AtLine(at.m_line).Emit(MidoriIROp::Call, m_functions[function.m_index].m_return_type, std::move(arguments), function);
		}
	}

	return GlobalForResolved(resolved_name, MidoriType::MakeFunctionType(argument_types, TypeRef(result_type)), at)
		.transform([&](MidoriIRGlobalSlot slot)
		{
			return Builder().AtLine(at.m_line).Emit(MidoriIROp::CallGlobal, result_type, std::move(arguments), slot);
		});
}

std::optional<std::string> Lowering::IntrinsicName(const MidoriExpression::Call& call) const
{
	const MidoriExpression::NameAccess* name = GlobalName(*call.m_callee);
	if (name == nullptr || !std::ranges::contains(s_intrinsic_calls, name->m_name.m_lexeme))
	{
		return std::nullopt;
	}
	return name->m_name.m_lexeme;
}

Lowering::Lowered Lowering::LowerIntrinsic(const std::string& name, MidoriExpression::Call& call)
{
	struct Intrinsic
	{
		std::string_view m_name;
		MidoriIROp m_op;
	};
	static constexpr std::array<Intrinsic, 3u> s_intrinsics =
	{{
		{ "Concurrency::Close", MidoriIROp::ChannelClose },
		{ "Concurrency::IsDone", MidoriIROp::WorkerIsDone },
		{ "Concurrency::Cancel", MidoriIROp::WorkerCancel }
	}};

	const MidoriIROp op = std::ranges::find(s_intrinsics, name, &Intrinsic::m_name)->m_op;
	std::vector<MidoriExpression*> operands = call.m_arguments
		| std::views::transform([](std::unique_ptr<MidoriExpression>& argument) { return argument.get(); })
		| std::ranges::to<std::vector>();
	return LowerOperation(op, Concrete(call.m_type_data), std::move(operands), {}, call.m_paren.m_line);
}

Lowering::Lowered Lowering::LowerCall(MidoriExpression::Call& call)
{
	const std::optional<std::string> intrinsic = IntrinsicName(call);
	if (intrinsic.has_value())
	{
		return LowerIntrinsic(intrinsic.value(), call);
	}

	const TypeRef call_type = Concrete(call.m_type_data);
	const int line = call.m_paren.m_line;
	return LowerArguments(call.m_arguments)
		.and_then([&](std::vector<MidoriIRValueId> arguments) -> Lowered
		{
			if (IsDead())
			{
				return Placeholder(call_type);
			}
			return ResolveCallee(call, arguments, call_type)
				.transform([&](Callee callee)
				{
					return EmitCall(callee, std::move(arguments), call_type, line);
				});
		});
}

// The language promises that a call in tail position runs without a new
// frame, whatever it calls. A foreign call has no frame of its own to reuse,
// and a call whose result is not the function's is not one it can return.
Lowering::Emitted Lowering::LowerTailCall(MidoriExpression::Call& call)
{
	if (call.m_is_foreign || IntrinsicName(call).has_value())
	{
		return LowerCall(call)
			.transform([this](MidoriIRValueId value)
			{
				Builder().Return(value);
			});
	}

	const TypeRef call_type = Concrete(call.m_type_data);
	const int line = call.m_paren.m_line;
	return LowerArguments(call.m_arguments)
		.and_then([&](std::vector<MidoriIRValueId> arguments) -> Emitted
		{
			if (IsDead())
			{
				Builder().Return(Placeholder(call_type));
				return {};
			}
			return ResolveCallee(call, arguments, call_type)
				.transform([&](Callee callee)
				{
					const TypeRef result_type = ResultTypeOf(callee, call_type);
					if (!IsNever(result_type) && !MidoriIRSameType(result_type, Scope().m_function.m_return_type))
					{
						Builder().Return(EmitCall(callee, std::move(arguments), call_type, line));
						return;
					}
					if (callee.m_value.has_value())
					{
						arguments.insert(arguments.begin(), callee.m_value.value());
					}
					Builder().AtLine(line).TailCall(callee.m_target, std::move(arguments));
				});
		});
}

// A call's value has the type its callee returns, which the type checker does
// not always record inside a generic's body.
Lowering::TypeRef Lowering::ResultTypeOf(const Callee& callee, const TypeRef& call_type)
{
	const auto returned_by = [&call_type](const TypeRef& function_type)
	{
		return function_type->IsType<MidoriType::FunctionType>() ? function_type->GetType<MidoriType::FunctionType>().m_return_type : call_type;
	};
	if (std::holds_alternative<MidoriIRFunctionId>(callee.m_target))
	{
		return m_functions[std::get<MidoriIRFunctionId>(callee.m_target).m_index].m_return_type;
	}
	if (std::holds_alternative<MidoriIRGlobalSlot>(callee.m_target))
	{
		return returned_by(m_module.m_globals[std::get<MidoriIRGlobalSlot>(callee.m_target).m_value].m_type);
	}
	if (callee.m_value.has_value())
	{
		return returned_by(Scope().m_function.TypeOf(callee.m_value.value()));
	}
	return call_type;
}

MidoriIRValueId Lowering::EmitCall(const Callee& callee, std::vector<MidoriIRValueId> arguments, const TypeRef& call_type, int line)
{
	const TypeRef result_type = ResultTypeOf(callee, call_type);
	const bool is_foreign = std::holds_alternative<MidoriIRForeign>(callee.m_target) || (callee.m_value.has_value() && !Scope().m_function.TypeOf(callee.m_value.value())->IsType<MidoriType::FunctionType>());
	if (callee.m_value.has_value())
	{
		arguments.insert(arguments.begin(), callee.m_value.value());
	}
	const MidoriIROp op = std::holds_alternative<MidoriIRFunctionId>(callee.m_target) ? MidoriIROp::Call
		: std::holds_alternative<MidoriIRGlobalSlot>(callee.m_target) ? MidoriIROp::CallGlobal
		: is_foreign ? MidoriIROp::CallForeign
		: MidoriIROp::CallValue;
	const MidoriIRValueId result = Builder().AtLine(line).Emit(op, result_type, std::move(arguments), callee.m_target);
	if (IsNever(result_type))
	{
		AfterNever();
	}
	return result;
}

// What a call runs: a builtin foreign function by name, another foreign one by
// the name its global holds, the specialization of a generic, the instance
// method a class method resolves to, this module's function, a global, or the
// closure its callee evaluates to, after the arguments.
std::expected<Lowering::Callee, CompilerError> Lowering::ResolveCallee(MidoriExpression::Call& call, const std::vector<MidoriIRValueId>& arguments, const TypeRef& call_type)
{
	const MidoriExpression::NameAccess* name = GlobalName(*call.m_callee);
	const Specialization& specialization = Scope().m_specialization;
	const Token& paren = call.m_paren;

	// A foreign function is called by the name it is looked up by: a builtin
	// by index, any other by the name its declaration holds. A value that only
	// has a foreign function's type holds the closure that calls one.
	if (call.m_is_foreign)
	{
		const MidoriExpression::NameAccess& foreign = call.m_callee->GetExpression<MidoriExpression::NameAccess>();
		const ForeignIndices::const_iterator builtin = specialization.m_foreign_indices->find(foreign.m_name.m_lexeme);
		if (builtin != specialization.m_foreign_indices->cend())
		{
			return Callee{ MidoriIRForeign{ std::string(MarmotBuiltins::At(builtin->second)) }, std::nullopt };
		}
		if (name != nullptr && IsForeignDeclaration(name->m_name.m_lexeme))
		{
			return GlobalFor(name->m_name, TextType())
				.transform([this, &paren](MidoriIRGlobalSlot slot)
				{
					const MidoriIRGlobal& global = m_module.m_globals[slot.m_value];
					return Callee{ std::monostate{}, Builder().AtLine(paren.m_line).Emit(MidoriIROp::GlobalGet, global.m_type, {}, slot, global.m_name) };
				});
		}
	}

	if (name == nullptr)
	{
		return Lower(*call.m_callee)
			.transform([](MidoriIRValueId closure) { return Callee{ std::monostate{}, closure }; });
	}

	const std::string& lexeme = name->m_name.m_lexeme;
	std::vector<TypeRef> argument_types;
	for (size_t index = 0u; index < arguments.size(); index += 1u)
	{
		argument_types.push_back(NominalType(*call.m_arguments[index], arguments[index]));
	}
	std::optional<std::string> resolved;
	if (specialization.m_methods.contains(lexeme))
	{
		const MethodResolution<std::string> method = m_resolver.ResolveConstrainedCall(specialization.m_methods, lexeme, argument_types.empty() ? nullptr : &argument_types.front(), call_type);
		if (!method.has_value())
		{
			return std::unexpected(ResolutionError(method.error(), paren));
		}
		resolved = method.value();
	}
	else if (lexeme.find(NameSeparator) != std::string::npos)
	{
		const MethodResolution<std::optional<std::string>> method = m_resolver.ResolveConcreteCall(lexeme, argument_types, call_type);
		if (!method.has_value())
		{
			return std::unexpected(ResolutionError(method.error(), paren));
		}
		resolved = method.value();
	}

	const std::optional<std::string> generic_key = m_generics.FindKey(resolved.value_or(lexeme), specialization.m_source_module);
	if (generic_key.has_value())
	{
		return Specialize(generic_key.value(), argument_types, call_type)
			.transform([](MidoriIRFunctionId function) { return Callee{ function, std::nullopt }; });
	}

	if (resolved.has_value())
	{
		const ResolvedInstanceName instance(resolved.value());
		const std::unordered_map<std::string, TopLevelName>::const_iterator own = instance.m_module.has_value() ? m_top_level_names.cend() : m_top_level_names.find(instance.m_symbol);
		if (own != m_top_level_names.cend() && own->second.m_function.has_value())
		{
			return Callee{ own->second.m_function.value(), std::nullopt };
		}
		return GlobalForResolved(resolved.value(), MidoriType::MakeFunctionType(argument_types, TypeRef(call_type)), paren)
			.transform([](MidoriIRGlobalSlot slot) { return Callee{ slot, std::nullopt }; });
	}

	if (!specialization.m_source_module.has_value())
	{
		const std::unordered_map<std::string, TopLevelName>::const_iterator own = m_top_level_names.find(lexeme);
		if (own != m_top_level_names.cend() && own->second.m_function.has_value())
		{
			return Callee{ own->second.m_function.value(), std::nullopt };
		}
	}

	return GlobalFor(name->m_name, Concrete(name->m_type_data))
		.transform([](MidoriIRGlobalSlot slot) { return Callee{ slot, std::nullopt }; });
}

Lowering::LoweredValues Lowering::LowerArguments(std::vector<std::unique_ptr<MidoriExpression>>& arguments)
{
	std::vector<MidoriIRValueId> values;
	values.reserve(arguments.size());
	for (std::unique_ptr<MidoriExpression>& argument : arguments)
	{
		const Lowered value = Lower(*argument);
		if (!value.has_value())
		{
			return std::unexpected(value.error());
		}
		values.push_back(value.value());
	}
	return values;
}

std::vector<Lowering::TypeRef> Lowering::TypesOf(const std::vector<MidoriIRValueId>& values)
{
	const MidoriIRFunction& function = Scope().m_function;
	return values
		| std::views::transform([&function](MidoriIRValueId value) { return function.TypeOf(value); })
		| std::ranges::to<std::vector>();
}
