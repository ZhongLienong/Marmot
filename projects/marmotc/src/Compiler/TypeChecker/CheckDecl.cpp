#include "TypeChecker.h"
#include "Compiler/Names/SpecialNames.h"
#include "Compiler/Operator/Operator.h"
#include "Compiler/TypeMatching/InstanceTypeMatcher.h"
#include <algorithm>
#include <format>
#include <ranges>

using namespace std::string_literals;

namespace
{
	std::shared_ptr<MarmotType> GlobalAnnotation(const MarmotStatement::VariableDefinition& definition)
	{
		const std::shared_ptr<MarmotType>& annotation = *definition.m_annotated_type;
		return definition.m_value != nullptr && definition.m_value->IsExpression<MarmotExpression::Function>()
			? MarmotType::WithTransferableFunction(annotation)
			: annotation;
	}

	// A union's members' fields, in tag order: a member's tag is its place in the
	// declaration.
	std::vector<std::vector<std::shared_ptr<MarmotType>>> FieldsByTag(const MarmotType::UnionType& union_type)
	{
		std::vector<std::vector<std::shared_ptr<MarmotType>>> fields(union_type.m_member_info.size());
		for (const std::pair<const std::string, MarmotType::UnionType::UnionMemberContext>& member : union_type.m_member_info)
		{
			fields[static_cast<size_t>(member.second.m_tag)] = member.second.m_member_types;
		}
		return fields;
	}

	bool IsSpawnSignature(const std::shared_ptr<MarmotType>& argument, const std::shared_ptr<MarmotType>& function, const std::shared_ptr<MarmotType>& result)
	{
		if (!function->IsType<MarmotType::FunctionType>() || !result->IsType<MarmotType::WorkerType>())
		{
			return false;
		}
		const MarmotType::FunctionType& spawned = function->GetType<MarmotType::FunctionType>();
		return spawned.m_param_types.size() == 1u
			&& *spawned.m_param_types[0u] == *argument
			&& *spawned.m_return_type == *result->GetType<MarmotType::WorkerType>().m_result_type;
	}

	// What the VM builds when it joins a worker, by position rather than by name:
	// member 0 holds the worker's value and member 1 why there is none, cancelled
	// (member 0, no fields) or failed (member 1, the message).
	bool IsJoinOutcome(const std::shared_ptr<MarmotType>& outcome, const std::shared_ptr<MarmotType>& value)
	{
		if (!outcome->IsType<MarmotType::UnionType>())
		{
			return false;
		}
		const std::vector<std::vector<std::shared_ptr<MarmotType>>> outcome_fields = FieldsByTag(outcome->GetType<MarmotType::UnionType>());
		if (outcome_fields.size() != 2u || outcome_fields[0u].size() != 1u || *outcome_fields[0u][0u] != *value
			|| outcome_fields[1u].size() != 1u || !outcome_fields[1u][0u]->IsType<MarmotType::UnionType>())
		{
			return false;
		}
		const std::vector<std::vector<std::shared_ptr<MarmotType>>> error_fields = FieldsByTag(outcome_fields[1u][0u]->GetType<MarmotType::UnionType>());
		return error_fields.size() == 2u && error_fields[0u].empty()
			&& error_fields[1u].size() == 1u && error_fields[1u][0u]->IsType<MarmotType::TextType>();
	}

	// The VM uses tag 0 for a closed and drained channel and tag 1 for a message.
	bool IsReceiveOutcome(const std::shared_ptr<MarmotType>& outcome, const std::shared_ptr<MarmotType>& value)
	{
		if (!outcome->IsType<MarmotType::UnionType>())
		{
			return false;
		}
		const std::vector<std::vector<std::shared_ptr<MarmotType>>> fields = FieldsByTag(outcome->GetType<MarmotType::UnionType>());
		return fields.size() == 2u && fields[0u].empty() && fields[1u].size() == 1u && *fields[1u][0u] == *value;
	}
}

void TypeChecker::ApplyTransferableParameterConstraints(MarmotType::FunctionType& signature)
{
	// A constraint on a function signature is a requirement on its matching
	// callback parameters. It proves nothing about other values of the same type.
	for (MarmotType::ClassConstraint& constraint : signature.m_constraints)
	{
		if (constraint.m_class_name != TRANSFERABLE_CLASS_NAME || constraint.m_type_args.size() != 1u
			|| !constraint.m_type_args.front()->IsType<MarmotType::FunctionType>())
		{
			continue;
		}

		const std::shared_ptr<MarmotType>& required = constraint.m_type_args.front();
		const std::shared_ptr<MarmotType> transferable = MarmotType::WithTransferableFunction(required);
		bool matched = false;
		for (std::shared_ptr<MarmotType>& parameter : signature.m_param_types)
		{
			if (*parameter == *required || *parameter == *transferable)
			{
				parameter = transferable;
				matched = true;
			}
		}
		if (matched)
		{
			constraint.m_type_args.front() = transferable;
		}
	}
}

// A constructor belongs to the module that declared its type: two modules
// may each declare a `Wrap`, and `Wrap(1)` in one is not the other's.
std::string TypeChecker::ConstructorKey(const std::string& module_name, const std::string& constructor_name)
{
	return module_name.empty() ? constructor_name : module_name + std::string(NameSeparator) + constructor_name;
}

std::shared_ptr<MarmotType> TypeChecker::MakeGlobalFunctionType(const std::vector<std::shared_ptr<MarmotType>>& parameters, const std::shared_ptr<MarmotType>& result, const std::vector<MarmotType::ClassConstraint>& constraints)
{
	std::shared_ptr<MarmotType> type = MarmotType::MakeFunctionType(parameters, std::shared_ptr<MarmotType>(result));
	MarmotType::FunctionType& signature = type->GetType<MarmotType::FunctionType>();
	signature.m_is_transferable = true;
	signature.m_constraints = constraints;
	ApplyTransferableParameterConstraints(signature);
	return type;
}

TypeChecker::CheckingContext::CheckingContext(TypeScope& scope)
	: m_scope(&scope),
	m_assumptions(std::make_shared<const CheckingAssumptions>())
{
}

TypeChecker::CheckingContext TypeChecker::CheckingContext::WithScope(TypeScope& scope) const
{
	CheckingContext result(*this);
	result.m_scope = &scope;
	return result;
}

TypeChecker::CheckingContext TypeChecker::CheckingContext::WithExpectedType(std::shared_ptr<MarmotType> type) const
{
	CheckingContext result(*this);
	result.m_expected_expr_type = std::move(type);
	return result;
}

TypeChecker::CheckingContext TypeChecker::CheckingContext::WithAssumptions(std::shared_ptr<const CheckingAssumptions> assumptions) const
{
	CheckingContext result(*this);
	result.m_assumptions = std::move(assumptions);
	return result;
}

TypeChecker::CheckingContext TypeChecker::CheckingContext::ForFunction(std::shared_ptr<MarmotType> return_type, const std::vector<MarmotType::ClassConstraint>& constraints) const
{
	CheckingContext result = WithExpectedType(return_type);
	result.m_expected_return_type = std::move(return_type);
	std::shared_ptr<CheckingAssumptions> assumptions = std::make_shared<CheckingAssumptions>(*m_assumptions);
	for (const MarmotType::ClassConstraint& constraint : constraints)
	{
		AppendUniqueConstraint(assumptions->m_constraints, MarmotType::ClassConstraint(constraint));
	}
	result.m_assumptions = std::move(assumptions);
	return result;
}

TypeChecker::GenericContext::GenericContext(CheckingContext context, std::vector<std::pair<int, std::string>> parameters)
	: m_context(std::move(context)),
	m_parameters(std::move(parameters))
{
}

TypeChecker::GenericContext TypeChecker::ForGenericParameters(const CheckingContext& context, const TypeEnvironment& parameters, LexicalFunctionId function)
{
	std::shared_ptr<CheckingAssumptions> assumptions = std::make_shared<CheckingAssumptions>(*context.m_assumptions);
	std::vector<std::pair<int, std::string>> own_parameters;
	for (const auto& [name, type] : parameters)
	{
		if (type->IsType<MarmotType::TypeVariable>() && assumptions->m_rigid_type_vars.emplace(type->GetType<MarmotType::TypeVariable>().m_id, name).second)
		{
			const int id = type->GetType<MarmotType::TypeVariable>().m_id;
			own_parameters.emplace_back(id, name);
			m_generic_parameter_names.emplace(id, name);
		}
	}
	std::ranges::sort(own_parameters, {}, &std::pair<int, std::string>::second);
	if (function != nullptr)
	{
		assumptions->m_defining_generic_functions.push_back(std::move(function));
	}
	return GenericContext(context.WithAssumptions(std::move(assumptions)), std::move(own_parameters));
}

