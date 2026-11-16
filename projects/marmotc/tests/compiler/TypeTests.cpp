#include <catch2/catch_test_macros.hpp>

#include "Compiler/AbstractSyntaxTree/Type.h"
#include "Compiler/TypeTransform/TypeTransform.h"

#include <memory>
#include <string>
#include <vector>

TEST_CASE("A newtype renders as its own name, never its representation", "[type]")
{
	// ToString is what InstanceKey and MangleInstanceMethodName are keyed on, so
	// this single behaviour is what gives a newtype independent typeclass
	// instances. If it ever renders as "Int", Hashable<Meters> collapses into
	// Hashable<Int> and the feature is gone.
	const std::shared_ptr<MarmotType> representation = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const std::shared_ptr<MarmotType> meters = MarmotType::MakeNewType("Meters", "TypeTests", representation, {});

	REQUIRE(meters->ToString() == "TypeTests::Meters");
	REQUIRE(meters->DisplayString() == "Meters");
	REQUIRE(meters->ToString() != representation->ToString());
}

TEST_CASE("A newtype is unequal to its representation in both directions", "[type]")
{
	const std::shared_ptr<MarmotType> representation = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const std::shared_ptr<MarmotType> meters = MarmotType::MakeNewType("Meters", "TypeTests", representation, {});

	REQUIRE_FALSE(*meters == *representation);
	REQUIRE_FALSE(*representation == *meters);
}

TEST_CASE("Two newtypes over one representation are distinct from each other", "[type]")
{
	// Without this, `Meters` and `Seconds` would silently interconvert, which is
	// the exact confusion the feature exists to prevent.
	const std::shared_ptr<MarmotType> representation = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const std::shared_ptr<MarmotType> meters = MarmotType::MakeNewType("Meters", "TypeTests", representation, {});
	const std::shared_ptr<MarmotType> seconds = MarmotType::MakeNewType("Seconds", "TypeTests", representation, {});

	REQUIRE_FALSE(*meters == *seconds);
}

TEST_CASE("A newtype equals another newtype with the same name", "[type]")
{
	const std::shared_ptr<MarmotType> representation = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const std::shared_ptr<MarmotType> left = MarmotType::MakeNewType("Meters", "TypeTests", representation, {});
	const std::shared_ptr<MarmotType> right = MarmotType::MakeNewType("Meters", "TypeTests", representation, {});

	REQUIRE(*left == *right);
}

TEST_CASE("Two modules' types of the same name are two types", "[type]")
{
	// The name alone is not the identity: a module may declare `Wrap` while an
	// imported module declares its own, and a value of one is not a value of
	// the other. The rendering says which is which.
	const std::shared_ptr<MarmotType> representation = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const std::shared_ptr<MarmotType> mine = MarmotType::MakeNewType("Wrap", "Mine", representation, {});
	const std::shared_ptr<MarmotType> theirs = MarmotType::MakeNewType("Wrap", "Theirs", representation, {});

	REQUIRE_FALSE(*mine == *theirs);
	REQUIRE(mine->ToString() != theirs->ToString());
	REQUIRE(mine->DisplayString() == theirs->DisplayString());

	std::vector<std::shared_ptr<MarmotType>> member_types{ representation };
	std::vector<std::string> member_names{ "value" };
	std::vector<std::shared_ptr<MarmotType>> other_member_types{ representation };
	std::vector<std::string> other_member_names{ "value" };
	const std::shared_ptr<MarmotType> my_struct = MarmotType::MakeStructType("Point", "Mine", std::move(member_types), std::move(member_names));
	const std::shared_ptr<MarmotType> their_struct = MarmotType::MakeStructType("Point", "Theirs", std::move(other_member_types), std::move(other_member_names));

	REQUIRE_FALSE(*my_struct == *their_struct);

	const std::shared_ptr<MarmotType> my_union = MarmotType::MakeUnionType("Shade", "Mine");
	const std::shared_ptr<MarmotType> their_union = MarmotType::MakeUnionType("Shade", "Theirs");

	REQUIRE_FALSE(*my_union == *their_union);
}

