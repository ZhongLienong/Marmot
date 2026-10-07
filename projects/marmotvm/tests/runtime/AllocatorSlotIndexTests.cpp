#include <catch2/catch_test_macros.hpp>

#include "Interpreter/Allocator/MidoriAllocator.h"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

TEST_CASE("Slot index round-trips through SlotAt", "[allocator][gc]")
{
	MidoriAllocator allocator;

	void* first = allocator.Allocate(MidoriAllocator::MAX_SLOT_SIZE);
	void* second = allocator.Allocate(MidoriAllocator::MAX_SLOT_SIZE);
	REQUIRE(first != nullptr);
	REQUIRE(second != nullptr);

	std::optional<size_t> first_index = allocator.TryGetSlotIndex(first);
	std::optional<size_t> second_index = allocator.TryGetSlotIndex(second);
	REQUIRE(first_index.has_value());
	REQUIRE(second_index.has_value());
	REQUIRE(*first_index != *second_index);
	REQUIRE(allocator.SlotAt(*first_index) == first);
	REQUIRE(allocator.SlotAt(*second_index) == second);

	allocator.Free(first, *first_index);
	allocator.Free(second, *second_index);
}

TEST_CASE("A freed slot is allocated again under its own index", "[allocator][gc]")
{
	MidoriAllocator allocator;

	void* slot = allocator.Allocate(MidoriAllocator::MAX_SLOT_SIZE);
	const std::optional<size_t> index = allocator.TryGetSlotIndex(slot);
	REQUIRE(index.has_value());
	allocator.Free(slot, *index);

	REQUIRE(allocator.Allocate(MidoriAllocator::MAX_SLOT_SIZE) == slot);
	const uint64_t* words = allocator.LiveBitWords();
	REQUIRE((words[*index / 64uz] & (1ull << (*index % 64uz))) != 0ull);
	REQUIRE(allocator.LiveSlotCount() == 1uz);
}

TEST_CASE("Slot index rejects foreign and misaligned pointers", "[allocator][gc]")
{
	MidoriAllocator allocator;

	void* slot = allocator.Allocate(MidoriAllocator::MAX_SLOT_SIZE);
	REQUIRE(slot != nullptr);

	int stack_object = 0;
	REQUIRE_FALSE(allocator.TryGetSlotIndex(&stack_object).has_value());
	REQUIRE_FALSE(allocator.TryGetSlotIndex(nullptr).has_value());

	uint8_t* misaligned = static_cast<uint8_t*>(slot) + 1;
	REQUIRE_FALSE(allocator.TryGetSlotIndex(misaligned).has_value());

	// A whole number of slots past the last one in the block lands in its tail
	// padding, not in a slot.
	constexpr size_t SLOTS_PER_BLOCK = MidoriAllocator::BLOCK_SIZE / MidoriAllocator::MAX_SLOT_SIZE;
	uint8_t* padding_ptr = static_cast<uint8_t*>(allocator.SlotAt(0uz)) + SLOTS_PER_BLOCK * MidoriAllocator::MAX_SLOT_SIZE;
	REQUIRE_FALSE(allocator.TryGetSlotIndex(padding_ptr).has_value());
}

TEST_CASE("Live bit words reflect allocation state", "[allocator][gc]")
{
	MidoriAllocator allocator;

	void* slot = allocator.Allocate(MidoriAllocator::MAX_SLOT_SIZE);
	std::optional<size_t> index = allocator.TryGetSlotIndex(slot);
	REQUIRE(index.has_value());

	const uint64_t* words = allocator.LiveBitWords();
	REQUIRE(allocator.SlotWordCount() > *index / 64uz);
	REQUIRE((words[*index / 64uz] & (1ull << (*index % 64uz))) != 0ull);

	allocator.Free(slot, *index);
	REQUIRE((words[*index / 64uz] & (1ull << (*index % 64uz))) == 0ull);
}

TEST_CASE("Slot indices stay valid past the first commit granules", "[allocator][gc]")
{
	MidoriAllocator allocator;

	// 5 MB of slots spans three 2 MB granules on POSIX and 80 blocks everywhere.
	constexpr size_t SLOT_COUNT = (5uz << 20uz) / MidoriAllocator::MAX_SLOT_SIZE;
	std::vector<std::pair<void*, size_t>> slots;
	for (size_t i = 0uz; i < SLOT_COUNT; i += 1uz)
	{
		void* slot = allocator.Allocate(MidoriAllocator::MAX_SLOT_SIZE);
		REQUIRE(slot != nullptr);

		std::optional<size_t> index = allocator.TryGetSlotIndex(slot);
		REQUIRE(index.has_value());
		REQUIRE(allocator.SlotAt(*index) == slot);
		REQUIRE(allocator.Contains(slot));
		slots.emplace_back(slot, *index);
	}
	REQUIRE(allocator.LiveSlotCount() == SLOT_COUNT);

	for (const auto& [slot, index] : slots)
	{
		allocator.Free(slot, index);
	}
	REQUIRE(allocator.LiveSlotCount() == 0uz);
}

TEST_CASE("Slots of every size keep their own indices in interleaved blocks", "[allocator][gc]")
{
	MidoriAllocator allocator;

	// Enough of each size to fill several blocks, allocated round-robin so
	// blocks of different sizes sit next to each other in the region.
	constexpr std::array<size_t, 5uz> SIZES{ 16uz, 24uz, 40uz, 64uz, 80uz };
	constexpr size_t ROUNDS = 3uz * MidoriAllocator::BLOCK_SIZE / 16uz;
	std::vector<std::pair<void*, size_t>> slots;
	void* last_48_byte_slot = nullptr;
	for (size_t round = 0uz; round < ROUNDS; round += 1uz)
	{
		for (size_t size : SIZES)
		{
			void* slot = allocator.Allocate(size);
			const std::optional<size_t> index = allocator.TryGetSlotIndex(slot);
			REQUIRE(index.has_value());
			REQUIRE(allocator.SlotAt(*index) == slot);
			REQUIRE(allocator.SlotIndexOf(slot) == *index);
			REQUIRE(reinterpret_cast<uintptr_t>(slot) % MidoriAllocator::SLOT_GRANULE == 0uz);

			// Inside the slot rather than at its start, as a stale or computed
			// value on the stack might be.
			REQUIRE_FALSE(allocator.TryGetSlotIndex(static_cast<uint8_t*>(slot) + 8).has_value());
			if (MidoriAllocator::SlotSizeFor(size) > MidoriAllocator::SLOT_GRANULE)
			{
				REQUIRE_FALSE(allocator.TryGetSlotIndex(static_cast<uint8_t*>(slot) + MidoriAllocator::SLOT_GRANULE).has_value());
			}

			if (size == 40uz)
			{
				last_48_byte_slot = slot;
			}
			slots.emplace_back(slot, *index);
		}
	}
	REQUIRE(allocator.LiveSlotCount() == slots.size());

	for (const auto& [slot, index] : slots)
	{
		allocator.Free(slot, index);
	}
	REQUIRE(allocator.LiveSlotCount() == 0uz);

	// A freed slot goes back to the list of its own size.
	REQUIRE(allocator.Allocate(48uz) == last_48_byte_slot);
}
