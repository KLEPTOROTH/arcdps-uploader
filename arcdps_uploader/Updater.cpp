// Self-updater, following the same convention arcdps itself uses:
// check at startup, download the new dll into our config folder
// (addons/uploader/ -- kept out of arcdps' extension scan path, the same
// reason arcdps stages arcdps.dll_update inside addons/arcdps/), then
// swap via rename. The running session keeps the old version in memory;
// the new one loads on the next game start. The previous dll is kept one
// generation as d3d9_uploader.dll_prev for manual rollback.

#include "Updater.h"

#include <Windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "loguru.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace Updater {

static std::mutex state_mutex;
static State state;
static std::thread worker;

static const char* RELEASES_API =
    "https://api.github.com/repos/KLEPTOROTH/arcdps-uploader/releases/latest";
static const wchar_t* DLL_NAME_W = L"d3d9_uploader.dll";
static const char* STAGING_PATH = "./addons/uploader/d3d9_uploader.dll_update";
static const char* PREV_PATH = "./addons/uploader/d3d9_uploader.dll_prev";

std::array<int, 3> version_triplet(const std::string& v) {
    std::array<int, 3> t{0, 0, 0};
    size_t start = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
    sscanf(v.c_str() + start, "%d.%d.%d", &t[0], &t[1], &t[2]);
    return t;
}

bool is_newer(const std::string& remote, const std::string& local) {
    return version_triplet(remote) > version_triplet(local);
}

static void set_state(Status s, const std::string& latest,
                      const std::string& msg) {
    std::lock_guard<std::mutex> lk(state_mutex);
    state.status = s;
    state.latest_version = latest;
    state.message = msg;
}

State get_state() {
    std::lock_guard<std::mutex> lk(state_mutex);
    return state;
}

static std::optional<fs::path> own_dll_path() {
    HMODULE hmod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&own_dll_path, &hmod)) {
        return std::nullopt;
    }
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(hmod, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::nullopt;
    return fs::path(buf);
}

void cleanup_old() {
    std::error_code ec;
    fs::remove(PREV_PATH, ec);
    fs::remove(STAGING_PATH, ec);
}

static void check_thread(std::string current, bool auto_apply) {
    set_state(Status::CHECKING, "", "Checking for updates...");

    auto resp = cpr::Get(
        cpr::Url{RELEASES_API},
        cpr::Header{{"User-Agent", "arcdps-uploader-updater"},
                    {"Accept", "application/vnd.github+json"}},
        cpr::Timeout{20000});
    if (resp.status_code != 200) {
        LOG_F(WARNING, "Updater: release check failed (HTTP %ld)",
              resp.status_code);
        set_state(Status::ERR, "", "Update check failed");
        return;
    }

    std::string tag, asset_url;
    try {
        json j = json::parse(resp.text);
        tag = j.value("tag_name", "");
        for (const auto& asset : j.value("assets", json::array())) {
            if (asset.value("name", "") == "d3d9_uploader.dll") {
                asset_url = asset.value("browser_download_url", "");
                break;
            }
        }
    } catch (const json::exception& e) {
        LOG_F(WARNING, "Updater: bad release json: %s", e.what());
        set_state(Status::ERR, "", "Update check failed");
        return;
    }

    if (tag.empty() || asset_url.empty() || !is_newer(tag, current)) {
        set_state(Status::UP_TO_DATE, tag, "");
        return;
    }

    // Only self-stage when actually running as the installed dll -- keeps
    // the standalone/test builds from ever touching their own binaries.
    auto self = own_dll_path();
    if (!auto_apply || !self || self->filename() != DLL_NAME_W) {
        set_state(Status::AVAILABLE, tag,
                  "Update " + tag + " available on GitHub");
        return;
    }

    LOG_F(INFO, "Updater: downloading %s", tag.c_str());
    auto dl = cpr::Get(cpr::Url{asset_url},
                       cpr::Header{{"User-Agent", "arcdps-uploader-updater"}},
                       cpr::Timeout{60000});
    // Sanity: HTTP ok, plausible size for this dll, PE header present.
    if (dl.status_code != 200 || dl.text.size() < 1024 * 1024 ||
        dl.text[0] != 'M' || dl.text[1] != 'Z') {
        LOG_F(WARNING, "Updater: download failed (HTTP %ld, %zu bytes)",
              dl.status_code, dl.text.size());
        set_state(Status::ERR, tag, "Update download failed");
        return;
    }

    {
        std::ofstream out(STAGING_PATH, std::ios::binary | std::ios::trunc);
        out.write(dl.text.data(), dl.text.size());
    }
    std::error_code ec;
    if (fs::file_size(STAGING_PATH, ec) != dl.text.size()) {
        set_state(Status::ERR, tag, "Update download failed");
        return;
    }

    // Swap: the loaded dll can be renamed but not overwritten.
    fs::remove(PREV_PATH, ec);
    ec.clear();
    fs::rename(*self, PREV_PATH, ec);
    if (ec) {
        LOG_F(WARNING, "Updater: could not move current dll: %s",
              ec.message().c_str());
        set_state(Status::ERR, tag, "Update failed to install");
        return;
    }
    fs::rename(STAGING_PATH, *self, ec);
    if (ec) {
        std::error_code ec2;
        fs::rename(PREV_PATH, *self, ec2);  // roll back
        LOG_F(WARNING, "Updater: could not place new dll: %s",
              ec.message().c_str());
        set_state(Status::ERR, tag, "Update failed to install");
        return;
    }

    LOG_F(INFO, "Updater: %s staged, active next game start", tag.c_str());
    set_state(Status::STAGED, tag,
              "Updated to " + tag + " - loads on next game start");
}

void begin_check(const std::string& current_version, bool auto_apply) {
    if (worker.joinable()) return;
    worker = std::thread(check_thread, current_version, auto_apply);
}

void shutdown() {
    if (worker.joinable()) worker.join();
}

}  // namespace Updater
