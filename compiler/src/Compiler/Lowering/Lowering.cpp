#include "Lowering.h"
#include "Common/Builtins/BuiltinTable.h"
#include "Common/Constant/Constant.h"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <utility>

namespace
{
	using TypeRef = std::shared_ptr<MidoriType>;

	// Called like functions, but the code generator emits each as an opcode.
	constexpr std::array<std::string_view, 6u> s_intrinsic_calls =
	{
		"Cell::New",
		"Cell::Get",
		"Cell::Set",
		"Concurrency::Close",
		"Concurrency::IsDone",
		"Concurrency::Cancel"
	};

	std::optional<MidoriIRScalar> ScalarOf(const TypeRef& type)
	{
		if (type->IsType<MidoriType::IntegerType>())
		{
			return MidoriIRScalar::Int;
		}
		if (type->IsType<MidoriType::FloatType>())
		{
			return MidoriIRScalar::Float;
		}
		if (type->IsType<MidoriType::ByteType>())
		{
			return MidoriIRScalar::Byte;
		}
		if (type->IsType<MidoriType::WordType>())
		{
			return MidoriIRScalar::Word;
		}
		if (type->IsType<MidoriType::BoolType>())
		{
			return MidoriIRScalar::Bool;
		}
		if (type->IsType<MidoriType::TextType>())
		{
			return MidoriIRScalar::Text;
		}
		return std::nullopt;
	}

	// One op per scalar type, in the order Int, Float, Byte, Word, Bool, Text;
	// nothing where the operator does not apply to that type.
	using ScalarOps = std::array<std::optional<MidoriIROp>, 6u>;

	std::optional<MidoriIROp> SelectOp(const ScalarOps& ops, MidoriIRScalar scalar)
	{
		return ops[static_cast<size_t>(scalar)];
	}

	std::optional<ScalarOps> BinaryOps(Token::Name op)
	{
		using enum MidoriIROp;
		switch (op)
		{
		case Token::Name::SINGLE_PLUS:
			return ScalarOps{ AddInt, AddFloat, AddByte, AddWord, std::nullopt, std::nullopt };
		case Token::Name::DOUBLE_PLUS:
			return ScalarOps{ std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, Concat };
		case Token::Name::SINGLE_MINUS:
			return ScalarOps{ SubInt, SubFloat, SubByte, SubWord, std::nullopt, std::nullopt };
		case Token::Name::STAR:
			return ScalarOps{ MulInt, MulFloat, MulByte, MulWord, std::nullopt, std::nullopt };
		case Token::Name::SLASH:
			return ScalarOps{ DivInt, DivFloat, DivByte, DivWord, std::nullopt, std::nullopt };
		case Token::Name::PERCENT:
			return ScalarOps{ ModInt, ModFloat, ModByte, ModWord, std::nullopt, std::nullopt };
		case Token::Name::LEFT_SHIFT:
			return ScalarOps{ ShlInt, std::nullopt, ShlByte, ShlWord, std::nullopt, std::nullopt };
		case Token::Name::RIGHT_SHIFT:
			return ScalarOps{ ShrInt, std::nullopt, ShrByte, ShrWord, std::nullopt, std::nullopt };
		case Token::Name::SINGLE_AMPERSAND:
			return ScalarOps{ BitAndInt, std::nullopt, BitAndByte, BitAndWord, std::nullopt, std::nullopt };
		case Token::Name::SINGLE_BAR:
			return ScalarOps{ BitOrInt, std::nullopt, BitOrByte, BitOrWord, std::nullopt, std::nullopt };
		case Token::Name::CARET:
			return ScalarOps{ BitXorInt, std::nullopt, BitXorByte, BitXorWord, std::nullopt, std::nullopt };
		case Token::Name::LEFT_ANGLE:
			return ScalarOps{ LtInt, LtFloat, LtByte, LtWord, std::nullopt, std::nullopt };
		case Token::Name::LESS_EQUAL:
			return ScalarOps{ LeInt, LeFloat, LeByte, LeWord, std::nullopt, std::nullopt };
		case Token::Name::RIGHT_ANGLE:
			return ScalarOps{ GtInt, GtFloat, GtByte, GtWord, std::nullopt, std::nullopt };
		case Token::Name::GREATER_EQUAL:
			return ScalarOps{ GeInt, GeFloat, GeByte, GeWord, std::nullopt, std::nullopt };
		case Token::Name::DOUBLE_EQUAL:
			return ScalarOps{ EqInt, EqFloat, EqByte, EqWord, EqBool, EqText };
		case Token::Name::BANG_EQUAL:
			return ScalarOps{ NeInt, NeFloat, NeByte, NeWord, NeBool, NeText };
		default:
			return std::nullopt;
		}
	}

	using Conversion = std::pair<std::pair<MidoriIRScalar, MidoriIRScalar>, std::vector<MidoriIROp>>;

	// A conversion between two scalar types, as the ops that make it; empty
	// when there is none.
	std::vector<MidoriIROp> ConversionOps(MidoriIRScalar from, MidoriIRScalar to)
	{
		using enum MidoriIROp;
		using enum MidoriIRScalar;
		const std::pair<MidoriIRScalar, MidoriIRScalar> key{ from, to };
		static const std::array<Conversion, 18u> s_conversions =
		{{
			{ { Int, Float }, { IntToFloat } },
			{ { Text, Float }, { TextToFloat } },
			{ { Byte, Float }, { ByteToFloat } },
			{ { Word, Float }, { WordToFloat } },
			{ { Float, Int }, { FloatToInt } },
			{ { Text, Int }, { TextToInt } },
			{ { Byte, Int }, { ByteToInt } },
			{ { Word, Int }, { WordToInt } },
			{ { Int, Byte }, { IntToByte } },
			{ { Word, Byte }, { WordToByte } },
			{ { Float, Byte }, { FloatToByte } },
			{ { Int, Word }, { IntToWord } },
			{ { Byte, Word }, { ByteToWord } },
			{ { Float, Word }, { FloatToWord } },
			{ { Float, Text }, { FloatToText } },
			{ { Int, Text }, { IntToText } },
			{ { Byte, Text }, { ByteToInt, IntToText } },
			{ { Word, Text }, { WordToText } }
		}};

		const std::array<Conversion, 18u>::const_iterator found = std::ranges::find(s_conversions, key, &Conversion::first);
		return found == s_conversions.cend() ? std::vector<MidoriIROp>{} : found->second;
	}

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

