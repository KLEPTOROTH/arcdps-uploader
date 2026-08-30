#include "Uploader.h"

#include <ShlObj.h>

#include <nlohmann/json.hpp>
#include <thread>

#include "Aleeva.h"
#include "Updater.h"
#include "Wingman.h"
#include "imgui/imgui.h"
#include "imgui/imgui_stdlib.h"
#include "loguru.hpp"

using json = nlohmann::json;

inline auto initStorage(const std::string& path) {
    using namespace sqlite_orm;
    return make_storage(
        path,
        make_table("logs",
                   make_column("id", &Log::id, autoincrement(), primary_key()),
                   make_column("path", &Log::path),
                   make_column("filename", &Log::filename),
                   make_column("human_time", &Log::human_time),
                   make_column("time", &Log::time),
                   make_column("uploaded", &Log::uploaded),
                   make_column("error", &Log::error),
                   make_column("report_id", &Log::report_id),
                   make_column("permalink", &Log::permalink),
                   make_column("boss_id", &Log::boss_id),
                   make_column("boss_name", &Log::boss_name),
                   make_column("players_json", &Log::players_json),
                   make_column("json_available", &Log::json_available),
                   make_column("success", &Log::success),
                   make_column("wingman_link", &Log::wingman_link)),
        make_table(
            "webhooks",
            make_column("id", &Webhook::id, autoincrement(), primary_key()),
            make_column("name", &Webhook::name),
            make_column("url", &Webhook::url),
            make_column("raids", &Webhook::raids),
            make_column("fractals", &Webhook::fractals),
            make_column("strikes", &Webhook::strikes),
            make_column("golems", &Webhook::golems),
            make_column("wvw", &Webhook::wvw),
            make_column("filter", &Webhook::filter),
            make_column("filter_min", &Webhook::filter_min),
            make_column("success", &Webhook::success)),
        make_table(
            "usertokens",
            make_column("id", &UserToken::id, autoincrement(), primary_key()),
            make_column("value", &UserToken::value),
            make_column("disabled", &UserToken::disabled)));
}
using Storage = decltype(initStorage(""));
static std::unique_ptr<Storage> storage;

// One sqlite connection is shared by the imgui thread, the upload thread and
// the async refresh. sqlite_orm is not safe for concurrent use of a single
// connection, so every storage-> access is serialized behind this mutex.
// Recursive so a locked path may call another locked helper; it is never held
// across network I/O (cpr calls), only around the actual db operations.
static std::recursive_mutex db_mutex;
using db_lock = std::lock_guard<std::recursive_mutex>;

// The recent-log list is capped at this size everywhere: it bounds both the
// `logs` query (limit) and the fixed `selected[]` selection array, so they can
// never disagree and index out of bounds.
static constexpr int kMaxLogs = 75;

// Copy a std::string into a fixed char buffer: never overflows (truncates a
// too-long source) and always leaves the buffer NUL-terminated. Guards every
// string->buffer copy against oversized db/server values (/GS 0xc0000409).
static void safe_copy(char* dst, size_t dst_size, const std::string& src) {
    if (!dst || dst_size == 0) return;
    size_t n = src.size();
    if (n > dst_size - 1) n = dst_size - 1;
    memcpy(dst, src.data(), n);
    dst[n] = '\0';
}
template <size_t N>
static void safe_copy(char (&dst)[N], const std::string& src) {
    safe_copy(dst, N, src);
}

// Exception-safe parse of a dps.report /uploadContent response body into `log`.
// Fills token_out with any returned userToken. Returns true only when a usable
// permalink was found. NEVER throws: a malformed, HTML, empty, or wrong-shaped
// body just yields false. This is the guard that keeps a bad server response
// from unwinding off the upload thread (std::terminate -> 0xc0000409).
bool parse_dpsreport_response(const std::string& body, Log& log,
                             std::string& token_out) {
    try {
        json parsed = json::parse(body);
        log.report_id = parsed.value("id", std::string());
        log.permalink = parsed.value("permalink", std::string());
        if (parsed.contains("encounter") && parsed["encounter"].is_object()) {
            const json& enc = parsed["encounter"];
            log.boss_id = enc.value("bossId", 0);
            log.boss_name = enc.value("boss", std::string());
            log.json_available = enc.value("jsonAvailable", false);
            log.success = enc.value("success", false);
        }
        if (parsed.contains("players")) {
            log.players_json = parsed["players"].dump();
        }
        token_out = parsed.value("userToken", std::string());
        return !log.permalink.empty();
    } catch (...) {
        return false;
    }
}

Uploader::Uploader(fs::path data_path, std::optional<fs::path> custom_log_path)
    : is_open(false), in_combat(false), settings(data_path / "uploader.ini") {
    // Load settings from INI
    settings.load();

    // Sqlite Database
    fs::path db_path = data_path / "uploader.db";
    LOG_F(INFO, "DB Path: %s", db_path.string().c_str());

    // Schema changes must never go through sqlite_orm's sync_schema
    // rebuild: it drops and recreates tables when it cannot ALTER,
    // destroying upload history (v1.2.1 wiped the logs table this way).
    // Back the db up and add new columns in place first, so sync_schema
    // sees a matching schema and leaves the data alone.
    if (fs::exists(db_path)) {
        std::error_code ec;
        fs::copy_file(db_path, data_path / "uploader.db.bak",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_F(WARNING, "Failed to back up db: %s", ec.message().c_str());
        }
        sqlite3* raw = nullptr;
        if (sqlite3_open(db_path.string().c_str(), &raw) == SQLITE_OK) {
            // No-op (harmless error) when the column already exists.
            sqlite3_exec(raw,
                         "ALTER TABLE logs ADD COLUMN wingman_link TEXT NOT "
                         "NULL DEFAULT ''",
                         nullptr, nullptr, nullptr);
            sqlite3_close(raw);
        }
    }

    storage = std::make_unique<Storage>(initStorage(db_path.string()));

    storage->sync_schema(true);
    storage->open_forever();

    // dps.report User Token
    userTokens = storage->get_all<UserToken>();
    if (userTokens.size() == 0) {
        userToken.id = -1;
        userToken.value = "";
        userToken.disabled = true;
        storage->insert(userToken);
        userTokens = storage->get_all<UserToken>();
    }
    userToken.id = userTokens.front().id;
    userToken.value = userTokens.front().value;
    userToken.disabled = userTokens.front().disabled;
    if (userToken.disabled) {
        safe_copy(userToken.value_buf, "--DISABLED--");
    } else {
        safe_copy(userToken.value_buf, userToken.value);
    }

    // Webhooks
    webhooks = storage->get_all<Webhook>();
    for (auto& wh : webhooks) {
        safe_copy(wh.name_buf, wh.name);
        safe_copy(wh.url_buf, wh.url);
        safe_copy(wh.filter_buf, wh.filter);
    }

    if (custom_log_path) {
        log_path = *custom_log_path;
    } else {
        /* my documents */
        WCHAR my_documents[MAX_PATH];
        HRESULT result = SHGetFolderPath(NULL, CSIDL_MYDOCUMENTS, NULL,
                                         SHGFP_TYPE_CURRENT, my_documents);
        if (result == S_OK) {
            // TODO: disable the whole thing if we can't find docs?
            CHAR utf_path[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, my_documents, -1, utf_path,
                                MAX_PATH, NULL, NULL);
            std::string mydocs = std::string(utf_path);
            fs::path mydocs_path = fs::path(mydocs);
            LOG_F(INFO, "Documents Path: %s", mydocs_path.string().c_str());
            log_path =
                mydocs_path / "Guild Wars 2\\addons\\arcdps\\arcdps.cbtlogs\\";
        } else {
            LOG_F(ERROR, "Failed to find Documents paths. Fatal.");
        }
    }

    LOG_F(INFO, "Logs Path: %s", log_path.string().c_str());
    std::error_code log_path_ec;
    if (!std::filesystem::exists(log_path, log_path_ec) || log_path_ec) {
        {
            StatusMessage msg;
            msg.msg =
                "Log path not found. Is Arcdps logging enabled and is the log "
                "path valid?";
            std::lock_guard<std::mutex> lk(ts_msg_mutex);
            thread_status_messages.push_back(msg);
        }
    }
}

