#pragma once

#include "game_options.h"
#include "game_manager.h"
#include "game_event_manager.h"

typedef libmcc::s_flags<int, libmcc::e_module> s_module_flags;

struct s_game_prop {
	libmcc::s_scenario_map_id map = libmcc::k_map_id_none;
	libmcc::e_module module = libmcc::k_module_none;
	libmcc::e_game_mode mode = libmcc::_game_mode_campaign;
	libmcc::e_campaign_difficulty_level difficulty = libmcc::_campaign_difficulty_level_normal;

	int hopper_game_variant = -1;
	int hopper_map_variant = -1;
	int film = -1;

	int game_tick = 60;
};

constexpr const char* k_game_names[]{
	"halo1",
	"halo2",
	"halo3",
	"halo4",
	"groundhog",
	"halo3odst",
	"haloreach"
};

struct s_game_local {
	// Absolute directory the variants below were enumerated from. Used by
	// get_saved_game_file to load the right .bin / .mvar — populated by
	// load_local. Empty == cwd-relative (legacy junction setup).
	std::wstring             game_variant_dir;
	std::wstring             map_variant_dir;
	std::vector<std::string> hopper_game_variants;
	std::vector<std::string> hopper_map_variants;
	std::vector<std::string> films;
};

namespace libmcc {
	class i_unknown_deleter {
	public:
		void operator()(i_unknown* ptr) {
			if (ptr) {
				ptr->free();
			}
		}
	};

	template<typename T>
	using i_unknown_ptr = std::unique_ptr<T, i_unknown_deleter>;
}

class c_game_instance_manager {

public:
	int initialize();
	int shutdown();

	LRESULT process_message(
		HWND hWnd, 
		UINT uMsg, 
		WPARAM wParam, 
		LPARAM lParam);

	int post_message(
		libmcc::e_game_message message,
		const libmcc::s_game_message_parameter* parameter = nullptr);

	// Tear down the pause overlay AND resume the game engine. Used by every
	// pause-menu button that closes the overlay — RESUME, REVERT, RESTART
	// all need this. Without it the engine stays paused (since ESC is no
	// longer the only path out) and queued actions like revert/restart
	// never get processed by the per-tick pump.
	void dismiss_pause_overlay();

	int launch_game(const s_game_prop* prop);

	// Worker-thread entry point. Called only by halox::ui::ui_launch_run_internal
	// (the trampoline declared in game_instance_manager.win32.cpp). Drives the
	// existing launch_game_internal() — exposed publicly only so the ui module
	// doesn't need to befriend the manager.
	int launch_game_internal_for_worker() { return launch_game_internal(); }

	s_module_flags load_modules(s_module_flags);
	s_module_flags unload_modules(s_module_flags);
	s_module_flags get_module_states();
	libmcc::i_data_access* get_module_data_access(libmcc::e_module);

	int load_local();

	const s_game_local* get_game_locals(libmcc::e_module game) {
		return s_game_locals + game;
	}

	libmcc::i_unknown_ptr<libmcc::i_unknown> get_saved_game_file(
		libmcc::e_module game,
		int game_file_type,
		int game_file_index
	);

	libmcc::i_unknown_ptr<libmcc::i_game_engine_variant> get_game_variant(
		libmcc::e_module game,
		int hopper_game_variant
	);

	libmcc::i_unknown_ptr<libmcc::i_scenario_map_variant> get_map_variant(
		libmcc::e_module game,
		int hopper_map_variant
	);

	bool in_game() {
		return m_game_thread != nullptr;
	}

	libmcc::e_module get_game() {
		return m_game;
	}

	libmcc::e_game_mode get_mode() {
		return m_game_options_storage.game_options.game_mode;
	}

	static c_game_instance_manager g_game_instance_manager;

private:
	int initialize_module();
	int shutdown_module();

	int launch_game_internal();

private:
	HANDLE m_game_thread;
	DWORD m_game_thread_exit_code;
	bool m_game_paused;
	libmcc::e_module m_game;
	libmcc::i_game_engine* m_game_engine;
	s_game_options_storage m_game_options_storage;

	libmcc::e_module m_running_game = libmcc::k_module_none;

	s_game_local s_game_locals[libmcc::k_game_count];

	libmcc::s_module_info m_game_module_infos[libmcc::k_game_count];
	s_module_flags m_game_module_status;
};

inline c_game_instance_manager* game_instance_manager() {
	return &c_game_instance_manager::g_game_instance_manager;
}