	bool IsGlobal(const MidoriExpression::NameAccess& name)
	{
		return std::holds_alternative<MidoriExpression::NameContext::Global>(name.m_name_ctx);
	}

	const MidoriExpression::NameAccess* GlobalName(const MidoriExpression& expression)
	{
		if (!expression.IsExpression<MidoriExpression::NameAccess>())
		{
			return nullptr;
		}
		const MidoriExpression::NameAccess& name = expression.GetExpression<MidoriExpression::NameAccess>();
		return IsGlobal(name) ? &name : nullptr;
	}
}

LoweredExport::LoweredExport(std::string name, BytecodeModule::SymbolType kind, std::optional<MidoriIRGlobalSlot> slot, Token token)
	: m_name(std::move(name)),
	m_kind(kind),
	m_slot(slot),
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

Lowering::FunctionScope::FunctionScope(MidoriIRFunctionId id, MidoriIRFunction& function)
	: m_id(id),
	m_builder(function)
{
}

Lowering::Lowering(MidoriProgramTree& program, std::string_view file_name, const std::vector<std::string>& source_lines, std::string module_name, const std::unordered_set<std::string>& export_symbols, const LoweringImports& imports)
	: m_program(program),
	m_file_name(file_name),
	m_source_lines(source_lines),
	m_module_name(module_name),
	m_export_symbols(export_symbols),
	m_imports(imports),
	m_module(std::move(module_name))
{
}

MidoriResult::DiagnosticsResult<LoweredModule> Lowering::Lower() &&
{
	const MidoriIRFunctionId top_level = NewFunction(std::string(MAIN_PROCEDURE_PREFIX), UnitType());
	m_module.m_top_level = top_level;
	FunctionScope scope(top_level, m_functions[top_level.m_index]);
	m_scopes.push_back(&scope);

	const Emitted lowered = ReserveTopLevelNames()
		.and_then([this]() -> Emitted
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
		});
	m_scopes.pop_back();

	if (!lowered.has_value())
	{
		return std::unexpected(MidoriResult::CompilerDiagnostics(lowered.error()));
	}

	std::ranges::move(m_functions, std::back_inserter(m_module.m_functions));
	LoweredModule result(std::move(m_module));
	result.m_exports = CollectExports();
	result.m_imports = std::move(m_import_tokens);
	return result;
}

CompilerError Lowering::Unsupported(std::string_view construct, const Token& token) const
{
	return MidoriError::GenerateCodeGeneratorErrorWithContext(CompilerErrorCode::CodeGeneratorUnsupportedLowering, std::format("The MidoriIR backend cannot lower {} yet.", construct), token, m_file_name, m_source_lines);
}

Lowering::FunctionScope& Lowering::Scope()
{
	return *m_scopes.back();
}

MidoriIRBuilder& Lowering::Builder()
{
	return Scope().m_builder;
}

MidoriIRFunctionId Lowering::NewFunction(std::string name, std::shared_ptr<MidoriType> return_type)
{
	m_functions.emplace_back(std::move(name), std::move(return_type));
	return MidoriIRFunctionId{ static_cast<uint32_t>(m_functions.size() - 1u) };
}

