#pragma once

#include "Support/Attributes/Attributes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

class MidoriAllocator
{
public:
	static constexpr size_t BLOCK_SIZE = 65536uz;
	static constexpr size_t SLOT_SIZE = 80uz;
	static constexpr size_t SLOTS_PER_BLOCK = BLOCK_SIZE / SLOT_SIZE;

	MidoriAllocator();
	~MidoriAllocator();

	MidoriAllocator(const MidoriAllocator&) = delete;
	MidoriAllocator& operator=(const MidoriAllocator&) = delete;

	void* Allocate();

	MidoriAllocator& Free(void* slot, size_t slot_index) &;

	bool Contains(const void* ptr) const noexcept;

	void* SlotAt(size_t slot_index) const noexcept;

	size_t SlotWordCount() const noexcept;

	const uint64_t* LiveBitWords() const noexcept;

	size_t LiveSlotCount() const noexcept;

private:
	// A free slot knows its index, so allocating one sets its live bit
	// without working the index out from its address.
	struct FreeNode
	{
		FreeNode* m_next;
		size_t m_slot_index;
	};

	static constexpr size_t USABLE_BLOCK_BYTES = SLOTS_PER_BLOCK * SLOT_SIZE;
	static constexpr size_t LIVE_WORDS_PER_BLOCK = (SLOTS_PER_BLOCK + 63uz) / 64uz;
	static constexpr size_t BITS_PER_BLOCK = LIVE_WORDS_PER_BLOCK * 64uz;

	std::vector<uint64_t> m_live_bits;
	FreeNode* m_free_list = nullptr;

	bool AllocateBlock();
	MIDORI_NOINLINE void* AllocateFromNewBlock();

#ifndef __EMSCRIPTEN__
public:
	// Slot indices are global bit indices compatible with m_live_bits: each block
	// contributes BITS_PER_BLOCK positions (LIVE_WORDS_PER_BLOCK words * 64), of
	// which only the first SLOTS_PER_BLOCK are real slots; padding bits are never set.
	MIDORI_FORCE_INLINE std::optional<size_t> TryGetSlotIndex(const void* ptr) const noexcept
	{
		const size_t offset = static_cast<size_t>(reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(m_region_base));
		if (offset >= m_block_bytes)
		{
			return std::nullopt;
		}

		const size_t block_offset = offset % BLOCK_SIZE;
		const size_t slot_in_block = block_offset / SLOT_SIZE;
		if (slot_in_block * SLOT_SIZE != block_offset || slot_in_block >= SLOTS_PER_BLOCK)
		{
			return std::nullopt;
		}

		return (offset / BLOCK_SIZE) * BITS_PER_BLOCK + slot_in_block;
	}

	// The index of a slot Allocate returned, which TryGetSlotIndex would
	// check first.
	MIDORI_FORCE_INLINE size_t SlotIndexOf(const void* slot) const noexcept
	{
		const size_t offset = static_cast<size_t>(reinterpret_cast<uintptr_t>(slot) - reinterpret_cast<uintptr_t>(m_region_base));
		return (offset / BLOCK_SIZE) * BITS_PER_BLOCK + (offset % BLOCK_SIZE) / SLOT_SIZE;
	}

private:
	static constexpr size_t RESERVED_REGION_SIZE = 1uz << 30uz;
#ifdef _WIN32
	static constexpr size_t COMMIT_GRANULE = BLOCK_SIZE;
#else
	// One transparent huge page. Committing a 64 KB block at a time never makes a
	// whole 2 MB page accessible, so the kernel faulted a growing heap in 4 KB at a
	// time: a million-cell list took about 20,000 faults.
	static constexpr size_t COMMIT_GRANULE = 2uz << 20uz;
#endif

	uint8_t* m_reservation = nullptr;
	uint8_t* m_region_base = nullptr;
	size_t m_committed_bytes = 0uz;
	size_t m_block_bytes = 0uz;

	bool CommitGranule();
#else
public:
	std::optional<size_t> TryGetSlotIndex(const void* ptr) const noexcept;

	size_t SlotIndexOf(const void* slot) const noexcept;

private:
	std::vector<uint8_t*> m_blocks;

	std::optional<size_t> FindBlockIndex(const void* ptr) const noexcept;
#endif
};
