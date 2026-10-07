#include "MidoriAllocator.h"
#include "Value/Value.h"
#include "Error/RuntimeError.h"

#include <bit>
#include <cstdlib>

static_assert(sizeof(MidoriTraceable) <= MidoriAllocator::MAX_SLOT_SIZE, "MidoriTraceable must fit the largest allocator slot");
static_assert(alignof(MidoriTraceable) <= MidoriAllocator::SLOT_GRANULE, "every slot is aligned to the granule");

#ifdef __EMSCRIPTEN__

MidoriAllocator::MidoriAllocator() = default;

MidoriAllocator::~MidoriAllocator()
{
	for (uint8_t* block : m_block_bases)
	{
		std::free(block);
	}
}

bool MidoriAllocator::AllocateBlock(size_t size_class)
{
	uint8_t* block = static_cast<uint8_t*>(std::aligned_alloc(SLOT_GRANULE, BLOCK_SIZE));
	if (block == nullptr)
	{
		return false;
	}

	m_block_bases.push_back(block);
	AddBlock(block, size_class);
	return true;
}

std::optional<size_t> MidoriAllocator::FindBlock(const void* ptr) const noexcept
{
	const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
	for (size_t block_index = 0uz; block_index < m_block_bases.size(); block_index += 1uz)
	{
		const uintptr_t base = reinterpret_cast<uintptr_t>(m_block_bases[block_index]);
		if (address >= base && address < base + BLOCK_SIZE)
		{
			return block_index;
		}
	}
	return std::nullopt;
}

const uint8_t* MidoriAllocator::BlockBase(size_t block) const noexcept
{
	return m_block_bases[block];
}

std::optional<size_t> MidoriAllocator::TryGetSlotIndex(const void* ptr) const noexcept
{
	const std::optional<size_t> block = FindBlock(ptr);
	if (!block.has_value())
	{
		return std::nullopt;
	}
	return SlotIn(m_blocks[*block], static_cast<uint64_t>(static_cast<const uint8_t*>(ptr) - BlockBase(*block)));
}

size_t MidoriAllocator::SlotIndexOf(const void* slot) const noexcept
{
	return TryGetSlotIndex(slot).value();
}

#else

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