// A body may name a top-level definition that comes after it, so every one has
// its global, and every `def f = fn ...` its function, before any is lowered.
Lowering::Emitted Lowering::ReserveTopLevelNames()
{
	for (std::unique_ptr<MidoriStatement>& statement : m_program)
	{
		const Emitted reserved = ReserveTopLevelName(*statement);
		if (!reserved.has_value())
		{
			return reserved;
		}
	}
	return {};
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

			const MidoriExpression::Function& function = definition.m_value->GetExpression<MidoriExpression::Function>();
			if (!function.m_generic_params.empty())
			{
				return std::unexpected(m_self.Unsupported("a generic function", definition.m_name));
			}

			const TypeRef closure_type = ClosureType(definition.m_value->GetType());
			const MidoriIRFunctionId id = m_self.NewFunction(name, closure_type->GetType<MidoriType::FunctionType>().m_return_type);
			m_self.m_top_level_names.emplace(name, TopLevelName{ m_self.m_module.ReserveGlobal(name, closure_type), id });
			return {};
		}

		Emitted operator()(MidoriStatement::TupleDefinition& definition) const
		{
			return std::unexpected(m_self.Unsupported("a tuple definition", definition.m_names.front()));
		}

		Emitted operator()(MidoriStatement::FunctionDefinition& definition) const
		{
			return std::unexpected(m_self.Unsupported("an instance method", definition.m_name));
		}

		Emitted operator()(MidoriStatement::ForeignDefinition& foreign) const
		{
			return m_self.ReserveForeign(foreign);
		}

		Emitted operator()(MidoriStatement::Class& class_statement) const
		{
			return std::unexpected(m_self.Unsupported("a class", class_statement.m_name));
		}

		Emitted operator()(MidoriStatement::Instance& instance) const
		{
			return std::unexpected(m_self.Unsupported("an instance", instance.m_class_name));
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

// A foreign function's global holds the name the VM looks it up by, which is
// how another module calls it.
Lowering::Emitted Lowering::ReserveForeign(const MidoriStatement::ForeignDefinition& foreign)
{
	const Token& name = foreign.m_function_name;
	if (foreign.m_library.has_value())
	{
		return std::unexpected(Unsupported("a foreign function from a library", name));
	}

	const TypeRef& return_type = foreign.m_type->GetType<MidoriType::FunctionType>().m_return_type;
	const bool is_supported_return = return_type->IsType<MidoriType::IntegerType>() || return_type->IsType<MidoriType::FloatType>() || return_type->IsType<MidoriType::BoolType>() || return_type->IsType<MidoriType::UnitType>() || return_type->IsType<MidoriType::TextType>() || return_type->IsType<MidoriType::ArrayType>() || return_type->IsType<MidoriType::ByteType>() || return_type->IsType<MidoriType::WordType>();
	if (!is_supported_return)
	{
		return std::unexpected(MidoriError::GenerateCodeGeneratorErrorWithContext(CompilerErrorCode::CodeGeneratorUnsupportedLowering, "Unsupported return type for foreign function", name, m_file_name, m_source_lines));
	}

	if (!MarmotBuiltins::FindIndex(foreign.m_foreign_name).has_value())
	{
		return std::unexpected(MidoriError::GenerateCodeGeneratorErrorWithContext(CompilerErrorCode::CodeGeneratorUnknownForeignFunction, std::format("Unknown foreign function '{}': it is not a Marmot builtin. Name the library that exports it: foreign \"{}\" ... from \"library\";", foreign.m_foreign_name, foreign.m_foreign_name), name, m_file_name, m_source_lines));
	}

	m_builtin_foreigns.emplace(name.m_lexeme, foreign.m_foreign_name);
	m_top_level_names.emplace(name.m_lexeme, TopLevelName{ m_module.ReserveGlobal(name.m_lexeme, MidoriType::MakeLiteralType<MidoriType::TextType>()), std::nullopt });
	return {};
}

std::vector<LoweredExport> Lowering::CollectExports() const
{
	std::vector<LoweredExport> exports;

	for (const std::unique_ptr<MidoriStatement>& statement : m_program)
	{
		if (statement->IsStatement<MidoriStatement::VariableDefinition>())
		{
			const MidoriStatement::VariableDefinition& definition = statement->GetStatement<MidoriStatement::VariableDefinition>();
			if (m_export_symbols.contains(definition.m_name.m_lexeme))
			{
				exports.emplace_back(definition.m_name.m_lexeme, BytecodeModule::SymbolType::GLOBAL_VARIABLE, m_top_level_names.at(definition.m_name.m_lexeme).m_slot, definition.m_name);
			}
		}
		else if (statement->IsStatement<MidoriStatement::ForeignDefinition>())
		{
			const MidoriStatement::ForeignDefinition& foreign = statement->GetStatement<MidoriStatement::ForeignDefinition>();
			if (m_export_symbols.contains(foreign.m_function_name.m_lexeme))
			{
				exports.emplace_back(foreign.m_function_name.m_lexeme, BytecodeModule::SymbolType::FOREIGN_FUNCTION, m_top_level_names.at(foreign.m_function_name.m_lexeme).m_slot, foreign.m_function_name);
			}
		}
		else if (statement->IsStatement<MidoriStatement::Struct>())
		{
			const Token& name = statement->GetStatement<MidoriStatement::Struct>().m_name;
			if (m_export_symbols.contains(name.m_lexeme))
			{
				exports.emplace_back(name.m_lexeme, BytecodeModule::SymbolType::STRUCT_TYPE, std::nullopt, name);
			}
		}
		else if (statement->IsStatement<MidoriStatement::Union>())
		{
			const Token& name = statement->GetStatement<MidoriStatement::Union>().m_name;
			if (m_export_symbols.contains(name.m_lexeme))
			{
				exports.emplace_back(name.m_lexeme, BytecodeModule::SymbolType::UNION_TYPE, std::nullopt, name);
			}
		}
	}
	return exports;
}

Lowering::Emitted Lowering::LowerTopLevelStatement(MidoriStatement& statement)
{
	if (statement.IsStatement<MidoriStatement::ForeignDefinition>())
	{
		const MidoriStatement::ForeignDefinition& foreign = statement.GetStatement<MidoriStatement::ForeignDefinition>();
		const std::string& name = foreign.m_function_name.m_lexeme;
		Builder().AtLine(foreign.m_function_name.m_line);
		Builder().Emit(MidoriIROp::GlobalDefine, UnitType(), { Builder().ConstText(foreign.m_foreign_name) }, m_top_level_names.at(name).m_slot);
		return {};
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

	const TopLevelName& name = m_top_level_names.at(definition.m_name.m_lexeme);
	const int line = definition.m_name.m_line;
	if (!name.m_function.has_value())
	{
		return Lower(*definition.m_value)
			.transform([this, &name, line](MidoriIRValueId value)
			{
				Builder().AtLine(line).Emit(MidoriIROp::GlobalDefine, UnitType(), { value }, name.m_slot);
			});
	}

	MidoriExpression::Function& function = definition.m_value->GetExpression<MidoriExpression::Function>();
	const TypeRef closure_type = m_module.m_globals[name.m_slot.m_value].m_type;
	return LowerFunctionBody(name.m_function.value(), closure_type->GetType<MidoriType::FunctionType>().m_param_types, function.m_params, *function.m_body)
		.transform([this, &name, &closure_type, &definition, line]()
		{
			const MidoriIRValueId closure = Builder().AtLine(line).Emit(MidoriIROp::MakeClosure, closure_type, {}, name.m_function.value(), definition.m_name.m_lexeme);
			Builder().Emit(MidoriIROp::GlobalDefine, UnitType(), { closure }, name.m_slot);
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
			return std::unexpected(m_self.Unsupported("a tuple definition", definition.m_names.front()));
		}

		Emitted operator()(MidoriStatement::FunctionDefinition& definition) const
		{
			return std::unexpected(m_self.Unsupported("a local function definition", definition.m_name));
		}

		Emitted operator()(MidoriStatement::ForeignDefinition& foreign) const
		{
			return std::unexpected(m_self.Unsupported("a local foreign function", foreign.m_function_name));
		}

		// Only at the top level, where ReserveTopLevelNames has already
		// stopped the module.
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
			Scope().m_locals.insert_or_assign(local_index, value);
		});
}

// Parameters are the parser's first locals, so parameter i is local i.
Lowering::Emitted Lowering::LowerFunctionBody(MidoriIRFunctionId id, const std::vector<std::shared_ptr<MidoriType>>& param_types, const std::vector<Token>& params, MidoriExpression& body)
{
	FunctionScope scope(id, m_functions[id.m_index]);
	for (size_t index = 0u; index < params.size(); index += 1u)
	{
		const MidoriIRValueId parameter = scope.m_builder.AddParameter(MidoriIRFunction::s_entry_block, param_types[index], params[index].m_lexeme);
		scope.m_locals.emplace(static_cast<int>(index), parameter);
	}

	m_scopes.push_back(&scope);
	const Emitted lowered = LowerReturn(body);
	m_scopes.pop_back();
	return lowered;
}

Lowering::Lowered Lowering::Lower(MidoriExpression& expression)
{
	struct ExpressionLowering
	{
		Lowering& m_self;

		Lowered operator()(MidoriExpression::Literal& literal) const
		{
			return m_self.LowerLiteral(literal);
		}

		Lowered operator()(MidoriExpression::Group& group) const
		{
			return m_self.Lower(*group.m_expr_in);
		}

		Lowered operator()(MidoriExpression::Binary& binary) const
		{
			return m_self.LowerBinary(binary);
		}

		Lowered operator()(MidoriExpression::UnaryPrefix& unary) const
		{
			return m_self.LowerUnary(unary);
		}

		Lowered operator()(MidoriExpression::As& as) const
		{
			return m_self.LowerAs(as);
		}

		Lowered operator()(MidoriExpression::NameAccess& name) const
		{
			return m_self.LowerName(name);
		}

		Lowered operator()(MidoriExpression::IfElse& if_else) const
		{
			return m_self.LowerIf(if_else);
		}

		Lowered operator()(MidoriExpression::Block& block) const
		{
			return m_self.LowerBlock(block);
		}

		Lowered operator()(MidoriExpression::Function& function) const
		{
			return m_self.LowerFunction(function);
		}

		Lowered operator()(MidoriExpression::Call& call) const
		{
			return m_self.LowerCall(call);
		}

		Lowered operator()(MidoriExpression::UnarySuffix& unary) const
		{
			return std::unexpected(m_self.Unsupported("a suffix operator", unary.m_op));
		}

		Lowered operator()(MidoriExpression::Tuple& tuple) const
		{
			return std::unexpected(m_self.Unsupported("a tuple", tuple.m_op));
		}

		Lowered operator()(MidoriExpression::Spawn& spawn) const
		{
			return std::unexpected(m_self.Unsupported("spawn", spawn.m_spawn_keyword));
		}

		Lowered operator()(MidoriExpression::Join& join) const
		{
			return std::unexpected(m_self.Unsupported("join", join.m_join_keyword));
		}

		Lowered operator()(MidoriExpression::ChannelCreate& channel) const
		{
			return std::unexpected(m_self.Unsupported("a channel", channel.m_channel_keyword));
		}

		Lowered operator()(MidoriExpression::Send& send) const
		{
			return std::unexpected(m_self.Unsupported("a send", send.m_arrow));
		}

		Lowered operator()(MidoriExpression::Receive& receive) const
		{
			return std::unexpected(m_self.Unsupported("a receive", receive.m_arrow));
		}

		Lowered operator()(MidoriExpression::Construct& construct) const
		{
			return std::unexpected(m_self.Unsupported("a constructor", construct.m_data_name));
		}

		Lowered operator()(MidoriExpression::RecordUpdate& record_update) const
		{
			return std::unexpected(m_self.Unsupported("a record update", record_update.m_with_keyword));
		}

		Lowered operator()(MidoriExpression::MemberAccess& member) const
		{
			return std::unexpected(m_self.Unsupported("a member access", member.m_member_name));
		}

		Lowered operator()(MidoriExpression::Array& array) const
		{
			return std::unexpected(m_self.Unsupported("an array", array.m_op));
		}

		Lowered operator()(MidoriExpression::IndexAccess& index) const
		{
			return std::unexpected(m_self.Unsupported("an index", index.m_op));
		}

		Lowered operator()(MidoriExpression::ArrayComprehension& comprehension) const
		{
			return std::unexpected(m_self.Unsupported("an array comprehension", comprehension.m_bracket));
		}

		Lowered operator()(MidoriExpression::RangeBinary& range) const
		{
			return std::unexpected(m_self.Unsupported("a range", range.m_range_op));
		}

		Lowered operator()(MidoriExpression::RangeTernary& range) const
		{
			return std::unexpected(m_self.Unsupported("a range", range.m_first_range_op));
		}

		Lowered operator()(MidoriExpression::Match& match) const
		{
			return std::unexpected(m_self.Unsupported("match", match.m_match_keyword));
		}

		Lowered operator()(MidoriExpression::Case& case_expression) const
		{
			return std::unexpected(m_self.Unsupported("match", case_expression.m_keyword));
		}

		Lowered operator()(MidoriExpression::For& for_expression) const
		{
			return std::unexpected(m_self.Unsupported("a for loop", for_expression.m_for_keyword));
		}
	};

	return VisitNode(ExpressionLowering{ *this }, expression);
}

// A call in tail position runs without a new frame, whatever it calls: the
// language promises it. Tail position reaches through `if` branches and a
// block's final expression.
Lowering::Emitted Lowering::LowerReturn(MidoriExpression& expression)
{
	if (expression.IsExpression<MidoriExpression::Group>())
	{
		return LowerReturn(*expression.GetExpression<MidoriExpression::Group>().m_expr_in);
	}

	if (expression.IsExpression<MidoriExpression::IfElse>())
	{
		MidoriExpression::IfElse& if_else = expression.GetExpression<MidoriExpression::IfElse>();
		return Lower(*if_else.m_condition)
			.and_then([this, &if_else](MidoriIRValueId condition) -> Emitted
			{
				const MidoriIRBlockId then_block = Builder().CreateBlock();
				const MidoriIRBlockId else_block = Builder().CreateBlock();
				Builder().AtLine(if_else.m_if_token.m_line).Branch(condition, MidoriIRSuccessor(then_block), MidoriIRSuccessor(else_block));

				Builder().PositionAt(then_block);
				return LowerReturn(*if_else.m_true_branch)
					.and_then([this, &if_else, else_block]() -> Emitted
					{
						Builder().PositionAt(else_block);
						if (if_else.m_else_branch == nullptr)
						{
							Builder().Return(Builder().ConstUnit());
							return {};
						}
						return LowerReturn(*if_else.m_else_branch);
					});
			});
	}

	if (expression.IsExpression<MidoriExpression::Block>())
	{
		MidoriExpression::Block& block = expression.GetExpression<MidoriExpression::Block>();
		for (std::unique_ptr<MidoriStatement>& statement : block.m_stmts)
		{
			const Emitted lowered = LowerStatement(*statement);
			if (!lowered.has_value())
			{
				return lowered;
			}
		}
		if (!block.m_final_expr.has_value())
		{
			Builder().AtLine(block.m_right_brace.m_line).Return(Builder().ConstUnit());
			return {};
		}
		return LowerReturn(*block.m_final_expr.value());
	}

	if (expression.IsExpression<MidoriExpression::Call>() && !expression.GetExpression<MidoriExpression::Call>().m_is_foreign)
	{
		return LowerTailCall(expression.GetExpression<MidoriExpression::Call>());
	}

	return Lower(expression)
		.transform([this](MidoriIRValueId value)
		{
			Builder().Return(value);
		});
}

Lowering::Lowered Lowering::LowerLiteral(const MidoriExpression::Literal& literal)
{
	const Token& token = literal.m_token;
	Builder().AtLine(token.m_line);
	switch (literal.m_kind)
	{
	case MidoriExpression::LiteralKind::Text:
		return Builder().ConstText(token.m_lexeme);
	case MidoriExpression::LiteralKind::Bool:
		return Builder().ConstBool(token.m_lexeme == "true");
	case MidoriExpression::LiteralKind::Float:
		return Builder().ConstFloat(ParseFloatLiteral(token.m_lexeme).value());
	case MidoriExpression::LiteralKind::Unit:
		return Builder().ConstUnit();
	case MidoriExpression::LiteralKind::Integer:
	{
		const std::optional<int64_t> value = ParseIntegerLiteral(token.m_lexeme);
		if (!value.has_value())
		{
			return std::unexpected(MidoriError::GenerateCodeGeneratorErrorWithContext("Integer literal '" + token.m_lexeme + "' is out of range. Maximum value is 9223372036854775807 (2^63 - 1), minimum value is -9223372036854775808 (-2^63).", token, m_file_name, m_source_lines));
		}
		return Builder().ConstInt(value.value());
	}
	case MidoriExpression::LiteralKind::Byte:
	{
		const std::optional<uint64_t> value = ParseUnsignedLiteral(token.m_lexeme);
		if (!value.has_value() || value.value() > 0xFFu)
		{
			return std::unexpected(MidoriError::GenerateCodeGeneratorErrorWithContext("Byte literal '" + token.m_lexeme + "' is out of range. Maximum value is 255 (0xFF).", token, m_file_name, m_source_lines));
		}
		return Builder().Emit(MidoriIROp::Const, MidoriType::MakeLiteralType<MidoriType::ByteType>(), {}, MidoriIRByte{ static_cast<uint8_t>(value.value()) });
	}
	case MidoriExpression::LiteralKind::Word:
	{
		const std::optional<uint64_t> value = ParseUnsignedLiteral(token.m_lexeme);
		if (!value.has_value())
		{
			return std::unexpected(MidoriError::GenerateCodeGeneratorErrorWithContext("Word literal '" + token.m_lexeme + "' is out of range. Maximum value is 18446744073709551615 (0xFFFFFFFFFFFFFFFF).", token, m_file_name, m_source_lines));
		}
		return Builder().Emit(MidoriIROp::Const, MidoriType::MakeLiteralType<MidoriType::WordType>(), {}, MidoriIRWord{ value.value() });
	}
	}
	std::unreachable();
}

Lowering::Lowered Lowering::LowerBinary(MidoriExpression::Binary& binary)
{
	const Token& op = binary.m_op;
	if (op.m_token_name == Token::Name::DOUBLE_AMPERSAND || op.m_token_name == Token::Name::DOUBLE_BAR)
	{
		return LowerShortCircuit(binary);
	}
	if (binary.m_uses_concatenable || binary.m_uses_equatable || binary.m_uses_orderable)
	{
		return std::unexpected(Unsupported("an operator a class instance provides", op));
	}

	const TypeRef& operand_type = binary.m_left->GetType();
	const std::optional<MidoriIRScalar> scalar = ScalarOf(operand_type);
	const std::optional<ScalarOps> ops = BinaryOps(op.m_token_name);
	const std::optional<MidoriIROp> ir_op = (scalar.has_value() && ops.has_value()) ? SelectOp(ops.value(), scalar.value()) : std::nullopt;
	if (!ir_op.has_value())
	{
		return std::unexpected(Unsupported(std::format("'{}' on {}", op.m_lexeme, operand_type->DisplayString()), op));
	}

	return Lower(*binary.m_left)
		.and_then([this, &binary](MidoriIRValueId left)
		{
			return Lower(*binary.m_right)
				.transform([left](MidoriIRValueId right) { return std::pair{ left, right }; });
		})
		.transform([this, &op, ir_op, scalar](std::pair<MidoriIRValueId, MidoriIRValueId> operands)
		{
			Builder().AtLine(op.m_line);
			if (GetMidoriIROpInfo(ir_op.value()).m_arity == MidoriIRArity::Binary)
			{
				return Builder().Binary(ir_op.value(), operands.first, operands.second);
			}
			return Builder().Emit(ir_op.value(), MidoriIRScalarType(scalar.value()), { operands.first, operands.second });
		});
}

// `a && b` is `b` when `a` holds and `a` otherwise; `a || b` the other way.
Lowering::Lowered Lowering::LowerShortCircuit(MidoriExpression::Binary& binary)
{
	const bool is_and = binary.m_op.m_token_name == Token::Name::DOUBLE_AMPERSAND;
	return Lower(*binary.m_left)
		.and_then([this, &binary, is_and](MidoriIRValueId left) -> Lowered
		{
			const MidoriIRBlockId right_block = Builder().CreateBlock();
			const MidoriIRBlockId join = Builder().CreateBlock();
			const MidoriIRValueId result = Builder().AddParameter(join, MidoriIRScalarType(MidoriIRScalar::Bool));
			const MidoriIRSuccessor evaluate_right(right_block);
			const MidoriIRSuccessor skip_right(join, { left });
			Builder().AtLine(binary.m_op.m_line).Branch(left, is_and ? evaluate_right : skip_right, is_and ? skip_right : evaluate_right);

			Builder().PositionAt(right_block);
			return Lower(*binary.m_right)
				.transform([this, join, result](MidoriIRValueId right)
				{
					Builder().Jump(join, { right });
					Builder().PositionAt(join);
					return result;
				});
		});
}

Lowering::Lowered Lowering::LowerUnary(MidoriExpression::UnaryPrefix& unary)
{
	const Token& op = unary.m_op;
	if (op.m_token_name == Token::Name::SINGLE_PLUS)
	{
		return Lower(*unary.m_expr);
	}

	const TypeRef& operand_type = unary.m_expr->GetType();
	const std::optional<MidoriIRScalar> scalar = ScalarOf(operand_type);
	const std::optional<MidoriIROp> ir_op = [&]() -> std::optional<MidoriIROp>
	{
		if (!scalar.has_value())
		{
			return std::nullopt;
		}
		using enum MidoriIROp;
		switch (op.m_token_name)
		{
		case Token::Name::SINGLE_MINUS:
			return SelectOp(ScalarOps{ NegInt, NegFloat, std::nullopt, std::nullopt, std::nullopt, std::nullopt }, scalar.value());
		case Token::Name::BANG:
			return SelectOp(ScalarOps{ std::nullopt, std::nullopt, std::nullopt, std::nullopt, NotBool, std::nullopt }, scalar.value());
		case Token::Name::TILDE:
			return SelectOp(ScalarOps{ BitNotInt, std::nullopt, BitNotByte, BitNotWord, std::nullopt, std::nullopt }, scalar.value());
		default:
			return std::nullopt;
		}
	}();
	if (!ir_op.has_value())
	{
		return std::unexpected(Unsupported(std::format("'{}' on {}", op.m_lexeme, operand_type->DisplayString()), op));
	}

	return Lower(*unary.m_expr)
		.transform([this, &op, ir_op](MidoriIRValueId operand)
		{
			return Builder().AtLine(op.m_line).Unary(ir_op.value(), operand);
		});
}

Lowering::Lowered Lowering::LowerAs(MidoriExpression::As& as)
{
	const Token& keyword = as.m_as_keyword;
	if (as.m_uses_convertable)
	{
		return std::unexpected(Unsupported("a conversion a Convertable instance provides", keyword));
	}

	const TypeRef from_type = as.m_from_type.lock();
	const TypeRef& to_type = as.m_to_type;
	if (*from_type == *to_type)
	{
		return Lower(*as.m_expr);
	}

	const std::optional<MidoriIRScalar> from = ScalarOf(from_type);
	const std::optional<MidoriIRScalar> to = ScalarOf(to_type);
	const bool is_bool_to_text = from == MidoriIRScalar::Bool && to == MidoriIRScalar::Text;
	const std::vector<MidoriIROp> ops = (from.has_value() && to.has_value()) ? ConversionOps(from.value(), to.value()) : std::vector<MidoriIROp>{};
	if (!is_bool_to_text && ops.empty())
	{
		return std::unexpected(Unsupported(std::format("a conversion from {} to {}", from_type->DisplayString(), to_type->DisplayString()), keyword));
	}

	return Lower(*as.m_expr)
		.and_then([this, &keyword, is_bool_to_text, &ops](MidoriIRValueId value) -> Lowered
		{
			Builder().AtLine(keyword.m_line);
			if (is_bool_to_text)
			{
				return LowerBoolToText(value, keyword.m_line);
			}
			return std::ranges::fold_left(ops, value, [this](MidoriIRValueId operand, MidoriIROp op) { return Builder().Unary(op, operand); });
		});
}

Lowering::Lowered Lowering::LowerBoolToText(MidoriIRValueId value, int line)
{
	const MidoriIRBlockId when_true = Builder().CreateBlock();
	const MidoriIRBlockId when_false = Builder().CreateBlock();
	const MidoriIRBlockId join = Builder().CreateBlock();
	const MidoriIRValueId text = Builder().AddParameter(join, MidoriIRScalarType(MidoriIRScalar::Text));
	Builder().AtLine(line).Branch(value, MidoriIRSuccessor(when_true), MidoriIRSuccessor(when_false));

	Builder().PositionAt(when_true);
	Builder().Jump(join, { Builder().ConstText("true") });
	Builder().PositionAt(when_false);
	Builder().Jump(join, { Builder().ConstText("false") });
	Builder().PositionAt(join);
	return text;
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
		return std::unexpected(Unsupported("a name a closure captures", token));
	}
	if (m_builtin_foreigns.contains(token.m_lexeme))
	{
		return std::unexpected(Unsupported("a foreign function used as a value", token));
	}

	return GlobalFor(token, name.m_type_data)
		.transform([this, &token](MidoriIRGlobalSlot slot)
		{
			const MidoriIRGlobal& global = m_module.m_globals[slot.m_value];
			return Builder().AtLine(token.m_line).Emit(MidoriIROp::GlobalGet, global.m_type, {}, slot, global.m_name);
		});
}

