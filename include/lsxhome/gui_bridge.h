#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "spsc_token_ring.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // intentional alignas(64) padding
#endif

namespace lsxhome {

/// A single decoded token flowing from the compute pipeline into the UI.
///
/// Fixed-size (one cache line), trivially copyable — safe for the lock-free
/// ring slot. `token_id` is the model vocabulary id; `text` holds the decoded
/// bytes (e.g. token 11751 -> "Paris" for the GLM-5.2 checkpoint).
struct alignas(64) TokenPayload {
    std::uint32_t token_id = 0;   // vocabulary token id
    std::uint32_t stream_seq = 0; // monotonic publish order for UI replay
    std::int64_t  ns_stamp = 0;   // producer-side steady-clock stamp
    char          text[32] = {};  // decoded token span, NUL-terminated
};
static_assert(sizeof(TokenPayload) == 64, "TokenPayload must stay on one cache line");
static_assert(std::is_trivially_copyable_v<TokenPayload>,
              "TokenPayload must be trivially copyable for the lock-free slot");

inline TokenPayload MakeTokenPayload(std::uint32_t token_id,
                                     std::uint32_t stream_seq,
                                     const char* text) noexcept {
    TokenPayload p;
    std::memset(&p, 0, sizeof(p));
    p.token_id = token_id;
    p.stream_seq = stream_seq;
    std::snprintf(p.text, sizeof(p.text), "%s", text ? text : "");
    return p;
}

/// The compute -> UI handoff. Thin, header-only wrapper over the lock-free
/// SPSC ring; the UI thread owns the only consumer instance, the GPU compute
/// thread owns the only producer one.
class GuiBridge {
public:
    static constexpr std::size_t kPoolSlots = 1024;  // power of two

    using Ring = SpscTokenRing<TokenPayload, kPoolSlots>;

    GuiBridge() noexcept = default;
    ~GuiBridge() noexcept = default;

    GuiBridge(const GuiBridge&)            = delete;
    GuiBridge& operator=(const GuiBridge&) = delete;

    /// GPU compute thread side. Non-blocking backpressure attempt. On a full
    /// pool it leaves the token to the caller: the compute stream either retries
    /// `Submit` for the same token (UI keeps up, never loses a token) or calls
    /// `Publish` to drop-and-count. Never blocks the compute stream.
    bool Submit(const TokenPayload& token) noexcept {
        if (ring_.TryPush(token)) {
            published_.fetch_add(1, std::memory_order_release);
            return true;
        }
        return false;
    }

    /// GPU compute thread side, drop-accepting variant: a full pool determin-
    /// istically drops the token and counts it (the "Drop" contract used by the
    /// live shell — the caller has already committed to not blocking). Returns
    /// false if the pool refused and the token was counted as dropped.
    bool Publish(const TokenPayload& token) noexcept {
        if (ring_.TryPush(token)) {
            published_.fetch_add(1, std::memory_order_release);
            return true;
        }
        dropped_.fetch_add(1, std::memory_order_release);
        return false;
    }

    /// UI thread side. Drains at most one token; false when empty.
    bool Poll(TokenPayload& out) noexcept {
        return ring_.TryPop(out);
    }

    /// UI thread side: drain-everything snapshot for one frame.
    std::size_t DrainAll(TokenPayload* out, std::size_t cap) noexcept {
        std::size_t n = 0;
        TokenPayload t;
        while (n < cap && ring_.TryPop(t)) {
            out[n++] = t;
        }
        return n;
    }

    std::size_t Published() const noexcept {
        return published_.load(std::memory_order_acquire);
    }

    std::size_t Dropped() const noexcept {
        return dropped_.load(std::memory_order_acquire);
    }

    const Ring& ring() const noexcept { return ring_; }

private:
    Ring ring_;
    std::atomic<std::size_t> published_{0};
    std::atomic<std::size_t> dropped_{0};
};

static_assert(std::is_trivially_copyable_v<TokenPayload>);
static_assert(std::is_trivially_destructible_v<TokenPayload>);

}  // namespace lsxhome

#if defined(_MSC_VER)
#pragma warning(pop)
#endif