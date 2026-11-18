#include <catch2/catch_test_macros.hpp>

#include "Compiler/Lowering/InstanceResolver.h"
#include "Compiler/TypeTransform/TypeTransform.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

TEST_CASE("Instance resolution retains the declaring module through constraints and method values", "[lowering][instance-resolver][instance-origin]")
{
	const std::shared_ptr<MarmotType> integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const std::string symbol = MarmotType::MangleInstanceMethodName("Copy", "Echo", { integer });
	for (const std::string& origin : std::vector<std::string>{ "Origin", "" })
	{
		INFO(origin);
		TypeclassMetadata metadata;
		metadata.m_method_names = { "Copy" };
		metadata.m_instances.emplace_back(std::vector{ integer }, TypeclassInstance::AssociatedTypes(), std::vector<MarmotType::ClassConstraint>(), std::vector{ symbol }, origin);
		const InstanceResolver resolver({ { "Echo", std::move(metadata) } });
		const ResolvedInstanceName expected(symbol, origin);
		const std::optional<ResolvedInstanceName> exact = resolver.ResolveInstanceName("Echo", symbol);
		REQUIRE(exact.has_value());
		CHECK(exact.value() == expected);
		const MethodResolutionMap constraints = resolver.ResolveConstraints({ MarmotType::ClassConstraint("Echo", { integer }) }, {});
		const MethodResolution<std::optional<ResolvedInstanceName>> call = resolver.ResolveConstrainedCall(constraints, "Echo::Copy", { integer }, integer);
		REQUIRE(call.has_value());
		REQUIRE(call->has_value());
		CHECK(call->value() == expected);
		const MethodResolution<std::optional<ResolvedInstanceName>> value = resolver.ResolveConcreteCall("Echo::Copy", { integer }, integer);
		REQUIRE(value.has_value());
		REQUIRE(value->has_value());
		CHECK(value->value() == expected);
	}
}

TEST_CASE("Constrained calls select instances by concrete return identity", "[lowering][instance-resolver]")
{
	const InstanceResolver resolver({});
	const std::shared_ptr<MarmotType> argument_type = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const std::vector<std::shared_ptr<MarmotType>> return_types
	{
		MarmotType::MakeLiteralType<MarmotType::TextType>(),
		MarmotType::MakeNewType("Box", "T0Results", argument_type, {})
	};
	for (const std::shared_ptr<MarmotType>& return_type : return_types)
	{
		INFO(return_type->ToString());
		const MethodResolutionMap resolutions
		{
			{ "Convertable::Convert", {
				ResolvedMethodCandidate("Int", return_type->ToString(), ResolvedInstanceName("to_result", "Origin")),
				ResolvedMethodCandidate("Int", "Bool", ResolvedInstanceName("to_bool", "Origin"))
			} }
		};
		const MethodResolution<std::optional<ResolvedInstanceName>> resolved = resolver.ResolveConstrainedCall(resolutions, "Convertable::Convert", { argument_type }, return_type);
		REQUIRE(resolved.has_value());
		REQUIRE(resolved->has_value());
		CHECK(resolved->value() == ResolvedInstanceName("to_result", "Origin"));
	}
}

TEST_CASE("Constrained calls keep unresolved return hints ambiguous", "[lowering][instance-resolver]")
{
	const InstanceResolver resolver({});
	const std::shared_ptr<MarmotType> argument_type = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const std::vector<std::shared_ptr<MarmotType>> return_types
	{
		MarmotType::MakeUndecidedType(),
		MarmotType::MakeTypeVariable(42),
		MarmotType::MakeGenericType("Element"),
		MarmotType::MakeGenericType("T42"),
		MarmotType::MakeArrayType(MarmotType::MakeTypeVariable(42)),
		MarmotType::MakeTupleType({ argument_type, MarmotType::MakeGenericType("Element") })
	};
	for (const std::shared_ptr<MarmotType>& return_type : return_types)
	{
		INFO(return_type->ToString());
		const MethodResolutionMap resolutions
		{
			{ "Convertable::Convert", {
				ResolvedMethodCandidate("Int", return_type->ToString(), std::nullopt),
				ResolvedMethodCandidate("Int", "Text", ResolvedInstanceName("to_text", "Origin"))
			} }
		};
		const MethodResolution<std::optional<ResolvedInstanceName>> resolved = resolver.ResolveConstrainedCall(resolutions, "Convertable::Convert", { argument_type }, return_type);
		REQUIRE_FALSE(resolved.has_value());
		CHECK(resolved.error().m_code == CompilerErrorCode::LoweringAmbiguousMethodResolution);
	}
}

TEST_CASE("Associated type resolution binds phantom parameters from a nominal prototype", "[lowering][instance-resolver][instance-matching]")
{
	const std::shared_ptr<MarmotType> integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const std::shared_ptr<MarmotType> parameter = MarmotType::MakeGenericType("T");
	const std::shared_ptr<MarmotType> prototype = MarmotType::MakeStructType("Phantom", "Origin", {}, {}, { "T" });
	const std::shared_ptr<MarmotType> concrete = TypeTransform::Instantiate(prototype, { { "T", integer } });
	TypeclassMetadata metadata;
	metadata.m_type_param_names = { "Container" };
	metadata.m_associated_type_names = { "Item" };
	metadata.m_instances.emplace_back(std::vector{ prototype }, TypeclassInstance::AssociatedTypes{ { "Item", parameter } }, std::vector<MarmotType::ClassConstraint>(), std::vector<std::string>(), "Origin");
	const InstanceResolver resolver({ { "Extract", std::move(metadata) } });
	const std::optional<std::shared_ptr<MarmotType>> resolved = resolver.ResolveAssociatedType(MarmotType::AssociatedType("Extract", "Item", { concrete }));
	REQUIRE(resolved.has_value());
	CHECK(**resolved == *integer);
}
