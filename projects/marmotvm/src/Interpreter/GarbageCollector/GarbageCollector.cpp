#include "GarbageCollector.h"
#include "Support/Attributes/Attributes.h"
#include "Support/Diagnostics/Diagnostics.h"
#include "Interpreter/Allocator/MidoriAllocator.h"

#include <algorithm>
#include <bit>
#include <optional>

#if MIDORI_DEBUG_INFO
#include "Support/Terminal/Terminal.h"

#include <chrono>
#include <format>

namespace
{
	std::string FormatTime(int64_t nanoseconds)
	{
		constexpr int64_t NS_PER_US = 1'000;
		constexpr int64_t NS_PER_MS = 1'000'000;
		constexpr int64_t NS_PER_S = 1'000'000'000;

		if (nanoseconds < NS_PER_US)
		{
			return std::format("{} ns", nanoseconds);
		}
		else if (nanoseconds < NS_PER_MS)
		{
			int64_t us = nanoseconds / NS_PER_US;
			int64_t ns = nanoseconds % NS_PER_US;
			if (ns == 0)
			{
				return std::format("{} us", us);
			}
			return std::format("{} us {} ns", us, ns);
		}
		else if (nanoseconds < NS_PER_S)
		{
			int64_t ms = nanoseconds / NS_PER_MS;
			int64_t us = (nanoseconds % NS_PER_MS) / NS_PER_US;
			if (us == 0)
			{
				return std::format("{} ms", ms);
			}
			return std::format("{} ms {} us", ms, us);
		}
		else
		{
			int64_t s = nanoseconds / NS_PER_S;
			int64_t ms = (nanoseconds % NS_PER_S) / NS_PER_MS;
			if (ms == 0)
			{
				return std::format("{} s", s);
			}
			return std::format("{} s {} ms", s, ms);
		}
	}

	std::string FormatBytes(size_t bytes)
	{
		constexpr double KB = 1024.0;
		constexpr double MB = 1024.0 * KB;
		constexpr double GB = 1024.0 * MB;

		if (bytes < 1024)
		{
			return std::format("{} bytes", bytes);
		}
		else if (bytes < 1024 * 1024)
		{
			double kb = static_cast<double>(bytes) / KB;
			return std::format("{:.2f} KB", kb);
		}
		else if (bytes < 1024 * 1024 * 1024)
		{
			double mb = static_cast<double>(bytes) / MB;
			return std::format("{:.2f} MB", mb);
		}
		else
		{
			double gb = static_cast<double>(bytes) / GB;
			return std::format("{:.2f} GB", gb);
		}
	}
}
#endif

bool GarbageCollector::Contains(MidoriTraceable* ptr) const
{
	return m_allocator != nullptr && m_allocator->Contains(ptr);
}

void GarbageCollector::WriteBarrier(MidoriTraceable* target) noexcept
{
	if (m_allocator == nullptr)
	{
		return;
	}

	const size_t slot_index = m_allocator->SlotIndexOf(target);
	const size_t word_index = slot_index / 64uz;
	const uint64_t mask = 1ull << (slot_index % 64uz);
	if (word_index >= m_mark_bits.size() || (m_mark_bits[word_index] & mask) == 0ull)
	{
		return;
	}
	if (word_index < m_logged_bits.size() && (m_logged_bits[word_index] & mask) != 0ull)
	{
		return;
	}
	Remember(target, word_index, mask);
}

void GarbageCollector::Remember(MidoriTraceable* target, size_t word_index, uint64_t mask) noexcept
{
	if (m_logged_bits.size() < m_mark_bits.size())
	{
		m_logged_bits.resize(m_mark_bits.size(), 0ull);
	}
	m_logged_bits[word_index] |= mask;
	m_remembered_set.emplace_back(target);
}

MIDORI_FORCE_INLINE void GarbageCollector::TryMark(MidoriTraceable* child_ptr)
{
	const std::optional<size_t> slot_index = m_allocator->TryGetSlotIndex(child_ptr);
	if (!slot_index.has_value())
	{
		return;
	}

	const size_t word_index = *slot_index / 64uz;
	const uint64_t mask = 1ull << (*slot_index % 64uz);
	if ((m_allocator->LiveBitWords()[word_index] & mask) == 0ull || (m_mark_bits[word_index] & mask) != 0ull)
	{
		return;
	}

#if MIDORI_DEBUG_FULL
	if (RuntimeDiagnostics::GcMetricsEnabled())
	{
		RuntimeDiagnostics::Print(std::format("Marking traceable pointer: {:p}\n", static_cast<void*>(child_ptr)));
	}
#endif

	m_mark_bits[word_index] |= mask;
	m_mark_stack.push_back(child_ptr);
}