Lowering::Lowered Lowering::LowerIf(MidoriExpression::IfElse& if_else)
{
	return Lower(*if_else.m_condition)
		.and_then([this, &if_else](MidoriIRValueId condition) -> Lowered
		{
			const MidoriIRBlockId then_block = Builder().CreateBlock();
			const MidoriIRBlockId else_block = Builder().CreateBlock();
			const MidoriIRBlockId join = Builder().CreateBlock();
			const MidoriIRValueId result = Builder().AddParameter(join, if_else.m_type_data);
			Builder().AtLine(if_else.m_if_token.m_line).Branch(condition, MidoriIRSuccessor(then_block), MidoriIRSuccessor(else_block));

			return LowerJoinedBranch(then_block, if_else.m_true_branch.get(), join)
				.and_then([&]() { return LowerJoinedBranch(else_block, if_else.m_else_branch.get(), join); })
				.transform([this, join, result]()
				{
					Builder().PositionAt(join);
					return result;
				});
		});
}

// An `if` without an `else` is Unit when its condition fails.
Lowering::Emitted Lowering::LowerJoinedBranch(MidoriIRBlockId block, MidoriExpression* branch, MidoriIRBlockId join)
{
	Builder().PositionAt(block);
	const Lowered value = branch == nullptr ? Lowered(Builder().ConstUnit()) : Lower(*branch);
	return value.transform([this, join](MidoriIRValueId branch_value) { Builder().Jump(join, { branch_value }); });
}