Uploader::~Uploader() {
    LOG_F(INFO, "Uploader destructor begin...");
    // Save our settings if we previously loaded/created an ini file
    settings.save();

    // Stop the upload thread. Set the flag under the same mutex the cv waits
    // on, then notify, so the wakeup can never be lost (otherwise the join
    // below can hang forever and GW2 never exits).
    {
        std::lock_guard<std::mutex> lk(ut_mutex);
        upload_thread_run = false;
    }
    ut_cv.notify_all();
    if (upload_thread.joinable()) upload_thread.join();
}

uintptr_t Uploader::imgui_tick(uint32_t not_charsel_or_loading) {
    // At character select / loading screens, surface a pending update the
    // way arcdps surfaces its own update notes.
    if (!not_charsel_or_loading) {
        imgui_draw_update_notice();
    }

#ifdef STANDALONE
    if (1) {
#else
    if (is_open) {
#endif
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);

        if (!ImGui::Begin("Uploader", &is_open,
                          ImGuiWindowFlags_AlwaysAutoResize |
                              ImGuiWindowFlags_NoCollapse)) {
            ImGui::End();
            return uintptr_t();
        }

        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(0.f, 1.f, 0.f, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.f, 1.f, 0.f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.f, 1.f, 0.f, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                              ImVec4(0.f, 1.f, 0.f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.f, 1.f, 0.f, 0.25f));

        imgui_draw_logs();

        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::Separator();

        imgui_draw_status();
        imgui_draw_options();

        if (in_combat) {
            ImGui::TextColored(ImVec4(1.f, 0.f, 0.f, 1.f),
                               "In Combat - Uploads Disabled");
        }

        {
            auto us = Updater::get_state();
            if (us.status == Updater::Status::STAGED) {
                ImGui::TextColored(ImVec4(0.f, 1.f, 0.f, 1.f), "%s",
                                   us.message.c_str());
            } else if (us.status == Updater::Status::AVAILABLE) {
                ImGui::TextColored(ImVec4(1.f, 1.f, 0.f, 1.f), "%s",
                                   us.message.c_str());
            } else if (us.status == Updater::Status::ERR) {
                ImGui::TextDisabled("%s", us.message.c_str());
            }
        }

        ImGui::PopStyleColor();
        ImGui::PopStyleColor();
        ImGui::PopStyleColor();
        ImGui::PopStyleColor();
        ImGui::PopStyleColor();

        ImGui::End();

        ImGui::PopStyleVar();

        // Pick up any messages from our upload thread
        {
            std::lock_guard<std::mutex> lk(ts_msg_mutex);
            status_messages.insert(status_messages.end(),
                                   thread_status_messages.begin(),
                                   thread_status_messages.end());
            thread_status_messages.clear();
        }
    }

    if (!in_combat) {
        poll_async_refresh_log_list();
    }

    return uintptr_t();
}

void Uploader::imgui_draw_update_notice() {
    if (update_notice_dismissed) return;
    auto us = Updater::get_state();
    if (us.status != Updater::Status::STAGED &&
        us.status != Updater::Status::AVAILABLE) {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Uploader Update", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoCollapse)) {
        if (us.status == Updater::Status::STAGED) {
            ImGui::TextColored(ImVec4(0.f, 1.f, 0.f, 1.f), "%s",
                               us.message.c_str());
            ImGui::TextUnformatted(
                "The update was downloaded automatically and takes effect "
                "the next time the game starts.");
        } else {
            ImGui::TextColored(ImVec4(1.f, 1.f, 0.f, 1.f), "%s",
                               us.message.c_str());
            ImGui::TextUnformatted(
                "Auto-update is off or unavailable; grab it from the "
                "GitHub releases page.");
        }
        if (ImGui::Button("Dismiss")) {
            update_notice_dismissed = true;
        }
    }
    ImGui::End();
}

static void open_url_in_browser(const std::string& url) {
    if (url.empty()) return;
    int sz =
        MultiByteToWideChar(CP_UTF8, 0, url.c_str(), (int)url.size(), 0, 0);
    std::wstring wstr(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), (int)url.size(), &wstr[0],
                        sz);
    ShellExecute(0, 0, wstr.c_str(), 0, 0, SW_SHOW);
}

