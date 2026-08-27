#include "Wingman.h"

#include <chrono>
#include <thread>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "loguru.hpp"

using json = nlohmann::json;

namespace Wingman {

static const char* WINGMAN_BASE = "https://gw2wingman.nevermindcreations.de";
static const char* UPLOAD_URL =
    "https://gw2wingman.nevermindcreations.de/uploadEVTC";

std::string log_url(const std::string& slug) {
    return std::string(WINGMAN_BASE) + "/log/" + slug;
}

std::string fetch_log_link(const std::string& filename, long long filesize,
                           int boss_id, const std::string& account) {
    // `filename` must be the name Wingman knows the upload by: the actual
    // file name INCLUDING extension (the multipart upload sends the
    // basename of the evtc). Wingman indexes asynchronously and can take
    // a while, so poll patiently before giving up.
    for (int attempt = 0; attempt < 6; attempt++) {
        std::this_thread::sleep_for(std::chrono::seconds(attempt == 0 ? 5 : 10));
        auto resp = cpr::Post(
            cpr::Url{std::string(WINGMAN_BASE) + "/checkUploadSuccessfulWithLog"},
            cpr::Payload{{"file", filename},
                         {"filesize", std::to_string(filesize)},
                         {"bossID", std::to_string(boss_id)},
                         {"account", account}},
            cpr::Header{{"User-Agent", "arcdps-uploader"}},
            cpr::Timeout{20000});
        if (resp.status_code != 200) continue;
        try {
            json j = json::parse(resp.text);
            std::string slug = j.value("html", "");
            if (!slug.empty()) {
                return log_url(slug);
            }
        } catch (const json::exception&) {
            // "False" or other non-json body: not indexed yet
        }
    }
    LOG_F(INFO, "Wingman: no log page yet for %s", filename.c_str());
    return "";
}

std::pair<bool, std::string> upload_evtc(const std::string& file_path,
                                         long long filesize, int boss_id,
                                         const std::string& account) {
    cpr::Multipart multi{{"account", account},
                         {"filesize", std::to_string(filesize)},
                         {"triggerID", std::to_string(boss_id)},
                         {"file", cpr::File{file_path}}};
    auto resp = cpr::Post(cpr::Url{UPLOAD_URL}, multi,
                          cpr::Header{{"User-Agent", "arcdps-uploader"}},
                          cpr::Timeout{120000});
    if (resp.status_code != 200) {
        LOG_F(WARNING, "Wingman: upload failed (HTTP %ld): %s",
              resp.status_code, resp.text.c_str());
        return {false,
                "upload failed (HTTP " + std::to_string(resp.status_code) + ")"};
    }
    // Response shape: {"result": true} on success, {"error": "..."} on failure.
    try {
        json j = json::parse(resp.text);
        if (j.value("result", false)) {
            return {true, "uploaded"};
        }
        std::string err = j.value("error", "unknown error");
        LOG_F(WARNING, "Wingman: rejected: %s", err.c_str());
        return {false, "rejected: " + err};
    } catch (const json::exception&) {
        LOG_F(WARNING, "Wingman: unexpected response: %s", resp.text.c_str());
        return {false, "unexpected response"};
    }
}

}  // namespace Wingman
