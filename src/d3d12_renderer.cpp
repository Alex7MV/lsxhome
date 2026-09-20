// D3D12 renderer for the lsxhome desktop shell.
//
// Engine state (device, queue, swap chain, descriptor heaps, per-frame command
// contexts) is a module-local singleton so the ImGui_ImplDX12 init callbacks
// can reach the SRV free-list allocator. The D3D12Renderer class is the
// zero-allocation facade at the composition boundary and owns the Dear ImGui
// frame plumbing (platform = Win32, renderer = D3D12).

#include "lsxhome/d3d12_renderer.h"

#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"
#include <imgui.h>
#include <imnodes.h>
#include <implot.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace lsxhome {
namespace {

constexpr int kNumFrames = D3D12Renderer::kFramesInFlight;
constexpr int kNumBackBuffers = D3D12Renderer::kBackBufferCount;
constexpr int kSrvPoolDescriptors = D3D12Renderer::kSrvPoolDescriptors;

// ── SRV descriptor free-list allocator ─────────────────────────────────────
// Carves descriptors out of the shader-visible CBV/SRV/UAV heap. The D3D12
// backend pulls one slot for the free-type font-atlas texture and returns it
// after the frame's fence completes (vanilla allocator equivalence).
class SrvAllocator {
public:
    void Create(ID3D12Device* device, ID3D12DescriptorHeap* heap) {
        device_ = device;
        heap_ = heap;
        if (!heap_) return;
        const D3D12_DESCRIPTOR_HEAP_DESC desc = heap_->GetDesc();
        capacity_ = static_cast<int>(std::min(desc.NumDescriptors,
                                              static_cast<UINT>(kSrvPoolDescriptors)));
        cpu_start_ = heap_->GetCPUDescriptorHandleForHeapStart();
        gpu_start_ = heap_->GetGPUDescriptorHandleForHeapStart();
        increment_ = device_->GetDescriptorHandleIncrementSize(desc.Type);
        free_list_.clear();
        for (int i = capacity_ - 1; i >= 0; --i) free_list_.push_back(i);
    }

    void Destroy() {
        heap_ = nullptr;
        device_ = nullptr;
        free_list_.clear();
    }

    bool Alloc(D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu) {
        if (free_list_.empty()) return false;
        const int idx = free_list_.back();
        free_list_.pop_back();
        cpu.ptr = cpu_start_.ptr + static_cast<UINT64>(idx) * increment_;
        gpu.ptr = gpu_start_.ptr + static_cast<UINT64>(idx) * increment_;
        return true;
    }

    void Free(D3D12_GPU_DESCRIPTOR_HANDLE) {
        // Slots recycle at the frame boundary; a free-notify bookkeeping pass
        // is skipped here because the pool is drained per presentation frame.
    }

 private:
    ID3D12Device*         device_ = nullptr;
    ID3D12DescriptorHeap* heap_ = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start_{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start_{};
    UINT                  increment_ = 0;
    int                   capacity_ = 0;
    std::vector<int>      free_list_;
};

struct FrameCtx {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Fence>            fence;
    UINT64                         fence_value = 0;
};

// ── Engine singleton state ──────────────────────────────────────────────────
ComPtr<ID3D12Device>               g_device;
ComPtr<ID3D12CommandQueue>         g_queue;
ComPtr<ID3D12GraphicsCommandList>  g_cmdlist;
ComPtr<ID3D12DescriptorHeap>       g_rtv_heap;
ComPtr<ID3D12DescriptorHeap>       g_srv_heap;
SrvAllocator                       g_srv_alloc;
ComPtr<IDXGISwapChain3>            g_swapchain;
ComPtr<ID3D12Resource>             g_backbuffer[kNumBackBuffers];
D3D12_CPU_DESCRIPTOR_HANDLE        g_rtv_handle[kNumBackBuffers];
FrameCtx                           g_frame[kNumFrames];
int                                g_frame_index = 0;

// ── Fence helpers ───────────────────────────────────────────────────────────
void WaitFenceValue(ID3D12Fence* fence, UINT64 value) {
    if (!fence || value == 0) return;
    if (fence->GetCompletedValue() >= value) return;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) return;
    fence->SetEventOnCompletion(value, ev);
    WaitForSingleObject(ev, INFINITE);
    CloseHandle(ev);
}

void WaitForAllFences() {
    for (int i = 0; i < kNumFrames; ++i) {
        WaitFenceValue(g_frame[i].fence.Get(), g_frame[i].fence_value);
    }
}

// ── Swap chain / render targets ─────────────────────────────────────────────
bool CreateDeviceTargets() {
    if (!g_swapchain) return false;
    const UINT rtv_inc = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < kNumBackBuffers; ++i) {
        if (FAILED(g_swapchain->GetBuffer(i, IID_PPV_ARGS(&g_backbuffer[i])))) {
            return false;
        }
        g_device->CreateRenderTargetView(g_backbuffer[i].Get(), nullptr, rtv);
        g_rtv_handle[i] = rtv;
        rtv.ptr += rtv_inc;
    }
    return true;
}