void GarbageCollector::Trace(const GarbageCollectionRoots& roots)
{
	m_mark_stack.clear();

	auto mark_tuple_values = [this](MidoriTuple& tuple)
		{
			const int length = tuple.GetLength();
			for (int idx = 0; idx < length; idx += 1)
			{
				TryMark(tuple[idx].GetPointer());
			}
		};

	for (MidoriTraceable* root : roots)
	{
		TryMark(root);
	}

	m_mark_stack.insert(m_mark_stack.end(), m_remembered_set.begin(), m_remembered_set.end());

	while (!m_mark_stack.empty())
	{
		MidoriTraceable* current = m_mark_stack.back();
		m_mark_stack.pop_back();

		switch (current->GetType())
		{
		case MidoriTraceable::TraceableType::Array:
		{
			MidoriArray& arr = current->GetTraceable<MidoriArray>();
			const int length = arr.GetLength();
			for (int idx = 0; idx < length; idx += 1)
			{
				TryMark(arr[idx].GetPointer());
			}
			break;
		}
		case MidoriTraceable::TraceableType::Tuple:
			mark_tuple_values(current->GetTraceable<MidoriTuple>());
			break;
		case MidoriTraceable::TraceableType::Closure:
			mark_tuple_values(current->GetTraceable<MidoriClosure>().m_cell_values);
			break;
		case MidoriTraceable::TraceableType::Cell:
			TryMark(current->GetTraceable<MidoriCellValue>().GetValue().GetPointer());
			break;
		case MidoriTraceable::TraceableType::MutableCell:
			TryMark(current->GetTraceable<MidoriMutableCell>().m_value.GetPointer());
			break;
		case MidoriTraceable::TraceableType::Struct:
			mark_tuple_values(current->GetTraceable<MidoriStruct>().m_values);
			break;
		case MidoriTraceable::TraceableType::Union:
			mark_tuple_values(current->GetTraceable<MidoriUnion>().m_values);
			break;
		case MidoriTraceable::TraceableType::Text:
		case MidoriTraceable::TraceableType::IntRange:
		case MidoriTraceable::TraceableType::FloatRange:
			break;
		}
	}
}

void GarbageCollector::Sweep(MidoriAllocator& allocator, size_t& sweep_count, size_t& bytes_reclaimed)
{
	const uint64_t* live_words = allocator.LiveBitWords();
	const size_t word_count = allocator.SlotWordCount();

	for (size_t word_index = 0uz; word_index < word_count; word_index += 1uz)
	{
		uint64_t garbage = live_words[word_index] & ~m_mark_bits[word_index];
		while (garbage != 0ull)
		{
			const size_t bit = static_cast<size_t>(std::countr_zero(garbage));
			garbage &= garbage - 1ull;

			const size_t slot_index = word_index * 64uz + bit;
			MidoriTraceable* ptr = static_cast<MidoriTraceable*>(allocator.SlotAt(slot_index));
			const size_t registered_size = ptr->GetSize();
			bytes_reclaimed += registered_size;
			m_total_bytes_allocated -= std::min(registered_size, m_total_bytes_allocated);
			sweep_count += 1uz;

			ptr->~MidoriTraceable();
			allocator.Free(ptr, slot_index);
		}
	}
}

void GarbageCollector::ClearRememberedSet() noexcept
{
	m_remembered_set.clear();
	std::fill(m_logged_bits.begin(), m_logged_bits.end(), 0ull);
}

