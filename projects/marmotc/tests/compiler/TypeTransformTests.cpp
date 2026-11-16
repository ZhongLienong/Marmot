#include <catch2/catch_test_macros.hpp>

#include "Compiler/AbstractSyntaxTree/Type.h"
#include "Compiler/TypeTransform/TypeTransform.h"

namespace
{
	using TypeRef = std::shared_ptr<MarmotType>;

	std::vector<TypeRef> PhantomTypes()
	{
		const TypeRef integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
		return
		{
			MarmotType::MakeStructType("Record", "Origin", {}, {}, { "T" }),
			MarmotType::MakeUnionType("Union", "Origin", { "T" }),
			MarmotType::MakeNewType("Newtype", "Origin", integer, { "T" })
		};
	}
}

TEST_CASE("Type instantiation and lowering preserve phantom arguments and declaring identity", "[type-transform]")
{
	const TypeRef integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	for (const TypeRef& prototype : PhantomTypes())
	{
		const std::string name = prototype->ToString();
		INFO(name);
		const TypeRef instance = TypeTransform::Instantiate(prototype, { { "T", integer } });
		const TypeRef specialized = TypeTransform::Specialize(prototype, { { "T", integer } });
		CHECK(instance->ToString() == name.substr(0u, name.find('<')) + "<Int>");
		CHECK(specialized->ToString() == instance->ToString());
		CHECK(prototype->ToString() == name);
	}
}

TEST_CASE("Empty instantiation keeps its distinct record and newtype declaration semantics", "[type-transform]")
{
	const std::vector<TypeRef> prototypes = PhantomTypes();
	const TypeRef record = TypeTransform::Instantiate(prototypes[0u], {});
	const TypeRef newtype = TypeTransform::Instantiate(prototypes[2u], {});
	CHECK(record->GetType<MarmotType::StructType>().m_generic_params.empty());
	CHECK(record->GetType<MarmotType::StructType>().m_type_arguments.size() == 1u);
	CHECK(newtype->GetType<MarmotType::NewType>().m_generic_params == std::vector<std::string>{ "T" });
	CHECK(TypeTransform::Specialize(prototypes[2u], {})->GetType<MarmotType::NewType>().m_generic_params.empty());
}

TEST_CASE("Instantiation ties recursive references to its result and to parser placeholders", "[type-transform]")
{
	const TypeRef generic = MarmotType::MakeGenericType("T");
	const TypeRef tree = MarmotType::MakeUnionType("Tree", "Origin", { "T" });
	tree->GetType<MarmotType::UnionType>().m_member_info.emplace("Leaf", MarmotType::UnionType::UnionMemberContext{ { generic }, 0 });
	tree->GetType<MarmotType::UnionType>().m_member_info.emplace("Branch", MarmotType::UnionType::UnionMemberContext{ { MarmotType::MakeArrayType(tree) }, 1 });
	const TypeRef placeholder = MarmotType::MakeUnionType("Tree", "Origin", {});
	const TypeRef integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const TypeRef instance = TypeTransform::Instantiate(tree, { { "T", integer } });
	TypeTransform::InstantiateInto(placeholder, tree, { { "T", integer } });
	for (const TypeRef& result : { instance, placeholder, TypeTransform::Specialize(tree, { { "T", integer } }) })
	{
		const MarmotType::UnionType& data = result->GetType<MarmotType::UnionType>();
		CHECK(data.m_member_info.at("Leaf").m_member_types[0u]->IsType<MarmotType::IntegerType>());
		CHECK(data.m_member_info.at("Branch").m_member_types[0u]->GetType<MarmotType::ArrayType>().m_element_type == result);
		CHECK(data.m_member_info.at("Branch").m_tag == 1);
	}
	CHECK(tree->GetType<MarmotType::UnionType>().m_member_info.at("Leaf").m_member_types[0u] == generic);
}

TEST_CASE("Type transformations preserve foreign status and rewrite both forms of constraint", "[type-transform]")
{
	const TypeRef generic = MarmotType::MakeGenericType("T");
	const TypeRef integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const TypeRef function = MarmotType::MakeFunctionType({ generic }, TypeRef(generic), true);
	MarmotType::FunctionType& declared = function->GetType<MarmotType::FunctionType>();
	declared.m_primitive = MarmotPrimitive::Spawn;
	declared.m_constraints = { MarmotType::ClassConstraint("Equal", { generic }), MarmotType::ClassConstraint(TypeRef(generic), TypeRef(integer)) };
	for (const TypeRef& result : { TypeTransform::Instantiate(function, { { "T", integer } }), TypeTransform::Specialize(function, { { "T", integer } }) })
	{
		const MarmotType::FunctionType& mapped = result->GetType<MarmotType::FunctionType>();
		CHECK(mapped.m_is_foreign);
		CHECK_FALSE(mapped.m_primitive.has_value());
		CHECK(mapped.m_constraints[0u].m_type_args[0u]->IsType<MarmotType::IntegerType>());
		CHECK(mapped.m_constraints[1u].m_equality_lhs->IsType<MarmotType::IntegerType>());
		CHECK(mapped.m_constraints[1u].m_equality_rhs->IsType<MarmotType::IntegerType>());
	}
	CHECK(declared.m_primitive == MarmotPrimitive::Spawn);
}

