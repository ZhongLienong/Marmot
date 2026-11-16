#include <catch2/catch_test_macros.hpp>

#include "Compiler/Module/TypeclassMetadata.h"

namespace
{
	using TypeRef = std::shared_ptr<MarmotType>;

	TypeclassMetadata MakeClass(std::vector<TypeclassInstance> instances)
	{
		TypeclassMetadata metadata;
		metadata.m_type_param_names = { "T" };
		metadata.m_associated_type_names = { "Item" };
		metadata.m_method_names = { "Get" };
		metadata.m_method_types.emplace("Get", MarmotType::MakeFunctionType({ MarmotType::MakeGenericType("T") }, TypeRef(MarmotType::MakeLiteralType<MarmotType::IntegerType>())));
		metadata.m_instances = std::move(instances);
		return metadata;
	}

	TypeclassInstance MakeInstance(TypeRef head, TypeRef item, std::string origin, std::string method)
	{
		return TypeclassInstance({ head }, { { "Item", item } }, { MarmotType::ClassConstraint("Allowed", { item }) }, { std::move(method) }, std::move(origin));
	}
}

TEST_CASE("Typeclass imports keep whole instance facts through a diamond and an append", "[module][typeclass-metadata]")
{
	const TypeRef integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const TypeRef text = MarmotType::MakeLiteralType<MarmotType::TextType>();
	const TypeclassMetadata shared = MakeClass({ MakeInstance(integer, text, "Origin", "Get_Int") });
	TypeclassMetadata right = shared;
	right.m_instances.push_back(MakeInstance(text, integer, "Right", "Get_Text"));
	TypeclassImports imports;
	REQUIRE_FALSE(imports.Import("Left", { { "Select", shared } }).has_value());
	REQUIRE_FALSE(imports.Import("Right", { { "Select", right } }).has_value());
	REQUIRE_FALSE(imports.Import("Origin", { { "Select", shared } }).has_value());
	const TypeclassMetadataMap merged = std::move(imports).TakeMetadata();
	const std::vector<TypeclassInstance>& instances = merged.at("Select").m_instances;
	REQUIRE(instances.size() == 2u);
	CHECK(instances[0].HasTypeArguments({ integer }));
	CHECK(*instances[0].m_associated_type_bindings.at("Item") == *text);
	CHECK(instances[0].m_constraints == shared.m_instances[0].m_constraints);
	CHECK(instances[0].m_declaring_module == "Origin");
	CHECK(instances[0].m_methods == std::vector<std::string>{ "Get_Int" });
	CHECK(instances[1].HasTypeArguments({ text }));
	CHECK(*instances[1].m_associated_type_bindings.at("Item") == *integer);
	CHECK(instances[1].m_constraints == right.m_instances[1].m_constraints);
	CHECK(instances[1].m_declaring_module == "Right");
	CHECK(instances[1].m_methods == std::vector<std::string>{ "Get_Text" });
	CHECK(merged.at("Select").InstanceMethods() == std::vector<std::string>{ "Get_Int@Origin", "Get_Text@Right" });
}

TEST_CASE("Typeclass imports diagnose duplicate declarations at their direct origins", "[module][typeclass-metadata]")
{
	const TypeRef integer = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	TypeclassImports imports;
	REQUIRE_FALSE(imports.Import("Left", { { "Select", MakeClass({ MakeInstance(integer, integer, "Left", "Get_Int") }) } }).has_value());
	const std::optional<std::string> conflict = imports.Import("Right", { { "Select", MakeClass({ MakeInstance(integer, integer, "Right", "Get_Int") }) } });
	REQUIRE(conflict.has_value());
	CHECK(*conflict == "Typeclass 'Select' has two instances for 'Int': one in module 'Left', one in module 'Right'. An instance belongs in the module that declares the class or the module that declares the type.");
}

TEST_CASE("Typeclass imports preserve module identity and zero-method instances", "[module][typeclass-metadata]")
{
	const TypeRef left = MarmotType::MakeStructType("Wrap", "Left", {}, {});
	const TypeRef right = MarmotType::MakeStructType("Wrap", "Right", {}, {});
	TypeclassMetadata left_metadata;
	left_metadata.m_type_param_names = { "T" };
	left_metadata.m_instances.emplace_back(TypeclassInstance::TypeArguments{ left }, TypeclassInstance::AssociatedTypes(), std::vector<MarmotType::ClassConstraint>(), std::vector<std::string>(), "Left");
	TypeclassMetadata right_metadata;
	right_metadata.m_type_param_names = { "T" };
	right_metadata.m_instances.emplace_back(TypeclassInstance::TypeArguments{ right }, TypeclassInstance::AssociatedTypes(), std::vector<MarmotType::ClassConstraint>(), std::vector<std::string>(), "Right");
	TypeclassImports imports;
	REQUIRE_FALSE(imports.Import("Left", { { "Marker", left_metadata } }).has_value());
	REQUIRE_FALSE(imports.Import("Right", { { "Marker", right_metadata } }).has_value());
	const TypeclassMetadataMap merged = std::move(imports).TakeMetadata();
	const TypeclassMetadata& marker = merged.at("Marker");
	REQUIRE(marker.m_instances.size() == 2u);
	CHECK(marker.m_instances[0].HasTypeArguments({ left }));
	CHECK_FALSE(marker.m_instances[0].HasTypeArguments({ right }));
	CHECK(marker.m_instances[1].HasTypeArguments({ right }));
	CHECK(marker.InstanceMethods().empty());
}

TEST_CASE("Typeclass imports diagnose incompatible definitions", "[module][typeclass-metadata]")
{
	TypeclassMetadata left = MakeClass({});
	TypeclassMetadata right = left;
	right.m_method_types["Get"] = MarmotType::MakeFunctionType({ MarmotType::MakeGenericType("T") }, TypeRef(MarmotType::MakeLiteralType<MarmotType::TextType>()));
	TypeclassImports imports;
	REQUIRE_FALSE(imports.Import("Left", { { "Select", left } }).has_value());
	const std::optional<std::string> conflict = imports.Import("Right", { { "Select", right } });
	REQUIRE(conflict.has_value());
	CHECK(*conflict == "Typeclass 'Select' is defined in multiple imported modules ('Left' and 'Right')");
}