TEST_CASE("A self-referential newtype's ToString recursion guard preserves nominal identity", "[type]")
{
	// FINDING 1 regression test. MarmotType::ToString()'s outer recursion guard
	// special-cases StructType and UnionType to return their own name on cycle
	// re-entry, and previously fell through to the literal string "Recursive"
	// for every other alternative, NewType included. Since InstanceKey and
	// MangleInstanceMethodName are both keyed on ToString, two distinct
	// self-referential newtypes both rendering "Recursive" would collapse into
	// one shared instance slot -- exactly the nominality collision this feature
	// exists to prevent.
	//
	// A cycle planted in m_representation cannot reach this guard: the NewType
	// arm of ToStringVisitor never stringifies m_representation at all (that is
	// the point of the type -- see the "never renders its representation" case
	// above), so a self-reference there is simply never visited and the guard
	// is never exercised. The reachable cycle is through m_type_arguments on a
	// generic instantiation, which StringifyTypeArguments does recurse into, so
	// that is what this test constructs -- directly through the public API, no
	// undefined behaviour.
	const std::shared_ptr<MarmotType> meters = MarmotType::MakeNewType("Meters", "TypeTests", MarmotType::MakeLiteralType<MarmotType::IntegerType>(), {});
	MarmotType::NewType& meters_data = meters->GetType<MarmotType::NewType>();
	meters_data.m_is_generic_instantiation = true;
	meters_data.m_type_arguments = { meters };

	const std::shared_ptr<MarmotType> seconds = MarmotType::MakeNewType("Seconds", "TypeTests", MarmotType::MakeLiteralType<MarmotType::IntegerType>(), {});
	MarmotType::NewType& seconds_data = seconds->GetType<MarmotType::NewType>();
	seconds_data.m_is_generic_instantiation = true;
	seconds_data.m_type_arguments = { seconds };

	REQUIRE(meters->ToString() == "TypeTests::Meters<TypeTests::Meters>");
	REQUIRE(seconds->ToString() == "TypeTests::Seconds<TypeTests::Seconds>");
	REQUIRE(meters->ToString() != seconds->ToString());
}

TEST_CASE("Substituting a newtype's parameter rewrites its representation", "[type]")
{
	const std::shared_ptr<MarmotType> element = MarmotType::MakeGenericType("T");
	const std::shared_ptr<MarmotType> representation = MarmotType::MakeArrayType(element);
	const std::shared_ptr<MarmotType> boxed = MarmotType::MakeNewType("Boxed", "TypeTests", representation, {"T"});

	std::unordered_map<std::string, std::shared_ptr<MarmotType>> substitutions;
	substitutions["T"] = MarmotType::MakeLiteralType<MarmotType::IntegerType>();

	const std::shared_ptr<MarmotType> instantiated = TypeTransform::Instantiate(boxed, substitutions);

	REQUIRE(instantiated->IsType<MarmotType::NewType>());

	const MarmotType::NewType& result = instantiated->GetType<MarmotType::NewType>();
	REQUIRE(result.m_name == "Boxed");
	REQUIRE(result.m_representation->IsType<MarmotType::ArrayType>());
	REQUIRE(result.m_representation->GetType<MarmotType::ArrayType>().m_element_type->IsType<MarmotType::IntegerType>());
}

TEST_CASE("Substituting a newtype with an empty map preserves its parameters", "[type]")
{
	// Newtype instantiation retains declaration parameters for representation
	// checks, unlike record and union instantiation.
	const std::shared_ptr<MarmotType> element = MarmotType::MakeGenericType("T");
	const std::shared_ptr<MarmotType> representation = MarmotType::MakeArrayType(element);
	const std::shared_ptr<MarmotType> boxed = MarmotType::MakeNewType("Boxed", "TypeTests", representation, {"T"});

	const std::unordered_map<std::string, std::shared_ptr<MarmotType>> empty;
	const std::shared_ptr<MarmotType> result = TypeTransform::Instantiate(boxed, empty);

	REQUIRE(result->IsType<MarmotType::NewType>());
	REQUIRE(result->GetType<MarmotType::NewType>().m_generic_params == std::vector<std::string>{"T"});
}