TEST_CASE("Lowering shares unchanged structures and resolves projections after argument replacement", "[type-transform]")
{
	const TypeRef integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const TypeRef array = MarmotType::MakeArrayType(integer);
	CHECK(TypeTransform::Specialize(array, {}) == array);
	const TypeRef variable = MarmotType::MakeTypeVariable(42);
	const TypeRef projection = MarmotType::MakeAssociatedType("Iterable", "Element", { variable });
	size_t resolutions = 0u;
	const TypeRef result = TypeTransform::Specialize(projection, { { "T42", array } }, [&](const MarmotType::AssociatedType& associated) -> std::optional<TypeRef>
	{
		++resolutions;
		CHECK(associated.m_type_args[0u] == array);
		return integer;
	});
	CHECK(result == integer);
	CHECK(resolutions == 1u);
	CHECK(TypeTransform::Specialize(projection, {}, [](const MarmotType::AssociatedType&) -> std::optional<TypeRef> { return std::nullopt; }) == projection);
}

TEST_CASE("Freshening correlates parameters and repeated variables while preserving enclosing parameters", "[type-transform]")
{
	int next = 10;
	TypeTransform::Freshener freshener([&next]() { return MarmotType::MakeTypeVariable(next++); });
	const TypeRef enclosing = MarmotType::MakeTypeVariable(1);
	freshener.KeepParameter("Outer", enclosing);
	freshener.KeepParameter("Outer", MarmotType::MakeTypeVariable(2));
	const TypeRef variable = MarmotType::MakeTypeVariable(3);
	const TypeRef tuple = MarmotType::MakeTupleType({ MarmotType::MakeGenericType("T"), MarmotType::MakeGenericType("T"), variable, variable, enclosing });
	const TypeRef result = freshener.Apply(tuple);
	const std::vector<TypeRef>& elements = result->GetType<MarmotType::TupleType>().m_element_types;
	CHECK(elements[0u] == elements[1u]);
	CHECK(elements[2u] == elements[3u]);
	CHECK(elements[0u] != elements[2u]);
	CHECK(elements[4u] == enclosing);
	CHECK(freshener.Parameter("Outer") == enclosing);
	CHECK(freshener.Parameter("T") == elements[0u]);
	CHECK(next == 12);
	CHECK(variable->GetType<MarmotType::TypeVariable>().m_id == 3);
}

TEST_CASE("Inference substitution follows bindings transitively without changing its inputs", "[type-transform]")
{
	const TypeRef first = MarmotType::MakeTypeVariable(1);
	const TypeRef second = MarmotType::MakeTypeVariable(2);
	const TypeRef integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const TypeTransform::Variables bindings{ { 1, second }, { 2, integer } };
	const TypeRef array = MarmotType::MakeArrayType(first);
	const TypeRef result = TypeTransform::Apply(array, bindings, {});
	CHECK(result->GetType<MarmotType::ArrayType>().m_element_type == integer);
	CHECK(array->GetType<MarmotType::ArrayType>().m_element_type == first);
	CHECK(first->GetType<MarmotType::TypeVariable>().m_id == 1);
	CHECK(bindings.at(1) == second);
	CHECK(TypeTransform::Apply(array, {}, {}) == array);
	const TypeRef phantom = PhantomTypes()[0u];
	CHECK(TypeTransform::Apply(phantom, bindings, {}) == phantom);
}

TEST_CASE("Freshening and inference substitution preserve recursive nominal graphs and phantom arguments", "[type-transform]")
{
	int next = 1;
	TypeTransform::Freshener freshener([&next]() { return MarmotType::MakeTypeVariable(next++); });
	const TypeRef graph = MarmotType::MakeStructType("Node", "Origin", {}, { "children" }, { "T" });
	graph->GetType<MarmotType::StructType>().m_member_types = { MarmotType::MakeArrayType(graph) };
	const TypeRef fresh = freshener.Apply(graph);
	const TypeRef parameter = freshener.Parameter("T");
	const TypeRef integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const TypeRef solved = TypeTransform::Apply(fresh, { { parameter->GetType<MarmotType::TypeVariable>().m_id, integer } }, {});
	CHECK(fresh->GetType<MarmotType::StructType>().m_type_arguments == std::vector<TypeRef>{ parameter });
	for (const TypeRef& result : { fresh, solved })
	{
		const MarmotType::StructType& data = result->GetType<MarmotType::StructType>();
		CHECK(data.m_member_types[0u]->GetType<MarmotType::ArrayType>().m_element_type == result);
		CHECK(data.m_module_name == "Origin");
		CHECK(data.m_generic_params.empty());
	}
	CHECK(solved->ToString() == "Origin::Node<Int>");
	CHECK(graph->GetType<MarmotType::StructType>().m_generic_params == std::vector<std::string>{ "T" });
}

TEST_CASE("Inference substitution resolves associated types in constraints after binding their arguments", "[type-transform]")
{
	const TypeRef variable = MarmotType::MakeTypeVariable(1);
	const TypeRef integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const TypeRef projection = MarmotType::MakeAssociatedType("Iterable", "Element", { variable });
	const TypeRef function = MarmotType::MakeFunctionType({}, TypeRef(integer));
	function->GetType<MarmotType::FunctionType>().m_constraints = { MarmotType::ClassConstraint(TypeRef(projection), TypeRef(variable)) };
	const TypeRef result = TypeTransform::Apply(function, { { 1, integer } }, [&integer](const MarmotType::AssociatedType& associated) -> std::optional<TypeRef>
	{
		CHECK(associated.m_type_args[0u] == integer);
		return integer;
	});
	const MarmotType::ClassConstraint& constraint = result->GetType<MarmotType::FunctionType>().m_constraints[0u];
	CHECK(constraint.m_equality_lhs == integer);
	CHECK(constraint.m_equality_rhs == integer);
}