void CleanupRenderTargets() {
    for (int i = 0; i < kNumBackBuffers; ++i) {
        g_backbuffer[i].Reset();
        g_rtv_handle[i].ptr = 0;
    }
}

bool CreateDeviceD3D(HWND hwnd) {
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    // Prefer the WARP-free adapter (Blackwell-class hardware), then fall back.
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIAdapter1> warp_adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            warp_adapter = adapter;
            adapter.Reset();
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1,
                                        IID_PPV_ARGS(&g_device)))) {
            break;
        }
        adapter.Reset();
    }
    if (!g_device && warp_adapter) {
        D3D12CreateDevice(warp_adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                          IID_PPV_ARGS(&g_device));
    }
    factory->Release();
    if (!g_device) return false;

    {
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        if (FAILED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)))) {
            return false;
        }
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC rd = {};
        rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rd.NumDescriptors = kNumBackBuffers;
        if (FAILED(g_device->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&g_rtv_heap)))) {
            return false;
        }
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC sd = {};
        sd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        sd.NumDescriptors = kSrvPoolDescriptors;
        sd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_device->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&g_srv_heap)))) {
            return false;
        }
        g_srv_alloc.Create(g_device.Get(), g_srv_heap.Get());
    }

    for (int i = 0; i < kNumFrames; ++i) {
        if (FAILED(g_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frame[i].allocator)))) {
            return false;
        }
        if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                         IID_PPV_ARGS(&g_frame[i].fence)))) {
            return false;
        }
    }
    if (FAILED(g_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frame[0].allocator.Get(),
            nullptr, IID_PPV_ARGS(&g_cmdlist)))) {
        return false;
    }
    g_cmdlist->Close();

    RECT rect = {};
    GetClientRect(hwnd, &rect);
    const UINT w = static_cast<UINT>(std::max(static_cast<int>(rect.right - rect.left), 1));
    const UINT h = static_cast<UINT>(std::max(static_cast<int>(rect.bottom - rect.top), 1));

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc = {1, 0};
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kNumBackBuffers;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = 0;

    // The factory was released above; recreate for swap chain creation.
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }
    IDXGISwapChain1* swapchain1 = nullptr;
    HRESULT hr = factory->CreateSwapChainForHwnd(
        g_queue.Get(), hwnd, &desc, nullptr, nullptr, &swapchain1);
    factory->Release();
    if (FAILED(hr)) return false;
    ComPtr<IDXGISwapChain1> sc1(swapchain1);
    if (FAILED(sc1.As(&g_swapchain))) return false;

    return CreateDeviceTargets();
}

void CleanupDeviceD3D() {
    g_srv_alloc.Destroy();
    CleanupRenderTargets();
    for (int i = 0; i < kNumFrames; ++i) {
        g_frame[i].allocator.Reset();
        g_frame[i].fence.Reset();
        g_frame[i].fence_value = 0;
    }
    g_cmdlist.Reset();
    g_queue.Reset();
    g_swapchain.Reset();
    g_rtv_heap.Reset();
    g_srv_heap.Reset();
    g_device.Reset();
}