void TypeChecker::BindParameters(const CheckingContext& context, const FunctionParameters& parameters, const std::vector<std::shared_ptr<MarmotType>>& types)
{
	for (size_t index = 0u; index < parameters.Size(); index += 1u)
	{
		if (const BindingId& binding = parameters.BindingAt(index))
		{
			context.m_scope->m_bindings.emplace(binding, types[index]);
		}
	}
}

void TypeChecker::BindValue(const CheckingContext& context, const Token& name, const std::optional<BindingId>& binding, std::shared_ptr<MarmotType> type)
{
	if (name.IsDiscard())
	{
		return;
	}
	if (binding.has_value())
	{
		context.m_scope->m_bindings.insert_or_assign(binding.value(), std::move(type));
	}
	else
	{
		context.m_scope->m_names.insert_or_assign(name.m_lexeme, std::move(type));
	}
}

std::shared_ptr<MarmotType>* TypeChecker::FindBindingType(const CheckingContext& context, const BindingId& binding)
{
	for (TypeScope* scope = context.m_scope; scope != nullptr; scope = scope->m_parent)
	{
		const std::unordered_map<BindingId, std::shared_ptr<MarmotType>>::iterator found = scope->m_bindings.find(binding);
		if (found != scope->m_bindings.end())
		{
			return &found->second;
		}
	}
	std::unreachable();
}

const std::shared_ptr<MarmotType>* TypeChecker::FindValueType(const CheckingContext& context, const MarmotExpression::NameAccess& name) const
{
	const BindingId* binding = nullptr;
	if (const MarmotExpression::NameContext::Local* local = std::get_if<MarmotExpression::NameContext::Local>(&name.m_name_ctx))
	{
		binding = &local->m_binding;
	}
	else if (const MarmotExpression::NameContext::Cell* captured = std::get_if<MarmotExpression::NameContext::Cell>(&name.m_name_ctx))
	{
		binding = &captured->m_binding;
	}
	if (binding != nullptr)
	{
		for (const TypeScope* scope = context.m_scope; scope != nullptr; scope = scope->m_parent)
		{
			const std::unordered_map<BindingId, std::shared_ptr<MarmotType>>::const_iterator found = scope->m_bindings.find(*binding);
			if (found != scope->m_bindings.cend())
			{
				return &found->second;
			}
		}
		std::unreachable();
	}
	return FindNameType(context, name.m_name.m_lexeme);
}



std::shared_ptr<MarmotType>* TypeChecker::FindNameType(const CheckingContext& context, const std::string& name)
{
	for (TypeScope* it = context.m_scope; it != nullptr; it = it->m_parent)
	{
		TypeEnvironment::iterator found = it->m_names.find(name);
		if (found != it->m_names.end())
		{
			return &found->second;
		}
	}
	return nullptr;
}

const std::shared_ptr<MarmotType>* TypeChecker::FindNameType(const CheckingContext& context, const std::string& name) const
{
	for (const TypeScope* it = context.m_scope; it != nullptr; it = it->m_parent)
	{
		TypeEnvironment::const_iterator found = it->m_names.find(name);
		if (found != it->m_names.cend())
		{
			return &found->second;
		}
	}
	return nullptr;
}

MarmotResult::TypeResult TypeChecker::Evaluate(const CheckingContext& context, const std::unique_ptr<MarmotStatement>& statement)
{
	return VisitNode
	(
		[this, &context]<typename T>(T& stmt) -> MarmotResult::TypeResult
		{
			return (*this)(context, stmt);
		},
		statement
	);
}

TypeChecker::LocalGenericFunction::LocalGenericFunction(BindingId binding, LexicalFunctionId function, std::vector<int> parameters)
	: m_binding(std::move(binding)),
	m_function(std::move(function)),
	m_parameters(std::move(parameters))
{
}

TypeChecker::ClassInfo::ClassInfo(const std::string& name, std::vector<std::string>&& params, AssociatedTypeEnvironment&& associated_types, TypeEnvironment&& methods)
	: m_name(name),
	m_type_param_names(std::move(params)),
	m_associated_types(std::move(associated_types)),
	m_method_types(std::move(methods))
{
}

TypeChecker::InstanceInfo::InstanceInfo(const std::string& tc_name, std::vector<std::shared_ptr<MarmotType>>&& args, std::vector<MarmotType::ClassConstraint>&& constraints, AssociatedTypeEnvironment&& associated_type_bindings, std::unordered_map<std::string, std::unique_ptr<MarmotStatement>>&& methods, bool is_derived)
	: m_class_name(tc_name),
	m_type_args(std::move(args)),
	m_constraints(std::move(constraints)),
	m_associated_type_bindings(std::move(associated_type_bindings)),
	m_method_impls(std::move(methods)),
	m_is_derived(is_derived)
{
}

TypeChecker::TypeChecker(
	MarmotProgramTree&& parser_result,
	std::string_view file_name,
	const std::vector<std::string>& source_lines,
	TypeEnvironment imported_types,
	const TypeclassMetadataMap& imported_typeclasses
)
	: m_program_tree(std::move(parser_result)),
	m_file_name(file_name),
	m_source_lines(source_lines),
	m_next_type_var_id(0)
{
	for (const auto& [class_name, metadata] : imported_typeclasses)
	{
		AssociatedTypeEnvironment associated_types;
		for (const std::string& name : metadata.m_associated_type_names)
		{
			std::vector<std::shared_ptr<MarmotType>> type_args;
			for (const std::string& param : metadata.m_type_param_names)
			{
				type_args.emplace_back(MarmotType::MakeGenericType(param));
			}
			associated_types.emplace(name, MarmotType::MakeAssociatedType(class_name, name, std::move(type_args)));
		}
		m_classes.emplace(class_name, ClassInfo(class_name, std::vector<std::string>(metadata.m_type_param_names), std::move(associated_types), TypeEnvironment(metadata.m_method_types)));
	}

	// Pre-populate type environment with imported types
	if (!imported_types.empty())
	{
		TypeEnvironment imported_names;
		for (const auto& [name, type] : imported_types)
		{
			// An exported value can have a struct type too, `def Origin = P(0, 0)`,
			// and is bound to its type like any value. Only the struct's own name
			// is its constructor.
			const size_t separator_pos = name.rfind(NameSeparator);
			const std::string_view bare_name = separator_pos == std::string::npos ? std::string_view(name) : std::string_view(name).substr(separator_pos + NameSeparator.length());
			if (type->IsType<MarmotType::StructType>() && type->GetType<MarmotType::StructType>().m_name == bare_name)
			{
				const MarmotType::StructType& struct_type = type->GetType<MarmotType::StructType>();
				m_struct_type_definitions[ConstructorKey(struct_type.m_module_name, struct_type.m_name)] = type;
				if (!struct_type.m_generic_params.empty())
				{
					m_generic_structs.insert(ConstructorKey(struct_type.m_module_name, struct_type.m_name));
				}

				// The declaring module binds a struct's name to its constructor's
				// function type, not to the struct type, so an importing module must
				// bind the same thing or there is nothing callable under the name.
				// The type itself is still reachable through m_struct_type_definitions,
				// registered just above.
				// It carries the struct's where clause, as the declaring module's does,
				// so constructing one here is held to the same constraints.
				std::shared_ptr<MarmotType> constructor_type = MarmotType::MakeFunctionType(struct_type.m_member_types, std::shared_ptr<MarmotType>(type));
				constructor_type->GetType<MarmotType::FunctionType>().m_constraints = struct_type.m_constraints;
				imported_names[name] = std::move(constructor_type);
				continue;
			}

			// The declaring module derives a newtype's conversions to and from its
			// representation itself, so they are not among the instances it
			// exports; without them `4 as Module::Meters` found no instance.
			if (type->IsType<MarmotType::NewType>())
			{
				RegisterIdentityConversion(type, type->GetType<MarmotType::NewType>().m_representation);
				RegisterIdentityConversion(type->GetType<MarmotType::NewType>().m_representation, type);
			}

			if (type->IsType<MarmotType::UnionType>())
			{
				const MarmotType::UnionType& union_type = type->GetType<MarmotType::UnionType>();
				m_union_type_definitions[ConstructorKey(union_type.m_module_name, union_type.m_name)] = type;
				if (!union_type.m_generic_params.empty())
				{
					m_generic_unions.insert(ConstructorKey(union_type.m_module_name, union_type.m_name));
				}
			}

			imported_names[name] = type;
		}

		m_imported_types = std::move(imported_names);
	}

	for (const auto& [class_name, metadata] : imported_typeclasses)
	{
		for (const TypeclassInstance& instance : metadata.m_instances)
		{
			std::vector<std::string> concrete_type_names;
			concrete_type_names.reserve(instance.m_type_args.size());
			for (const std::shared_ptr<MarmotType>& type_arg : instance.m_type_args)
			{
				concrete_type_names.emplace_back(type_arg->ToString());
			}
			InstanceKey instance_key{ class_name, std::move(concrete_type_names) };
			if (!m_instances.contains(instance_key))
			{
				m_instances.emplace(std::move(instance_key), InstanceInfo(class_name, TypeclassInstance::TypeArguments(instance.m_type_args), std::vector<MarmotType::ClassConstraint>(instance.m_constraints), AssociatedTypeEnvironment(instance.m_associated_type_bindings), std::unordered_map<std::string, std::unique_ptr<MarmotStatement>>()));
			}
		}
	}
}