Lowering::Lowered Lowering::LowerBlock(MidoriExpression::Block& block)
{
	for (std::unique_ptr<MidoriStatement>& statement : block.m_stmts)
	{
		const Emitted lowered = LowerStatement(*statement);
		if (!lowered.has_value())
		{
			return std::unexpected(lowered.error());
		}
	}

	if (block.m_final_expr.has_value())
	{
		return Lower(*block.m_final_expr.value());
	}
	return Builder().AtLine(block.m_right_brace.m_line).ConstUnit();
}

Lowering::Lowered Lowering::LowerFunction(MidoriExpression::Function& function)
{
	const Token& keyword = function.m_function_keyword;
	if (function.m_captured_count > 0)
	{
		return std::unexpected(Unsupported("a closure that can capture a local", keyword));
	}

	const TypeRef closure_type = ClosureType(function.m_type_data);
	const MidoriType::FunctionType& signature = closure_type->GetType<MidoriType::FunctionType>();
	const MidoriIRFunctionId id = NewFunction(std::format("Anonymous Function at line: {}", keyword.m_line), signature.m_return_type);
	return LowerFunctionBody(id, signature.m_param_types, function.m_params, *function.m_body)
		.transform([this, id, &closure_type, &keyword]()
		{
			return Builder().AtLine(keyword.m_line).Emit(MidoriIROp::MakeClosure, closure_type, {}, id);
		});
}

