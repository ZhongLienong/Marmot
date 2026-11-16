#pragma once

#include "Compiler/AbstractSyntaxTree/Type.h"

namespace TypeTransform
{
	using TypeRef = std::shared_ptr<MarmotType>;
	using Parameters = std::unordered_map<std::string, TypeRef>;
	using Variables = std::unordered_map<int, TypeRef>;
	using ProjectionResolver = std::function<std::optional<TypeRef>(const MarmotType::AssociatedType&)>;

	// Instantiation consumes nominal declarations' parameters, including phantom
	// parameters. Newtypes retain their declaration parameters for representation checks.
	TypeRef Instantiate(const TypeRef& type, const Parameters& parameters);
	void InstantiateInto(const TypeRef& target, const TypeRef& type, const Parameters& parameters);

	// Inference follows variable bindings transitively and shares unchanged types.
	TypeRef Apply(const TypeRef& type, const Variables& variables, const ProjectionResolver& resolve);

	// Lowering replaces names and rendered variable IDs once, then resolves projections.
	TypeRef Specialize(const TypeRef& type, const Parameters& parameters, const ProjectionResolver& resolve = {});

	void ForEachChild(const MarmotType& type, const std::function<void(const TypeRef&)>& visit);

	class Freshener
	{
	public:
		explicit Freshener(std::function<TypeRef()> fresh_variable);

		TypeRef Apply(const TypeRef& type);
		TypeRef Parameter(const std::string& name);
		void KeepParameter(const std::string& name, TypeRef variable);
		const Parameters& Bindings() const;

	private:
		std::function<TypeRef()> m_fresh_variable;
		Parameters m_parameters;
		std::unordered_map<const MarmotType*, TypeRef> m_cache;
	};
}
