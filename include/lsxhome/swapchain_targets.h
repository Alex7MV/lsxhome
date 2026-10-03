#pragma once

namespace lsxhome {

/// Bookkeeping for the swap chain's render targets.
///
/// The D3D12 layer owns the actual `ID3D12Resource`/`ID3D12CPU_DESCRIPTOR_HANDLE`
/// pairs; this type records only how many back buffers are currently installed
/// so the frame path can refuse to build a transition against a target that is
/// not there.
///
/// A resize that fails (`ResizeBuffers` returns an error) leaves the pool
/// empty: the back buffers were already released, and issuing a resource
/// barrier on a null resource is undefined behaviour that ends in device
/// removal. `Ready()` is the guard the frame loop asks before touching a back
/// buffer index.
class SwapchainTargets {
public:
    /// Marks @p count back buffers as installed. Non-positive values install
    /// nothing.
    void Install(int count) noexcept {
        installed_ = count > 0 ? count : 0;
    }

    /// Drops every back buffer — called whenever the resources are released.
    void Invalidate() noexcept { installed_ = 0; }

    /// True when @p index names an installed back buffer.
    bool Ready(int index) const noexcept {
        return index >= 0 && index < installed_;
    }

    int Installed() const noexcept { return installed_; }

private:
    int installed_ = 0;
};

}  // namespace lsxhome