#include <catch2/catch_test_macros.hpp>

#include "Interpreter/Allocator/MidoriAllocator.h"

#include <cstdint>
#include <optional>
#include <vector>

TEST_CASE("Slot index round-trips through SlotAt", "[allocator][gc]")
{
	MidoriAllocator allocator;

	void* first = allocator.Allocate();
	void* second = allocator.Allocate();
	REQUIRE(first != nullptr);
	REQUIRE(second != nullptr);

	std::optional<size_t> first_index = allocator.TryGetSlotIndex(first);
	std::optional<size_t> second_index = allocator.TryGetSlotIndex(second);
	REQUIRE(first_index.has_value());
	REQUIRE(second_index.has_value());
	REQUIRE(*first_index != *second_index);
	REQUIRE(allocator.SlotAt(*first_index) == first);
	REQUIRE(allocator.SlotAt(*second_index) == second);

	allocator.Free(*first_index);
	allocator.Free(*second_index);
}

TEST_CASE("Slot index rejects foreign and misaligned pointers", "[allocator][gc]")
{
	MidoriAllocator allocator;

	void* slot = allocator.Allocate();
	REQUIRE(slot != nullptr);

	int stack_object = 0;
	REQUIRE_FALSE(allocator.TryGetSlotIndex(&stack_object).has_value());
	REQUIRE_FALSE(allocator.TryGetSlotIndex(nullptr).has_value());

	uint8_t* misaligned = static_cast<uint8_t*>(slot) + 1;
	REQUIRE_FALSE(allocator.TryGetSlotIndex(misaligned).has_value());

	// Offset SLOTS_PER_BLOCK * SLOT_SIZE is slot-aligned but lands in the tail
	// padding of the first block, past the last real slot.
	uint8_t* padding_ptr = static_cast<uint8_t*>(allocator.SlotAt(0uz)) + MidoriAllocator::SLOTS_PER_BLOCK * MidoriAllocator::SLOT_SIZE;
	REQUIRE_FALSE(allocator.TryGetSlotIndex(padding_ptr).has_value());
}

TEST_CASE("Live bit words reflect allocation state", "[allocator][gc]")
{
	MidoriAllocator allocator;

	void* slot = allocator.Allocate();
	std::optional<size_t> index = allocator.TryGetSlotIndex(slot);
	REQUIRE(index.has_value());

	const uint64_t* words = allocator.LiveBitWords();
	REQUIRE(allocator.SlotWordCount() > *index / 64uz);
	REQUIRE((words[*index / 64uz] & (1ull << (*index % 64uz))) != 0ull);

	allocator.Free(*index);
	REQUIRE((words[*index / 64uz] & (1ull << (*index % 64uz))) == 0ull);
}

TEST_CASE("Slot indices stay valid past the first commit granules", "[allocator][gc]")
{
	MidoriAllocator allocator;

	// 5 MB of slots spans three 2 MB granules on POSIX and 80 blocks everywhere.
	constexpr size_t SLOT_COUNT = (5uz << 20uz) / MidoriAllocator::SLOT_SIZE;
	std::vector<size_t> indices;
	for (size_t i = 0uz; i < SLOT_COUNT; i += 1uz)
	{
		void* slot = allocator.Allocate();
		REQUIRE(slot != nullptr);

		std::optional<size_t> index = allocator.TryGetSlotIndex(slot);
		REQUIRE(index.has_value());
		REQUIRE(allocator.SlotAt(*index) == slot);
		REQUIRE(allocator.Contains(slot));
		indices.emplace_back(*index);
	}
	REQUIRE(allocator.LiveSlotCount() == SLOT_COUNT);

	for (size_t index : indices)
	{
		allocator.Free(index);
	}
	REQUIRE(allocator.LiveSlotCount() == 0uz);
}
