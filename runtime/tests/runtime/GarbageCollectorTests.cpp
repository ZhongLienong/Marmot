#include <catch2/catch_test_macros.hpp>

#include "Common/Value/Value.h"
#include "Interpreter/Allocator/MidoriAllocator.h"
#include "Interpreter/GarbageCollector/GarbageCollector.h"

namespace
{
	MidoriTraceable* AllocateText(MidoriAllocator& allocator, GarbageCollector& gc, const char* content)
	{
		void* memory = allocator.Allocate(sizeof(MidoriTraceable));
		MidoriTraceable* traceable = new(memory) MidoriTraceable(MidoriText(content));
		gc.RegisterObject(traceable);
		return traceable;
	}

	MidoriTraceable* AllocateArrayOf(MidoriAllocator& allocator, GarbageCollector& gc, MidoriTraceable* element)
	{
		MidoriArray array;
		if (element != nullptr)
		{
			array.AddBack(MidoriValue(element));
		}
		else
		{
			array.AddBack(MidoriValue(static_cast<MidoriInteger>(0)));
		}
		void* memory = allocator.Allocate(sizeof(MidoriTraceable));
		MidoriTraceable* traceable = new(memory) MidoriTraceable(std::move(array));
		gc.RegisterObject(traceable);
		return traceable;
	}

	// A text whose registered GetSize (sizeof(MidoriTraceable) + GetCapacity()) is large
	// enough to push total allocated bytes past INITIAL_GC_THRESHOLD after only a few
	// allocations. MidoriText::GetCapacity() reflects the long-buffer capacity reserved
	// via Reserve(), not the logical string length.
	MidoriTraceable* AllocateLargeText(MidoriAllocator& allocator, GarbageCollector& gc, int reserve_bytes)
	{
		MidoriText text;
		text.Reserve(reserve_bytes);
		void* memory = allocator.Allocate(sizeof(MidoriTraceable));
		MidoriTraceable* traceable = new(memory) MidoriTraceable(std::move(text));
		gc.RegisterObject(traceable);
		return traceable;
	}
}

TEST_CASE("Collection keeps rooted objects and reclaims garbage", "[gc]")
{
	MidoriAllocator allocator;
	GarbageCollector gc;
	gc.SetAllocator(&allocator);

	MidoriTraceable* rooted = AllocateText(allocator, gc, "rooted");
	MidoriTraceable* garbage = AllocateText(allocator, gc, "garbage");

	GarbageCollector::GarbageCollectionRoots roots{ rooted };
	gc.ReclaimMemory(roots, allocator, true);

	REQUIRE(allocator.Contains(rooted));
	REQUIRE_FALSE(allocator.Contains(garbage));

	GarbageCollector::GarbageCollectionRoots no_roots;
	gc.ReclaimMemory(no_roots, allocator, true);
	REQUIRE_FALSE(allocator.Contains(rooted));
}

TEST_CASE("Collection traces through containers", "[gc]")
{
	MidoriAllocator allocator;
	GarbageCollector gc;
	gc.SetAllocator(&allocator);

	MidoriTraceable* leaf = AllocateText(allocator, gc, "leaf");
	MidoriTraceable* holder = AllocateArrayOf(allocator, gc, leaf);

	GarbageCollector::GarbageCollectionRoots roots{ holder };
	gc.ReclaimMemory(roots, allocator, true);

	REQUIRE(allocator.Contains(holder));
	REQUIRE(allocator.Contains(leaf));

	GarbageCollector::GarbageCollectionRoots no_roots;
	gc.ReclaimMemory(no_roots, allocator, true);
	REQUIRE_FALSE(allocator.Contains(holder));
	REQUIRE_FALSE(allocator.Contains(leaf));
}

TEST_CASE("Collection survives repeated cycles with the same roots", "[gc]")
{
	MidoriAllocator allocator;
	GarbageCollector gc;
	gc.SetAllocator(&allocator);

	MidoriTraceable* rooted = AllocateText(allocator, gc, "stable");
	GarbageCollector::GarbageCollectionRoots roots{ rooted };

	for (int cycle = 0; cycle < 5; cycle += 1)
	{
		AllocateText(allocator, gc, "transient");
		gc.ReclaimMemory(roots, allocator, true);
		REQUIRE(allocator.Contains(rooted));
	}

	GarbageCollector::GarbageCollectionRoots no_roots;
	gc.ReclaimMemory(no_roots, allocator, true);
}

