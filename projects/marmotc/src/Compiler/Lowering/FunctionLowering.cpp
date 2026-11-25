#include "FunctionLowering.h"
#include "Bytecode/Intrinsics/IntrinsicTable.h"
#include "Compiler/Names/SpecialNames.h"
#include "Compiler/Operator/Operator.h"
#include "Support/Expected.h"
#include "Support/Optional.h"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <utility>

namespace
{
	using TypeRef = std::shared_ptr<MarmotType>;

	MarmotIROp InstructionOf(MarmotPrimitive primitive)
	{
		switch (primitive)
		{
		case MarmotPrimitive::Spawn:
			return MarmotIROp::Spawn;
		case MarmotPrimitive::Join:
			return MarmotIROp::Join;
		case MarmotPrimitive::ChannelNew:
			return MarmotIROp::ChannelNew;
		case MarmotPrimitive::ChannelClose:
			return MarmotIROp::ChannelClose;
		case MarmotPrimitive::ChannelSend:
			return MarmotIROp::Send;
		case MarmotPrimitive::ChannelReceive:
			return MarmotIROp::Receive;
		case MarmotPrimitive::WorkerIsDone:
			return MarmotIROp::WorkerIsDone;
		case MarmotPrimitive::WorkerCancel:
			return MarmotIROp::WorkerCancel;
		default:
			std::unreachable();
		}
		std::unreachable();
	}

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

	bool IsNever(const TypeRef& type)
	{
		return type->IsType<MarmotType::NeverType>();
	}

	const MarmotExpression::NameAccess* GlobalName(const MarmotExpression& expression)
	{
		if (!expression.IsExpression<MarmotExpression::NameAccess>())
		{
			return nullptr;
		}
		const MarmotExpression::NameAccess& name = expression.GetExpression<MarmotExpression::NameAccess>();
		return std::holds_alternative<MarmotExpression::NameContext::Global>(name.m_name_ctx) ? &name : nullptr;
	}
}

Lowering::FunctionLowering::JoinPoint::JoinPoint(MarmotIRBlockId block, TypeRef type)
	: m_block(block),
	m_type(std::move(type))
{
}

Lowering::FunctionLowering::FunctionLowering(Lowering& owner, MarmotIRFunctionId id, const Specialization& specialization, FunctionLowering* parent, LexicalFunctionId identity, MarmotCompiler::Optional<ActiveGenericLambda> generic_lambda)
	: m_owner(owner),
	m_id(id),
	m_function(owner.m_functions[id.m_index]),
	m_builder(m_function),
	m_specialization(specialization),
	m_parent(parent),
	m_identity(std::move(identity)),
	m_live_blocks(1u, true),
	m_generic_lambda(std::move(generic_lambda))
{
}

Lowering::FunctionLowering::ActiveGenericLambda::ActiveGenericLambda(const MarmotExpression::Function* function, std::string signature, LocalKey self)
	: m_function(function),
	m_signature(std::move(signature)),
	m_self(self)
{
}

Lowering::Emitted Lowering::FunctionLowering::LowerTopLevel()
{
	for (const std::unique_ptr<MarmotStatement>& statement : m_owner.m_program)
	{
		const Emitted lowered = LowerTopLevelStatement(*statement);
		if (!lowered.HasValue())
		{
			return lowered;
		}
	}
	m_builder.AtLine(0).Return(m_builder.ConstUnit());
	return {};
}

Lowering::TypeRef Lowering::FunctionLowering::Concrete(const TypeRef& type)
{
	const GenericTypes::TypeEnvironment& types = m_specialization.m_types;
	return types.empty() ? type : m_owner.Substitute(type, types);
}

Lowering::TypeRef Lowering::FunctionLowering::TypeOf(const MarmotExpression& expression)
{
	return Concrete(expression.GetType());
}

Lowering::TypeRef Lowering::FunctionLowering::NominalType(const MarmotExpression& expression, MarmotIRValueId value)
{
	const TypeRef type = TypeOf(expression);
	return !IsNever(type) && GenericTypes::IsConcrete(type) ? type : m_function.TypeOf(value);
}

MarmotIRBlockId Lowering::FunctionLowering::NewBlock()
{
	m_live_blocks.push_back(false);
	return m_builder.CreateBlock();
}

bool Lowering::FunctionLowering::IsDead()
{
	return !m_live_blocks[m_builder.CurrentBlock().m_index];
}

void Lowering::FunctionLowering::PositionAt(MarmotIRBlockId block)
{
	m_builder.PositionAt(block);
}

void Lowering::FunctionLowering::Jump(MarmotIRBlockId target, std::vector<MarmotIRValueId> arguments)
{
	if (!IsDead())
	{
		m_live_blocks[target.m_index] = true;
	}
	m_builder.Jump(target, std::move(arguments));
}

void Lowering::FunctionLowering::Branch(MarmotIRValueId condition, MarmotIRSuccessor if_true, MarmotIRSuccessor if_false)
{
	if (!IsDead())
	{
		m_live_blocks[if_true.m_block.m_index] = true;
		m_live_blocks[if_false.m_block.m_index] = true;
	}
	m_builder.Branch(condition, std::move(if_true), std::move(if_false));
}

MarmotIRValueId Lowering::FunctionLowering::Placeholder(const TypeRef& type)
{
	return m_builder.Emit(MarmotIROp::Const, type);
}

void Lowering::FunctionLowering::AfterNever()
{
	m_builder.Unreachable();
	PositionAt(NewBlock());
}