const TypeChecker::TypeEnvironment& TypeChecker::ModuleTypes() const noexcept
{
	return m_module_types;
}

std::unordered_map<int, std::string> TypeChecker::TakeGenericParameterNames() &&
{
	return std::move(m_generic_parameter_names);
}

namespace
{
	bool IsWrittenDown(const std::shared_ptr<MarmotType>& type)
	{
		return !type->IsType<MarmotType::UndecidedType>();
	}
}

void TypeChecker::CollectLaterDefinitions()
{
	for (const std::unique_ptr<MarmotStatement>& statement : m_program_tree)
	{
		if (statement->IsStatement<MarmotStatement::VariableDefinition>())
		{
			const MarmotStatement::VariableDefinition& def = statement->GetStatement<MarmotStatement::VariableDefinition>();
			if (def.m_name.IsDiscard())
			{
				continue;
			}
			if (def.m_annotated_type.has_value())
			{
				m_later_definitions[def.m_name.m_lexeme] = GlobalAnnotation(def);
				continue;
			}

			if ((def.m_value != nullptr) && def.m_value->IsExpression<MarmotExpression::Function>())
			{
				const MarmotExpression::Function& function = def.m_value->GetExpression<MarmotExpression::Function>();
				if (std::ranges::all_of(function.m_param_types, IsWrittenDown) && IsWrittenDown(function.m_return_type))
				{
					m_later_definitions[def.m_name.m_lexeme] = MakeGlobalFunctionType(function.m_param_types, function.m_return_type, function.m_constraints);
					continue;
				}
			}

			m_later_unannotated.insert(def.m_name.m_lexeme);
		}
		else if (statement->IsStatement<MarmotStatement::FunctionDefinition>())
		{
			const MarmotStatement::FunctionDefinition& defun = statement->GetStatement<MarmotStatement::FunctionDefinition>();
			m_later_definitions[defun.m_name.m_lexeme] = MakeGlobalFunctionType(defun.m_param_types, defun.m_return_type, defun.m_constraints);
		}
		else if (statement->IsStatement<MarmotStatement::ForeignDefinition>())
		{
			const MarmotStatement::ForeignDefinition& foreign = statement->GetStatement<MarmotStatement::ForeignDefinition>();
			m_later_definitions[foreign.m_function_name.m_lexeme] = foreign.m_type;
		}
		else if (statement->IsStatement<MarmotStatement::TupleDefinition>())
		{
			for (const Token& name : statement->GetStatement<MarmotStatement::TupleDefinition>().m_names)
			{
				if (!name.IsDiscard())
				{
					m_later_unannotated.insert(name.m_lexeme);
				}
			}
		}
	}

	// A module's own declaration hides an imported bare name even before
	// its statement is checked.
	for (const std::string& name : m_later_definitions | std::views::keys)
	{
		m_imported_types.erase(name);
	}
	for (const std::string& name : m_later_unannotated)
	{
		m_imported_types.erase(name);
	}
}

void TypeChecker::RecordFailedDefinition(const MarmotStatement& statement)
{
	if (statement.IsStatement<MarmotStatement::VariableDefinition>())
	{
		const Token& name = statement.GetStatement<MarmotStatement::VariableDefinition>().m_name;
		if (!name.IsDiscard())
		{
			m_failed_definitions.insert(name.m_lexeme);
		}
	}
	else if (statement.IsStatement<MarmotStatement::TupleDefinition>())
	{
		for (const Token& name : statement.GetStatement<MarmotStatement::TupleDefinition>().m_names)
		{
			if (!name.IsDiscard())
			{
				m_failed_definitions.insert(name.m_lexeme);
			}
		}
	}
}

MarmotResult::TypeCheckerResult TypeChecker::TypeCheck()
{
	CollectLaterDefinitions();
	TypeScope imports(std::move(m_imported_types));
	const CheckingContext context(imports);

	return InScope
	(
		context,
		[&](const CheckingContext& context) -> MarmotResult::TypeCheckerResult
		{
			std::vector<CompilerError> errors;

			// Declarations first: a type may be named and constructed, and an
			// instance selected, anywhere in its module, and declaring one runs
			// nothing. An instance's method bodies are code, checked in order.
			const std::function<bool(const std::unique_ptr<MarmotStatement>&)> is_declaration = [](const std::unique_ptr<MarmotStatement>& statement)
				{
					return statement->IsStatement<MarmotStatement::Class>()
						|| statement->IsStatement<MarmotStatement::Struct>()
						|| statement->IsStatement<MarmotStatement::Union>()
						|| statement->IsStatement<MarmotStatement::TypeAlias>();
				};
			const std::function<void(MarmotResult::TypeResult&&)> collect = [&errors](MarmotResult::TypeResult&& result)
				{
					if (!result.has_value())
					{
						errors.emplace_back(std::move(result.error()));
					}
				};

			for (std::unique_ptr<MarmotStatement>& statement : m_program_tree | std::views::filter(is_declaration))
			{
				collect(Evaluate(context, statement));
			}
			for (std::unique_ptr<MarmotStatement>& statement : m_program_tree)
			{
				if (statement->IsStatement<MarmotStatement::Instance>())
				{
					collect(RegisterInstance(context, statement->GetStatement<MarmotStatement::Instance>()));
				}
			}
			if (!errors.empty())
			{
				return std::unexpected(MarmotResult::CompilerDiagnostics(std::move(errors)));
			}

			for (std::unique_ptr<MarmotStatement>& statement : m_program_tree | std::views::filter([&is_declaration](const std::unique_ptr<MarmotStatement>& statement) { return !is_declaration(statement); }))
			{
				m_used_failed_definition = false;
				MarmotResult::TypeResult result = Evaluate(context, statement);
				if (!result.has_value())
				{
					RecordFailedDefinition(*statement);
					if (m_used_failed_definition)
					{
						continue;
					}
				}
				collect(std::move(result));
			}

			if (errors.empty())
			{
				if (std::optional<CompilerError> error = CheckDecidedPendingInstances(context, 0u))
				{
					errors.push_back(std::move(*error));
				}
				for (const PendingInstance& undecided : m_pending_instances)
				{
					errors.push_back(MarmotError::GenerateTypeCheckerErrorWithContext(undecided.m_undecided_message, undecided.m_token, m_file_name, m_source_lines));
				}
			}
			if (errors.empty())
			{
				for (std::unique_ptr<MarmotStatement>& statement : m_program_tree)
				{
					ResolveRecordedTypes(context, *statement);
				}
				errors = std::move(m_undecided_call_errors);
			}
			if (errors.empty())
			{
				m_module_types = context.m_scope->m_names;
				return std::move(m_program_tree);
			}
			return std::unexpected(MarmotResult::CompilerDiagnostics(std::move(errors)));
		}
	);
}

void TypeChecker::ResolveRecordedTypes(const CheckingContext& context, MarmotStatement& statement)
{
	struct StatementResolver
	{
		TypeChecker& m_self;
		const CheckingContext& m_context;

		void operator()(MarmotStatement::ExpressionStatement& expression_statement) const
		{
			m_self.ResolveRecordedTypes(m_context, *expression_statement.m_expr);
		}

		void operator()(MarmotStatement::VariableDefinition& definition) const
		{
			if (definition.m_value != nullptr)
			{
				m_self.ResolveRecordedTypes(m_context, *definition.m_value);
			}
		}

		void operator()(MarmotStatement::TupleDefinition& definition) const
		{
			m_self.ResolveRecordedTypes(m_context, *definition.m_value);
		}

		void operator()(MarmotStatement::FunctionDefinition& definition) const
		{
			if (definition.m_body != nullptr)
			{
				m_self.ResolveRecordedTypes(m_context, *definition.m_body);
			}
		}

		void operator()(MarmotStatement::Instance& instance) const
		{
			for (MarmotStatement::Instance::Method& method : instance.m_methods)
			{
				(*this)(method.m_definition);
			}
		}

		void operator()(MarmotStatement::ForeignDefinition&) const {}
		void operator()(MarmotStatement::Struct&) const {}
		void operator()(MarmotStatement::Union&) const {}
		void operator()(MarmotStatement::Class&) const {}
		void operator()(MarmotStatement::TypeAlias&) const {}
	};

	VisitNode(StatementResolver{ *this, context }, statement);
}

