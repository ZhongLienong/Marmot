#include "TypeTransform.h"

#include <unordered_set>
#include <utility>

namespace
{
	using TypeRef = TypeTransform::TypeRef;
	using Cache = std::unordered_map<const MarmotType*, TypeRef>;

	enum class Operation { Instantiation, Freshening, Inference, Specialization };

	bool NeedsInference(const TypeRef& type, std::unordered_set<const MarmotType*>& visited)
	{
		if (!visited.insert(type.get()).second)
		{
			return false;
		}
		if (type->IsType<MarmotType::TypeVariable>() || type->IsType<MarmotType::AssociatedType>())
		{
			return true;
		}
		bool found = false;
		TypeTransform::ForEachChild(*type, [&found, &visited](const TypeRef& child) { found = found || NeedsInference(child, visited); });
		return found;
	}

	bool NeedsInference(const TypeRef& type)
	{
		std::unordered_set<const MarmotType*> visited;
		return NeedsInference(type, visited);
	}

	class GraphRewrite
	{
	public:
		GraphRewrite(Operation operation, Cache& cache, std::function<TypeRef(const TypeRef&)> replace, TypeTransform::ProjectionResolver resolve = {}, TypeRef target = {}, const MarmotType* root = nullptr)
			: m_operation(operation), m_cache(cache), m_replace(std::move(replace)), m_resolve(std::move(resolve)), m_target(std::move(target)), m_root(root)
		{
		}

		TypeRef Apply(const TypeRef& type)
		{
			if (const Cache::const_iterator cached = m_cache.find(type.get()); cached != m_cache.cend())
			{
				return cached->second;
			}
			if (type->IsType<MarmotType::GenericParam>() || type->IsType<MarmotType::TypeVariable>() || type->IsType<MarmotType::UndecidedType>())
			{
				TypeRef replacement = m_replace(type);
				if (m_operation == Operation::Inference && replacement != type)
				{
					return Apply(replacement);
				}
				if (m_operation == Operation::Freshening && !type->IsType<MarmotType::GenericParam>())
				{
					m_cache.emplace(type.get(), replacement);
				}
				return replacement;
			}
			if (m_operation == Operation::Inference && (type->IsType<MarmotType::StructType>() || type->IsType<MarmotType::UnionType>()) && !NeedsInference(type))
			{
				return type;
			}
			return std::visit([this, &type](const auto& node) { return Rebuild(type, node); }, type->m_type);
		}

	private:
		Operation m_operation;
		Cache& m_cache;
		std::function<TypeRef(const TypeRef&)> m_replace;
		TypeTransform::ProjectionResolver m_resolve;
		TypeRef m_target;
		const MarmotType* m_root;

