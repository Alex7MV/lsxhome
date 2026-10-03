#include <catch2/catch_test_macros.hpp>

#include "lsxhome/blackwell_theme.h"
#include "lsxhome/gui_bridge.h"
#include "lsxhome/spsc_token_ring.h"
#include "lsxhome/srv_descriptor_pool.h"
#include "lsxhome/swapchain_targets.h"
#include "lsxhome/text_splitter.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

using lsxhome::GuiBridge;
using lsxhome::MakeFallbackTokenPayload;
using lsxhome::MakeTokenPayload;
using lsxhome::SpscTokenRing;
using lsxhome::SrvDescriptorPool;
using lsxhome::SplitOnSpaces;
using lsxhome::SwapchainTargets;
using lsxhome::TokenPayload;
using lsxhome::TokenTextFits;

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

TEST_CASE("lsxhome: token payload carries a producer-side steady-clock stamp",
          "[lsxhome][gui]") {
    // ns_stamp is documented as the producer-side steady-clock stamp, so a
    // freshly built payload must carry a real reading — never the zeroed
    // default that would silently read as "no latency data".
    const TokenPayload first = MakeTokenPayload(kParisTokenId, 0, "Paris");
    REQUIRE(first.ns_stamp > 0);

    // Stamps must advance with the producer, never move backwards.
    const TokenPayload second = MakeTokenPayload(kParisTokenId, 1, "Paris");
    REQUIRE(second.ns_stamp >= first.ns_stamp);
}

TEST_CASE("lsxhome: space splitting never emits empty words", "[lsxhome][text]") {
    // The producer used to split on a bare ' ' scan, so a double space or a
    // leading space published an empty token straight into the UI stream.
    std::string_view words[8];

    REQUIRE(SplitOnSpaces("alpha beta gamma", words, 8) == 3);
    REQUIRE(words[0] == "alpha");
    REQUIRE(words[1] == "beta");
    REQUIRE(words[2] == "gamma");

    REQUIRE(SplitOnSpaces("alpha  beta", words, 8) == 2);
    REQUIRE(words[0] == "alpha");
    REQUIRE(words[1] == "beta");

    REQUIRE(SplitOnSpaces("   alpha", words, 8) == 1);
    REQUIRE(words[0] == "alpha");

    REQUIRE(SplitOnSpaces("alpha   ", words, 8) == 1);
    REQUIRE(words[0] == "alpha");

    // Whitespace-only and empty input yield no words at all.
    REQUIRE(SplitOnSpaces("     ", words, 8) == 0);
    REQUIRE(SplitOnSpaces("", words, 8) == 0);
}

TEST_CASE("lsxhome: space splitting honours the output capacity", "[lsxhome][text]") {
    std::string_view words[2];
    REQUIRE(SplitOnSpaces("a b c d", words, 2) == 2);
    REQUIRE(words[0] == "a");
    REQUIRE(words[1] == "b");

    std::string_view none[1];
    REQUIRE(SplitOnSpaces("a b", none, 0) == 0);
}

TEST_CASE("lsxhome: a refused drain neither writes nor loses tokens", "[lsxhome][gui]") {
    // DrainAll wrote straight into the caller buffer, so a null destination
    // was an access violation rather than a refusal.
    GuiBridge bridge;
    REQUIRE(bridge.Publish(MakeTokenPayload(kParisTokenId, 0, "Paris")));

    REQUIRE(bridge.DrainAll(nullptr, 8) == 0);

    // Refusing must not consume: the token is still available to the UI.
    TokenPayload out;
    REQUIRE(bridge.Poll(out));
    REQUIRE(out.token_id == kParisTokenId);

    TokenPayload buf[4];
    REQUIRE(bridge.DrainAll(buf, 0) == 0);
}

TEST_CASE("lsxhome: callers can detect a token payload that would truncate",
          "[lsxhome][gui]") {
    // The payload is a fixed one-cache-line slot, so a long word is cut at
    // runtime. MakeTokenPayload cannot report that through its signature, so
    // the capacity has to be discoverable before the call.
    REQUIRE(TokenTextFits(""));
    REQUIRE(TokenTextFits("Paris"));

    // 31 characters plus the NUL terminator exactly fills text[32].
    const std::string longest(lsxhome::kTokenTextCapacity - 1, 'x');
    REQUIRE(TokenTextFits(longest));

    const std::string too_long(lsxhome::kTokenTextCapacity, 'x');
    REQUIRE_FALSE(TokenTextFits(too_long));
}

