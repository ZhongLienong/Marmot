#include <catch2/catch_test_macros.hpp>

#include "Compiler/TypeMatching/InstanceTypeMatcher.h"

namespace
{
	using TypeRef = InstanceTypeMatcher::TypeRef;
}

TEST_CASE("Instance matching keeps repeated parameter bindings consistent", "[compiler][instance-matching]")
{
	const TypeRef parameter = MarmotType::MakeGenericType("T");
	const TypeRef integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const TypeRef text = MarmotType::MakeLiteralType<MarmotType::TextType>();
	const TypeRef pattern = MarmotType::MakeFunctionType({ MarmotType::MakeArrayType(parameter) }, TypeRef(parameter));
	InstanceTypeMatcher consistent;
	REQUIRE(consistent.Match(pattern, MarmotType::MakeFunctionType({ MarmotType::MakeArrayType(integer) }, TypeRef(integer))));
	CHECK(consistent.Bindings().at("T") == integer);
	InstanceTypeMatcher inconsistent;
	CHECK_FALSE(inconsistent.Match(pattern, MarmotType::MakeFunctionType({ MarmotType::MakeArrayType(integer) }, TypeRef(text))));
	InstanceTypeMatcher arity;
	CHECK_FALSE(arity.MatchAll(std::vector<TypeRef>{ parameter }, std::vector<TypeRef>{ integer, text }));
}

TEST_CASE("Instance matching binds phantom arguments for every nominal type shape", "[compiler][instance-matching]")
{
	const TypeRef integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const TypeRef text = MarmotType::MakeLiteralType<MarmotType::TextType>();
	const std::vector<TypeRef> prototypes
	{
		MarmotType::MakeStructType("Phantom", "Origin", {}, {}, { "T" }),
		MarmotType::MakeUnionType("Phantom", "Origin", { "T" }),
		MarmotType::MakeNewType("Phantom", "Origin", integer, { "T" })
	};
	for (const TypeRef& prototype : prototypes)
	{
		const TypeRef concrete = MarmotType::SubstituteTypeParams(prototype, { { "T", integer } });
		const TypeRef other = MarmotType::SubstituteTypeParams(prototype, { { "T", text } });
		InstanceTypeMatcher match;
		REQUIRE(match.Match(prototype, concrete));
		CHECK(match.Bindings().at("T") == integer);
		InstanceTypeMatcher mismatch;
		CHECK_FALSE(mismatch.Match(concrete, other));
	}
	InstanceTypeMatcher module_identity;
	CHECK_FALSE(module_identity.Match(MarmotType::MakeStructType("Point", "Left", {}, {}), MarmotType::MakeStructType("Point", "Right", {}, {})));
}

TEST_CASE("Instance matching keeps bindings from separate prototype expansions", "[compiler][instance-matching]")
{
	const TypeRef first = MarmotType::MakeStructType("Phantom", "Origin", {}, {}, { "T" });
	const TypeRef second = MarmotType::MakeStructType("Phantom", "Origin", {}, {}, { "U" });
	const TypeRef integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const TypeRef concrete = MarmotType::SubstituteTypeParams(first, { { "T", integer } });
	InstanceTypeMatcher matcher;
	REQUIRE(matcher.MatchAll(std::vector<TypeRef>{ first, second }, std::vector<TypeRef>{ concrete, concrete }));
	CHECK(matcher.Bindings().at("T") == integer);
	CHECK(matcher.Bindings().at("U") == integer);
}

TEST_CASE("Instance matching terminates on recursive nominal types", "[compiler][instance-matching]")
{
	const TypeRef parameter = MarmotType::MakeGenericType("T");
	const TypeRef integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const TypeRef pattern = MarmotType::MakeUnionType("Chain", "Origin", { "T" });
	pattern->GetType<MarmotType::UnionType>().m_member_info.emplace("Link", MarmotType::UnionType::UnionMemberContext{ { parameter, pattern }, 0 });
	const TypeRef concrete = MarmotType::SubstituteTypeParams(pattern, { { "T", integer } });
	InstanceTypeMatcher matcher;
	CHECK(matcher.Match(pattern, concrete));
	CHECK(matcher.Bindings().at("T") == integer);
	pattern->GetType<MarmotType::UnionType>().m_member_info.clear();
	concrete->GetType<MarmotType::UnionType>().m_member_info.clear();
}

TEST_CASE("Instance matching binds projection arguments without resolving the projection", "[compiler][instance-matching]")
{
	const TypeRef parameter = MarmotType::MakeGenericType("T");
	const TypeRef integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const TypeRef pattern = MarmotType::MakeAssociatedType("Iterable", "Item", { MarmotType::MakeArrayType(parameter) });
	InstanceTypeMatcher matcher;
	REQUIRE(matcher.Match(pattern, MarmotType::MakeAssociatedType("Iterable", "Item", { MarmotType::MakeArrayType(integer) })));
	CHECK(matcher.Bindings().at("T") == integer);
	InstanceTypeMatcher other_class;
	CHECK_FALSE(other_class.Match(pattern, MarmotType::MakeAssociatedType("Other", "Item", { MarmotType::MakeArrayType(integer) })));
}

TEST_CASE("Instance matching probes can extend bindings without changing the original", "[compiler][instance-matching]")
{
	const TypeRef parameter = MarmotType::MakeGenericType("T");
	const TypeRef result = MarmotType::MakeGenericType("Result");
	const TypeRef integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const TypeRef text = MarmotType::MakeLiteralType<MarmotType::TextType>();
	InstanceTypeMatcher matcher;
	REQUIRE(matcher.Match(parameter, integer));
	InstanceTypeMatcher probe = matcher;
	REQUIRE(probe.Match(result, text));
	CHECK_FALSE(probe.Match(parameter, text));
	CHECK_FALSE(matcher.Bindings().contains("Result"));
	CHECK(matcher.Bindings().at("T") == integer);
	CHECK(probe.Bindings().at("Result") == text);
}

TEST_CASE("Instance matching rejects inconsistent bindings across temporary types", "[compiler][instance-matching][matching-lifetime]")
{
	const TypeRef parameter = MarmotType::MakeGenericType("T");
	const TypeRef pattern = MarmotType::MakeArrayType(parameter);
	const TypeRef integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const TypeRef text = MarmotType::MakeLiteralType<MarmotType::TextType>();
	InstanceTypeMatcher matcher;
	REQUIRE(matcher.Match(pattern, MarmotType::MakeArrayType(integer)));
	CHECK_FALSE(matcher.Match(pattern, MarmotType::MakeArrayType(text)));
}