// Extract type signatures from parsed AST without full type checking
// This allows parallel type checking of dependent modules
TypeChecker::TypeEnvironment TypeChecker::ExtractTypeSignatures(const MarmotProgramTree& ast, const std::unordered_set<std::string>* exported_symbols)
{
	TypeEnvironment signatures;

	for (const std::unique_ptr<MarmotStatement>& statement : ast)
	{
		std::visit
		(
			[&signatures, exported_symbols]<typename T>(const T& stmt) {
				using Node = std::decay_t<T>;

				if constexpr (std::is_same_v<Node, MarmotStatement::FunctionDefinition>)
				{
					if (exported_symbols == nullptr || exported_symbols->contains(stmt.m_name.m_lexeme))
					{
						signatures[stmt.m_name.m_lexeme] = MakeGlobalFunctionType(stmt.m_param_types, stmt.m_return_type, stmt.m_constraints);
					}
				}
				else if constexpr (std::is_same_v<Node, MarmotStatement::Struct>)
				{
					if (exported_symbols == nullptr || exported_symbols->contains(stmt.m_name.m_lexeme))
					{
						signatures[stmt.m_name.m_lexeme] = stmt.m_self_type;
					}
				}
				else if constexpr (std::is_same_v<Node, MarmotStatement::Union>)
				{
					if (exported_symbols == nullptr || exported_symbols->contains(stmt.m_name.m_lexeme))
					{
						signatures[stmt.m_name.m_lexeme] = stmt.m_self_type;

						if (stmt.m_self_type->template IsType<MarmotType::UnionType>())
						{
							const MarmotType::UnionType& union_type = stmt.m_self_type->template GetType<MarmotType::UnionType>();
							for (const auto& [member_name, member_ctx] : union_type.m_member_info)
							{
								std::vector<std::shared_ptr<MarmotType>> member_types_copy = member_ctx.m_member_types;
								std::shared_ptr<MarmotType> union_constructor_type = MarmotType::MakeFunctionType(std::move(member_types_copy), std::shared_ptr<MarmotType>(stmt.m_self_type));
								union_constructor_type->GetType<MarmotType::FunctionType>().m_constraints = stmt.m_constraints;
								signatures[member_name] = union_constructor_type;
							}
						}
					}
				}
				else if constexpr (std::is_same_v<Node, MarmotStatement::ForeignDefinition>)
				{
					if (exported_symbols == nullptr || exported_symbols->contains(stmt.m_function_name.m_lexeme))
					{
						signatures[stmt.m_function_name.m_lexeme] = stmt.m_type;
					}
				}
				else if constexpr (std::is_same_v<Node, MarmotStatement::TypeAlias>)
				{
					if (exported_symbols == nullptr || exported_symbols->contains(stmt.m_name.m_lexeme))
					{
						signatures[stmt.m_name.m_lexeme] = stmt.m_aliased_type;
					}
				}
				else if constexpr (std::is_same_v<Node, MarmotStatement::VariableDefinition>)
				{
					if (!stmt.m_name.IsDiscard() && (exported_symbols == nullptr || exported_symbols->contains(stmt.m_name.m_lexeme)))
					{
						if (stmt.m_annotated_type.has_value())
						{
							signatures[stmt.m_name.m_lexeme] = GlobalAnnotation(stmt);
						}
						else if (stmt.m_value != nullptr && stmt.m_value->template IsExpression<MarmotExpression::Function>())
						{
							const MarmotExpression::Function& function = stmt.m_value->template GetExpression<MarmotExpression::Function>();
							signatures[stmt.m_name.m_lexeme] = MakeGlobalFunctionType(function.m_param_types, function.m_return_type, function.m_constraints);
						}
					}
				}
			},
			**statement
		);
	}

	return signatures;
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotStatement::ExpressionStatement& simple)
{
	return Evaluate(context, simple.m_expr)
		.and_then
		(
			[](std::shared_ptr<MarmotType>&&) ->MarmotResult::TypeResult
			{
				return MarmotType::MakeUndecidedType();
			}
		);
}