TEST_CASE("lsxhome: a bare launch falls back to the Paris self-check token",
          "[lsxhome][gui]") {
    // No model was requested, so there is nothing to generate from: the shell
    // publishes the deterministic GLM-5.2 self-check token. This used to be
    // unreachable, because the prompt is never empty — ResolveGenerationPrompt
    // substitutes the engine's default blueprint prompt.
    const TokenPayload demo = MakeFallbackTokenPayload("", false);
    REQUIRE(demo.token_id == 11751);
    REQUIRE(std::strcmp(demo.text, "Paris") == 0);

    const TokenPayload demo_with_default = MakeFallbackTokenPayload("blueprint", false);
    REQUIRE(demo_with_default.token_id == 11751);
    REQUIRE(std::strcmp(demo_with_default.text, "Paris") == 0);
}

TEST_CASE("lsxhome: a failed model run never mislabels the echoed prompt as Paris",
          "[lsxhome][gui]") {
    // A model was requested but produced nothing. The prompt must come back so
    // the user sees it — tagged with the unknown-id 0, because id 11751 really
    // does mean "Paris" and stamping it on arbitrary text is a lie.
    const TokenPayload echoed = MakeFallbackTokenPayload("why is the sky blue", true);
    REQUIRE(echoed.token_id == 0);
    REQUIRE(std::strcmp(echoed.text, "why is the sky blue") == 0);
    REQUIRE(echoed.ns_stamp > 0);
}

TEST_CASE("lsxhome: an invalidated back-buffer pool refuses every index",
          "[lsxhome][renderer]") {
    // Regression guard for the audited resize crash: ResizeBuffers failure left
    // the back buffers reset, yet EndFrame still built a PRESENT->RENDER_TARGET
    // barrier against a null resource. The frame path must be able to ask
    // whether a back buffer exists before touching it.
    SwapchainTargets targets;
    targets.Install(3);
    REQUIRE(targets.Ready(2));

    targets.Invalidate();
    REQUIRE(targets.Installed() == 0);
    REQUIRE_FALSE(targets.Ready(0));
    REQUIRE_FALSE(targets.Ready(1));
    REQUIRE_FALSE(targets.Ready(2));
}

TEST_CASE("lsxhome: installed back buffers are exactly the reported range",
          "[lsxhome][renderer]") {
    SwapchainTargets fresh;
    REQUIRE(fresh.Installed() == 0);
    REQUIRE_FALSE(fresh.Ready(0));

    SwapchainTargets targets;
    targets.Install(3);
    REQUIRE(targets.Installed() == 3);
    REQUIRE(targets.Ready(0));
    REQUIRE(targets.Ready(1));
    REQUIRE(targets.Ready(2));
    REQUIRE_FALSE(targets.Ready(3));
    REQUIRE_FALSE(targets.Ready(-1));
}

TEST_CASE("lsxhome: a freed SRV descriptor slot returns to the pool",
          "[lsxhome][renderer]") {
    // Regression guard for the audited leak: the renderer's descriptor
    // free-list used to drop the slot on Free, so the heap drained for the
    // lifetime of the process. A freed slot must be allocatable again.
    SrvDescriptorPool pool;
    pool.Reset(2);

    int a = -1;
    int b = -1;
    REQUIRE(pool.Alloc(a));
    REQUIRE(pool.Alloc(b));
    REQUIRE(pool.InUseCount() == 2);

    int exhausted = -1;
    REQUIRE_FALSE(pool.Alloc(exhausted));  // both slots are live

    REQUIRE(pool.Free(a));
    REQUIRE(pool.InUseCount() == 1);

    int recycled = -1;
    REQUIRE(pool.Alloc(recycled));         // the freed slot came back
    REQUIRE(pool.InUseCount() == 2);
    REQUIRE((recycled == a || recycled == b));
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