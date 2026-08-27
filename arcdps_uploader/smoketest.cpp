// Headless smoke test for the arcdps uploader extension.
//
// Reproduces the in-game crash path without the game: creates a real
// imgui context (same version arcdps runs, IMGUI_VERSION_NUM must match),
// initializes the module, opens the uploader window through the real
// Alt+Shift+U wndproc path, and draws it for 120 frames with the null
// backend. Exits 0 on success; any crash/assert fails the CI job.

#include <cstdio>
#include <filesystem>

#include "arcdps_uploader.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_null.h"

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplNull_Init();

    std::filesystem::create_directories("./addons/uploader");

    arcdps_exports* ex = mod_init();
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

    mod_release();
    ImGui_ImplNull_Shutdown();
    ImGui::DestroyContext();
    printf("SMOKETEST PASS: 120 frames drawn with uploader window open (imgui %s)\n",
           IMGUI_VERSION);
    return 0;
}