void Uploader::imgui_draw_logs() {
    static bool success_only = false;

    static ImVec2 log_size(450, 258);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Recent Logs");
    ImGui::SameLine(450.f - 170.f);
    ImGui::Checkbox("Filter Wipes", &success_only);
    ImGui::SameLine(450.f - 54.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.f, 3.f));
    if (ImGui::Button("Refresh")) {
        start_async_refresh_log_list();
    }
    ImGui::PopStyleVar();

    ImGui::BeginChild("List", log_size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);

    ImGui::Columns(3, "mycolumns");
    float last_col = log_size.x - ImGui::CalcTextSize("DPS WM").x * 1.9f;
    ImGui::SetColumnOffset(0, 0);
    ImGui::SetColumnOffset(
        1, last_col - ImGui::CalcTextSize("00:00PM (Mon Jan 00)").x * 1.1f);
    ImGui::SetColumnOffset(2, last_col);
    ImGui::TextUnformatted("Name");
    ImGui::NextColumn();
    ImGui::TextUnformatted("Created");
    ImGui::NextColumn();
    ImGui::TextUnformatted("");
    ImGui::NextColumn();
    ImGui::Separator();
    static bool selected[kMaxLogs]{false};
    static int select_anchor = -1;
    for (int i = 0; i < logs.size(); ++i) {
        Log& s = logs.at(i);
        std::string display;
        if (s.uploaded) {
            display = s.boss_name;
        } else {
            display = s.filename;
        }

        ImVec4 col = ImVec4(1.f, 0.f, 0.f, 1.f);
        if (s.success) {
            col = ImVec4(0.f, 1.f, 0.f, 1.f);
        } else if (success_only) {
            continue;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::PushID(s.human_time.c_str());
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::Selectable(display.c_str(), &selected[i],
                              ImGuiSelectableFlags_SpanAllColumns)) {
            if (ImGui::GetIO().KeyShift && select_anchor >= 0 &&
                select_anchor < (int)logs.size()) {
                // Shift+click: select the whole range from the anchor,
                // skipping rows hidden by the wipe filter. The anchor is
                // kept so repeated shift-clicks extend from the same spot.
                int lo = (std::min)(select_anchor, i);
                int hi = (std::max)(select_anchor, i);
                for (int k = lo; k <= hi && k < kMaxLogs; k++) {
                    if (success_only && !logs.at(k).success) continue;
                    selected[k] = true;
                }
            } else {
                // Plain click toggles one row (non-contiguous selection)
                // and becomes the new range anchor.
                select_anchor = i;
            }
        }
        ImGui::PopID();
        ImGui::PopStyleColor();
        ImGui::NextColumn();
        ImGui::TextUnformatted(s.human_time.c_str());
        ImGui::NextColumn();
        if (s.uploaded) {
            ImGui::PushID(s.filename.c_str());
            if (ImGui::SmallButton("DPS")) {
                open_url_in_browser(s.permalink);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Open on dps.report");
                ImGui::EndTooltip();
            }
            if (!s.wingman_link.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("WM")) {
                    open_url_in_browser(s.wingman_link);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted("Open on Wingman");
                    ImGui::EndTooltip();
                }
            }
            ImGui::PopID();
        }

        ImGui::NextColumn();

        if (logs.size() < 9 && i == logs.size() - 1) {
            log_size.y = ImGui::GetCursorPosY();
        } else if (i == 9) {
            log_size.y = ImGui::GetCursorPosY();
        }
    }
    ImGui::Columns();
    ImGui::EndChild();

    if (ImGui::Button("Copy Selected")) {
        std::string msg;
        for (int i = 0; i < logs.size(); ++i) {
            if (selected[i]) {
                const Log& s = logs.at(i);
                msg += s.permalink + "\n";
            }
        }
        ImGui::SetClipboardText(msg.c_str());
    }

    ImGui::SameLine();

    if (ImGui::Button("Copy & Format Selected")) {
        std::time_t now = std::time(nullptr);
        std::tm* local = std::localtime(&now);
        char buf[64];
        strftime(buf, 64, "__**%b %d %Y**__\n\n", local);

        std::string msg(buf);

        for (int i = 0; i < logs.size(); ++i) {
            if (selected[i]) {
                const Log& s = logs.at(i);
                msg += format_msg(s);
            }
        }
        ImGui::SetClipboardText(msg.c_str());
    }

    ImGui::SameLine();

    if (ImGui::Button("Copy & Format Recent Clears")) {
        std::time_t now = std::time(nullptr);
        std::tm* local = std::localtime(&now);
        char buf[64];
        strftime(buf, 64, "__**%b %d %Y**__\n\n", local);

        std::string msg(buf);

        std::chrono::system_clock::time_point past;
        if (settings.recent_clears_today) {
            // Clears from today: everything since local midnight.
            std::tm midnight = *local;
            midnight.tm_hour = 0;
            midnight.tm_min = 0;
            midnight.tm_sec = 0;
            midnight.tm_isdst = -1;
            past = std::chrono::system_clock::from_time_t(
                std::mktime(&midnight));
        } else {
            past = std::chrono::system_clock::now() -
                   std::chrono::minutes(settings.recent_minutes);
        }
        for (int i = 0; i < logs.size(); ++i) {
            const Log& s = logs.at(i);
            if (s.uploaded && s.success) {
                if (s.time > past) {
                    msg += format_msg(s);
                }
            }
        }
        ImGui::SetClipboardText(msg.c_str());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(
            settings.recent_clears_today
                ? "Copies all of today's clears (configurable under Other)"
                : "Copies clears from the configured window (see Other)");
        ImGui::EndTooltip();
    }

#ifdef STANDALONE
    ImGui::SameLine();
    if (ImGui::Button("Reupload")) {
        std::vector<int> queue;
        for (int i = 0; i < logs.size(); ++i) {
            if (selected[i]) {
                Log& s = logs.at(i);
                queue.push_back(s.id);
            }
        }
        add_pending_upload_logs(queue);
    }
#endif
}

void Uploader::imgui_draw_status() {
    ImGui::TextUnformatted("Status");
    ImGui::BeginChild("Status Messages", ImVec2(450, 150), ImGuiChildFlags_Borders);

    for (const auto& status : status_messages) {
        ImGui::TextUnformatted(status.msg.c_str());
        if (status.log_id > 0) {
            std::unique_ptr<Log> log;
            try {
                db_lock dlk(db_mutex);
                log = storage->get_pointer<Log>(status.log_id);
            } catch (...) {
                log.reset();  // never unwind through the imgui render stack
            }
            if (log) {
                if (log->permalink.size() > 8) {
                    ImGui::Text("%s",
                                log->permalink.substr(8, log->permalink.size())
                                    .c_str());
                } else {
                    ImGui::Text("%s", log->permalink.c_str());
                }
                ImGui::SameLine();
                ImGui::PushID(std::string("Url" + log->permalink).c_str());
                if (ImGui::SmallButton("Copy")) {
                    ImGui::SetClipboardText(log->permalink.c_str());
                }
                ImGui::PopID();
            }
        }
    }
    static uint8_t status_message_count = 0;
    if (status_messages.size() > status_message_count) {
        ImGui::SetScrollHereY();
    }
    status_message_count = (uint8_t)status_messages.size();

    ImGui::EndChild();
}

