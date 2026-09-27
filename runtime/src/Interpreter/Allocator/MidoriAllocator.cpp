#include "MidoriAllocator.h"
#include "Common/Value/Value.h"

#include <bit>
#include <cstdlib>

static_assert(sizeof(MidoriTraceable) <= MidoriAllocator::SLOT_SIZE, "MidoriTraceable must fit one allocator slot");
static_assert(alignof(MidoriTraceable) <= alignof(std::max_align_t), "slots rely on malloc alignment");

#ifdef __EMSCRIPTEN__

MidoriAllocator::MidoriAllocator()
{
	AllocateBlock();
}

MidoriAllocator::~MidoriAllocator()
{
	for (uint8_t* block : m_blocks)
	{
		std::free(block);
	}
}

bool MidoriAllocator::AllocateBlock()
{
	uint8_t* block = static_cast<uint8_t*>(std::malloc(BLOCK_SIZE));
	if (block == nullptr)
	{
		return false;
	}

	m_blocks.push_back(block);
	m_live_bits.insert(m_live_bits.end(), LIVE_WORDS_PER_BLOCK, 0ull);

	uint8_t* slot_ptr = block;
	for (size_t i = 0uz; i < SLOTS_PER_BLOCK; i += 1uz)
	{
		FreeNode* node = reinterpret_cast<FreeNode*>(slot_ptr);
		node->m_next = m_free_list;
		m_free_list = node;
		slot_ptr += SLOT_SIZE;
	}

	return true;
}

// Blocks are append-only and never sorted: persistent GC bitmaps are keyed by slot
// index, so renumbering is forbidden — do not replace this linear scan with a sorted
// table/binary search.
std::optional<size_t> MidoriAllocator::FindBlockIndex(const void* ptr) const noexcept
{
	const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
	for (size_t block_index = 0uz; block_index < m_blocks.size(); block_index += 1uz)
	{
		const uintptr_t base = reinterpret_cast<uintptr_t>(m_blocks[block_index]);
		if (address >= base && address < base + BLOCK_SIZE)
		{
			return block_index;
		}
	}
	return std::nullopt;
}

std::optional<size_t> MidoriAllocator::TryGetSlotIndex(const void* ptr) const noexcept
{
	const std::optional<size_t> block_index = FindBlockIndex(ptr);
	if (!block_index.has_value())
	{
		return std::nullopt;
	}

	const size_t block_offset = static_cast<size_t>(static_cast<const uint8_t*>(ptr) - m_blocks[*block_index]);
	if (block_offset % SLOT_SIZE != 0uz || block_offset >= USABLE_BLOCK_BYTES)
	{
		return std::nullopt;
	}

	return *block_index * BITS_PER_BLOCK + block_offset / SLOT_SIZE;
}

// Precondition: slot_index must correspond to a set live bit (callers derive indices
// from LiveBitWords/TryGetSlotIndex); out-of-range or padding indices yield pointers
// outside the slot area.
void* MidoriAllocator::SlotAt(size_t slot_index) const noexcept
{
	const size_t block_index = slot_index / BITS_PER_BLOCK;
	const size_t slot_in_block = slot_index % BITS_PER_BLOCK;
	return m_blocks[block_index] + slot_in_block * SLOT_SIZE;
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

	AllocateBlock();
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
	// The first granule stays on small pages, so a program (or worker) that
	// allocates little does not fault in and zero a whole 2 MB page.
	if (m_committed_bytes != 0uz)
	{
		static_cast<void>(madvise(granule, COMMIT_GRANULE, MADV_HUGEPAGE));
	}
#endif
#endif

	m_committed_bytes += COMMIT_GRANULE;
	return true;
}

bool MidoriAllocator::AllocateBlock()
{
	if (m_block_bytes == m_committed_bytes && !CommitGranule())
	{
		return false;
	}

	uint8_t* block_base = m_region_base + m_block_bytes;
	m_block_bytes += BLOCK_SIZE;
	m_live_bits.insert(m_live_bits.end(), LIVE_WORDS_PER_BLOCK, 0ull);

	uint8_t* slot_ptr = block_base;
	for (size_t i = 0uz; i < SLOTS_PER_BLOCK; i += 1uz)
	{
		FreeNode* node = reinterpret_cast<FreeNode*>(slot_ptr);
		node->m_next = m_free_list;
		m_free_list = node;
		slot_ptr += SLOT_SIZE;
	}

	return true;
}

// Precondition: slot_index must correspond to a set live bit (callers derive indices
// from LiveBitWords/TryGetSlotIndex); out-of-range or padding indices yield pointers
// outside the slot area.
void* MidoriAllocator::SlotAt(size_t slot_index) const noexcept
{
	const size_t block_index = slot_index / BITS_PER_BLOCK;
	const size_t slot_in_block = slot_index % BITS_PER_BLOCK;
	return m_region_base + block_index * BLOCK_SIZE + slot_in_block * SLOT_SIZE;
}

#endif

void* MidoriAllocator::AllocateFromNewBlock()
{
	if (!AllocateBlock())
	{
		return nullptr;
	}
	return Allocate();
}

void* MidoriAllocator::Allocate()
{
	if (m_free_list == nullptr)
	{
		return AllocateFromNewBlock();
	}

	FreeNode* node = m_free_list;
	m_free_list = node->m_next;

	const size_t slot_index = *TryGetSlotIndex(node);
	m_live_bits[slot_index / 64uz] |= 1ull << (slot_index % 64uz);
	return node;
}

MidoriAllocator& MidoriAllocator::Free(size_t slot_index) &
{
	m_live_bits[slot_index / 64uz] &= ~(1ull << (slot_index % 64uz));

	FreeNode* node = static_cast<FreeNode*>(SlotAt(slot_index));
	node->m_next = m_free_list;
	m_free_list = node;
	return *this;
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
