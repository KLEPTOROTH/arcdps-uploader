// Live end-to-end test (local only, NOT run in CI): points a sandboxed
// Uploader instance at a directory of real evtc logs, runs the actual
// upload pipeline (dps.report + Wingman if enabled in the sandbox ini),
// and draws the UI headlessly throughout. Verification of the resulting
// db fields (which drive the View/WM buttons) happens outside, against
// ./addons/uploader/uploader.db in the working directory.
//
// Usage: uploader_livetest <logs-dir> [seconds]

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <thread>

#include "Uploader.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_null.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: uploader_livetest <logs-dir> [seconds]\n");
        return 2;
    }
    std::filesystem::path logs_dir(argv[1]);
    if (!std::filesystem::exists(logs_dir)) {
        printf("logs dir does not exist: %s\n", argv[1]);
        return 2;
    }
    int seconds = (argc > 2) ? atoi(argv[2]) : 60;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplNull_Init();

    std::filesystem::create_directories("./addons/uploader");
    {
        Uploader up("./addons/uploader/", logs_dir);
        up.is_open = true;
        up.start_async_refresh_log_list();
        up.start_upload_thread();

        for (int n = 0; n < seconds * 20; n++) {
            ImGui_ImplNull_NewFrame();
            ImGui::NewFrame();
            up.imgui_tick(1);
            ImGui::Render();
            ImGui_ImplNullRender_RenderDrawData(ImGui::GetDrawData());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // Destructor joins the upload thread, letting an in-flight
        // wingman upload/link poll finish.
        printf("LIVETEST: shutting down (upload thread may still finish "
               "an in-flight task)...\n");
    }
    ImGui_ImplNull_Shutdown();
    ImGui::DestroyContext();
    printf("LIVETEST DONE\n");
    return 0;
}