MarmotResult::TypeResult TypeChecker::TypeCheckGenericLambdaDefinition(const CheckingContext& context, MarmotStatement::VariableDefinition& def, MarmotExpression::Function& function)
{
	if (def.m_annotated_type.has_value())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Function expression type error: a generic lambda cannot carry a type annotation, because its type differs at each call site", function.m_function_keyword, m_file_name, m_source_lines));
	}

	// Bound before the body so the lambda can recurse, and recorded as generic so
	// each use is instantiated instead of pinned to one.
	const std::string& name = def.m_name.m_lexeme;
	return CheckGenericLambda
	(
		context,
		function,
		def.m_name,
		def.m_binding.value_or(nullptr),
		[this, &def, &function, &name, &context](const std::shared_ptr<MarmotType>& function_type, const std::vector<int>& parameters)
		{
			def.m_value->GetType() = function_type;
			BindValue(context, def.m_name, def.m_binding, function_type);
			if (def.m_binding.has_value())
			{
				context.m_scope->m_generic_functions.emplace_back( def.m_binding.value(), function.m_params.Function(), parameters);
			}
			else
			{
				m_generic_functions.emplace(name, function.m_params.Function());
			}
		}
	);
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotStatement::VariableDefinition& def)
{
	if (def.m_annotated_type.has_value())
	{
		def.m_annotated_type = InstantiateWrittenType(context, def.m_annotated_type.value());
	}

	if (def.m_name.IsDiscard())
	{
		const CheckingContext initializer_context = context.WithExpectedType(def.m_annotated_type.value_or(nullptr));
		MarmotResult::TypeResult value = Evaluate(initializer_context, def.m_value);
		if (value.has_value() && def.m_annotated_type.has_value())
		{
			return Unify(initializer_context, def.m_name, def.m_annotated_type.value(), value.value(), UnifyDiagnosticMode::ExpectedActual);
		}
		return value;
	}

	// Special handling for functions (scope management required)
	if (def.m_value->IsExpression<MarmotExpression::Function>())
	{
		MarmotExpression::Function& function = def.m_value->GetExpression<MarmotExpression::Function>();

		// A generic lambda is a definition, not an ordinary initializer: it must be
		// registered by name so each use site is instantiated separately.
		if (!function.m_generic_params.empty())
		{
			return TypeCheckGenericLambdaDefinition(context, def, function);
		}

		bool has_inferred_param_types = std::ranges::any_of
		(
			function.m_param_types,
			[](const std::shared_ptr<MarmotType>& param_type)
			{
				return param_type->IsType<MarmotType::UndecidedType>();
			}
		);

		if (!def.m_annotated_type.has_value() && has_inferred_param_types)
		{
			return std::unexpected
			(
				MarmotError::GenerateTypeCheckerErrorWithContext
				(
					"Function expression type error: could not infer all lambda parameter or return types",
					function.m_function_keyword,
					m_file_name,
					m_source_lines
				)
			);
		}

		// Freshen any UndecidedType parameters to TypeVariables
		for (std::shared_ptr<MarmotType>& param_type : function.m_param_types)
		{
			param_type = Freshen(param_type);
		}
		function.m_return_type = Freshen(function.m_return_type);

		std::shared_ptr<MarmotType> return_type_copy = function.m_return_type;
		std::shared_ptr<MarmotType> function_type = MarmotType::MakeFunctionType(function.m_param_types, std::move(return_type_copy));
		MarmotType::FunctionType& signature = function_type->GetType<MarmotType::FunctionType>();
		signature.m_is_transferable = !def.m_binding.has_value();
		signature.m_constraints = function.m_constraints;
		ApplyTransferableParameterConstraints(signature);
		function.m_type_data = function_type;
		def.m_value->GetType() = function.m_type_data;

		if (def.m_annotated_type.has_value())
		{
			std::shared_ptr<MarmotType>& annotated_type = def.m_annotated_type.value();
			MarmotResult::TypeResult annotation_result = Unify(context, def.m_name, function_type, annotated_type, UnifyDiagnosticMode::ActualExpected);
			if (!annotation_result.has_value())
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Define statement type error: function type annotation doesn't match function signature", def.m_name, m_file_name, m_source_lines, function_type, annotated_type));
			}

			function.m_type_data = ApplySubstitution(context, function_type);
			def.m_value->GetType() = function.m_type_data;
			const MarmotType::FunctionType& resolved_function_type = function.m_type_data->GetType<MarmotType::FunctionType>();
			function.m_param_types = resolved_function_type.m_param_types;
			function.m_return_type = resolved_function_type.m_return_type;
		}

		BindValue(context, def.m_name, def.m_binding, def.m_value->GetType());
		MarmotType::FunctionType& function_type_ref = def.m_value->GetType()->GetType<MarmotType::FunctionType>();

		return InScope
		(
			context,
			[&](const CheckingContext& context) -> MarmotResult::TypeResult
			{
				BindParameters(context, function.m_params, function_type_ref.m_param_types);

				return CheckLambda(context.WithExpectedType(def.m_annotated_type.value_or(nullptr)), function, def.m_binding.value_or(nullptr))
					.and_then
					(
						[&def, &function, this, &context](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
						{
							std::shared_ptr<MarmotType> resolved_function_type = ApplySubstitution(context, def.m_value->GetType());
							if (def.m_annotated_type.has_value())
							{
								MarmotResult::TypeResult annotation_result = Unify(context, def.m_name, resolved_function_type, *def.m_annotated_type, UnifyDiagnosticMode::ActualExpected);
								if (!annotation_result.has_value())
								{
									return annotation_result;
								}
							}
							def.m_value->GetType() = resolved_function_type;
							function.m_type_data = resolved_function_type;

							if (resolved_function_type->IsType<MarmotType::FunctionType>())
							{
								const MarmotType::FunctionType& resolved_signature = resolved_function_type->GetType<MarmotType::FunctionType>();
								function.m_param_types = resolved_signature.m_param_types;
								function.m_return_type = resolved_signature.m_return_type;
							}

							if (HasTypeVariables(resolved_function_type))
							{
								return std::unexpected
								(
									MarmotError::GenerateTypeCheckerErrorWithContext
									(
										"Function expression type error: could not infer all lambda parameter or return types",
										function.m_function_keyword,
										m_file_name,
										m_source_lines
									)
								);
							}

							if (std::shared_ptr<MarmotType>* binding = def.m_binding.has_value() ? FindBindingType(context, def.m_binding.value()) : FindNameType(context, def.m_name.m_lexeme))
							{
								*binding = resolved_function_type;
							}

							return MarmotType::MakeUndecidedType();
						}
					);
			}
		);
	}

	// Set expected type from annotation if present
	const CheckingContext initializer_context = def.m_annotated_type.has_value() ? context.WithExpectedType(def.m_annotated_type.value()) : context;

	// General case
	return Evaluate(initializer_context, def.m_value)
		.and_then
		(
			[&def, this, &initializer_context](std::shared_ptr<MarmotType>&& type)->MarmotResult::TypeResult
			{
				if (def.m_annotated_type.has_value())
				{
					std::shared_ptr<MarmotType>& annotated_type = def.m_annotated_type.value();

					// Reject NeverType values with concrete type annotations
					// NeverType represents expressions that never return (infinite loops, unconditional returns/breaks)
					// They cannot be assigned to variables with concrete types
					if (type->IsType<MarmotType::NeverType>() && !annotated_type->IsType<MarmotType::NeverType>())
					{
						return std::unexpected(
							MarmotError::GenerateTypeCheckerErrorWithContext
							(
								"Cannot assign a never-returning expression to a variable with type " + annotated_type->DisplayString(),
								def.m_name,
								m_file_name,
								m_source_lines
							)
						);
					}

					return Unify(initializer_context, def.m_name, annotated_type, type, UnifyDiagnosticMode::ExpectedActual)
						.and_then
						(
							[&def, &annotated_type, this, &initializer_context](std::shared_ptr<MarmotType>&& unified_type)->MarmotResult::TypeResult
							{
								BindValue(initializer_context, def.m_name, def.m_binding, annotated_type);
								return unified_type;
							}
						);
				}

				// Check if this is an empty array without type annotation
				if (type->IsType<MarmotType::ArrayType>())
				{
					const MarmotType::ArrayType& array_type = type->GetType<MarmotType::ArrayType>();
					if (array_type.m_element_type->IsType<MarmotType::UndecidedType>())
					{
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Define statement type error: empty arrays require type annotations. Use 'def " + def.m_name.m_lexeme + " : Array<ElementType> = [];'", def.m_name, m_file_name, m_source_lines));
					}
				}

				BindValue(initializer_context, def.m_name, def.m_binding, type);
				return MarmotType::MakeUndecidedType();
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotStatement::TupleDefinition& def_tuple)
{
	return Evaluate(context, def_tuple.m_value)
		.and_then
		(
			[&def_tuple, this, &context](std::shared_ptr<MarmotType>&& type)->MarmotResult::TypeResult
			{
				if (!type->IsType<MarmotType::TupleType>())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("DefineTuple statement type error: expected tuple expression on right-hand side", def_tuple.m_names[0], m_file_name, m_source_lines));
				}

				const MarmotType::TupleType& tuple_type = type->GetType<MarmotType::TupleType>();


				if (def_tuple.m_names.size() != tuple_type.m_element_types.size())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("DefineTuple statement type error: tuple pattern has " + std::to_string(def_tuple.m_names.size()) + " bindings but tuple has " + std::to_string(tuple_type.m_element_types.size()) + " elements", def_tuple.m_names[0u], m_file_name, m_source_lines));
				}

				for (size_t i = 0u; i < def_tuple.m_names.size(); i += 1u)
				{
					BindValue(context, def_tuple.m_names[i], def_tuple.m_bindings[i], tuple_type.m_element_types[i]);
				}

				return MarmotType::MakeUndecidedType();
			}
		);
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotStatement::FunctionDefinition& defun)
{
	// If function has generic parameters, check for lambda syntax error
	if (!defun.m_generic_params.empty())
	{
		// Validate that generic parameter names are unique
		std::unordered_set<std::string> generic_param_names;
		for (const Token& generic_param : defun.m_generic_params)
		{
			if (!generic_param_names.insert(generic_param.m_lexeme).second)
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("DefineFunction type error: duplicate generic parameter name", generic_param, m_file_name, m_source_lines));
			}
		}

		// Validate that generic parameters don't conflict with function parameters
		for (size_t index = 0u; index < defun.m_params.Size(); index += 1u)
		{
			if (defun.m_params.GeneratedBindingAt(index) != nullptr)
			{
				continue;
			}
			const Token& param = defun.m_params.SourceToken(index);
			if (generic_param_names.contains(param.m_lexeme))
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("DefineFunction type error: generic parameter conflicts with function parameter", param, m_file_name, m_source_lines));
			}
		}
	}

	std::vector<MarmotType::ClassConstraint> propagated_constraints = CollectSignatureConstraints(defun.m_param_types, defun.m_return_type);
	for (MarmotType::ClassConstraint& propagated_constraint : propagated_constraints)
	{
		AppendUniqueConstraint(defun.m_constraints, std::move(propagated_constraint));
	}

	for (const MarmotType::ClassConstraint& constraint : defun.m_constraints)
	{
		if (!m_classes.contains(ConstrainedClassName(constraint)))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("DefineFunction type error: undefined class '" + ConstrainedClassName(constraint) + "' in constraint", defun.m_name, m_file_name, m_source_lines));
		}

		// An equality constraint names no class of its own, so the arity check
		// below does not apply to it.
		if (constraint.IsEquality())
		{
			continue;
		}

		const ClassInfo& tc_info = m_classes.at(constraint.m_class_name);

		if (constraint.m_type_args.size() != tc_info.m_type_param_names.size())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("DefineFunction type error: class '" + constraint.m_class_name + "' expects " + std::to_string(tc_info.m_type_param_names.size()) + " type argument(s) but got " + std::to_string(constraint.m_type_args.size()), defun.m_name, m_file_name, m_source_lines));
		}
	}

	TypeTransform::Freshener freshening_context([this]() { return FreshTypeVar(); });
	for (std::shared_ptr<MarmotType>& param_type : defun.m_param_types)
	{
		param_type = freshening_context.Apply(param_type);
	}
	defun.m_return_type = freshening_context.Apply(defun.m_return_type);
	// An equality constraint's types are its operands, which Map freshens with the
	// signature: otherwise the projection would never match the one built at the
	// use site.
	defun.m_constraints = MarmotType::MapConstraints(defun.m_constraints, [&freshening_context](const std::shared_ptr<MarmotType>& constraint_type) { return freshening_context.Apply(constraint_type); });

	std::shared_ptr<MarmotType> function_type = MakeGlobalFunctionType(defun.m_param_types, defun.m_return_type, defun.m_constraints);
	MarmotType::FunctionType& signature = function_type->GetType<MarmotType::FunctionType>();
	defun.m_param_types = signature.m_param_types;
	defun.m_constraints = signature.m_constraints;
	BindValue(context, defun.m_name, defun.m_binding, function_type);
	if (!defun.m_generic_params.empty())
	{
		m_generic_functions.emplace(defun.m_name.m_lexeme, defun.m_params.Function());
	}

	return InScope
	(
		context,
		[&](const CheckingContext& context) -> MarmotResult::TypeResult
		{
			for (const Token& generic_param : defun.m_generic_params)
			{
				const std::string& param_name = generic_param.m_lexeme;
				context.m_scope->m_names.emplace(param_name, freshening_context.Parameter(param_name));
			}
			BindParameters(context, defun.m_params, defun.m_param_types);

			const GenericContext generic = ForGenericParameters(context, freshening_context.Bindings(), nullptr);
			const size_t pending_start = m_pending_instances.size();
			return CheckFunctionBody(generic.m_context, defun.m_body, defun.m_return_type, defun.m_constraints, defun.m_name)
				.and_then
				(
					[&defun, &generic, pending_start, this](std::shared_ptr<MarmotType>&&) -> MarmotResult::TypeResult
					{
						if (std::optional<CompilerError> error = CheckGenericParametersStayGeneric(generic.m_context, generic.m_parameters, defun.m_generic_params, defun.m_name))
						{
							return std::unexpected(std::move(*error));
						}
						if (std::optional<CompilerError> error = CheckDecidedPendingInstances(generic.m_context, pending_start))
						{
							return std::unexpected(std::move(*error));
						}
						return defun.m_return_type;
					}
				);
		}
	);
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotStatement::ForeignDefinition& foreign)
{
	if (foreign.DeclaredPrimitive().has_value())
	{
		if (std::optional<CompilerError> error = CheckPrimitiveDeclaration(foreign))
		{
			return std::unexpected(std::move(*error));
		}
		if (foreign.DeclaredPrimitive() == MarmotPrimitive::ChannelSend)
		{
			MarmotType::FunctionType& signature = foreign.m_type->GetType<MarmotType::FunctionType>();
			signature.m_constraints.push_back(MarmotType::ClassConstraint(std::string(TRANSFERABLE_CLASS_NAME), { signature.m_param_types[1u] }));
		}
	}

	BindValue(context, foreign.m_function_name, foreign.m_binding, foreign.m_type);
	return MarmotType::MakeUndecidedType();
}

