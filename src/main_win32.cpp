// lsxhome (LogestiX Home) — Windows-native x64 desktop shell.
//
// Lifecycle: WinMain -> native window -> D3D12 hardware renderer + Dear ImGui
// (docking branch); a ChatSession worker thread runs the logestix engine and
// publishes decoded text into the lock-free SPSC GuiBridge; the UI thread
// drains it every frame into the chat transcript. Strict separation: the UI
// thread never touches the model, and the worker never touches the window.

#include "backends/imgui_impl_win32.h"
#include <imgui.h>

#include "lsxhome/chat_session.h"
#include "lsxhome/chat_state.h"
#include "lsxhome/d3d12_renderer.h"
#include "lsxhome/font_loader.h"
#include "lsxhome/gui_bridge.h"
#include "lsxhome/gui_renderer.h"
#include "lsxhome/lsx_generation_backend.h"

#include "lsxcommon/generation_cli.h"

#include <absl/flags/flag.h>
#include <absl/flags/parse.h>

#include <windows.h>

#include <cstdint>
#include <string>
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
    auto backend = lsxhome::MakeLsxGenerationBackend(model_path);
    lsxhome::ChatSession session(bridge, *backend);
    lsxhome::ChatState chat;

    // --run keeps its CLI contract: the resolved prompt is submitted as the first
    // chat turn. With no --model there is nothing to run, so the deterministic
    // GLM-5.2 self-check token (11751 -> "Paris") becomes the answer instead of
    // starting a load that cannot succeed. The self-check goes straight into the
    // transcript: publishing it through the bridge would make the UI thread a
    // second producer on a single-producer ring.
    if (!absl::GetFlag(FLAGS_run).empty()) {
        if (model_path.empty()) {
            const lsxhome::TokenPayload self_check =
                lsxhome::MakeFallbackTokenPayload(run_prompt, false);
            if (chat.BeginTurn(run_prompt)) {
                chat.AppendDelta(self_check.stream_seq, self_check.text);
                chat.EndTurn(false);
            }
        } else if (session.Submit(run_prompt)) {
            chat.BeginTurn(run_prompt);
        }
    }

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
            // No renderable back buffer (e.g. a resize that D3D12 refused while
            // the window was minimized). Yield instead of spinning at 100% CPU;
            // the next WM_SIZE restores the swap chain.
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            continue;
        }
        lsxhome::ApplyBlackwellCoworkTheme();

        // Edge-to-edge docking root + chat panel; the panel drains the bridge into
        // the transcript every frame.
        lsxhome::BuildWorkspaceSkeleton(session, chat);

        renderer.EndFrame(true);
    }

    // The ChatSession destructor aborts and joins its worker, which unloads the
    // model with the engine session it owns.
    renderer.Shutdown();
    g_renderer = nullptr;
    UnregisterClassW(kWindowClass, hInst);
    return static_cast<int>(msg.wParam);
}
