#pragma once

struct HWND__;
typedef HWND__* HWND;

namespace lsxhome {

/// DirectX 12 hardware-accelerated renderer for the lsxhome desktop shell.
///
/// Facade over a module-local D3D12 engine state (device, command queue, swap
/// chain, RTV heap, shader-visible SRV heap + SRV free-list allocator, per-
/// frame command allocators and fences). Owns the Dear ImGui frame plumbing
/// (ImGui_ImplWin32 + ImGui_ImplDX12). The UI thread drives the full frame;
/// compute results arrive only through the lock-free `GuiBridge`, never through
/// direct renderer access. imgui_freetype's dynamic font-atlas texture gets its
/// SRV slot out of the static shader-visible descriptor heap.
class D3D12Renderer {
public:
    static constexpr int kBackBufferCount = 3;    // FLIP_DISCARD requires >= 2
    static constexpr int kFramesInFlight  = 3;    // CPU/GPU overlap depth
    static constexpr int kSrvPoolDescriptors = 256;

    D3D12Renderer() noexcept = default;
    ~D3D12Renderer() noexcept { Shutdown(); }

    D3D12Renderer(const D3D12Renderer&)            = delete;
    D3D12Renderer& operator=(const D3D12Renderer&) = delete;

    /// Creates the device, swap chain and ImGui context for @p hwnd.
    /// Returns 0 on success, -1 on any D3D/ImGui init failure.
    int Init(HWND hwnd, int width, int height) noexcept;
    void Shutdown() noexcept;

    /// Begins an ImGui frame and waits for the oldest in-flight frame fence.
    /// Returns false if the swap chain is in a discard-resized state.
    bool BeginFrame() noexcept;
    void EndFrame(bool present) noexcept;

    /// Handles WM_SIZE -> swaps to the new client size (recreates RT views).
    void Resize(int width, int height) noexcept;

    int  Width() const noexcept { return width_; }
    int  Height() const noexcept { return height_; }
    bool Initialized() const noexcept { return initialized_; }

private:
    HWND    hwnd_      = nullptr;
    int     width_     = 1280;
    int     height_    = 800;
    bool    initialized_ = false;
};

}  // namespace lsxhome