#pragma once

#include "Support/Attributes/Attributes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Slots come in multiples of SLOT_GRANULE up to MAX_SLOT_SIZE, and each block
// holds slots of one size. A block keeps its size for good: the collector's
// bitmaps are keyed by slot index, so a block's slots are never renumbered.
// ponytail: a block emptied of one size is not reused for another, so a
// program that switches from large objects to small ones keeps the old blocks.
// Re-sizing an empty block needs its free slots unlinked from their list.
class MidoriAllocator
{
public:
	static constexpr size_t BLOCK_SIZE = 65536uz;
	static constexpr size_t SLOT_GRANULE = 16uz;
	static constexpr size_t MAX_SLOT_SIZE = 80uz;
	static constexpr size_t SIZE_CLASS_COUNT = MAX_SLOT_SIZE / SLOT_GRANULE;

	static constexpr size_t SlotSizeFor(size_t bytes) noexcept
	{
		return (bytes + SLOT_GRANULE - 1uz) & ~(SLOT_GRANULE - 1uz);
	}

	MidoriAllocator();
	~MidoriAllocator();

	MidoriAllocator(const MidoriAllocator&) = delete;
	MidoriAllocator& operator=(const MidoriAllocator&) = delete;

	// A slot of at least `bytes`, at most MAX_SLOT_SIZE.
	void* Allocate(size_t bytes);

	MidoriAllocator& Free(void* slot, size_t slot_index) &;

	bool Contains(const void* ptr) const noexcept;

	void* SlotAt(size_t slot_index) const noexcept;

	size_t SlotWordCount() const noexcept;

	const uint64_t* LiveBitWords() const noexcept;

	size_t LiveSlotCount() const noexcept;

	// Slot indices are bit indices into the live bitmap. Each block owns whole
	// words of it, enough for its slots; the bits past its last slot are never set.
#ifndef __EMSCRIPTEN__
	MIDORI_FORCE_INLINE std::optional<size_t> TryGetSlotIndex(const void* ptr) const noexcept
	{
		const size_t offset = static_cast<size_t>(reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(m_region_base));
		if (offset >= m_block_bytes)
		{
			return std::nullopt;
		}
		return SlotIn(m_blocks[offset / BLOCK_SIZE], offset % BLOCK_SIZE);
	}

	// The index of a slot Allocate returned, which TryGetSlotIndex would
	// check first.
	MIDORI_FORCE_INLINE size_t SlotIndexOf(const void* slot) const noexcept
	{
		const size_t offset = static_cast<size_t>(reinterpret_cast<uintptr_t>(slot) - reinterpret_cast<uintptr_t>(m_region_base));
		const BlockLayout& layout = m_blocks[offset / BLOCK_SIZE];
		return static_cast<size_t>(layout.m_first_bit + ((static_cast<uint64_t>(offset % BLOCK_SIZE) * layout.m_reciprocal) >> 32u));
	}
#else
	std::optional<size_t> TryGetSlotIndex(const void* ptr) const noexcept;

	size_t SlotIndexOf(const void* slot) const noexcept;
#endif

private:
	// A free slot knows its index, so allocating one sets its live bit
	// without working the index out from its address.
	struct FreeNode
	{
		FreeNode* m_next;
		size_t m_slot_index;
	};

	// The reciprocal turns a byte offset in the block into a slot with a
	// multiply: ceil(2^32 / slot size) is exact for offsets below 2^16.
	struct BlockLayout
	{
		uint32_t m_first_bit;
		uint32_t m_reciprocal;
		uint32_t m_slot_size;
		uint32_t m_slot_count;
	};

	static_assert(sizeof(FreeNode) <= SLOT_GRANULE);

	// The slot at `offset` bytes into a block, if a slot starts there.
	static MIDORI_FORCE_INLINE std::optional<size_t> SlotIn(const BlockLayout& layout, uint64_t offset) noexcept
	{
		const uint64_t slot = (offset * layout.m_reciprocal) >> 32u;
		if (slot * layout.m_slot_size != offset || slot >= layout.m_slot_count)
		{
			return std::nullopt;
		}
		return static_cast<size_t>(layout.m_first_bit + slot);
	}

	std::vector<BlockLayout> m_blocks;
	// The block that owns each word of m_live_bits.
	std::vector<uint32_t> m_word_blocks;
	std::vector<uint64_t> m_live_bits;
	std::array<FreeNode*, SIZE_CLASS_COUNT> m_free_lists{};

	bool AllocateBlock(size_t size_class);
	void AddBlock(uint8_t* base, size_t size_class);
	MIDORI_NOINLINE void* AllocateFromNewBlock(size_t size_class);

#ifndef __EMSCRIPTEN__
	MIDORI_FORCE_INLINE const uint8_t* BlockBase(size_t block) const noexcept
	{
		return m_region_base + block * BLOCK_SIZE;
	}

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
	// Blocks are separate allocations here, found by a linear scan. They are
	// append-only and never sorted: renumbering would invalidate slot indices.
	std::optional<size_t> FindBlock(const void* ptr) const noexcept;

	const uint8_t* BlockBase(size_t block) const noexcept;

	std::vector<uint8_t*> m_block_bases;
#endif
};