MidoriAllocator::MidoriAllocator()
{
#ifdef _WIN32
	m_reservation = static_cast<uint8_t*>(VirtualAlloc(nullptr, RESERVED_REGION_SIZE, MEM_RESERVE, PAGE_NOACCESS));
	m_region_base = m_reservation;
#else
	// Reserved one granule larger, so the region can start on a granule boundary:
	// a huge page is only used for a naturally aligned 2 MB range.
	void* reservation = mmap(nullptr, RESERVED_REGION_SIZE + COMMIT_GRANULE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (reservation != MAP_FAILED)
	{
		m_reservation = static_cast<uint8_t*>(reservation);
		const uintptr_t aligned = (reinterpret_cast<uintptr_t>(reservation) + COMMIT_GRANULE - 1uz) & ~(COMMIT_GRANULE - 1uz);
		m_region_base = reinterpret_cast<uint8_t*>(aligned);
	}
#endif
}

MidoriAllocator::~MidoriAllocator()
{
	if (m_reservation != nullptr)
	{
#ifdef _WIN32
		VirtualFree(m_reservation, 0u, MEM_RELEASE);
#else
		static_cast<void>(munmap(m_reservation, RESERVED_REGION_SIZE + COMMIT_GRANULE));
#endif
	}
}

bool MidoriAllocator::CommitGranule()
{
	if (m_region_base == nullptr || m_committed_bytes >= RESERVED_REGION_SIZE)
	{
		return false;
	}

	uint8_t* granule = m_region_base + m_committed_bytes;
#ifdef _WIN32
	if (VirtualAlloc(granule, COMMIT_GRANULE, MEM_COMMIT, PAGE_READWRITE) == nullptr)
	{
		return false;
	}
#else
	if (mprotect(granule, COMMIT_GRANULE, PROT_READ | PROT_WRITE) != 0)
	{
		return false;
	}
#ifdef MADV_HUGEPAGE
	// The first granule too: on small pages a short program pays about 400
	// faults over its first 1.6 MB of slots, which costs more than faulting
	// in and zeroing one 2 MB page, at the price of about 2 MB more RSS per
	// VM. Pre-faulting small pages with MADV_POPULATE_WRITE measured worse.
	static_cast<void>(madvise(granule, COMMIT_GRANULE, MADV_HUGEPAGE));
#endif
#endif

	m_committed_bytes += COMMIT_GRANULE;
	return true;
}

bool MidoriAllocator::AllocateBlock(size_t size_class)
{
	if (m_block_bytes == m_committed_bytes && !CommitGranule())
	{
		return false;
	}

	uint8_t* block = m_region_base + m_block_bytes;
	m_block_bytes += BLOCK_SIZE;
	AddBlock(block, size_class);
	return true;
}

#endif

void MidoriAllocator::AddBlock(uint8_t* base, size_t size_class)
{
	const size_t slot_size = (size_class + 1uz) * SLOT_GRANULE;
	const size_t slot_count = BLOCK_SIZE / slot_size;
	const size_t word_count = (slot_count + 63uz) / 64uz;
	const size_t first_bit = m_live_bits.size() * 64uz;
	const uint32_t block_index = static_cast<uint32_t>(m_blocks.size());

	m_blocks.push_back(BlockLayout
	{
		.m_first_bit = static_cast<uint32_t>(first_bit),
		.m_reciprocal = static_cast<uint32_t>(((1ull << 32u) + slot_size - 1uz) / slot_size),
		.m_slot_size = static_cast<uint32_t>(slot_size),
		.m_slot_count = static_cast<uint32_t>(slot_count),
	});
	m_live_bits.insert(m_live_bits.end(), word_count, 0ull);
	m_word_blocks.insert(m_word_blocks.end(), word_count, block_index);

	// Threaded from the last slot down, so the block is handed out in address
	// order and a structure built in one go is laid out the way it is walked.
	FreeNode*& free_list = m_free_lists[size_class];
	for (size_t slot = slot_count; slot > 0uz; slot -= 1uz)
	{
		FreeNode* node = reinterpret_cast<FreeNode*>(base + (slot - 1uz) * slot_size);
		node->m_next = free_list;
		node->m_slot_index = first_bit + slot - 1uz;
		free_list = node;
	}
}

void* MidoriAllocator::AllocateFromNewBlock(size_t size_class)
{
	if (!AllocateBlock(size_class))
	{
		FatalOutOfMemory("the heap is full", (size_class + 1uz) * SLOT_GRANULE);
	}
	return Allocate((size_class + 1uz) * SLOT_GRANULE);
}

void* MidoriAllocator::Allocate(size_t bytes)
{
	const size_t size_class = (bytes - 1uz) / SLOT_GRANULE;
	FreeNode* node = m_free_lists[size_class];
	if (node == nullptr)
	{
		return AllocateFromNewBlock(size_class);
	}

	m_free_lists[size_class] = node->m_next;
	const size_t slot_index = node->m_slot_index;
	m_live_bits[slot_index / 64uz] |= 1ull << (slot_index % 64uz);
	return node;
}

MidoriAllocator& MidoriAllocator::Free(void* slot, size_t slot_index) &
{
	m_live_bits[slot_index / 64uz] &= ~(1ull << (slot_index % 64uz));

	const size_t size_class = m_blocks[m_word_blocks[slot_index / 64uz]].m_slot_size / SLOT_GRANULE - 1uz;
	FreeNode* node = static_cast<FreeNode*>(slot);
	node->m_next = m_free_lists[size_class];
	node->m_slot_index = slot_index;
	m_free_lists[size_class] = node;
	return *this;
}

// Precondition: slot_index is a live slot's (callers derive indices from
// LiveBitWords or TryGetSlotIndex); a padding bit maps past the block's slots.
void* MidoriAllocator::SlotAt(size_t slot_index) const noexcept
{
	const size_t block = m_word_blocks[slot_index / 64uz];
	const BlockLayout& layout = m_blocks[block];
	return const_cast<uint8_t*>(BlockBase(block)) + (slot_index - layout.m_first_bit) * layout.m_slot_size;
}

bool MidoriAllocator::Contains(const void* ptr) const noexcept
{
	const std::optional<size_t> slot_index = TryGetSlotIndex(ptr);
	return slot_index.has_value() && (m_live_bits[*slot_index / 64uz] & (1ull << (*slot_index % 64uz))) != 0ull;
}

size_t MidoriAllocator::SlotWordCount() const noexcept
{
	return m_live_bits.size();
}

const uint64_t* MidoriAllocator::LiveBitWords() const noexcept
{
	return m_live_bits.data();
}

size_t MidoriAllocator::LiveSlotCount() const noexcept
{
	size_t count = 0uz;
	for (uint64_t word : m_live_bits)
	{
		count += static_cast<size_t>(std::popcount(word));
	}
	return count;
}