// The VM gives a primitive's instruction only what its declaration says, so the
// declaration must say what the instruction takes and gives.
std::optional<CompilerError> TypeChecker::CheckPrimitiveDeclaration(const MarmotStatement::ForeignDefinition& foreign) const
{
	const MarmotType::FunctionType& signature = foreign.m_type->GetType<MarmotType::FunctionType>();
	const std::vector<std::shared_ptr<MarmotType>>& params = signature.m_param_types;
	const std::shared_ptr<MarmotType>& result = signature.m_return_type;
	const bool takes_one = params.size() == 1u;
	struct DeclaredShape
	{
		bool m_matches;
		std::string_view m_expected;
	};
	const DeclaredShape shape = [&]() -> DeclaredShape
	{
		if (const MarmotOperator* operation = PrimitiveOperator(foreign.DeclaredPrimitive().value()))
		{
			const size_t arity = operation->m_unary ? 1u : 2u;
			bool matches = params.size() == arity && params[0u]->IsNumericType() && *params[0u] == *result;
			if (matches && operation->m_integer_only)
			{
				matches = !params[0u]->IsType<MarmotType::FloatType>();
			}
			if (matches && !operation->m_unary)
			{
				matches = operation->m_shift ? params[1u]->IsType<MarmotType::IntegerType>() : *params[0u] == *params[1u];
			}
			return { matches, "a concrete numeric signature with matching operand and result types (an Int shift amount)" };
		}
		switch (foreign.DeclaredPrimitive().value())
		{
		case MarmotPrimitive::Spawn:
			return { params.size() == 2u && IsSpawnSignature(params[0u], params[1u], result), "fn(A, fn(A) -> R) -> Worker<R>" };
		case MarmotPrimitive::Join:
			return { takes_one && params[0u]->IsType<MarmotType::WorkerType>() && IsJoinOutcome(result, params[0u]->GetType<MarmotType::WorkerType>().m_result_type), "fn(Worker<T>) -> R, a union whose member 0 holds T and member 1 an error, itself a union whose member 0 holds nothing and member 1 a Text" };
		case MarmotPrimitive::ChannelNew:
			return { takes_one && params[0u]->IsType<MarmotType::IntegerType>() && result->IsType<MarmotType::ChannelType>(), "fn(Int) -> Channel<T>" };
		case MarmotPrimitive::ChannelClose:
			return { takes_one && params[0u]->IsType<MarmotType::ChannelType>() && result->IsType<MarmotType::UnitType>(), "fn(Channel<T>) -> Unit" };
		case MarmotPrimitive::ChannelSend:
			return
			{
				params.size() == 2u && params[0u]->IsType<MarmotType::ChannelType>()
					&& *params[0u]->GetType<MarmotType::ChannelType>().m_element_type == *params[1u]
					&& result->IsType<MarmotType::BoolType>(),
				"fn(Channel<T>, T) -> Bool"
			};
		case MarmotPrimitive::ChannelReceive:
			return
			{
				takes_one && params[0u]->IsType<MarmotType::ChannelType>()
					&& IsReceiveOutcome(result, params[0u]->GetType<MarmotType::ChannelType>().m_element_type),
				"fn(Channel<T>) -> R, a union whose member 0 holds nothing and member 1 holds T"
			};
		case MarmotPrimitive::WorkerIsDone:
		case MarmotPrimitive::WorkerCancel:
			return { takes_one && params[0u]->IsType<MarmotType::WorkerType>() && result->IsType<MarmotType::BoolType>(), "fn(Worker<T>) -> Bool" };
		default:
			std::unreachable();
		}
		std::unreachable();
	}();

	if (shape.m_matches)
	{
		return std::nullopt;
	}
	return MarmotError::GenerateTypeCheckerErrorWithContext(std::format("Foreign declaration error: '{}' declares the primitive \"{}\", whose type must be {}", foreign.m_function_name.m_lexeme, foreign.m_foreign_name, shape.m_expected), foreign.m_function_name, m_file_name, m_source_lines);
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotStatement::Struct& struct_stmt)
{
	if (!struct_stmt.m_generic_params.empty())
	{
		std::unordered_set<std::string> generic_param_names;
		for (const Token& generic_param : struct_stmt.m_generic_params)
		{
			if (!generic_param_names.insert(generic_param.m_lexeme).second)
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Struct declaration type error: duplicate generic parameter name", generic_param, m_file_name, m_source_lines));
			}
		}

		m_generic_structs.insert(ConstructorKey(struct_stmt.m_self_type->GetType<MarmotType::StructType>().m_module_name, struct_stmt.m_name.m_lexeme));
	}

	if (!struct_stmt.m_constraints.empty())
	{
		if (struct_stmt.m_generic_params.empty())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Struct declaration error: constraints require at least one type parameter", struct_stmt.m_name, m_file_name, m_source_lines));
		}

		for (const MarmotType::ClassConstraint& constraint : struct_stmt.m_constraints)
		{
			if (!m_classes.contains(ConstrainedClassName(constraint)))
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Struct declaration error: undefined class '" + ConstrainedClassName(constraint) + "' in constraint", struct_stmt.m_name, m_file_name, m_source_lines));
			}

			// An equality constraint names no class of its own, so the arity check
			// below does not apply to it.
			if (constraint.IsEquality())
			{
				continue;
			}

			const ClassInfo& tc_info = m_classes.at(constraint.m_class_name);
			if (constraint.m_type_args.size() != tc_info.m_type_param_names.size())
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Struct declaration error: class '" + constraint.m_class_name + "' expects " + std::to_string(tc_info.m_type_param_names.size()) + " type argument(s) but got " + std::to_string(constraint.m_type_args.size()), struct_stmt.m_name, m_file_name, m_source_lines));
			}
		}
	}

	struct_stmt.m_self_type->GetType<MarmotType::StructType>().m_constraints = struct_stmt.m_constraints;
	const std::string struct_module_name = struct_stmt.m_self_type->GetType<MarmotType::StructType>().m_module_name;
	std::shared_ptr<MarmotType> struct_constructor_type = MarmotType::MakeFunctionType(struct_stmt.m_self_type->GetType<MarmotType::StructType>().m_member_types, std::move(struct_stmt.m_self_type));
	struct_constructor_type->GetType<MarmotType::FunctionType>().m_constraints = struct_stmt.m_constraints;
	context.m_scope->m_names[struct_stmt.m_name.m_lexeme] = struct_constructor_type;
	context.m_scope->m_names[ConstructorKey(struct_module_name, struct_stmt.m_name.m_lexeme)] = struct_constructor_type;
	m_struct_type_definitions[ConstructorKey(struct_module_name, struct_stmt.m_name.m_lexeme)] = struct_constructor_type->GetType<MarmotType::FunctionType>().m_return_type;

	return MarmotType::MakeUndecidedType();
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotStatement::Union& union_stmt)
{
	if (!union_stmt.m_generic_params.empty())
	{
		std::unordered_set<std::string> generic_param_names;
		for (const Token& generic_param : union_stmt.m_generic_params)
		{
			if (!generic_param_names.insert(generic_param.m_lexeme).second)
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Union declaration type error: duplicate generic parameter name", generic_param, m_file_name, m_source_lines));
			}
		}

		m_generic_unions.insert(ConstructorKey(union_stmt.m_self_type->GetType<MarmotType::UnionType>().m_module_name, union_stmt.m_name.m_lexeme));
	}

	if (!union_stmt.m_constraints.empty())
	{
		if (union_stmt.m_generic_params.empty())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Union declaration error: constraints require at least one type parameter", union_stmt.m_name, m_file_name, m_source_lines));
		}

		for (const MarmotType::ClassConstraint& constraint : union_stmt.m_constraints)
		{
			if (!m_classes.contains(ConstrainedClassName(constraint)))
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Union declaration error: undefined class '" + ConstrainedClassName(constraint) + "' in constraint", union_stmt.m_name, m_file_name, m_source_lines));
			}

			// An equality constraint names no class of its own, so the arity check
			// below does not apply to it.
			if (constraint.IsEquality())
			{
				continue;
			}

			const ClassInfo& tc_info = m_classes.at(constraint.m_class_name);
			if (constraint.m_type_args.size() != tc_info.m_type_param_names.size())
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Union declaration error: class '" + constraint.m_class_name + "' expects " + std::to_string(tc_info.m_type_param_names.size()) + " type argument(s) but got " + std::to_string(constraint.m_type_args.size()), union_stmt.m_name, m_file_name, m_source_lines));
			}
		}
	}

	MarmotType::UnionType& union_type = union_stmt.m_self_type->GetType<MarmotType::UnionType>();
	union_type.m_constraints = union_stmt.m_constraints;
	for (auto& [member_name, member_ctx] : union_type.m_member_info)
	{
		std::vector<std::shared_ptr<MarmotType>> member_types_copy = member_ctx.m_member_types;
		std::shared_ptr<MarmotType> union_constructor_type = MarmotType::MakeFunctionType(std::move(member_types_copy), std::shared_ptr(union_stmt.m_self_type));
		union_constructor_type->GetType<MarmotType::FunctionType>().m_constraints = union_stmt.m_constraints;
		context.m_scope->m_names[member_name] = union_constructor_type;
		context.m_scope->m_names[ConstructorKey(union_type.m_module_name, member_name)] = union_constructor_type;
	}
	m_union_type_definitions[ConstructorKey(union_type.m_module_name, union_stmt.m_name.m_lexeme)] = union_stmt.m_self_type;

	return MarmotType::MakeUndecidedType();
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext&, MarmotStatement::Class& class_stmt)
{
	std::unordered_set<std::string> type_param_names;
	for (const Token& type_param : class_stmt.m_type_params)
	{
		if (!type_param_names.insert(type_param.m_lexeme).second)
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Class declaration error: duplicate type parameter name", type_param, m_file_name, m_source_lines));
		}
	}

	if (m_classes.contains(class_stmt.m_name.m_lexeme))
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Class declaration error: class already defined", class_stmt.m_name, m_file_name, m_source_lines));
	}

	AssociatedTypeEnvironment associated_types;
	for (const MarmotStatement::Class::AssociatedTypeDeclaration& associated_type_decl : class_stmt.m_associated_types)
	{
		if (type_param_names.contains(associated_type_decl.m_name.m_lexeme))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Class declaration error: associated type name conflicts with class type parameter", associated_type_decl.m_name, m_file_name, m_source_lines));
		}
		if (associated_types.contains(associated_type_decl.m_name.m_lexeme))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Class declaration error: duplicate associated type name", associated_type_decl.m_name, m_file_name, m_source_lines));
		}

		std::vector<std::shared_ptr<MarmotType>> associated_type_args;
		associated_type_args.reserve(class_stmt.m_type_params.size());
		for (const Token& type_param : class_stmt.m_type_params)
		{
			associated_type_args.emplace_back(MarmotType::MakeGenericType(type_param.m_lexeme));
		}

		associated_types.emplace
		(
			associated_type_decl.m_name.m_lexeme,
			MarmotType::MakeAssociatedType(class_stmt.m_name.m_lexeme, associated_type_decl.m_name.m_lexeme, std::move(associated_type_args))
		);
	}

	TypeEnvironment method_types;
	for (std::unique_ptr<MarmotStatement>& method : class_stmt.m_methods)
	{
		if (!method->IsStatement<MarmotStatement::FunctionDefinition>())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Class declaration error: methods must be function definitions", class_stmt.m_name, m_file_name, m_source_lines));
		}

		MarmotStatement::FunctionDefinition& defun = method->GetStatement<MarmotStatement::FunctionDefinition>();
		if (method_types.contains(defun.m_name.m_lexeme))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Class declaration error: duplicate method name", defun.m_name, m_file_name, m_source_lines));
		}

		std::shared_ptr<MarmotType> method_type = MarmotType::MakeFunctionType(defun.m_param_types, std::shared_ptr<MarmotType>(defun.m_return_type));

		method_types[defun.m_name.m_lexeme] = method_type;
	}

	std::vector<std::string> param_names;
	for (const Token& param : class_stmt.m_type_params)
	{
		param_names.emplace_back(param.m_lexeme);
	}

	m_classes[class_stmt.m_name.m_lexeme] = ClassInfo(class_stmt.m_name.m_lexeme, std::move(param_names), std::move(associated_types), std::move(method_types));

	return MarmotType::MakeUndecidedType();
}

