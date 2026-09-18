#include <catch2/catch_test_macros.hpp>

#include "Common/BuildConfig/BuildConfig.h"
#include "Common/Value/Value.h"

#include <cstddef>
#include <string_view>

// Guards against a translation unit reading BuildConfig.h differently from the
// MidoriCore library it links. BuildConfig.h derives MIDORI_DEBUG_LEVEL from
// MIDORI_BUILD_* compile definitions, and headers change type layouts on it:
// sizeof(MidoriValue) is 8 at levels 1-2 and 16 at level 3. The unit test
// targets once compiled at a different level from MidoriCore because they never
// received those definitions, and in x64-debug that was a live out-of-bounds
// read when a test passed an 8-byte MidoriValue to library code expecting 16.
//
// Each side of every comparison is computed differently on purpose. The left
// comes from a function defined out of line in MidoriCore, compiled with the
// library's definitions. The right is a macro or sizeof evaluated right here, in
// this translation unit, with this target's definitions. Neither is an inline
// function, so the linker cannot merge the two and hide a mismatch.

TEST_CASE("Test translation units see MidoriCore's debug level", "[build][abi]")
{
	CHECK(MidoriBuild::LibraryDebugLevel() == MIDORI_DEBUG_LEVEL);
}

TEST_CASE("Test translation units see MidoriCore's MidoriValue layout", "[build][abi]")
{
	CHECK(MidoriValue::LibrarySize() == sizeof(MidoriValue));
}

TEST_CASE("Test translation units see MidoriCore's version string", "[build][abi]")
{
	CHECK(MidoriBuild::LibraryVersionString() == std::string_view(MIDORI_VERSION_STRING));
}

TEST_CASE("Test translation units see MidoriCore's endianness", "[build][abi]")
{
#if defined(MIDORI_LITTLE_ENDIAN)
	const bool this_unit_is_little_endian = true;
#else
	const bool this_unit_is_little_endian = false;
#endif
	CHECK(MidoriBuild::LibraryIsLittleEndian() == this_unit_is_little_endian);
}
