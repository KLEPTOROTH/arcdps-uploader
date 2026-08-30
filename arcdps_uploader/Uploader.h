#pragma once

#include "arcdps_defs.h"
#include "SimpleIni.h"
#include <cpr/cpr.h>
#include <filesystem>
#include <future>
#include <deque>
#include "Revtc.h"
#include "sqlite_orm.h"
#include "Log.h"
#include "Settings.h"

namespace fs = std::filesystem;

struct StatusMessage
{
	std::string msg;
	int log_id;
};

struct UserToken
{
	int id;
	std::string value;
	bool disabled;
	char value_buf[128];
};

struct Webhook
{
	int id;
	std::string name;
	std::string url;
	bool raids;
	bool fractals;
	bool strikes;
	bool golems;
	bool wvw;
	std::string filter;
	int filter_min;
	bool success;

	char name_buf[64];
	char url_buf[192];
	char filter_buf[256];
};

class Uploader
{
	fs::path log_path;
	std::vector<Log> logs;
	std::future<decltype(logs)> ft_file_list;
	std::chrono::system_clock::time_point refresh_time;

	std::deque<int> upload_queue;
	std::vector<std::future<cpr::Response>> ft_uploads;
	std::vector<UserToken> userTokens;
	UserToken userToken;
	std::vector<Webhook> webhooks;
	std::mutex wh_mutex;
	std::deque<int> wh_queue;

	std::vector<StatusMessage> status_messages;
	std::mutex ts_msg_mutex;
	std::vector<StatusMessage> thread_status_messages;

	// Guards UI-owned state (userToken and the settings strings the worker
	// threads read) against concurrent edits on the imgui thread.
	std::mutex ui_mutex;

	std::thread upload_thread;
	std::atomic<bool> upload_thread_run;
	std::mutex ut_mutex;
	std::condition_variable ut_cv;

	bool update_notice_dismissed = false;

	void imgui_draw_logs();
	void imgui_draw_status();
	void imgui_draw_options();
	void imgui_draw_options_aleeva();
	void imgui_draw_update_notice();
	void create_log_table(Log& l);

	// Reloads the webhook list from the db and refreshes the imgui edit
	// buffers, under wh_mutex so the upload thread never sees a half-swapped
	// vector.
	void reload_webhooks();

	void check_webhooks(int log_id);
	void check_gw2bot(int log_id);
	void check_aleeva(int log_id);
	void check_wingman(Log& log);

	void upload_thread_loop();
	void add_pending_upload_logs(std::vector<int>& queue);
	void poll_async_refresh_log_list();

	void queue_status_message(const std::string& msg, int log_id = -1);
	void queue_status_message(const StatusMessage& status);

	// Best-effort: mark a log uploaded+error in the db so the auto-upload
	// queue stops retrying (and re-crashing on) it. Never throws.
	void mark_log_errored(int log_id);

	std::string format_msg(Log log);
public:
	Settings settings;

	bool is_open;
	bool in_combat;

	Uploader(fs::path data_path, std::optional<fs::path> custom_log_path);
	~Uploader();

	uintptr_t imgui_tick(uint32_t not_charsel_or_loading);
	void imgui_window_checkbox();

	// Surface a status message in the uploader window. Also used by the smoke
	// test to drive the status draw path with hostile input (the status text
	// must never be treated as a printf format string).
	void push_status_message(const std::string& msg) { queue_status_message(msg); }
	
	void start_async_refresh_log_list();

	void start_upload_thread();
};