// An instance is registered with the declarations, so code anywhere in the
// module can select it; its method bodies are checked where it stands.
MarmotResult::TypeResult TypeChecker::RegisterInstance(const CheckingContext& context, MarmotStatement::Instance& instance_stmt)
{
	std::unordered_map<std::string, ClassInfo>::iterator tc_it = m_classes.find(instance_stmt.m_class_name.m_lexeme);
	if (tc_it == m_classes.end())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: unknown class '" + instance_stmt.m_class_name.m_lexeme + "'", instance_stmt.m_class_name, m_file_name, m_source_lines));
	}

	const ClassInfo& tc_info = tc_it->second;
	if (instance_stmt.m_type_args.size() != tc_info.m_type_param_names.size())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: type argument count mismatch", instance_stmt.m_class_name, m_file_name, m_source_lines));
	}

	// Numeric describes primitive range bounds; an instance cannot extend the
	// range instructions to another representation.
	const std::shared_ptr<MarmotType>& first_type_arg = instance_stmt.m_type_args.front();
	if (instance_stmt.m_class_name.m_lexeme == NUMERIC_CLASS_NAME && !first_type_arg->IsNumericType())
	{
		return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode::TypeUnsatisfiedConstraint, std::format("Instance declaration error: primitive ranges require Int, Float, Byte or Word, so there is no Numeric<{}>", first_type_arg->DisplayString()), instance_stmt.m_class_name, m_file_name, m_source_lines));
	}

	std::vector<std::string> concrete_type_names;
	for (const std::shared_ptr<MarmotType>& type_arg : instance_stmt.m_type_args)
	{
		concrete_type_names.push_back(type_arg->ToString());
	}

	InstanceKey instance_key{instance_stmt.m_class_name.m_lexeme, concrete_type_names};

	std::unordered_map<InstanceKey, InstanceInfo, InstanceKeyHash>::iterator existing_instance_it = m_instances.find(instance_key);
	if (existing_instance_it != m_instances.end())
	{
		// A derived identity conversion is only a stand-in for the instance the user did
		// not write. An explicit declaration with real behaviour replaces it rather than
		// colliding with it; the type alias that derived it is always checked first, so
		// this is the only order in which the two can meet.
		if (!existing_instance_it->second.m_is_derived)
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: instance already defined for this type", instance_stmt.m_class_name, m_file_name, m_source_lines));
		}

		m_instances.erase(existing_instance_it);
	}

	AssociatedTypeEnvironment associated_type_bindings;
	for (const MarmotStatement::Instance::AssociatedTypeBinding& binding : instance_stmt.m_associated_types)
	{
		if (!tc_info.m_associated_types.contains(binding.m_name.m_lexeme))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: unknown associated type '" + binding.m_name.m_lexeme + "'", binding.m_name, m_file_name, m_source_lines));
		}
		if (associated_type_bindings.contains(binding.m_name.m_lexeme))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: duplicate associated type binding", binding.m_name, m_file_name, m_source_lines));
		}

		associated_type_bindings.emplace(binding.m_name.m_lexeme, binding.m_type);
	}

	for (const auto& [associated_type_name, _] : tc_info.m_associated_types)
	{
		if (!associated_type_bindings.contains(associated_type_name))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: missing binding for associated type '" + associated_type_name + "'", instance_stmt.m_class_name, m_file_name, m_source_lines));
		}
	}

	m_instances.emplace
	(
		instance_key,
		InstanceInfo
		(
			instance_stmt.m_class_name.m_lexeme,
			std::vector<std::shared_ptr<MarmotType>>(instance_stmt.m_type_args),
			std::vector<MarmotType::ClassConstraint>(instance_stmt.m_constraints),
			AssociatedTypeEnvironment(associated_type_bindings),
			std::unordered_map<std::string, std::unique_ptr<MarmotStatement>>()
		)
	);

	struct InstanceRegistrationGuard
	{
		std::unordered_map<InstanceKey, InstanceInfo, InstanceKeyHash>* m_instances = nullptr;
		InstanceKey m_key;
		bool m_keep = false;

		~InstanceRegistrationGuard()
		{
			if (m_instances != nullptr && !m_keep)
			{
				m_instances->erase(m_key);
			}
		}
	} registration_guard{ &m_instances, instance_key, false };

	std::unordered_set<std::string> implemented_methods;
	for (const MarmotStatement::Instance::Method& method : instance_stmt.m_methods)
	{
		const std::string& method_name = method.m_name.m_lexeme;

		if (!tc_info.m_method_types.contains(method_name))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: method '" + method_name + "' not defined in typeclass", method.m_name, m_file_name, m_source_lines));
		}

		if (implemented_methods.contains(method_name))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: duplicate method implementation", method.m_name, m_file_name, m_source_lines));
		}

		implemented_methods.emplace(method_name);
	}

	for (const auto& [method_name, method_type] : tc_info.m_method_types)
	{
		if (!implemented_methods.contains(method_name))
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: missing implementation for method '" + method_name + "'", instance_stmt.m_class_name, m_file_name, m_source_lines));
		}
	}

	TypeEnvironment type_param_substitutions;
	for (size_t i = 0u; i < tc_info.m_type_param_names.size(); i += 1u)
	{
		type_param_substitutions.emplace(tc_info.m_type_param_names[i], instance_stmt.m_type_args[i]);
	}

	for (const MarmotStatement::Instance::Method& method : instance_stmt.m_methods)
	{
		const MarmotStatement::FunctionDefinition& defun = method.m_definition;
		const std::string& method_name = method.m_name.m_lexeme;

		if (!defun.m_generic_params.empty())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: instance methods cannot declare generic parameters", method.m_name, m_file_name, m_source_lines));
		}

		TypeEnvironment::const_iterator expected_it = tc_info.m_method_types.find(method_name);
		if (expected_it == tc_info.m_method_types.cend())
		{
			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: method '" + method_name + "' not defined in class", method.m_name, m_file_name, m_source_lines));
		}

		std::shared_ptr<MarmotType> expected_signature = ApplySubstitution(context, TypeTransform::Instantiate(expected_it->second, type_param_substitutions));
		std::shared_ptr<MarmotType> actual_signature = ApplySubstitution(context, MarmotType::MakeFunctionType(defun.m_param_types, std::shared_ptr<MarmotType>(defun.m_return_type)));

		if (*expected_signature != *actual_signature)
		{
			if (expected_signature->IsType<MarmotType::FunctionType>() && actual_signature->IsType<MarmotType::FunctionType>())
			{
				const MarmotType::FunctionType& expected_type = expected_signature->GetType<MarmotType::FunctionType>();
				const MarmotType::FunctionType& actual_type = actual_signature->GetType<MarmotType::FunctionType>();

				if (expected_type.m_param_types.size() != actual_type.m_param_types.size())
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: method '" + method_name + "' parameter count does not match class declaration", method.m_name, m_file_name, m_source_lines, actual_signature, expected_signature));
				}

				for (size_t i = 0u; i < expected_type.m_param_types.size(); i += 1u)
				{
					const std::shared_ptr<MarmotType>& expected_param = expected_type.m_param_types[i];
					const std::shared_ptr<MarmotType>& actual_param = actual_type.m_param_types[i];
					if (*expected_param != *actual_param)
					{
						return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext(std::format("Instance declaration error: method '{}' parameter {} type does not match class declaration", method_name, i + 1u), method.m_name, m_file_name, m_source_lines, actual_param, expected_param));
					}
				}

				if (*expected_type.m_return_type != *actual_type.m_return_type)
				{
					return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: method '" + method_name + "' return type does not match class declaration", method.m_name, m_file_name, m_source_lines, actual_type.m_return_type, expected_type.m_return_type));
				}
			}

			return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Instance declaration error: method '" + method_name + "' signature does not match class declaration", method.m_name, m_file_name, m_source_lines, actual_signature, expected_signature));
		}
	}

	registration_guard.m_keep = true;
	return MarmotType::MakeUndecidedType();
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext& context, MarmotStatement::Instance& instance_stmt)
{
	// AppendUniqueConstraint does NOT make this append idempotent: on a second visit the stored
	// constraint has been freshened in place by the function-definition path while the incoming one has not,
	// so they compare unequal and a duplicate would accumulate. Safety comes instead from this
	// statement being visited at most once - TypeCheck traverses m_program_tree in a single pass,
	// the duplicate-instance guard above rejects a repeated instance key before reaching here,
	// and each module gets its own TypeChecker owning its own tree (imports carry instance
	// metadata, not Instance AST nodes). Preserve that property before reusing this loop.
	for (MarmotStatement::Instance::Method& method : instance_stmt.m_methods)
	{
		MarmotStatement::FunctionDefinition& defun = method.m_definition;
		for (const MarmotType::ClassConstraint& constraint : instance_stmt.m_constraints)
		{
			AppendUniqueConstraint(defun.m_constraints, MarmotType::ClassConstraint(constraint));
		}

		MarmotResult::TypeResult result = (*this)(context, defun);
		if (!result.has_value())
		{
			return result;
		}
	}

	return MarmotType::MakeUndecidedType();
}