Lowering::Lowered Lowering::LowerCall(MidoriExpression::Call& call)
{
	const Token& paren = call.m_paren;
	const MidoriExpression::NameAccess* callee_name = GlobalName(*call.m_callee);
	if (callee_name != nullptr && std::ranges::contains(s_intrinsic_calls, callee_name->m_name.m_lexeme))
	{
		return std::unexpected(Unsupported(callee_name->m_name.m_lexeme, callee_name->m_name));
	}

	const std::optional<std::string> foreign = BuiltinForeignCallee(call);
	if (call.m_is_foreign && !foreign.has_value())
	{
		return std::unexpected(Unsupported("a call of another module's foreign function", paren));
	}

	return LowerArguments(call.m_arguments)
		.and_then([this, &call, &paren, callee_name, &foreign](std::vector<MidoriIRValueId> arguments) -> Lowered
		{
			const TypeRef& type = call.m_type_data;
			if (foreign.has_value())
			{
				return Builder().AtLine(paren.m_line).Emit(MidoriIROp::CallForeign, type, std::move(arguments), MidoriIRForeign{ foreign.value() });
			}

			const std::optional<MidoriIRFunctionId> direct = DirectCallee(*call.m_callee);
			if (direct.has_value())
			{
				return Builder().AtLine(paren.m_line).Emit(MidoriIROp::Call, type, std::move(arguments), direct.value());
			}

			if (callee_name != nullptr)
			{
				return GlobalFor(callee_name->m_name, callee_name->m_type_data)
					.transform([this, &paren, &type, &arguments](MidoriIRGlobalSlot slot)
					{
						return Builder().AtLine(paren.m_line).Emit(MidoriIROp::CallGlobal, type, std::move(arguments), slot);
					});
			}

			return Lower(*call.m_callee)
				.transform([this, &paren, &type, &arguments](MidoriIRValueId callee)
				{
					arguments.insert(arguments.begin(), callee);
					return Builder().AtLine(paren.m_line).Emit(MidoriIROp::CallValue, type, std::move(arguments));
				});
		});
}