Lowering::Emitted Lowering::FunctionLowering::LowerTopLevelStatement(const MarmotStatement& statement)
{
	if (statement.IsStatement<MarmotStatement::ForeignDefinition>())
	{
		const MarmotStatement::ForeignDefinition& foreign = statement.GetStatement<MarmotStatement::ForeignDefinition>();
		if (foreign.DeclaredPrimitive().HasValue())
		{
			return {};
		}
		const std::string value = foreign.m_library.HasValue() ? foreign.m_library.Value() + NATIVE_SYMBOL_SEPARATOR + foreign.m_foreign_name : foreign.m_foreign_name;
		m_builder.AtLine(foreign.m_function_name.m_line);
		m_builder.Emit(MarmotIROp::GlobalDefine, UnitType(), { m_builder.ConstText(value) }, m_owner.m_top_level_names.at(foreign.m_function_name.m_lexeme).m_slot);
		return {};
	}

	if (statement.IsStatement<MarmotStatement::FunctionDefinition>())
	{
		const MarmotStatement::FunctionDefinition& definition = statement.GetStatement<MarmotStatement::FunctionDefinition>();
		if (!definition.m_generic_params.empty())
		{
			return {};
		}
		return LowerTopLevelFunction(definition.m_name.m_lexeme, definition.m_params, *definition.m_body, definition.m_name.m_line);
	}

	if (statement.IsStatement<MarmotStatement::Instance>())
	{
		const MarmotStatement::Instance& instance = statement.GetStatement<MarmotStatement::Instance>();
		for (const MarmotStatement::Instance::Method& instance_method : instance.m_methods)
		{
			const MarmotStatement::FunctionDefinition& method = instance_method.m_definition;
			if (m_owner.m_generics.KeyIn(m_owner.m_module_name, method.m_name.m_lexeme).HasValue())
			{
				continue;
			}
			const Emitted lowered = LowerTopLevelFunction(method.m_name.m_lexeme, method.m_params, *method.m_body, method.m_name.m_line);
			if (!lowered.HasValue())
			{
				return lowered;
			}
		}
		return {};
	}

	if (statement.IsStatement<MarmotStatement::Class>())
	{
		return {};
	}

	if (statement.IsStatement<MarmotStatement::TupleDefinition>())
	{
		return LowerTupleDefinition(statement.GetStatement<MarmotStatement::TupleDefinition>());
	}

	if (!statement.IsStatement<MarmotStatement::VariableDefinition>())
	{
		return LowerStatement(statement);
	}

	const MarmotStatement::VariableDefinition& definition = statement.GetStatement<MarmotStatement::VariableDefinition>();
	if (definition.m_name.IsDiscard())
	{
		return LowerLocalDefinition(definition);
	}
	if (definition.m_is_elided)
	{
		return {};
	}

	const std::unordered_map<std::string, TopLevelName>::const_iterator name = m_owner.m_top_level_names.find(definition.m_name.m_lexeme);
	if (name == m_owner.m_top_level_names.cend())
	{
		return {};
	}

	const int line = definition.m_name.m_line;
	if (name->second.m_function.HasValue())
	{
		const MarmotExpression::Function& function = definition.m_value->GetExpression<MarmotExpression::Function>();
		return LowerTopLevelFunction(definition.m_name.m_lexeme, function.m_params, *function.m_body, line);
	}

	const MarmotIRGlobalSlot slot = name->second.m_slot;
	return Lower(*definition.m_value)
		.Transform([this, slot, line](MarmotIRValueId value)
		{
			m_builder.AtLine(line).Emit(MarmotIROp::GlobalDefine, UnitType(), { value }, slot);
		});
}

Lowering::Emitted Lowering::FunctionLowering::LowerTopLevelFunction(const std::string& name, const FunctionParameters& params, const MarmotExpression& body, int line)
{
	const TopLevelName& top_level_name = m_owner.m_top_level_names.at(name);
	const TypeRef closure_type = m_owner.m_module.m_globals[top_level_name.m_slot.m_value].m_type;
	const MarmotIRFunctionId id = top_level_name.m_function.Value();
	FunctionLowering child(m_owner, id, m_owner.m_specializations.front(), this, params.Function());
	return child.LowerBody(closure_type->GetType<MarmotType::FunctionType>().m_param_types, params, body)
		.AndThen([&]() { return MakeClosure(child, closure_type, line, name); })
		.Transform([this, &top_level_name, line](MarmotIRValueId closure)
		{
			m_builder.AtLine(line).Emit(MarmotIROp::GlobalDefine, UnitType(), { closure }, top_level_name.m_slot);
		});
}

Lowering::Emitted Lowering::FunctionLowering::LowerStatement(const MarmotStatement& statement)
{
	struct StatementLowering
	{
		FunctionLowering& m_self;

		Emitted operator()(const MarmotStatement::ExpressionStatement& expression_statement) const
		{
			return m_self.Lower(*expression_statement.m_expr).Transform([](MarmotIRValueId) {});
		}

		Emitted operator()(const MarmotStatement::VariableDefinition& definition) const
		{
			return m_self.LowerLocalDefinition(definition);
		}

		Emitted operator()(const MarmotStatement::TupleDefinition& definition) const
		{
			return m_self.LowerTupleDefinition(definition);
		}

		Emitted operator()(const MarmotStatement::FunctionDefinition& definition) const
		{
			return MarmotCompiler::Failure(m_self.m_owner.Unsupported("a local function definition", definition.m_name));
		}

		Emitted operator()(const MarmotStatement::ForeignDefinition& foreign) const
		{
			if (foreign.DeclaredPrimitive().HasValue())
			{
				return {};
			}
			return m_self.m_owner.ForeignName(foreign)
				.Transform([this, &foreign](const std::string& value)
				{
					m_self.DefineLocal(foreign.m_binding.Value(), m_self.m_builder.AtLine(foreign.m_function_name.m_line).ConstText(value));
				});
		}

		// Only at the top level.
		Emitted operator()(const MarmotStatement::Class&) const
		{
			std::unreachable();
		}

		Emitted operator()(const MarmotStatement::Instance&) const
		{
			std::unreachable();
		}

		Emitted operator()(const MarmotStatement::Struct&) const
		{
			return {};
		}

		Emitted operator()(const MarmotStatement::Union&) const
		{
			return {};
		}

		Emitted operator()(const MarmotStatement::TypeAlias&) const
		{
			return {};
		}
	};

	return std::visit(StatementLowering{ *this }, *statement);
}

