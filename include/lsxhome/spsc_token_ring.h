#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // intentional alignas(64) slot padding
#endif

namespace lsxhome {

/// Lock-free single-producer / single-consumer token ring buffer feeding the
/// GUI thread from the GPU compute thread.
///
/// ZERO-ALLOC: the ring owns a fixed, pre-allocated pool of `Capacity` slots
/// (cache-line aligned) carved once at construction. `TryPush` / `TryPop` are
/// non-blocking, lock-free, syscall-free and never touch the heap — the UI
/// render loop polls an empty ring instead of blocking the message pump.
///
/// The handoff uses the Vyukov per-slot sequence protocol mirrored from
/// `lsxnetwork::FrameQueue`: a slot's `seq` is the monotonic token count of
/// its next valid access. Single ownership is enforced by construction —
/// exactly one producer (compute thread) and exactly one consumer (UI thread).
template <typename T, std::size_t Capacity, std::size_t SlotAlign = 64>
class SpscTokenRing {
    static_assert(Capacity >= 2, "ring capacity must be >= 2 for the sequence protocol");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "ring capacity must be a power of two");
    static_assert(std::is_nothrow_copy_constructible_v<T>,
                  "payload must be no-throw copy constructible");

public:
    static constexpr std::size_t kCapacity     = Capacity;
    static constexpr std::size_t kSlotAlignment = SlotAlign;

    SpscTokenRing() noexcept {
        for (std::size_t i = 0; i < kCapacity; ++i) {
            slots_[i].seq.store(static_cast<std::uint64_t>(i),
                                std::memory_order_relaxed);
        }
    }
    ~SpscTokenRing() noexcept = default;

    SpscTokenRing(const SpscTokenRing&)            = delete;
    SpscTokenRing& operator=(const SpscTokenRing&) = delete;

    /// Producer (compute thread): stores @p item into the next free slot and
    /// publishes it. False means the pool is full — deterministic backpressure,
    /// the caller retries on the next frame. Never blocks.
    bool TryPush(const T& item) noexcept {
        const std::size_t n = head_.load(std::memory_order_relaxed);
        const std::size_t slot = n & kMask;
        if (slots_[slot].seq.load(std::memory_order_acquire) < n) {
            return false;
        }
        slots_[slot].item = item;
        slots_[slot].seq.store(n + 1, std::memory_order_release);
        head_.store(n + 1, std::memory_order_relaxed);
        return true;
    }

    /// Consumer (UI thread): pops the oldest published token. False means the
    /// pool is empty — drain whatever is available and present the frame.
    bool TryPop(T& out) noexcept {
        const std::size_t n = tail_.load(std::memory_order_relaxed);
        const std::size_t slot = n & kMask;
        if (slots_[slot].seq.load(std::memory_order_acquire) != n + 1) {
            return false;
        }
        out = slots_[slot].item;
        slots_[slot].seq.store(n + kCapacity, std::memory_order_release);
        tail_.store(n + 1, std::memory_order_relaxed);
        return true;
    }

    bool Empty() const noexcept {
        const std::size_t n = tail_.load(std::memory_order_relaxed);
        return slots_[n & kMask].seq.load(std::memory_order_acquire) != n + 1;
    }

    bool Full() const noexcept {
        const std::size_t n = head_.load(std::memory_order_relaxed);
        return slots_[n & kMask].seq.load(std::memory_order_acquire) < n;
    }

    static constexpr std::size_t Capacity() noexcept { return kCapacity; }

private:
    struct alignas(SlotAlign) Slot {
        std::atomic<std::uint64_t> seq;
        T item;
    };

    static constexpr std::size_t kMask = kCapacity - 1;

    alignas(64) std::atomic<std::size_t> head_{0};  // producer (write) index
    alignas(64) std::atomic<std::size_t> tail_{0};  // consumer (read) index
    Slot slots_[kCapacity];
};

}  // namespace lsxhome

#if defined(_MSC_VER)
#pragma warning(pop)
#endif