TEST_CASE("A substituted newtype renders its type argument, not its parameter name", "[type]")
{
	// If ToStringVisitor's NewType arm tested m_generic_params before
	// m_is_generic_instantiation, this would render "Boxed<T>" even after
	// instantiation, which deliberately preserves m_generic_params.
	// InstanceKey and MangleInstanceMethodName are
	// keyed on ToString, so that would collapse every instantiation onto one
	// instance slot.
	const std::shared_ptr<MarmotType> element = MarmotType::MakeGenericType("T");
	const std::shared_ptr<MarmotType> representation = MarmotType::MakeArrayType(element);
	const std::shared_ptr<MarmotType> boxed = MarmotType::MakeNewType("Boxed", "TypeTests", representation, {"T"});

	std::unordered_map<std::string, std::shared_ptr<MarmotType>> substitutions;
	substitutions["T"] = MarmotType::MakeLiteralType<MarmotType::IntegerType>();

	const std::shared_ptr<MarmotType> instantiated = TypeTransform::Instantiate(boxed, substitutions);

	REQUIRE(instantiated->ToString() == "TypeTests::Boxed<Int>");
	REQUIRE(instantiated->DisplayString() == "Boxed<Int>");
}

TEST_CASE("Two distinct newtype instantiations render differently", "[type]")
{
	// This is the assertion that actually pins the InstanceKey property: case
	// above alone would still pass if both instantiations collapsed onto some
	// other shared string, so long as it wasn't "Boxed<T>".
	const std::shared_ptr<MarmotType> element = MarmotType::MakeGenericType("T");
	const std::shared_ptr<MarmotType> representation = MarmotType::MakeArrayType(element);
	const std::shared_ptr<MarmotType> boxed = MarmotType::MakeNewType("Boxed", "TypeTests", representation, {"T"});

	std::unordered_map<std::string, std::shared_ptr<MarmotType>> int_substitutions;
	int_substitutions["T"] = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const std::shared_ptr<MarmotType> boxed_int = TypeTransform::Instantiate(boxed, int_substitutions);

	std::unordered_map<std::string, std::shared_ptr<MarmotType>> text_substitutions;
	text_substitutions["T"] = MarmotType::MakeLiteralType<MarmotType::TextType>();
	const std::shared_ptr<MarmotType> boxed_text = TypeTransform::Instantiate(boxed, text_substitutions);

	REQUIRE(boxed_int->ToString() != boxed_text->ToString());
}

TEST_CASE("Ref<T> renders with its element type", "[type][cell]")
{
	const std::shared_ptr<MarmotType> element = MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	const std::shared_ptr<MarmotType> cell = MarmotType::MakeCellType(element);

	REQUIRE(cell->ToString() == "Ref<Int>");
}

TEST_CASE("Cell types are equal exactly when their element types are", "[type][cell]")
{
	const std::shared_ptr<MarmotType> int_cell = MarmotType::MakeCellType(MarmotType::MakeLiteralType<MarmotType::IntegerType>());
	const std::shared_ptr<MarmotType> other_int_cell = MarmotType::MakeCellType(MarmotType::MakeLiteralType<MarmotType::IntegerType>());
	const std::shared_ptr<MarmotType> text_cell = MarmotType::MakeCellType(MarmotType::MakeLiteralType<MarmotType::TextType>());
	const std::shared_ptr<MarmotType> int_channel = MarmotType::MakeChannelType(MarmotType::MakeLiteralType<MarmotType::IntegerType>());

	REQUIRE(*int_cell == *other_int_cell);
	REQUIRE_FALSE(*int_cell == *text_cell);
	REQUIRE_FALSE(*int_cell == *int_channel);
}
