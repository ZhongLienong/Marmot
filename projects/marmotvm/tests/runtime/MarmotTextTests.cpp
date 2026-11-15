#include <catch2/catch_test_macros.hpp>

#include "Value/Value.h"

#include <cstring>
#include <string>
#include <string_view>

namespace
{
	// Longer than a short text holds, so it lives in a buffer.
	const std::string s_long(100uz, 'a');
}

TEST_CASE("A text that ends its buffer is appended to in place and shares it", "[text]")
{
	const MarmotText a(s_long.c_str());
	const MarmotText b = MarmotText::Concatenate(a, MarmotText("b"));

	CHECK(b.View() == s_long + "b");
	CHECK(a.View() == s_long);
	CHECK(b.View().data() == a.View().data());
	CHECK(a.GetOwnedBytes() > 0uz);
	CHECK(b.GetOwnedBytes() == 0uz);
}

TEST_CASE("A second append to the same text copies instead of writing over the first", "[text]")
{
	const MarmotText a(s_long.c_str());
	const MarmotText b = MarmotText::Concatenate(a, MarmotText("b"));
	const MarmotText c = MarmotText::Concatenate(a, MarmotText("c"));

	CHECK(b.View() == s_long + "b");
	CHECK(c.View() == s_long + "c");
	CHECK(c.View().data() != a.View().data());
	CHECK(c.GetOwnedBytes() > 0uz);
}

TEST_CASE("A copy shares its text's buffer and outlives the original", "[text]")
{
	MarmotText* original = new MarmotText(s_long.c_str());
	const MarmotText copy(*original);
	CHECK(copy.View().data() == original->View().data());
	delete original;

	CHECK(copy.View() == s_long);
}

TEST_CASE("CString moves a prefix that another text extends to a buffer ending in a NUL", "[text]")
{
	MarmotText a(s_long.c_str());
	const MarmotText b = MarmotText::Concatenate(a, MarmotText("b"));

	const char* terminated = a.CString();
	CHECK(std::strlen(terminated) == s_long.size());
	CHECK(std::string_view(terminated) == s_long);
	CHECK(b.View() == s_long + "b");
}

TEST_CASE("Append, Pop and Prepend on a shared text leave the texts that share it alone", "[text]")
{
	const MarmotText a(s_long.c_str());
	const MarmotText b = MarmotText::Concatenate(a, MarmotText("b"));

	MarmotText appended(a);
	appended.Append("x");
	MarmotText popped(b);
	popped.Pop();
	MarmotText prepended(b);
	prepended.Prepend("p");

	CHECK(appended.View() == s_long + "x");
	CHECK(popped.View() == s_long);
	CHECK(prepended.View() == "p" + s_long + "b");
	CHECK(a.View() == s_long);
	CHECK(b.View() == s_long + "b");
}

TEST_CASE("A text appended to itself reads what it was before", "[text]")
{
	MarmotText text(s_long.c_str());
	text.Append(text);

	CHECK(text.View() == s_long + s_long);
}

TEST_CASE("A chain of appends grows its buffer by doubling", "[text]")
{
	MarmotText text(s_long.c_str());
	std::string expected = s_long;
	int moves = 0;
	for (int i = 0; i < 10000; i += 1)
	{
		const char* before = text.View().data();
		text = MarmotText::Concatenate(text, MarmotText("z"));
		expected += 'z';
		moves += text.View().data() != before ? 1 : 0;
	}

	CHECK(text.View() == expected);
	CHECK(moves <= 10);
}