TEST_CASE("Collection updates byte accounting for reclaimed objects", "[gc]")
{
	MidoriAllocator allocator;
	GarbageCollector gc;
	gc.SetAllocator(&allocator);

	REQUIRE(gc.TotalBytesAllocated() == 0uz);

	size_t expected_bytes = 0uz;
	MidoriTraceable* first = AllocateText(allocator, gc, "first");
	expected_bytes += first->GetSize();
	REQUIRE(gc.TotalBytesAllocated() == expected_bytes);

	MidoriTraceable* second = AllocateText(allocator, gc, "second");
	expected_bytes += second->GetSize();
	MidoriTraceable* third = AllocateText(allocator, gc, "third");
	expected_bytes += third->GetSize();
	REQUIRE(gc.TotalBytesAllocated() == expected_bytes);

	// A handful of texts is nowhere near the initial threshold.
	REQUIRE_FALSE(gc.ShouldCollect());

	GarbageCollector::GarbageCollectionRoots no_roots;
	gc.ReclaimMemory(no_roots, allocator, true);

	REQUIRE(allocator.LiveSlotCount() == 0uz);
	REQUIRE(gc.TotalBytesAllocated() == 0uz);
}

TEST_CASE("Collection sweeps garbage across multiple bitmap words", "[gc]")
{
	MidoriAllocator allocator;
	GarbageCollector gc;
	gc.SetAllocator(&allocator);

	constexpr size_t OBJECT_COUNT = 100uz;
	GarbageCollector::GarbageCollectionRoots roots;
	for (size_t idx = 0uz; idx < OBJECT_COUNT; idx += 1uz)
	{
		MidoriTraceable* traceable = AllocateText(allocator, gc, "multi-word");
		if (idx % 10uz == 0uz)
		{
			roots.emplace_back(traceable);
		}
	}

	gc.ReclaimMemory(roots, allocator, true);

	for (MidoriTraceable* rooted : roots)
	{
		REQUIRE(allocator.Contains(rooted));
	}
	REQUIRE(allocator.LiveSlotCount() == roots.size());

	GarbageCollector::GarbageCollectionRoots no_roots;
	gc.ReclaimMemory(no_roots, allocator, true);
	REQUIRE(allocator.LiveSlotCount() == 0uz);
}

TEST_CASE("Minor collection skips old objects but write barrier keeps old-to-young edges alive", "[gc][generational]")
{
	MidoriAllocator allocator;
	GarbageCollector gc;
	gc.SetAllocator(&allocator);

	MidoriTraceable* old_holder = AllocateArrayOf(allocator, gc, nullptr);
	GarbageCollector::GarbageCollectionRoots holder_roots{ old_holder };
	gc.CollectNow(holder_roots, allocator, GarbageCollector::CollectionKind::Minor);
	REQUIRE(allocator.Contains(old_holder));

	MidoriTraceable* young_child = AllocateText(allocator, gc, "young");
	gc.WriteBarrier(old_holder);
	old_holder->GetTraceable<MidoriArray>()[0] = MidoriValue(young_child);

	gc.CollectNow(holder_roots, allocator, GarbageCollector::CollectionKind::Minor);
	REQUIRE(allocator.Contains(old_holder));
	REQUIRE(allocator.Contains(young_child));

	GarbageCollector::GarbageCollectionRoots no_roots;
	gc.CollectNow(no_roots, allocator, GarbageCollector::CollectionKind::Major);
	REQUIRE_FALSE(allocator.Contains(old_holder));
	REQUIRE_FALSE(allocator.Contains(young_child));
}

