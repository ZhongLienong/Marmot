#include <algorithm>
#include <format>
#include <fstream>
#include <limits>
#include <queue>
#include <ranges>
#include <sstream>

#include "Common/Constant/Constant.h"
#include "Compiler/Lexer/Lexer.h"
#include "Parser.h"

using namespace std::string_literals;

namespace
{
	using ParamTuple = std::pair<Token, std::shared_ptr<MidoriType>>;
	using StructMemberTuple = std::tuple<std::shared_ptr<MidoriType>, std::string>;
	using UnionMemberTuple = std::tuple<std::string, std::vector<std::shared_ptr<MidoriType>>, int>;
	using UnionMemberInfo = std::unordered_map<std::string, MidoriType::UnionType::UnionMemberContext>;

	struct ParamSplit
	{
		std::vector<Token> m_params;
		std::vector<std::shared_ptr<MidoriType>> m_types;
	};

	struct StructMemberSplit
	{
		std::vector<std::shared_ptr<MidoriType>> m_types;
		std::vector<std::string> m_names;
	};

	ParamSplit SplitParamTuples(std::vector<ParamTuple>&& tuples)
	{
		ParamSplit result;
		result.m_params.reserve(tuples.size());
		result.m_types.reserve(tuples.size());
		for (ParamTuple& tuple : tuples)
		{
			result.m_params.emplace_back(std::move(tuple.first));
			result.m_types.emplace_back(std::move(tuple.second));
		}
		return result;
	}

	StructMemberSplit SplitStructMemberTuples(std::vector<StructMemberTuple>&& tuples)
	{
		StructMemberSplit result;
		result.m_types.reserve(tuples.size());
		result.m_names.reserve(tuples.size());
		for (StructMemberTuple& tuple : tuples)
		{
			result.m_types.emplace_back(std::move(std::get<0>(tuple)));
			result.m_names.emplace_back(std::move(std::get<1>(tuple)));
		}
		return result;
	}

	UnionMemberInfo BuildUnionMemberInfo(std::vector<UnionMemberTuple>&& members)
	{
		UnionMemberInfo info;
		info.reserve(members.size());
		for (UnionMemberTuple& member : members)
		{
			std::string& name = std::get<0>(member);
			std::vector<std::shared_ptr<MidoriType>>& types = std::get<1>(member);
			int tag = std::get<2>(member);
			info.emplace(std::move(name), MidoriType::UnionType::UnionMemberContext{ std::move(types), tag });
		}
		return info;
	}

	bool ContainsConstraint(const std::vector<MidoriType::ClassConstraint>& constraints, const MidoriType::ClassConstraint& constraint)
	{
		return std::ranges::any_of
		(
			constraints,
			[&constraint](const MidoriType::ClassConstraint& existing)
			{
				return existing == constraint;
			}
		);
	}

	void AppendUniqueConstraint(std::vector<MidoriType::ClassConstraint>& constraints, MidoriType::ClassConstraint&& constraint)
	{
		if (!ContainsConstraint(constraints, constraint))
		{
			constraints.push_back(std::move(constraint));
		}
	}

	// `close`, `is_done` and `cancel` were unqualified builtins while everything else
	// concurrency moved under Concurrency::. Writing one now names its replacement.
	// A case arm covers its whole constructor only when every argument it carries
	// matches anything. `Err(_)` covers `Err`; `Err(Alpha)` discriminates within it,
	// so a later `Err(Beta)` is a different case rather than a duplicate.
	bool IsCatchAllPattern(const MidoriPattern& pattern)
	{
		if (pattern.IsPattern<MidoriPattern::Binding>() || pattern.IsPattern<MidoriPattern::Wildcard>())
		{
			return true;
		}

		if (pattern.IsPattern<MidoriPattern::Tuple>())
		{
			const MidoriPattern::Tuple& tuple = pattern.GetPattern<MidoriPattern::Tuple>();
			return std::ranges::all_of(tuple.m_elements, [](const std::unique_ptr<MidoriPattern>& element) { return IsCatchAllPattern(*element); });
		}

		return false;
	}

	// A rendering that two arms share exactly when they match the same values, used
	// to keep reporting the arm that really is written twice. A binding and a
	// wildcard render alike because they accept alike.
	std::string PatternSignature(const MidoriPattern& pattern)
	{
		if (pattern.IsPattern<MidoriPattern::Binding>() || pattern.IsPattern<MidoriPattern::Wildcard>())
		{
			return "_";
		}

		if (pattern.IsPattern<MidoriPattern::Literal>())
		{
			return pattern.GetPattern<MidoriPattern::Literal>().m_token.m_lexeme;
		}

		const auto join = [](const std::vector<std::unique_ptr<MidoriPattern>>& parts) -> std::string
		{
			std::string rendered;
			for (const std::unique_ptr<MidoriPattern>& part : parts)
			{
				if (!rendered.empty())
				{
					rendered.append(", ");
				}
				rendered.append(PatternSignature(*part));
			}
			return rendered;
		};

		if (pattern.IsPattern<MidoriPattern::Tuple>())
		{
			return "(" + join(pattern.GetPattern<MidoriPattern::Tuple>().m_elements) + ")";
		}

		if (pattern.IsPattern<MidoriPattern::Array>())
		{
			return "[" + join(pattern.GetPattern<MidoriPattern::Array>().m_elements) + "]";
		}

		const MidoriPattern::Constructor& constructor = pattern.GetPattern<MidoriPattern::Constructor>();
		return constructor.m_name + "(" + join(constructor.m_args) + ")";
	}

	bool IsRemovedConcurrencyBuiltin(std::string_view name)
	{
		return name == "close" || name == "is_done" || name == "cancel";
	}

	// Concurrency::Spawn, Join and MakeChannel are written as calls but have no
	// declaration in Concurrency.mmt: the parser turns each call into the node the
	// compiler already checks and lowers, as it resolves Result for Join by name.
	constexpr std::string_view ConcurrencyModuleName = "Concurrency";
	constexpr std::string_view SpawnIntrinsicName = "Concurrency::Spawn";
	constexpr std::string_view JoinIntrinsicName = "Concurrency::Join";
	constexpr std::string_view MakeChannelIntrinsicName = "Concurrency::MakeChannel";
	constexpr std::string_view CloseIntrinsicName = "Concurrency::Close";
	constexpr std::string_view IsDoneIntrinsicName = "Concurrency::IsDone";
	constexpr std::string_view CancelIntrinsicName = "Concurrency::Cancel";

	bool IsConcurrencyIntrinsicName(std::string_view name)
	{
		return name == SpawnIntrinsicName || name == JoinIntrinsicName || name == MakeChannelIntrinsicName
			|| name == CloseIntrinsicName || name == IsDoneIntrinsicName || name == CancelIntrinsicName;
	}

	// Cell::New, Get and Set are compiler-provided like the concurrency calls above:
	// importing Cell.mmt makes them, and the Cell<T> type, available.
	constexpr std::string_view CellModuleName = "Cell";
	constexpr std::string_view CellNewIntrinsicName = "Cell::New";
	constexpr std::string_view CellGetIntrinsicName = "Cell::Get";
	constexpr std::string_view CellSetIntrinsicName = "Cell::Set";

	bool IsCellIntrinsicName(std::string_view name)
	{
		return name == CellNewIntrinsicName || name == CellGetIntrinsicName || name == CellSetIntrinsicName;
	}

	void CollectTypeConstraints(
		const std::shared_ptr<MidoriType>& type,
		std::vector<MidoriType::ClassConstraint>& constraints,
		std::unordered_set<const MidoriType*>& visited
	)
	{
		if (type == nullptr || !visited.insert(type.get()).second)
		{
			return;
		}

		if (type->IsType<MidoriType::ArrayType>())
		{
			CollectTypeConstraints(type->GetType<MidoriType::ArrayType>().m_element_type, constraints, visited);
			return;
		}

		if (type->IsType<MidoriType::CellType>())
		{
			CollectTypeConstraints(type->GetType<MidoriType::CellType>().m_element_type, constraints, visited);
			return;
		}

		if (type->IsType<MidoriType::RangeType>())
		{
			CollectTypeConstraints(type->GetType<MidoriType::RangeType>().m_element_type, constraints, visited);
			return;
		}

		if (type->IsType<MidoriType::TupleType>())
		{
			for (const std::shared_ptr<MidoriType>& element_type : type->GetType<MidoriType::TupleType>().m_element_types)
			{
				CollectTypeConstraints(element_type, constraints, visited);
			}
			return;
		}

		if (type->IsType<MidoriType::FunctionType>())
		{
			const MidoriType::FunctionType& function_type = type->GetType<MidoriType::FunctionType>();
			for (const std::shared_ptr<MidoriType>& param_type : function_type.m_param_types)
			{
				CollectTypeConstraints(param_type, constraints, visited);
			}
			CollectTypeConstraints(function_type.m_return_type, constraints, visited);
			for (const MidoriType::ClassConstraint& constraint : function_type.m_constraints)
			{
				AppendUniqueConstraint(constraints, MidoriType::ClassConstraint(constraint));
			}
			return;
		}

		if (type->IsType<MidoriType::StructType>())
		{
			const MidoriType::StructType& struct_type = type->GetType<MidoriType::StructType>();
			for (const MidoriType::ClassConstraint& constraint : struct_type.m_constraints)
			{
				AppendUniqueConstraint(constraints, MidoriType::ClassConstraint(constraint));
			}
			for (const std::shared_ptr<MidoriType>& member_type : struct_type.m_member_types)
			{
				CollectTypeConstraints(member_type, constraints, visited);
			}
			return;
		}

		if (type->IsType<MidoriType::UnionType>())
		{
			const MidoriType::UnionType& union_type = type->GetType<MidoriType::UnionType>();
			for (const MidoriType::ClassConstraint& constraint : union_type.m_constraints)
			{
				AppendUniqueConstraint(constraints, MidoriType::ClassConstraint(constraint));
			}
			for (const auto& [_, member_ctx] : union_type.m_member_info)
			{
				for (const std::shared_ptr<MidoriType>& member_type : member_ctx.m_member_types)
				{
					CollectTypeConstraints(member_type, constraints, visited);
				}
			}
		}
	}

	std::vector<MidoriType::ClassConstraint> CollectSignatureConstraints(
		const std::vector<std::shared_ptr<MidoriType>>& param_types,
		const std::shared_ptr<MidoriType>& return_type
	)
	{
		std::vector<MidoriType::ClassConstraint> constraints;
		std::unordered_set<const MidoriType*> visited;
		for (const std::shared_ptr<MidoriType>& param_type : param_types)
		{
			CollectTypeConstraints(param_type, constraints, visited);
		}
		CollectTypeConstraints(return_type, constraints, visited);
		return constraints;
	}

	std::string_view TopLevelNamespace(std::string_view full_name)
	{
		size_t pos = full_name.find('.');
		if (pos != std::string_view::npos)
		{
			return full_name.substr(0u, pos);
		}
		return full_name;
	}

	bool IsExportedInAnyModule(const std::unordered_map<std::string, ModuleDeclaration>& modules, const std::string& symbol_name)
	{
		for (const std::pair<const std::string, ModuleDeclaration>& entry : modules)
		{
			if (entry.second.HasExport(symbol_name))
			{
				return true;
			}
		}
		return false;
	}

	struct PatternBindingCounter
	{
		static int Count(const MidoriPattern& pattern)
		{
			return std::visit(PatternBindingCounter{}, *pattern);
		}

		int operator()(const MidoriPattern::Binding&) const
		{
			return 1;
		}

		int operator()(const MidoriPattern::Wildcard&) const
		{
			return 0;
		}

		int operator()(const MidoriPattern::Literal&) const
		{
			return 0;
		}

		int operator()(const MidoriPattern::Tuple& tuple) const
		{
			return CountElements(tuple.m_elements);
		}

		int operator()(const MidoriPattern::Array& array) const
		{
			return CountElements(array.m_elements);
		}

		int operator()(const MidoriPattern::Constructor& constructor) const
		{
			return CountElements(constructor.m_args);
		}

	private:
		static int CountElements(const std::vector<std::unique_ptr<MidoriPattern>>& elements)
		{
			int total = 0;
			for (const std::unique_ptr<MidoriPattern>& elem : elements)
			{
				total += Count(*elem);
			}
			return total;
		}
	};

