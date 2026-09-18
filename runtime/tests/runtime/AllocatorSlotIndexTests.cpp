#include <catch2/catch_test_macros.hpp>

#include "Interpreter/Allocator/MidoriAllocator.h"

#include <cstdint>
#include <optional>

TEST_CASE("Slot index round-trips through SlotAt", "[allocator][gc]")
{
	MidoriAllocator allocator;

	void* first = allocator.Allocate(MidoriAllocator::SLOT_SIZE);
	void* second = allocator.Allocate(MidoriAllocator::SLOT_SIZE);
	REQUIRE(first != nullptr);
	REQUIRE(second != nullptr);

	std::optional<size_t> first_index = allocator.TryGetSlotIndex(first);
	std::optional<size_t> second_index = allocator.TryGetSlotIndex(second);
	REQUIRE(first_index.has_value());
	REQUIRE(second_index.has_value());
	REQUIRE(*first_index != *second_index);
	REQUIRE(allocator.SlotAt(*first_index) == first);
	REQUIRE(allocator.SlotAt(*second_index) == second);

	allocator.Free(first, MidoriAllocator::SLOT_SIZE);
	allocator.Free(second, MidoriAllocator::SLOT_SIZE);
}

TEST_CASE("Slot index rejects foreign and misaligned pointers", "[allocator][gc]")
{
	MidoriAllocator allocator;

	void* slot = allocator.Allocate(MidoriAllocator::SLOT_SIZE);
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

	allocator.Free(slot, MidoriAllocator::SLOT_SIZE);
}

TEST_CASE("Live bit words reflect allocation state", "[allocator][gc]")
{
	MidoriAllocator allocator;

	void* slot = allocator.Allocate(MidoriAllocator::SLOT_SIZE);
	std::optional<size_t> index = allocator.TryGetSlotIndex(slot);
	REQUIRE(index.has_value());

	const uint64_t* words = allocator.LiveBitWords();
	REQUIRE(allocator.SlotWordCount() > *index / 64uz);
	REQUIRE((words[*index / 64uz] & (1ull << (*index % 64uz))) != 0ull);

	allocator.Free(slot, MidoriAllocator::SLOT_SIZE);
	REQUIRE((words[*index / 64uz] & (1ull << (*index % 64uz))) == 0ull);
}
