# Arcdps-Uploader
This is an extension for Arcdps that automatically uploads EVTC combat logs *in-game*.

> **Note:** This is a fork of [nbarrios/arcdps-uploader](https://github.com/nbarrios/arcdps-uploader) by the original author. Upstream builds (v1.0.4 and earlier) were compiled against imgui 1.80 and no longer load on current arcdps — they crash the game when the window opens. This fork ports the extension to imgui 1.92.7 (the version current arcdps builds against) so it loads natively again, and wires arcdps' allocator into imgui as the extension API requires.

![Image of the Uploader](https://i.imgur.com/BfcNAR2.png)
* Easy access to recent logs, one-click to view on dps.reports
* No network usage while you're in combat
* Discord Webhook integration

## Usage
Grab the latest [release](https://github.com/KLEPTOROTH/arcdps-uploader/releases). Copy `d3d9_uploader.dll` to the same directory as **Arcdps** (the folder containing arcdps' dll — for a default arcdps install that is `C:\Program Files\Guild Wars 2` or `bin64`).

*If the extension fails to load, install the latest x64 ![Visual C++ redistributable](https://support.microsoft.com/en-us/help/2977003/the-latest-supported-visual-c-downloads)*

Use *Alt-Shift-U* to bring the uploader window up.

## Changelog
**1.2.6**
* Crash-hardening pass across the whole addon so a bad server response, an odd log file, a large config value, or shutdown timing can no longer take the game down:
  * The upload thread now parses the dps.report response defensively and can never let an exception escape (an unexpected/HTML/rate-limited response used to unwind off the worker thread and crash the game at character select). Failing logs are marked so they are not retried and re-crashed on every launch.
  * Every arcdps callback (`mod_init`, `mod_imgui`, `mod_wnd`, `mod_combat`, options) is now exception-guarded — nothing can unwind across arcdps' C ABI. A failed init disables the addon instead of crashing the game.
  * All fixed-size buffer copies (user token, webhook name/URL/filter) are bounds-checked, so an over-long stored value can't overflow.
  * The single SQLite connection is now serialized behind a mutex (it was used from the imgui, upload, and refresh threads at once), and the webhook list, user token, and relevant settings are synchronized across threads — fixing intermittent, machine-dependent crashes.
  * The log-refresh walk no longer throws on an unreadable folder or an unusually named log; the upload thread shuts down cleanly (no more possible hang on exit); and webhook add/delete no longer invalidates the list mid-iteration.
* No user-facing behavior changes — this release is entirely stability.

**1.2.5**
* Fixed a crash (Windows exception `0xc0000409`) that could take the game down at the character-select screen. Status text and log timestamps were drawn with `ImGui::Text(str.c_str())`, treating the string as a printf format; a `%` in the text — common in a raw server response body or a percent-encoded URL — was parsed as a format specifier and tripped the CRT, hard-crashing the game before any crash log was written. These are now drawn with `ImGui::TextUnformatted`. The smoke test now draws a status message full of format specifiers every frame as a regression guard.

**1.2.4**
* Shift+click selects the whole range of fights between the last clicked fight and the shift-clicked one; plain clicks still toggle fights individually
* "Copy & Format Recent Clears" now copies today's clears by default (since local midnight); the previous minutes-back window is available via "Recent clears: today only" under Options → Other

**1.2.3**
* Wingman links resolve correctly now (the page slug is nested in the API response; it was read from the wrong place, so WM buttons never appeared)
* A log Wingman already has ("already exists") now counts as uploaded and gets its page link fetched
* Renamed the "View" button to "DPS" (opens dps.report; "WM" opens Wingman)
* Added `uploader_livetest`, a local end-to-end harness that runs the real upload pipeline against a directory of logs and verifies the stored links

**1.2.2**
* "WM" button next to "View" in the log table opens the log's Wingman page (the page is looked up after each Wingman upload and stored with the log)
* Database schema changes are now non-destructive: new columns are added via in-place `ALTER TABLE`, with an automatic `uploader.db.bak` backup taken before every schema sync
* Default copy format is now one line per fight (`@1 - *@2*\n`)
* The uploader window opens on the character select and loading screens again (1.1.0 regression)
* A pending update now pops up a notice at the character select screen, the way arcdps surfaces its own updates (dismissable per session)

**1.2.0**
* Self-updater: checks GitHub releases at startup (arcdps-style — downloads into `addons\uploader\`, swaps via rename, new version loads on the next game start; previous version kept as `d3d9_uploader.dll_prev` for rollback). Toggle with "Auto-update on launch" under Options → Other.
* Wingman support: optionally upload every log to [gw2wingman](https://gw2wingman.nevermindcreations.de) after the dps.report upload (Options → Wingman; set your account name; WvW logs are skipped as Wingman does not accept them).

**1.1.2**
* Added missing encounter IDs to Discord webhook category routing — these encounters previously fell to UNKNOWN and were silently never posted:
  * Raids: Greer, Decima, Ura (Wing 8 — Mount Balrior), Kela (Guardian's Glade, Visions of Eternity), Spirit Race (Wing 1 event)
  * Fractals: Kanaxai (Silent Surf CM), Eparch (Lonely Tower CM), Whispering Shadow (Kinfall)
  * Strikes: the third Captain Mai Trin encounter ID (was defined but missing from the routing table)
* The `revtc` submodule is now vendored directly into the repo (no more `git submodule update --init` after cloning)
* Smoke test now pins the encounter→category mapping

**1.1.1**
* "Copy & Format Selected" no longer copies only the date when the configured format string is empty — an empty `Msg_Format` now falls back to the default format (`@1 - \n*@2*\n\n`)
* Fixed the format-string escape parser eating the character after a `\` that wasn't part of `\n`

**1.1.0**
* Ported the bundled imgui from 1.80 to 1.92.7 (19270) so the extension loads natively on current arcdps instead of crashing through the legacy shim
* Route imgui allocations through the allocator arcdps provides (`ImGui::SetAllocatorFunctions`) — required for a shared context; skipping it corrupts the game heap when the window opens
* The imgui callback now receives `not_charsel_or_loading` and skips drawing during character select and loading screens
* Added a headless smoke test that drives the real `get_init_addr` entrypoint, opens the window via the actual Alt+Shift+U path, draws 120 frames, and asserts allocator routing — runs in CI on every build
* CI: moved off the retired `windows-2019` runner, artifacts uploaded on every build, releases on tags

**1.0.1**
* Added Aleeva integration
* Contributed by @covertPZ: Additional user configuration for formatted log messages

**0.9.3**
* Use a fixed window size to prevent auto-resize feedback loop (Fixes #7)
* View button opens logs in default browser (#8)
* Auto refresh every minute instead of every two minutes

**0.9:**
- Automatically uploads logs when not in combat
- Discord Webhook integration
  - Filter by success, log type, or players present
- Removed (unmaintained) built-in parser

**0.8:**
- Cache all parsed logs to prevent any refresh delay for users with 500+ logs
- Add Wing 7 Boss Names and ID's
- Improve parsing accuracy (dps should match Elite Insights)

## ToS Compliance
This extension has minimal interaction with GW2 (basically it displays a window), and is essentially a QoL upgrade over uploading the logs yourself. It provides no inherent advantage over other players. As such, I believe it to be ToS compliant.

If you have any doubts, refer to [Arenanet's policy on Third-Party Programs](https://en-forum.guildwars2.com/discussion/65547/policy-third-party-programs).

## Support
Please open an issue and leave a detailed description of your problem, feature request, etc.

## Attribution
This fork makes use of [revtc](https://github.com/datatobridge/revtc), an EVTC parsing library by **datatobridge** (the original author of this uploader), which is vendored directly into this repository under the `revtc/` directory (MIT licensed — see `revtc/LICENSE`). It provides the boss ID tables and encounter category routing used for Discord webhook filtering.

*Thanks to nbarrios/datatobridge for writing the original uploader and revtc, Arc/Delta for writing and supporting Arcdps, and the Elite Insights team for their excellent parser*
