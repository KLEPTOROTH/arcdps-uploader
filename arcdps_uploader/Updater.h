#pragma once

#include <array>
#include <string>

namespace Updater {

enum class Status {
    IDLE,
    CHECKING,
    UP_TO_DATE,
    AVAILABLE,   // newer release exists (auto-update off, or apply failed)
    STAGED,      // new dll downloaded and staged; loads on next game start
    ERR,         // check/download failed; never fatal
};

struct State {
    Status status = Status::IDLE;
    std::string latest_version;
    std::string message;
};

// Parse "1.2.0" or "v1.2.0" into {major, minor, patch}; missing parts are 0.
std::array<int, 3> version_triplet(const std::string& v);
bool is_newer(const std::string& remote, const std::string& local);

// Delete the leftover renamed dll from a previous staged update, if any.
void cleanup_old();

// Query the latest GitHub release on a background thread. If it is newer
// and auto_apply is set, download and stage it. Thread-safe, non-blocking.
void begin_check(const std::string& current_version, bool auto_apply);

// Snapshot of the updater state for UI display.
State get_state();

// Join the background thread. Call from mod_release.
void shutdown();

}  // namespace Updater