void Uploader::imgui_draw_options() {
    if (ImGui::CollapsingHeader("Options")) {
        if (ImGui::TreeNode("dps.report User Token")) {
            ImGui::PushItemWidth(250);
            ImGui::InputText(
                "userToken", userToken.value_buf, sizeof(userToken.value_buf),
                (userToken.disabled && ImGuiInputTextFlags_ReadOnly));
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text(
                    "userToken used by dps.report. Do not share this token "
                    "with "
                    "others, unless you know what you are doing!");
                ImGui::EndTooltip();
            }
            if (ImGui::Button("Save") && !userToken.disabled) {
                std::lock_guard<std::mutex> ulk(ui_mutex);
                userToken.value = userToken.value_buf;
                db_lock lk(db_mutex);
                storage->update(userToken);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text(
                    "Applies the userToken. It will then be used for future "
                    "uploads.");
                ImGui::EndTooltip();
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset")) {
                ImGui::OpenPopup("Reset_Confirm");
            }
            if (ImGui::BeginPopup("Reset_Confirm")) {
                if (ImGui::Button("Confirm")) {
                    std::lock_guard<std::mutex> ulk(ui_mutex);
                    safe_copy(userToken.value_buf, "");
                    userToken.value = "";
                    userToken.disabled = false;
                    db_lock lk(db_mutex);
                    storage->update(userToken);
                }
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text(
                    "Clear the current userToken. On the next upload a new "
                    "token is "
                    "generated wich will then be used for future uploads.");
                ImGui::Text("Your current userToken will be lost!");
                ImGui::EndTooltip();
            }
            ImGui::SameLine();
            if (userToken.disabled) {
                if (ImGui::Button("Enable")) {
                    std::lock_guard<std::mutex> ulk(ui_mutex);
                    safe_copy(userToken.value_buf, userToken.value);
                    userToken.disabled = false;
                    db_lock lk(db_mutex);
                    storage->update(userToken);
                }
            } else {
                if (ImGui::Button("Disable")) {
                    std::lock_guard<std::mutex> ulk(ui_mutex);
                    safe_copy(userToken.value_buf, "--DISABLED--");
                    userToken.disabled = true;
                    db_lock lk(db_mutex);
                    storage->update(userToken);
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text(
                    "If the userToken is disabled, it will no longer be sent "
                    "to "
                    "dps.report. This means that a new userToken will be "
                    "generated for "
                    "every upload.");
                ImGui::EndTooltip();
            }
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Webhooks (Discord)")) {
            int webhook_to_delete = -1;
            for (auto& wh : webhooks) {
                ImGui::BeginChild(
                    wh.name.c_str(),
                    ImVec2(ImGui::GetContentRegionAvail().x, 148), ImGuiChildFlags_Borders);

                ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x -
                                     ImGui::CalcTextSize("Filter").x - 1);
                ImGui::InputText("Name", wh.name_buf, 64);
                ImGui::PopItemWidth();
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text(
                        "For display purposes only (64 characters max)");
                    ImGui::EndTooltip();
                }

                ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x -
                                     ImGui::CalcTextSize("Filter").x - 1);
                ImGui::InputText("URL", wh.url_buf, 192);
                ImGui::PopItemWidth();
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("The webhook URL, copy and paste from Discord");
                    ImGui::EndTooltip();
                }

                ImGui::Checkbox("Raids", &wh.raids);
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Use this webhook for raid logs");
                    ImGui::EndTooltip();
                }
                ImGui::SameLine();

                ImGui::Checkbox("Fractals", &wh.fractals);
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Use this webhook for fractal logs");
                    ImGui::EndTooltip();
                }
                ImGui::SameLine();

                ImGui::Checkbox("Strikes", &wh.strikes);
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Use this webhook for strike logs");
                    ImGui::EndTooltip();
                }
                ImGui::SameLine();

                ImGui::Checkbox("Golems", &wh.golems);
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Use this webhook for golem logs");
                    ImGui::EndTooltip();
                }
                ImGui::SameLine();

                ImGui::Checkbox("WvW", &wh.wvw);
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Use this webhook for WvW logs");
                    ImGui::EndTooltip();
                }

                ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x -
                                     ImGui::CalcTextSize("Filter").x - 1);
                ImGui::InputText("Filter", wh.filter_buf, 256);
                ImGui::PopItemWidth();
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text(
                        "Account Names (account.1234, another.5677, ...)");
                    ImGui::Text(
                        "Logs will only be posted if at least the minimum "
                        "number of "
                        "accounts listed here are present");
                    ImGui::EndTooltip();
                }

                ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x *
                                     0.25f);
                ImGui::InputInt("Min", &wh.filter_min, 1, 2);
                ImGui::PopItemWidth();
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text(
                        "Use this webhook only if the minimum number of "
                        "accounts\nfrom "
                        "the list above are present");
                    ImGui::EndTooltip();
                }
                ImGui::SameLine();

                ImGui::Checkbox("Clears Only", &wh.success);
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Use this webhook for clears/success only");
                    ImGui::EndTooltip();
                }

                if (ImGui::Button("Save")) {
                    std::lock_guard<std::mutex> lk(wh_mutex);
                    wh.name = wh.name_buf;
                    wh.url = wh.url_buf;
                    wh.filter = wh.filter_buf;
                    db_lock dlk(db_mutex);
                    storage->update(wh);
                }
                ImGui::SameLine();

                if (ImGui::Button("Delete")) {
                    ImGui::OpenPopup("Delete_Confirm");
                }
                if (ImGui::BeginPopup("Delete_Confirm")) {
                    if (ImGui::Button("Confirm")) {
                        // Defer the actual delete+reload until after the loop:
                        // reassigning `webhooks` here would invalidate this
                        // range-for's iterator.
                        webhook_to_delete = wh.id;
                    }
                    ImGui::EndPopup();
                }

                ImGui::EndChild();
            }

            if (webhook_to_delete >= 0) {
                {
                    db_lock dlk(db_mutex);
                    storage->remove<Webhook>(webhook_to_delete);
                }
                reload_webhooks();
            }

            ImGui::Separator();
            if (ImGui::Button("Add Webhook")) {
                Webhook nwh = {};
                nwh.id = -1;
                nwh.raids = true;
                nwh.fractals = true;
                nwh.strikes = true;
                nwh.golems = true;
                nwh.wvw = true;
                nwh.success = true;
                nwh.filter_min = 10;
                {
                    db_lock dlk(db_mutex);
                    storage->insert(nwh);
                }
                reload_webhooks();
            }

            ImGui::TreePop();
        }

        //Aleeva
        imgui_draw_options_aleeva();

        if (ImGui::TreeNode("GW2Bot")) {
            ImGui::Checkbox("GW2Bot Integration Enabled",
                            &settings.gw2bot_enabled);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text(
                    "Post logs to GW2Bot for use with the EVTC Automation "
                    "commands.");
                ImGui::EndTooltip();
            }

            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                ImGui::TextUnformatted(
                    "Use the /evtc_automation autopost add_destination command "
                    "to have "
                    "GW2Bot post logs to a Discord channel.\nSee "
                    "gw2bot.info/commands "
                    "for details.");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }

            if (settings.gw2bot_enabled) {
                ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x -
                                     ImGui::CalcTextSize("EVTC Api Key").x - 5);
                {
                    // Locked: the upload thread snapshots gw2bot_key; this
                    // InputText can reallocate the std::string as the user types.
                    std::lock_guard<std::mutex> ulk(ui_mutex);
                    ImGui::InputText("EVTC Api Key", &settings.gw2bot_key);
                }
                ImGui::PopItemWidth();
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text(
                        "Use GW2Bot's /evtc_automation api_key command to "
                        "generate an "
                        "API key.");
                    ImGui::EndTooltip();
                }

                ImGui::Checkbox("Clears only", &settings.gw2bot_success_only);
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Only post clears/successful logs to GW2Bot.");
                    ImGui::EndTooltip();
                }
            }

            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Wingman")) {
            ImGui::Checkbox("Upload to Wingman", &settings.wingman_enabled);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text(
                    "Also upload each log to gw2wingman.nevermindcreations.de "
                    "after the dps.report upload. WvW logs are skipped "
                    "(Wingman does not accept them).");
                ImGui::EndTooltip();
            }

            if (settings.wingman_enabled) {
                ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x -
                                     ImGui::CalcTextSize("Account name").x - 5);
                {
                    // Locked: check_wingman snapshots wingman_account on the
                    // upload thread while this edits it.
                    std::lock_guard<std::mutex> ulk(ui_mutex);
                    ImGui::InputText("Account name", &settings.wingman_account);
                }
                ImGui::PopItemWidth();
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text(
                        "Your GW2 account name (Name.1234) so Wingman can "
                        "attribute the logs.");
                    ImGui::EndTooltip();
                }
            }

            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Other")) {
            ImGui::Checkbox("Auto-update on launch", &settings.auto_update);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text(
                    "Check GitHub for a new uploader version at startup. "
                    "Updates download in the background and load on the next "
                    "game start.");
                ImGui::EndTooltip();
            }

            ImGui::Checkbox("Enable detailed WvW reports",
                            &settings.wvw_detailed_enabled);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text(
                    "Particularly long logs may fail to upload if this is "
                    "enabled");
                ImGui::EndTooltip();
            }

            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x -
                ImGui::CalcTextSize("Formatted log output").x - 5);
            ImGui::InputText("Formatted log string", &settings.msg_format);
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text(
                    "@1: Boss Name\n"
                    "@2: dps.report link\n"
                    "\\n: new line");
                ImGui::EndTooltip();
            }

            ImGui::Checkbox("Recent clears: today only",
                            &settings.recent_clears_today);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text(
                    "\"Copy & Format Recent Clears\" copies clears since "
                    "midnight. Uncheck to use a minutes-back window instead.");
                ImGui::EndTooltip();
            }
            if (!settings.recent_clears_today) {
                ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.25f);
                ImGui::InputInt("# of minutes back for recent clears",
                                &settings.recent_minutes);
                ImGui::PopItemWidth();
            }

            ImGui::TreePop();
        }
    }
}