TEST_CASE("Minor collection without barrier does not retain unreferenced young objects", "[gc][generational]")
{
	MidoriAllocator allocator;
	GarbageCollector gc;
	gc.SetAllocator(&allocator);

	MidoriTraceable* rooted = AllocateText(allocator, gc, "rooted");
	GarbageCollector::GarbageCollectionRoots roots{ rooted };
	gc.CollectNow(roots, allocator, GarbageCollector::CollectionKind::Minor);

	MidoriTraceable* young_garbage = AllocateText(allocator, gc, "young-garbage");
	gc.CollectNow(roots, allocator, GarbageCollector::CollectionKind::Minor);

	REQUIRE(allocator.Contains(rooted));
	REQUIRE_FALSE(allocator.Contains(young_garbage));

	GarbageCollector::GarbageCollectionRoots no_roots;
	gc.CollectNow(no_roots, allocator, GarbageCollector::CollectionKind::Major);
}

TEST_CASE("Minor collection loses old-to-young edges when the barrier is skipped", "[gc][generational]")
{
	MidoriAllocator allocator;
	GarbageCollector gc;
	gc.SetAllocator(&allocator);

	MidoriTraceable* old_holder = AllocateArrayOf(allocator, gc, nullptr);
	GarbageCollector::GarbageCollectionRoots holder_roots{ old_holder };
	gc.CollectNow(holder_roots, allocator, GarbageCollector::CollectionKind::Minor);
	REQUIRE(allocator.Contains(old_holder));

	MidoriTraceable* young_text = AllocateText(allocator, gc, "young");
	// Deliberately NO WriteBarrier here: the old-to-young edge is unrecorded.
	old_holder->GetTraceable<MidoriArray>()[0] = MidoriValue(young_text);

	gc.CollectNow(holder_roots, allocator, GarbageCollector::CollectionKind::Minor);
	REQUIRE(allocator.Contains(old_holder));
	// Containment check only: young_text is dangling after the collection and
	// must never be dereferenced.
	REQUIRE_FALSE(allocator.Contains(young_text));

	GarbageCollector::GarbageCollectionRoots no_roots;
	gc.CollectNow(no_roots, allocator, GarbageCollector::CollectionKind::Major);
}

TEST_CASE("ReclaimMemory escalates to a major collection when live bytes stay high", "[gc][generational]")
{
	MidoriAllocator allocator;
	GarbageCollector gc;
	gc.SetAllocator(&allocator);

	// Each text reserves 4MB of long-buffer capacity, so registered GetSize() is
	// roughly 4MB + sizeof(MidoriTraceable) per object (MidoriText::GetCapacity()
	// reflects buffer capacity, not logical length). Three of them register
	// ~12.6MB, comfortably above INITIAL_GC_THRESHOLD (4,096,000 bytes) so a
	// non-forced ReclaimMemory actually collects, and comfortably above
	// 2 * m_live_bytes_after_major (2 * 4,096,000 = 8,192,000 bytes, since
	// m_live_bytes_after_major starts at INITIAL_GC_THRESHOLD) so the survivors
	// left live after the minor collection force an escalation to a major one.
	constexpr int RESERVE_BYTES = 4 * 1024 * 1024;
	GarbageCollector::GarbageCollectionRoots roots;
	roots.emplace_back(AllocateLargeText(allocator, gc, RESERVE_BYTES));
	roots.emplace_back(AllocateLargeText(allocator, gc, RESERVE_BYTES));
	roots.emplace_back(AllocateLargeText(allocator, gc, RESERVE_BYTES));

	REQUIRE(gc.ShouldCollect());
	REQUIRE(gc.MinorCollectionCount() == 0uz);
	REQUIRE(gc.MajorCollectionCount() == 0uz);

	gc.ReclaimMemory(roots, allocator, false);

	REQUIRE(gc.MinorCollectionCount() == 1uz);
	REQUIRE(gc.MajorCollectionCount() == 1uz);
	for (MidoriTraceable* rooted : roots)
	{
		REQUIRE(allocator.Contains(rooted));
	}

	GarbageCollector::GarbageCollectionRoots no_roots;
	gc.ReclaimMemory(no_roots, allocator, true);
	REQUIRE(gc.MajorCollectionCount() == 2uz);
	REQUIRE(allocator.LiveSlotCount() == 0uz);
}

