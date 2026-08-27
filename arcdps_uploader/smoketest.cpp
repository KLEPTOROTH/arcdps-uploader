// Headless smoke test for the arcdps uploader extension.
//
// Reproduces the in-game integration without the game: creates a real
// imgui context (same version arcdps runs, IMGUI_VERSION_NUM must match),
// initializes the module through the REAL get_init_addr entrypoint with
// caller-provided allocator functions (as arcdps does), opens the uploader
// window through the real Alt+Shift+U wndproc path, and draws it for 120
// frames with the null backend.
//
// Verifies the extension routes imgui allocations through the provided
// allocator functions -- skipping ImGui::SetAllocatorFunctions corrupts the
// game heap the moment the window opens (0xc0000374), while working fine in
// any single-heap test, so this is asserted explicitly.
//
// Exits 0 on success; any crash/assert/check fails the CI job.

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "arcdps_uploader.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_null.h"

static size_t g_alloc_calls = 0;
static size_t g_free_calls = 0;
static void* CountingAlloc(size_t sz, void*) {
    g_alloc_calls++;
    return malloc(sz);
}
static void CountingFree(void* ptr, void*) {
    g_free_calls++;
    free(ptr);
}

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();  // default allocators, like arcdps's own heap
    ImGui_ImplNull_Init();

    std::filesystem::create_directories("./addons/uploader");

    // Drive the real entrypoint the way arcdps does.
    typedef arcdps_exports* (*ModInitFn)();
    ModInitFn init_fn = (ModInitFn)get_init_addr(
        (char*)"20260816.114918-607-x64", ImGui::GetCurrentContext(), nullptr,
        (HANDLE)GetModuleHandle(nullptr), (void*)CountingAlloc,
        (void*)CountingFree, IMGUI_VERSION_NUM);
    arcdps_exports* ex = init_fn();
    if (!ex) {
        printf("FAIL: mod_init returned null\n");
        return 1;
    }
    printf("exports: name=%s build=%s imguivers=%u (compiled %d)\n",
           ex->out_name, ex->out_build, ex->imguivers, IMGUI_VERSION_NUM);
    if (ex->imguivers != (uint32_t)IMGUI_VERSION_NUM) {
        printf("FAIL: imguivers mismatch\n");
        return 1;
    }

    // Toggle the window open through the real hotkey path (Alt+Shift+U)
    mod_wnd(nullptr, WM_KEYDOWN, VK_MENU, 0);
    mod_wnd(nullptr, WM_KEYDOWN, VK_SHIFT, 0);
    mod_wnd(nullptr, WM_KEYDOWN, 'U', 0);
    mod_wnd(nullptr, WM_KEYUP, 'U', 0);
    mod_wnd(nullptr, WM_KEYUP, VK_SHIFT, 0);
    mod_wnd(nullptr, WM_KEYUP, VK_MENU, 0);

    for (int n = 0; n < 120; n++) {
        ImGui_ImplNull_NewFrame();
        ImGui::NewFrame();
        mod_imgui(1);
        mod_options_windows(nullptr);
        ImGui::Render();
        ImGui_ImplNullRender_RenderDrawData(ImGui::GetDrawData());
    }

    if (g_alloc_calls == 0) {
        printf("FAIL: extension did not route imgui allocations through the "
               "allocator functions passed to get_init_addr "
               "(missing ImGui::SetAllocatorFunctions -> heap corruption "
               "in-game)\n");
        return 1;
    }
    printf("allocator routing OK: %zu allocs / %zu frees through provided "
           "functions\n",
           g_alloc_calls, g_free_calls);

    typedef uintptr_t (*ModReleaseFn)();
    ModReleaseFn release_fn = (ModReleaseFn)get_release_addr();
    release_fn();
    ImGui_ImplNull_Shutdown();
    ImGui::DestroyContext();
    printf("SMOKETEST PASS: 120 frames drawn with uploader window open "
           "(imgui %s)\n",
           IMGUI_VERSION);
    return 0;
}
