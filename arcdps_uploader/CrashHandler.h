#pragma once

namespace CrashHandler {

// Install process-wide crash diagnostics that append a timestamped stack trace
// to `crash_log_path` when this addon faults. arcdps does not always produce a
// crash log (a CRT fast-fail / 0xc0000409 kills the process too abruptly), so
// this captures the fault ourselves: CRT invalid-parameter (the usual
// 0xc0000409 source), access violations, pure-virtual calls, unhandled C++
// exceptions (std::terminate), and any unhandled SEH exception. If a matching
// PDB is present next to the module the frames are symbolized (function + line);
// otherwise each frame is logged as module!+0xRVA, which can be symbolized
// later against the build's PDB.
//
// Safe to call once, as early as possible. Never throws.
void install(const char* crash_log_path);

}  // namespace CrashHandler