namespace
{
	constexpr int LARGE_TEXT_RESERVE_BYTES = 4 * 1024 * 1024;

	// Roots enough 4MB texts to hold at least live_bytes, then collects, so the
	// collector's next threshold is computed from that much live data.
	GarbageCollector::GarbageCollectionRoots RootLiveBytes(MidoriAllocator& allocator, GarbageCollector& gc, size_t live_bytes)
	{
		GarbageCollector::GarbageCollectionRoots roots;
		while (gc.TotalBytesAllocated() < live_bytes)
		{
			roots.emplace_back(AllocateLargeText(allocator, gc, LARGE_TEXT_RESERVE_BYTES));
		}
		gc.ReclaimMemory(roots, allocator, true);
		return roots;
	}
}

// Regression: the threshold used to be clamped to an absolute maximum of
// 65,536,000 bytes. Once live data outgrew it, the threshold sat below the live
// bytes, ShouldCollect() stayed true, and the VM ran a full collection -- which
// freed nothing -- on every allocation check. Building ~840k union values took
// minutes instead of a tenth of a second.
TEST_CASE("Threshold stays above live bytes when live data exceeds the headroom cap", "[gc][threshold]")
{
	MidoriAllocator allocator;
	GarbageCollector gc;
	gc.SetAllocator(&allocator);

	GarbageCollector::GarbageCollectionRoots roots = RootLiveBytes(allocator, gc, GarbageCollector::MAX_GC_HEADROOM + 16uz * 1024uz * 1024uz);
	REQUIRE(gc.TotalBytesAllocated() > GarbageCollector::MAX_GC_HEADROOM);

	REQUIRE_FALSE(gc.ShouldCollect());
	AllocateText(allocator, gc, "one more small object");
	REQUIRE_FALSE(gc.ShouldCollect());

	GarbageCollector::GarbageCollectionRoots no_roots;
	gc.ReclaimMemory(no_roots, allocator, true);
	REQUIRE(allocator.LiveSlotCount() == 0uz);
}

TEST_CASE("Room before the next collection above a large heap is positive and capped", "[gc][threshold]")
{
	MidoriAllocator allocator;
	GarbageCollector gc;
	gc.SetAllocator(&allocator);

	// Twice the cap: the uncapped growth (half the live bytes) would exceed it.
	GarbageCollector::GarbageCollectionRoots roots = RootLiveBytes(allocator, gc, 2uz * GarbageCollector::MAX_GC_HEADROOM + 1uz);
	const size_t live_bytes = gc.TotalBytesAllocated();
	const size_t one_large_text = roots.back()->GetSize();

	while (!gc.ShouldCollect())
	{
		AllocateLargeText(allocator, gc, LARGE_TEXT_RESERVE_BYTES);
	}
	const size_t allocated_since_collection = gc.TotalBytesAllocated() - live_bytes;

	REQUIRE(allocated_since_collection > GarbageCollector::MAX_GC_HEADROOM - one_large_text);
	REQUIRE(allocated_since_collection <= GarbageCollector::MAX_GC_HEADROOM + one_large_text);

	GarbageCollector::GarbageCollectionRoots no_roots;
	gc.ReclaimMemory(no_roots, allocator, true);
	REQUIRE(allocator.LiveSlotCount() == 0uz);
}

TEST_CASE("Write barrier deduplicates remembered objects", "[gc][generational]")
{
	MidoriAllocator allocator;
	GarbageCollector gc;
	gc.SetAllocator(&allocator);

	MidoriTraceable* old_holder = AllocateArrayOf(allocator, gc, nullptr);
	GarbageCollector::GarbageCollectionRoots roots{ old_holder };
	gc.CollectNow(roots, allocator, GarbageCollector::CollectionKind::Minor);

	gc.WriteBarrier(old_holder);
	gc.WriteBarrier(old_holder);
	gc.WriteBarrier(old_holder);
	REQUIRE(gc.RememberedSetSize() == 1uz);

	GarbageCollector::GarbageCollectionRoots no_roots;
	gc.CollectNow(no_roots, allocator, GarbageCollector::CollectionKind::Major);
	REQUIRE(gc.RememberedSetSize() == 0uz);
}
