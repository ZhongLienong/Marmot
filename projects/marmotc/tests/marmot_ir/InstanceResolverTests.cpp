#include <catch2/catch_test_macros.hpp>

#include "Compiler/Lowering/InstanceResolver.h"

#include <memory>
#include <string>
#include <vector>

TEST_CASE("Constrained calls select instances by concrete return identity", "[lowering][instance-resolver]")
{
	const InstanceResolver resolver({}, [](const std::string&) { return false; });
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
				ResolvedMethodCandidate("Int", return_type->ToString(), "to_result", true),
				ResolvedMethodCandidate("Int", "Bool", "to_bool", true)
			} }
		};
		const MethodResolution<std::optional<std::string>> resolved = resolver.ResolveConstrainedCall(resolutions, "Convertable::Convert", &argument_type, return_type);
		REQUIRE(resolved.has_value());
		REQUIRE(resolved->has_value());
		CHECK(resolved->value() == "to_result");
	}
}

TEST_CASE("Constrained calls keep unresolved return hints ambiguous", "[lowering][instance-resolver]")
{
	const InstanceResolver resolver({}, [](const std::string&) { return false; });
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
				ResolvedMethodCandidate("Int", return_type->ToString(), "unresolved", false),
				ResolvedMethodCandidate("Int", "Text", "to_text", true)
			} }
		};
		const MethodResolution<std::optional<std::string>> resolved = resolver.ResolveConstrainedCall(resolutions, "Convertable::Convert", &argument_type, return_type);
		REQUIRE_FALSE(resolved.has_value());
		CHECK(resolved.error().m_code == CompilerErrorCode::LoweringAmbiguousMethodResolution);
	}
}