Lowering::Emitted Lowering::FunctionLowering::LowerLocalDefinition(const MarmotStatement::VariableDefinition& definition)
{
	if (definition.m_name.IsDiscard())
	{
		return Lower(*definition.m_value).Transform([](MarmotIRValueId) {});
	}
	if (definition.m_is_elided)
	{
		return {};
	}

	const BindingId& binding = definition.m_binding.Value();
	if (definition.m_value->IsExpression<MarmotExpression::Function>() && !definition.m_value->GetExpression<MarmotExpression::Function>().m_generic_params.empty())
	{
		m_local_generics.emplace(binding, &definition.m_value->GetExpression<MarmotExpression::Function>());
		return {};
	}
	return Lower(*definition.m_value)
		.Transform([this, binding](MarmotIRValueId value)
		{
			DefineLocal(binding, value);
		});
}

// Each name takes its element of the tuple, as a local or, at the top level,
// as a global.
Lowering::Emitted Lowering::FunctionLowering::LowerTupleDefinition(const MarmotStatement::TupleDefinition& definition)
{
	const int line = definition.m_names.front().m_line;
	return Lower(*definition.m_value)
		.Transform
		(
			[this, &definition, line](MarmotIRValueId tuple)
			{
				const bool is_dead = IsDead();
				m_builder.AtLine(line);
				for (size_t index = 0u; index < definition.m_names.size(); index += 1u)
				{
					if (definition.m_names[index].IsDiscard())
					{
						continue;
					}
					const MarmotIRValueId element = is_dead
						? Placeholder(UnitType())
						: m_builder.Emit(MarmotIROp::TupleGet, GenericTypes::RepresentationOf(m_function.TypeOf(tuple))->GetType<MarmotType::TupleType>().m_element_types[index], { tuple }, MarmotIRIndex{ static_cast<uint32_t>(index) }, definition.m_names[index].m_lexeme);
					const MarmotCompiler::Optional<BindingId>& binding = definition.m_bindings[index];
					if (binding.HasValue())
					{
						DefineLocal(binding.Value(), element);
					}
					else if (!is_dead)
					{
						m_builder.Emit(MarmotIROp::GlobalDefine, UnitType(), { element }, m_owner.m_top_level_names.at(definition.m_names[index].m_lexeme).m_slot);
					}
				}
			}
		);
}

// A closure made before this local, which captures it, gets it now.
void Lowering::FunctionLowering::DefineLocal(const BindingId& binding, MarmotIRValueId value)
{
	m_locals.insert_or_assign(binding, value);

	std::erase_if
	(
		m_pending_captures,
		[&](const PendingCapture& pending)
		{
			if (pending.m_binding != binding)
			{
				return false;
			}
			m_builder.Emit(MarmotIROp::BindCaptures, UnitType(), { pending.m_closure, value }, MarmotIRIndex{ pending.m_capture });
			return true;
		}
	);
}

Lowering::FunctionLowering::LocalKey Lowering::FunctionLowering::KeyForBinding(const BindingId& binding) const
{
	const FunctionLowering* owner = this;
	while (owner->m_identity != binding->m_function)
	{
		owner = owner->m_parent;
	}
	return LocalKey{ owner, binding };
}

// A capture is read once, at the top of the function, so its value dominates
// every use.
MarmotIRValueId Lowering::FunctionLowering::CaptureFor(const LocalKey& key, const TypeRef& type)
{
	const std::vector<LocalKey>::const_iterator found = std::ranges::find(m_captures, key);
	if (found != m_captures.cend())
	{
		return m_capture_values[static_cast<size_t>(std::distance(m_captures.cbegin(), found))];
	}

	const uint32_t index = static_cast<uint32_t>(m_captures.size());
	m_function.m_values.emplace_back(type, std::string());
	const MarmotIRValueId value{ static_cast<uint32_t>(m_function.m_values.size() - 1u) };
	std::vector<MarmotIRInstruction>& entry = m_function.Block(MarmotIRFunction::s_entry_block).m_instructions;
	entry.emplace(entry.begin() + index, MarmotIRInstruction(MarmotIROp::GetCapture, value, type, {}, MarmotIRIndex{ index }, {}, 0));

	m_function.m_capture_types.push_back(type);
	m_captures.push_back(key);
	m_capture_values.push_back(value);
	return value;
}

Lowering::Emitted Lowering::FunctionLowering::LowerBody(const std::vector<TypeRef>& param_types, const FunctionParameters& params, const MarmotExpression& body)
{
	for (size_t index = 0u; index < params.Size(); index += 1u)
	{
		const MarmotIRValueId parameter = m_builder.AddParameter(MarmotIRFunction::s_entry_block, param_types[index], params.DisplayName(index));
		if (const BindingId& binding = params.BindingAt(index))
		{
			m_locals.emplace(binding, parameter);
		}
	}
	return LowerReturn(body);
}