Lowering::Emitted Lowering::LowerTailCall(MidoriExpression::Call& call)
{
	const Token& paren = call.m_paren;
	const MidoriExpression::NameAccess* callee_name = GlobalName(*call.m_callee);
	if (callee_name != nullptr && std::ranges::contains(s_intrinsic_calls, callee_name->m_name.m_lexeme))
	{
		return std::unexpected(Unsupported(callee_name->m_name.m_lexeme, callee_name->m_name));
	}
	if (!(*call.m_type_data == *m_functions[Scope().m_id.m_index].m_return_type))
	{
		return std::unexpected(Unsupported("a tail call whose type is not the function's return type", paren));
	}

	return LowerArguments(call.m_arguments)
		.and_then([this, &call, &paren, callee_name](std::vector<MidoriIRValueId> arguments) -> Emitted
		{
			const std::optional<MidoriIRFunctionId> direct = DirectCallee(*call.m_callee);
			if (direct.has_value())
			{
				Builder().AtLine(paren.m_line).TailCall(direct.value(), std::move(arguments));
				return {};
			}

			if (callee_name != nullptr)
			{
				return GlobalFor(callee_name->m_name, callee_name->m_type_data)
					.transform([this, &paren, &arguments](MidoriIRGlobalSlot slot)
					{
						Builder().AtLine(paren.m_line).TailCall(slot, std::move(arguments));
					});
			}

			return Lower(*call.m_callee)
				.transform([this, &paren, &arguments](MidoriIRValueId callee)
				{
					arguments.insert(arguments.begin(), callee);
					Builder().AtLine(paren.m_line).TailCall({}, std::move(arguments));
				});
		});
}

