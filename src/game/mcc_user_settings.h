#pragma once

#include <libmcc/libmcc.h>

// Parsed subset of MCC's GameUserSettings.ini that HaloX cares about.
// Indexed by libmcc::e_module (0..k_game_count-1) — same ordering MCC uses.
struct s_mcc_user_settings {
	bool   loaded = false;

	// Mouse / aim
	float  mouse_sensitivity[libmcc::k_game_count]      = { 1.6f, 1.6f, 1.6f, 1.6f, 1.6f, 1.6f, 1.6f };
	bool   mouse_look_inverted[libmcc::k_game_count]    = {};
	bool   mouse_smoothing[libmcc::k_game_count]        = {};
	bool   mouse_acceleration[libmcc::k_game_count]     = {};
	float  mouse_accel_min_rate[libmcc::k_game_count]   = {};
	float  mouse_accel_max_accel[libmcc::k_game_count]  = {};
	float  mouse_accel_scale[libmcc::k_game_count]      = {};
	float  mouse_accel_exp[libmcc::k_game_count]        = {};
	bool   mouse_aircraft_inverted[libmcc::k_game_count] = {};
	bool   gamepad_look_inverted[libmcc::k_game_count]   = {};
	bool   gamepad_aircraft_inverted[libmcc::k_game_count] = {};

	// Field of view (0 in MCC means "use the game's built-in default")
	int    fov[libmcc::k_game_count]         = {};
	int    vehicle_fov[libmcc::k_game_count] = {};

	// Customization-section toggles (per game, indexed by libmcc::e_module)
	bool   vibration_disabled[libmcc::k_game_count]      = {};
	bool   impulse_triggers_disabled[libmcc::k_game_count] = {};
	bool   auto_center_enabled[libmcc::k_game_count]     = {};
	bool   crouch_lock_enabled[libmcc::k_game_count]     = {};
	bool   mk_crouch_lock_enabled[libmcc::k_game_count]  = {};
	bool   clench_protection_enabled[libmcc::k_game_count] = {};
	bool   use_female_voice[libmcc::k_game_count]        = {};
	bool   hold_to_zoom[libmcc::k_game_count]            = {};
	bool   use_elite_model[libmcc::k_game_count]         = {};
	bool   use_modern_aim_control[libmcc::k_game_count]  = {};

	// Controller layout / look feel
	int    keyboard_mouse_button_preset[libmcc::k_game_count] = {};
	int    button_preset[libmcc::k_game_count]                = {};
	int    stick_preset[libmcc::k_game_count]                 = {};
	bool   lefty_toggle[libmcc::k_game_count]                 = {};
	bool   swap_triggers_and_bumpers[libmcc::k_game_count]    = {};
	int    vertical_look_sensitivity[libmcc::k_game_count]    = {};
	int    horizontal_look_sensitivity[libmcc::k_game_count]  = {};
	bool   look_acceleration[libmcc::k_game_count]            = {};
	float  look_axial_dead_zone[libmcc::k_game_count]         = {};
	float  look_radial_dead_zone[libmcc::k_game_count]        = {};
	float  zoom_look_sens_mult[libmcc::k_game_count]          = {};
	float  vehicle_look_sens_mult[libmcc::k_game_count]       = {};
	bool   use_double_press_jump_to_jetpack[libmcc::k_game_count] = {};
	bool   dual_wield_inverted[libmcc::k_game_count]              = {};
	bool   controller_dual_wield_inverted[libmcc::k_game_count]   = {};
	bool   controller_hornet_control_joystick[libmcc::k_game_count] = {};
	bool   controller_banshee_trick_buttons_swapped[libmcc::k_game_count] = {};
	int    flying_camera_turn_sensitivity[libmcc::k_game_count] = {};
	int    flying_camera_panning[libmcc::k_game_count]          = {};
	int    flying_camera_speed[libmcc::k_game_count]            = {};
	int    flying_camera_thrust[libmcc::k_game_count]           = {};
	int    theater_turn_sensitivity[libmcc::k_game_count]    = {};
	int    theater_panning[libmcc::k_game_count]             = {};
	int    theater_speed[libmcc::k_game_count]               = {};
	int    theater_thrust[libmcc::k_game_count]              = {};
	int    mk_theater_turn_sensitivity[libmcc::k_game_count] = {};
	int    mk_theater_panning[libmcc::k_game_count]          = {};
	int    mk_theater_speed[libmcc::k_game_count]            = {};
	int    mk_theater_thrust[libmcc::k_game_count]           = {};

	// Per-game custom keyboard/mouse mapping table. The runtime profile holds
	// 66 entries indexed by libmcc::e_game_abstract_button, each with up to 5
	// alternative virtual-key codes; the ini stores the same shape.
	struct s_kbm_entry {
		int abstract_button = -1;            // -1 = unset (use built-in default)
		int virtual_key_codes[5] = {};
	};
	s_kbm_entry custom_kbm[libmcc::k_game_count][66] = {};
};

// Loads %USERPROFILE%\AppData\LocalLow\MCC\Saved\Config\WindowsNoEditor\GameUserSettings.ini.
// Returns false if the file is missing or unreadable; an empty default-init
// struct is left in *out so callers can use baselines.
bool load_mcc_user_settings(s_mcc_user_settings* out);

// Singleton accessor — populated once at startup.
const s_mcc_user_settings* mcc_user_settings();
// Mutable accessor — for the imgui input panel that lets the user live-edit
// the parsed settings. Edits propagate immediately because every
// c_game_manager::get_player_profile call re-stamps the profile from this
// singleton (see mcc_user_settings_stamp_profile).
s_mcc_user_settings* mcc_user_settings_mut();
void mcc_user_settings_initialize();
void mcc_user_settings_reload();
// Re-parse the ini for a single game only — used by "Reset this game to MCC
// defaults" so a per-game reset doesn't wipe other games' edits.
void mcc_user_settings_reload_module(libmcc::e_module module);

// MCC's MouseSensitivity[N] is on a scale where ~1.6 is the in-game default.
// Our raw-input pipeline expects a multiplier near 0.10 to feel similar, so
// translate by this constant when applying to win32_input.
constexpr float k_mcc_to_halox_mouse_scale = 0.0625f;  // 0.10 / 1.6

// Apply the parsed settings for the given game to the live win32 input state
// (sensitivity, invert-Y). Called by the launcher when the title selection
// changes and again right before launch_game.
void apply_mcc_settings_for_module(libmcc::e_module module);

// Overlay the parsed MCC settings for `module` onto an in-memory profile,
// without going through player_manager. Used by both apply_mcc_settings_for_module
// (which loops local players + writes back via set_player_profile) and by
// c_game_manager::get_player_profile (which calls this on every fetch so live
// imgui edits to the singleton propagate to the game on the next frame).
//
// Safe to call when settings have not been loaded — it will short-circuit and
// leave *profile untouched.
void mcc_user_settings_stamp_profile(libmcc::e_module module, libmcc::s_player_profile* profile);