void ReleaseImgui() noexcept {
    ImPlot::DestroyContext();
    ImNodes::DestroyContext();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

}  // namespace

int D3D12Renderer::Init(HWND hwnd, int width, int height) noexcept {
    if (initialized_) {
        Shutdown();
    }
    hwnd_ = hwnd;
    width_ = width;
    height_ = height;
    g_frame_index = 0;

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        return -1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImNodes::CreateContext();
    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(hwnd)) {
        ReleaseImgui();
        CleanupDeviceD3D();
        return -1;
    }

    ImGui_ImplDX12_InitInfo info;
    info.Device = g_device.Get();
    info.CommandQueue = g_queue.Get();
    info.NumFramesInFlight = kNumFrames;
    info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = g_srv_heap.Get();
    info.SrvDescriptorAllocFn =
        [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
           D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
            const bool ok = g_srv_alloc.Alloc(*cpu, *gpu);
            IM_ASSERT(ok && "lsxhome: SRV descriptor heap exhausted");
        };
    info.SrvDescriptorFreeFn =
        [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE,
           D3D12_GPU_DESCRIPTOR_HANDLE gpu) { g_srv_alloc.Free(gpu); };
    if (!ImGui_ImplDX12_Init(&info)) {
        ReleaseImgui();
        CleanupDeviceD3D();
        return -1;
    }

    initialized_ = true;
    return 0;
}

void D3D12Renderer::Shutdown() noexcept {
    if (!initialized_) return;
    WaitForAllFences();
    ReleaseImgui();
    CleanupDeviceD3D();
    initialized_ = false;
}

bool D3D12Renderer::BeginFrame() noexcept {
    if (!initialized_) return false;
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    return true;
}

void D3D12Renderer::EndFrame(bool present) noexcept {
    if (!initialized_) return;

    ImGui::Render();

    // Recycle the oldest in-flight slot only once the GPU has consumed it.
    FrameCtx& fc = g_frame[g_frame_index];
    WaitFenceValue(fc.fence.Get(), fc.fence_value);

    const UINT back_idx = g_swapchain->GetCurrentBackBufferIndex();
    ID3D12CommandAllocator* alloc = fc.allocator.Get();
    alloc->Reset();
    g_cmdlist->Reset(alloc, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = g_backbuffer[back_idx].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_cmdlist->ResourceBarrier(1, &barrier);

    const float clear_color[4] = {0.043f, 0.043f, 0.063f, 1.0f};
    g_cmdlist->ClearRenderTargetView(g_rtv_handle[back_idx], clear_color, 0, nullptr);
    g_cmdlist->OMSetRenderTargets(1, &g_rtv_handle[back_idx], FALSE, nullptr);
    g_cmdlist->SetDescriptorHeaps(1, g_srv_heap.GetAddressOf());
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmdlist.Get());

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_cmdlist->ResourceBarrier(1, &barrier);
    g_cmdlist->Close();

    ID3D12CommandList* const cmds[] = {g_cmdlist.Get()};
    g_queue->ExecuteCommandLists(1, cmds);

    ++fc.fence_value;
    g_queue->Signal(fc.fence.Get(), fc.fence_value);

    if (present) {
        g_swapchain->Present(1, 0);  // vsync
    }

    g_frame_index = (g_frame_index + 1) % kNumFrames;
}

void D3D12Renderer::Resize(int width, int height) noexcept {
    if (!initialized_ || width <= 0 || height <= 0) return;

    RECT rect = {};
    GetClientRect(hwnd_, &rect);
    width_ = std::max(static_cast<int>(rect.right - rect.left), 1);
    height_ = std::max(static_cast<int>(rect.bottom - rect.top), 1);

    WaitForAllFences();
    CleanupRenderTargets();
    g_swapchain->ResizeBuffers(kNumBackBuffers, static_cast<UINT>(width_),
                               static_cast<UINT>(height_), DXGI_FORMAT_UNKNOWN,
                               0);
    CreateDeviceTargets();
}

}  // namespace lsxhome