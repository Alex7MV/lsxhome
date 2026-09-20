// lsxhome (LogestiX Home) — Windows-native x64 desktop shell.
//
// Lifecycle: WinMain -> native window -> D3D12 hardware renderer + Dear ImGui
// (docking branch); a compute thread publishes decoded tokens into the
// lock-free SPSC GuiBridge; the UI thread drains them every frame and renders
// the Cowork-style Blackwell theme. Strict separation: the UI thread never
// touches the model, and the compute thread never touches the window.

#include "backends/imgui_impl_win32.h"
#include <imgui.h>

#include "lsxhome/d3d12_renderer.h"
#include "lsxhome/font_loader.h"
#include "lsxhome/gui_bridge.h"
#include "lsxhome/gui_renderer.h"

#include "lsxcommon/generation_cli.h"
#include "lsxcommon/model.h"
#include "lsxcommon/model_engine.h"
#include "lsxcommon/model_factory.h"
#include "lsxcommon/prompt_framing.h"

#include <absl/flags/flag.h>
#include <absl/flags/parse.h>

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Dear ImGui's Win32 backend keeps WndProcHandler intentionally commented out;
// the application is responsible for this exact forward declaration.
IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Generation path execution option: --run/-r routes the resolved prompt into
// the shell's token stream. A bare --run/-r (no value) falls back to the
// default blueprint prompt via lsxcommon::cli.
ABSL_FLAG(std::string, run, "", "generation prompt (--run \"PROMPT\" / -r \"PROMPT\")");
ABSL_FLAG(std::string, model, "", "model path for the --run generation path");

namespace {

constexpr const wchar_t* kWindowClass = L"LogestiXHomeWindow";
constexpr const wchar_t* kWindowTitle = L"lsxhome — LogestiX Home";

constexpr int kDefaultWidth = 1440;
constexpr int kDefaultHeight = 900;

// GLM-5.2 self-check token: 11751 decodes to "Paris".
constexpr std::uint32_t kParisTokenId = 11751;

lsxhome::D3D12Renderer* g_renderer = nullptr;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (g_renderer && g_renderer->Initialized()) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
            return 1;
        }
    }
    if (msg == WM_SIZE && g_renderer && wparam != SIZE_MINIMIZED) {
        g_renderer->Resize(LOWORD(lparam), HIWORD(lparam));
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void ComputeProducer(lsxhome::GuiBridge& bridge, std::atomic<bool>& running,
                     const std::string& model_path,
                     const std::string& prompt) {
    bool published = false;
    // Runs the resolved generation prompt through the model once and publishes
    // each decoded token into the SPSC bridge. Falls back to the previous demo
    // behavior (single "Paris" token pump) when no model is loadable.
    if (!model_path.empty()) {
        // Fixed-speed policy: the engine session applies the per-GPU knob
        // policy (same as lsxfabric/lsxbenchmark) on entry and tears the
        // engine down on scope exit.
        lsxcommon::InferenceEngine::Session engine_session;
        lsxcommon::ModelInitConfig cfg;
        cfg.model_path = model_path;
        auto model = lsxcommon::LsxModelFactory::Create(cfg);
        if (model && model->Load()) {
            std::vector<int32_t> input_ids;
            if (model->BuildPromptInputIds(
                    model->Tokenizer(), prompt, input_ids)) {
                lsxcommon::ModelRequest req{std::move(input_ids), 64};
                auto result = model->Infer(req);
                if (result.ok && !result.output_text.empty()) {
                    int idx = 0;
                    std::string_view rest = result.output_text;
                    while (!rest.empty()) {
                        auto sp = rest.find(' ');
                        std::string_view tok =
                            (sp == rest.npos) ? rest : rest.substr(0, sp);
                        bridge.Publish(lsxhome::MakeTokenPayload(
                            0, static_cast<std::uint32_t>(idx++),
                            std::string(tok).c_str()));
                        rest.remove_prefix(sp == rest.npos ? rest.size()
                                                           : sp + 1);
                    }
                    published = true;
                }
            }
            model->Unload();
        }
    }
    if (!published) {
        bridge.Publish(lsxhome::MakeTokenPayload(
            kParisTokenId, 0,
            prompt.empty() ? "Paris" : prompt.c_str()));
    }

    // Keep the compute thread alive until the shell tears down; the UI thread
    // drains the already-published token stream every frame.
    while (running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

}  // namespace

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    // Canonicalize the generation-path flag (bare --run/-r -> default prompt)
    // before absl::ParseCommandLine consumes argv.
    auto normalized = lsxcommon::cli::NormalizeGenerationArgv(__argc, __argv);
    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(normalized.size());
    for (auto& arg : normalized) argv_ptrs.push_back(arg.data());
    absl::ParseCommandLine(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());

    std::string run_prompt =
        lsxcommon::cli::ResolveGenerationPrompt(absl::GetFlag(FLAGS_run));
    std::string model_path = absl::GetFlag(FLAGS_model);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) {
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, kDefaultWidth, kDefaultHeight,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        return 2;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    lsxhome::D3D12Renderer renderer;
    if (renderer.Init(hwnd, kDefaultWidth, kDefaultHeight) != 0) {
        return 3;
    }
    g_renderer = &renderer;

    // Docking + multithreaded renderer are both required pieces of the shell.
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    // imgui_freetype subpixel rasterizer is configured by the loader; the base
    // font comes from the embedded static TTF payload.
    lsxhome::FontLoader::LoadDefault(*io.Fonts);

    // Hero heading face for the welcome screen's 34px greeting.
    lsxhome::SetHeadingFont(lsxhome::FontLoader::LoadHeading(*io.Fonts));

    lsxhome::GuiBridge bridge;
    std::atomic<bool> running{true};
    std::thread compute_thread(
        ComputeProducer, std::ref(bridge), std::ref(running),
        model_path, run_prompt);

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (msg.message == WM_QUIT) {
            break;
        }

        if (!renderer.BeginFrame()) {
            continue;
        }
        lsxhome::ApplyBlackwellCoworkTheme();

        // Edge-to-edge docking root + seamless sidebar/panel layout.
        lsxhome::BuildWorkspaceSkeleton(bridge);

        renderer.EndFrame(true);
    }

    running.store(false, std::memory_order_relaxed);
    if (compute_thread.joinable()) {
        compute_thread.join();
    }

    renderer.Shutdown();
    g_renderer = nullptr;
    UnregisterClassW(kWindowClass, hInst);
    return static_cast<int>(msg.wParam);
}