		template<typename Node>
		TypeRef Rebuild(const TypeRef& type, const Node& original)
		{
			Node node = original;
			bool changed = m_operation == Operation::Instantiation || m_operation == Operation::Freshening;
			const auto map = [this, &changed](const TypeRef& child)
			{
				TypeRef mapped = Apply(child);
				changed = changed || mapped != child;
				return mapped;
			};
			const auto map_all = [&map](const std::vector<TypeRef>& children)
			{
				return children | std::views::transform(map) | std::ranges::to<std::vector>();
			};

			if constexpr (std::is_same_v<Node, MarmotType::ArrayType> || std::is_same_v<Node, MarmotType::RangeType>
				|| std::is_same_v<Node, MarmotType::ChannelType> || std::is_same_v<Node, MarmotType::CellType>)
			{
				node.m_element_type = map(original.m_element_type);
			}
			else if constexpr (std::is_same_v<Node, MarmotType::WorkerType>)
			{
				node.m_result_type = map(original.m_result_type);
			}
			else if constexpr (std::is_same_v<Node, MarmotType::TupleType>)
			{
				node.m_element_types = map_all(original.m_element_types);
			}
			else if constexpr (std::is_same_v<Node, MarmotType::FunctionType>)
			{
				node.m_param_types = map_all(original.m_param_types);
				node.m_return_type = map(original.m_return_type);
				node.m_constraints = MarmotType::MapConstraints(original.m_constraints, map);
				// Primitive identity belongs to the declaration, never a rebuilt function type.
				node.m_primitive.reset();
			}
			else if constexpr (std::is_same_v<Node, MarmotType::AssociatedType>)
			{
				node.m_type_args = map_all(original.m_type_args);
				TypeRef projection = changed ? std::make_shared<MarmotType>(std::move(node)) : type;
				TypeRef result = m_resolve ? m_resolve(projection->GetType<MarmotType::AssociatedType>()).value_or(projection) : projection;
				if (m_operation == Operation::Inference)
				{
					m_cache[type.get()] = result;
				}
				return result;
			}
			else if constexpr (std::is_same_v<Node, MarmotType::StructType> || std::is_same_v<Node, MarmotType::UnionType> || std::is_same_v<Node, MarmotType::NewType>)
			{
				// Publish the result shell before descending so nominal backedges point
				// into the rewritten graph, including the parser's existing placeholder.
				TypeRef result = m_root == type.get() ? m_target : std::make_shared<MarmotType>(node);
				m_cache[type.get()] = result;
				const auto map_arguments = [&]()
				{
					if constexpr (!std::is_same_v<Node, MarmotType::NewType>)
					{
						if (m_operation == Operation::Inference)
						{
							node.m_type_arguments = map_all(original.m_type_arguments);
							return;
						}
					}
					if (!original.m_generic_params.empty() || original.m_is_generic_instantiation)
					{
						node.m_type_arguments = MarmotType::InstantiateTypeArguments(original.m_generic_params, original.m_type_arguments, map);
					}
				};
				if (m_operation == Operation::Freshening && !std::is_same_v<Node, MarmotType::NewType>)
				{
					map_arguments();
				}
				if constexpr (std::is_same_v<Node, MarmotType::StructType>)
				{
					node.m_member_types = map_all(original.m_member_types);
				}
				else if constexpr (std::is_same_v<Node, MarmotType::NewType>)
				{
					node.m_representation = map(original.m_representation);
				}
				node.m_constraints = MarmotType::MapConstraints(original.m_constraints, map);
				if (m_operation != Operation::Freshening || std::is_same_v<Node, MarmotType::NewType>)
				{
					map_arguments();
				}
				if constexpr (std::is_same_v<Node, MarmotType::UnionType>)
				{
					for (const auto& [name, member] : original.m_member_info)
					{
						node.m_member_info.at(name).m_member_types = map_all(member.m_member_types);
					}
				}
				if (m_operation == Operation::Inference && !changed && !std::is_same_v<Node, MarmotType::NewType>)
				{
					m_cache[type.get()] = type;
					return type;
				}
				node.m_is_generic_instantiation = original.m_is_generic_instantiation || !original.m_generic_params.empty();
				if (!std::is_same_v<Node, MarmotType::NewType> || m_operation == Operation::Specialization)
				{
					node.m_generic_params.clear();
				}
				result->m_type = std::move(node);
				return result;
			}
			else
			{
				return m_operation == Operation::Instantiation ? std::make_shared<MarmotType>(node) : type;
			}
			return changed ? std::make_shared<MarmotType>(std::move(node)) : type;
		}
	};

	TypeRef ReplaceParameter(const TypeRef& type, const TypeTransform::Parameters& parameters, bool instantiate)
	{
		if (type->IsType<MarmotType::GenericParam>() || (!instantiate && type->IsType<MarmotType::TypeVariable>()))
		{
			const std::string name = type->IsType<MarmotType::GenericParam>() ? type->GetType<MarmotType::GenericParam>().m_name : type->ToString();
			if (const TypeTransform::Parameters::const_iterator found = parameters.find(name); found != parameters.cend())
			{
				return found->second;
			}
		}
		return instantiate ? std::make_shared<MarmotType>(*type) : type;
	}
}

TypeTransform::TypeRef TypeTransform::Instantiate(const TypeRef& type, const Parameters& parameters)
{
	Cache cache;
	return GraphRewrite(Operation::Instantiation, cache, [&parameters](const TypeRef& leaf) { return ReplaceParameter(leaf, parameters, true); }).Apply(type);
}

void TypeTransform::InstantiateInto(const TypeRef& target, const TypeRef& type, const Parameters& parameters)
{
	Cache cache;
	GraphRewrite(Operation::Instantiation, cache, [&parameters](const TypeRef& leaf) { return ReplaceParameter(leaf, parameters, true); }, {}, target, type.get()).Apply(type);
}

TypeTransform::TypeRef TypeTransform::Apply(const TypeRef& type, const Variables& variables, const ProjectionResolver& resolve)
{
	if (!NeedsInference(type))
	{
		return type;
	}
	Cache cache;
	return GraphRewrite(Operation::Inference, cache, [&variables](const TypeRef& leaf)
	{
		if (leaf->IsType<MarmotType::TypeVariable>())
		{
			const Variables::const_iterator binding = variables.find(leaf->GetType<MarmotType::TypeVariable>().m_id);
			if (binding != variables.cend())
			{
				return binding->second;
			}
		}
		return leaf;
	}, resolve).Apply(type);
}