// Each capture is a value of the enclosing function: its local, or one it
// captures in turn. A local not defined yet, which a local function that names
// itself or a later one captures, goes last, and is bound once it is defined.
Lowering::Lowered Lowering::FunctionLowering::MakeClosure(FunctionLowering& child, const TypeRef& closure_type, int line, std::string name)
{
	std::vector<size_t> order;
	std::vector<size_t> late;
	std::vector<MarmotIRValueId> operands;
	for (size_t index = 0u; index < child.m_captures.size(); index += 1u)
	{
		const LocalKey& key = child.m_captures[index];
		if (key.m_owner != this)
		{
			operands.push_back(CaptureFor(key, child.m_function.m_capture_types[index]));
			order.push_back(index);
			continue;
		}

		const std::unordered_map<BindingId, MarmotIRValueId>::const_iterator local = m_locals.find(key.m_binding);
		if (local == m_locals.cend())
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
	for (MarmotIRInstruction& instruction : child.m_function.Block(MarmotIRFunction::s_entry_block).m_instructions)
	{
		if (instruction.m_op == MarmotIROp::GetCapture)
		{
			MarmotIRIndex& capture = std::get<MarmotIRIndex>(instruction.m_immediate);
			capture.m_value = position[capture.m_value];
		}
	}

	const MarmotIRValueId closure = m_builder.AtLine(line).Emit(MarmotIROp::MakeClosure, closure_type, std::move(operands), child.m_id, std::move(name));
	for (const size_t index : late)
	{
		m_pending_captures.push_back(PendingCapture{ closure, position[index], child.m_captures[index].m_binding });
	}
	return closure;
}

Lowering::Lowered Lowering::FunctionLowering::LowerFunction(const MarmotExpression::Function& function)
{
	const int line = function.m_function_keyword.m_line;
	if (!function.m_generic_params.empty())
	{
		return LowerGenericLambda(function, Concrete(function.m_type_data), std::format("Anonymous Function at line: {}", line), line);
	}
	const TypeRef closure_type = ClosureType(Concrete(function.m_type_data));
	const MarmotType::FunctionType& signature = closure_type->GetType<MarmotType::FunctionType>();
	const MarmotIRFunctionId id = m_owner.NewFunction(std::format("Anonymous Function at line: {}", line), signature.m_return_type, m_specialization);
	FunctionLowering child(m_owner, id, m_specialization, this, function.m_params.Function());
	return child.LowerBody(signature.m_param_types, function.m_params, *function.m_body)
		.AndThen([&]() { return MakeClosure(child, closure_type, line); });
}

// A generic lambda's closure for the types `type` gives its parameters, with
// the enclosing function's types and the methods its constraints resolve to.
Lowering::Lowered Lowering::FunctionLowering::LowerGenericLambda(const MarmotExpression::Function& function, const TypeRef& type, std::string name, int line, MarmotCompiler::Optional<ActiveGenericLambda> generic_lambda)
{
	const MarmotType::FunctionType& instance = type->GetType<MarmotType::FunctionType>();
	GenericTypes::TypeEnvironment types = m_specialization.m_types;
	GenericTypes::Deduce(function.m_return_type, instance.m_return_type, types);
	for (size_t index = 0u; index < function.m_param_types.size(); index += 1u)
	{
		GenericTypes::Deduce(function.m_param_types[index], instance.m_param_types[index], types);
	}
	MethodResolutionMap methods = m_specialization.m_methods;
	for (auto& [method, candidates] : m_owner.m_resolver.ResolveConstraints(function.m_constraints, types))
	{
		std::vector<ResolvedMethodCandidate>& existing = methods[method];
		existing.insert(existing.end(), std::make_move_iterator(candidates.begin()), std::make_move_iterator(candidates.end()));
	}
	const Specialization& specialization = m_owner.m_specializations.emplace_back(std::move(types), std::move(methods), m_specialization.m_source_module);

	const TypeRef closure_type = ClosureType(type);
	const MarmotType::FunctionType& signature = closure_type->GetType<MarmotType::FunctionType>();
	const MarmotIRFunctionId id = m_owner.NewFunction(std::move(name), signature.m_return_type, specialization);
	FunctionLowering child(m_owner, id, specialization, this, function.m_params.Function(), std::move(generic_lambda));
	return child.LowerBody(signature.m_param_types, function.m_params, *function.m_body)
		.AndThen([&]() { return MakeClosure(child, closure_type, line); });
}

Lowering::Lowered Lowering::FunctionLowering::LowerLocalGeneric(const MarmotExpression::Function& function, const TypeRef& type, const Token& name)
{
	const std::string signature = type->ToString();
	for (const FunctionLowering* owner = this; owner != nullptr; owner = owner->m_parent)
	{
		if (owner->m_generic_lambda.HasValue())
		{
			const ActiveGenericLambda& active = owner->m_generic_lambda.Value();
			if (active.m_function == &function && active.m_signature == signature)
			{
				return CaptureFor(active.m_self, ClosureType(type));
			}
		}
	}

	const LocalKey self{ this, std::make_shared<BindingIdentity>(m_identity, -1) };
	return LowerGenericLambda(function, type, name.m_lexeme, name.m_line, ActiveGenericLambda(&function, signature, self))
		.Transform
		(
			[this, &self](MarmotIRValueId value)
			{
				DefineLocal(self.m_binding, value);
				return value;
			}
		);
}

// One function per generic and set of argument types. Its types come from the
// arguments', and from the call's where only the result names a type parameter.
MarmotCompiler::Expected<MarmotIRFunctionId, CompilerError> Lowering::FunctionLowering::Specialize(const std::string& key, const std::vector<TypeRef>& argument_types, const TypeRef& call_type)
{
	const GenericFunctionInfo& info = m_owner.m_generics.At(key);
	GenericTypes::TypeEnvironment types;
	GenericTypes::Deduce(info.m_generic_return_type, call_type, types);
	for (size_t index = 0u; index < info.m_param_types.size() && index < argument_types.size(); index += 1u)
	{
		GenericTypes::Deduce(info.m_param_types[index], argument_types[index], types);
	}
	// `where Iterable::Item<S> ~ A` is what says A, once S is known.
	for (const MarmotType::ClassConstraint& constraint : info.m_constraints)
	{
		if (!constraint.IsEquality())
		{
			continue;
		}
		const TypeRef projected = m_owner.Substitute(constraint.m_equality_lhs, types);
		if (GenericTypes::IsConcrete(projected))
		{
			GenericTypes::Deduce(constraint.m_equality_rhs, projected, types);
		}
	}

	const std::string signature = std::format("{}<{}>", key, argument_types
		| std::views::transform([](const TypeRef& type) { return type->ToString(); })
		| std::views::join_with(',')
		| std::ranges::to<std::string>());
	const TypeRef declared_return = m_owner.Substitute(info.m_generic_return_type, types);
	const TypeRef return_type = GenericTypes::IsConcrete(declared_return) ? declared_return : call_type;
	const std::string cache_key = std::format("{}->{}", signature, return_type->ToString());
	const std::unordered_map<std::string, MarmotIRFunctionId>::const_iterator existing = m_owner.m_specialized_functions.find(cache_key);
	if (existing != m_owner.m_specialized_functions.cend())
	{
		return existing->second;
	}
	const std::unordered_map<std::string, MarmotIRFunctionId>::const_iterator by_arguments = m_owner.m_specializations_by_arguments.find(signature);
	if (!GenericTypes::IsConcrete(return_type) && by_arguments != m_owner.m_specializations_by_arguments.cend())
	{
		return by_arguments->second;
	}

	const bool is_foreign = !info.m_defining_module.empty() && info.m_defining_module != m_owner.m_module_name;
	const MarmotCompiler::Optional<std::string> source_module = is_foreign ? MarmotCompiler::Optional<std::string>(info.m_defining_module) : MarmotCompiler::None;
	MethodResolutionMap methods = m_owner.m_resolver.ResolveConstraints(info.m_constraints, types);
	const Specialization& specialization = m_owner.m_specializations.emplace_back(std::move(types), std::move(methods), source_module);

	const MarmotIRFunctionId id = m_owner.NewFunction(signature, return_type, specialization);
	m_owner.m_specialized_functions.emplace(cache_key, id);
	m_owner.m_specializations_by_arguments.emplace(signature, id);
	FunctionLowering child(m_owner, id, specialization, nullptr, info.m_params.Function());
	const std::shared_ptr<const MarmotExpression> body = info.m_body;
	const FunctionParameters params = info.m_params;
	return child.LowerBody(argument_types, params, *body)
		.Transform([id]() { return id; });
}

bool Lowering::FunctionLowering::IsForeignFunction(const TypeRef& type) const
{
	return type->IsType<MarmotType::FunctionType>() && type->GetType<MarmotType::FunctionType>().m_is_foreign;
}

// A foreign declaration's global holds its name: this module's, one another
// module declared, or, inside a specialization, the defining module's.
bool Lowering::FunctionLowering::IsForeignDeclaration(const std::string& name)
{
	const std::unordered_map<std::string, TopLevelName>::const_iterator own = m_owner.m_top_level_names.find(name);
	const bool is_own = !m_specialization.m_source_module.HasValue() && own != m_owner.m_top_level_names.cend();
	return !is_own || m_owner.m_module.m_globals[own->second.m_slot.m_value].m_type->IsType<MarmotType::TextType>();
}

// A foreign function used as a value is a function that calls it.
Lowering::Lowered Lowering::FunctionLowering::LowerForeignValue(const MarmotExpression::NameAccess& declaration, const TypeRef& type)
{
	const Token& name = declaration.m_name;
	const TypeRef closure_type = ClosureType(type);
	const MarmotType::FunctionType& signature = closure_type->GetType<MarmotType::FunctionType>();
	const MarmotIRFunctionId id = m_owner.NewFunction(std::format("foreign {}", name.m_lexeme), signature.m_return_type, m_specialization);
	FunctionLowering child(m_owner, id, m_specialization, this, nullptr);
	std::vector<MarmotIRValueId> arguments;
	for (const TypeRef& parameter_type : signature.m_param_types)
	{
		arguments.push_back(child.m_builder.AddParameter(MarmotIRFunction::s_entry_block, parameter_type));
	}

	child.m_builder.AtLine(name.m_line);
	MarmotIRImmediate target;
	if (declaration.m_intrinsic.HasValue())
	{
		target = MarmotIRForeign{ std::string(MarmotIntrinsics::At(declaration.m_intrinsic.Value()).m_name) };
	}
	else
	{
		const MarmotCompiler::Expected<MarmotIRGlobalSlot, CompilerError> slot = child.GlobalFor(name, TextType());
		if (!slot.HasValue())
		{
			return MarmotCompiler::Failure(slot.Error());
		}
		arguments.insert(arguments.begin(), child.m_builder.Emit(MarmotIROp::GlobalGet, TextType(), {}, slot.Value()));
	}
	const MarmotIRValueId result = child.m_builder.Emit(MarmotIROp::CallForeign, signature.m_return_type, std::move(arguments), target);
	if (IsNever(signature.m_return_type))
	{
		child.m_builder.Unreachable();
	}
	else
	{
		child.m_builder.Return(result);
	}
	return MakeClosure(child, closure_type, name.m_line);
}

Lowering::Lowered Lowering::FunctionLowering::LowerName(const MarmotExpression::NameAccess& name)
{
	const Token& token = name.m_name;
	if (name.m_intrinsic.HasValue())
	{
		return LowerForeignValue(name, Concrete(name.m_type_data));
	}

	const bool is_local = std::holds_alternative<MarmotExpression::NameContext::Local>(name.m_name_ctx);
	if (is_local || std::holds_alternative<MarmotExpression::NameContext::Cell>(name.m_name_ctx))
	{
		const BindingId& binding = is_local
			? std::get<MarmotExpression::NameContext::Local>(name.m_name_ctx).m_binding
			: std::get<MarmotExpression::NameContext::Cell>(name.m_name_ctx).m_binding;
		const LocalKey key = KeyForBinding(binding);
		const std::unordered_map<BindingId, const MarmotExpression::Function*>::const_iterator generic = key.m_owner->m_local_generics.find(key.m_binding);
		if (generic != key.m_owner->m_local_generics.cend())
		{
			return LowerLocalGeneric(*generic->second, Concrete(name.m_type_data), token);
		}
		return is_local ? m_locals.at(key.m_binding) : CaptureFor(key, Concrete(name.m_type_data));
	}

	const std::string& lexeme = token.m_lexeme;
	const Specialization& specialization = m_specialization;
	if (m_owner.m_resolver.IsClassMethod(lexeme))
	{
		const TypeRef type = Concrete(name.m_type_data);
		const MarmotType::FunctionType& signature = type->GetType<MarmotType::FunctionType>();
		const MethodResolution<MarmotCompiler::Optional<ResolvedInstanceName>> resolved = m_owner.m_resolver.ResolveConcreteCall(lexeme, signature.m_param_types, signature.m_return_type);
		if (!resolved.HasValue())
		{
			return MarmotCompiler::Failure(m_owner.ResolutionError(resolved.Error(), token));
		}
		return LoadResolved(resolved.Value().Value(), type, token);
	}
	// A generic function used as a value is its specialization for the types
	// the value has.
	const MarmotCompiler::Optional<std::string> generic_key = m_owner.m_top_level_names.contains(lexeme) ? MarmotCompiler::None : m_owner.m_generics.FindKey(lexeme, specialization.m_source_module);
	if (generic_key.HasValue())
	{
		const TypeRef type = Concrete(name.m_type_data);
		const MarmotType::FunctionType& instance = type->GetType<MarmotType::FunctionType>();
		return Specialize(generic_key.Value(), instance.m_param_types, instance.m_return_type)
			.Transform([this, &type, &token](MarmotIRFunctionId function)
			{
				return m_builder.AtLine(token.m_line).Emit(MarmotIROp::MakeClosure, ClosureType(type), {}, function, token.m_lexeme);
			});
	}
	if (IsForeignFunction(name.m_type_data))
	{
		return LowerForeignValue(name, Concrete(name.m_type_data));
	}

	return GlobalFor(token, Concrete(name.m_type_data))
		.Transform
		(
			[this, &token](MarmotIRGlobalSlot slot)
			{
				const MarmotIRGlobal& global = m_owner.m_module.m_globals[slot.m_value];
				return m_builder.AtLine(token.m_line).Emit(MarmotIROp::GlobalGet, global.m_type, {}, slot, global.m_name);
			}
		);
}

// A bare name inside a specialization of another module's generic is one of
// that module's, whatever this module defines.
MarmotCompiler::Expected<MarmotIRGlobalSlot, CompilerError> Lowering::FunctionLowering::GlobalFor(const Token& name, const TypeRef& type)
{
	const std::string& lexeme = name.m_lexeme;
	const TypeRef import_type = type->IsType<MarmotType::FunctionType>() ? ClosureType(type) : type;
	const size_t separator = lexeme.find(NameSeparator);
	const MarmotCompiler::Optional<std::string>& source_module = m_specialization.m_source_module;
	if (source_module.HasValue() && separator == std::string::npos)
	{
		return ImportGlobal(source_module.Value(), lexeme, import_type, name);
	}

	const std::unordered_map<std::string, TopLevelName>::const_iterator own = m_owner.m_top_level_names.find(lexeme);
	if (own != m_owner.m_top_level_names.cend())
	{
		return own->second.m_slot;
	}
	if (separator == std::string::npos)
	{
		return MarmotCompiler::Failure(m_owner.Unsupported(std::format("'{}'", lexeme), name));
	}
	return ImportGlobal(lexeme.substr(0u, separator), lexeme.substr(separator + NameSeparator.length()), import_type, name);
}

// A selected instance's declaring module determines its global, independently
// of the source module of a generic currently being specialized.
MarmotCompiler::Expected<MarmotIRGlobalSlot, CompilerError> Lowering::FunctionLowering::GlobalForResolved(const ResolvedInstanceName& name, const TypeRef& type, const Token& at)
{
	if (name.m_declaring_module != m_owner.m_module_name)
	{
		if (m_owner.m_generics.KeyIn(name.m_declaring_module, name.m_symbol).HasValue())
		{
			return MarmotCompiler::Failure(m_owner.Error(CompilerErrorCode::LoweringUnresolvedMethodResolution, std::format("Constrained instance method '{}' from module '{}' is monomorphised at each call site, so it has no address to take. Call it directly instead of using it through an operator or as a value.", name.m_symbol, name.m_declaring_module), at));
		}
		return ImportGlobal(name.m_declaring_module, name.m_symbol, type, at);
	}

	const std::unordered_map<std::string, TopLevelName>::const_iterator own = m_owner.m_top_level_names.find(name.m_symbol);
	if (own == m_owner.m_top_level_names.cend())
	{
		return MarmotCompiler::Failure(m_owner.Error(CompilerErrorCode::None, std::format("Resolved symbol '{}' not found in globals.", name.m_symbol), at));
	}
	return own->second.m_slot;
}

MarmotIRGlobalSlot Lowering::FunctionLowering::ImportGlobal(const std::string& module, const std::string& symbol, const TypeRef& type, const Token& at)
{
	const std::string key = module + std::string(NameSeparator) + symbol;
	const std::unordered_map<std::string, MarmotIRGlobalSlot>::const_iterator imported = m_owner.m_import_slots.find(key);
	if (imported != m_owner.m_import_slots.cend())
	{
		return imported->second;
	}

	const MarmotIRGlobalSlot slot = m_owner.m_module.ReserveImport(module, symbol, type);
	m_owner.m_import_slots.emplace(key, slot);
	m_owner.m_import_tokens.emplace_back(slot, at);
	return slot;
}

Lowering::Lowered Lowering::FunctionLowering::LoadResolved(const ResolvedInstanceName& resolved_name, const TypeRef& type, const Token& at)
{
	if (const MarmotCompiler::Optional<std::string> key = m_owner.m_generics.KeyIn(resolved_name.m_declaring_module, resolved_name.m_symbol))
	{
		const MarmotType::FunctionType& signature = type->GetType<MarmotType::FunctionType>();
		return Specialize(key.Value(), signature.m_param_types, signature.m_return_type).Transform
		(
			[&](MarmotIRFunctionId function)
			{
				return m_builder.AtLine(at.m_line).Emit(MarmotIROp::MakeClosure, ClosureType(type), {}, function, at.m_lexeme);
			}
		);
	}
	return GlobalForResolved(resolved_name, type, at)
		.Transform
		(
			[this, &at](MarmotIRGlobalSlot slot)
			{
				const MarmotIRGlobal& global = m_owner.m_module.m_globals[slot.m_value];
				return m_builder.AtLine(at.m_line).Emit(MarmotIROp::GlobalGet, global.m_type, {}, slot, global.m_name);
			}
		);
}

// A constrained instance's method is a generic, specialized for these
// arguments; this module's own is called directly; another module's through
// its global.
Lowering::Lowered Lowering::FunctionLowering::CallResolved(const ResolvedInstanceName& name, std::vector<MarmotIRValueId> arguments, const std::vector<TypeRef>& argument_types, const TypeRef& result_type, const Token& at)
{
	const MarmotCompiler::Optional<std::string> generic_key = m_owner.m_generics.KeyIn(name.m_declaring_module, name.m_symbol);
	if (generic_key.HasValue())
	{
		return Specialize(generic_key.Value(), argument_types, result_type)
			.Transform([&](MarmotIRFunctionId function)
			{
				return m_builder.AtLine(at.m_line).Emit(MarmotIROp::Call, m_owner.m_functions[function.m_index].m_return_type, std::move(arguments), function);
			});
	}

	if (name.m_declaring_module == m_owner.m_module_name)
	{
		const std::unordered_map<std::string, TopLevelName>::const_iterator own = m_owner.m_top_level_names.find(name.m_symbol);
		if (own != m_owner.m_top_level_names.cend() && own->second.m_function.HasValue())
		{
			const MarmotIRFunctionId function = own->second.m_function.Value();
			return m_builder.AtLine(at.m_line).Emit(MarmotIROp::Call, m_owner.m_functions[function.m_index].m_return_type, std::move(arguments), function);
		}
	}

	return GlobalForResolved(name, MarmotType::MakeFunctionType(argument_types, TypeRef(result_type)), at)
		.Transform([&](MarmotIRGlobalSlot slot)
		{
			return m_builder.AtLine(at.m_line).Emit(MarmotIROp::CallGlobal, result_type, std::move(arguments), slot);
		});
}

// A primitive is its instruction, whichever module declared it.
Lowering::Lowered Lowering::FunctionLowering::LowerPrimitive(const MarmotExpression::Call& call)
{
	const MarmotPrimitive primitive = call.m_primitive.Value();
	if (primitive == MarmotPrimitive::Spawn)
	{
		return LowerSpawn(call);
	}

	if (const MarmotOperator* operation = PrimitiveOperator(primitive))
	{
		return LowerArguments(call.m_arguments).AndThen
		(
			[&](std::vector<MarmotIRValueId> arguments) -> Lowered
			{
				if (IsDead())
				{
					return Placeholder(Concrete(call.m_type_data));
				}
				const Token op(operation->m_token, call.m_paren);
				return operation->m_unary ? LowerScalarUnary(op, arguments[0u]) : LowerScalarBinary(op, arguments[0u], arguments[1u]);
			}
		);
	}
	std::vector<const MarmotExpression*> operands = call.m_arguments
		| std::views::transform([](const std::unique_ptr<MarmotExpression>& argument) -> const MarmotExpression* { return argument.get(); })
		| std::ranges::to<std::vector>();
	return LowerOperation(InstructionOf(primitive), Concrete(call.m_type_data), std::move(operands), {}, call.m_paren.m_line);
}

Lowering::Lowered Lowering::FunctionLowering::LowerCall(const MarmotExpression::Call& call)
{
	if (call.m_primitive.HasValue())
	{
		return LowerPrimitive(call);
	}

	const TypeRef call_type = Concrete(call.m_type_data);
	const int line = call.m_paren.m_line;
	return LowerArguments(call.m_arguments)
		.AndThen
		(
			[&](std::vector<MarmotIRValueId> arguments) -> Lowered
			{
				if (IsDead())
				{
					return Placeholder(call_type);
				}
				return ResolveCallee(call, arguments, call_type)
					.Transform([&](Callee callee)
					{
						return EmitCall(callee, std::move(arguments), call_type, line);
					});
			}
		);
}

// The language promises that a call in tail position runs without a new
// frame, whatever it calls. A foreign call has no frame of its own to reuse,
// and a call whose result is not the function's is not one it can return.
Lowering::Emitted Lowering::FunctionLowering::LowerTailCall(const MarmotExpression::Call& call)
{
	if (call.m_is_foreign || call.m_primitive.HasValue())
	{
		return LowerCall(call)
			.Transform([this](MarmotIRValueId value)
			{
				m_builder.Return(value);
			});
	}

	const TypeRef call_type = Concrete(call.m_type_data);
	const int line = call.m_paren.m_line;
	return LowerArguments(call.m_arguments)
		.AndThen
		(
			[&](std::vector<MarmotIRValueId> arguments) -> Emitted
			{
				if (IsDead())
				{
					m_builder.Return(Placeholder(call_type));
					return {};
				}
				return ResolveCallee(call, arguments, call_type)
					.Transform
					(
						[&](Callee callee)
						{
							const TypeRef result_type = ResultTypeOf(callee, call_type);
							if (!IsNever(result_type) && !MarmotIRSameType(result_type, m_function.m_return_type))
							{
								m_builder.Return(EmitCall(callee, std::move(arguments), call_type, line));
								return;
							}
							if (callee.m_value.HasValue())
							{
								arguments.insert(arguments.begin(), callee.m_value.Value());
							}
							m_builder.AtLine(line).TailCall(callee.m_target, std::move(arguments));
						}
					);
			}
		);
}

// A call's value has the type its callee returns, which the type checker does
// not always record inside a generic's body.
Lowering::TypeRef Lowering::FunctionLowering::ResultTypeOf(const Callee& callee, const TypeRef& call_type)
{
	const auto returned_by = [&call_type](const TypeRef& function_type)
	{
		return function_type->IsType<MarmotType::FunctionType>() ? function_type->GetType<MarmotType::FunctionType>().m_return_type : call_type;
	};
	if (std::holds_alternative<MarmotIRFunctionId>(callee.m_target))
	{
		return m_owner.m_functions[std::get<MarmotIRFunctionId>(callee.m_target).m_index].m_return_type;
	}
	if (std::holds_alternative<MarmotIRGlobalSlot>(callee.m_target))
	{
		return returned_by(m_owner.m_module.m_globals[std::get<MarmotIRGlobalSlot>(callee.m_target).m_value].m_type);
	}
	if (callee.m_value.HasValue())
	{
		return returned_by(m_function.TypeOf(callee.m_value.Value()));
	}
	return call_type;
}

MarmotIRValueId Lowering::FunctionLowering::EmitCall(const Callee& callee, std::vector<MarmotIRValueId> arguments, const TypeRef& call_type, int line)
{
	const TypeRef result_type = ResultTypeOf(callee, call_type);
	const bool is_foreign = std::holds_alternative<MarmotIRForeign>(callee.m_target) || (callee.m_value.HasValue() && !m_function.TypeOf(callee.m_value.Value())->IsType<MarmotType::FunctionType>());
	if (callee.m_value.HasValue())
	{
		arguments.insert(arguments.begin(), callee.m_value.Value());
	}
	const MarmotIROp op = std::holds_alternative<MarmotIRFunctionId>(callee.m_target) ? MarmotIROp::Call
		: std::holds_alternative<MarmotIRGlobalSlot>(callee.m_target) ? MarmotIROp::CallGlobal
		: is_foreign ? MarmotIROp::CallForeign
		: MarmotIROp::CallValue;
	const MarmotIRValueId result = m_builder.AtLine(line).Emit(op, result_type, std::move(arguments), callee.m_target);
	if (IsNever(result_type))
	{
		AfterNever();
	}
	return result;
}

// What a call runs: an intrinsic by ID, a native function by the name its
// global holds, the specialization of a generic, the instance
// method a class method resolves to, this module's function, a global, or the
// closure its callee evaluates to, after the arguments.
MarmotCompiler::Expected<Lowering::FunctionLowering::Callee, CompilerError> Lowering::FunctionLowering::ResolveCallee(const MarmotExpression::Call& call, const std::vector<MarmotIRValueId>& arguments, const TypeRef& call_type)
{
	const MarmotExpression::NameAccess* name = GlobalName(*call.m_callee);
	const Specialization& specialization = m_specialization;
	const Token& paren = call.m_paren;

	if (call.m_is_foreign)
	{
		const MarmotExpression::NameAccess& foreign = call.m_callee->GetExpression<MarmotExpression::NameAccess>();
		if (foreign.m_intrinsic.HasValue())
		{
			return Callee{ MarmotIRForeign{ std::string(MarmotIntrinsics::At(foreign.m_intrinsic.Value()).m_name) }, MarmotCompiler::None };
		}
		if (name != nullptr && IsForeignDeclaration(name->m_name.m_lexeme))
		{
			return GlobalFor(name->m_name, TextType())
				.Transform
				(
					[this, &paren](MarmotIRGlobalSlot slot)
					{
						const MarmotIRGlobal& global = m_owner.m_module.m_globals[slot.m_value];
						return Callee{ std::monostate{}, m_builder.AtLine(paren.m_line).Emit(MarmotIROp::GlobalGet, global.m_type, {}, slot, global.m_name) };
					}
				);
		}
	}

	if (name == nullptr)
	{
		return Lower(*call.m_callee)
			.Transform([](MarmotIRValueId closure) { return Callee{ std::monostate{}, closure }; });
	}

	const std::string& lexeme = name->m_name.m_lexeme;
	std::vector<TypeRef> argument_types;
	for (size_t index = 0u; index < arguments.size(); index += 1u)
	{
		argument_types.push_back(NominalType(*call.m_arguments[index], arguments[index]));
	}
	MarmotCompiler::Optional<ResolvedInstanceName> resolved;
	if (specialization.m_methods.contains(lexeme))
	{
		const MethodResolution<MarmotCompiler::Optional<ResolvedInstanceName>> method = m_owner.m_resolver.ResolveConstrainedCall(specialization.m_methods, lexeme, argument_types, call_type);
		if (!method.HasValue())
		{
			return MarmotCompiler::Failure(m_owner.ResolutionError(method.Error(), paren));
		}
		resolved = method.Value();
	}
	if (!resolved.HasValue() && lexeme.find(NameSeparator) != std::string::npos)
	{
		const MethodResolution<MarmotCompiler::Optional<ResolvedInstanceName>> method = m_owner.m_resolver.ResolveConcreteCall(lexeme, argument_types, call_type);
		if (!method.HasValue())
		{
			return MarmotCompiler::Failure(m_owner.ResolutionError(method.Error(), paren));
		}
		resolved = method.Value();
	}

	const MarmotCompiler::Optional<std::string> generic_key = resolved.HasValue()
		? m_owner.m_generics.KeyIn(resolved->m_declaring_module, resolved->m_symbol)
		: m_owner.m_generics.FindKey(lexeme, specialization.m_source_module);
	if (generic_key.HasValue())
	{
		return Specialize(generic_key.Value(), argument_types, call_type)
			.Transform([](MarmotIRFunctionId function) { return Callee{ function, MarmotCompiler::None }; });
	}

	if (resolved.HasValue())
	{
		const ResolvedInstanceName& instance = resolved.Value();
		const std::unordered_map<std::string, TopLevelName>::const_iterator own = instance.m_declaring_module != m_owner.m_module_name ? m_owner.m_top_level_names.cend() : m_owner.m_top_level_names.find(instance.m_symbol);
		if (own != m_owner.m_top_level_names.cend() && own->second.m_function.HasValue())
		{
			return Callee{ own->second.m_function.Value(), MarmotCompiler::None };
		}
		return GlobalForResolved(resolved.Value(), MarmotType::MakeFunctionType(argument_types, TypeRef(call_type)), paren)
			.Transform([](MarmotIRGlobalSlot slot) { return Callee{ slot, MarmotCompiler::None }; });
	}

	if (!specialization.m_source_module.HasValue())
	{
		const std::unordered_map<std::string, TopLevelName>::const_iterator own = m_owner.m_top_level_names.find(lexeme);
		if (own != m_owner.m_top_level_names.cend() && own->second.m_function.HasValue())
		{
			return Callee{ own->second.m_function.Value(), MarmotCompiler::None };
		}
	}

	return GlobalFor(name->m_name, Concrete(name->m_type_data))
		.Transform([](MarmotIRGlobalSlot slot) { return Callee{ slot, MarmotCompiler::None }; });
}

Lowering::LoweredValues Lowering::FunctionLowering::LowerArguments(const std::vector<std::unique_ptr<MarmotExpression>>& arguments)
{
	std::vector<MarmotIRValueId> values;
	values.reserve(arguments.size());
	for (const std::unique_ptr<MarmotExpression>& argument : arguments)
	{
		const Lowered value = Lower(*argument);
		if (!value.HasValue())
		{
			return MarmotCompiler::Failure(value.Error());
		}
		values.push_back(value.Value());
	}
	return values;
}

std::vector<Lowering::TypeRef> Lowering::FunctionLowering::TypesOf(const std::vector<MarmotIRValueId>& values)
{
	const MarmotIRFunction& function = m_function;
	return values
		| std::views::transform([&function](MarmotIRValueId value) { return function.TypeOf(value); })
		| std::ranges::to<std::vector>();
}
