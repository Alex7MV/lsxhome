#include <catch2/catch_test_macros.hpp>

#include "lsxhome/blackwell_theme.h"
#include "lsxhome/gui_bridge.h"
#include "lsxhome/spsc_token_ring.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

using lsxhome::GuiBridge;
using lsxhome::MakeTokenPayload;
using lsxhome::SpscTokenRing;
using lsxhome::TokenPayload;

// Deterministic GLM-5.2 self-check token: 11751 decodes to "Paris".
constexpr std::uint32_t kParisTokenId = 11751;

// ────────────────────────────────────────────────────────────────────────────
// Counting heap allocator: overrides the global (replacement) operator
// new/delete and tallies call sites into a process-wide atomic. The GUI bridge
// fast path must never touch it — the tests diff the counter across a token
// burst to prove the SPSC pool is pre-allocated and leak-free (the exact
// pattern used by tests/test_token_translator.cpp).
// ────────────────────────────────────────────────────────────────────────────
namespace {
std::atomic<long long> g_alloc_count{0};
}  // namespace

void* operator new(std::size_t n) {
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void*, std::size_t) noexcept {}
void operator delete[](void*, std::size_t) noexcept {}

TEST_CASE("lsxhome: token payload fits one aligned cache-line slot", "[lsxhome][gui]") {
    STATIC_REQUIRE(sizeof(TokenPayload) == 64);
    STATIC_REQUIRE(alignof(TokenPayload) == 64);
    STATIC_REQUIRE(std::is_trivially_copyable_v<TokenPayload>);
    REQUIRE(sizeof(TokenPayload) == 64);
    REQUIRE(alignof(TokenPayload) == 64);
}

TEST_CASE("lsxhome: deterministic 11751->'Paris' round-trips through the bridge",
          "[lsxhome][gui]") {
    GuiBridge bridge;

    TokenPayload in = MakeTokenPayload(kParisTokenId, 0, "Paris");
    REQUIRE(bridge.Publish(in));

    TokenPayload out;
    REQUIRE(bridge.Poll(out));
    REQUIRE(out.token_id == kParisTokenId);
    REQUIRE(out.stream_seq == 0);
    REQUIRE(std::strcmp(out.text, "Paris") == 0);
}

TEST_CASE("lsxhome: SPSC pool is pre-allocated and never allocates on publish",
          "[lsxhome][gui]") {
    GuiBridge bridge;

    const long long before = g_alloc_count.load(std::memory_order_relaxed);
    std::uint32_t seq = 0;

    // Fill the entire pre-allocated pool.
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < GuiBridge::kPoolSlots; ++i) {
        if (bridge.Publish(MakeTokenPayload(kParisTokenId, seq++, "Paris"))) {
            ++accepted;
        }
    }
    REQUIRE(accepted == GuiBridge::kPoolSlots);
    REQUIRE(bridge.Published() == GuiBridge::kPoolSlots);

    // Over-subscription: the 1025th slot must be deterministically refused —
    // the pool never grows, so the token is dropped, not leaked.
    REQUIRE_FALSE(bridge.Publish(MakeTokenPayload(kParisTokenId, seq, "Paris")));
    REQUIRE(bridge.Dropped() == 1);

    // Drain: slots come back in FIFO order — the same pooled slots, no new
    // memory. The allocator counter must be unchanged: zero leakage, zero
    // runtime allocation on the publish/drain hot path.
    TokenPayload out;
    std::size_t drained = 0;
    while (bridge.Poll(out)) {
        REQUIRE(out.token_id == kParisTokenId);
        REQUIRE(out.stream_seq == drained);
        ++drained;
    }
    REQUIRE(drained == GuiBridge::kPoolSlots);
    REQUIRE(bridge.ring().Empty());

    const long long after = g_alloc_count.load(std::memory_order_relaxed);
    REQUIRE(after - before == 0);
}

TEST_CASE("lsxhome: concurrent compute->UI burst has no stalls and zero drops",
          "[lsxhome][gui]") {
    constexpr std::size_t kBurst = 1000000;
    GuiBridge bridge;

    std::atomic<std::size_t> published{0};

    // Producer: compute-thread stand-in. When the pool is momentarily full it
    // yields and retries the SAME token until the consumer frees a slot —
    // never blocks/locks, never drops, and the task is time-bounded below.
    std::thread producer([&] {
        std::uint32_t seq = 0;
        while (published.load(std::memory_order_relaxed) < kBurst) {
            TokenPayload t =
                MakeTokenPayload(kParisTokenId, seq++, "Paris");
            while (!bridge.Submit(t)) {
                std::this_thread::yield();  // SPSC backpressure: retry, never block
            }
            published.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Consumer: UI-thread stand-in, hungry loop mimicking per-frame DrainAll.
    const auto t0 = std::chrono::steady_clock::now();
    std::size_t seen = 0;
    while (seen < kBurst) {
        TokenPayload buf[512];
        const std::size_t n = bridge.DrainAll(buf, 512);
        for (std::size_t i = 0; i < n; ++i) {
            REQUIRE(buf[i].token_id == kParisTokenId);
            ++seen;
        }
        if (n == 0) {
            std::this_thread::yield();
        }
        // Stall watchdog: blocked SPSC would never make forward progress.
        const auto el = std::chrono::steady_clock::now() - t0;
        REQUIRE(el < std::chrono::seconds{10});
    }

    producer.join();

    REQUIRE(seen == kBurst);
    REQUIRE(published.load(std::memory_order_relaxed) == kBurst);
    REQUIRE(bridge.Dropped() == 0);  // consumer kept up -> zero backpressure drops
    REQUIRE(bridge.Published() == kBurst);
    REQUIRE(bridge.ring().Empty());
}

TEST_CASE("lsxhome: standalone SPSC ring backpressures to one slot", "[lsxhome][gui]") {
    // Fully unlocked pool: pushes beyond capacity must be rejected without
    // stomping the oldest un-consumed slot.
    using RingT = SpscTokenRing<TokenPayload, 8>;
    RingT ring;

    for (std::size_t i = 0; i < RingT::kCapacity; ++i) {
        REQUIRE(ring.TryPush(
            MakeTokenPayload(kParisTokenId, static_cast<std::uint32_t>(i),
                             "Paris")));
    }
    REQUIRE(ring.Full());

    TokenPayload out;
    REQUIRE(ring.TryPop(out));
    REQUIRE(out.stream_seq == 0);

    // After the pop exactly one free slot: re-push succeeds, the next must not.
    REQUIRE(ring.TryPush(MakeTokenPayload(kParisTokenId, 100u, "Paris")));
    REQUIRE_FALSE(ring.TryPush(MakeTokenPayload(kParisTokenId, 101u, "Paris")));
}