TypeTransform::TypeRef TypeTransform::Specialize(const TypeRef& type, const Parameters& parameters, const ProjectionResolver& resolve)
{
	Cache cache;
	return GraphRewrite(Operation::Specialization, cache, [&parameters](const TypeRef& leaf) { return ReplaceParameter(leaf, parameters, false); }, resolve).Apply(type);
}

TypeTransform::Freshener::Freshener(std::function<TypeRef()> fresh_variable)
	: m_fresh_variable(std::move(fresh_variable))
{
}

TypeTransform::TypeRef TypeTransform::Freshener::Apply(const TypeRef& type)
{
	return GraphRewrite(Operation::Freshening, m_cache, [this](const TypeRef& leaf)
	{
		return leaf->IsType<MarmotType::GenericParam>() ? Parameter(leaf->GetType<MarmotType::GenericParam>().m_name) : m_fresh_variable();
	}).Apply(type);
}

TypeTransform::TypeRef TypeTransform::Freshener::Parameter(const std::string& name)
{
	if (const Parameters::const_iterator binding = m_parameters.find(name); binding != m_parameters.cend())
	{
		return binding->second;
	}
	return m_parameters.emplace(name, m_fresh_variable()).first->second;
}

void TypeTransform::Freshener::KeepParameter(const std::string& name, TypeRef variable)
{
	if (m_parameters.try_emplace(name, variable).second)
	{
		m_cache.emplace(variable.get(), variable);
	}
}

const TypeTransform::Parameters& TypeTransform::Freshener::Bindings() const
{
	return m_parameters;
}

void TypeTransform::ForEachChild(const MarmotType& type, const std::function<void(const std::shared_ptr<MarmotType>&)>& visit)
{
	const std::function<void(const std::vector<std::shared_ptr<MarmotType>>&)> visit_all = [&visit](const std::vector<std::shared_ptr<MarmotType>>& components)
	{
		for (const std::shared_ptr<MarmotType>& component : components)
		{
			visit(component);
		}
	};
	const std::function<void(const std::vector<MarmotType::ClassConstraint>&)> visit_constraints = [&visit, &visit_all](const std::vector<MarmotType::ClassConstraint>& constraints)
	{
		for (const MarmotType::ClassConstraint& constraint : constraints)
		{
			visit_all(constraint.m_type_args);
			if (constraint.IsEquality())
			{
				visit(constraint.m_equality_lhs);
				visit(constraint.m_equality_rhs);
			}
		}
	};

	std::visit
	(
		[&visit, &visit_all, &visit_constraints]<typename T>(const T& node)
		{
			using Node = std::decay_t<T>;
			if constexpr (std::is_same_v<Node, MarmotType::ArrayType> || std::is_same_v<Node, MarmotType::ChannelType> || std::is_same_v<Node, MarmotType::CellType> || std::is_same_v<Node, MarmotType::RangeType>)
			{
				visit(node.m_element_type);
			}
			else if constexpr (std::is_same_v<Node, MarmotType::WorkerType>)
			{
				visit(node.m_result_type);
			}
			else if constexpr (std::is_same_v<Node, MarmotType::TupleType>)
			{
				visit_all(node.m_element_types);
			}
			else if constexpr (std::is_same_v<Node, MarmotType::FunctionType>)
			{
				visit_all(node.m_param_types);
				visit(node.m_return_type);
				visit_constraints(node.m_constraints);
			}
			else if constexpr (std::is_same_v<Node, MarmotType::StructType>)
			{
				visit_all(node.m_member_types);
				visit_all(node.m_type_arguments);
				visit_constraints(node.m_constraints);
			}
			else if constexpr (std::is_same_v<Node, MarmotType::UnionType>)
			{
				for (const auto& [_, member_ctx] : node.m_member_info)
				{
					visit_all(member_ctx.m_member_types);
				}
				visit_all(node.m_type_arguments);
				visit_constraints(node.m_constraints);
			}
			else if constexpr (std::is_same_v<Node, MarmotType::NewType>)
			{
				visit(node.m_representation);
				visit_all(node.m_type_arguments);
				visit_constraints(node.m_constraints);
			}
			else if constexpr (std::is_same_v<Node, MarmotType::AssociatedType> || std::is_same_v<Node, MarmotType::ClassConstraint>)
			{
				visit_all(node.m_type_args);
			}
		},
		type.m_type
	);
}

