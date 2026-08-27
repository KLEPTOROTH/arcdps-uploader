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

}  // namespace Wingman