void Uploader::imgui_draw_options_aleeva() {
    if (ImGui::TreeNode("Aleeva")) {
        ImGui::Checkbox("Aleeva Integration Enabled", &settings.aleeva.enabled);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("Post logs for Aleeva to manage");
            ImGui::EndTooltip();
        }

        if (settings.aleeva.enabled) {
            if (!settings.aleeva.authorised) {
                const char* access_title = "Access Code";
                ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x -
                                     ImGui::CalcTextSize(access_title).x - 5);
                {
                    // Locked: the Aleeva login task reads settings on another
                    // thread while this edits the access code.
                    std::lock_guard<std::mutex> ulk(ui_mutex);
                    ImGui::InputText(access_title, &settings.aleeva.access_code);
                }
                ImGui::PopItemWidth();
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text(
                        "Use Aleeva's /profile command to generate an "
                        "access code");
                    ImGui::EndTooltip();
                }

                if (ImGui::Button("Login")) {
                    auto future = std::async(
                        std::launch::async, [this]() {
                            std::string msg =
                                "Aleeva login failed. Please check your access "
                                "code and try to login again.";
                            try {
                                if (Aleeva::login(settings)) {
                                    msg = "Aleeva login successful.";
                                }
                            } catch (...) {
                            }
                            queue_status_message(msg);
                        });
                }
            } else {
                ImGui::SameLine();
                if (ImGui::Button("Logout")) {
                    Aleeva::deauthorize(settings);
                }

                ImGui::Checkbox("Post To Discord", &settings.aleeva.should_post);
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Have Aleeva post logs to the selected Discord channel.");
                    ImGui::EndTooltip();
                }
                if (settings.aleeva.should_post) {
                    ImGui::Indent();

                    const char* server_title = "";
                    for (const auto& server : settings.aleeva.server_ids) {
                        if (server.id == settings.aleeva.selected_server_id) {
                            server_title = server.name.c_str();
                            break;
                        }
                    }
                    if (ImGui::BeginCombo("Server", server_title, ImGuiComboFlags_None)) {
                        for (auto& server : settings.aleeva.server_ids) {
                            bool is_selected = (server.id == settings.aleeva.selected_server_id);
                            if (ImGui::Selectable(server.name.c_str(), is_selected)) {
                                settings.aleeva.selected_server_id = server.id;
                            }
                            
                            if (is_selected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }

                        ImGui::EndCombo();
                    }

                    const char* channel_title = "";
                    if (settings.aleeva.channel_ids.count(settings.aleeva.selected_server_id)) {
                        const std::vector<Aleeva::DiscordId>& channel_ids = 
                            settings.aleeva.channel_ids[settings.aleeva.selected_server_id];
                        for (const auto& channel : channel_ids) {
                            if (channel.id == settings.aleeva.selected_channel_id) {
                                channel_title = channel.name.c_str();
                                break;
                            }
                        }
                    }
                    if (ImGui::BeginCombo("Channel", channel_title, ImGuiComboFlags_None)) {
                        const std::vector<Aleeva::DiscordId>& channel_ids = 
                            settings.aleeva.channel_ids[settings.aleeva.selected_server_id];

                        for (auto& channel : channel_ids) {
                            bool is_selected = (channel.id == settings.aleeva.selected_server_id);
                            if (ImGui::Selectable(channel.name.c_str(), is_selected)) {
                                settings.aleeva.selected_channel_id = channel.id;
                            }
                            
                            if (is_selected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }

                        ImGui::EndCombo();
                    }

                    ImGui::Unindent();
                }

                ImGui::Checkbox("Clears only", &settings.gw2bot_success_only);
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Only post clears/successful logs to Aleeva.");
                    ImGui::EndTooltip();
                }
            }

        }

        ImGui::TreePop();
    }
}

void Uploader::imgui_window_checkbox() {
    ImGui::Checkbox("Uploader", &is_open);
}

void Uploader::create_log_table(Log& l) {
    auto seconds_to_string = [](uint64_t seconds) -> std::string {
        uint32_t minutes = (uint32_t)seconds / 60;
        float secondsf = fmodf((float)seconds, 60.f);

        char buf[32];
        sprintf(buf, "%02u:%02.0f", minutes, secondsf);
        return std::string(buf);
    };

    static std::map<std::string, ImVec4> colors = {
        {"Druid", ImVec4(17.f / 255.f, 122.f / 255.f, 101.f / 255.f, 1.f)},
        {"Daredevil", ImVec4(133.f / 255.f, 146.f / 255.f, 158.f / 255.f, 1.f)},
        {"Berserker", ImVec4(211.f / 255.f, 84.f / 255.f, 0.f, 1.f)},
        {"Dragonhunter",
         ImVec4(52.f / 255.f, 152.f / 255.f, 219.f / 255.f, 1.f)},
        {"Reaper", ImVec4(20.f / 255.f, 90.f / 255.f, 50.f / 255.f, 1.f)},
        {"Chronomancer",
         ImVec4(142.f / 255.f, 68.f / 255.f, 173.f / 255.f, 1.f)},
        {"Scrapper", ImVec4(230.f / 255.f, 126.f / 255.f, 34.f / 255.f, 1.f)},
        {"Tempest", ImVec4(93.f / 255.f, 173.f / 255.f, 226.f / 255.f, 1.f)},
        {"Herald", ImVec4(84.f / 255.f, 153.f / 255.f, 199.f / 255.f, 1.f)},
        {"Soulbeast", ImVec4(39.f / 255.f, 174.f / 255.f, 96.f / 255.f, 1.f)},
        {"Weaver", ImVec4(192.f / 255.f, 57.f / 255.f, 43.f / 255.f, 1.f)},
        {"Holosmith", ImVec4(243.f / 255.f, 156.f / 255.f, 18.f / 255.f, 1.f)},
        {"Deadye", ImVec4(203.f / 255.f, 67.f / 255.f, 53.f / 255.f, 1.f)},
        {"Mirage", ImVec4(155.f / 255.f, 89.f / 255.f, 182.f, 1.f)},
        {"Scourge", ImVec4(241.f / 255.f, 196.f / 255.f, 15.f / 255.f, 1.f)},
        {"Spellbreaker",
         ImVec4(212.f / 255.f, 172.f / 255.f, 13.f / 255.f, 1.f)},
        {"Firebrand", ImVec4(93.f / 255.f, 173.f / 255.f, 226.f / 255.f, 1.f)},
        {"Renegade", ImVec4(148.f / 255.f, 49.f / 255.f, 38.f / 255.f, 1.f)}};

    if (l.players) {
        /*
        static ImVec2 size = ImVec2(800, 250);
        ImGui::Text("%s (%s)", l.encounter_name.c_str(),
        seconds_to_string(l.parsed.encounter_duration).c_str());
        ImGui::Separator(); ImGui::Spacing(); ImGui::BeginChild("DPS Table",
        size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);

        ImGui::Columns(10);
        ImGui::SetColumnOffset(0, 0); //Sub
        ImGui::SetColumnOffset(1, 15); //Class
        ImGui::SetColumnOffset(2, 15 + 55); //Name
        ImGui::SetColumnOffset(3, 15 + 55 + 180); //Account
        ImGui::SetColumnOffset(4, 15 + 55 + 180 + 180); //Boss DPS
        ImGui::SetColumnOffset(5, 15 + 55 + 180 + 180 + 85); //DPS
        ImGui::SetColumnOffset(6, 15 + 55 + 180 + 180 + 85 + 70); //Might
        ImGui::SetColumnOffset(7, 15 + 55 + 180 + 180 + 85 + 70 + 55); //Fury
        ImGui::SetColumnOffset(8, 15 + 55 + 180 + 180 + 85 + 70 + 55 + 55);
        //Quickness ImGui::SetColumnOffset(9, 15 + 55 + 180 + 180 + 85 + 70 + 55
        + 55 + 55); //Alacrity ImGui::TextUnformatted(""); ImGui::NextColumn();
        ImGui::TextUnformatted("Class"); ImGui::NextColumn();
        ImGui::TextUnformatted("Name"); ImGui::NextColumn();
        ImGui::TextUnformatted("Account"); ImGui::NextColumn();
        ImGui::TextUnformatted("Boss DPS"); ImGui::NextColumn();
        ImGui::TextUnformatted("DPS"); ImGui::NextColumn();
        ImGui::TextUnformatted("Might"); ImGui::NextColumn();
        ImGui::TextUnformatted("Fury"); ImGui::NextColumn();
        ImGui::TextUnformatted("Quick"); ImGui::NextColumn();
        ImGui::TextUnformatted("Alac"); ImGui::NextColumn();
        ImGui::Separator();

        int16_t sub = -1;
        for (const auto& p : l.parsed.players) {
                if (p.subgroup != sub) {
                        ImGui::Separator();
                        sub = p.subgroup;
                }
                ImGui::Text("%u", p.subgroup); ImGui::NextColumn();
                if (colors.count(p.elite_spec_name)) {
                        ImGui::TextColored(colors.at(p.elite_spec_name), "%s",
        p.elite_spec_name == "Unknown" ? p.profession_name_short.c_str() :
        p.elite_spec_name_short.c_str()); } else { ImGui::Text("%s",
        p.elite_spec_name == "Unknown" ? p.profession_name_short.c_str() :
        p.elite_spec_name_short.c_str());
                }
                ImGui::NextColumn();
                ImGui::Text("%s", p.name.c_str()); ImGui::NextColumn();
                ImGui::Text("%s", p.account.c_str()); ImGui::NextColumn();
                ImGui::Text("%.2fk", (float)p.boss_dps / 1000.f);
        ImGui::NextColumn(); ImGui::Text("%.2fk", (float)p.dps / 1000.f);
        ImGui::NextColumn(); ImGui::Text("%.1f", p.might_avg);
        ImGui::NextColumn(); ImGui::Text("%.1f%%", p.fury_avg * 100.f);
        ImGui::NextColumn(); ImGui::Text("%.1f%%", p.quickness_avg * 100.f);
        ImGui::NextColumn(); ImGui::Text("%.1f%%", p.alacrity_avg * 100.f);
        ImGui::NextColumn();
        }

        size.y = ImGui::GetCursorPosY();

        ImGui::EndChild();
        */
    }
}

