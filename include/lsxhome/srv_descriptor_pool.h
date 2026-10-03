#pragma once

namespace lsxhome {

/// Free-list over the shader-visible CBV/SRV/UAV descriptor heap.
///
/// The D3D12 layer keeps the descriptor-handle arithmetic; this pool owns only
/// the slot bookkeeping, so the alloc/free contract is verifiable without a
/// device. Every `Free` MUST hand its slot back to the pool: a dropped slot
/// permanently shrinks the heap and eventually starves the renderer, because
/// the heap is created once per window and never grows.
class SrvDescriptorPool {
public:
    /// Upper bound on descriptor slots; sized to the shader-visible heap the
    /// renderer carves for the font atlas and any future dynamic textures.
    static constexpr int kMaxDescriptors = 256;

    /// Rebuilds the pool with @p capacity slots, all free. Values outside
    /// [0, kMaxDescriptors] are clamped.
    void Reset(int capacity) noexcept {
        if (capacity < 0) {
            capacity = 0;
        }
        if (capacity > kMaxDescriptors) {
            capacity = kMaxDescriptors;
        }
        capacity_ = capacity;
        for (int i = capacity_ - 1; i >= 0; --i) {
            free_list_[i] = i;
        }
        free_count_ = capacity_;
    }

    /// Takes the next free slot. False means the pool is exhausted — the
    /// caller must decide between recycling and refusing the resource.
    bool Alloc(int& index) noexcept {
        if (free_count_ == 0) {
            return false;
        }
        index = free_list_[--free_count_];
        return true;
    }

    /// Returns a slot handed out by `Alloc`. False when @p index never came
    /// from this pool's range.
    bool Free(int index) noexcept {
        if (index < 0 || index >= capacity_) {
            return false;
        }
        free_list_[free_count_++] = index;
        return true;
    }

    int InUseCount() const noexcept { return capacity_ - free_count_; }

private:
    int free_list_[kMaxDescriptors] = {};
    int free_count_ = 0;
    int capacity_  = 0;
};

}  // namespace lsxhome
