#pragma once

#include "Common/Value/Value.h"
#include "Common/BuildConfig/BuildConfig.h"
#include "Interpreter/Allocator/MidoriAllocator.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

class GarbageCollector
{
public:
	static constexpr inline size_t INITIAL_GC_THRESHOLD = 512000uz * 8uz;
	static constexpr inline size_t MIN_GC_THRESHOLD = 512000uz * 4uz;
	// Caps the headroom above live bytes, not the threshold itself: a threshold at
	// or below the live bytes would make every allocation check collect.
	static constexpr inline size_t MAX_GC_HEADROOM = 512000uz * 128uz;
	static constexpr inline double GC_GROWTH_FACTOR = 1.5;

	using GarbageCollectionRoots = std::vector<MidoriTraceable*>;

	enum class CollectionKind : uint8_t
	{
		Minor,
		Major
	};

private:
	size_t m_total_bytes_allocated = 0uz;
	size_t m_gc_threshold = INITIAL_GC_THRESHOLD;
	size_t m_live_bytes_after_major = INITIAL_GC_THRESHOLD;
	std::vector<uint64_t> m_mark_bits;
	std::vector<uint64_t> m_logged_bits;
	std::vector<MidoriTraceable*> m_mark_stack;
	std::vector<MidoriTraceable*> m_remembered_set;
	const MidoriAllocator* m_allocator = nullptr;
	// Kept unconditional (not under #if MIDORI_DEBUG_INFO) so that sizeof(GarbageCollector)
	// cannot differ between translation units. This was first a workaround: the unit test
	// targets used to be compiled without MIDORI_BUILD_DEVELOPMENT, so guarding these members
	// would have given a test TU a smaller object than MidoriCore, and CollectNow (in core)
	// would have written these counters past its end. The cause is now fixed at the root --
	// MidoriCore's build-level definitions are PUBLIC and every consumer inherits them,
	// checked by tests/unit/common/AbiConsistencyTests.cpp -- but a layout that does not
	// depend on the debug level is still the safer default for a type shared across that
	// boundary. Only the increments/telemetry are guarded.
	size_t m_minor_collection_count = 0uz;
	size_t m_major_collection_count = 0uz;

public:
	GarbageCollector() = default;
	~GarbageCollector() = default;

	GarbageCollector(const GarbageCollector&) = delete;
	GarbageCollector& operator=(const GarbageCollector&) = delete;

	void ReclaimMemory(const GarbageCollectionRoots& roots, MidoriAllocator& allocator, bool force_clean = false);

	void CollectNow(const GarbageCollectionRoots& roots, MidoriAllocator& allocator, CollectionKind kind);

	size_t RememberedSetSize() const noexcept { return m_remembered_set.size(); }

	size_t MinorCollectionCount() const noexcept { return m_minor_collection_count; }

	size_t MajorCollectionCount() const noexcept { return m_major_collection_count; }

	// Out-of-line (MIDORI_NOINLINE): the barrier fires only on container-mutation opcodes,
	// and keeping it out of the dispatch-loop TU minimizes code-size/alignment perturbation
	// of ExecuteLoop, whose performance is highly layout-sensitive (see commit 0a7921e).
	MIDORI_NOINLINE void WriteBarrier(MidoriTraceable* target) noexcept;

	MIDORI_FORCE_INLINE bool ShouldCollect() const noexcept
	{
		return m_total_bytes_allocated >= m_gc_threshold;
	}

	size_t TotalBytesAllocated() const noexcept { return m_total_bytes_allocated; }

	void SetAllocator(const MidoriAllocator* allocator) noexcept { m_allocator = allocator; }

	MIDORI_FORCE_INLINE void RegisterObject(MidoriTraceable* traceable) noexcept
	{
		m_total_bytes_allocated += traceable->GetSize();
	}

	// Declared unconditionally so the class definition is identical across translation
	// units regardless of MIDORI_DEBUG_INFO; the definition (and every call site) stays
	// guarded in the .cpp, so non-debug builds never reference it.
	void PrintMemoryTelemetry();

	bool Contains(MidoriTraceable* ptr) const;

private:
	void Trace(const GarbageCollectionRoots& roots);
	void TryMark(MidoriTraceable* child_ptr);
	void Sweep(MidoriAllocator& allocator, size_t& sweep_count, size_t& bytes_reclaimed);
	void ClearRememberedSet() noexcept;
};