	std::unique_ptr<MidoriPattern> MakeNumericLiteralPattern(Token token)
	{
		const std::string& lexeme = token.m_lexeme;
		if (lexeme.size() >= 3u && lexeme[0u] == '0' && (lexeme[1u] == 'x' || lexeme[1u] == 'X' || lexeme[1u] == 'b' || lexeme[1u] == 'B'))
		{
			// A lexeme too large for 64 bits is classified as Word; code generation reports it.
			const uint64_t value = ParseUnsignedLiteral(lexeme).value_or(std::numeric_limits<uint64_t>::max());

			if (value <= 0xFF)
			{
				return std::make_unique<MidoriPattern>(MidoriPattern::Literal(token, MidoriPattern::LiteralKind::Byte));
			}
			if (value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
			{
				return std::make_unique<MidoriPattern>(MidoriPattern::Literal(token, MidoriPattern::LiteralKind::Integer));
			}
			return std::make_unique<MidoriPattern>(MidoriPattern::Literal(token, MidoriPattern::LiteralKind::Word));
		}

		return std::make_unique<MidoriPattern>(MidoriPattern::Literal(token, MidoriPattern::LiteralKind::Integer));
	}

	bool IsExactGenericParam(const std::shared_ptr<MidoriType>& type, std::string_view name)
	{
		return type->IsType<MidoriType::GenericParam>() && type->GetType<MidoriType::GenericParam>().m_name == name;
	}

	bool IsUnionSelfReference(const std::shared_ptr<MidoriType>& type, std::string_view union_name)
	{
		return type->IsType<MidoriType::UnionType>() && type->GetType<MidoriType::UnionType>().m_name == union_name;
	}

	bool ContainsGenericParam(const std::shared_ptr<MidoriType>& type, std::string_view name, std::unordered_set<const MidoriType*>& visited)
	{
		if (visited.contains(type.get()))
		{
			return false;
		}
		visited.emplace(type.get());

		if (IsExactGenericParam(type, name))
		{
			return true;
		}
		if (type->IsType<MidoriType::ArrayType>())
		{
			return ContainsGenericParam(type->GetType<MidoriType::ArrayType>().m_element_type, name, visited);
		}

		if (type->IsType<MidoriType::CellType>())
		{
			return ContainsGenericParam(type->GetType<MidoriType::CellType>().m_element_type, name, visited);
		}
		if (type->IsType<MidoriType::RangeType>())
		{
			return ContainsGenericParam(type->GetType<MidoriType::RangeType>().m_element_type, name, visited);
		}
		if (type->IsType<MidoriType::TupleType>())
		{
			for (const std::shared_ptr<MidoriType>& elem_type : type->GetType<MidoriType::TupleType>().m_element_types)
			{
				if (ContainsGenericParam(elem_type, name, visited))
				{
					return true;
				}
			}
			return false;
		}
		if (type->IsType<MidoriType::FunctionType>())
		{
			const MidoriType::FunctionType& function_type = type->GetType<MidoriType::FunctionType>();
			for (const std::shared_ptr<MidoriType>& param_type : function_type.m_param_types)
			{
				if (ContainsGenericParam(param_type, name, visited))
				{
					return true;
				}
			}
			return ContainsGenericParam(function_type.m_return_type, name, visited);
		}
		if (type->IsType<MidoriType::StructType>())
		{
			for (const std::shared_ptr<MidoriType>& member_type : type->GetType<MidoriType::StructType>().m_member_types)
			{
				if (ContainsGenericParam(member_type, name, visited))
				{
					return true;
				}
			}
			return false;
		}
		if (type->IsType<MidoriType::UnionType>())
		{
			for (const auto& [member_name, member_ctx] : type->GetType<MidoriType::UnionType>().m_member_info)
			{
				for (const std::shared_ptr<MidoriType>& member_type : member_ctx.m_member_types)
				{
					if (ContainsGenericParam(member_type, name, visited))
					{
						return true;
					}
				}
			}
			return false;
		}
		return false;
	}

	bool ContainsGenericParam(const std::shared_ptr<MidoriType>& type, std::string_view name)
	{
		std::unordered_set<const MidoriType*> visited;
		return ContainsGenericParam(type, name, visited);
	}
}

Parser::ParseContext::ParseContext(TokenStream&& tokens, std::string_view file_name, const std::vector<std::string>& source_lines, const std::unordered_map<std::string, CompiledModule::SymbolTable>& imports, const std::unordered_map<std::string, TypeEnvironment>& imported_type_signatures, const ModuleDeclaration* module_decl)
	: m_imported_symbols(imports),
	m_imported_type_signatures(imported_type_signatures),
	m_tokens(std::move(tokens)),
	m_file_name(file_name),
	m_current_module(module_decl),
	m_module_declarations(nullptr),
	m_use_imports(nullptr),
	m_source_lines(&source_lines)
{
}

Parser::ParseState::ParseState(const std::vector<UseImport>& use_imports)
	: m_current_use_imports(use_imports)
{
}

Parser::ActiveConstraintGuard::ActiveConstraintGuard(Parser* parser, size_t prev_size)
	: m_parser(parser),
	m_prev_size(prev_size)
{
}

Parser::ActiveConstraintGuard::~ActiveConstraintGuard()
{
	if (m_parser != nullptr)
	{
		m_parser->m_state.m_active_constraints.resize(m_prev_size);
	}
}

Parser::Parser(TokenStream&& tokens,std::string_view file_name, const std::vector<std::string>& source_lines, const std::unordered_map<std::string, CompiledModule::SymbolTable>& imports, const std::unordered_map<std::string, TypeEnvironment>& imported_type_signatures, const std::vector<UseImport>& use_imports, const ModuleDeclaration* module_decl, const CompiledModule::TypeclassMetadataMap& imported_typeclass_metadata)
	: m_context(std::move(tokens), file_name, source_lines, imports, imported_type_signatures, module_decl),
	m_state(use_imports)
{
	for (const auto& [tc_name, metadata] : imported_typeclass_metadata)
	{
		m_state.m_class_methods[tc_name] = metadata.m_method_names;
		m_state.m_typeclass_type_params[tc_name] = metadata.m_type_param_names;
		m_state.m_typeclass_associated_types[tc_name] = metadata.m_associated_type_names;
		m_state.m_class_instances[tc_name] = metadata.m_instance_methods;
		m_state.m_class_instance_type_args[tc_name] = metadata.m_instance_type_args;
		m_state.m_class_instance_associated_type_bindings[tc_name] = metadata.m_instance_associated_type_bindings;
		m_state.m_typeclass_method_types[tc_name] = metadata.m_method_types;
	}
}

bool Parser::SharesNamespace(const std::string& namespace1, const std::string& namespace2) const
{
	std::string_view top_ns1 = TopLevelNamespace(namespace1);
	std::string_view top_ns2 = TopLevelNamespace(namespace2);

	// Both in global namespace (empty module names)
	if (top_ns1.empty() && top_ns2.empty())
	{
		return true;
	}

	// One is in global, other is not
	if (top_ns1.empty() || top_ns2.empty())
	{
		return false;
	}

	// Check if they share the same top-level namespace
	// Math.Vector and Math.Matrix both share "Math"
	// Math.Vector.Internal and Math.Utils both share "Math"
	return top_ns1 == top_ns2;
}

std::string Parser::ExtractSymbolName(const std::string& qualified_name) const
{
	size_t last_separator = qualified_name.rfind(NameSeparator);
	if (last_separator != std::string::npos)
	{
		return qualified_name.substr(last_separator + NameSeparator.length());
	}
	else
	{
		return qualified_name;
	}
}

std::string Parser::ExtractQualifier(const std::string& qualified_name) const
{
	size_t last_separator = qualified_name.rfind(NameSeparator);
	if (last_separator != std::string::npos)
	{
		return qualified_name.substr(0u, last_separator);
	}
	else
	{
		return {};
	}
}

MidoriResult::ExpressionResult Parser::ResolveQualifiedName(const Token& name_token, const std::string& mangled_name)
{
	std::string lookup_name = mangled_name;
	std::vector<Scope>::const_reverse_iterator found_scope_it = FindVariableScope(lookup_name);

	if (found_scope_it != m_state.m_scopes.rend())
	{
		Scope::VariableTable::const_iterator find_result = found_scope_it->m_variables.find(lookup_name);

		// Global
		if (IsGlobalName(found_scope_it))
		{
			return std::make_unique<MidoriExpression>(MidoriExpression::NameAccess(name_token, MidoriExpression::NameContext::Global()));
		}
		// Local
		else if (IsLocalName(find_result))
		{
			return std::make_unique<MidoriExpression>(MidoriExpression::NameAccess(name_token, MidoriExpression::NameContext::Local(find_result->second.m_relative_index.value())));
		}
		// Cell
		else
		{
			int var_depth = find_result->second.m_function_depth.value();
			int parent_base = (var_depth >= 1) ? m_state.m_function_base_variable_index[static_cast<size_t>(var_depth - 1)] : 0;
			int cell_index = find_result->second.m_absolute_index.value() - parent_base;
			return std::make_unique<MidoriExpression>(MidoriExpression::NameAccess(name_token, MidoriExpression::NameContext::Cell(cell_index)));
		}
	}

	// Not found in local scopes - check imported modules (for bare imports)
	const UseImportResolution use_import_resolution = ResolveUseImport(lookup_name);
	if (use_import_resolution.m_status == UseImportResolutionStatus::Ambiguous)
	{
		return std::unexpected(GenerateParserError(BuildAmbiguousUseImportError(lookup_name, use_import_resolution.m_conflicting_modules), name_token));
	}

	if (use_import_resolution.m_status == UseImportResolutionStatus::Resolved)
	{
		const std::string& imported_module_name = use_import_resolution.m_module_name;
		const ImportedSymbolAccess access = ResolveImportedSymbolAccess(imported_module_name, lookup_name);
		if (access == ImportedSymbolAccess::Accessible)
		{
			Token qualified_token = name_token;
			qualified_token.m_lexeme = imported_module_name + NameSeparator.data() + lookup_name;
			return std::make_unique<MidoriExpression>(MidoriExpression::NameAccess(qualified_token, MidoriExpression::NameContext::Global()));
		}

		return std::unexpected(GenerateParserError(BuildImportedSymbolAccessError(imported_module_name, lookup_name, access), name_token));
	}

	for (const auto& [imported_module_name, symbol_table] : m_context.m_imported_symbols)
	{
		if (symbol_table.HasExport(lookup_name))
		{
			const ImportedSymbolAccess access = ResolveImportedSymbolAccess(imported_module_name, lookup_name);
			if (access == ImportedSymbolAccess::Accessible)
			{
				Token qualified_token = name_token;
				qualified_token.m_lexeme = imported_module_name + NameSeparator.data() + lookup_name;
				return std::make_unique<MidoriExpression>(MidoriExpression::NameAccess(qualified_token, MidoriExpression::NameContext::Global()));
			}
			else if (access == ImportedSymbolAccess::PrivateInaccessible)
			{
				return std::unexpected(GenerateParserError(BuildImportedSymbolAccessError(imported_module_name, lookup_name, access), name_token));
			}
		}
	}

	std::vector<std::string> matching_typeclasses;
	for (const MidoriType::ClassConstraint& constraint : m_state.m_active_constraints)
	{
		std::unordered_map<std::string, std::unordered_set<std::string>>::const_iterator tc_it = m_state.m_class_methods.find(constraint.m_class_name);
		if (tc_it != m_state.m_class_methods.cend() && tc_it->second.contains(lookup_name))
		{
			matching_typeclasses.emplace_back(constraint.m_class_name);
		}
	}

	if (!matching_typeclasses.empty())
	{
		if (matching_typeclasses.size() == 1u)
		{
			return std::unexpected
			(
				GenerateParserError
				(
					std::format("Unqualified class method '{}'. Use qualified syntax like '{}{}{}'.", lookup_name, matching_typeclasses[0], NameSeparator, lookup_name),
					name_token
				)
			);
		}

		std::string candidates;
		for (size_t i = 0u; i < matching_typeclasses.size(); i += 1u)
		{
			if (i > 0u)
			{
				candidates.append(", "s);
			}
			candidates.append(matching_typeclasses[i]).append(NameSeparator).append(lookup_name);
		}

		return std::unexpected
		(
			GenerateParserError
			(
				std::format("Unqualified class method '{}'. Use qualified syntax like one of: {}.", lookup_name, candidates),
				name_token
			)
		);
	}

	if (IsRemovedConcurrencyBuiltin(lookup_name))
	{
		static const std::unordered_map<std::string_view, std::string_view> replacements
		{
			{ "close", "Concurrency::Close" },
			{ "is_done", "Concurrency::IsDone" },
			{ "cancel", "Concurrency::Cancel" }
		};
		return std::unexpected(GenerateParserError(std::format("'{}' is no longer supported. Write '{}', which needs \"MarmotPrelude/Concurrency.mmt\" imported like the rest of the concurrency surface.", lookup_name, replacements.at(lookup_name)), name_token));
	}

	// `loop`, `break`, `continue` and `return` are no longer keywords, so a file
	// still using them lexes them as ordinary identifiers and would fail with a bare
	// "Undefined name." Name the removal instead. Reached only when the name does not
	// resolve, so a binding actually called `loop` is untouched.
	if (lookup_name == "return")
	{
		return std::unexpected(GenerateParserError("'return' is no longer supported. A function's value is its body's value, and a block's value is its last expression, so drop 'return'. To leave early, make the rest of the body an 'if'/'else' or a 'match' branch.", name_token));
	}
	if (lookup_name == "loop")
	{
		return std::unexpected(GenerateParserError("'loop' is no longer supported. Write a recursive function, or 'for x in iterable { ... }' to consume an iterable for its effects.", name_token));
	}
	if (lookup_name == "break")
	{
		return std::unexpected(GenerateParserError("'break' is no longer supported. A loop that stops early becomes a recursive function whose base case returns the answer, which also has to say what happens when nothing is found.", name_token));
	}
	if (lookup_name == "continue")
	{
		return std::unexpected(GenerateParserError("'continue' is no longer supported. Guard the body with 'if' instead.", name_token));
	}
	if (lookup_name == "spawn")
	{
		return std::unexpected(GenerateParserError("'spawn' is no longer supported. Write 'Concurrency::Spawn(argument, F)', passing several arguments as one tuple: 'spawn F(a, b)' becomes 'Concurrency::Spawn((a, b), F)'.", name_token));
	}
	if (lookup_name == "join")
	{
		return std::unexpected(GenerateParserError("'join' is no longer supported. Write 'Concurrency::Join(w)' or 'w |> Concurrency::Join'.", name_token));
	}
	if (lookup_name == "channel")
	{
		return std::unexpected(GenerateParserError("'channel' is no longer supported. Write 'Concurrency::MakeChannel(capacity)' where the element type is known, as in 'def ch : Channel<Int> = Concurrency::MakeChannel(4);'.", name_token));
	}

	return std::unexpected(GenerateParserError(CompilerErrorCode::TypeUndefinedName, "Undefined name.", name_token));
}

Parser::ConstructorResolutionResult Parser::ResolveConstructorName(const Token& name_token, const std::string& mangled_name)
{
	for (Scopes::const_reverse_iterator scopes_iter = m_state.m_scopes.crbegin(); scopes_iter != m_state.m_scopes.crend(); ++scopes_iter)
	{
		const Scope& scope = *scopes_iter;
		if (scope.m_union_constructors.contains(mangled_name))
		{
			return ConstructorResolution(std::shared_ptr<MidoriType>(scope.m_union_constructors.at(mangled_name)), std::string(mangled_name), false);
		}
		else if (scope.m_struct_constructors.contains(mangled_name))
		{
			return ConstructorResolution(std::shared_ptr<MidoriType>(scope.m_struct_constructors.at(mangled_name)), std::string(mangled_name), true);
		}
	}

	std::string lookup_base = mangled_name;
	size_t separator_pos = mangled_name.find(NameSeparator);
	if (separator_pos != std::string::npos)
	{
		lookup_base = mangled_name.substr(0u, separator_pos);
	}

	const UseImportResolution use_import_resolution = ResolveUseImport(lookup_base);
	if (use_import_resolution.m_status == UseImportResolutionStatus::Ambiguous)
	{
		return std::unexpected(GenerateParserError(BuildAmbiguousUseImportError(lookup_base, use_import_resolution.m_conflicting_modules), name_token));
	}

	if (use_import_resolution.m_status == UseImportResolutionStatus::Resolved)
	{
		const std::string& module_name = use_import_resolution.m_module_name;
		if (m_context.m_imported_type_signatures.contains(module_name))
		{
			const TypeEnvironment& env = m_context.m_imported_type_signatures.at(module_name);
			if (env.contains(lookup_base))
			{
				std::shared_ptr<MidoriType> type = env.at(lookup_base);
				if (type->IsType<MidoriType::UnionType>() && separator_pos != std::string::npos)
				{
					const MidoriType::UnionType& union_type = type->GetType<MidoriType::UnionType>();
					std::string member_key = union_type.m_name + NameSeparator.data() + mangled_name.substr(separator_pos + NameSeparator.length());
					if (union_type.m_member_info.contains(member_key))
					{
						return ConstructorResolution(std::move(type), std::move(member_key), false);
					}
				}
				else if (type->IsType<MidoriType::StructType>() && lookup_base == mangled_name)
				{
					return ConstructorResolution(std::move(type), std::string(mangled_name), true);
				}
			}
		}
	}

	if (separator_pos != std::string::npos)
	{
		for (const std::pair<const std::string, TypeEnvironment>& imported : m_context.m_imported_type_signatures)
		{
			const TypeEnvironment& env = imported.second;
			if (!env.contains(lookup_base))
			{
				continue;
			}

			std::shared_ptr<MidoriType> type = env.at(lookup_base);
			if (!type->IsType<MidoriType::UnionType>())
			{
				continue;
			}

			const MidoriType::UnionType& union_type = type->GetType<MidoriType::UnionType>();
			std::string member_key = union_type.m_name + NameSeparator.data() + mangled_name.substr(separator_pos + NameSeparator.length());
			if (union_type.m_member_info.contains(member_key))
			{
				return ConstructorResolution(std::move(type), std::move(member_key), false);
			}
		}
	}

	return std::optional<ConstructorResolution>(std::nullopt);
}

bool Parser::CanAccessSymbol(const std::string& symbol_name) const
{
	// No module system enabled, allow all access
	if (m_context.m_module_declarations == nullptr || m_context.m_current_module == nullptr)
	{
		return true;
	}

	// First check if symbol is defined in current scope (local or global)
	// Local symbols always take precedence over imported symbols
	std::string mangled_name = symbol_name;  // For namespace-qualified names
	std::vector<Scope>::const_reverse_iterator found_scope_it = std::ranges::find_if
	(
		m_state.m_scopes.rbegin(),
		m_state.m_scopes.rend(),
		[&mangled_name](const Scope& scope)
		{
			return scope.m_variables.contains(mangled_name) ||
				scope.m_struct_constructors.contains(mangled_name) ||
				scope.m_union_constructors.contains(mangled_name) ||
				scope.m_defined_names.contains(mangled_name);
		}
	);

	// If symbol is defined in current scope, always allow access
	if (found_scope_it != m_state.m_scopes.rend())
	{
		return true;
	}

	// For unqualified symbol access to external symbols, the symbol must either be:
	// 1. Explicitly imported via 'use' statement, OR
	// 2. Not exported by any module (i.e., foreign function)
	// Check if symbol was explicitly imported via 'use'
	const UseImportResolution use_import_resolution = ResolveUseImport(symbol_name);
	if (use_import_resolution.m_status == UseImportResolutionStatus::Resolved)
	{
		// Symbol is in use imports, verify it's actually exported by that module
		return ResolveQualifiedSymbol(use_import_resolution.m_module_name, symbol_name);
	}

	if (use_import_resolution.m_status == UseImportResolutionStatus::Ambiguous)
	{
		return true;
	}

	// Check if symbol is exported by ANY module
	bool found_in_any_export = false;
	for (const auto& [file_path, module_decl] : *m_context.m_module_declarations)
	{
		if (module_decl.HasExport(symbol_name))
		{
			found_in_any_export = true;
			break;
		}
	}

	// If symbol is exported by a module but NOT in use imports, deny access
	// (must use qualified name like Module.Symbol)
	if (found_in_any_export)
	{
		return false;
	}

	// Symbol not exported by any module - allow access (might be foreign function)
	return true;
}

bool Parser::IsInUseImports(const std::string& symbol_name, std::string& out_module_name) const
{
	const UseImportResolution resolution = ResolveUseImport(symbol_name);
	if (resolution.m_status == UseImportResolutionStatus::Resolved)
	{
		out_module_name = resolution.m_module_name;
		return true;
	}

	return false;
}

Parser::UseImportResolution Parser::ResolveUseImport(const std::string& symbol_name) const
{
	UseImportResolution resolution;

	for (const UseImport& use_import : m_state.m_current_use_imports)
	{
		if
		(
			use_import.m_symbol_name == symbol_name &&
			!std::ranges::contains(resolution.m_conflicting_modules, use_import.m_module_name)
		)
		{
			resolution.m_conflicting_modules.emplace_back(use_import.m_module_name);
		}
	}

	if (resolution.m_conflicting_modules.empty())
	{
		return resolution;
	}

	std::ranges::sort(resolution.m_conflicting_modules);
	if (resolution.m_conflicting_modules.size() == 1u)
	{
		resolution.m_status = UseImportResolutionStatus::Resolved;
		resolution.m_module_name = resolution.m_conflicting_modules[0u];
		return resolution;
	}

	resolution.m_status = UseImportResolutionStatus::Ambiguous;
	return resolution;
}

std::string Parser::BuildTypeArgumentCountMismatchMessage(const std::string& type_name, bool is_alias, size_t expected_count, size_t actual_count)
{
	if (!is_alias)
	{
		return std::format("Type argument count mismatch: expected {}, got {}", expected_count, actual_count);
	}

	return std::format("Type argument count mismatch for alias '{}': expected {}, got {}", type_name, expected_count, actual_count);
}

std::string Parser::BuildAmbiguousUseImportError(const std::string& symbol_name, const std::vector<std::string>& module_names) const
{
	std::string modules;
	for (size_t i = 0u; i < module_names.size(); i += 1u)
	{
		if (i > 0u)
		{
			modules.append(", ");
		}

		modules.append("'").append(module_names[i]).append("'");
	}

	const std::string qualified_example =
		module_names.empty()
		? "ModuleName"s + NameSeparator.data() + symbol_name
		: module_names[0u] + NameSeparator.data() + symbol_name;

	return std::format
	(
		"Ambiguous use import for symbol '{}': imported from modules {}. Use qualified access like '{}' or remove one of the conflicting use imports.",
		symbol_name,
		modules,
		qualified_example
	);
}

Parser::ImportedSymbolAccess Parser::ResolveImportedSymbolAccess(const std::string& module_name, const std::string& symbol_name) const
{
	const std::unordered_map<std::string, CompiledModule::SymbolTable>::const_iterator module_it = m_context.m_imported_symbols.find(module_name);
	if (module_it == m_context.m_imported_symbols.cend())
	{
		return ImportedSymbolAccess::ModuleNotFound;
	}

	const VisibilityLevel* visibility = module_it->second.FindExportVisibility(symbol_name);
	if (visibility == nullptr)
	{
		return ImportedSymbolAccess::SymbolNotExported;
	}

	if (*visibility == VisibilityLevel::Public)
	{
		return ImportedSymbolAccess::Accessible;
	}

	if (*visibility == VisibilityLevel::Private)
	{
		if (m_context.m_current_module != nullptr &&
			m_context.m_current_module->HasModuleDeclaration() &&
			SharesNamespace(m_context.m_current_module->ModuleName(), module_name))
		{
			return ImportedSymbolAccess::Accessible;
		}

		return ImportedSymbolAccess::PrivateInaccessible;
	}

	return ImportedSymbolAccess::SymbolNotExported;
}

std::string Parser::BuildImportedSymbolAccessError(const std::string& module_name, const std::string& symbol_name, ImportedSymbolAccess access) const
{
	switch (access)
	{
	case ImportedSymbolAccess::Accessible:
		return {};
	case ImportedSymbolAccess::ModuleNotFound:
		return std::format("Module '{}' not found.", module_name);
	case ImportedSymbolAccess::SymbolNotExported:
		return std::format("Symbol '{}' is not exported by module '{}'.", symbol_name, module_name);
	case ImportedSymbolAccess::PrivateInaccessible:
		if (m_context.m_current_module != nullptr && m_context.m_current_module->HasModuleDeclaration())
		{
			return std::format
			(
				"Symbol '{}' is private to module '{}' and is not accessible from current namespace '{}'.",
				symbol_name,
				module_name,
				TopLevelNamespace(m_context.m_current_module->ModuleName())
			);
		}

		return std::format("Symbol '{}' is private to module '{}' and is not accessible from the current module.", symbol_name, module_name);
	}

	return std::format("Symbol '{}' is not accessible from module '{}'.", symbol_name, module_name);
}

bool Parser::ResolveQualifiedSymbol(const std::string& module_name, const std::string& symbol_name) const
{
	return ResolveImportedSymbolAccess(module_name, symbol_name) == ImportedSymbolAccess::Accessible;
}

bool Parser::IsGlobalName(const std::vector<Scope>::const_reverse_iterator& found_scope_it) const
{
	return found_scope_it == std::prev(m_state.m_scopes.crend());
}

bool Parser::IsLocalName(const Scope::VariableTable::const_iterator& found_tbl_it) const
{
	return m_state.m_function_depth == 0 || found_tbl_it->second.m_function_depth == m_state.m_function_depth;
}

bool Parser::IsAtGlobalScope() const
{
	return m_state.m_scopes.size() == 1u;
}

CompilerError Parser::GenerateParserError(std::string&& message, const Token& token)
{
	return GenerateParserError(CompilerErrorCode::None, std::move(message), token);
}

CompilerError Parser::GenerateParserError(CompilerErrorCode code, std::string&& message, const Token& token)
{
	// If the token is from a different file, read that file's source lines
	if (token.m_file_name != m_context.m_file_name && !token.m_file_name.empty())
	{
		std::ifstream file(token.m_file_name);
		if (file.is_open())
		{
			std::vector<std::string> token_source_lines;
			std::string line;
			while (std::getline(file, line))
			{
				token_source_lines.emplace_back(std::move(line));
			}
			return MidoriError::GenerateParserErrorWithContext(code, std::move(message), token, token.m_file_name, token_source_lines);
		}
	}

	return MidoriError::GenerateParserErrorWithContext(code, std::move(message), token, token.m_file_name, *m_context.m_source_lines);
}

bool Parser::IsNoMatchError(const CompilerError& error) const
{
	return error.IsNoMatch();
}

bool Parser::IsAtEnd(ParseState& state)
{
	return Peek(state, 0).m_token_name == Token::Name::END_OF_FILE;
}

bool Parser::Check(ParseState& state, Token::Name type, int offset)
{
	return !IsAtEnd(state) && Peek(state, offset).m_token_name == type;
}

Token& Parser::Peek(ParseState& state, int offset)
{
	return state.m_current_token_index + offset < m_context.m_tokens.Size()
		? m_context.m_tokens[state.m_current_token_index + offset]
		: m_context.m_tokens[m_context.m_tokens.Size() - 1];
}

Token& Parser::Previous(ParseState& state)
{
	return m_context.m_tokens[static_cast<size_t>(state.m_current_token_index - 1)];
}

Token& Parser::Advance(ParseState& state)
{
	if (!IsAtEnd(state))
	{
		state.m_current_token_index += 1;
	}
	return Previous(state);
}

bool Parser::IsAtEnd()
{
	return IsAtEnd(m_state);
}

bool Parser::Check(Token::Name type, int offset)
{
	return Check(m_state, type, offset);
}

Token& Parser::Peek(int offset)
{
	return Peek(m_state, offset);
}

Token& Parser::Previous()
{
	return Previous(m_state);
}

std::vector<Parser::Scope>::const_reverse_iterator Parser::FindTypeScope(std::string& name)
{
	for (std::vector<Parser::Scope>::const_reverse_iterator it = m_state.m_scopes.crbegin(); it != m_state.m_scopes.crend(); ++it)
	{
		if (it->m_defined_types.find(name) != it->m_defined_types.end())
		{
			return it;
		}
	}

	std::string mangled_name;
	for (size_t end_idx : std::views::iota(0u, m_state.m_namespaces.size()))
	{
		std::string stacked_namespace;
		for (size_t idx : std::views::iota(0u, end_idx + 1u))
		{
			stacked_namespace.append(m_state.m_namespaces[idx]).append(NameSeparator);
		}
		mangled_name.append(stacked_namespace).append(name);

		for (std::vector<Parser::Scope>::const_reverse_iterator it = m_state.m_scopes.crbegin(); it != m_state.m_scopes.crend(); ++it)
		{
			if (it->m_defined_types.find(mangled_name) != it->m_defined_types.end())
			{
				name = std::move(mangled_name);
				return it;
			}
		}

		mangled_name.clear();
	}

	return m_state.m_scopes.crend();
}

std::vector<Parser::Scope>::const_reverse_iterator Parser::FindVariableScope(std::string& name)
{
	for (std::vector<Parser::Scope>::const_reverse_iterator it = m_state.m_scopes.crbegin(); it != m_state.m_scopes.crend(); ++it)
	{
		if (it->m_variables.find(name) != it->m_variables.end())
		{
			return it;
		}
	}

	std::string mangled_name;
	for (size_t end_idx : std::views::iota(0u, m_state.m_namespaces.size()))
	{
		std::string stacked_namespace;
		for (size_t idx : std::views::iota(0u, end_idx + 1u))
		{
			stacked_namespace.append(m_state.m_namespaces[idx]).append(NameSeparator);
		}
		mangled_name.append(stacked_namespace).append(name);

		for (std::vector<Parser::Scope>::const_reverse_iterator it = m_state.m_scopes.crbegin(); it != m_state.m_scopes.crend(); ++it)
		{
			if (it->m_variables.find(mangled_name) != it->m_variables.end())
			{
				name = std::move(mangled_name);
				return it;
			}
		}

		mangled_name.clear();
	}

	return m_state.m_scopes.crend();
}

Token& Parser::Advance()
{
	return Advance(m_state);
}

MidoriResult::TokenResult Parser::Consume(Token::Name type, std::string_view message)
{
	if (Check(type, 0))
	{
		return Advance();
	}
	else
	{
		return std::unexpected(GenerateParserError(std::string(message), Peek(0)));
	}
}

MidoriResult::TokenResult Parser::ConsumeTypeRightAngle(std::string_view message)
{
	if (Check(Token::Name::RIGHT_ANGLE, 0))
	{
		return Advance();
	}

	if (Check(Token::Name::RIGHT_SHIFT, 0))
	{
		const Token anchor = Peek(0);
		Token first_right_angle = MakeSyntheticToken(">", Token::Name::RIGHT_ANGLE, anchor);
		Token second_right_angle = MakeSyntheticToken(">", Token::Name::RIGHT_ANGLE, anchor);

		m_context.m_tokens[m_state.m_current_token_index] = std::move(first_right_angle);

		// The insert shifts later tokens, so the enclosing level closes on the second
		// '>'. TokenStream keeps each Token at a stable address precisely so that the
		// parse frames holding Token& across this call survive the insert.
		TokenStream split_tokens;
		split_tokens.AddToken(std::move(second_right_angle));
		m_context.m_tokens.Insert(m_context.m_tokens.begin() + m_state.m_current_token_index + 1, std::move(split_tokens));

		return Advance();
	}

	return std::unexpected(GenerateParserError(std::string(message), Peek(0)));
}

CompilerError Parser::GenerateRemovedReturnTypeColonError()
{
	return GenerateParserError("':' is no longer supported in return position. Write '-> Type' instead.", Peek(0));
}

MidoriResult::TokenResult Parser::ConsumeReturnTypeSeparator(std::string_view message)
{
	if (Check(Token::Name::THIN_ARROW, 0))
	{
		return Advance();
	}

	// ':' used to be accepted here as well. It now has the one job of ascribing a type
	// to a name, so name the removal rather than report a missing '->'. Every caller
	// reaches this immediately after a parameter list, where only '->', 'where' or '=>'
	// is legal, so a ':' at this point can only ever be the old return spelling - it can
	// never be the ascription in `def x : Int`, a record field, or a parameter, each of
	// which is consumed by a different site well before this one.
	if (Check(Token::Name::SINGLE_COLON, 0))
	{
		return std::unexpected(GenerateRemovedReturnTypeColonError());
	}

	return std::unexpected(GenerateParserError(std::string(message), Peek(0)));
}

Parser& Parser::BeginScope() &
{
	m_state.m_scopes.emplace_back();
	return *this;
}

Parser&& Parser::BeginScope() &&
{
	m_state.m_scopes.emplace_back();
	return std::move(*this);
}

int Parser::EndScope()
{
	const Scope& scope = m_state.m_scopes.back();
	int block_local_count = static_cast<int>(scope.m_variables.size());
	m_state.m_total_locals_in_curr_scope -= block_local_count;
	m_state.m_total_variables -= block_local_count;
	m_state.m_scopes.pop_back();
	return block_local_count;
}

std::string Parser::Mangle(std::string_view name)
{
	size_t sep_idx = name.find(NameSeparator);

	if (sep_idx != std::string::npos)
	{
		// Name already has a qualifier (e.g., "UnionName::Member")
		std::string_view top_qualifier = name.substr(0u, sep_idx);
		std::vector<std::string>::const_iterator find_result = std::find(m_state.m_namespaces.cbegin(), m_state.m_namespaces.cend(), top_qualifier);

		if (find_result != m_state.m_namespaces.cend())
		{
			// Found the qualifier in our stack, resolve to absolute path
			std::string mangled_name;

			// Prepend qualifiers up to (but not including) the found one
			for (std::vector<std::string>::const_iterator it = m_state.m_namespaces.cbegin(); it != find_result; ++it)
			{
				mangled_name.append(*it).append(NameSeparator);
			}

			// Append the original name (which already includes the found qualifier)
			mangled_name.append(name);
			return mangled_name;
		}
		else
		{
			// Qualifier not in our stack - return as-is (already absolute)
			return std::string(name);
		}
	}
	else
	{
		// No separator - prepend all current qualification contexts (e.g., union names)
		std::string mangled_name;
		for (const std::string& qualifier : m_state.m_namespaces)
		{
			mangled_name.append(qualifier).append(NameSeparator);
		}
		mangled_name.append(name);
		return mangled_name;
	}
}

MidoriResult::TokenResult Parser::DefineName(Token& name, bool is_variable)
{
	name.m_lexeme = Mangle(name.m_lexeme);

	if (m_state.m_scopes.back().m_defined_names.contains(name.m_lexeme))
	{
		return std::unexpected(GenerateParserError("Name already exists in the current scope", name));
	}

	m_state.m_scopes.back().m_defined_names.emplace(name.m_lexeme);
	if (is_variable)
	{
		m_state.m_scopes.back().m_variables.emplace(name.m_lexeme, VariableContext());
	}

	return name;
}

std::optional<int> Parser::RegisterOrUpdateLocalVariable(const std::string& name)
{
	std::optional<int> local_index = std::nullopt;

	if (!IsAtGlobalScope())
	{
		m_state.m_scopes.back().m_variables[name] = VariableContext(m_state.m_total_locals_in_curr_scope++, m_state.m_total_variables++, m_state.m_function_depth);
		local_index.emplace(m_state.m_scopes.back().m_variables[name].m_relative_index.value());
	}

	return local_index;
}

std::optional<int> Parser::RegisterHiddenLocal(const std::string&)
{
	int local_index = m_state.m_total_locals_in_curr_scope++;
	m_state.m_total_variables += 1;
	return local_index;
}

std::optional<int> Parser::BinaryOperatorClass(Token::Name name)
{
	switch (name)
	{
	case Token::Name::STAR:
	case Token::Name::SLASH:
	case Token::Name::PERCENT:
		return 0;
	case Token::Name::SINGLE_PLUS:
	case Token::Name::SINGLE_MINUS:
		return 1;
	case Token::Name::DOUBLE_PLUS:
		return 2;
	case Token::Name::LEFT_SHIFT:
	case Token::Name::RIGHT_SHIFT:
		return 3;
	case Token::Name::LEFT_ANGLE:
	case Token::Name::LESS_EQUAL:
	case Token::Name::RIGHT_ANGLE:
	case Token::Name::GREATER_EQUAL:
		return 4;
	case Token::Name::BANG_EQUAL:
	case Token::Name::DOUBLE_EQUAL:
		return 5;
	case Token::Name::SINGLE_AMPERSAND:
		return 6;
	case Token::Name::CARET:
		return 7;
	case Token::Name::SINGLE_BAR:
		return 8;
	case Token::Name::DOUBLE_AMPERSAND:
		return 9;
	case Token::Name::DOUBLE_BAR:
		return 10;
	default:
		return std::nullopt;
	}
}

std::optional<CompilerError> Parser::CheckOperatorMixing(const Token& op, const MidoriExpression& operand)
{
	// Two operators of different precedence in one expression bind by a table the
	// reader has to remember. Parentheses say it instead, so they are required:
	// `a + b * c` is written `a + (b * c)`. A chain of one operator, or of operators
	// that bind equally like `+` and `-`, needs none.
	if (!operand.IsExpression<MidoriExpression::Binary>())
	{
		return std::nullopt;
	}

	const std::optional<int> outer_class = BinaryOperatorClass(op.m_token_name);
	const Token& inner_op = operand.GetExpression<MidoriExpression::Binary>().m_op;
	const std::optional<int> inner_class = BinaryOperatorClass(inner_op.m_token_name);
	if (!outer_class.has_value() || !inner_class.has_value() || outer_class.value() == inner_class.value())
	{
		return std::nullopt;
	}

	return GenerateParserError(std::format("'{}' and '{}' do not bind equally, so mixing them needs parentheses: write the '{}' operand in its own parentheses.", op.m_lexeme, inner_op.m_lexeme, inner_op.m_lexeme), op);
}

MidoriResult::ExpressionResult Parser::ParseFactor()
{
	return ParseBinary(&Parser::ParseUnaryLogicalBitwise, Token::Name::STAR, Token::Name::SLASH, Token::Name::PERCENT);
}

MidoriResult::ExpressionResult Parser::ParseShift()
{
	return ParseBinary(&Parser::ParseTerm, Token::Name::LEFT_SHIFT, Token::Name::RIGHT_SHIFT);
}

MidoriResult::ExpressionResult Parser::ParseRange()
{
	return ParseShift()
		.and_then
		(
			[this](std::unique_ptr<MidoriExpression>&& start) -> MidoriResult::ExpressionResult
			{
				if (!Match(Token::Name::DOUBLE_DOT))
				{
					return start;
				}

				Token first_range_op = Previous();

				return ParseShift()
					.and_then
					(
						[this, &first_range_op, &start](std::unique_ptr<MidoriExpression>&& middle) -> MidoriResult::ExpressionResult
						{
							if (!Match(Token::Name::DOUBLE_DOT))
							{
								return std::unexpected(GenerateParserError("Expected '..' for step in range expression. Use 'start..step..end' syntax.", Peek(0)));
							}

							Token second_range_op = Previous();

							return ParseShift()
								.and_then
								(
									[&first_range_op, &second_range_op, &start, &middle](std::unique_ptr<MidoriExpression>&& end) -> MidoriResult::ExpressionResult
									{
										return std::make_unique<MidoriExpression>(MidoriExpression::RangeTernary(first_range_op, second_range_op, std::move(start), std::move(middle), std::move(end)));
									}
								);
						}
					);
			}
		);
}

MidoriResult::ExpressionResult Parser::ParseTerm()
{
	return ParseBinary(&Parser::ParseFactor, Token::Name::SINGLE_PLUS, Token::Name::DOUBLE_PLUS, Token::Name::SINGLE_MINUS);
}

MidoriResult::ExpressionResult Parser::ParseComparison()
{
	return ParseBinary(&Parser::ParseRange, Token::Name::LEFT_ANGLE, Token::Name::LESS_EQUAL, Token::Name::RIGHT_ANGLE, Token::Name::GREATER_EQUAL);
}

MidoriResult::ExpressionResult Parser::ParseEquality()
{
	return ParseBinary(&Parser::ParseComparison, Token::Name::BANG_EQUAL, Token::Name::DOUBLE_EQUAL);
}

MidoriResult::ExpressionResult Parser::ParseBitwiseAnd()
{
	return ParseBinary(&Parser::ParseEquality, Token::Name::SINGLE_AMPERSAND);
}

MidoriResult::ExpressionResult Parser::ParseBitwiseXor()
{
	return ParseBinary(&Parser::ParseBitwiseAnd, Token::Name::CARET);
}

MidoriResult::ExpressionResult Parser::ParseBitwiseOr()
{
	return ParseBinary(&Parser::ParseBitwiseXor, Token::Name::SINGLE_BAR);
}

// Assignment is not grammar. ParseBind survives only to reject the forms that
// used to parse here, each naming what replaces it.
MidoriResult::ExpressionResult Parser::ParseBind()
{
	return ParseLogicalOr()
		.and_then
		(
			[this](std::unique_ptr<MidoriExpression>&& left_expr) -> MidoriResult::ExpressionResult
			{
				if (Match(Token::Name::SINGLE_EQUAL))
				{
					return std::unexpected(GenerateParserError("Assignment is no longer supported. Bind the new value to a name with 'def', rebuild a struct with '{ s with field = value }', or an array with 'ArrayUtil::WithReplaced(a, i, v)'.", Previous()));
				}
				else if (Match(Token::Name::PLUS_EQUAL, Token::Name::MINUS_EQUAL, Token::Name::STAR_EQUAL, Token::Name::SLASH_EQUAL, Token::Name::PERCENT_EQUAL, Token::Name::AMPERSAND_EQUAL, Token::Name::BAR_EQUAL, Token::Name::CARET_EQUAL, Token::Name::LEFT_SHIFT_EQUAL, Token::Name::RIGHT_SHIFT_EQUAL))
				{
					const Token& op = Previous();
					return std::unexpected(GenerateParserError(std::format("Compound assignment '{}' is no longer supported. Compute the value and bind it with 'def'; a loop that accumulates becomes a comprehension or a recursive helper.", op.m_lexeme), op));
				}
				else if (Match(Token::Name::PLUS_PLUS_EQUAL))
				{
					return std::unexpected(GenerateParserError("Concatenation assignment syntax '++=' is no longer supported. Bind the concatenation with 'def', or use Appendable::Append / Extendable::Extend.", Previous()));
				}
				else if (Match(Token::Name::EQUAL_PLUS_PLUS))
				{
					return std::unexpected(GenerateParserError("Prepend assignment syntax '=++' is no longer supported. Bind the concatenation with 'def', or use Prependable::Prepend.", Previous()));
				}

				return left_expr;
			}
		);
}

MidoriResult::ExpressionResult Parser::ParseUnaryLogicalBitwise()
{
	if (Match(Token::Name::BANG, Token::Name::TILDE, Token::Name::HASH))
	{
		Token& op = Previous();
		return ParseUnaryLogicalBitwise()
			.and_then
			(
				[&op](std::unique_ptr<MidoriExpression>&& right) -> MidoriResult::ExpressionResult
				{
					return std::make_unique<MidoriExpression>(MidoriExpression::UnaryPrefix(op, std::move(right)));
				}
			);
	}

	return ParseUnaryArithmetic();
}

MidoriResult::ExpressionResult Parser::ParseUnaryArithmetic()
{
	if (Match(Token::Name::LEFT_ARROW, Token::Name::SINGLE_MINUS, Token::Name::SINGLE_PLUS))
	{
		Token& op = Previous();
		return ParseUnaryArithmetic()
			.and_then
			(
				[&op](std::unique_ptr<MidoriExpression>&& right) -> MidoriResult::ExpressionResult
				{
					if (op.m_token_name == Token::Name::LEFT_ARROW)
					{
						return std::make_unique<MidoriExpression>(MidoriExpression::Receive(op, std::move(right)));
					}

					return std::make_unique<MidoriExpression>(MidoriExpression::UnaryPrefix(op, std::move(right)));
				}
			);
	}
	else
	{
		return ParseConstruct();
	}
}

MidoriResult::ExpressionResult Parser::ParseExpression()
{
	return ParseAs();
}

MidoriResult::ExpressionResult Parser::ParseAs()
{
	return ParseBind()
		.and_then
		(
			[this](std::unique_ptr<MidoriExpression>&& expr) ->MidoriResult::ExpressionResult
			{
				while (Match(Token::Name::AS))
				{
					Token& as = Previous();
					MidoriResult::TypeResult type = ParseType();
					if (!type.has_value())
					{
						return std::unexpected(type.error());
					}

					expr = std::make_unique<MidoriExpression>(MidoriExpression::As(as, std::move(type.value()), std::move(expr)));
				}

				return expr;
			}
		);
}

MidoriResult::ExpressionResult Parser::ParseArrayAccessHelper(std::unique_ptr<MidoriExpression>&& arr_var)
{
	Token& op = Previous();

	return Consume(Token::Name::LEFT_BRACKET, "Expected '[' before index.")
		.and_then
		(
			[&op, &arr_var, this](Token&&) ->MidoriResult::ExpressionResult
			{
				return ParseBind()
					.and_then
					(
						[&op, &arr_var, this](std::unique_ptr<MidoriExpression>&& index) ->MidoriResult::ExpressionResult
						{
							return Consume(Token::Name::RIGHT_BRACKET, "Expected ']' after index.")
								.and_then
								(
									[&op, &arr_var, index = std::move(index)](Token&&) mutable ->MidoriResult::ExpressionResult
									{
										return std::make_unique<MidoriExpression>(MidoriExpression::IndexAccess(op, std::move(index), std::move(arr_var)));
									}
								);
						}
					);
			}
		);
}

MidoriResult::ExpressionResult Parser::ParseArrayAccess()
{
	return ParsePrimary()
		.and_then
		(
			[this](std::unique_ptr<MidoriExpression>&& arr_var) -> MidoriResult::ExpressionResult
			{
				// Check if we can do array access: next token is '['.
				bool has_bracket_for_access = Check(Token::Name::LEFT_BRACKET, 0);

				return has_bracket_for_access
					? ParseArrayAccessHelper(std::move(arr_var))
					: std::move(arr_var);
			}
		);
}

MidoriResult::ExpressionResult Parser::ParsePostfixChain(std::unique_ptr<MidoriExpression>&& expr)
{
	if (Match(Token::Name::LEFT_PAREN))
	{
		return FinishCall(std::move(expr))
			.and_then
			(
				[this](std::unique_ptr<MidoriExpression>&& called) -> MidoriResult::ExpressionResult
				{
					return ParsePostfixChain(std::move(called));
				}
			);
	}
	else if (Match(Token::Name::SINGLE_DOT))
	{
		return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected identifier after '.'.")
			.and_then
			(
				[&expr, this](Token&& name) -> MidoriResult::ExpressionResult
				{
					return ParsePostfixChain(std::make_unique<MidoriExpression>(MidoriExpression::MemberAccess(name, std::move(expr))));
				}
			);
	}
	else if (Check(Token::Name::LEFT_BRACKET, 0))
	{
		return ParseArrayAccessHelper(std::move(expr))
			.and_then
			(
				[this](std::unique_ptr<MidoriExpression>&& indexed) -> MidoriResult::ExpressionResult
				{
					return ParsePostfixChain(std::move(indexed));
				}
			);
	}
	else
	{
		return expr;
	}
}

MidoriResult::ExpressionResult Parser::ParseCall()
{
	return ParseArrayAccess()
		.and_then
		(
			[this](std::unique_ptr<MidoriExpression>&& expr)
			{
				return ParsePostfixChain(std::move(expr));
			}
		);
}

MidoriResult::ExpressionResult Parser::ParseConstruct()
{
	// `new` was the second spelling of a construction and is gone: `Point(1, 2)` is the only
	// form, and it is told from an ordinary call by the constructor lookup in ParseCall. The
	// word lexes as an ordinary identifier now, so name the removal here rather than let it
	// fall through to a bare "Undefined name.". Guarded on a following identifier, so a value
	// named `new` is left alone.
	if (Check(Token::Name::IDENTIFIER_LITERAL, 0) && Peek(0).m_lexeme == "new" && Check(Token::Name::IDENTIFIER_LITERAL, 1))
	{
		return std::unexpected(GenerateParserError("'new' is no longer supported. Write 'Name(args)' instead.", Peek(0)));
	}

	return ParseCall();
}

MidoriResult::ExpressionResult Parser::FinishCall(std::unique_ptr<MidoriExpression>&& callee)
{
	return ParseDelimitedZeroOrMoreLimited<std::unique_ptr<MidoriExpression>>
		(
			[this]() { return ParseExpression(); },
			[this]() { return Consume(Token::Name::COMMA, "Expected ',' after expression."); },
			[this]() { return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after arguments."); }
		)
		.and_then
		(
			[&callee, this](std::vector<std::unique_ptr<MidoriExpression>>&& arguments) ->MidoriResult::ExpressionResult
			{
				return LowerConcurrencyIntrinsic(std::make_unique<MidoriExpression>(MidoriExpression::Call(Previous(), std::move(callee), std::move(arguments))));
			}
		);
}

MidoriResult::ExpressionResult Parser::LowerConcurrencyIntrinsic(std::unique_ptr<MidoriExpression>&& expr)
{
	// A call keeps its Call node until it has every argument, because a pipe
	// prepends one later: `(ch, 5) |> Concurrency::Spawn(Producer)`. A call that
	// never gets the right count stays a Call, and the type checker reports it.
	if (!expr->IsExpression<MidoriExpression::Call>())
	{
		return std::move(expr);
	}

	MidoriExpression::Call& call = expr->GetExpression<MidoriExpression::Call>();
	if (!call.m_callee->IsExpression<MidoriExpression::NameAccess>())
	{
		return std::move(expr);
	}

	const Token& callee_name = call.m_callee->GetExpression<MidoriExpression::NameAccess>().m_name;
	if (callee_name.m_lexeme == SpawnIntrinsicName && call.m_arguments.size() == 2u)
	{
		std::vector<std::unique_ptr<MidoriExpression>> arguments;
		arguments.emplace_back(std::move(call.m_arguments[0u]));
		return std::make_unique<MidoriExpression>(MidoriExpression::Spawn(callee_name, std::move(call.m_arguments[1u]), std::move(arguments)));
	}

	if (callee_name.m_lexeme == JoinIntrinsicName && call.m_arguments.size() == 1u)
	{
		// Join evaluates to Result<T, WorkerError>, so both declarations must be
		// visible where it is written.
		std::shared_ptr<MidoriType> result_type = FindDeclaredTypeByName("Result");
		std::shared_ptr<MidoriType> worker_error_type = FindDeclaredTypeByName("WorkerError");
		if (result_type == nullptr || worker_error_type == nullptr)
		{
			return std::unexpected(GenerateParserError("'Concurrency::Join' evaluates to Result<T, WorkerError>. Import \"MarmotPrelude/Prelude/Result.mmt\" and \"MarmotPrelude/Concurrency.mmt\" to use it.", callee_name));
		}

		return std::make_unique<MidoriExpression>(MidoriExpression::Join(callee_name, std::move(call.m_arguments[0u]), std::move(result_type), std::move(worker_error_type)));
	}

	if (callee_name.m_lexeme == MakeChannelIntrinsicName && call.m_arguments.size() == 1u)
	{
		// The element type comes from the expected type during type checking.
		return std::make_unique<MidoriExpression>(MidoriExpression::ChannelCreate(callee_name, nullptr, std::move(call.m_arguments[0u])));
	}

	return std::move(expr);
}

MidoriResult::ExpressionResult Parser::FinishConstruct(Token&& constructor_token, std::shared_ptr<MidoriType>&& constructed_type, bool is_struct)
{
	MidoriResult::TokenResult left_paren = Consume(Token::Name::LEFT_PAREN, "Expected '(' after type.");
	if (!left_paren.has_value())
	{
		return std::unexpected(std::move(left_paren.error()));
	}

	MidoriResult::Result<std::vector<std::unique_ptr<MidoriExpression>>> arguments = ParseDelimitedZeroOrMoreLimited<std::unique_ptr<MidoriExpression>>
		(
			[this]() { return ParseExpression(); },
			[this]() { return Consume(Token::Name::COMMA, "Expected ',' after expression."); },
			[this]() { return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after arguments."); }
		);
	if (!arguments.has_value())
	{
		return std::unexpected(std::move(arguments.error()));
	}

	if (is_struct)
	{
		return std::make_unique<MidoriExpression>(MidoriExpression::Construct(constructor_token, std::move(arguments.value()), std::move(constructed_type), false, MidoriExpression::Construct::Struct{}));
	}

	const int tag = constructed_type->GetType<MidoriType::UnionType>().m_member_info.at(constructor_token.m_lexeme).m_tag;
	return std::make_unique<MidoriExpression>(MidoriExpression::Construct(constructor_token, std::move(arguments.value()), std::move(constructed_type), false, MidoriExpression::Construct::Union(tag)));
}

MidoriResult::ExpressionResult Parser::ParsePrimary()
{
	if (Match(Token::Name::LEFT_BRACE))
	{
		return ProbeRecordUpdate()
			? ParseRecordUpdate()
			: ParseBlockExpression();
	}
	else if (Match(Token::Name::LEFT_PAREN))
	{
		Token left_paren = Previous();

		if (Match(Token::Name::RIGHT_PAREN))
		{
			// Empty tuple is unit
			return std::make_unique<MidoriExpression>(MidoriExpression::Literal(Previous(), MidoriExpression::LiteralKind::Unit));
		}

		return ParseExpression()
			.and_then
			(
				[this, left_paren](std::unique_ptr<MidoriExpression>&& first_expr) -> MidoriResult::ExpressionResult
				{
					if (Match(Token::Name::COMMA))
					{
						// It's a tuple - parse remaining elements
						std::vector<std::unique_ptr<MidoriExpression>> elements;
						elements.push_back(std::move(first_expr));

						// Parse remaining tuple elements
						do
						{
							MidoriResult::ExpressionResult elem_result = ParseExpression();
							if (!elem_result)
							{
								return elem_result;
							}
							elements.push_back(std::move(elem_result.value()));
						} while (Match(Token::Name::COMMA));

						return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after tuple elements.")
							.and_then
							(
								[&elements, left_paren](Token&&) -> MidoriResult::ExpressionResult
								{
									return std::make_unique<MidoriExpression>(MidoriExpression::Tuple(left_paren, std::move(elements)));
								}
							);
					}
					else
					{
						return Consume(Token::Name::RIGHT_PAREN, "Expected right parentheses.")
							.and_then
							(
								[&first_expr](Token&&) -> MidoriResult::ExpressionResult
								{
									return std::make_unique<MidoriExpression>(MidoriExpression::Group(std::move(first_expr)));
								}
							);
					}
				}
			);
	}
	else if (Match(Token::Name::IDENTIFIER_LITERAL))
	{
		return MatchNameResolution()
			.and_then
			(
				[this](Token&& variable) -> MidoriResult::ExpressionResult
				{
					std::string mangled_name = Mangle(variable.m_lexeme);
					std::string symbol_name = ExtractSymbolName(variable.m_lexeme);
					std::string qualifier = ExtractQualifier(variable.m_lexeme);

					// Only check CanAccessSymbol for unqualified names
					// Qualified names (Module::Symbol) bypass this check and are validated below
					if (qualifier.empty() && !CanAccessSymbol(symbol_name))
					{
						std::string error_msg = "Symbol '"s + symbol_name + "' is not accessible"s;
						if (m_context.m_module_declarations != nullptr)
						{
							for (const auto& [file_path, module_decl] : *m_context.m_module_declarations)
							{
								if (module_decl.HasExport(symbol_name))
								{
									VisibilityLevel visibility = module_decl.GetExportVisibility(symbol_name);
									const std::string& module_name = module_decl.ModuleName();
									if (visibility == VisibilityLevel::Private)
									{
										error_msg += "\n  Note: '"s + symbol_name + "' is marked as 'private export' in module "s + module_name;
										error_msg += "\n  Note: Only modules in the "s + module_name.substr(0, module_name.find_last_of('.')) + " namespace can access it"s;
									}
									else if (visibility == VisibilityLevel::Internal)
									{
										error_msg += "\n  Note: '"s + symbol_name + "' is not exported from module "s + module_name;
										error_msg += "\n  Suggestion: Add it to a 'public export' or 'private export' block"s;
									}
									break;
								}
							}
						}

						const bool has_matching_export = (m_context.m_module_declarations != nullptr) && IsExportedInAnyModule(*m_context.m_module_declarations, symbol_name);
						error_msg += "\n  Hint: Use 'use "s + std::string(has_matching_export ? "ModuleName"s : ""s) + ".{"s + symbol_name + "}' to import it, or use qualified access like 'ModuleName"s + NameSeparator.data() + symbol_name + "'"s;
						return std::unexpected(GenerateParserError(std::move(error_msg), variable));
					}

					// A constructor may be written without 'new': `Point(1, 2)` and
					// `Option::Some(5)` build the same Construct node `new` builds. A name
					// bound to a variable still wins, so this only adds spellings that used
					// to be errors. The check precedes import resolution because a union
					// constructor is qualified by its union, not by a module, and import
					// resolution would report a missing module instead.
					std::string variable_lookup_name = mangled_name;
					if (FindVariableScope(variable_lookup_name) == m_state.m_scopes.crend())
					{
						ConstructorResolutionResult resolution = ResolveConstructorName(variable, mangled_name);
						if (!resolution.has_value())
						{
							return std::unexpected(std::move(resolution.error()));
						}

						if (resolution.value().has_value())
						{
							ConstructorResolution& constructor = resolution.value().value();
							Token constructor_token = variable;
							constructor_token.m_lexeme = constructor.m_constructor_name;

							if (!Check(Token::Name::LEFT_PAREN, 0))
							{
								return std::unexpected
								(
									GenerateParserError
									(
										std::format("Constructor '{}' cannot be used as a value; it is monomorphised at each construction site and has no single procedure to pass around. Write '{}(...)' to construct, or wrap it in a lambda.", constructor.m_constructor_name, constructor.m_constructor_name),
										constructor_token
									)
								);
							}

							return FinishConstruct(std::move(constructor_token), std::move(constructor.m_type), constructor.m_is_struct);
						}
					}

					// Check if this is a module-qualified name
					if (!qualifier.empty())
					{
						// Qualified class method call (e.g., Show::show)
						for (const MidoriType::ClassConstraint& constraint : m_state.m_active_constraints)
						{
							if (constraint.m_class_name != qualifier)
							{
								continue;
							}

							std::unordered_map<std::string, std::unordered_set<std::string>>::const_iterator tc_it = m_state.m_class_methods.find(constraint.m_class_name);
							if (tc_it != m_state.m_class_methods.cend() && tc_it->second.contains(symbol_name))
							{
								return std::make_unique<MidoriExpression>(MidoriExpression::NameAccess(variable, MidoriExpression::NameContext::Global()));
							}
						}

						std::unordered_map<std::string, std::unordered_set<std::string>>::const_iterator concrete_tc_it = m_state.m_class_methods.find(qualifier);
						if (concrete_tc_it != m_state.m_class_methods.cend() && concrete_tc_it->second.contains(symbol_name))
						{
							return std::make_unique<MidoriExpression>(MidoriExpression::NameAccess(variable, MidoriExpression::NameContext::Global()));
						}

						if (qualifier == ConcurrencyModuleName && IsConcurrencyIntrinsicName(variable.m_lexeme) && IsModuleVisible(ConcurrencyModuleName))
						{
							return std::make_unique<MidoriExpression>(MidoriExpression::NameAccess(variable, MidoriExpression::NameContext::Global()));
						}

						if (qualifier == CellModuleName && IsCellIntrinsicName(variable.m_lexeme) && IsModuleVisible(CellModuleName))
						{
							return std::make_unique<MidoriExpression>(MidoriExpression::NameAccess(variable, MidoriExpression::NameContext::Global()));
						}

						const ImportedSymbolAccess access = ResolveImportedSymbolAccess(qualifier, symbol_name);
						if (access == ImportedSymbolAccess::Accessible)
						{
							// Keep the fully qualified name so the code generator can identify imports
							return std::make_unique<MidoriExpression>(MidoriExpression::NameAccess(variable, MidoriExpression::NameContext::Global()));
						}

						return std::unexpected(GenerateParserError(BuildImportedSymbolAccessError(qualifier, symbol_name, access), variable));
					}

					return ResolveQualifiedName(variable, mangled_name);
				}
			);
	}
	else if (Match(Token::Name::FUNCTION))
	{
		return ParseFunctionExpression();
	}
	else if (Match(Token::Name::TRUE, Token::Name::FALSE))
	{
		return std::make_unique<MidoriExpression>(MidoriExpression::Literal(Previous(), MidoriExpression::LiteralKind::Bool));
	}
	else if (Match(Token::Name::FLOAT_LITERAL))
	{
		return std::make_unique<MidoriExpression>(MidoriExpression::Literal(Previous(), MidoriExpression::LiteralKind::Float));
	}
	else if (Match(Token::Name::INTEGER_LITERAL))
	{
		Token& token = Previous();
		const std::string& lexeme = token.m_lexeme;

		// Check if it's a hex or binary literal
		if (lexeme.size() >= 3 && lexeme[0u] == '0' && (lexeme[1u] == 'x' || lexeme[1u] == 'X' || lexeme[1u] == 'b' || lexeme[1u] == 'B'))
		{
			// A lexeme too large for 64 bits is classified as Word; code generation reports it.
			const uint64_t value = ParseUnsignedLiteral(lexeme).value_or(std::numeric_limits<uint64_t>::max());

			// Determine type based on value
			if (value <= 0xFF)
			{
				// Fits in Byte (0-255)
				return std::make_unique<MidoriExpression>(MidoriExpression::Literal(token, MidoriExpression::LiteralKind::Byte));
			}
			else if (value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
			{
				// Fits in signed Int
				return std::make_unique<MidoriExpression>(MidoriExpression::Literal(token, MidoriExpression::LiteralKind::Integer));
			}
			else
			{
				// Needs Word (unsigned 64-bit)
				return std::make_unique<MidoriExpression>(MidoriExpression::Literal(token, MidoriExpression::LiteralKind::Word));
			}
		}
		else
		{
			// Decimal literal - an Integer literal
			return std::make_unique<MidoriExpression>(MidoriExpression::Literal(token, MidoriExpression::LiteralKind::Integer));
		}
	}
	else if (Match(Token::Name::TEXT_LITERAL))
	{
		return std::make_unique<MidoriExpression>(MidoriExpression::Literal(Previous(), MidoriExpression::LiteralKind::Text));
	}
	else if (Match(Token::Name::LEFT_BRACKET))
	{
		Token& op = Previous();

		// Check for empty array first
		if (Match(Token::Name::RIGHT_BRACKET))
		{
			return std::make_unique<MidoriExpression>(MidoriExpression::Array(op, {}));
		}

		// Probe for comprehension intent before parsing the first element.
		// This lets malformed comprehensions route into the dedicated diagnostics path
		// without stealing arrays whose first element is itself a `for` expression.
		ArrayComprehensionProbe comprehension_probe = ProbeArrayComprehension();
		if (comprehension_probe.m_is_candidate)
		{
			return ParseArrayComprehension(op, comprehension_probe);
		}

		// Otherwise, parse as normal array literal
		return ParseExpression()
			.and_then
			(
				[&op, this](std::unique_ptr<MidoriExpression>&& first_expr) -> MidoriResult::ExpressionResult
				{
					if (Match(Token::Name::COMMA))
					{
						// Parse remaining elements
						return ParseDelimitedZeroOrMoreLimited<std::unique_ptr<MidoriExpression>>
							(
								[this]() { return ParseExpression(); },
								[this]() { return Consume(Token::Name::COMMA, "Expected ',' after expression."); },
								[this]() -> MidoriResult::TokenResult
								{
									if (Match(Token::Name::RIGHT_BRACKET))
									{
										return Previous();
									}
									else
									{
										return std::unexpected(GenerateParserError("Expected ']' for array expression.", Peek(0)));
									}
								}
							)
							.and_then
							(
								[&op, first_expr = std::move(first_expr)](std::vector<std::unique_ptr<MidoriExpression>>&& expressions) mutable -> MidoriResult::ExpressionResult
								{
									expressions.insert(expressions.begin(), std::move(first_expr));
									return std::make_unique<MidoriExpression>(MidoriExpression::Array(op, std::move(expressions)));
								}
							);
					}
					else if (Match(Token::Name::RIGHT_BRACKET))
					{
						// Single element array
						std::vector<std::unique_ptr<MidoriExpression>> expressions;
						expressions.emplace_back(std::move(first_expr));
						return std::make_unique<MidoriExpression>(MidoriExpression::Array(op, std::move(expressions)));
					}
					else
					{
						return std::unexpected(GenerateParserError("Expected ',' or ']' after array element.", Peek(0)));
					}
				}
			);
	}
	else if (Match(Token::Name::IF))
	{
		return ParseIfElseExpression();
	}
	else if (Match(Token::Name::MATCH))
	{
		return ParseMatchExpression();
	}
	else if (Match(Token::Name::FOR))
	{
		return ParseForExpression();
	}
	else
	{
		return std::unexpected(GenerateParserError("Expected expression.", Peek(0)));
	}
}

MidoriResult::ExpressionResult Parser::ParseLogicalAnd()
{
	return ParseBinary(&Parser::ParseSend, Token::Name::DOUBLE_AMPERSAND);
}

MidoriResult::ExpressionResult Parser::ParseLogicalOr()
{
	return ParseBinary(&Parser::ParseLogicalAnd, Token::Name::DOUBLE_BAR);
}

MidoriResult::ExpressionResult Parser::ParseSend()
{
	return ParsePipe()
		.and_then
		(
			[this](std::unique_ptr<MidoriExpression>&& left_expr) -> MidoriResult::ExpressionResult
			{
				while (Match(Token::Name::THIN_ARROW))
				{
					Token& arrow = Previous();
					MidoriResult::ExpressionResult right = ParsePipe();
					if (!right.has_value())
					{
						return std::unexpected(std::move(right.error()));
					}

					left_expr = std::make_unique<MidoriExpression>(MidoriExpression::Send(arrow, std::move(left_expr), std::move(right.value())));
				}

				return left_expr;
			}
		);
}

MidoriResult::ExpressionResult Parser::ParsePipe()
{
	return ParseBitwiseOr()
		.and_then
		(
			[this](std::unique_ptr<MidoriExpression>&& left_expr) -> MidoriResult::ExpressionResult
			{
				while (Match(Token::Name::BAR_BRACKET))
				{
					Token& pipe_op = Previous();

					if (Match(Token::Name::MATCH))
					{
						Token& match_keyword = Previous();
						MidoriResult::ExpressionResult right = ParseMatchExpressionWithScrutinee(match_keyword, std::move(left_expr));
						if (!right.has_value())
						{
							return std::unexpected(std::move(right.error()));
						}

						left_expr = std::move(right.value());
						continue;
					}

					MidoriResult::ExpressionResult right = ParseBitwiseOr();
					if (!right.has_value())
					{
						return std::unexpected(std::move(right.error()));
					}

					// Prepend the piped value into whichever argument list the right-hand side
					// already owns, and fall back to a call when it owns none.
					// x |> f(y) becomes f(x, y)
					// x |> Point(y) becomes Point(x, y)
					// x |> Concurrency::Spawn(F) becomes Concurrency::Spawn(x, F)
					// x |> f becomes f(x)
					// A construction is not a Call node, so it needs its own branch. A call to
					// Concurrency::Spawn, Join or MakeChannel becomes its own node only once the
					// pipe has supplied its last argument.
					// Prepending at parse time is also what lets the piped value take part in generic
					// inference on the same footing as a written argument.
					if (right.value()->IsExpression<MidoriExpression::Call>())
					{
						MidoriExpression::Call& call_expr = right.value()->GetExpression<MidoriExpression::Call>();
						call_expr.m_arguments.insert(call_expr.m_arguments.begin(), std::move(left_expr));
						MidoriResult::ExpressionResult lowered = LowerConcurrencyIntrinsic(std::move(right.value()));
						if (!lowered.has_value())
						{
							return std::unexpected(std::move(lowered.error()));
						}
						left_expr = std::move(lowered.value());
					}
					else if (right.value()->IsExpression<MidoriExpression::Join>()
						|| right.value()->IsExpression<MidoriExpression::ChannelCreate>()
						|| right.value()->IsExpression<MidoriExpression::Spawn>())
					{
						return std::unexpected(GenerateParserError("This call already has all of its arguments, so there is nothing for the pipe to fill. Pipe into it with one argument left out, as in 'w |> Concurrency::Join' or 'value |> Concurrency::Spawn(F)'.", pipe_op));
					}
					else if (right.value()->IsExpression<MidoriExpression::Construct>())
					{
						MidoriExpression::Construct& construct_expr = right.value()->GetExpression<MidoriExpression::Construct>();
						construct_expr.m_params.insert(construct_expr.m_params.begin(), std::move(left_expr));
						left_expr = std::move(right.value());
					}
					else
					{
						std::vector<std::unique_ptr<MidoriExpression>> arguments;
						arguments.emplace_back(std::move(left_expr));
						MidoriResult::ExpressionResult lowered = LowerConcurrencyIntrinsic(std::make_unique<MidoriExpression>(MidoriExpression::Call(pipe_op, std::move(right.value()), std::move(arguments))));
						if (!lowered.has_value())
						{
							return std::unexpected(std::move(lowered.error()));
						}
						left_expr = std::move(lowered.value());
					}
				}

				return left_expr;
			}
		);
}

std::shared_ptr<MidoriType> Parser::FindDeclaredTypeByName(const std::string& name)
{
	std::string lookup_name = name;
	std::vector<Scope>::const_reverse_iterator found_scope_it = FindTypeScope(lookup_name);
	if (found_scope_it != m_state.m_scopes.crend())
	{
		return found_scope_it->m_defined_types.at(lookup_name);
	}

	for (const std::pair<const std::string, TypeEnvironment>& imported_module : m_context.m_imported_type_signatures)
	{
		TypeEnvironment::const_iterator type_it = imported_module.second.find(name);
		if (type_it != imported_module.second.cend())
		{
			return type_it->second;
		}
	}

	return nullptr;
}

bool Parser::ProbeRecordUpdate()
{
	// Called with the opening '{' already consumed, so Peek(0) is the first token inside.
	//
	// '{' opens a block today. A record update `{ source with f = v }` is told apart by
	// scanning at nesting depth 0 for the first of ';' (block), '}' (block) or 'with'
	// (record update). The scan is bounded by the enclosing brace, in the same way
	// ProbeArrayComprehension is bounded by its bracket.
	//
	// The pending-match counter is load-bearing, not defensive padding: `{ match x with
	// case ... }` is a block whose 'with' sits at depth 0. Simulating this probe over the
	// 352 .mmt files in test/, MarmotPrelude/, benchmark/, reference_package/ and tests/
	// misclassifies 0 braces with the counter and 51 without it - most of
	// MarmotPrelude/Prelude/Result.mmt and Option.mmt among them.
	int offset = 0;
	int depth = 0;
	int pending_match = 0;

	while (true)
	{
		Token::Name current = Peek(offset).m_token_name;

		if (current == Token::Name::END_OF_FILE)
		{
			return false;
		}

		if (current == Token::Name::LEFT_PAREN || current == Token::Name::LEFT_BRACKET || current == Token::Name::LEFT_BRACE)
		{
			depth += 1;
		}
		else if (current == Token::Name::RIGHT_PAREN || current == Token::Name::RIGHT_BRACKET)
		{
			depth -= 1;
		}
		else if (current == Token::Name::RIGHT_BRACE)
		{
			if (depth == 0)
			{
				return false;
			}
			depth -= 1;
		}
		else if (depth == 0)
		{
			if (current == Token::Name::SINGLE_SEMICOLON)
			{
				return false;
			}
			else if (current == Token::Name::MATCH)
			{
				pending_match += 1;
			}
			else if (current == Token::Name::WITH)
			{
				if (pending_match == 0)
				{
					return true;
				}
				pending_match -= 1;
			}
		}

		offset += 1;

		// Safety limit to prevent infinite loop
		if (offset > MAX_ARRAY_SIZE)
		{
			return false;
		}
	}
}

MidoriResult::ExpressionResult Parser::ParseRecordUpdate()
{
	// Called with the opening '{' already consumed and ProbeRecordUpdate() having said
	// there is a 'with' at depth 0 ahead of any ';' or '}'.
	MidoriResult::ExpressionResult source_result = ParseExpression();
	if (!source_result.has_value())
	{
		return source_result;
	}

	MidoriResult::TokenResult with_result = Consume(Token::Name::WITH, "Expected 'with' in record update.");
	if (!with_result.has_value())
	{
		return std::unexpected(std::move(with_result.error()));
	}
	Token with_keyword = with_result.value();

	std::vector<MidoriExpression::RecordUpdate::FieldUpdate> updates;
	std::unordered_set<std::string> seen_fields;

	while (true)
	{
		MidoriResult::TokenResult name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected field name in record update.");
		if (!name_result.has_value())
		{
			return std::unexpected(std::move(name_result.error()));
		}
		Token field_name = name_result.value();

		// A duplicate field is an error, not last-one-wins.
		if (!seen_fields.emplace(field_name.m_lexeme).second)
		{
			return std::unexpected(GenerateParserError(std::format("Field '{}' is assigned more than once in this record update.", field_name.m_lexeme), field_name));
		}

		MidoriResult::TokenResult equal_result = Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after field name in record update.");
		if (!equal_result.has_value())
		{
			return std::unexpected(std::move(equal_result.error()));
		}

		MidoriResult::ExpressionResult value_result = ParseExpression();
		if (!value_result.has_value())
		{
			return value_result;
		}

		updates.emplace_back(field_name, std::move(value_result.value()));

		if (!Match(Token::Name::COMMA))
		{
			break;
		}

		// Allow a trailing comma before '}'.
		if (Check(Token::Name::RIGHT_BRACE, 0))
		{
			break;
		}
	}

	return Consume(Token::Name::RIGHT_BRACE, "Expected '}' after record update fields.")
		.and_then
		(
			[&with_keyword, &source_result, &updates](Token&&) -> MidoriResult::ExpressionResult
			{
				return std::make_unique<MidoriExpression>(MidoriExpression::RecordUpdate(with_keyword, std::move(source_result.value()), std::move(updates)));
			}
		);
}

MidoriResult::ExpressionResult Parser::ParseBlockExpression()
{
	std::function<MidoriResult::ExpressionResult(std::vector<std::unique_ptr<MidoriStatement>>&&, std::unique_ptr<MidoriExpression>&&)> build_block = [this](std::vector<std::unique_ptr<MidoriStatement>>&& stmts, std::unique_ptr<MidoriExpression>&& final_expr) -> MidoriResult::ExpressionResult
		{
			return Consume(Token::Name::RIGHT_BRACE, "Expected '}' after block expression.")
				.and_then
				(
					[&stmts, &final_expr, this](Token&& right_brace)
					{
						int block_local_count = EndScope();
						return final_expr != nullptr
							? MidoriResult::ExpressionResult(std::make_unique<MidoriExpression>(MidoriExpression::Block(right_brace, std::move(stmts), block_local_count, std::move(final_expr))))
							: MidoriResult::ExpressionResult(std::make_unique<MidoriExpression>(MidoriExpression::Block(right_brace, std::move(stmts), block_local_count)));
					}
				);
		};

	BeginScope();
	return ParseZeroOrMoreUnlimited<std::unique_ptr<MidoriStatement>>([this]() { return ParseDeclaration(); })
		.and_then
		(
			[&build_block, this](std::vector<std::unique_ptr<MidoriStatement>>&& stmts) ->MidoriResult::ExpressionResult
			{
				return TryParser<std::unique_ptr<MidoriExpression>>
					(
						[this]() -> MidoriResult::ExpressionResult
						{
							// Peek ahead: if next token is '}', do NOT try to parse an expression
							if (Check(Token::Name::RIGHT_BRACE, 0))
							{
								return std::unexpected(CompilerError::NoMatch());  // Signal: no expression expected
							}
							else
							{
								return ParseExpression();
							}
						}
					)
					.and_then
					(
						[&stmts, &build_block](std::unique_ptr<MidoriExpression>&& final_expr) -> MidoriResult::ExpressionResult
						{
							return build_block(std::move(stmts), std::move(final_expr));
						}
					)
					.or_else
					(
						[&stmts, &build_block](CompilerError&& err) -> MidoriResult::ExpressionResult
						{
							// Only fall back if this was an intentional absence
							if (err.IsNoMatch())
							{
								return build_block(std::move(stmts), nullptr);
							}

							return std::unexpected(std::move(err));
						}
					);
			}
		);
}

MidoriResult::ExpressionResult Parser::ParseForExpression()
{
	Token& for_keyword = Previous();

	if (!Match(Token::Name::IDENTIFIER_LITERAL))
	{
		return std::unexpected(GenerateParserError("Expected identifier after 'for'.", Peek(0)));
	}
	Token loop_variable = Previous();

	if (!Match(Token::Name::IN))
	{
		return std::unexpected(GenerateParserError("Expected 'in' after loop variable.", Peek(0)));
	}
	Token in_keyword = Previous();

	return ParseExpression()
		.and_then
		(
			[&for_keyword, &loop_variable, &in_keyword, this](std::unique_ptr<MidoriExpression>&& range)->MidoriResult::ExpressionResult
			{
				static int s_for_counter = 0;
				BeginScope();

				// Add loop variable to scope. Every index here is the slot within this
				// function's frame, which is what the code generator emits: the running
				// m_total_variables count is absolute across nested functions, so using it
				// addressed slots past the frame once a loop sat inside a nested lambda.
				std::string var_name(loop_variable.m_lexeme);
				std::optional<int> local_index = RegisterOrUpdateLocalVariable(var_name);
				int var_index = local_index.value_or(m_state.m_total_variables - 1);

				// Reserve additional local variable slots for hidden loop state values
				// These are not actual variables that can be referenced by name, but they need
				// to occupy local variable slots to prevent conflicts with body variables
				// For range iteration: step and end
				// For array iteration: current index, length, and array reference
				// Names use '$' prefix which is not valid in user identifiers
				int hidden_step_index = RegisterOrUpdateLocalVariable(std::string(FOR_STEP_PREFIX) + std::to_string(s_for_counter)).value_or(m_state.m_total_variables - 1);

				int hidden_end_index = RegisterOrUpdateLocalVariable(std::string(FOR_END_PREFIX) + std::to_string(s_for_counter)).value_or(m_state.m_total_variables - 1);

				int hidden_array_index = RegisterOrUpdateLocalVariable(std::string(FOR_ARRAY_PREFIX) + std::to_string(s_for_counter)).value_or(m_state.m_total_variables - 1);
				s_for_counter += 1;

				return ParseExpression()
					.and_then
					(
						[&for_keyword, &loop_variable, &in_keyword, range = std::move(range), var_index, hidden_step_index, hidden_end_index, hidden_array_index, this](std::unique_ptr<MidoriExpression>&& body) mutable ->MidoriResult::ExpressionResult
						{
							EndScope();

							std::unique_ptr<MidoriExpression> for_expr = std::make_unique<MidoriExpression>(MidoriExpression::For(for_keyword, loop_variable, in_keyword, std::move(range), std::move(body)));
							MidoriExpression::For& for_expr_ref = for_expr->GetExpression<MidoriExpression::For>();
							for_expr_ref.m_loop_variable_index = var_index;
							for_expr_ref.m_hidden_step_index = hidden_step_index;
							for_expr_ref.m_hidden_end_index = hidden_end_index;
							for_expr_ref.m_hidden_array_index = hidden_array_index;
							return for_expr;
						}
					);
			}
		);
}

Parser::ArrayComprehensionProbe Parser::ProbeArrayComprehension()
{
	// Look ahead for a likely comprehension boundary at bracket depth 0.
	// Returning a candidate here means the array should be parsed through the
	// comprehension path so syntax errors stay comprehension-specific.

	int offset = 0;
	int bracket_depth = 0;
	int paren_depth = 0;
	int brace_depth = 0;

	while (true)
	{
		Token::Name current = Peek(offset).m_token_name;
		if (current == Token::Name::END_OF_FILE)
		{
			return {};
		}

		const bool at_top_level = bracket_depth == 0 && paren_depth == 0 && brace_depth == 0;

		// Track nesting
		if (current == Token::Name::LEFT_BRACKET)
		{
			bracket_depth += 1;
		}
		else if (current == Token::Name::RIGHT_BRACKET)
		{
			if (at_top_level)
			{
				return {};
			}
			bracket_depth -= 1;
		}
		else if (current == Token::Name::LEFT_PAREN)
		{
			paren_depth += 1;
		}
		else if (current == Token::Name::RIGHT_PAREN)
		{
			paren_depth -= 1;
		}
		else if (current == Token::Name::LEFT_BRACE)
		{
			brace_depth += 1;
		}
		else if (current == Token::Name::RIGHT_BRACE)
		{
			brace_depth -= 1;
		}
		else if (current == Token::Name::COMMA && at_top_level)
		{
			return {};
		}
		else if (current == Token::Name::FOR && at_top_level)
		{
			// `[for ...]` is an array whose first element is a for-expression, not a comprehension.
			if (offset == 0)
			{
				return {};
			}

			ArrayComprehensionProbe probe;
			probe.m_is_candidate = true;
			if (Peek(offset + 1).m_token_name == Token::Name::IDENTIFIER_LITERAL)
			{
				probe.m_loop_variable_offset = offset + 1;
			}
			return probe;
		}
		else if (current == Token::Name::IDENTIFIER_LITERAL && at_top_level && offset > 0 && Peek(offset + 1).m_token_name == Token::Name::IN)
		{
			return ArrayComprehensionProbe{ true, offset };
		}

		offset += 1;

		// Safety limit to prevent infinite loop
		if (offset > MAX_ARRAY_SIZE)
		{
			return {};
		}
	}
}

MidoriResult::ExpressionResult Parser::ParseArrayComprehension(Token& bracket, const ArrayComprehensionProbe& probe)
{
	static int s_comp_counter = 0;
	bool scope_open = false;
	auto close_scope = [this, &scope_open]()
		{
			if (scope_open)
			{
				EndScope();
				scope_open = false;
			}
		};

	int var_index = -1;
	int hidden_step_index = -1;
	int hidden_end_index = -1;
	int hidden_array_index = -1;
	int result_array_index = -1;

	if (probe.m_loop_variable_offset.has_value())
	{
		Token loop_variable = Peek(probe.m_loop_variable_offset.value());
		BeginScope();
		scope_open = true;

		// Register the loop variable before parsing the transform expression so
		// comprehensions like `[i for i in range]` resolve `i` correctly. Each index is
		// the slot within this function's frame; m_total_variables counts across nested
		// functions, so using it addressed slots past the frame from inside a lambda.
		var_index = RegisterOrUpdateLocalVariable(std::string(loop_variable.m_lexeme)).value_or(m_state.m_total_variables - 1);

		hidden_step_index = RegisterOrUpdateLocalVariable(std::string(FOR_STEP_PREFIX) + std::to_string(s_comp_counter)).value_or(m_state.m_total_variables - 1);

		hidden_end_index = RegisterOrUpdateLocalVariable(std::string(FOR_END_PREFIX) + std::to_string(s_comp_counter)).value_or(m_state.m_total_variables - 1);

		hidden_array_index = RegisterOrUpdateLocalVariable(std::string(FOR_ARRAY_PREFIX) + std::to_string(s_comp_counter)).value_or(m_state.m_total_variables - 1);

		result_array_index = RegisterOrUpdateLocalVariable(std::string(COMPREHENSION_RESULT_PREFIX) + std::to_string(s_comp_counter)).value_or(m_state.m_total_variables - 1);

		s_comp_counter += 1;
	}

	return ParseExpression()
		.and_then
		(
			[&bracket, &close_scope, var_index, hidden_step_index, hidden_end_index, hidden_array_index, result_array_index, this](std::unique_ptr<MidoriExpression>&& transform_expr) -> MidoriResult::ExpressionResult
			{
				if (!Match(Token::Name::FOR))
				{
					close_scope();
					return std::unexpected(GenerateParserError("Expected 'for' in array comprehension. Use '[expr for item in range]' syntax.", Peek(0)));
				}

				if (!Match(Token::Name::IDENTIFIER_LITERAL))
				{
					close_scope();
					return std::unexpected(GenerateParserError("Expected identifier after 'for' in array comprehension.", Peek(0)));
				}
				Token actual_loop_var = Previous();

				if (!Match(Token::Name::IN))
				{
					close_scope();
					return std::unexpected(GenerateParserError("Expected 'in' after loop variable in array comprehension. Use '[expr for item in range]' syntax.", Peek(0)));
				}
				Token in_keyword = Previous();

				return ParseExpression()
					.and_then
					(
						[&bracket, &actual_loop_var, &in_keyword, &close_scope, var_index, hidden_step_index, hidden_end_index, hidden_array_index, result_array_index, transform_expr = std::move(transform_expr), this](std::unique_ptr<MidoriExpression>&& range) mutable -> MidoriResult::ExpressionResult
						{
							if (!Match(Token::Name::RIGHT_BRACKET))
							{
								close_scope();
								return std::unexpected(GenerateParserError("Expected ']' after array comprehension.", Peek(0)));
							}

							close_scope();
							if (var_index < 0 || hidden_step_index < 0 || hidden_end_index < 0 || hidden_array_index < 0 || result_array_index < 0)
							{
								return std::unexpected(GenerateParserError("Internal error: array comprehension loop binding was not initialized.", actual_loop_var));
							}

							std::unique_ptr<MidoriExpression> comp_expr = std::make_unique<MidoriExpression>(MidoriExpression::ArrayComprehension(bracket, actual_loop_var, in_keyword, std::move(transform_expr), std::move(range)));

							MidoriExpression::ArrayComprehension& comp_ref = comp_expr->GetExpression<MidoriExpression::ArrayComprehension>();
							comp_ref.m_loop_variable_index = var_index;
							comp_ref.m_hidden_step_index = hidden_step_index;
							comp_ref.m_hidden_end_index = hidden_end_index;
							comp_ref.m_hidden_array_index = hidden_array_index;
							comp_ref.m_result_array_index = result_array_index;

							return comp_expr;
						}
					)
					.or_else
					(
						[&close_scope](CompilerError&& error) -> MidoriResult::ExpressionResult
						{
							close_scope();
							return std::unexpected(std::move(error));
						}
					);
			}
		)
		.or_else
		(
			[&close_scope](CompilerError&& error) -> MidoriResult::ExpressionResult
			{
				close_scope();
				return std::unexpected(std::move(error));
			}
		);
}

MidoriResult::StatementResult Parser::ParseDefineStatement()
{
	if (Match(Token::Name::LEFT_PAREN))
	{
		std::vector<Token> names;
		std::vector<std::optional<int>> local_indices;

		do
		{
			MidoriResult::TokenResult name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected identifier in tuple pattern.");
			if (!name_result)
			{
				return std::unexpected(name_result.error());
			}

			Token var_name = std::move(name_result.value());
			constexpr bool is_variable = true;

			MidoriResult::TokenResult defined_name_result = DefineName(var_name, is_variable);
			if (!defined_name_result)
			{
				return std::unexpected(defined_name_result.error());
			}

			Token defined_name = std::move(defined_name_result.value());
			std::optional<int> local_index = RegisterOrUpdateLocalVariable(defined_name.m_lexeme);

			names.emplace_back(std::move(defined_name));
			local_indices.emplace_back(std::move(local_index));

		} while (Match(Token::Name::COMMA));

		return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after tuple pattern.")
			.and_then
			(
				[&names, &local_indices, this](Token&&) -> MidoriResult::StatementResult
				{
					return Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after tuple pattern.")
						.and_then
						(
							[&names, &local_indices, this](Token&&) -> MidoriResult::StatementResult
							{
								return ParseExpression()
									.and_then
									(
										[&names, &local_indices, this](std::unique_ptr<MidoriExpression>&& expr) -> MidoriResult::StatementResult
										{
											return Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after tuple binding.")
												.and_then
												(
													[&names, &local_indices, &expr](Token&&) -> MidoriResult::StatementResult
													{
														return std::make_unique<MidoriStatement>(MidoriStatement::TupleDefinition(std::move(names), std::move(expr), std::move(local_indices)));
													}
												);
										}
									);
							}
						);
				}
			);
	}

	// Single variable: def x = ...
	return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected name.")
		.and_then
		(
			[this](Token&& var_name) -> MidoriResult::StatementResult
			{
				constexpr bool is_variable = true;
				return DefineName(var_name, is_variable)
					.and_then
					(
						[this](Token&& define_name) -> MidoriResult::StatementResult
						{
							std::function<MidoriResult::StatementResult(std::optional<std::shared_ptr<MidoriType>>)> def_aux_func = [&define_name, this](std::optional<std::shared_ptr<MidoriType>> type_annotation)
								{
									std::optional<int> local_index = RegisterOrUpdateLocalVariable(define_name.m_lexeme);

									return Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after defining a name.")
										.and_then
										(
											[&define_name, &type_annotation, &local_index, this](Token&&) -> MidoriResult::StatementResult
											{
												return ParseExpression()
													.and_then
													(
														[&define_name, &type_annotation, &local_index, this](std::unique_ptr<MidoriExpression>&& expr) -> MidoriResult::StatementResult
														{
															return Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after name binding.")
																.and_then
																(
																	[&define_name, &expr, &type_annotation, &local_index](Token&&) -> MidoriResult::StatementResult
																	{
																		return std::make_unique<MidoriStatement>(MidoriStatement::VariableDefinition(define_name, std::move(expr), std::move(type_annotation), std::move(local_index)));
																	}
																);
														}
													);
											}
										);
								};

							if (Match(Token::Name::SINGLE_COLON))
							{
								return ParseType()
									.and_then
									(
										[&def_aux_func](std::shared_ptr<MidoriType>&& type)->MidoriResult::StatementResult
										{
											return def_aux_func(std::move(type));
										}
									);
							}
							else
							{
								return def_aux_func(std::nullopt);
							}
						}
					);
			}
		);
}

std::expected<Parser::TypeDeclarationHeader, CompilerError> Parser::ParseTypeDeclarationHeader(std::string_view noun, std::string_view capitalized_noun)
{
	MidoriResult::TokenResult name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected "s + std::string(noun) + " name."s);
	if (!name_result.has_value())
	{
		return std::unexpected(name_result.error());
	}

	Token declaration_name = std::move(name_result.value());
	std::string name_before_mangle = declaration_name.m_lexeme;
	declaration_name.m_lexeme = Mangle(declaration_name.m_lexeme);

	if (declaration_name.m_lexeme[0u] != std::toupper(declaration_name.m_lexeme[0u]))
	{
		return std::unexpected(GenerateParserError(std::string(capitalized_noun) + " name must start with a capital letter."s, declaration_name));
	}

	constexpr bool is_variable = false;
	MidoriResult::TokenResult defined_name_result = DefineName(declaration_name, is_variable);
	if (!defined_name_result.has_value())
	{
		return std::unexpected(defined_name_result.error());
	}

	TypeDeclarationHeader header(std::move(defined_name_result.value()), std::move(name_before_mangle));

	// Generic parameters open a scope before they are parsed, so that DefineName()
	// inside ParseGenericParameters() adds them to it. The body parsers close it.
	if (Match(Token::Name::LEFT_ANGLE))
	{
		header.m_has_generic_params = true;
		BeginScope();

		MidoriResult::TokenListResult generic_parse_result = ParseGenericParameters(&header.m_generic_param_types);
		if (!generic_parse_result.has_value())
		{
			EndScope();
			return std::unexpected(generic_parse_result.error());
		}

		header.m_generic_params = std::move(generic_parse_result.value());
	}

	if (Match(Token::Name::WHERE))
	{
		if (!header.m_has_generic_params)
		{
			return std::unexpected(GenerateParserError(std::string(capitalized_noun) + " constraints require at least one type parameter."s, header.m_name));
		}

		std::expected<std::vector<MidoriType::ClassConstraint>, CompilerError> constraints_result = ParseClassConstraints(header.m_name);
		if (!constraints_result.has_value())
		{
			return std::unexpected(constraints_result.error());
		}

		header.m_constraints = std::move(constraints_result.value());
	}

	return header;
}

MidoriResult::StatementResult Parser::ParseStructBody(TypeDeclarationHeader&& header)
{
	MidoriResult::TokenResult brace_result = Consume(Token::Name::LEFT_BRACE, "Expected '{' before struct body.");
	if (!brace_result.has_value())
	{
		return std::unexpected(brace_result.error());
	}

	std::expected<std::vector<StructMemberTuple>, CompilerError> members_result = ParseDelimitedZeroOrMoreLimited<StructMemberTuple>
	(
		[this]() -> std::expected<StructMemberTuple, CompilerError>
		{
			MidoriResult::TokenResult member_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected struct member name.");
			if (!member_name_result.has_value())
			{
				return std::unexpected(member_name_result.error());
			}

			Token member_name = std::move(member_name_result.value());

			MidoriResult::TokenResult colon_result = Consume(Token::Name::SINGLE_COLON, "Expected ':' before struct member type token.");
			if (!colon_result.has_value())
			{
				return std::unexpected(colon_result.error());
			}

			MidoriResult::TypeResult member_type_result = ParseType();
			if (!member_type_result.has_value())
			{
				return std::unexpected(member_type_result.error());
			}

			return std::make_tuple(std::move(member_type_result.value()), member_name.m_lexeme);
		},
		[this]() { return Consume(Token::Name::COMMA, "Expected ',' struct member."); },
		[this]() { return Consume(Token::Name::RIGHT_BRACE, "Expected '}' struct members."); }
	);
	if (!members_result.has_value())
	{
		return std::unexpected(members_result.error());
	}

	std::vector<Token> deriving_targets;
	if (Match(Token::Name::DERIVING))
	{
		std::expected<std::vector<Token>, CompilerError> deriving_result = ParseDerivingTargets(header.m_name);
		if (!deriving_result.has_value())
		{
			return std::unexpected(std::move(deriving_result.error()));
		}

		deriving_targets = std::move(deriving_result.value());
	}

	MidoriResult::TokenResult semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after struct body.");
	if (!semicolon_result.has_value())
	{
		return std::unexpected(semicolon_result.error());
	}

	StructMemberSplit member_split = SplitStructMemberTuples(std::move(members_result.value()));

	std::vector<std::string> generic_param_names;
	std::ranges::transform(header.m_generic_params, std::back_inserter(generic_param_names), [](const Token& tok) { return tok.m_lexeme; });

	std::shared_ptr<MidoriType> struct_type = MidoriType::MakeStructType(header.m_name.m_lexeme, std::move(member_split.m_types), std::move(member_split.m_names), std::move(generic_param_names));
	struct_type->GetType<MidoriType::StructType>().m_constraints = header.m_constraints;

	// End the generic param scope if it was created
	if (header.m_has_generic_params)
	{
		EndScope();
	}

	m_state.m_scopes.back().m_struct_constructors[header.m_name.m_lexeme] = struct_type;
	m_state.m_scopes.back().m_defined_types[header.m_name.m_lexeme] = struct_type;

	MidoriStatement::Struct struct_stmt(header.m_name, std::vector<Token>(header.m_generic_params), std::vector<MidoriType::ClassConstraint>(header.m_constraints), std::shared_ptr<MidoriType>(struct_type));
	if (!deriving_targets.empty())
	{
		std::expected<void, CompilerError> derive_result = QueueDerivedStructStatements(struct_stmt, deriving_targets);
		if (!derive_result.has_value())
		{
			return std::unexpected(std::move(derive_result.error()));
		}
	}

	return std::make_unique<MidoriStatement>(MidoriStatement::Struct(std::move(header.m_name), std::move(header.m_generic_params), std::move(header.m_constraints), std::move(struct_type)));
}

MidoriResult::StatementResult Parser::ParseUnionBody(TypeDeclarationHeader&& header)
{
	// The caller has already consumed the '=': ParseTypeDeclaration takes it before
	// dispatching on the shape of the body that follows.
	std::vector<std::string> generic_param_names;
	std::ranges::transform(header.m_generic_params, std::back_inserter(generic_param_names), [](const Token& tok) { return tok.m_lexeme; });

	std::shared_ptr<MidoriType> union_type = MidoriType::MakeUnionType(header.m_name.m_lexeme, std::move(generic_param_names));
	MidoriType::UnionType& union_type_ref = union_type->GetType<MidoriType::UnionType>();
	union_type_ref.m_constraints = header.m_constraints;

	// Registered before the body is parsed so that a variant can name the union it
	// belongs to, as `type List = Nil | Cons(Int, List)` does.
	size_t type_scope_idx = header.m_has_generic_params ? m_state.m_scopes.size() - 2uz : m_state.m_scopes.size() - 1uz;
	m_state.m_scopes[type_scope_idx].m_defined_types[header.m_name.m_lexeme] = union_type;
	m_state.m_namespaces.emplace_back(header.m_name_before_mangle);

	struct ActiveUnionScope
	{
		std::vector<std::shared_ptr<MidoriType>>& m_stack;

		ActiveUnionScope(std::vector<std::shared_ptr<MidoriType>>& stack, const std::shared_ptr<MidoriType>& type)
			: m_stack(stack)
		{
			m_stack.push_back(type);
		}

		~ActiveUnionScope()
		{
			m_stack.pop_back();
		}

		ActiveUnionScope(const ActiveUnionScope&) = delete;
		ActiveUnionScope& operator=(const ActiveUnionScope&) = delete;
	};

	std::vector<Token> constructor_names;
	int tag = 0;

	std::expected<std::vector<UnionMemberTuple>, CompilerError> members_result = [&constructor_names, &tag, &union_type, this]()
	{
		ActiveUnionScope scope(m_state.m_active_union_types, union_type);

		return ParseDelimitedZeroOrMoreUnlimited<UnionMemberTuple>
		(
			[&constructor_names, &tag, this]() -> std::expected<UnionMemberTuple, CompilerError>
			{
				MidoriResult::TokenResult member_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected union member name.");
				if (!member_name_result.has_value())
				{
					return std::unexpected(member_name_result.error());
				}

				Token member_name = std::move(member_name_result.value());
				member_name.m_lexeme = Mangle(member_name.m_lexeme);

				constexpr bool is_variable = false;
				MidoriResult::TokenResult defined_member_result = DefineName(member_name, is_variable);
				if (!defined_member_result.has_value())
				{
					return std::unexpected(defined_member_result.error());
				}

				member_name = std::move(defined_member_result.value());
				constructor_names.push_back(member_name);

				std::vector<std::shared_ptr<MidoriType>> member_types;
				if (Match(Token::Name::LEFT_PAREN))
				{
					MidoriResult::TypeListResult member_types_result = ParseDelimitedZeroOrMoreLimited<std::shared_ptr<MidoriType>>
					(
						[this]() { return ParseType(); },
						[this]() { return Consume(Token::Name::COMMA, "Expected ',' after type."); },
						[this]() { return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after union constructor."); }
					);
					if (!member_types_result.has_value())
					{
						return std::unexpected(member_types_result.error());
					}

					member_types = std::move(member_types_result.value());
				}

				UnionMemberTuple member = std::make_tuple(member_name.m_lexeme, std::move(member_types), tag);
				tag += 1;
				return member;
			},
			[this]() { return Consume(Token::Name::SINGLE_BAR, "Expected '|' after a union member."); }
		);
	}();

	if (!members_result.has_value())
	{
		return std::unexpected(members_result.error());
	}

	union_type_ref.m_member_info = BuildUnionMemberInfo(std::move(members_result.value()));

	// Store constructors in the parent scope (where the union is declared)
	// If we have generic params, we're one scope level deeper, so go back one
	Scope& constructor_scope = header.m_has_generic_params
		? m_state.m_scopes[m_state.m_scopes.size() - 2]
		: m_state.m_scopes.back();

	for (const std::pair<const std::string, MidoriType::UnionType::UnionMemberContext>& member_info_entry : union_type_ref.m_member_info)
	{
		constructor_scope.m_union_constructors[member_info_entry.first] = union_type;
	}

	std::vector<Token> deriving_targets;
	if (Match(Token::Name::DERIVING))
	{
		std::expected<std::vector<Token>, CompilerError> deriving_result = ParseDerivingTargets(header.m_name);
		if (!deriving_result.has_value())
		{
			return std::unexpected(std::move(deriving_result.error()));
		}

		deriving_targets = std::move(deriving_result.value());
	}

	MidoriResult::TokenResult semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after union body.");
	if (!semicolon_result.has_value())
	{
		return std::unexpected(semicolon_result.error());
	}

	m_state.m_namespaces.pop_back();

	if (header.m_has_generic_params)
	{
		EndScope();
	}

	MidoriStatement::Union union_stmt(header.m_name, std::vector<Token>(header.m_generic_params), std::vector<Token>(constructor_names), std::vector<MidoriType::ClassConstraint>(header.m_constraints), std::shared_ptr<MidoriType>(union_type));
	if (!deriving_targets.empty())
	{
		std::expected<void, CompilerError> derive_result = QueueDerivedUnionStatements(union_stmt, deriving_targets);
		if (!derive_result.has_value())
		{
			return std::unexpected(std::move(derive_result.error()));
		}
	}

	return std::make_unique<MidoriStatement>(MidoriStatement::Union(std::move(header.m_name), std::move(header.m_generic_params), std::move(constructor_names), std::move(header.m_constraints), std::move(union_type)));
}

bool Parser::TypeBodyHasTopLevelBar()
{
	// Bounded scan from the current token to the depth-0 ';' that ends the
	// declaration, capped by MAX_ARRAY_SIZE and bailing out as soon as any
	// depth counter would go negative, following the ProbeArrayComprehension
	// and ProbeRecordUpdate precedent: unlike those, this scan has no closing
	// delimiter of its own to bound it, so both guards are needed to keep a
	// malformed declaration from running this to true EOF. A depth-0 '|'
	// means a sum; anything else is a newtype over a type expression.
	//
	// The lexer merges adjacent '>' characters into one RIGHT_SHIFT token
	// (e.g. the '>>' closing Array<Array<Int>>), so RIGHT_SHIFT closes two
	// angle-bracket levels here, mirroring how ConsumeTypeRightAngle later
	// splits that same token into two synthetic RIGHT_ANGLE tokens when the
	// real type parser consumes it. LEFT_SHIFT gets no symmetric treatment:
	// two '<' are never adjacent in well-formed type syntax (every '<' that
	// opens a type argument list is preceded by a type name), and ParseType
	// has no LEFT_SHIFT-splitting counterpart to ConsumeTypeRightAngle, so
	// treating '<<' as two opens here would only desynchronize this probe
	// from what the real parser does with it.
	int paren_depth = 0;
	int angle_depth = 0;
	int brace_depth = 0;
	int bracket_depth = 0;

	for (int offset = 0; !Check(Token::Name::END_OF_FILE, offset); offset += 1)
	{
		const Token::Name token_name = Peek(offset).m_token_name;

		if (token_name == Token::Name::LEFT_PAREN)
		{
			paren_depth += 1;
		}
		else if (token_name == Token::Name::RIGHT_PAREN)
		{
			paren_depth -= 1;
		}
		else if (token_name == Token::Name::LEFT_ANGLE)
		{
			angle_depth += 1;
		}
		else if (token_name == Token::Name::RIGHT_ANGLE)
		{
			angle_depth -= 1;
		}
		else if (token_name == Token::Name::RIGHT_SHIFT)
		{
			angle_depth -= 2;
		}
		else if (token_name == Token::Name::LEFT_BRACE)
		{
			brace_depth += 1;
		}
		else if (token_name == Token::Name::RIGHT_BRACE)
		{
			brace_depth -= 1;
		}
		else if (token_name == Token::Name::LEFT_BRACKET)
		{
			bracket_depth += 1;
		}
		else if (token_name == Token::Name::RIGHT_BRACKET)
		{
			bracket_depth -= 1;
		}

		if (paren_depth < 0 || angle_depth < 0 || brace_depth < 0 || bracket_depth < 0)
		{
			return false;
		}

		const bool at_top_level = paren_depth == 0 && angle_depth == 0 && brace_depth == 0 && bracket_depth == 0;

		if (at_top_level && token_name == Token::Name::SINGLE_SEMICOLON)
		{
			return false;
		}

		if (at_top_level && token_name == Token::Name::SINGLE_BAR)
		{
			return true;
		}

		if (offset > MAX_ARRAY_SIZE)
		{
			return false;
		}
	}

	return false;
}

MidoriResult::StatementResult Parser::ParseNewTypeBody(TypeDeclarationHeader&& header)
{
	MidoriResult::TypeResult representation_result = ParseType();
	if (!representation_result.has_value())
	{
		return std::unexpected(representation_result.error());
	}

	std::shared_ptr<MidoriType> representation = std::move(representation_result.value());

	MidoriResult::TokenResult semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after newtype definition.");
	if (!semicolon_result.has_value())
	{
		return std::unexpected(semicolon_result.error());
	}

	std::vector<std::string> generic_param_names;
	std::ranges::transform(header.m_generic_params, std::back_inserter(generic_param_names), [](const Token& generic_param) { return generic_param.m_lexeme; });

	std::shared_ptr<MidoriType> new_type = MidoriType::MakeNewType(header.m_name.m_lexeme, representation, std::move(generic_param_names));
	new_type->GetType<MidoriType::NewType>().m_constraints = header.m_constraints;

	if (header.m_has_generic_params)
	{
		EndScope();
	}

	// Registered in the enclosing scope so uses of the name resolve to the
	// nominal type rather than to its representation.
	m_state.m_scopes.back().m_defined_types[header.m_name.m_lexeme] = new_type;

	return std::make_unique<MidoriStatement>(MidoriStatement::TypeAlias(std::move(header.m_name), std::move(header.m_generic_params), std::move(new_type)));
}

MidoriResult::StatementResult Parser::ParseTypeDeclaration()
{
	std::expected<TypeDeclarationHeader, CompilerError> header_result = ParseTypeDeclarationHeader("type", "Type");
	if (!header_result.has_value())
	{
		return std::unexpected(header_result.error());
	}

	MidoriResult::TokenResult equal_result = Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after type name.");
	if (!equal_result.has_value())
	{
		return std::unexpected(equal_result.error());
	}

	// The token after '=' decides the kind. '{' opens a record body and ParseType
	// has no LEFT_BRACE branch, so the two shapes cannot be confused.
	//
	// This '{' is not the record-update probe's '{'. ProbeRecordUpdate runs at
	// exactly two sites, ParsePrimary and the function-body fast path, both in
	// expression position; a type declaration never reaches expression parsing.
	// Were it somehow fed a record body, it would scan to the depth-0 '}' before
	// finding any 'with' and answer "block", so it fails safe either way.
	if (Check(Token::Name::LEFT_BRACE, 0))
	{
		return ParseStructBody(std::move(header_result.value()));
	}

	// A depth-0 '|' anywhere before the terminating ';' means a sum. Without one,
	// the right-hand side is a type expression and this declares a newtype. A
	// single-variant sum therefore needs an explicit leading bar, which is what
	// frees the bare-name spelling for newtypes.
	if (!TypeBodyHasTopLevelBar())
	{
		return ParseNewTypeBody(std::move(header_result.value()));
	}

	Match(Token::Name::SINGLE_BAR);
	return ParseUnionBody(std::move(header_result.value()));
}

MidoriResult::StatementResult Parser::ParseAliasDeclaration()
{
	// The header gives an alias the same prologue every nominal declaration has, and
	// with it the scope that binds `T` while `= Box<T>` is parsed.
	std::expected<TypeDeclarationHeader, CompilerError> header_result = ParseTypeDeclarationHeader("alias", "Alias");
	if (!header_result.has_value())
	{
		return std::unexpected(header_result.error());
	}

	TypeDeclarationHeader header = std::move(header_result.value());

	// An alias is transparent: it disappears into its expansion before anything can
	// discharge a constraint written on it. Rejecting is the only honest answer.
	if (!header.m_constraints.empty())
	{
		return std::unexpected(GenerateParserError("Alias declarations cannot carry 'where' constraints. Constrain the type the alias expands to instead.", header.m_name));
	}

	MidoriResult::TokenResult equal_result = Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after alias name.");
	if (!equal_result.has_value())
	{
		return std::unexpected(equal_result.error());
	}

	MidoriResult::TypeResult aliased_type_result = ParseType();
	if (!aliased_type_result.has_value())
	{
		return std::unexpected(aliased_type_result.error());
	}

	std::shared_ptr<MidoriType> aliased_type = std::move(aliased_type_result.value());

	MidoriResult::TokenResult semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after alias definition.");
	if (!semicolon_result.has_value())
	{
		return std::unexpected(semicolon_result.error());
	}

	if (header.m_has_generic_params)
	{
		EndScope();
	}

	// aliased_type is already the template a use site needs: substituting Box's own
	// parameters with the alias's left every occurrence of `T` in it standing for the
	// alias's `T`, so applying the alias is one more substitution over the same body.
	// Only the parameter names and their order do not survive that rewrite, so those
	// are what the scope records.
	std::vector<std::string> generic_param_names;
	std::ranges::transform(header.m_generic_params, std::back_inserter(generic_param_names), [](const Token& generic_param) { return generic_param.m_lexeme; });

	m_state.m_scopes.back().m_defined_types[header.m_name.m_lexeme] = aliased_type;
	if (!generic_param_names.empty())
	{
		m_state.m_scopes.back().m_alias_generic_params[header.m_name.m_lexeme] = std::move(generic_param_names);
	}

	return std::make_unique<MidoriStatement>(MidoriStatement::TypeAlias(std::move(header.m_name), std::move(header.m_generic_params), std::move(aliased_type)));
}

MidoriResult::StatementResult Parser::ParseClassDeclaration()
{
	MidoriResult::TokenResult class_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected class name.");
	if (!class_name_result.has_value())
	{
		return std::unexpected(class_name_result.error());
	}

	Token typeclass_name = std::move(class_name_result.value());
	typeclass_name.m_lexeme = Mangle(typeclass_name.m_lexeme);
	if (typeclass_name.m_lexeme[0u] != std::toupper(typeclass_name.m_lexeme[0u]))
	{
		return std::unexpected(GenerateParserError("Class name must start with a capital letter.", typeclass_name));
	}

	MidoriResult::TokenResult defined_name_result = DefineName(typeclass_name, false);
	if (!defined_name_result.has_value())
	{
		return std::unexpected(defined_name_result.error());
	}
	typeclass_name = std::move(defined_name_result.value());

	if (!Match(Token::Name::LEFT_ANGLE))
	{
		return std::unexpected(GenerateParserError("Class must have at least one type parameter.", typeclass_name));
	}

	BeginScope();
	struct ScopeGuard
	{
		Parser* m_parser;

		explicit ScopeGuard(Parser* parser)
			: m_parser(parser)
		{
		}

		~ScopeGuard()
		{
			if (m_parser != nullptr)
			{
				m_parser->EndScope();
			}
		}
	} scope_guard(this);

	std::vector<Token> type_params;
	std::vector<std::shared_ptr<MidoriType>> type_param_types;
	MidoriResult::TokenListResult generic_parse_result = ParseGenericParameters(&type_param_types);
	if (!generic_parse_result.has_value())
	{
		return std::unexpected(generic_parse_result.error());
	}
	type_params = std::move(generic_parse_result.value());

	std::vector<std::string> type_param_names;
	std::ranges::transform(type_params, std::back_inserter(type_param_names), [](const Token& tok) { return tok.m_lexeme; });
	m_state.m_typeclass_type_params[typeclass_name.m_lexeme] = type_param_names;
	std::vector<std::string>& associated_type_names = m_state.m_typeclass_associated_types[typeclass_name.m_lexeme];
	associated_type_names.clear();

	MidoriResult::TokenResult brace_result = Consume(Token::Name::LEFT_BRACE, "Expected '{' before class body.");
	if (!brace_result.has_value())
	{
		return std::unexpected(brace_result.error());
	}

	std::vector<MidoriStatement::Class::AssociatedTypeDeclaration> associated_types;
	std::vector<std::unique_ptr<MidoriStatement>> methods;
	while (!Match(Token::Name::RIGHT_BRACE))
	{
		if (IsAtEnd())
		{
			return std::unexpected(GenerateParserError("Expected '}' after class body.", Previous()));
		}

		if (Match(Token::Name::TYPE))
		{
			MidoriResult::TokenResult assoc_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected associated type name.");
			if (!assoc_name_result.has_value())
			{
				return std::unexpected(assoc_name_result.error());
			}

			Token assoc_name = std::move(assoc_name_result.value());
			assoc_name.m_lexeme = Mangle(assoc_name.m_lexeme);
			if (assoc_name.m_lexeme[0u] != std::toupper(assoc_name.m_lexeme[0u]))
			{
				return std::unexpected(GenerateParserError("Associated type name must start with a capital letter.", assoc_name));
			}

			MidoriResult::TokenResult defined_assoc_result = DefineName(assoc_name, false);
			if (!defined_assoc_result.has_value())
			{
				return std::unexpected(defined_assoc_result.error());
			}
			assoc_name = std::move(defined_assoc_result.value());

			MidoriResult::TokenResult assoc_semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after associated type declaration.");
			if (!assoc_semicolon_result.has_value())
			{
				return std::unexpected(assoc_semicolon_result.error());
			}

			associated_type_names.push_back(assoc_name.m_lexeme);
			std::vector<std::shared_ptr<MidoriType>> associated_type_args = type_param_types;
			m_state.m_scopes.back().m_defined_types[assoc_name.m_lexeme] = MidoriType::MakeAssociatedType(typeclass_name.m_lexeme, assoc_name.m_lexeme, std::move(associated_type_args));
			associated_types.emplace_back(assoc_name);
			continue;
		}

		MidoriResult::TokenResult method_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected method name.");
		if (!method_name_result.has_value())
		{
			return std::unexpected(method_name_result.error());
		}

		Token method_name = std::move(method_name_result.value());
		std::string method_name_str = method_name.m_lexeme;

		MidoriResult::TokenResult colon_result = Consume(Token::Name::SINGLE_COLON, "Expected ':' after method name.");
		if (!colon_result.has_value())
		{
			return std::unexpected(colon_result.error());
		}

		MidoriResult::TokenResult function_result = Consume(Token::Name::FUNCTION, "Expected 'fn' in method signature.");
		if (!function_result.has_value())
		{
			return std::unexpected(function_result.error());
		}

		MidoriResult::TokenResult left_paren_result = Consume(Token::Name::LEFT_PAREN, "Expected '(' before method parameters.");
		if (!left_paren_result.has_value())
		{
			return std::unexpected(left_paren_result.error());
		}

		using MethodParam = std::tuple<Token, std::shared_ptr<MidoriType>>;
		std::expected<std::vector<MethodParam>, CompilerError> params_result = ParseDelimitedZeroOrMoreLimited<MethodParam>
		(
			[this]() -> std::expected<MethodParam, CompilerError>
			{
				MidoriResult::TokenResult param_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected parameter name.");
				if (!param_name_result.has_value())
				{
					return std::unexpected(param_name_result.error());
				}

				Token param_name = std::move(param_name_result.value());
				MidoriResult::TokenResult param_colon_result = Consume(Token::Name::SINGLE_COLON, "Expected ':' after parameter name.");
				if (!param_colon_result.has_value())
				{
					return std::unexpected(param_colon_result.error());
				}

				MidoriResult::TypeResult param_type_result = ParseType();
				if (!param_type_result.has_value())
				{
					return std::unexpected(param_type_result.error());
				}

				return std::make_tuple(std::move(param_name), std::move(param_type_result.value()));
			},
			[this]() { return Consume(Token::Name::COMMA, "Expected ',' between parameters."); },
			[this]() { return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after parameters."); }
		);
		if (!params_result.has_value())
		{
			return std::unexpected(std::move(params_result.error()));
		}

		MidoriResult::TokenResult arrow_result = Consume(Token::Name::THIN_ARROW, "Expected '->' before return type.");
		if (!arrow_result.has_value())
		{
			return std::unexpected(arrow_result.error());
		}

		MidoriResult::TypeResult return_type_result = ParseType();
		if (!return_type_result.has_value())
		{
			return std::unexpected(return_type_result.error());
		}
		std::shared_ptr<MidoriType> return_type = std::move(return_type_result.value());

		MidoriResult::TokenResult method_semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after method signature.");
		if (!method_semicolon_result.has_value())
		{
			return std::unexpected(method_semicolon_result.error());
		}

		std::vector<std::shared_ptr<MidoriType>> param_types;
		std::vector<Token> param_tokens;
		param_types.reserve(params_result->size());
		param_tokens.reserve(params_result->size());
		for (MethodParam& tuple : params_result.value())
		{
			param_types.emplace_back(std::get<1>(tuple));
			param_tokens.emplace_back(std::move(std::get<0>(tuple)));
		}

		std::vector<std::shared_ptr<MidoriType>> param_types_copy = param_types;
		std::shared_ptr<MidoriType> return_type_copy = return_type;
		std::shared_ptr<MidoriType> method_type = MidoriType::MakeFunctionType(std::move(param_types_copy), std::move(return_type_copy));

		m_state.m_class_methods[typeclass_name.m_lexeme].insert(method_name_str);
		m_state.m_typeclass_method_types[typeclass_name.m_lexeme][method_name_str] = method_type;
		methods.emplace_back
		(
			std::make_unique<MidoriStatement>
			(
				MidoriStatement::FunctionDefinition(method_name, std::vector<Token>(), std::move(param_tokens), std::move(param_types), std::move(return_type), nullptr, std::nullopt, 0, std::vector<MidoriType::ClassConstraint>())
			)
		);
	}

	MidoriResult::TokenResult class_semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after class body.");
	if (!class_semicolon_result.has_value())
	{
		return std::unexpected(class_semicolon_result.error());
	}

	scope_guard.m_parser = nullptr;
	EndScope();

	return std::make_unique<MidoriStatement>
	(
		MidoriStatement::Class(std::move(typeclass_name), std::move(type_params), std::vector<MidoriType::ClassConstraint>(), std::move(associated_types), std::move(methods))
	);
}

MidoriResult::StatementResult Parser::ParseInstanceDeclaration()
{
	MidoriResult::TokenResult class_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected class name.");
	if (!class_result.has_value())
	{
		return std::unexpected(class_result.error());
	}

	Token typeclass_name = std::move(class_result.value());
	if (!m_state.m_typeclass_type_params.contains(typeclass_name.m_lexeme))
	{
		return std::unexpected(GenerateParserError("Unknown class '" + typeclass_name.m_lexeme + "'.", typeclass_name));
	}

	MidoriResult::TokenResult angle_result = Consume(Token::Name::LEFT_ANGLE, "Expected '<' before type arguments.");
	if (!angle_result.has_value())
	{
		return std::unexpected(angle_result.error());
	}

	BeginScope();
	struct ScopeGuard
	{
		Parser* m_parser;

		explicit ScopeGuard(Parser* parser)
			: m_parser(parser)
		{
		}

		~ScopeGuard()
		{
			if (m_parser != nullptr)
			{
				m_parser->EndScope();
			}
		}

		ScopeGuard(const ScopeGuard&) = delete;
		ScopeGuard& operator=(const ScopeGuard&) = delete;
	} scope_guard(this);

	struct ImplicitGenericParamGuard
	{
		Parser* m_parser;
		bool m_prev_value;

		explicit ImplicitGenericParamGuard(Parser* parser)
			: m_parser(parser),
			m_prev_value(parser->m_state.m_allow_implicit_generic_params)
		{
			m_parser->m_state.m_allow_implicit_generic_params = true;
		}

		~ImplicitGenericParamGuard()
		{
			if (m_parser != nullptr)
			{
				m_parser->m_state.m_allow_implicit_generic_params = m_prev_value;
			}
		}

		ImplicitGenericParamGuard(const ImplicitGenericParamGuard&) = delete;
		ImplicitGenericParamGuard& operator=(const ImplicitGenericParamGuard&) = delete;
	};

	std::vector<std::shared_ptr<MidoriType>> type_args;
	{
		ImplicitGenericParamGuard implicit_guard(this);
		MidoriResult::TypeListResult type_args_result = ParseDelimitedZeroOrMoreLimited<std::shared_ptr<MidoriType>>
			(
				[this]() { return ParseType(); },
				[this]() { return Consume(Token::Name::COMMA, "Expected ',' between type arguments."); },
				[this]() { return ConsumeTypeRightAngle("Expected '>' after type arguments."); }
			);
		if (!type_args_result.has_value())
		{
			return std::unexpected(type_args_result.error());
		}
		type_args = std::move(type_args_result.value());
	}

	if (type_args.empty())
	{
		return std::unexpected(GenerateParserError("Instance must have at least one type argument.", typeclass_name));
	}

	std::vector<MidoriType::ClassConstraint> constraints;
	if (Match(Token::Name::WHERE))
	{
		std::expected<std::vector<MidoriType::ClassConstraint>, CompilerError> constraints_result = ParseClassConstraints(typeclass_name);
		if (!constraints_result.has_value())
		{
			return std::unexpected(constraints_result.error());
		}

		constraints = std::move(constraints_result.value());
	}

	size_t prev_constraints_size = m_state.m_active_constraints.size();
	PushActiveConstraints(constraints);
	ActiveConstraintGuard constraint_guard(this, prev_constraints_size);

	MidoriResult::TokenResult brace_result = Consume(Token::Name::LEFT_BRACE, "Expected '{' before instance methods.");
	if (!brace_result.has_value())
	{
		return std::unexpected(brace_result.error());
	}

	const std::vector<std::string>& declared_associated_types = m_state.m_typeclass_associated_types[typeclass_name.m_lexeme];
	std::vector<MidoriStatement::Instance::AssociatedTypeBinding> associated_types;
	std::unordered_set<std::string> bound_associated_type_names;
	std::vector<std::unique_ptr<MidoriStatement>> methods;

	while (!Match(Token::Name::RIGHT_BRACE))
	{
		if (IsAtEnd())
		{
			return std::unexpected(GenerateParserError("Expected '}' after instance body.", Previous()));
		}

		if (Match(Token::Name::TYPE))
		{
			MidoriResult::TokenResult assoc_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected associated type name.");
			if (!assoc_name_result.has_value())
			{
				return std::unexpected(assoc_name_result.error());
			}

			Token assoc_name = std::move(assoc_name_result.value());
			assoc_name.m_lexeme = Mangle(assoc_name.m_lexeme);
			if (!std::ranges::contains(declared_associated_types, assoc_name.m_lexeme))
			{
				return std::unexpected(GenerateParserError("Unknown associated type '" + assoc_name.m_lexeme + "' for class '" + typeclass_name.m_lexeme + "'.", assoc_name));
			}
			if (!bound_associated_type_names.insert(assoc_name.m_lexeme).second)
			{
				return std::unexpected(GenerateParserError("Duplicate associated type binding '" + assoc_name.m_lexeme + "'.", assoc_name));
			}

			MidoriResult::TokenResult defined_assoc_result = DefineName(assoc_name, false);
			if (!defined_assoc_result.has_value())
			{
				return std::unexpected(defined_assoc_result.error());
			}
			assoc_name = std::move(defined_assoc_result.value());

			MidoriResult::TokenResult equals_result = Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after associated type name.");
			if (!equals_result.has_value())
			{
				return std::unexpected(equals_result.error());
			}

			MidoriResult::TypeResult assoc_type_result = ParseType();
			if (!assoc_type_result.has_value())
			{
				return std::unexpected(assoc_type_result.error());
			}
			std::shared_ptr<MidoriType> assoc_type = std::move(assoc_type_result.value());

			MidoriResult::TokenResult assoc_semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after associated type binding.");
			if (!assoc_semicolon_result.has_value())
			{
				return std::unexpected(assoc_semicolon_result.error());
			}

			m_state.m_scopes.back().m_defined_types[assoc_name.m_lexeme] = assoc_type;
			associated_types.emplace_back(assoc_name, std::shared_ptr<MidoriType>(assoc_type));
			continue;
		}

		// An instance method is spelled `def show = fn(...) -> R => body;`, the same
		// binding form a module-level function uses.
		if (!Match(Token::Name::DEF))
		{
			return std::unexpected(GenerateParserError("Expected 'def' or associated type binding in instance body.", Peek(0)));
		}

		MidoriResult::TokenResult method_name_result = Consume(Token::Name::IDENTIFIER_LITERAL, "Expected method name.");
		if (!method_name_result.has_value())
		{
			return std::unexpected(method_name_result.error());
		}
		Token method_name = std::move(method_name_result.value());

		MidoriResult::TokenResult method_equal_result = Consume(Token::Name::SINGLE_EQUAL, "Expected '=' after instance method name.");
		if (!method_equal_result.has_value())
		{
			return std::unexpected(method_equal_result.error());
		}

		MidoriResult::TokenResult method_function_result = Consume(Token::Name::FUNCTION, "Expected 'fn' after '=' in an instance method binding.");
		if (!method_function_result.has_value())
		{
			return std::unexpected(method_function_result.error());
		}

		std::vector<Token> generic_params;
		bool has_generic_params = false;
		if (Match(Token::Name::LEFT_ANGLE))
		{
			has_generic_params = true;
			BeginScope();

			MidoriResult::TokenListResult generic_parse_result = ParseGenericParameters(nullptr);
			if (!generic_parse_result.has_value())
			{
				EndScope();
				return std::unexpected(generic_parse_result.error());
			}

			generic_params = std::move(generic_parse_result.value());
		}

		MidoriResult::TokenResult left_paren_result = Consume(Token::Name::LEFT_PAREN, "Expected '(' before method parameters.");
		if (!left_paren_result.has_value())
		{
			if (has_generic_params)
			{
				EndScope();
			}
			return std::unexpected(left_paren_result.error());
		}

		m_state.m_function_depth += 1;
		m_state.m_function_base_variable_index.push_back(m_state.m_total_variables);
		int prev_total_locals = m_state.m_total_locals_in_curr_scope;
		m_state.m_total_locals_in_curr_scope = 0;
		BeginScope();

		MidoriResult::FunctionParamsResult params_parse_result = ParseFunctionParameters();
		if (!params_parse_result.has_value())
		{
			EndScope();
			m_state.m_total_locals_in_curr_scope = prev_total_locals;
			m_state.m_function_base_variable_index.pop_back();
			m_state.m_function_depth -= 1;
			if (has_generic_params)
			{
				EndScope();
			}
			return std::unexpected(params_parse_result.error());
		}

		std::vector<std::pair<Token, std::shared_ptr<MidoriType>>> param_tuples = std::move(params_parse_result.value());
		ParamSplit split = SplitParamTuples(std::move(param_tuples));
		std::vector<Token> params = std::move(split.m_params);
		std::vector<std::shared_ptr<MidoriType>> param_types = std::move(split.m_types);

		MidoriResult::TokenResult return_colon_result = ConsumeReturnTypeSeparator("Expected '->' before return type.");
		if (!return_colon_result.has_value())
		{
			EndScope();
			m_state.m_total_locals_in_curr_scope = prev_total_locals;
			m_state.m_function_base_variable_index.pop_back();
			m_state.m_function_depth -= 1;
			if (has_generic_params)
			{
				EndScope();
			}
			return std::unexpected(return_colon_result.error());
		}

		MidoriResult::TypeResult return_type_result = ParseType();
		if (!return_type_result.has_value())
		{
			EndScope();
			m_state.m_total_locals_in_curr_scope = prev_total_locals;
			m_state.m_function_base_variable_index.pop_back();
			m_state.m_function_depth -= 1;
			if (has_generic_params)
			{
				EndScope();
			}
			return std::unexpected(return_type_result.error());
		}
		std::shared_ptr<MidoriType> return_type = std::move(return_type_result.value());

		MidoriResult::TokenResult fat_arrow_result = Consume(Token::Name::FAT_ARROW, "Expected '=>' before method body.");
		if (!fat_arrow_result.has_value())
		{
			EndScope();
			m_state.m_total_locals_in_curr_scope = prev_total_locals;
			m_state.m_function_base_variable_index.pop_back();
			m_state.m_function_depth -= 1;
			if (has_generic_params)
			{
				EndScope();
			}
			return std::unexpected(fat_arrow_result.error());
		}

		MidoriResult::ExpressionResult body_result = ParseExpression();
		if (!body_result.has_value())
		{
			EndScope();
			m_state.m_total_locals_in_curr_scope = prev_total_locals;
			m_state.m_function_base_variable_index.pop_back();
			m_state.m_function_depth -= 1;
			if (has_generic_params)
			{
				EndScope();
			}
			return std::unexpected(body_result.error());
		}
		std::unique_ptr<MidoriExpression> body = std::move(body_result.value());

		MidoriResult::TokenResult method_semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after method body.");
		if (!method_semicolon_result.has_value())
		{
			EndScope();
			m_state.m_total_locals_in_curr_scope = prev_total_locals;
			m_state.m_function_base_variable_index.pop_back();
			m_state.m_function_depth -= 1;
			if (has_generic_params)
			{
				EndScope();
			}
			return std::unexpected(method_semicolon_result.error());
		}

		EndScope();
		m_state.m_total_locals_in_curr_scope = prev_total_locals;
		m_state.m_function_base_variable_index.pop_back();
		m_state.m_function_depth -= 1;
		if (has_generic_params)
		{
			EndScope();
		}

		methods.emplace_back
		(
			std::make_unique<MidoriStatement>
			(
				MidoriStatement::FunctionDefinition(
					method_name,
					std::move(generic_params),
					std::move(params),
					std::move(param_types),
					std::move(return_type),
					std::move(body),
					std::nullopt,
					0,
					std::vector<MidoriType::ClassConstraint>()
				)
			)
		);
	}

	MidoriResult::TokenResult instance_semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after instance body.");
	if (!instance_semicolon_result.has_value())
	{
		return std::unexpected(instance_semicolon_result.error());
	}

	std::vector<std::shared_ptr<MidoriType>> type_args_copy = type_args;
	m_state.m_class_instance_type_args[typeclass_name.m_lexeme].push_back(std::move(type_args_copy));

	std::unordered_map<std::string, std::shared_ptr<MidoriType>> associated_type_bindings;
	for (const MidoriStatement::Instance::AssociatedTypeBinding& binding : associated_types)
	{
		associated_type_bindings.emplace(binding.m_name.m_lexeme, binding.m_type);
	}
	m_state.m_class_instance_associated_type_bindings[typeclass_name.m_lexeme].push_back(associated_type_bindings);

	for (std::unique_ptr<MidoriStatement>& method_stmt : methods)
	{
		if (method_stmt->IsStatement<MidoriStatement::FunctionDefinition>())
		{
			MidoriStatement::FunctionDefinition& defun = method_stmt->GetStatement<MidoriStatement::FunctionDefinition>();

			std::string method_name = defun.m_name.m_lexeme;
			std::string mangled_name = MidoriType::MangleInstanceMethodName(method_name, typeclass_name.m_lexeme, type_args);
			std::string mangled_name_with_module = mangled_name;
			if (m_context.m_current_module && m_context.m_current_module->HasModuleDeclaration())
			{
				mangled_name_with_module += ModuleSeparator + m_context.m_current_module->ModuleName();
			}
			m_state.m_class_instances[typeclass_name.m_lexeme].push_back(mangled_name_with_module);
			defun.m_name.m_lexeme = mangled_name;
		}
	}

	scope_guard.m_parser = nullptr;
	EndScope();

	return std::make_unique<MidoriStatement>
	(
		MidoriStatement::Instance(std::move(typeclass_name), std::move(type_args), std::move(constraints), std::move(associated_types), std::move(methods))
	);
}

MidoriResult::StatementResult Parser::ParseSimpleStatement()
{
	ParseState checkpoint = m_state;

	MidoriResult::ExpressionResult expr_result = ParseExpression();
	if (!expr_result.has_value())
	{
		return std::unexpected(std::move(expr_result.error()));
	}

	std::unique_ptr<MidoriExpression> expr = std::move(expr_result.value());
	MidoriResult::TokenResult semicolon_result = Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after expression.");
	if (!semicolon_result.has_value())
	{
		if (Check(Token::Name::RIGHT_BRACE, 0))
		{
			m_state = std::move(checkpoint);
			return NoMatch<std::unique_ptr<MidoriStatement>>();
		}

		return std::unexpected(std::move(semicolon_result.error()));
	}

	Token semi_colon = std::move(semicolon_result.value());
	return std::make_unique<MidoriStatement>(MidoriStatement::ExpressionStatement(semi_colon, std::move(expr)));
}

MidoriResult::StatementResult Parser::ParseForeignStatement()
{
	return Consume(Token::Name::TEXT_LITERAL, "Expected name used in library.")
		.and_then
		(
			[this](Token&& foreign_name) ->MidoriResult::StatementResult
			{
				return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected foreign function name.")
					.and_then
					(
						[this, &foreign_name](Token&& function_name) ->MidoriResult::StatementResult
						{
							function_name.m_lexeme = Mangle(function_name.m_lexeme);

							return Consume(Token::Name::SINGLE_COLON, "Expected ':' before foreign function type.")
								.and_then
								(
									[&foreign_name, &function_name, this](Token&&) ->MidoriResult::StatementResult
									{
										constexpr bool is_variable = true;
										return DefineName(function_name, is_variable)
											.and_then
											(
												[&foreign_name, &function_name, this](Token&& name) ->MidoriResult::StatementResult
												{
													std::optional<int> local_index = RegisterOrUpdateLocalVariable(name.m_lexeme);
													constexpr bool is_foreign = true;
													BeginScope();
													struct ForeignTypeScopeGuard
													{
														Parser* m_parser;
														bool m_prev_allow_implicit_generic_params;

														explicit ForeignTypeScopeGuard(Parser* parser)
															: m_parser(parser),
															  m_prev_allow_implicit_generic_params(parser->m_state.m_allow_implicit_generic_params)
														{
															m_parser->m_state.m_allow_implicit_generic_params = true;
														}

														~ForeignTypeScopeGuard()
														{
															m_parser->m_state.m_allow_implicit_generic_params = m_prev_allow_implicit_generic_params;
															m_parser->EndScope();
														}
													} foreign_type_scope_guard(this);

													return ParseType(is_foreign)
														.and_then
														(
															[&foreign_name, &function_name, &local_index, this](std::shared_ptr<MidoriType>&& type)->MidoriResult::StatementResult
															{
																if (!type->IsType<MidoriType::FunctionType>())
																{
																	return std::unexpected(GenerateParserError("'foreign' only applies to function types.", function_name));
																}

																return Consume(Token::Name::SINGLE_SEMICOLON, "Expected ';' after foreign function type.")
																	.and_then
																	(
																		[&foreign_name, &function_name, &type, &local_index](Token&&) ->MidoriResult::StatementResult
																		{
																			return std::make_unique<MidoriStatement>(MidoriStatement::ForeignDefinition(function_name, foreign_name.m_lexeme, std::move(type), std::move(local_index)));
																		}
																	);
															}
														);
												}
											);
									}
								);
						}
					);
			}
		);
}

MidoriResult::ExpressionResult Parser::ParseMatchExpressionWithScrutinee(Token& match_keyword, std::unique_ptr<MidoriExpression>&& expr)
{
	static int s_match_counter = 0;
	std::optional<int> match_value_index_opt = RegisterHiddenLocal(std::string(MATCH_VALUE_PREFIX) + std::to_string(s_match_counter));
	int match_value_index = match_value_index_opt.value_or(-1);
	s_match_counter += 1;

	return Consume(Token::Name::WITH, "Expected 'with' after match expression.")
		.and_then
		(
			[expr = std::move(expr), &match_keyword, this, match_value_index, match_value_index_opt](Token&&) mutable ->MidoriResult::ExpressionResult
			{
				std::unordered_set<std::string> visited_names;
				std::vector<std::unique_ptr<MidoriExpression>> cases;

				while (Match(Token::Name::CASE))
				{
					Token& case_keyword = Previous();
					MidoriResult::ExpressionResult case_result = ParseCaseExpression(visited_names, case_keyword);
					if (!case_result.has_value())
					{
						return std::unexpected(std::move(case_result.error()));
					}
					cases.emplace_back(std::move(case_result.value()));
				}

				if (Check(Token::Name::IDENTIFIER_LITERAL, 0) && Peek(0).m_lexeme == "default")
				{
					return std::unexpected(GenerateParserError("'default' is no longer supported. Write 'case _ =>' instead; '_' is a wildcard pattern, so it also works nested inside other patterns.", Peek(0)));
				}

				if (cases.empty())
				{
					return std::unexpected(GenerateParserError("Expected at least one case.", match_keyword));
				}

				std::unique_ptr<MidoriExpression> match_expr = std::make_unique<MidoriExpression>(MidoriExpression::Match(match_keyword, std::move(expr), std::move(cases)));
				match_expr->GetExpression<MidoriExpression::Match>().m_match_value_index = match_value_index;

				if (match_value_index_opt.has_value())
				{
					m_state.m_total_locals_in_curr_scope -= 1;
					m_state.m_total_variables -= 1;
				}

				return match_expr;
			}
		);
}

MidoriResult::ExpressionResult Parser::ParseMatchExpression()
{
	Token& match_keyword = Previous();
	return ParseExpression()
		.and_then
		(
			[&match_keyword, this](std::unique_ptr<MidoriExpression>&& expr) ->MidoriResult::ExpressionResult
			{
				return ParseMatchExpressionWithScrutinee(match_keyword, std::move(expr));
			}
		);
}

MidoriResult::ExpressionResult Parser::ParseIfElseExpression()
{
	Token& if_token = Previous();
	return ParseExpression()
		.and_then
		(
			[&if_token, this](std::unique_ptr<MidoriExpression>&& condition) -> MidoriResult::ExpressionResult
			{
				return Consume(Token::Name::THEN, "Expected 'then'.")
					.and_then
					(
						[&if_token, &condition, this](Token&& then_token) -> MidoriResult::ExpressionResult
						{
							return ParseExpression()
								.and_then
								(
									[&if_token, &condition, &then_token, this](std::unique_ptr<MidoriExpression>&& true_branch) -> MidoriResult::ExpressionResult
									{
										return Consume(Token::Name::ELSE, "Expected 'else'.")
											.and_then
											(
												[&if_token, &condition, &true_branch, &then_token, this](Token&& else_token) -> MidoriResult::ExpressionResult
												{
													return ParseExpression()
														.and_then
														(
															[&if_token, &condition, &true_branch, &then_token, &else_token](std::unique_ptr<MidoriExpression>&& else_branch) -> MidoriResult::ExpressionResult
															{
																return std::make_unique<MidoriExpression>(MidoriExpression::IfElse(if_token, then_token, else_token, std::move(condition), std::move(true_branch), std::move(else_branch), MidoriExpression::ConditionOperandType::OTHER));
															}
														);
												}
											);
									}
								);
						}
					);
			}
		);
}

MidoriResult::ExpressionResult Parser::ParseFunctionExpression()
{
	Token& keyword = Previous();

	// Parse optional generic parameters <T, U, ...>
	// Create scope BEFORE parsing so DefineName() in ParseGenericParameters adds them to this scope
	std::vector<Token> generic_params;
	std::vector<std::shared_ptr<MidoriType>> generic_param_types;
	bool has_generic_params = false;

	if (Match(Token::Name::LEFT_ANGLE))
	{
		has_generic_params = true;
		BeginScope();  // Create scope for generic parameters

		MidoriResult::TokenListResult generic_parse_result = ParseGenericParameters(&generic_param_types);
		if (!generic_parse_result.has_value())
		{
			EndScope();  // Clean up scope on error
			return std::unexpected(generic_parse_result.error());
		}

		generic_params = std::move(generic_parse_result.value());
	}

	MidoriResult::TokenResult left_paren_result = Consume(Token::Name::LEFT_PAREN, "Expected '(' before function parameters.");
	if (!left_paren_result.has_value())
	{
		if (has_generic_params)
		{
			EndScope();
		}
		return std::unexpected(left_paren_result.error());
	}

	m_state.m_function_depth += 1;
	m_state.m_function_base_variable_index.push_back(m_state.m_total_variables);
	int prev_total_locals = m_state.m_total_locals_in_curr_scope;
	m_state.m_total_locals_in_curr_scope = 0;
	BeginScope();

	// Unwinds the function scope (and the generic parameter scope, if one was opened).
	auto unwind_function_state = [prev_total_locals, has_generic_params, this]()
	{
		EndScope();
		m_state.m_total_locals_in_curr_scope = prev_total_locals;
		m_state.m_function_base_variable_index.pop_back();
		m_state.m_function_depth -= 1;
		if (has_generic_params)
		{
			EndScope();
		}
	};

	MidoriResult::FunctionParamsResult params_parse_result = ParseFunctionParameters(true);
	if (!params_parse_result.has_value())
	{
		unwind_function_state();
		return std::unexpected(params_parse_result.error());
	}

	std::vector<std::pair<Token, std::shared_ptr<MidoriType>>> param_tuples = std::move(params_parse_result.value());
	ParamSplit split = SplitParamTuples(std::move(param_tuples));
	std::vector<Token> params = std::move(split.m_params);
	std::vector<std::shared_ptr<MidoriType>> param_types = std::move(split.m_types);

	// The return type stays optional - `fn(x) => e` is still a whole function. Only the
	// ':' spelling of the separator goes. It is checked before the '->' match rather than
	// left to fall through to the '=>' consume below, which would report a missing body.
	std::shared_ptr<MidoriType> return_type = MidoriType::MakeUndecidedType();
	if (Check(Token::Name::SINGLE_COLON, 0))
	{
		unwind_function_state();
		return std::unexpected(GenerateRemovedReturnTypeColonError());
	}

	if (Match(Token::Name::THIN_ARROW))
	{
		MidoriResult::TypeResult return_type_result = ParseType();
		if (!return_type_result.has_value())
		{
			unwind_function_state();
			return std::unexpected(return_type_result.error());
		}

		return_type = std::move(return_type_result.value());
	}

	// Parse optional 'where' clauses. A where clause may name constraints the
	// signature never mentions, so they must be recorded explicitly.
	std::vector<MidoriType::ClassConstraint> constraints;
	if (Match(Token::Name::WHERE))
	{
		std::expected<std::vector<MidoriType::ClassConstraint>, CompilerError> constraints_result = ParseClassConstraints(keyword);
		if (!constraints_result.has_value())
		{
			unwind_function_state();
			return std::unexpected(constraints_result.error());
		}

		constraints = std::move(constraints_result.value());
	}

	MidoriResult::TokenResult fat_arrow_result = Consume(Token::Name::FAT_ARROW, "Expected '=>' before function body.");
	if (!fat_arrow_result.has_value())
	{
		unwind_function_state();
		return std::unexpected(fat_arrow_result.error());
	}

	size_t prev_constraints_size = m_state.m_active_constraints.size();
	std::vector<MidoriType::ClassConstraint> propagated_constraints = CollectSignatureConstraints(param_types, return_type);
	for (MidoriType::ClassConstraint& propagated_constraint : propagated_constraints)
	{
		AppendUniqueConstraint(constraints, std::move(propagated_constraint));
	}
	PushActiveConstraints(constraints);
	ActiveConstraintGuard constraint_guard(this, prev_constraints_size);

	// If body is a block, parse just the block without continuing to parse calls
	// This prevents `fn() => {}()` from parsing `()` as part of the function body.
	//
	// A record update is not a block: its value is a struct you may well want to project
	// or call straight away, so `=> { c with p = 1 }.port` must behave exactly as it does
	// in expression position. It therefore does continue through the postfix chain. The
	// asymmetry is deliberate - the two constructs genuinely differ.
	MidoriResult::ExpressionResult body_result = [this]() -> MidoriResult::ExpressionResult
		{
			if (!Match(Token::Name::LEFT_BRACE))
			{
				return ParseExpression();
			}

			if (!ProbeRecordUpdate())
			{
				return ParseBlockExpression();
			}

			return ParseRecordUpdate()
				.and_then
				(
					[this](std::unique_ptr<MidoriExpression>&& record_update) -> MidoriResult::ExpressionResult
					{
						return ParsePostfixChain(std::move(record_update));
					}
				);
		}();

	if (!body_result.has_value())
	{
		unwind_function_state();
		return std::unexpected(body_result.error());
	}

	unwind_function_state();

	return std::make_unique<MidoriExpression>(MidoriExpression::Function(keyword, std::move(generic_params), std::move(params), std::move(param_types), std::move(return_type), std::move(body_result.value()), m_state.m_total_variables, std::move(constraints)));
}

MidoriResult::ExpressionResult Parser::ParseCaseExpression(std::unordered_set<std::string>& visited_members, Token& keyword)
{
	BeginScope();
	MidoriResult::PatternResult pattern_result = ParsePattern();
	if (!pattern_result.has_value())
	{
		EndScope();
		return std::unexpected(std::move(pattern_result.error()));
	}

	std::unique_ptr<MidoriPattern> pattern = std::move(pattern_result.value());

	std::unique_ptr<MidoriExpression> guard = nullptr;
	if (Match(Token::Name::IF))
	{
		MidoriResult::ExpressionResult guard_result = ParseExpression();
		if (!guard_result.has_value())
		{
			EndScope();
			return std::unexpected(std::move(guard_result.error()));
		}
		guard = std::move(guard_result.value());
	}

	if (guard == nullptr && pattern->IsPattern<MidoriPattern::Constructor>())
	{
		const MidoriPattern::Constructor& constructor = pattern->GetPattern<MidoriPattern::Constructor>();
		if (constructor.m_is_union)
		{
			const bool covers_constructor = std::ranges::all_of(constructor.m_args, [](const std::unique_ptr<MidoriPattern>& arg) { return IsCatchAllPattern(*arg); });
			const std::string signature = PatternSignature(*pattern);

			if (visited_members.contains(constructor.m_name))
			{
				EndScope();
				return std::unexpected(GenerateParserError(std::format("An earlier case already matches every '{}', so this one can never run.", constructor.m_name), constructor.m_name_token));
			}

			if (visited_members.contains(signature))
			{
				EndScope();
				return std::unexpected(GenerateParserError("Duplicate case in match statement.", constructor.m_name_token));
			}

			visited_members.emplace(signature);
			if (covers_constructor)
			{
				visited_members.emplace(constructor.m_name);
			}
		}
	}

	int binding_count = PatternBindingCounter::Count(*pattern);

	return Consume(Token::Name::FAT_ARROW, "Expected '=>' after case.")
		.and_then
		(
			[&keyword, &pattern, &guard, binding_count, this](Token&&)->MidoriResult::ExpressionResult
			{
				return ParseExpression()
					.and_then
					(
						[&keyword, &pattern, &guard, binding_count, this](std::unique_ptr<MidoriExpression>&& case_expr)->MidoriResult::ExpressionResult
						{
							EndScope();
							return std::make_unique<MidoriExpression>(MidoriExpression::Case(keyword, std::move(pattern), std::move(case_expr), binding_count, std::move(guard)));
						}
					);
			}
		);
}

MidoriResult::PatternResult Parser::ParsePattern()
{
	if (Match(Token::Name::LEFT_PAREN))
	{
		Token left_paren = Previous();

		if (Match(Token::Name::RIGHT_PAREN))
		{
			return std::make_unique<MidoriPattern>(MidoriPattern::Literal(Previous(), MidoriPattern::LiteralKind::Unit));
		}

		return ParsePattern()
			.and_then
			(
				[this, left_paren](std::unique_ptr<MidoriPattern>&& first_pattern) -> MidoriResult::PatternResult
				{
					if (Match(Token::Name::COMMA))
					{
						std::vector<std::unique_ptr<MidoriPattern>> elements;
						elements.push_back(std::move(first_pattern));

						do
						{
							MidoriResult::PatternResult elem_result = ParsePattern();
							if (!elem_result)
							{
								return std::unexpected(elem_result.error());
							}
							elements.push_back(std::move(elem_result.value()));
						} while (Match(Token::Name::COMMA));

						return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after tuple pattern.")
							.and_then
							(
								[&elements, left_paren](Token&&) -> MidoriResult::PatternResult
								{
									return std::make_unique<MidoriPattern>(MidoriPattern::Tuple(left_paren, std::move(elements)));
								}
							);
					}

					return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after pattern.")
						.and_then
						(
							[pattern = std::move(first_pattern)](Token&&) mutable -> MidoriResult::PatternResult
							{
								return std::move(pattern);
							}
						);
				}
			);
	}

	if (Match(Token::Name::LEFT_BRACKET))
	{
		Token left_bracket = Previous();
		if (Match(Token::Name::RIGHT_BRACKET))
		{
			return std::make_unique<MidoriPattern>(MidoriPattern::Array(left_bracket, {}));
		}

		return ParsePattern()
			.and_then
			(
				[this, left_bracket](std::unique_ptr<MidoriPattern>&& first_pattern) -> MidoriResult::PatternResult
				{
					std::vector<std::unique_ptr<MidoriPattern>> elements;
					elements.push_back(std::move(first_pattern));

					while (Match(Token::Name::COMMA))
					{
						MidoriResult::PatternResult elem_result = ParsePattern();
						if (!elem_result)
						{
							return std::unexpected(elem_result.error());
						}
						elements.push_back(std::move(elem_result.value()));
					}

					return Consume(Token::Name::RIGHT_BRACKET, "Expected ']' after array pattern.")
						.and_then
						(
							[&elements, left_bracket](Token&&) -> MidoriResult::PatternResult
							{
								return std::make_unique<MidoriPattern>(MidoriPattern::Array(left_bracket, std::move(elements)));
							}
						);
				}
			);
	}

	if (Match(Token::Name::TRUE, Token::Name::FALSE))
	{
		return std::make_unique<MidoriPattern>(MidoriPattern::Literal(Previous(), MidoriPattern::LiteralKind::Bool));
	}

	if (Match(Token::Name::FLOAT_LITERAL))
	{
		return std::make_unique<MidoriPattern>(MidoriPattern::Literal(Previous(), MidoriPattern::LiteralKind::Float));
	}

	if (Match(Token::Name::INTEGER_LITERAL))
	{
		return MakeNumericLiteralPattern(Previous());
	}

	if (Match(Token::Name::TEXT_LITERAL))
	{
		return std::make_unique<MidoriPattern>(MidoriPattern::Literal(Previous(), MidoriPattern::LiteralKind::Text));
	}

	if (Match(Token::Name::SINGLE_MINUS, Token::Name::SINGLE_PLUS))
	{
		Token sign_token = Previous();
		bool is_negative = sign_token.m_token_name == Token::Name::SINGLE_MINUS;
		if (Match(Token::Name::FLOAT_LITERAL))
		{
			Token literal = Previous();
			literal.m_lexeme = (is_negative ? "-"s : ""s) + literal.m_lexeme;
			return std::make_unique<MidoriPattern>(MidoriPattern::Literal(literal, MidoriPattern::LiteralKind::Float));
		}
		if (Match(Token::Name::INTEGER_LITERAL))
		{
			Token literal = Previous();
			literal.m_lexeme = (is_negative ? "-"s : ""s) + literal.m_lexeme;
			return MakeNumericLiteralPattern(std::move(literal));
		}

		return std::unexpected(GenerateParserError("Expected numeric literal after unary sign in pattern.", sign_token));
	}

	if (Match(Token::Name::IDENTIFIER_LITERAL))
	{
		Token identifier = Previous();
		if (identifier.m_lexeme == "_")
		{
			return std::make_unique<MidoriPattern>(MidoriPattern::Wildcard(identifier));
		}

		return MatchNameResolution()
			.and_then
			(
				[&identifier, this](Token&& resolved) -> MidoriResult::PatternResult
				{
					resolved.m_lexeme = Mangle(resolved.m_lexeme);

					bool is_union = false;
					bool is_constructor = false;
					for (Parser::Scopes::reverse_iterator it = m_state.m_scopes.rbegin(); it != m_state.m_scopes.rend(); ++it)
					{
						if (it->m_union_constructors.contains(resolved.m_lexeme))
						{
							is_union = true;
							is_constructor = true;
							break;
						}
						if (it->m_struct_constructors.contains(resolved.m_lexeme))
						{
							is_union = false;
							is_constructor = true;
							break;
						}
					}

					if (!is_constructor)
					{
						std::string raw_name = resolved.m_lexeme;
						std::string lookup_base = raw_name;
						size_t separator_pos = raw_name.find(NameSeparator);
						if (separator_pos != std::string::npos)
						{
							lookup_base = raw_name.substr(0, separator_pos);
						}

						const std::function<bool(const TypeEnvironment&)> resolve_imported_constructor = [&](const TypeEnvironment& env) -> bool
						{
							std::string type_name = lookup_base;
							if (!env.contains(type_name))
							{
								return false;
							}

							std::shared_ptr<MidoriType> type = env.at(type_name);
							if (type->IsType<MidoriType::UnionType>())
							{
								if (separator_pos == std::string::npos)
								{
									return false;
								}

								std::string constructor_part = raw_name.substr(separator_pos + NameSeparator.length());
								const MidoriType::UnionType& union_type = type->GetType<MidoriType::UnionType>();
								std::string member_key = union_type.m_name + NameSeparator.data() + constructor_part;

								if (union_type.m_member_info.contains(member_key))
								{
									resolved.m_lexeme = member_key;
									is_union = true;
									is_constructor = true;
									return true;
								}
							}
							else if (type->IsType<MidoriType::StructType>())
							{
								if (raw_name == type_name)
								{
									is_union = false;
									is_constructor = true;
									return true;
								}
							}

							return false;
						};

						const UseImportResolution use_import_resolution = ResolveUseImport(lookup_base);
						if (use_import_resolution.m_status == UseImportResolutionStatus::Ambiguous)
						{
							return std::unexpected(GenerateParserError(BuildAmbiguousUseImportError(lookup_base, use_import_resolution.m_conflicting_modules), identifier));
						}

						if (use_import_resolution.m_status == UseImportResolutionStatus::Resolved)
						{
							const std::string& module_name = use_import_resolution.m_module_name;
							if (m_context.m_imported_type_signatures.contains(module_name))
							{
								static_cast<void>(resolve_imported_constructor(m_context.m_imported_type_signatures.at(module_name)));
							}
						}

						if (!is_constructor)
						{
							for (const auto& [mod_name, env] : m_context.m_imported_type_signatures)
							{
								if (resolve_imported_constructor(env))
								{
									break;
								}
							}
						}
					}

					if (is_constructor)
					{
						if (Match(Token::Name::LEFT_PAREN))
						{
							std::vector<std::unique_ptr<MidoriPattern>> args;
							if (!Match(Token::Name::RIGHT_PAREN))
							{
								do
								{
									MidoriResult::PatternResult arg_result = ParsePattern();
									if (!arg_result)
									{
										return std::unexpected(arg_result.error());
									}
									args.push_back(std::move(arg_result.value()));
								} while (Match(Token::Name::COMMA));

								if (!Match(Token::Name::RIGHT_PAREN))
								{
									return std::unexpected(GenerateParserError("Expected ')' after constructor pattern.", Peek(0)));
								}
							}

							return std::make_unique<MidoriPattern>(MidoriPattern::Constructor(resolved, std::string(resolved.m_lexeme), std::move(args), is_union));
						}

						return std::make_unique<MidoriPattern>(MidoriPattern::Constructor(resolved, std::string(resolved.m_lexeme), {}, is_union));
					}

					if (resolved.m_lexeme.find(NameSeparator) != std::string::npos)
					{
						return std::unexpected(GenerateParserError("Unknown constructor in pattern.", resolved));
					}

					Token binding_name = std::move(identifier);
					constexpr bool is_variable = true;
					return DefineName(binding_name, is_variable)
						.and_then
						(
							[this](Token&& defined_name) -> MidoriResult::PatternResult
							{
								std::optional<int> local_index = RegisterOrUpdateLocalVariable(defined_name.m_lexeme);
								return std::make_unique<MidoriPattern>(MidoriPattern::Binding(defined_name, std::move(local_index)));
							}
						);
				}
			);
	}

	return std::unexpected(GenerateParserError("Expected pattern.", Peek(0)));
}

MidoriResult::StatementResult Parser::ParseStatement()
{
	return ParseSimpleStatement();
}

MidoriResult::TypeResult Parser::ParseType(bool is_foreign)
{
	return ParseChoice<std::shared_ptr<MidoriType>>(m_state,
		[this]() -> MidoriResult::TypeResult
		{
			return ParseWhen<std::shared_ptr<MidoriType>>(m_state,
				Token::Name::TEXT,
				[this](Token&&) -> MidoriResult::TypeResult
				{
					return MidoriType::MakeLiteralType<MidoriType::TextType>();
				}
			);
		},
		[this]() -> MidoriResult::TypeResult
		{
			return ParseWhen<std::shared_ptr<MidoriType>>(m_state,
				Token::Name::FLOAT,
				[this](Token&&) -> MidoriResult::TypeResult
				{
					return MidoriType::MakeLiteralType<MidoriType::FloatType>();
				}
			);
		},
		[this]() -> MidoriResult::TypeResult
		{
			return ParseWhen<std::shared_ptr<MidoriType>>(m_state,
				Token::Name::INTEGER,
				[this](Token&&) -> MidoriResult::TypeResult
				{
					return MidoriType::MakeLiteralType<MidoriType::IntegerType>();
				}
			);
		},
		[this]() -> MidoriResult::TypeResult
		{
			return ParseWhen<std::shared_ptr<MidoriType>>(m_state,
				Token::Name::BYTE,
				[this](Token&&) -> MidoriResult::TypeResult
				{
					return MidoriType::MakeLiteralType<MidoriType::ByteType>();
				}
			);
		},
		[this]() -> MidoriResult::TypeResult
		{
			return ParseWhen<std::shared_ptr<MidoriType>>(m_state,
				Token::Name::WORD,
				[this](Token&&) -> MidoriResult::TypeResult
				{
					return MidoriType::MakeLiteralType<MidoriType::WordType>();
				}
			);
		},
		[this]() -> MidoriResult::TypeResult
		{
			return ParseWhen<std::shared_ptr<MidoriType>>(m_state,
				Token::Name::BOOL,
				[this](Token&&) -> MidoriResult::TypeResult
				{
					return MidoriType::MakeLiteralType<MidoriType::BoolType>();
				}
			);
		},
		[this]() -> MidoriResult::TypeResult
		{
			return ParseWhen<std::shared_ptr<MidoriType>>(m_state,
				Token::Name::UNIT,
				[this](Token&&) -> MidoriResult::TypeResult
				{
					return MidoriType::MakeLiteralType<MidoriType::UnitType>();
				}
			);
		},
		[this]() -> MidoriResult::TypeResult
		{
			return ParseWhen<std::shared_ptr<MidoriType>>(m_state,
				Token::Name::NEVER,
				[this](Token&&) -> MidoriResult::TypeResult
				{
					return MidoriType::MakeLiteralType<MidoriType::NeverType>();
				}
			);
		},
		[this]() -> MidoriResult::TypeResult
		{
			return ParseWhen<std::shared_ptr<MidoriType>>(m_state,
				Token::Name::ARRAY,
				[this](Token&&) -> MidoriResult::TypeResult
				{
					return Consume(Token::Name::LEFT_ANGLE, "Expected '<' after 'Array'.")
						.and_then
						(
							[this](Token&&) -> MidoriResult::TypeResult
							{
								return ParseType()
									.and_then
									(
										[this](std::shared_ptr<MidoriType>&& type) -> MidoriResult::TypeResult
										{
											return ConsumeTypeRightAngle("Expected '>' after array type.")
												.and_then
												(
													[&type](Token&&) -> MidoriResult::TypeResult
													{
														return MidoriType::MakeArrayType(type);
													}
												);
										}
									);
							}
						);
				}
			);
		},
		[this]() -> MidoriResult::TypeResult
		{
			return ParseWhen<std::shared_ptr<MidoriType>>(m_state,
				Token::Name::RANGE,
				[this](Token&&) -> MidoriResult::TypeResult
				{
					return Consume(Token::Name::LEFT_ANGLE, "Expected '<' after 'Range'.")
						.and_then
						(
							[this](Token&&) -> MidoriResult::TypeResult
							{
								return ParseType()
									.and_then
									(
										[this](std::shared_ptr<MidoriType>&& type) -> MidoriResult::TypeResult
										{
											return ConsumeTypeRightAngle("Expected '>' after range element type.")
												.and_then
												(
													[&type](Token&&) -> MidoriResult::TypeResult
													{
														return MidoriType::MakeRangeType(type);
													}
												);
										}
									);
							}
						);
				}
			);
		},
		[this]() -> MidoriResult::TypeResult
		{
			return ParseWhen<std::shared_ptr<MidoriType>>(m_state,
				Token::Name::WORKER,
				[this](Token&&) -> MidoriResult::TypeResult
				{
					return Consume(Token::Name::LEFT_ANGLE, "Expected '<' after 'Worker'.")
						.and_then
						(
							[this](Token&&) -> MidoriResult::TypeResult
							{
								return ParseType()
									.and_then
									(
										[this](std::shared_ptr<MidoriType>&& type) -> MidoriResult::TypeResult
										{
											return ConsumeTypeRightAngle("Expected '>' after worker result type.")
												.and_then
												(
													[&type](Token&&) -> MidoriResult::TypeResult
													{
														return MidoriType::MakeWorkerType(type);
													}
												);
										}
									);
							}
						);
				}
			);
		},
		[this]() -> MidoriResult::TypeResult
		{
			return ParseWhen<std::shared_ptr<MidoriType>>(m_state,
				Token::Name::CHANNEL_TYPE,
				[this](Token&&) -> MidoriResult::TypeResult
				{
					return Consume(Token::Name::LEFT_ANGLE, "Expected '<' after 'Channel'.")
						.and_then
						(
							[this](Token&&) -> MidoriResult::TypeResult
							{
								return ParseType()
									.and_then
									(
										[this](std::shared_ptr<MidoriType>&& type) -> MidoriResult::TypeResult
										{
											return ConsumeTypeRightAngle("Expected '>' after channel element type.")
												.and_then
												(
													[&type](Token&&) -> MidoriResult::TypeResult
													{
														return MidoriType::MakeChannelType(type);
													}
												);
										}
									);
							}
						);
				}
			);
		},
		[this, is_foreign]() -> MidoriResult::TypeResult
		{
			return ParseWhen<std::shared_ptr<MidoriType>>(m_state,
				Token::Name::FUNCTION,
				[this, is_foreign](Token&&) -> MidoriResult::TypeResult
				{
					std::function<MidoriResult::TypeResult(std::vector<std::shared_ptr<MidoriType>>&&)> func_type_aux_func = [is_foreign, this](std::vector<std::shared_ptr<MidoriType>>&& types) ->MidoriResult::TypeResult
						{
							return Consume(Token::Name::THIN_ARROW, "Expected '->' before return type token.")
								.and_then
								(
									[&types, is_foreign, this](Token&&) ->MidoriResult::TypeResult
									{
										return ParseType()
											.and_then
											(
												[&types, is_foreign](std::shared_ptr<MidoriType>&& return_type) ->MidoriResult::TypeResult
												{
													return MidoriType::MakeFunctionType(std::move(types), std::move(return_type), is_foreign);
												}
											);
									}
								);
						};
					return Consume(Token::Name::LEFT_PAREN, "Expected '(' before function argument types.")
						.and_then
						(
							[&func_type_aux_func, this](Token&&)
							{
								if (!Match(Token::Name::RIGHT_PAREN))
								{
									return ParseDelimitedZeroOrMoreLimited<std::shared_ptr<MidoriType>>
										(
											[this]() { return ParseType(); },
											[this]() { return Consume(Token::Name::COMMA, "Expected ',' after argument type"); },
											[this]() { return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after argument types."); }
										)
										.and_then
										(
											[&func_type_aux_func](std::vector<std::shared_ptr<MidoriType>>&& types) ->MidoriResult::TypeResult
											{
												return func_type_aux_func(std::move(types));
											}
										);
								}
								else
								{
									return func_type_aux_func({});
								}
							}
						);
				}
			);
		},
		[this]() -> MidoriResult::TypeResult
		{
			return ParseWhen<std::shared_ptr<MidoriType>>(m_state,
				Token::Name::LEFT_PAREN,
				[this](Token&&) -> MidoriResult::TypeResult
				{
					if (Match(Token::Name::RIGHT_PAREN))
					{
						return MidoriType::MakeLiteralType<MidoriType::UnitType>();
					}

					return ParseType()
						.and_then
						(
							[this](std::shared_ptr<MidoriType>&& first_type) -> MidoriResult::TypeResult
							{
								if (Match(Token::Name::COMMA))
								{
									std::vector<std::shared_ptr<MidoriType>> element_types;
									element_types.push_back(std::move(first_type));

									do
									{
										MidoriResult::TypeResult elem_result = ParseType();
										if (!elem_result)
										{
											return elem_result;
										}
										element_types.push_back(std::move(elem_result.value()));
									} while (Match(Token::Name::COMMA));

									return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after tuple types.")
										.and_then
										(
											[&element_types](Token&&) -> MidoriResult::TypeResult
											{
												return MidoriType::MakeTupleType(std::move(element_types));
											}
										);
								}
								else
								{
									return Consume
									(
										Token::Name::RIGHT_PAREN, "Expected ')' after type.")
										.and_then([&first_type](Token&&) -> MidoriResult::TypeResult
											{
												return first_type;
											}
										);
								}
							}
						);
				}
			);
		},
		[this]() -> MidoriResult::TypeResult
		{
			return ParseWhen<std::shared_ptr<MidoriType>>(m_state,
				Token::Name::IDENTIFIER_LITERAL,
				[this](Token&&) -> MidoriResult::TypeResult
				{
					return MatchNameResolution()
						.and_then
						(
							[this](Token&& type_name) ->MidoriResult::TypeResult
							{
								std::string mangled_name = Mangle(type_name.m_lexeme);
								std::vector<Scope>::const_reverse_iterator found_scope_it = FindTypeScope(type_name.m_lexeme);

								// Cell is an ordinary identifier, not a keyword like Channel: a program
								// may declare its own type named Cell, and that one wins. Otherwise
								// Cell<T> is the built-in cell type once Cell.mmt is imported.
								if (found_scope_it == m_state.m_scopes.crend() && type_name.m_lexeme == CellModuleName && IsModuleVisible(CellModuleName))
								{
									return ParseCellTypeArguments();
								}

								std::shared_ptr<MidoriType> base_type = nullptr;
								const std::vector<std::string>* alias_generic_params = nullptr;
								auto try_parse_associated_type = [this, &type_name]() -> MidoriResult::TypeResult
								{
									std::string associated_type_qualifier = ExtractQualifier(type_name.m_lexeme);
									if (associated_type_qualifier.empty())
									{
										return std::unexpected(GenerateParserError("Undefined struct or union.", type_name));
									}

									std::string associated_type_name = ExtractSymbolName(type_name.m_lexeme);
									TypeclassAssociatedTypeMap::const_iterator assoc_it = m_state.m_typeclass_associated_types.find(associated_type_qualifier);
									if (assoc_it == m_state.m_typeclass_associated_types.cend()
										|| !std::ranges::contains(assoc_it->second, associated_type_name))
									{
										return std::unexpected(GenerateParserError("Undefined struct or union.", type_name));
									}

									if (!Match(Token::Name::LEFT_ANGLE))
									{
										return std::unexpected(GenerateParserError("Expected '<' after associated type name.", type_name));
									}

									MidoriResult::TypeListResult type_args_result = ParseDelimitedZeroOrMoreLimited<std::shared_ptr<MidoriType>>
									(
										[this]() { return ParseType(); },
										[this]() { return Consume(Token::Name::COMMA, "Expected ',' after associated type argument."); },
										[this]() { return ConsumeTypeRightAngle("Expected '>' after associated type arguments."); }
									);
									if (!type_args_result.has_value())
									{
										return std::unexpected(type_args_result.error());
									}

									const size_t expected_arg_count = m_state.m_typeclass_type_params.contains(associated_type_qualifier)
										? m_state.m_typeclass_type_params.at(associated_type_qualifier).size()
										: 0u;
									std::vector<std::shared_ptr<MidoriType>> type_args = std::move(type_args_result.value());
									if (type_args.size() != expected_arg_count)
									{
										return std::unexpected
										(
											GenerateParserError
											(
												"Associated type argument count mismatch: expected " + std::to_string(expected_arg_count) +
												", got " + std::to_string(type_args.size()),
												type_name
											)
										);
									}

									return MidoriType::MakeAssociatedType(associated_type_qualifier, associated_type_name, std::move(type_args));
								};

								if (found_scope_it != m_state.m_scopes.crend())
								{
									base_type = found_scope_it->m_defined_types.at(type_name.m_lexeme);

									Scope::AliasGenericParamTable::const_iterator alias_params_it = found_scope_it->m_alias_generic_params.find(type_name.m_lexeme);
									if (alias_params_it != found_scope_it->m_alias_generic_params.cend())
									{
										alias_generic_params = std::addressof(alias_params_it->second);
									}
								}
								else
								{
									const UseImportResolution use_import_resolution = ResolveUseImport(type_name.m_lexeme);
									if (use_import_resolution.m_status == UseImportResolutionStatus::Ambiguous)
									{
										return std::unexpected(GenerateParserError(BuildAmbiguousUseImportError(type_name.m_lexeme, use_import_resolution.m_conflicting_modules), type_name));
									}

									if (use_import_resolution.m_status == UseImportResolutionStatus::Resolved)
									{
										std::unordered_map<std::string, TypeEnvironment>::const_iterator module_it = m_context.m_imported_type_signatures.find(use_import_resolution.m_module_name);
										if (module_it != m_context.m_imported_type_signatures.cend())
										{
											TypeEnvironment::const_iterator type_it = module_it->second.find(type_name.m_lexeme);
											if (type_it != module_it->second.cend())
											{
												base_type = type_it->second;
											}
										}
									}

									if (base_type == nullptr)
									{
										std::string associated_type_qualifier = ExtractQualifier(type_name.m_lexeme);
										if (!associated_type_qualifier.empty())
										{
											std::string associated_type_name = ExtractSymbolName(type_name.m_lexeme);
											TypeclassAssociatedTypeMap::const_iterator assoc_it = m_state.m_typeclass_associated_types.find(associated_type_qualifier);
											if (assoc_it != m_state.m_typeclass_associated_types.cend()
												&& std::ranges::contains(assoc_it->second, associated_type_name)
												&& Check(Token::Name::LEFT_ANGLE, 0))
											{
												return try_parse_associated_type();
											}
										}
									}

									if (base_type == nullptr)
									{
										std::string qualifier = ExtractQualifier(type_name.m_lexeme);
										if (!qualifier.empty())
										{
											std::string symbol_name = ExtractSymbolName(type_name.m_lexeme);
											std::unordered_map<std::string, TypeEnvironment>::const_iterator module_it = m_context.m_imported_type_signatures.find(qualifier);
											if (module_it != m_context.m_imported_type_signatures.cend())
											{
												TypeEnvironment::const_iterator type_it = module_it->second.find(symbol_name);
												if (type_it != module_it->second.cend())
												{
													base_type = type_it->second;
												}
												else
												{
													std::unordered_map<std::string, CompiledModule::SymbolTable>::const_iterator symbols_it = m_context.m_imported_symbols.find(qualifier);
													if (symbols_it != m_context.m_imported_symbols.cend())
													{
														return std::unexpected(GenerateParserError("Type '" + symbol_name + "' is not exported by module '" + qualifier + "'.", type_name));
													}
												}
											}
										}
									}

									if (base_type == nullptr)
									{
										for (const auto& [mod_name, env] : m_context.m_imported_type_signatures)
										{
											if (env.contains(type_name.m_lexeme))
											{
												base_type = env.at(type_name.m_lexeme);
												break;
											}
										}
									}

									if (base_type == nullptr)
									{
										std::string associated_type_qualifier = ExtractQualifier(type_name.m_lexeme);
										if (!associated_type_qualifier.empty())
										{
											std::string associated_type_name = ExtractSymbolName(type_name.m_lexeme);
											TypeclassAssociatedTypeMap::const_iterator assoc_it = m_state.m_typeclass_associated_types.find(associated_type_qualifier);
											if (assoc_it != m_state.m_typeclass_associated_types.cend() && std::ranges::contains(assoc_it->second, associated_type_name))
											{
												return try_parse_associated_type();
											}
										}

										if (m_state.m_allow_implicit_generic_params && type_name.m_lexeme.find(NameSeparator) == std::string::npos)
										{
											std::shared_ptr<MidoriType> generic_type = MidoriType::MakeGenericType(type_name.m_lexeme);
											m_state.m_scopes.back().m_defined_types[type_name.m_lexeme] = generic_type;
											base_type = generic_type;
										}
										else
										{
											return std::unexpected(GenerateParserError("Undefined struct or union.", type_name));
										}
									}
								}

								if (Match(Token::Name::LEFT_ANGLE))
								{
									MidoriResult::TypeListResult type_args_result = ParseDelimitedZeroOrMoreLimited<std::shared_ptr<MidoriType>>
										(
											[this]() { return ParseType(); },
											[this]() { return Consume(Token::Name::COMMA, "Expected ',' after type argument."); },
											[this]() { return ConsumeTypeRightAngle("Expected '>' after type arguments."); }
										);

									if (!type_args_result.has_value())
									{
										return std::unexpected(type_args_result.error());
									}

									std::vector<std::shared_ptr<MidoriType>> type_args = std::move(type_args_result.value());

									// An alias binds its own parameters, and its expansion no longer
									// carries any, so the alias is asked first and the expansion
									// only when the name is not one.
									std::vector<std::string> generic_params;
									if (alias_generic_params != nullptr)
									{
										generic_params = *alias_generic_params;
									}
									else if (base_type->IsType<MidoriType::StructType>())
									{
										generic_params = base_type->GetType<MidoriType::StructType>().m_generic_params;
									}
									else if (base_type->IsType<MidoriType::UnionType>())
									{
										generic_params = base_type->GetType<MidoriType::UnionType>().m_generic_params;
									}
									else if (base_type->IsType<MidoriType::NewType>())
									{
										// A newtype is not transparent like an alias: it carries its own
										// m_generic_params (Task 2's substitution arm deliberately
										// preserves them), spelled with the newtype's own parameter
										// names, so the base type can be read directly here exactly as
										// for struct and union. No alias_generic_params side table is
										// needed because there is no separate "expansion" whose params
										// could differ from the declaration's own.
										generic_params = base_type->GetType<MidoriType::NewType>().m_generic_params;
									}

									if (type_args.size() != generic_params.size())
									{
										return std::unexpected(GenerateParserError(
											BuildTypeArgumentCountMismatchMessage(type_name.m_lexeme, alias_generic_params != nullptr, generic_params.size(), type_args.size()), type_name));
									}

									if (base_type->IsType<MidoriType::UnionType>())
									{
										bool is_active_union = false;
										for (const std::shared_ptr<MidoriType>& active_union : m_state.m_active_union_types)
										{
											if (active_union.get() == base_type.get())
											{
												is_active_union = true;
												break;
											}
										}

										if (is_active_union)
										{
											bool type_args_match = true;
											for (size_t i = 0u; i < type_args.size(); i += 1u)
											{
												if (!type_args[i]->IsType<MidoriType::GenericParam>())
												{
													type_args_match = false;
													break;
												}

												const std::string& param_name = type_args[i]->GetType<MidoriType::GenericParam>().m_name;
												if (param_name != generic_params[i])
												{
													type_args_match = false;
													break;
												}
											}

											if (type_args_match)
											{
												return base_type;
											}
										}
									}

									std::unordered_map<std::string, std::shared_ptr<MidoriType>> substitutions;
									for (size_t i = 0; i < generic_params.size(); ++i)
									{
										substitutions[generic_params[i]] = type_args[i];
									}

									return MidoriType::SubstituteTypeParams(base_type, substitutions);
								}

								// Without arguments a parameterised alias would hand back a template
								// whose parameters nothing binds.
								if (alias_generic_params != nullptr)
								{
									return std::unexpected(GenerateParserError(
										BuildTypeArgumentCountMismatchMessage(type_name.m_lexeme, true, alias_generic_params->size(), 0u), type_name));
								}

								return base_type;
							}
						);
				}
			);
		},
		[this]() -> MidoriResult::TypeResult
		{
			return std::unexpected(GenerateParserError("Expected type token.", Peek(0)));
		}
	);
}

MidoriResult::StatementResult Parser::ParseDeclaration()
{
	// End of current block: no further declarations to parse here.
	if (Check(Token::Name::RIGHT_BRACE, 0))
	{
		return NoMatch<std::unique_ptr<MidoriStatement>>();
	}

	// `defun` is no longer a keyword, so it now lexes as an ordinary identifier and a
	// file still using it would fail with a bare "Undefined name." Name the removal
	// instead, but only for `defun Name`, so an identifier spelled `defun` is untouched.
	if (Check(Token::Name::IDENTIFIER_LITERAL, 0) && Peek(0).m_lexeme == "defun" && Check(Token::Name::IDENTIFIER_LITERAL, 1))
	{
		return std::unexpected(GenerateParserError("'defun' is no longer supported. Write 'def Name = fn(params) -> Type => body;' instead.", Peek(0)));
	}

	// `struct` and `union` went the same way, replaced by the one `type` keyword. Both
	// now lex as ordinary identifiers, so name the removal here rather than let the
	// declaration fall through to a bare "Undefined name." Guarded on a following
	// identifier, so a value named `struct` or `union` is left alone.
	if (Check(Token::Name::IDENTIFIER_LITERAL, 0) && Peek(0).m_lexeme == "struct" && Check(Token::Name::IDENTIFIER_LITERAL, 1))
	{
		return std::unexpected(GenerateParserError("'struct' is no longer supported. Write 'type Name = { field: Type, ... };' instead.", Peek(0)));
	}

	if (Check(Token::Name::IDENTIFIER_LITERAL, 0) && Peek(0).m_lexeme == "union" && Check(Token::Name::IDENTIFIER_LITERAL, 1))
	{
		return std::unexpected(GenerateParserError("'union' is no longer supported. Write 'type Name = A | B(Type);' instead.", Peek(0)));
	}

	return ParseChoice<std::unique_ptr<MidoriStatement>>(m_state,
		[this]() -> MidoriResult::StatementResult
		{
			return ParseWhen<std::unique_ptr<MidoriStatement>>(m_state,
				Token::Name::DEF,
				[this](Token&&) -> MidoriResult::StatementResult
				{
					return ParseDefineStatement();
				}
			);
		},
		[this]() -> MidoriResult::StatementResult
		{
			return ParseWhen<std::unique_ptr<MidoriStatement>>(m_state,
				Token::Name::CLASS,
				[this](Token&&) -> MidoriResult::StatementResult
				{
					return ParseClassDeclaration();
				}
			);
		},
		[this]() -> MidoriResult::StatementResult
		{
			return ParseWhen<std::unique_ptr<MidoriStatement>>(m_state,
				Token::Name::INSTANCE,
				[this](Token&&) -> MidoriResult::StatementResult
				{
					return ParseInstanceDeclaration();
				}
			);
		},
		[this]() -> MidoriResult::StatementResult
		{
			return ParseWhen<std::unique_ptr<MidoriStatement>>(m_state,
				Token::Name::FOREIGN,
				[this](Token&&) -> MidoriResult::StatementResult
				{
					return ParseForeignStatement();
				}
			);
		},
		[this]() -> MidoriResult::StatementResult
		{
			return ParseWhen<std::unique_ptr<MidoriStatement>>(m_state,
				Token::Name::TYPE,
				[this](Token&&) -> MidoriResult::StatementResult
				{
					return ParseTypeDeclaration();
				}
			);
		},
		[this]() -> MidoriResult::StatementResult
		{
			return ParseWhen<std::unique_ptr<MidoriStatement>>(m_state,
				Token::Name::ALIAS,
				[this](Token&&) -> MidoriResult::StatementResult
				{
					return ParseAliasDeclaration();
				}
			);
		},
		[this]() -> MidoriResult::StatementResult
		{
			return ParseStatement();
		}
	);
}

MidoriResult::ParserResult Parser::Parse()
{
	MidoriProgramTree programTree;

	while (!IsAtEnd() || !m_pending_statements.empty())
	{
		if (!m_pending_statements.empty())
		{
			programTree.emplace_back(std::move(m_pending_statements.front()));
			m_pending_statements.pop();
			continue;
		}

		MidoriResult::StatementResult result = ParseDeclaration();
		if (result.has_value())
		{
			programTree.emplace_back(std::move(result.value()));
		}
		else
		{
			// Phase 2 recovery policy: Parse() is the single recovery boundary for hard parser errors.
			// The parser still stops at the first hard error after synchronizing to the next declaration starter.
			Synchronize();
			return std::unexpected(MidoriResult::CompilerDiagnostics(std::move(result.error())));
		}
	}

	return MidoriResult::ParserResult(std::move(programTree));
}

MidoriResult::TokenResult Parser::MatchNameResolution()
{
	Token resolved_name = Previous();
	std::string& resolved_name_str = resolved_name.m_lexeme;

	while (Match(Token::Name::DOUBLE_COLON))
	{
		// We found the separator, now we must have an identifier
		if (!Match(Token::Name::IDENTIFIER_LITERAL))
		{
			return std::unexpected(GenerateParserError(std::format("Expected identifier after '{}'.", NameSeparator), Previous()));
		}

		resolved_name_str.append(NameSeparator).append(Previous().m_lexeme);
	}

	return resolved_name;
}

MidoriResult::TokenListResult Parser::ParseGenericParameters(std::vector<std::shared_ptr<MidoriType>>* out_types)
{
	return ParseDelimitedZeroOrMoreLimited<Token>
		(
			[this, out_types]() -> MidoriResult::TokenResult
			{
				return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected generic parameter name.")
					.and_then
					(
						[this, out_types](Token&& param_name) -> MidoriResult::TokenResult
						{
							constexpr bool is_variable = false;
							return DefineName(param_name, is_variable)
								.and_then
								(
									[this, out_types](Token&& param_name) -> MidoriResult::TokenResult
									{
										std::shared_ptr<MidoriType> param_type = MidoriType::MakeGenericType(param_name.m_lexeme);
										m_state.m_scopes.back().m_defined_types[param_name.m_lexeme] = param_type;

										// Keep the type alive if requested
										if (out_types != nullptr)
										{
											out_types->push_back(param_type);
										}

										return param_name;
									}
								);
						}
					);
			},
			[this]() { return Consume(Token::Name::COMMA, "Expected ',' between generic parameters."); },
			[this]() { return ConsumeTypeRightAngle("Expected '>' after generic parameters."); }
		);
}

void Parser::PushActiveConstraints(const std::vector<MidoriType::ClassConstraint>& constraints)
{
	for (const MidoriType::ClassConstraint& constraint : constraints)
	{
		if (!ContainsConstraint(m_state.m_active_constraints, constraint))
		{
			m_state.m_active_constraints.push_back(constraint);
		}
	}
}

std::expected<std::vector<MidoriType::ClassConstraint>, CompilerError> Parser::ParseClassConstraints(const Token& context_token)
{
	// An equality constraint reads 'Class::Assoc<S> ~ Type'. The '::' one token
	// past the leading identifier is what distinguishes it from a class
	// constraint, which reads 'Class<Type,...>'.
	std::function<std::expected<MidoriType::ClassConstraint, CompilerError>()> parse_equality_constraint = [this]() -> std::expected<MidoriType::ClassConstraint, CompilerError>
		{
			Token projection_token = Peek(0);
			return ParseType()
				.and_then
				(
					[&projection_token, this](std::shared_ptr<MidoriType>&& equality_lhs) -> std::expected<MidoriType::ClassConstraint, CompilerError>
					{
						if (!equality_lhs->IsType<MidoriType::AssociatedType>())
						{
							return std::unexpected(GenerateParserError("The left side of a '~' constraint must be an associated type projection, such as 'Stepper::Item<S>'.", projection_token));
						}

						return Consume(Token::Name::TILDE, "Expected '~' after the associated type projection in an equality constraint.")
							.and_then
							(
								[&equality_lhs, this](Token&&) -> std::expected<MidoriType::ClassConstraint, CompilerError>
								{
									return ParseType()
										.and_then
										(
											[&equality_lhs](std::shared_ptr<MidoriType>&& equality_rhs) -> std::expected<MidoriType::ClassConstraint, CompilerError>
											{
												return MidoriType::ClassConstraint{ std::move(equality_lhs), std::move(equality_rhs) };
											}
										);
								}
							);
					}
				);
		};

	std::function<std::expected<MidoriType::ClassConstraint, CompilerError>()> parse_constraint = [&parse_equality_constraint, this]() -> std::expected<MidoriType::ClassConstraint, CompilerError>
		{
			// A leading '~' after the identifier routes here too, so that 'T ~ Int'
			// is rejected by the rule it actually breaks rather than by a missing '<'.
			if (Check(Token::Name::IDENTIFIER_LITERAL, 0) && (Check(Token::Name::DOUBLE_COLON, 1) || Check(Token::Name::TILDE, 1)))
			{
				return parse_equality_constraint();
			}

			return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected class name in constraint.")
				.and_then
				(
					[this](Token&& first_token) -> std::expected<MidoriType::ClassConstraint, CompilerError>
					{
						Token typeclass_name = std::move(first_token);
						return Consume(Token::Name::LEFT_ANGLE, "Expected '<' after class name in constraint (e.g., 'Show<T>').")
							.and_then
							(
								[&typeclass_name, this](Token&&) -> std::expected<MidoriType::ClassConstraint, CompilerError>
								{
									return ParseDelimitedZeroOrMoreLimited<std::shared_ptr<MidoriType>>
										(
											[this]() { return ParseType(); },
											[this]() { return Consume(Token::Name::COMMA, "Expected ',' between type arguments."); },
											[this]() { return ConsumeTypeRightAngle("Expected '>' after type arguments."); }
										)
										.and_then
										(
											[&typeclass_name](std::vector<std::shared_ptr<MidoriType>>&& type_args) -> std::expected<MidoriType::ClassConstraint, CompilerError>
											{
												return MidoriType::ClassConstraint{ typeclass_name.m_lexeme, std::move(type_args) };
											}
										);
								}
							);
					}
				);
		};

	std::expected<std::vector<MidoriType::ClassConstraint>, CompilerError> constraints_result = ParseDelimitedZeroOrMoreUnlimited<MidoriType::ClassConstraint>
	(
		parse_constraint,
		[this]() { return Consume(Token::Name::COMMA, "Expected ',' between constraints."); }
	);

	if (!constraints_result.has_value())
	{
		return std::unexpected(std::move(constraints_result.error()));
	}

	std::vector<MidoriType::ClassConstraint> constraints = std::move(constraints_result.value());
	if (constraints.empty())
	{
		return std::unexpected(GenerateParserError("Expected at least one constraint after 'where' keyword.", context_token));
	}

	return constraints;
}

std::expected<std::vector<Token>, CompilerError> Parser::ParseDerivingTargets(const Token& context_token)
{
	return Consume(Token::Name::LEFT_PAREN, "Expected '(' after 'deriving'.")
		.and_then
		(
			[&context_token, this](Token&&) -> std::expected<std::vector<Token>, CompilerError>
			{
				std::unordered_set<std::string> seen_targets;
				return ParseDelimitedZeroOrMoreLimited<Token>
					(
						[&seen_targets, this]() -> MidoriResult::TokenResult
						{
							return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected deriving target.")
								.and_then
								(
									[&seen_targets, this](Token&& target) -> MidoriResult::TokenResult
									{
										if (!seen_targets.insert(target.m_lexeme).second)
										{
											return std::unexpected(GenerateParserError("Duplicate deriving target.", target));
										}

										return target;
									}
								);
						},
						[this]() { return Consume(Token::Name::COMMA, "Expected ',' after deriving target."); },
						[this]() { return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after deriving targets."); }
					)
					.and_then
					(
						[&context_token, this](std::vector<Token>&& targets) -> std::expected<std::vector<Token>, CompilerError>
						{
							if (targets.empty())
							{
								return std::unexpected(GenerateParserError("Expected at least one deriving target.", context_token));
							}

							return std::move(targets);
						}
					);
			}
		);
}

Token Parser::MakeSyntheticToken(std::string lexeme, Token::Name token_name, const Token& anchor) const
{
	return Token(std::move(lexeme), token_name, anchor);
}

std::string Parser::AppendSuffixToQualifiedName(std::string_view qualified_name, std::string_view suffix) const
{
	size_t separator_pos = qualified_name.rfind(NameSeparator);
	if (separator_pos == std::string_view::npos)
	{
		return std::string(qualified_name) + std::string(suffix);
	}

	std::string result(qualified_name.substr(0u, separator_pos + NameSeparator.length()));
	result.append(qualified_name.substr(separator_pos + NameSeparator.length()));
	result.append(suffix);
	return result;
}

MidoriResult::TokenResult Parser::RegisterSyntheticGlobalName(const std::string& name, const Token& anchor)
{
	Token synthetic_name = MakeSyntheticToken(name, Token::Name::IDENTIFIER_LITERAL, anchor);
	return DefineName(synthetic_name, true);
}

void Parser::RegisterSyntheticInstanceMetadata(const std::string& class_name, const std::vector<std::shared_ptr<MidoriType>>& type_args, const std::vector<std::string>& mangled_method_names)
{
	if (m_state.m_class_methods.contains(class_name))
	{
		m_state.m_class_instance_type_args[class_name].push_back(type_args);
		m_state.m_class_instance_associated_type_bindings[class_name].emplace_back();
	}

	for (const std::string& mangled_method_name : mangled_method_names)
	{
		std::string mangled_name_with_module = mangled_method_name;
		if (m_context.m_current_module && m_context.m_current_module->HasModuleDeclaration())
		{
			mangled_name_with_module += ModuleSeparator + m_context.m_current_module->ModuleName();
		}

		m_state.m_class_instances[class_name].push_back(std::move(mangled_name_with_module));
	}
}

MidoriResult::FunctionParamsResult Parser::ParseFunctionParameters(bool allow_inferred_types)
{
	return ParseDelimitedZeroOrMoreLimited<std::pair<Token, std::shared_ptr<MidoriType>>>
		(
			[this, allow_inferred_types]() -> MidoriResult::FunctionParamResult
			{
				return Consume(Token::Name::IDENTIFIER_LITERAL, "Expected parameter name.")
					.and_then
					(
						[this, allow_inferred_types](Token&& param_name) -> MidoriResult::FunctionParamResult
						{
							return DefineName(param_name, true)
								.and_then
								(
									[this, allow_inferred_types](Token&& param_name) -> MidoriResult::FunctionParamResult
									{
										auto finish_param = [&param_name, this](std::shared_ptr<MidoriType>&& type) -> MidoriResult::FunctionParamResult
										{
											RegisterOrUpdateLocalVariable(param_name.m_lexeme);
											return std::make_pair(std::move(param_name), std::move(type));
										};

										if (Match(Token::Name::SINGLE_COLON))
										{
											return ParseType()
												.and_then
												(
													[&finish_param](std::shared_ptr<MidoriType>&& type) -> MidoriResult::FunctionParamResult
													{
														return finish_param(std::move(type));
													}
												);
										}

										if (!allow_inferred_types)
										{
											return std::unexpected(GenerateParserError("Expected ':' after parameter name.", param_name));
										}

										return finish_param(std::shared_ptr<MidoriType>(MidoriType::MakeUndecidedType()));
									}
								);
						}
					);
			},
			[this]() { return Consume(Token::Name::COMMA, "Expected ',' after function parameter."); },
			[this]() { return Consume(Token::Name::RIGHT_PAREN, "Expected ')' after function parameters."); }
		);
}

std::expected<void, CompilerError> Parser::QueueDerivedStructStatements(const MidoriStatement::Struct& struct_stmt, const std::vector<Token>& deriving_targets)
{
	if (!IsAtGlobalScope())
	{
		return std::unexpected(GenerateParserError("Deriving declarations are only supported at global scope.", struct_stmt.m_name));
	}

	if (!struct_stmt.m_generic_params.empty())
	{
		return std::unexpected(GenerateParserError("Structural deriving for generic structs is not supported yet.", struct_stmt.m_name));
	}

	const MidoriType::StructType& struct_type = struct_stmt.m_self_type->GetType<MidoriType::StructType>();
	for (const std::shared_ptr<MidoriType>& member_type : struct_type.m_member_types)
	{
		if ((member_type->IsType<MidoriType::StructType>() && member_type->GetType<MidoriType::StructType>().m_name == struct_type.m_name)
			|| (member_type->IsType<MidoriType::UnionType>() && member_type->GetType<MidoriType::UnionType>().m_name == struct_type.m_name))
		{
			return std::unexpected(GenerateParserError("Structural deriving for recursive structs is not supported yet.", struct_stmt.m_name));
		}
	}

	auto make_local_name = [this, &struct_stmt](const std::string& name, int index) -> std::unique_ptr<MidoriExpression>
	{
		Token token = MakeSyntheticToken(name, Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name);
		return std::make_unique<MidoriExpression>(MidoriExpression::NameAccess(token, MidoriExpression::NameContext::Local{ index }));
	};

	auto make_global_name = [this, &struct_stmt](const std::string& name) -> std::unique_ptr<MidoriExpression>
	{
		Token token = MakeSyntheticToken(name, Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name);
		return std::make_unique<MidoriExpression>(MidoriExpression::NameAccess(token, MidoriExpression::NameContext::Global{}));
	};

	auto make_call = [this, &struct_stmt](std::unique_ptr<MidoriExpression>&& callee, std::vector<std::unique_ptr<MidoriExpression>>&& args) -> std::unique_ptr<MidoriExpression>
	{
		Token paren = MakeSyntheticToken("(", Token::Name::LEFT_PAREN, struct_stmt.m_name);
		return std::make_unique<MidoriExpression>(MidoriExpression::Call(paren, std::move(callee), std::move(args)));
	};

	auto make_qualified_call = [&make_call, &make_global_name](const std::string& qualifier, const std::string& method_name, std::vector<std::unique_ptr<MidoriExpression>>&& args) -> std::unique_ptr<MidoriExpression>
	{
		return make_call(make_global_name(qualifier + std::string(NameSeparator) + method_name), std::move(args));
	};

	auto make_member_access = [this, &struct_stmt, &make_local_name](const std::string& base_name, int base_index, const std::string& member_name, int member_index) -> std::unique_ptr<MidoriExpression>
	{
		Token member_token = MakeSyntheticToken(member_name, Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name);
		return std::make_unique<MidoriExpression>(MidoriExpression::MemberAccess(member_token, make_local_name(base_name, base_index), member_index));
	};

	auto make_binary = [this, &struct_stmt](Token::Name token_name, const std::string& lexeme, std::unique_ptr<MidoriExpression>&& left, std::unique_ptr<MidoriExpression>&& right) -> std::unique_ptr<MidoriExpression>
	{
		Token op = MakeSyntheticToken(lexeme, token_name, struct_stmt.m_name);
		return std::make_unique<MidoriExpression>(MidoriExpression::Binary(op, std::move(left), std::move(right)));
	};

	auto make_bool_literal = [this, &struct_stmt](bool value) -> std::unique_ptr<MidoriExpression>
	{
		return std::make_unique<MidoriExpression>(MidoriExpression::Literal(MakeSyntheticToken(value ? "true" : "false", value ? Token::Name::TRUE : Token::Name::FALSE, struct_stmt.m_name), MidoriExpression::LiteralKind::Bool));
	};

	auto make_int_literal = [this, &struct_stmt](int value) -> std::unique_ptr<MidoriExpression>
	{
		return std::make_unique<MidoriExpression>(MidoriExpression::Literal(MakeSyntheticToken(std::to_string(value), Token::Name::INTEGER_LITERAL, struct_stmt.m_name), MidoriExpression::LiteralKind::Integer));
	};

	for (const Token& derive_target : deriving_targets)
	{
		if (derive_target.m_lexeme != "Equatable" && derive_target.m_lexeme != "Hashable" && derive_target.m_lexeme != "Transferable")
		{
			return std::unexpected(GenerateParserError("Unsupported deriving target for struct.", derive_target));
		}

		std::vector<std::shared_ptr<MidoriType>> type_args{ std::shared_ptr<MidoriType>(struct_stmt.m_self_type) };
		Token class_token = MakeSyntheticToken(derive_target.m_lexeme, Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name);
		std::vector<std::unique_ptr<MidoriStatement>> methods;
		std::vector<std::string> mangled_method_names;

		if (derive_target.m_lexeme == "Transferable")
		{
			RegisterSyntheticInstanceMetadata(derive_target.m_lexeme, type_args, mangled_method_names);
			m_pending_statements.emplace(std::make_unique<MidoriStatement>(MidoriStatement::Instance(class_token, std::move(type_args), {}, {}, std::move(methods))));
			continue;
		}

		if (derive_target.m_lexeme == "Equatable")
		{
			const std::string mangled_name = MidoriType::MangleInstanceMethodName("Equals", derive_target.m_lexeme, type_args);
			Token method_token = MakeSyntheticToken(mangled_name, Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name);
			std::vector<Token> params
			{
				MakeSyntheticToken("a", Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name),
				MakeSyntheticToken("b", Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name)
			};
			std::vector<std::shared_ptr<MidoriType>> param_types{ std::shared_ptr<MidoriType>(struct_stmt.m_self_type), std::shared_ptr<MidoriType>(struct_stmt.m_self_type) };

			std::unique_ptr<MidoriExpression> body = make_bool_literal(true);
			for (size_t i = 0u; i < struct_type.m_member_types.size(); i += 1u)
			{
				std::vector<std::unique_ptr<MidoriExpression>> eq_args;
				eq_args.emplace_back(make_member_access("a", 0, struct_type.m_member_names[i], static_cast<int>(i)));
				eq_args.emplace_back(make_member_access("b", 1, struct_type.m_member_names[i], static_cast<int>(i)));

				std::unique_ptr<MidoriExpression> compare_expr = make_qualified_call("Equatable", "Equals", std::move(eq_args));
				body = make_binary(Token::Name::DOUBLE_AMPERSAND, "&&", std::move(body), std::move(compare_expr));
			}

			methods.emplace_back
			(
				std::make_unique<MidoriStatement>
				(
					MidoriStatement::FunctionDefinition(method_token, {}, std::move(params), std::move(param_types), std::shared_ptr<MidoriType>(MidoriType::MakeLiteralType<MidoriType::BoolType>()), std::move(body), std::nullopt, 0, {})
				)
			);
			mangled_method_names.emplace_back(mangled_name);
		}
		else
		{
			const std::string mangled_name = MidoriType::MangleInstanceMethodName("Hash", derive_target.m_lexeme, type_args);
			Token method_token = MakeSyntheticToken(mangled_name, Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name);
			std::vector<Token> params{ MakeSyntheticToken("value", Token::Name::IDENTIFIER_LITERAL, struct_stmt.m_name) };
			std::vector<std::shared_ptr<MidoriType>> param_types{ std::shared_ptr<MidoriType>(struct_stmt.m_self_type) };

			std::unique_ptr<MidoriExpression> body = make_int_literal(0);
			for (size_t i = 0u; i < struct_type.m_member_types.size(); i += 1u)
			{
				std::vector<std::unique_ptr<MidoriExpression>> hash_args;
				hash_args.emplace_back(make_member_access("value", 0, struct_type.m_member_names[i], static_cast<int>(i)));

				std::unique_ptr<MidoriExpression> member_hash = make_qualified_call("Hashable", "Hash", std::move(hash_args));
				std::unique_ptr<MidoriExpression> scaled = make_binary(Token::Name::STAR, "*", std::move(body), make_int_literal(31));
				body = make_binary(Token::Name::SINGLE_PLUS, "+", std::move(scaled), std::move(member_hash));
			}

			methods.emplace_back
			(
				std::make_unique<MidoriStatement>
				(
					MidoriStatement::FunctionDefinition(method_token, {}, std::move(params), std::move(param_types), std::shared_ptr<MidoriType>(MidoriType::MakeLiteralType<MidoriType::IntegerType>()), std::move(body), std::nullopt, 0, {})
				)
			);
			mangled_method_names.emplace_back(mangled_name);
		}

		RegisterSyntheticInstanceMetadata(derive_target.m_lexeme, type_args, mangled_method_names);
		m_pending_statements.emplace(std::make_unique<MidoriStatement>(MidoriStatement::Instance(class_token, std::move(type_args), {}, {}, std::move(methods))));
	}

	return {};
}

std::expected<void, CompilerError> Parser::QueueDerivedUnionStatements(const MidoriStatement::Union& union_stmt, const std::vector<Token>& deriving_targets)
{
	if (!IsAtGlobalScope())
	{
		return std::unexpected(GenerateParserError("Deriving declarations are only supported at global scope.", union_stmt.m_name));
	}

	const MidoriType::UnionType& union_type = union_stmt.m_self_type->GetType<MidoriType::UnionType>();

	auto make_local_name = [this, &union_stmt](const std::string& name, int index) -> std::unique_ptr<MidoriExpression>
	{
		Token token = MakeSyntheticToken(name, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
		return std::make_unique<MidoriExpression>(MidoriExpression::NameAccess(token, MidoriExpression::NameContext::Local{ index }));
	};

	auto make_global_name = [this, &union_stmt](const std::string& name) -> std::unique_ptr<MidoriExpression>
	{
		Token token = MakeSyntheticToken(name, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
		return std::make_unique<MidoriExpression>(MidoriExpression::NameAccess(token, MidoriExpression::NameContext::Global{}));
	};

	auto make_call = [this, &union_stmt](std::unique_ptr<MidoriExpression>&& callee, std::vector<std::unique_ptr<MidoriExpression>>&& args) -> std::unique_ptr<MidoriExpression>
	{
		Token paren = MakeSyntheticToken("(", Token::Name::LEFT_PAREN, union_stmt.m_name);
		return std::make_unique<MidoriExpression>(MidoriExpression::Call(paren, std::move(callee), std::move(args)));
	};

	auto make_qualified_call = [&make_call, &make_global_name](const std::string& qualifier, const std::string& method_name, std::vector<std::unique_ptr<MidoriExpression>>&& args) -> std::unique_ptr<MidoriExpression>
	{
		return make_call(make_global_name(qualifier + std::string(NameSeparator) + method_name), std::move(args));
	};

	auto make_binary = [this, &union_stmt](Token::Name token_name, const std::string& lexeme, std::unique_ptr<MidoriExpression>&& left, std::unique_ptr<MidoriExpression>&& right) -> std::unique_ptr<MidoriExpression>
	{
		Token op = MakeSyntheticToken(lexeme, token_name, union_stmt.m_name);
		return std::make_unique<MidoriExpression>(MidoriExpression::Binary(op, std::move(left), std::move(right)));
	};

	auto make_bool_literal = [this, &union_stmt](bool value) -> std::unique_ptr<MidoriExpression>
	{
		return std::make_unique<MidoriExpression>(MidoriExpression::Literal(MakeSyntheticToken(value ? "true" : "false", value ? Token::Name::TRUE : Token::Name::FALSE, union_stmt.m_name), MidoriExpression::LiteralKind::Bool));
	};

	auto make_int_literal = [this, &union_stmt](int value) -> std::unique_ptr<MidoriExpression>
	{
		return std::make_unique<MidoriExpression>(MidoriExpression::Literal(MakeSyntheticToken(std::to_string(value), Token::Name::INTEGER_LITERAL, union_stmt.m_name), MidoriExpression::LiteralKind::Integer));
	};

	auto make_binding_pattern = [this, &union_stmt](const std::string& name, int local_index) -> std::unique_ptr<MidoriPattern>
	{
		Token token = MakeSyntheticToken(name, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
		return std::make_unique<MidoriPattern>(MidoriPattern::Binding(token, local_index));
	};

	auto make_constructor_pattern = [this, &union_stmt, &make_binding_pattern](const std::string& ctor_name, int first_local_index, size_t field_count, const std::string& prefix) -> std::unique_ptr<MidoriPattern>
	{
		Token ctor_token = MakeSyntheticToken(ctor_name, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
		std::vector<std::unique_ptr<MidoriPattern>> args;
		for (size_t i = 0u; i < field_count; i += 1u)
		{
			args.emplace_back(make_binding_pattern(prefix + std::to_string(i), first_local_index + static_cast<int>(i)));
		}
		return std::make_unique<MidoriPattern>(MidoriPattern::Constructor(ctor_token, std::string(ctor_name), std::move(args), true));
	};

	auto make_case = [this, &union_stmt](std::unique_ptr<MidoriPattern>&& pattern, std::unique_ptr<MidoriExpression>&& expr, int binding_count) -> std::unique_ptr<MidoriExpression>
	{
		Token case_token = MakeSyntheticToken("case", Token::Name::CASE, union_stmt.m_name);
		return std::make_unique<MidoriExpression>(MidoriExpression::Case(case_token, std::move(pattern), std::move(expr), binding_count));
	};

	auto make_default_case = [this, &union_stmt](std::unique_ptr<MidoriExpression>&& expr) -> std::unique_ptr<MidoriExpression>
	{
		Token case_token = MakeSyntheticToken("case", Token::Name::CASE, union_stmt.m_name);
		Token wildcard_token = MakeSyntheticToken("_", Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
		std::unique_ptr<MidoriPattern> wildcard = std::make_unique<MidoriPattern>(MidoriPattern::Wildcard(wildcard_token));
		return std::make_unique<MidoriExpression>(MidoriExpression::Case(case_token, std::move(wildcard), std::move(expr), 0));
	};

	auto make_match = [this, &union_stmt](std::unique_ptr<MidoriExpression>&& scrutinee, int hidden_index, std::vector<std::unique_ptr<MidoriExpression>>&& cases) -> std::unique_ptr<MidoriExpression>
	{
		Token match_token = MakeSyntheticToken("match", Token::Name::MATCH, union_stmt.m_name);
		std::unique_ptr<MidoriExpression> match_expr = std::make_unique<MidoriExpression>(MidoriExpression::Match(match_token, std::move(scrutinee), std::move(cases)));
		match_expr->GetExpression<MidoriExpression::Match>().m_match_value_index = hidden_index;
		return match_expr;
	};

	auto make_union_construct = [this, &union_stmt](const std::string& ctor_name, int tag, std::vector<std::unique_ptr<MidoriExpression>>&& args, const std::shared_ptr<MidoriType>& return_type) -> std::unique_ptr<MidoriExpression>
	{
		Token ctor_token = MakeSyntheticToken(ctor_name, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
		return std::make_unique<MidoriExpression>(MidoriExpression::Construct(ctor_token, std::move(args), std::shared_ptr<MidoriType>(return_type), true, MidoriExpression::Construct::Union(tag)));
	};

	for (const Token& derive_target : deriving_targets)
	{
		const bool is_structural = derive_target.m_lexeme == "Equatable" || derive_target.m_lexeme == "Hashable";
		const bool is_transferable = derive_target.m_lexeme == "Transferable";
		const bool is_container = derive_target.m_lexeme == "Map" || derive_target.m_lexeme == "Bind" || derive_target.m_lexeme == "Unwrap";
		if (!is_structural && !is_transferable && !is_container)
		{
			return std::unexpected(GenerateParserError("Unsupported deriving target for union.", derive_target));
		}

		if (is_transferable)
		{
			std::vector<std::shared_ptr<MidoriType>> type_args{ std::shared_ptr<MidoriType>(union_stmt.m_self_type) };
			Token class_token = MakeSyntheticToken(derive_target.m_lexeme, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
			std::vector<std::unique_ptr<MidoriStatement>> methods;
			std::vector<std::string> mangled_method_names;
			RegisterSyntheticInstanceMetadata(derive_target.m_lexeme, type_args, mangled_method_names);
			m_pending_statements.emplace(std::make_unique<MidoriStatement>(MidoriStatement::Instance(class_token, std::move(type_args), {}, {}, std::move(methods))));
			continue;
		}

		if (is_structural)
		{
			if (!union_stmt.m_generic_params.empty())
			{
				return std::unexpected(GenerateParserError("Structural deriving for generic unions is not supported yet.", union_stmt.m_name));
			}

			for (const auto& [member_name, member_ctx] : union_type.m_member_info)
			{
				for (const std::shared_ptr<MidoriType>& member_type : member_ctx.m_member_types)
				{
					if (IsUnionSelfReference(member_type, union_type.m_name))
					{
						return std::unexpected(GenerateParserError("Structural deriving for recursive unions is not supported yet.", union_stmt.m_name));
					}
				}
			}

			std::vector<std::shared_ptr<MidoriType>> type_args{ std::shared_ptr<MidoriType>(union_stmt.m_self_type) };
			Token class_token = MakeSyntheticToken(derive_target.m_lexeme, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
			std::vector<std::unique_ptr<MidoriStatement>> methods;
			std::vector<std::string> mangled_method_names;

			if (derive_target.m_lexeme == "Equatable")
			{
				const std::string mangled_name = MidoriType::MangleInstanceMethodName("Equals", derive_target.m_lexeme, type_args);
				Token method_token = MakeSyntheticToken(mangled_name, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
				std::vector<Token> params
				{
					MakeSyntheticToken("a", Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name),
					MakeSyntheticToken("b", Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name)
				};
				std::vector<std::shared_ptr<MidoriType>> param_types{ std::shared_ptr<MidoriType>(union_stmt.m_self_type), std::shared_ptr<MidoriType>(union_stmt.m_self_type) };

				std::vector<std::unique_ptr<MidoriExpression>> outer_cases;
				for (const Token& ctor_name : union_stmt.m_constructor_names)
				{
					const MidoriType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(ctor_name.m_lexeme);
					const int outer_binding_start = 3;
					const int inner_match_hidden = outer_binding_start + static_cast<int>(member_ctx.m_member_types.size());

					std::vector<std::unique_ptr<MidoriExpression>> inner_cases;
					std::unique_ptr<MidoriExpression> inner_body = make_bool_literal(true);
					for (size_t i = 0u; i < member_ctx.m_member_types.size(); i += 1u)
					{
						std::vector<std::unique_ptr<MidoriExpression>> eq_args;
						eq_args.emplace_back(make_local_name("lhs" + std::to_string(i), outer_binding_start + static_cast<int>(i)));
						eq_args.emplace_back(make_local_name("rhs" + std::to_string(i), inner_match_hidden + 1 + static_cast<int>(i)));

						std::unique_ptr<MidoriExpression> compare_expr = make_qualified_call("Equatable", "Equals", std::move(eq_args));
						inner_body = make_binary(Token::Name::DOUBLE_AMPERSAND, "&&", std::move(inner_body), std::move(compare_expr));
					}

					inner_cases.emplace_back
					(
						make_case
						(
							make_constructor_pattern(ctor_name.m_lexeme, inner_match_hidden + 1, member_ctx.m_member_types.size(), "rhs"),
							std::move(inner_body),
							static_cast<int>(member_ctx.m_member_types.size())
						)
					);
					inner_cases.emplace_back(make_default_case(make_bool_literal(false)));

					outer_cases.emplace_back
					(
						make_case
						(
							make_constructor_pattern(ctor_name.m_lexeme, outer_binding_start, member_ctx.m_member_types.size(), "lhs"),
							make_match(make_local_name("b", 1), inner_match_hidden, std::move(inner_cases)),
							static_cast<int>(member_ctx.m_member_types.size())
						)
					);
				}

				std::unique_ptr<MidoriExpression> body = make_match(make_local_name("a", 0), 2, std::move(outer_cases));
				methods.emplace_back
				(
					std::make_unique<MidoriStatement>
					(
						MidoriStatement::FunctionDefinition(method_token, {}, std::move(params), std::move(param_types), std::shared_ptr<MidoriType>(MidoriType::MakeLiteralType<MidoriType::BoolType>()), std::move(body), std::nullopt, 0, {})
					)
				);
				mangled_method_names.emplace_back(mangled_name);
			}
			else
			{
				const std::string mangled_name = MidoriType::MangleInstanceMethodName("Hash", derive_target.m_lexeme, type_args);
				Token method_token = MakeSyntheticToken(mangled_name, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name);
				std::vector<Token> params{ MakeSyntheticToken("value", Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name) };
				std::vector<std::shared_ptr<MidoriType>> param_types{ std::shared_ptr<MidoriType>(union_stmt.m_self_type) };

				std::vector<std::unique_ptr<MidoriExpression>> cases;
				for (const Token& ctor_name : union_stmt.m_constructor_names)
				{
					const MidoriType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(ctor_name.m_lexeme);
					std::unique_ptr<MidoriExpression> case_body = make_int_literal(member_ctx.m_tag);
					for (size_t i = 0u; i < member_ctx.m_member_types.size(); i += 1u)
					{
						std::vector<std::unique_ptr<MidoriExpression>> hash_args;
						hash_args.emplace_back(make_local_name("field" + std::to_string(i), 2 + static_cast<int>(i)));

						std::unique_ptr<MidoriExpression> member_hash = make_qualified_call("Hashable", "Hash", std::move(hash_args));
						std::unique_ptr<MidoriExpression> scaled = make_binary(Token::Name::STAR, "*", std::move(case_body), make_int_literal(31));
						case_body = make_binary(Token::Name::SINGLE_PLUS, "+", std::move(scaled), std::move(member_hash));
					}

					cases.emplace_back
					(
						make_case
						(
							make_constructor_pattern(ctor_name.m_lexeme, 2, member_ctx.m_member_types.size(), "field"),
							std::move(case_body),
							static_cast<int>(member_ctx.m_member_types.size())
						)
					);
				}

				std::unique_ptr<MidoriExpression> body = make_match(make_local_name("value", 0), 1, std::move(cases));
				methods.emplace_back
				(
					std::make_unique<MidoriStatement>
					(
						MidoriStatement::FunctionDefinition(method_token, {}, std::move(params), std::move(param_types), std::shared_ptr<MidoriType>(MidoriType::MakeLiteralType<MidoriType::IntegerType>()), std::move(body), std::nullopt, 0, {})
					)
				);
				mangled_method_names.emplace_back(mangled_name);
			}

			RegisterSyntheticInstanceMetadata(derive_target.m_lexeme, type_args, mangled_method_names);
			m_pending_statements.emplace(std::make_unique<MidoriStatement>(MidoriStatement::Instance(class_token, std::move(type_args), {}, {}, std::move(methods))));
			continue;
		}

		if (union_type.m_generic_params.empty())
		{
			return std::unexpected(GenerateParserError("Container deriving requires at least one union type parameter.", union_stmt.m_name));
		}

		const std::string& first_type_param = union_type.m_generic_params[0u];
		std::string mapped_type_param_name = "B";
		while (std::ranges::any_of(union_stmt.m_generic_params, [&mapped_type_param_name](const Token& token) { return token.m_lexeme == mapped_type_param_name; }))
		{
			mapped_type_param_name.append("Result");
		}

		std::vector<Token> generic_params;
		generic_params.reserve(union_stmt.m_generic_params.size() + 1u);
		generic_params.push_back(union_stmt.m_generic_params[0u]);
		if (derive_target.m_lexeme != "Unwrap")
		{
			generic_params.push_back(MakeSyntheticToken(mapped_type_param_name, Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name));
		}
		for (size_t i = 1u; i < union_stmt.m_generic_params.size(); i += 1u)
		{
			generic_params.push_back(union_stmt.m_generic_params[i]);
		}

		MidoriResult::TokenResult function_name_result = RegisterSyntheticGlobalName(AppendSuffixToQualifiedName(union_stmt.m_name.m_lexeme, derive_target.m_lexeme), union_stmt.m_name);
		if (!function_name_result.has_value())
		{
			return std::unexpected(std::move(function_name_result.error()));
		}
		Token function_name = std::move(function_name_result.value());

		std::shared_ptr<MidoriType> input_union_type = std::shared_ptr<MidoriType>(union_stmt.m_self_type);
		std::shared_ptr<MidoriType> mapped_type = MidoriType::MakeGenericType(mapped_type_param_name);
		std::shared_ptr<MidoriType> source_type = MidoriType::MakeGenericType(first_type_param);
		std::unordered_map<std::string, std::shared_ptr<MidoriType>> output_substitutions;
		output_substitutions.emplace(first_type_param, mapped_type);
		std::shared_ptr<MidoriType> output_union_type = MidoriType::SubstituteTypeParams(union_stmt.m_self_type, output_substitutions);

		std::vector<Token> params;
		std::vector<std::shared_ptr<MidoriType>> param_types;
		params.emplace_back(MakeSyntheticToken("value", Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name));
		param_types.emplace_back(input_union_type);

		if (derive_target.m_lexeme == "Map")
		{
			params.emplace_back(MakeSyntheticToken("f", Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name));
			param_types.emplace_back(MidoriType::MakeFunctionType(std::vector<std::shared_ptr<MidoriType>>{ source_type }, std::shared_ptr<MidoriType>(mapped_type)));
			const int match_value_local_index = static_cast<int>(params.size());
			const int case_binding_base_index = match_value_local_index + 1;

			std::vector<std::unique_ptr<MidoriExpression>> cases;
			for (const Token& ctor_name : union_stmt.m_constructor_names)
			{
				const MidoriType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(ctor_name.m_lexeme);
				std::vector<std::unique_ptr<MidoriExpression>> ctor_args;
				for (size_t i = 0u; i < member_ctx.m_member_types.size(); i += 1u)
				{
					const std::shared_ptr<MidoriType>& member_type = member_ctx.m_member_types[i];
					const int local_index = case_binding_base_index + static_cast<int>(i);
					const std::string binding_name = "field" + std::to_string(i);

					if (IsExactGenericParam(member_type, first_type_param))
					{
						std::vector<std::unique_ptr<MidoriExpression>> call_args;
						call_args.emplace_back(make_local_name(binding_name, local_index));
						ctor_args.emplace_back(make_call(make_local_name("f", 1), std::move(call_args)));
					}
					else if (IsUnionSelfReference(member_type, union_type.m_name))
					{
						std::vector<std::unique_ptr<MidoriExpression>> call_args;
						call_args.emplace_back(make_local_name(binding_name, local_index));
						call_args.emplace_back(make_local_name("f", 1));
						ctor_args.emplace_back(make_call(make_global_name(function_name.m_lexeme), std::move(call_args)));
					}
					else
					{
						if (ContainsGenericParam(member_type, first_type_param))
						{
							return std::unexpected(GenerateParserError("Map deriving only supports direct occurrences of the first type parameter or recursive self fields.", union_stmt.m_name));
						}
						ctor_args.emplace_back(make_local_name(binding_name, local_index));
					}
				}

				cases.emplace_back
				(
						make_case
						(
							make_constructor_pattern(ctor_name.m_lexeme, case_binding_base_index, member_ctx.m_member_types.size(), "field"),
							make_union_construct(ctor_name.m_lexeme, member_ctx.m_tag, std::move(ctor_args), output_union_type),
							static_cast<int>(member_ctx.m_member_types.size())
						)
					);
			}

			std::unique_ptr<MidoriExpression> body = make_match(make_local_name("value", 0), match_value_local_index, std::move(cases));
			m_pending_statements.emplace
			(
				std::make_unique<MidoriStatement>
				(
					MidoriStatement::FunctionDefinition(function_name, std::move(generic_params), std::move(params), std::move(param_types), std::move(output_union_type), std::move(body), std::nullopt, 0, {})
				)
			);
			continue;
		}

		if (derive_target.m_lexeme == "Bind")
		{
			params.emplace_back(MakeSyntheticToken("f", Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name));
			param_types.emplace_back(MidoriType::MakeFunctionType(std::vector<std::shared_ptr<MidoriType>>{ source_type }, std::shared_ptr<MidoriType>(output_union_type)));
			const int match_value_local_index = static_cast<int>(params.size());
			const int case_binding_base_index = match_value_local_index + 1;

			std::vector<std::unique_ptr<MidoriExpression>> cases;
			for (const Token& ctor_name : union_stmt.m_constructor_names)
			{
				const MidoriType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(ctor_name.m_lexeme);
				int mapped_field_index = -1;
				bool has_recursive_field = false;
				bool unsupported_nested_generic = false;

				for (size_t i = 0u; i < member_ctx.m_member_types.size(); i += 1u)
				{
					const std::shared_ptr<MidoriType>& member_type = member_ctx.m_member_types[i];
					if (IsExactGenericParam(member_type, first_type_param))
					{
						if (mapped_field_index != -1)
						{
							return std::unexpected(GenerateParserError("Bind deriving requires variants to contain at most one direct field of the first type parameter.", union_stmt.m_name));
						}
						mapped_field_index = static_cast<int>(i);
					}
					else if (IsUnionSelfReference(member_type, union_type.m_name))
					{
						has_recursive_field = true;
					}
					else if (ContainsGenericParam(member_type, first_type_param))
					{
						unsupported_nested_generic = true;
					}
				}

				if (has_recursive_field || unsupported_nested_generic || (mapped_field_index >= 0 && member_ctx.m_member_types.size() != 1u))
				{
					return std::unexpected(GenerateParserError("Bind deriving only supports pass-through variants or single-value variants.", union_stmt.m_name));
				}

				std::unique_ptr<MidoriExpression> case_body;
				if (mapped_field_index >= 0)
				{
					std::vector<std::unique_ptr<MidoriExpression>> call_args;
					call_args.emplace_back(make_local_name("field" + std::to_string(mapped_field_index), case_binding_base_index + mapped_field_index));
					case_body = make_call(make_local_name("f", 1), std::move(call_args));
				}
				else
				{
					std::vector<std::unique_ptr<MidoriExpression>> ctor_args;
					for (size_t i = 0u; i < member_ctx.m_member_types.size(); i += 1u)
					{
						ctor_args.emplace_back(make_local_name("field" + std::to_string(i), case_binding_base_index + static_cast<int>(i)));
					}
					case_body = make_union_construct(ctor_name.m_lexeme, member_ctx.m_tag, std::move(ctor_args), output_union_type);
				}

				cases.emplace_back
				(
						make_case
						(
							make_constructor_pattern(ctor_name.m_lexeme, case_binding_base_index, member_ctx.m_member_types.size(), "field"),
							std::move(case_body),
							static_cast<int>(member_ctx.m_member_types.size())
						)
					);
			}

			std::unique_ptr<MidoriExpression> body = make_match(make_local_name("value", 0), match_value_local_index, std::move(cases));
			m_pending_statements.emplace
			(
				std::make_unique<MidoriStatement>
				(
					MidoriStatement::FunctionDefinition(function_name, std::move(generic_params), std::move(params), std::move(param_types), std::move(output_union_type), std::move(body), std::nullopt, 0, {})
				)
			);
			continue;
		}

		params.emplace_back(MakeSyntheticToken("default_value", Token::Name::IDENTIFIER_LITERAL, union_stmt.m_name));
		param_types.emplace_back(source_type);
		const int match_value_local_index = static_cast<int>(params.size());
		const int case_binding_base_index = match_value_local_index + 1;

		std::vector<std::unique_ptr<MidoriExpression>> cases;
		for (const Token& ctor_name : union_stmt.m_constructor_names)
		{
			const MidoriType::UnionType::UnionMemberContext& member_ctx = union_type.m_member_info.at(ctor_name.m_lexeme);
			int mapped_field_index = -1;
			bool has_recursive_field = false;
			bool unsupported_nested_generic = false;

			for (size_t i = 0u; i < member_ctx.m_member_types.size(); i += 1u)
			{
				const std::shared_ptr<MidoriType>& member_type = member_ctx.m_member_types[i];
				if (IsExactGenericParam(member_type, first_type_param))
				{
					if (mapped_field_index != -1)
					{
						return std::unexpected(GenerateParserError("Unwrap deriving requires variants to contain at most one direct field of the first type parameter.", union_stmt.m_name));
					}
					mapped_field_index = static_cast<int>(i);
				}
				else if (IsUnionSelfReference(member_type, union_type.m_name))
				{
					has_recursive_field = true;
				}
				else if (ContainsGenericParam(member_type, first_type_param))
				{
					unsupported_nested_generic = true;
				}
			}

			if (has_recursive_field || unsupported_nested_generic || (mapped_field_index >= 0 && member_ctx.m_member_types.size() != 1u))
			{
				return std::unexpected(GenerateParserError("Unwrap deriving only supports pass-through variants or single-value variants.", union_stmt.m_name));
			}

			std::unique_ptr<MidoriExpression> case_body =
				mapped_field_index >= 0
				? make_local_name("field" + std::to_string(mapped_field_index), case_binding_base_index + mapped_field_index)
				: make_local_name("default_value", 1);

			cases.emplace_back
			(
				make_case
				(
					make_constructor_pattern(ctor_name.m_lexeme, case_binding_base_index, member_ctx.m_member_types.size(), "field"),
					std::move(case_body),
					static_cast<int>(member_ctx.m_member_types.size())
				)
			);
		}

		std::unique_ptr<MidoriExpression> body = make_match(make_local_name("value", 0), match_value_local_index, std::move(cases));
		m_pending_statements.emplace
		(
			std::make_unique<MidoriStatement>
			(
				MidoriStatement::FunctionDefinition(function_name, std::move(generic_params), std::move(params), std::move(param_types), std::move(source_type), std::move(body), std::nullopt, 0, {})
			)
		);
	}

	return {};
}

Parser& Parser::Synchronize() &
{
	while (!IsAtEnd())
	{
		switch (Peek(0).m_token_name)
		{
			case Token::Name::DEF:
			case Token::Name::CLASS:
			case Token::Name::INSTANCE:
			case Token::Name::FOREIGN:
			case Token::Name::TYPE:
			case Token::Name::ALIAS:
				return *this;
			default:
				Advance();
				break;
		}
	}

	return *this;
}

Parser&& Parser::Synchronize() &&
{
	Synchronize();
	return std::move(*this);
}

Parser::VariableContext::VariableContext(int relative_index, int absolute_index, int function_depth)
	: m_relative_index(relative_index),
	m_absolute_index(absolute_index),
	m_function_depth(function_depth)
{
}

const Parser::TypeclassMethodMap& Parser::GetTypeclassMethods() const
{
	return m_state.m_class_methods;
}

const std::vector<CompilerWarning>& Parser::GetWarnings() const
{
	return m_warnings;
}

CompiledModule::TypeclassMetadataMap Parser::GetTypeclassMetadata() const
{
	CompiledModule::TypeclassMetadataMap result;
	for (const auto& [tc_name, type_params] : m_state.m_typeclass_type_params)
	{
		CompiledModule::TypeclassMetadata metadata;
		if (m_state.m_class_methods.contains(tc_name))
		{
			metadata.m_method_names = m_state.m_class_methods.at(tc_name);
		}
		if (m_state.m_typeclass_type_params.contains(tc_name))
		{
			metadata.m_type_param_names = m_state.m_typeclass_type_params.at(tc_name);
		}
		if (m_state.m_typeclass_associated_types.contains(tc_name))
		{
			metadata.m_associated_type_names = m_state.m_typeclass_associated_types.at(tc_name);
		}
		if (m_state.m_class_instances.contains(tc_name))
		{
			metadata.m_instance_methods = m_state.m_class_instances.at(tc_name);
		}
		if (m_state.m_class_instance_type_args.contains(tc_name))
		{
			metadata.m_instance_type_args = m_state.m_class_instance_type_args.at(tc_name);
		}
		if (m_state.m_class_instance_associated_type_bindings.contains(tc_name))
		{
			metadata.m_instance_associated_type_bindings = m_state.m_class_instance_associated_type_bindings.at(tc_name);
		}
		if (m_state.m_typeclass_method_types.contains(tc_name))
		{
			metadata.m_method_types = m_state.m_typeclass_method_types.at(tc_name);
		}
		result[tc_name] = std::move(metadata);
	}
	return result;
}

bool Parser::IsModuleVisible(std::string_view module_name) const
{
	return m_context.m_imported_symbols.contains(std::string(module_name))
		|| (m_context.m_current_module != nullptr && m_context.m_current_module->ModuleName() == module_name);
}

MidoriResult::TypeResult Parser::ParseCellTypeArguments()
{
	return Consume(Token::Name::LEFT_ANGLE, "Expected '<' after 'Cell'.")
		.and_then
		(
			[this](Token&&) -> MidoriResult::TypeResult
			{
				return ParseType()
					.and_then
					(
						[this](std::shared_ptr<MidoriType>&& element_type) -> MidoriResult::TypeResult
						{
							return ConsumeTypeRightAngle("Expected '>' after cell element type.")
								.and_then
								(
									[&element_type](Token&&) -> MidoriResult::TypeResult
									{
										return MidoriType::MakeCellType(element_type);
									}
								);
						}
					);
			}
		);
}
