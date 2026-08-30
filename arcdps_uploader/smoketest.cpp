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

#include "Log.h"
#include "Revtc.h"
#include "Updater.h"
#include "Wingman.h"
#include "arcdps_uploader.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_null.h"

// Defined in Uploader.cpp. Exception-safe dps.report response parser.
bool parse_dpsreport_response(const std::string& body, Log& log,
                             std::string& token_out);

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

    // Regression guard for the 0xc0000409 crash: status text (which can hold
    // a raw server response body or a percent-encoded URL) must be drawn with
    // ImGui::TextUnformatted, never ImGui::Text(msg.c_str()). A '%' in the
    // message would otherwise be parsed as a printf spec -- %n/lone-% trips
    // the CRT invalid-parameter handler and fastfails the game the moment the
    // status window draws (i.e. at character select).
    uploader_test_push_status(
        "Unknown response.\n%n %s %s %s %x%x%x 100%% "
        "http://x/%20/%ZZ trailing %");

    for (int n = 0; n < 120; n++) {
        ImGui_ImplNull_NewFrame();
        ImGui::NewFrame();
        // Alternate in-game and character-select frames: the window must
        // draw on both, and charsel frames exercise the update notice path.
        mod_imgui(n % 4 != 3);
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

    // Webhook category routing: encounters missing from this table are
    // silently never posted to Discord, so pin the mapping for everything
    // added in 1.1.2 alongside one sentinel per legacy category.
    struct {
        int id;
        Revtc::BossCategory cat;
        const char* name;
    } cat_cases[] = {
        {26725, Revtc::BossCategory::RAIDS, "Greer"},
        {26774, Revtc::BossCategory::RAIDS, "Decima"},
        {26712, Revtc::BossCategory::RAIDS, "Ura"},
        {27124, Revtc::BossCategory::RAIDS, "Kela"},
        {15415, Revtc::BossCategory::RAIDS, "Spirit Race"},
        {25577, Revtc::BossCategory::FRACTALS, "Kanaxai"},
        {26231, Revtc::BossCategory::FRACTALS, "Eparch"},
        {27010, Revtc::BossCategory::FRACTALS, "Whispering Shadow"},
        {25247, Revtc::BossCategory::STRIKES, "Captain Mai Trin (3)"},
        {15438, Revtc::BossCategory::RAIDS, "Vale Guardian"},
        {17021, Revtc::BossCategory::FRACTALS, "MAMA"},
        {25989, Revtc::BossCategory::STRIKES, "Cerus"},
        {16199, Revtc::BossCategory::GOLEMS, "Standard Golem"},
        {1, Revtc::BossCategory::WVW, "WvW"},
    };
    for (const auto& c : cat_cases) {
        Revtc::BossCategory got =
            Revtc::Parser::encounterCategory((Revtc::BossID)c.id);
        if (got != c.cat) {
            printf("FAIL: encounterCategory(%d) [%s] = %d, expected %d\n",
                   c.id, c.name, (int)got, (int)c.cat);
            return 1;
        }
    }
    printf("encounter category routing OK: %zu bosses mapped\n",
           sizeof(cat_cases) / sizeof(cat_cases[0]));

    // Updater version comparison (pure function, no network)
    struct {
        const char* remote;
        const char* local;
        bool newer;
    } ver_cases[] = {
        {"v1.2.1", "1.2.0", true},   {"1.2.0", "1.2.0", false},
        {"v1.1.9", "1.2.0", false},  {"2.0.0", "1.9.9", true},
        {"v1.10.0", "1.9.0", true},  {"1.2", "1.2.0", false},
    };
    for (const auto& c : ver_cases) {
        if (Updater::is_newer(c.remote, c.local) != c.newer) {
            printf("FAIL: is_newer(%s, %s) != %d\n", c.remote, c.local,
                   (int)c.newer);
            return 1;
        }
    }
    printf("updater version comparison OK: %zu cases\n",
           sizeof(ver_cases) / sizeof(ver_cases[0]));

    if (Wingman::log_url("abc123_boss") !=
        "https://gw2wingman.nevermindcreations.de/log/abc123_boss") {
        printf("FAIL: Wingman::log_url\n");
        return 1;
    }
    printf("wingman log url OK\n");

    // Regression: a malformed / unexpected dps.report 200 body must never throw
    // out of the parser (previously an unhandled json exception unwound off the
    // upload thread -> std::terminate -> 0xc0000409, the crash seen at
    // character select on real machines). Every hostile body must return false
    // without crashing, and a well-formed body must parse correctly.
    {
        const char* hostile[] = {
            "",
            "not json at all",
            "<html><body>503 Service Unavailable</body></html>",
            "{",
            "{}",
            "[]",
            "null",
            "{\"error\":\"rate limited\"}",
            "{\"permalink\":123}",                    // wrong type
            "{\"permalink\":\"https://x\",\"encounter\":42}",  // encounter not obj
            "{\"permalink\":\"https://x\",\"encounter\":{\"bossId\":\"nope\"}}",
            "{\"id\":null,\"permalink\":null}",
        };
        for (const char* b : hostile) {
            Log l{};
            std::string tok;
            parse_dpsreport_response(b, l, tok);  // must not throw/crash
        }
        Log good{};
        std::string tok;
        bool ok = parse_dpsreport_response(
            "{\"id\":\"AAAA\",\"permalink\":\"https://dps.report/AAAA\","
            "\"encounter\":{\"bossId\":15438,\"boss\":\"Vale Guardian\","
            "\"success\":true,\"jsonAvailable\":true},"
            "\"userToken\":\"tok123\"}",
            good, tok);
        if (!ok || good.permalink != "https://dps.report/AAAA" ||
            good.boss_id != 15438 || good.boss_name != "Vale Guardian" ||
            !good.success || tok != "tok123") {
            printf("FAIL: parse_dpsreport_response good-path\n");
            return 1;
        }
        // An empty-permalink 200 must be reported as unusable.
        Log nolink{};
        std::string tok2;
        if (parse_dpsreport_response("{\"id\":\"x\"}", nolink, tok2)) {
            printf("FAIL: parse_dpsreport_response should reject no-permalink\n");
            return 1;
        }
        printf("dps.report response parsing OK: %zu hostile bodies survived\n",
               sizeof(hostile) / sizeof(hostile[0]));
    }

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
