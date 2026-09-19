#include <catch2/catch_test_macros.hpp>

#include "Common/Json/JsonReader.h"

#include <expected>
#include <string>

TEST_CASE("JSON reader parses every kind of value", "[json]")
{
	const std::expected<MidoriJson::JsonValue, std::string> document = MidoriJson::Parse(
		R"({ "text": "a\"b\\c\/\n\u00e9\ud83d\ude00", "number": -12.5e1, "zero": 0, "yes": true, "no": false, "nothing": null, "list": [1, [], {}] })");
	REQUIRE(document.has_value());

	const MidoriJson::JsonValue& root = document.value();
	REQUIRE(root.IsObject());
	CHECK(root.AsObject().size() == 7u);
	CHECK(root.Find("text")->AsString() == "a\"b\\c/\n\xC3\xA9\xF0\x9F\x98\x80");
	CHECK(root.Find("number")->AsNumber() == -125.0);
	CHECK(root.Find("zero")->AsNumber() == 0.0);
	CHECK(root.Find("yes")->AsBool());
	CHECK_FALSE(root.Find("no")->AsBool());
	CHECK(root.Find("nothing")->IsNull());
	REQUIRE(root.Find("list")->IsArray());
	CHECK(root.Find("list")->AsArray().size() == 3u);
	CHECK(root.Find("list")->AsArray()[1].IsArray());
	CHECK(root.Find("list")->AsArray()[2].IsObject());
	CHECK(root.Find("missing") == nullptr);
}

TEST_CASE("JSON reader keeps object members in source order", "[json]")
{
	const std::expected<MidoriJson::JsonValue, std::string> document = MidoriJson::Parse(R"({"b": 1, "a": 2, "c": 3})");
	REQUIRE(document.has_value());
	const MidoriJson::JsonValue::Object& members = document->AsObject();
	REQUIRE(members.size() == 3u);
	CHECK(members[0].first == "b");
	CHECK(members[1].first == "a");
	CHECK(members[2].first == "c");
}

TEST_CASE("JSON reader rejects what RFC 8259 does not allow", "[json]")
{
	const char* const invalid[] =
	{
		"",
		"{",
		"[1, 2",
		"[1,]",
		"{\"a\": 1,}",
		"{a: 1}",
		"'text'",
		"01",
		"1.",
		"-",
		"1e",
		"0x10",
		"NaN",
		"\"unterminated",
		"\"bad \\q escape\"",
		"\"raw\nnewline\"",
		"\"\\ud83d alone\"",
		"\"\\ude00\"",
		"true false",
		"{\"a\": 1, \"a\": 2}",
		"1e999",
	};

	for (const char* text : invalid)
	{
		INFO(text);
		CHECK_FALSE(MidoriJson::Parse(text).has_value());
	}
}

TEST_CASE("JSON reader errors name the line and column", "[json]")
{
	const std::expected<MidoriJson::JsonValue, std::string> document = MidoriJson::Parse("{\n  \"a\": 1,\n  \"a\": 2\n}");
	REQUIRE_FALSE(document.has_value());
	CHECK(document.error() == "line 3, column 3: duplicate member \"a\"");
}

TEST_CASE("JSON reader bounds nesting depth", "[json]")
{
	const std::string deep = std::string(1000u, '[') + std::string(1000u, ']');
	const std::expected<MidoriJson::JsonValue, std::string> document = MidoriJson::Parse(deep);
	REQUIRE_FALSE(document.has_value());
	CHECK(document.error().find("nested too deeply") != std::string::npos);
}
