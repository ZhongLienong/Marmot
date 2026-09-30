#include <catch2/catch_test_macros.hpp>

#include "Value/Value.h"

// Guards against a translation unit compiling Value.h differently from the
// runtime library it links. sizeof(MidoriValue) is 8 in Dev and Release
// and 16 in Debug, chosen by MIDORI_DEBUG_FULL. The unit test targets once
// compiled at a different level from the library because they never received
// the build definitions, and in x64-debug that was a live out-of-bounds read
// when a test passed an 8-byte MidoriValue to library code expecting 16.
//
// The left side is computed out of line in MarmotRuntime, with the library's
// definitions; the right is sizeof evaluated here, with this target's.

TEST_CASE("Test translation units see the runtime's MidoriValue layout", "[build][abi]")
{
	CHECK(MidoriValue::LibrarySize() == sizeof(MidoriValue));
}
