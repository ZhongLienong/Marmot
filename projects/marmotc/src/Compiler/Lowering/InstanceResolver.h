#pragma once

#include "Compiler/Error/CompilerError.h"
#include "Compiler/AbstractSyntaxTree/Type.h"
#include "Compiler/Lowering/GenericTypes.h"

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Why a class method could not be resolved. The caller reports it under its
// own stage.
struct MethodResolutionError
{
	CompilerErrorCode m_code;
	std::string m_message;

	MethodResolutionError(CompilerErrorCode code, std::string message);
};

template<typename T>
using MethodResolution = std::expected<T, MethodResolutionError>;

// One class constraint of a specialized generic, resolved to the instance
// method its concrete type arguments select.
struct ResolvedMethodCandidate
{
	std::string m_first_type_name;
	std::string m_second_type_name;
	std::string m_resolved_name;
	bool m_has_instance;

	ResolvedMethodCandidate(std::string first_type_name, std::string second_type_name, std::string resolved_name, bool has_instance);
};

// `Class::Method` to the candidates the constraints in scope give it.
using MethodResolutionMap = std::unordered_map<std::string, std::vector<ResolvedMethodCandidate>>;

// A resolved instance method: `name`, a global of this module, or
// `name@Module`, one of Module's.
struct ResolvedInstanceName
{
	std::string m_symbol;
	std::optional<std::string> m_module;

	explicit ResolvedInstanceName(const std::string& resolved_name);
};

// Which instance method a class method call, an operator a class provides or a
// `for` over an Iterable runs. The one rule both lowering and the code
// generator resolve by.
class InstanceResolver
{
public:
	using ClassMethods = std::unordered_map<std::string, std::unordered_set<std::string>>;
	using ClassInstances = std::unordered_map<std::string, std::vector<std::string>>;
	using ClassInstanceTypes = std::unordered_map<std::string, std::vector<std::vector<std::shared_ptr<MidoriType>>>>;
	using AssociatedTypeBindings = std::unordered_map<std::string, std::shared_ptr<MidoriType>>;
	// Each class's instances' associated type bindings, in the order of their
	// type arguments in ClassInstanceTypes.
	using ClassInstanceAssociatedTypes = std::unordered_map<std::string, std::vector<AssociatedTypeBindings>>;
	using IsModuleGlobal = std::function<bool(const std::string&)>;

private:
	ClassMethods m_class_methods;
	ClassInstances m_class_instances;
	ClassInstanceTypes m_class_instance_type_args;
	std::unordered_map<std::string, std::vector<std::pair<std::vector<std::shared_ptr<MidoriType>>, AssociatedTypeBindings>>> m_associated_types;
	IsModuleGlobal m_is_module_global;

public:
	InstanceResolver(ClassMethods class_methods, ClassInstances class_instances, ClassInstanceTypes class_instance_type_args, const ClassInstanceAssociatedTypes& associated_types, IsModuleGlobal is_module_global);

	void AddClass(const std::string& class_name, std::unordered_set<std::string> method_names);
	void AddInstanceTypeArgs(const std::string& class_name, const std::vector<std::shared_ptr<MidoriType>>& type_args);
	void AddInstanceMethod(const std::string& class_name, const std::string& method_name);
	void AddAssociatedTypes(const std::string& class_name, const std::vector<std::shared_ptr<MidoriType>>& type_args, AssociatedTypeBindings bindings);

	bool IsClassMethod(const std::string& qualified_name) const;

	std::optional<std::string> ResolveInstanceName(const std::string& class_name, const std::string& base_name) const;
	// What the instance its concrete type arguments select binds a projection to.
	std::optional<std::shared_ptr<MidoriType>> ResolveAssociatedType(const MidoriType::AssociatedType& projection) const;
	std::optional<std::string> ResolveInstanceNameForTypeArgs(const std::string& class_name, const std::string& method_name, const std::vector<std::shared_ptr<MidoriType>>& concrete_type_args) const;

	// The candidates a specialization's constraints give each of their
	// classes' methods, once `substitutions` makes their type arguments concrete.
	MethodResolutionMap ResolveConstraints(const std::vector<MidoriType::ClassConstraint>& constraints, const GenericTypes::TypeEnvironment& substitutions) const;

	// A call of a method a constraint in scope provides.
	MethodResolution<std::string> ResolveConstrainedCall(const MethodResolutionMap& resolutions, const std::string& callee_name, const std::shared_ptr<MidoriType>* first_argument_type, const std::shared_ptr<MidoriType>& return_type) const;
	// A method a constraint in scope provides, used as a value.
	MethodResolution<std::string> ResolveConstrainedValue(const MethodResolutionMap& resolutions, const std::string& name) const;
	// A call of `Class::Method` with concrete arguments; nothing when the name
	// is no class method.
	MethodResolution<std::optional<std::string>> ResolveConcreteCall(const std::string& callee_name, const std::vector<std::shared_ptr<MidoriType>>& argument_types, const std::shared_ptr<MidoriType>& return_type) const;

	MethodResolution<std::string> ResolveConcat(const MethodResolutionMap& resolutions, const std::shared_ptr<MidoriType>& operand_type) const;
	MethodResolution<std::string> ResolveEquals(const std::shared_ptr<MidoriType>& operand_type) const;
	MethodResolution<std::string> ResolveCompare(const std::shared_ptr<MidoriType>& operand_type) const;
	MethodResolution<std::string> ResolveOperandInstance(const std::string& class_name, const std::string& method_name, const std::shared_ptr<MidoriType>& operand_type) const;
	// Nothing when `#` does not need Countable.
	MethodResolution<std::optional<std::string>> ResolveCount(const MethodResolutionMap& resolutions, const std::shared_ptr<MidoriType>& operand_type, bool uses_countable) const;
	MethodResolution<std::string> ResolveIndex(const MethodResolutionMap& resolutions, const std::shared_ptr<MidoriType>& container_type, const std::shared_ptr<MidoriType>& index_type) const;
	// Nothing when the conversion is a builtin one or erases a newtype. A
	// constraint in scope is looked up by `concrete_from_type`, the operand's
	// type once the specialization being emitted substitutes it.
	MethodResolution<std::optional<std::string>> ResolveConvert(const MethodResolutionMap& resolutions, const std::shared_ptr<MidoriType>& concrete_from_type, const std::shared_ptr<MidoriType>& from_type, const std::shared_ptr<MidoriType>& to_type, bool uses_convertable) const;
	MethodResolution<std::string> ResolveIterableNext(const MethodResolutionMap& resolutions, const std::shared_ptr<MidoriType>& iterator_type, const std::shared_ptr<MidoriType>& item_type) const;

private:
	// The one instance of `class_name` whose type arguments match, through
	// `matches`; an error names `description` when two do.
	MethodResolution<std::optional<std::string>> MatchSingleInstance(const std::string& class_name, const std::string& method_name, const std::function<bool(const std::vector<std::shared_ptr<MidoriType>>&, GenericTypes::TypeEnvironment&)>& matches, const std::string& ambiguity_message) const;
};