std::expected<std::vector<MidoriIRValueId>, CompilerError> Lowering::LowerArguments(std::vector<std::unique_ptr<MidoriExpression>>& arguments)
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

std::expected<MidoriIRGlobalSlot, CompilerError> Lowering::GlobalFor(const Token& name, const std::shared_ptr<MidoriType>& type)
{
	const std::string& lexeme = name.m_lexeme;
	const std::unordered_map<std::string, TopLevelName>::const_iterator own = m_top_level_names.find(lexeme);
	if (own != m_top_level_names.cend())
	{
		return own->second.m_slot;
	}

	const size_t separator = lexeme.find(NameSeparator);
	if (separator == std::string::npos)
	{
		return std::unexpected(Unsupported(std::format("'{}'", lexeme), name));
	}

	const std::string module = lexeme.substr(0u, separator);
	const std::string symbol = lexeme.substr(separator + NameSeparator.length());
	if (m_imports.m_generic_functions.contains(lexeme))
	{
		return std::unexpected(Unsupported("a generic function", name));
	}
	const std::unordered_map<std::string, std::unordered_set<std::string>>::const_iterator class_methods = m_imports.m_class_methods.find(module);
	if (class_methods != m_imports.m_class_methods.cend() && class_methods->second.contains(symbol))
	{
		return std::unexpected(Unsupported("a class method", name));
	}

	const std::unordered_map<std::string, MidoriIRGlobalSlot>::const_iterator imported = m_import_slots.find(lexeme);
	if (imported != m_import_slots.cend())
	{
		return imported->second;
	}

	const TypeRef import_type = type->IsType<MidoriType::FunctionType>() ? ClosureType(type) : type;
	const MidoriIRGlobalSlot slot = m_module.ReserveImport(module, symbol, import_type);
	m_import_slots.emplace(lexeme, slot);
	m_import_tokens.emplace_back(slot, name);
	return slot;
}

std::optional<MidoriIRFunctionId> Lowering::DirectCallee(const MidoriExpression& callee) const
{
	const MidoriExpression::NameAccess* name = GlobalName(callee);
	if (name == nullptr)
	{
		return std::nullopt;
	}
	const std::unordered_map<std::string, TopLevelName>::const_iterator own = m_top_level_names.find(name->m_name.m_lexeme);
	return own == m_top_level_names.cend() ? std::nullopt : own->second.m_function;
}

std::optional<std::string> Lowering::BuiltinForeignCallee(const MidoriExpression::Call& call) const
{
	const MidoriExpression::NameAccess* name = GlobalName(*call.m_callee);
	if (!call.m_is_foreign || name == nullptr)
	{
		return std::nullopt;
	}
	const std::unordered_map<std::string, std::string>::const_iterator foreign = m_builtin_foreigns.find(name->m_name.m_lexeme);
	return foreign == m_builtin_foreigns.cend() ? std::nullopt : std::optional<std::string>(foreign->second);
}
