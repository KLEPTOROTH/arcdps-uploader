#pragma once

#include <string>
#include <utility>

namespace Wingman {

// POST an EVTC log to gw2wingman (same /uploadEVTC multipart contract
// PlenBot uses: account, filesize, triggerID, file). Wingman does not
// accept WvW logs (triggerID 1) -- skip those at the call site.
// Returns (ok, human-readable status message).
std::pair<bool, std::string> upload_evtc(const std::string& file_path,
                                         long long filesize, int boss_id,
                                         const std::string& account);

// Full permalink for a Wingman log page slug.
std::string log_url(const std::string& slug);

// Ask Wingman for the published page of an uploaded log
// (checkUploadSuccessfulWithLog). Returns the full permalink, or an empty
// string if the log is not visible (yet).
std::string fetch_log_link(const std::string& filename, long long filesize,
                           int boss_id, const std::string& account);

}  // namespace Wingman