void Uploader::reload_webhooks() {
    std::lock_guard<std::mutex> lk(wh_mutex);
    {
        db_lock dlk(db_mutex);
        webhooks = storage->get_all<Webhook>();
    }
    for (auto& wh : webhooks) {
        safe_copy(wh.name_buf, wh.name);
        safe_copy(wh.url_buf, wh.url);
        safe_copy(wh.filter_buf, wh.filter);
    }
}

void Uploader::check_webhooks(int log_id) {
    std::unique_ptr<Log> log;
    {
        db_lock dlk(db_mutex);
        log = storage->get_pointer<Log>(log_id);
    }
    if (log) {
        if (!log->players) {
            try {
                json players = json::parse(log->players_json);
                log->players = std::make_optional<json>(players);
            } catch (json::parse_error& e) {
                LOG_F(ERROR, "Failed to parse player json for %s: %s",
                      log->filename.c_str(), e.what());
            }
        }

        Revtc::BossCategory category =
            Revtc::Parser::encounterCategory((Revtc::BossID)log->boss_id);
        // Iterate a snapshot: the imgui thread can reassign `webhooks` (Add/
        // Delete) while we're here, which would invalidate a live iterator.
        std::vector<Webhook> whs;
        {
            std::lock_guard<std::mutex> lk(wh_mutex);
            whs = webhooks;
        }
        for (const auto& wh : whs) {
            bool process = true;
            if (!log->success && wh.success) process = false;
            if (category == Revtc::BossCategory::RAIDS && !wh.raids)
                process = false;
            if (category == Revtc::BossCategory::FRACTALS && !wh.fractals)
                process = false;
            if (category == Revtc::BossCategory::STRIKES && !wh.strikes)
                process = false;
            if (category == Revtc::BossCategory::GOLEMS && !wh.golems)
                process = false;
            if (category == Revtc::BossCategory::WVW && !wh.wvw)
                process = false;
            if (category == Revtc::BossCategory::UNKNOWN)
                process = false;

            if (wh.filter.size() > 5) {
                std::vector<std::string> accounts;
                std::string account;
                std::istringstream accountStream(wh.filter);
                while (std::getline(accountStream, account, ',')) {
                    // Trim whitespace. front()/back() on an empty token is UB,
                    // and isspace/tolower require an unsigned-char value (a
                    // negative char from a non-ASCII account name is UB).
                    while (!account.empty() &&
                           std::isspace((unsigned char)account.front())) {
                        account.erase(account.begin());
                    }
                    while (!account.empty() &&
                           std::isspace((unsigned char)account.back())) {
                        account.pop_back();
                    }
                    if (account.empty()) continue;
                    std::transform(
                        account.begin(), account.end(), account.begin(),
                        [](unsigned char c) { return (char)std::tolower(c); });
                    accounts.push_back(account);
                }

                if (log->players && log->players->is_object()) {
                    const auto& players = *log->players;
                    int found = 0;
                    for (auto it = players.begin(); it != players.end(); ++it) {
                        if (!it.value().is_object()) continue;
                        // By value: json::value() returns a temporary; binding
                        // it to a reference would dangle.
                        std::string display_name =
                            it.value().value("display_name", std::string());
                        std::transform(
                            display_name.begin(), display_name.end(),
                            display_name.begin(),
                            [](unsigned char c) { return (char)std::tolower(c); });
                        if (std::find(accounts.begin(), accounts.end(),
                                      display_name) != accounts.end()) {
                            found++;
                        }
                    }
                    int required = wh.filter_min;
                    if (required > (int)accounts.size())
                        required = (int)accounts.size();
                    LOG_F(INFO, "Webhook (%s) - %s - Found/Required: %d/%d",
                          wh.name.c_str(), log->boss_name.c_str(), found,
                          required);
                    if (found < required) {
                        process = false;
                    }
                } else {
                    LOG_F(ERROR, "Players json was not an object.");
                }

                if (process) {
                    LOG_F(INFO, "Executing webhook \"%s\" for %s (%s)",
                          wh.name.c_str(), log->filename.c_str(),
                          log->boss_name.c_str());
                    auto webhook_future = std::async(
                        std::launch::async,
                        [](const Webhook& wh, Log log) {
                            cpr::Response response;
                            response = cpr::Post(
                                cpr::Url{wh.url},
                                cpr::Multipart{
                                    {"content", log.boss_name + " - *" +
                                                    log.human_time + "*" +
                                                    "\n" + log.permalink}});
                        },
                        wh, *log);
                }
            }
        }
    }
}

void Uploader::check_gw2bot(int log_id) {
    if (!settings.gw2bot_enabled) return;

    std::unique_ptr<Log> log;
    {
        db_lock dlk(db_mutex);
        log = storage->get_pointer<Log>(log_id);
    }
    if (log) {
        bool process = true;
        if (!log->success && settings.gw2bot_success_only) process = false;

        std::string gw2bot_key;
        {
            std::lock_guard<std::mutex> ulk(ui_mutex);
            gw2bot_key = settings.gw2bot_key;
        }

        if (process) {
            LOG_F(INFO, "Posting to GW2Bot: %s", log->permalink.c_str());
            auto gw2bot_future = std::async(
                std::launch::async,
                [this](const std::string& key, Log log) {
                    cpr::Response response;
                    response = cpr::Post(
                        cpr::Url{
                            "https://api.gw2bot.info/v1/evtc/notification"},
                        cpr::Header{
                            {"accept", "application/json"},
                            {"Authorization", "Bearer " + std::string(key)},
                            {"Content-Type", "application/json"},
                        },
                        cpr::Body{"{\"dpsreport_url\": \"" + log.permalink +
                                  "\"}"});
                    if (response.status_code != 201) {
                        queue_status_message("GW2Bot Error: " + response.text);
                    }
                    LOG_F(INFO, "GW2Bot response: %s", response.text.c_str());
                },
                gw2bot_key, *log);
        }
    }
}