void GarbageCollector::CollectNow(const GarbageCollectionRoots& roots, MidoriAllocator& allocator, CollectionKind kind)
{
	// Captured before any ClearRememberedSet() call in this function (the Major-path clear
	// below, or the Minor-path clear after sweep) so the debug telemetry below reports the
	// remembered-set size that was actually traced, not zero.
	const size_t remembered_count = m_remembered_set.size();

#if MIDORI_DEBUG_INFO
	const bool emit_gc_diagnostics = RuntimeDiagnostics::GcMetricsEnabled();
	using Clock = std::chrono::high_resolution_clock;
	using TimePoint = Clock::time_point;
	TimePoint t0{};
	TimePoint t_mark_start{};
	TimePoint t_mark_end{};
	TimePoint t_sweep_start{};
	TimePoint t_sweep_end{};
	if (emit_gc_diagnostics)
	{
		RuntimeDiagnostics::Print("\n----------------------------------------------\nBefore garbage collection:");
		PrintMemoryTelemetry();
		t0 = Clock::now();
	}
#endif

#if MIDORI_DEBUG_INFO
	if (emit_gc_diagnostics)
	{
		t_mark_start = Clock::now();
	}
#endif
	// Mark. Sticky: resize preserves existing mark bits and zero-fills only new
	// words, so blocks committed since the last collection start out young.
	m_mark_bits.resize(allocator.SlotWordCount(), 0ull);

	if (kind == CollectionKind::Major)
	{
		std::fill(m_mark_bits.begin(), m_mark_bits.end(), 0ull);
		ClearRememberedSet();
		m_major_collection_count += 1uz;
	}
	else
	{
		m_minor_collection_count += 1uz;
	}

	Trace(roots);
#if MIDORI_DEBUG_INFO
	if (emit_gc_diagnostics)
	{
		t_mark_end = Clock::now();
	}
#endif

	size_t sweep_count = 0uz;
	size_t bytes_reclaimed = 0uz;
#if MIDORI_DEBUG_INFO
	if (emit_gc_diagnostics)
	{
		t_sweep_start = Clock::now();
	}
#endif

	// Sweep
	Sweep(allocator, sweep_count, bytes_reclaimed);

	if (kind == CollectionKind::Minor)
	{
		// Young targets of remembered objects were promoted during the trace,
		// so the remembered set can be safely cleared after a minor collection.
		ClearRememberedSet();
	}
	else
	{
		m_live_bytes_after_major = std::max(m_total_bytes_allocated, MIN_GC_THRESHOLD);
		m_major_growth = bytes_reclaimed * 4uz < m_total_bytes_allocated + bytes_reclaimed ? MAJOR_GROWTH_WHILE_GROWING : MAJOR_GROWTH;
	}

#if MIDORI_DEBUG_INFO
	if (emit_gc_diagnostics)
	{
		t_sweep_end = Clock::now();
		TimePoint t1 = Clock::now();

		int64_t ns_mark = std::chrono::duration_cast<std::chrono::nanoseconds>(t_mark_end - t_mark_start).count();
		int64_t ns_sweep = std::chrono::duration_cast<std::chrono::nanoseconds>(t_sweep_end - t_sweep_start).count();
		int64_t ns_total = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

		RuntimeDiagnostics::Print
			(
				std::format
				(
					"\n[GC] Kind:         {}\n"
					"[GC] Minor count:  {}\n"
					"[GC] Major count:  {}\n"
					"[GC] Mark time:    {}\n"
					"[GC] Sweep time:   {}\n"
					"[GC] Total time:   {}\n"
					"[GC] Roots traced: {}\n"
					"[GC] Remembered:   {}\n"
					"[GC] Survivors:    {}\n"
					"[GC] Collected:    {} ({})\n",
					kind == CollectionKind::Major ? "Major" : "Minor",
					m_minor_collection_count,
					m_major_collection_count,
					FormatTime(ns_mark),
					FormatTime(ns_sweep),
					FormatTime(ns_total),
					roots.size(),
					remembered_count,
					allocator.LiveSlotCount(),
					sweep_count,
					FormatBytes(bytes_reclaimed)
				)
			);
		RuntimeDiagnostics::Print("\nAfter garbage collection:");
		PrintMemoryTelemetry();
	}
#endif

	// Grow geometrically, but bound the growth rather than the threshold. Clamping
	// the threshold itself to a maximum put it below the live bytes once the heap
	// outgrew that maximum, so ShouldCollect() stayed true and every allocation
	// check ran a collection that could free nothing.
	size_t headroom = static_cast<size_t>(static_cast<double>(m_total_bytes_allocated) * (GC_GROWTH_FACTOR - 1.0));
	headroom = std::min(headroom, MAX_GC_HEADROOM);
	m_gc_threshold = std::max(m_total_bytes_allocated + headroom, MIN_GC_THRESHOLD);
}

void GarbageCollector::ReclaimMemory(const GarbageCollectionRoots& roots, MidoriAllocator& allocator, bool force_clean)
{
	if (m_total_bytes_allocated < m_gc_threshold && !force_clean)
	{
		return;
	}

	if (force_clean)
	{
		CollectNow(roots, allocator, CollectionKind::Major);
		return;
	}

	CollectNow(roots, allocator, CollectionKind::Minor);
	if (m_total_bytes_allocated > m_major_growth * m_live_bytes_after_major)
	{
		CollectNow(roots, allocator, CollectionKind::Major);
	}
}

#if MIDORI_DEBUG_INFO
void GarbageCollector::PrintMemoryTelemetry()
{
	RuntimeDiagnostics::Print
		(
			std::format
			(
				"Total allocated: {}\nObject count:    {}\n----------------------------------------------\n",
				FormatBytes(m_total_bytes_allocated),
				m_allocator != nullptr ? m_allocator->LiveSlotCount() : 0uz
			)
		);
}
#endif
