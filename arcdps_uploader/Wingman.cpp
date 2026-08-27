#include "Wingman.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "loguru.hpp"

using json = nlohmann::json;

namespace Wingman {

static const char* UPLOAD_URL =
    "https://gw2wingman.nevermindcreations.de/uploadEVTC";

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