void Uploader::check_aleeva(int log_id) {
    if (!settings.aleeva.enabled) return;

    std::unique_ptr<Log> log;
    {
        db_lock dlk(db_mutex);
        log = storage->get_pointer<Log>(log_id);
    }
    if (log) {
        bool process = true;
        if (!log->success && settings.gw2bot_success_only) process = false;

        decltype(settings.aleeva) aleeva_snapshot;
        {
            std::lock_guard<std::mutex> ulk(ui_mutex);
            aleeva_snapshot = settings.aleeva;
        }

        if (process) {
            LOG_F(INFO, "Posting to Aleeva: %s", log->permalink.c_str());
            auto aleeva_future = std::async(
                std::launch::async,
                Aleeva::post_log,
                aleeva_snapshot, log->permalink);
        }
    }
}

void Uploader::start_async_refresh_log_list() {
    LOG_F(INFO, "Starting Async Log Refresh");
    using namespace sqlite_orm;
    // Early out if we are already waiting on a refresh
    if (ft_file_list.valid()) return;
    std::error_code ec;
    if (!std::filesystem::exists(log_path, ec) || ec) return;

    ft_file_list = std::async(
        std::launch::async,
        [this](fs::path path) {
            std::vector<Log> file_list;
            // Runs on a worker thread; its result is rethrown by .get() on the
            // imgui thread. Nothing may escape: catch everything and return
            // whatever we have.
            try {
                std::set<std::string> filename_set;
                {
                    db_lock dlk(db_mutex);
                    auto filenames = storage->select(&Log::filename);
                    filename_set.insert(filenames.begin(), filenames.end());
                }

                // 1) Walk the log tree WITHOUT holding the db lock. The
                // error_code iterator skips entries it can't read (permissions,
                // a dir that vanishes mid-scan) instead of throwing.
                std::vector<Log> new_logs;
                std::error_code ec;
                fs::recursive_directory_iterator it(path, ec), end;
                for (; it != end; it.increment(ec)) {
                    if (ec) {
                        ec.clear();
                        continue;
                    }
                    try {
                        const fs::directory_entry& p = *it;
                        std::error_code fec;
                        if (!fs::is_regular_file(p.status(fec)) || fec) continue;
                        std::string extension = p.path().extension().string();
                        if (extension != ".zevtc" && extension != ".evtc")
                            continue;
                        std::string fn = p.path()
                                             .filename()
                                             .replace_extension()
                                             .replace_extension()
                                             .string();
                        if (filename_set.count(fn) != 0) continue;

                        LOG_F(INFO, "Found new log: %s",
                              p.path().string().c_str());
                        Log log;
                        log.id = -1;
                        log.path = p.path();
                        log.filename = fn;

                        // Parse the timestamp from the arcdps filename
                        // (YYYYMMDD-HHMMSS). A non-standard name just yields no
                        // parsed time -- never a crash.
                        std::tm tm = {};
                        if (fn.size() >= 15) {
                            std::string temp = fn;
                            try {
                                temp.insert(4, "-");
                                temp.insert(7, "-");
                                temp.insert(13, "-");
                                temp.insert(16, "-");
                                std::stringstream ss(temp);
                                ss >> std::get_time(&tm,
                                                    "%Y-%m-%d-%H-%M-%S");
                            } catch (...) {
                                tm = {};
                            }
                        }
                        tm.tm_isdst = -1;
                        log.time = std::chrono::system_clock::from_time_t(
                            std::mktime(&tm));

                        char timestr[64];
                        if (std::strftime(timestr, sizeof timestr,
                                          "%I:%M%p (%a %b %d)", &tm) == 0) {
                            timestr[0] = '\0';
                        }
                        log.human_time = std::string(timestr);

                        log.uploaded = false;
                        log.error = false;
                        log.report_id = "";
                        log.permalink = "";
                        log.boss_id = 0;
                        log.json_available = false;
                        log.success = false;

                        new_logs.push_back(std::move(log));
                    } catch (const std::exception& e) {
                        LOG_F(ERROR, "Skipping unreadable log entry: %s",
                              e.what());
                    }
                }

                // 2) Insert the new logs in one transaction, under the lock.
                {
                    db_lock dlk(db_mutex);
                    storage->begin_transaction();
                    for (auto& log : new_logs) {
                        try {
                            log.id = storage->insert(log);
                        } catch (const std::exception& e) {
                            LOG_F(ERROR, "Sqlite insert error: %s", e.what());
                        }
                    }
                    storage->commit();

                    file_list = storage->get_all<Log>(
                        order_by(&Log::time).desc(), limit(kMaxLogs));
                }

                std::vector<int> queue;
                for (auto& log : file_list) {
                    if (!log.uploaded) queue.push_back(log.id);
                }
                add_pending_upload_logs(queue);
            } catch (const std::exception& e) {
                LOG_F(ERROR, "Log refresh failed: %s", e.what());
            } catch (...) {
                LOG_F(ERROR, "Log refresh failed: unknown exception");
            }
            return file_list;
        },
        log_path);
    refresh_time = std::chrono::system_clock::now();
}

void Uploader::poll_async_refresh_log_list() {
    if (ft_file_list.valid()) {
        if (ft_file_list.wait_for(std::chrono::milliseconds(1)) ==
            std::future_status::ready) {
            // The lambda already swallows its own errors, but .get() can still
            // rethrow (e.g. a stored bad_alloc); never let it cross into arcdps.
            try {
                logs = ft_file_list.get();
            } catch (const std::exception& e) {
                LOG_F(ERROR, "Log refresh result failed: %s", e.what());
            }
        }
    }

    auto now = std::chrono::system_clock::now();
    auto diff = now - refresh_time;
    if (diff > std::chrono::minutes(1)) {
        start_async_refresh_log_list();
        refresh_time = now;
    }

    // Upload Thread
    if (!upload_queue.empty()) {
        ut_cv.notify_one();
    }
}

void Uploader::start_upload_thread() {
    LOG_F(INFO, "Starting Upload Thread");
    // Create a thread that spins, waiting for uploads to process
    upload_thread_run = true;
    upload_thread = std::thread(&Uploader::upload_thread_loop, this);
    // Aleeva Authorise
    if (settings.aleeva.enabled) {
        auto future =
            std::async(std::launch::async, [this]() {
                std::string msg =
                    "Aleeva login failed. Please check your access code and try "
                    "to login again.";
                try {
                    if (Aleeva::login(settings)) {
                        msg = "Aleeva login successful.";
                    }
                } catch (...) {
                    // Aleeva::login swallows its own errors; belt-and-suspenders
                }
                // Post via the thread-safe queue, never straight into
                // status_messages (which the imgui thread iterates).
                queue_status_message(msg);
            });
    }
}

void Uploader::add_pending_upload_logs(std::vector<int>& queue) {
    if (queue.empty()) return;
    {
        std::lock_guard<std::mutex> lk(ut_mutex);
        for (int log_id : queue) {
            if (std::find(upload_queue.begin(), upload_queue.end(), log_id) ==
                upload_queue.end()) {
                upload_queue.push_back(log_id);
            }
        }
    }
    ut_cv.notify_one();
}