MarmotResult::TypeResult TypeChecker::operator()(const CheckingContext&, MarmotStatement::TypeAlias& type_alias)
{
	if (!type_alias.m_generic_params.empty())
	{
		std::unordered_set<std::string> generic_param_names;
		for (const Token& generic_param : type_alias.m_generic_params)
		{
			if (!generic_param_names.insert(generic_param.m_lexeme).second)
			{
				return std::unexpected(MarmotError::GenerateTypeCheckerErrorWithContext("Type alias declaration error: duplicate generic parameter name", generic_param, m_file_name, m_source_lines));
			}
		}
	}

	if (type_alias.m_aliased_type->IsType<MarmotType::NewType>())
	{
		const MarmotType::NewType& new_type = type_alias.m_aliased_type->GetType<MarmotType::NewType>();
		RegisterIdentityConversion(type_alias.m_aliased_type, new_type.m_representation);
		RegisterIdentityConversion(new_type.m_representation, type_alias.m_aliased_type);
	}

	// Type alias is already registered in the parser's type table
	// Nothing more to do at type-checking time - the alias is resolved at parse time
	return MarmotType::MakeUndecidedType();
}

void TypeChecker::RegisterIdentityConversion(const std::shared_ptr<MarmotType>& from_type, const std::shared_ptr<MarmotType>& to_type)
{
	InstanceKey conversion_key{ std::string(CONVERTIBLE_CLASS_NAME), { from_type->ToString(), to_type->ToString() } };
	if (m_instances.contains(conversion_key))
	{
		return;
	}

	std::vector<std::shared_ptr<MarmotType>> type_args{ from_type, to_type };
	m_instances.emplace
	(
		std::move(conversion_key),
		InstanceInfo(std::string(CONVERTIBLE_CLASS_NAME), std::move(type_args), {}, {}, {}, true)
	);
}