void Uploader::upload_thread_loop() {
    while (upload_thread_run) {
        std::unique_lock<std::mutex> lk(ut_mutex);
        // Predicate guards against the lost-wakeup race: if shutdown sets the
        // flag and notifies between our flag check and here, the predicate sees
        // it and we don't block forever (which would hang the game on exit).
        ut_cv.wait(lk, [this] {
            return !upload_queue.empty() || !upload_thread_run;
        });
        if (!upload_thread_run) break;

        bool process_log = false;
        int log_id;
        if (!upload_queue.empty()) {
            log_id = upload_queue.front();
            upload_queue.pop_front();
            process_log = true;
        }

        lk.unlock();

        if (process_log) {
            // This runs on a raw std::thread: any exception escaping this
            // body calls std::terminate and crashes the game (0xc0000409).
            // Everything is wrapped, and on failure the log is marked
            // uploaded+error so the auto-queue does not retry (and re-crash)
            // it on every launch.
            try {
                std::string display;
                std::unique_ptr<Log> log;
                {
                    db_lock dlk(db_mutex);
                    log = storage->get_pointer<Log>(log_id);
                }
                if (!log) continue;

                display = log->filename;

                queue_status_message("Uploading " + display + " - " +
                                     log->human_time + ".");

                // Snapshot UI-owned state under lock: a concurrent options
                // edit on the imgui thread could otherwise reallocate these
                // strings mid-read (torn read -> 0xc0000005).
                bool token_disabled;
                std::string token_value;
                bool wvw_detailed;
                {
                    std::lock_guard<std::mutex> ulk(ui_mutex);
                    token_disabled = userToken.disabled;
                    token_value = userToken.value;
                    wvw_detailed = settings.wvw_detailed_enabled;
                }

                cpr::Url url = cpr::Url{"https://dps.report/uploadContent"};
                cpr::Parameters params = cpr::Parameters{};
                cpr::Multipart multi = cpr::Multipart{
                    {"file", cpr::File{log->path.string()}}, {"json", "1"}};

                if (!token_disabled) {
                    params.Add({"userToken", token_value});
                }

                if (wvw_detailed) {
                    params.Add({"detailedwvw", "true"});
                }

                cpr::Response response = cpr::Post(url, params, multi);

                StatusMessage status;
                status.log_id = -1;
                if (response.status_code == 200) {
                    // A 200 does not guarantee a JSON body of the expected
                    // shape (rate-limit/HTML/error payloads happen). The parse
                    // is fully exception-safe and returns false for anything
                    // unusable -- this is the guard against the upload thread
                    // terminating (0xc0000409) on a bad server response.
                    std::string token;
                    bool usable =
                        parse_dpsreport_response(response.text, *log, token);
                    log->uploaded = true;

                    if (!usable) {
                        // 200 but not a usable response -> mark error so we
                        // don't present a dead button or retry (re-crash) it.
                        log->error = true;
                        status.msg = "Upload response was not usable for " +
                                     display + ".";
                    } else {
                        status.msg =
                            "Uploaded " + display + " - " + log->human_time + ".";
                        status.log_id = log->id;
                    }

                    if (!token_disabled && !token.empty()) {
                        std::lock_guard<std::mutex> ulk(ui_mutex);
                        if (userToken.value.empty()) {
                            safe_copy(userToken.value_buf, token);
                            userToken.value = userToken.value_buf;
                            db_lock dlk(db_mutex);
                            storage->update(userToken);
                        } else if (token != userToken.value) {
                            status.msg =
                                "ERROR: Configured userToken did not work. "
                                "Maybe a wrong token was used?";
                        }
                    }
                } else if (response.status_code == 401) {
                    status.msg =
                        "Upload failed. Invalid Username/Password. Please login "
                        "again.";
                } else if (response.status_code == 400) {
                    status.msg =
                        "Upload failed. Invalid File/File Error or Connection "
                        "Error.";
                } else {
                    status.msg = "Upload failed (HTTP " +
                                 std::to_string(response.status_code) + ").";
                    LOG_F(INFO, "Upload failed: %s - %ld, %s",
                          log->filename.c_str(), response.status_code,
                          response.error.message.c_str());
                    log->uploaded = true;
                    log->error = true;
                }

                try {
                    {
                        db_lock dlk(db_mutex);
                        storage->update(*log);
                    }
                    if (log->uploaded && !log->error) {
                        check_webhooks(log->id);
                        check_gw2bot(log->id);
                        check_aleeva(log->id);
                        check_wingman(*log);
                    };
                } catch (const std::exception& e) {
                    LOG_F(ERROR, "Post-upload processing failed: %s", e.what());
                }

                queue_status_message(status);
            } catch (const std::exception& e) {
                LOG_F(ERROR, "Upload processing failed for log %d: %s", log_id,
                      e.what());
                mark_log_errored(log_id);
                queue_status_message(
                    "Upload failed (unexpected server response); skipped.");
            } catch (...) {
                LOG_F(ERROR, "Upload processing failed for log %d (unknown)",
                      log_id);
                mark_log_errored(log_id);
                queue_status_message("Upload failed (unknown error); skipped.");
            }
        }
    }
}

void Uploader::check_wingman(Log& log) {
    std::string account;
    {
        std::lock_guard<std::mutex> ulk(ui_mutex);
        if (!settings.wingman_enabled) return;
        account = settings.wingman_account;
    }
    if (log.boss_id == 1) return;  // Wingman does not accept WvW logs
    std::error_code ec;
    auto size = fs::file_size(log.path, ec);
    if (ec) {
        queue_status_message("Wingman: log file missing for " + log.filename);
        return;
    }
    auto [ok, msg] = Wingman::upload_evtc(log.path.string(), (long long)size,
                                          log.boss_id, account);
    if (ok) {
        // Wingman knows the upload by the evtc's actual file name
        // (with extension), not our extension-less display name.
        log.wingman_link = Wingman::fetch_log_link(
            log.path.filename().string(), (long long)size, log.boss_id, account);
        if (!log.wingman_link.empty()) {
            try {
                db_lock dlk(db_mutex);
                storage->update(log);
            } catch (const std::exception& e) {
                LOG_F(ERROR, "Failed to store wingman link: %s", e.what());
            }
        }
    }
    queue_status_message("Wingman: " + log.boss_name + " " + msg);
}

void Uploader::queue_status_message(const std::string& msg, int log_id) {
    StatusMessage status{msg, log_id};
    queue_status_message(status);
}

void Uploader::queue_status_message(const StatusMessage& msg) {
    std::lock_guard<std::mutex> lk(ts_msg_mutex);
    thread_status_messages.push_back(msg);
}

void Uploader::mark_log_errored(int log_id) {
    try {
        db_lock lk(db_mutex);
        auto lg = storage->get_pointer<Log>(log_id);
        if (lg) {
            lg->uploaded = true;
            lg->error = true;
            storage->update(*lg);
        }
    } catch (...) {
        // best-effort; never throw out of the upload thread's error handler
    }
}

std::string Uploader::format_msg(Log log) {
    std::string f = settings.msg_format; // get format string
    if (f.empty()) {
        f = DEFAULT_MSG_FORMAT;
    }
    std::string msg = "";
    std::string::const_iterator it = f.begin();
    while (it != f.end()) {
        char c = *it++;
        if (c == '\\' && it != f.end() && *it == 'n') {
            c = '\n';
            ++it;
        }
        msg += c;
    }

    msg = std::regex_replace(msg, std::regex("@1"), log.boss_name);
    msg = std::regex_replace(msg, std::regex("@2"), log.permalink);
    return msg;
}