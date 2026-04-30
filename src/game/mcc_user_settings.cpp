#include "mcc_user_settings.h"

#include "../logging/logging.h"
#include "../input/win32_input.h"
#include "../player/player_manager.h"

#include <Windows.h>
#include <ShlObj.h>
#include <KnownFolders.h>
#include <cstring>
#include <fstream>
#include <string>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

static s_mcc_user_settings g_settings;

const s_mcc_user_settings* mcc_user_settings() { return &g_settings; }
s_mcc_user_settings*       mcc_user_settings_mut() { return &g_settings; }

static std::wstring resolve_settings_path() {
	PWSTR path = nullptr;
	if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppDataLow, 0, nullptr, &path)) || !path) {
		if (path) CoTaskMemFree(path);
		return L"";
	}
	std::wstring full = path;
	CoTaskMemFree(path);
	full += L"\\MCC\\Saved\\Config\\WindowsNoEditor\\GameUserSettings.ini";
	return full;
}

static bool starts_with(const std::string& s, const char* prefix) {
	auto n = std::strlen(prefix);
	return s.size() >= n && std::memcmp(s.data(), prefix, n) == 0;
}

// Parse "Name[idx]=value" -> writes to out_idx and out_value when matched.
static bool parse_indexed(const std::string& line, const char* name,
                          int* out_idx, std::string* out_value) {
	auto n = std::strlen(name);
	if (line.size() < n + 4) return false;                  // need [N]=v
	if (std::memcmp(line.data(), name, n) != 0) return false;
	if (line[n] != '[') return false;
	size_t p = n + 1;
	int idx = 0;
	bool any = false;
	while (p < line.size() && line[p] >= '0' && line[p] <= '9') {
		idx = idx * 10 + (line[p] - '0');
		++p;
		any = true;
	}
	if (!any) return false;
	if (p >= line.size() || line[p] != ']') return false;
	++p;
	if (p >= line.size() || line[p] != '=') return false;
	++p;
	*out_idx = idx;
	*out_value = line.substr(p);
	return true;
}

static void store_float(float* arr, int idx, const std::string& v) {
	if (idx < 0 || idx >= libmcc::k_game_count) return;
	arr[idx] = (float)std::atof(v.c_str());
}
static void store_int(int* arr, int idx, const std::string& v) {
	if (idx < 0 || idx >= libmcc::k_game_count) return;
	arr[idx] = std::atoi(v.c_str());
}
static void store_bool(bool* arr, int idx, const std::string& v) {
	if (idx < 0 || idx >= libmcc::k_game_count) return;
	// MCC writes 0/1 (sometimes "True"/"False", or floats like "0.000000" for
	// keys that are actually bool on the profile — handle each).
	if (v.empty()) { arr[idx] = false; return; }
	if (v[0] == 'T' || v[0] == 't') { arr[idx] = true;  return; }
	if (v[0] == 'F' || v[0] == 'f') { arr[idx] = false; return; }
	arr[idx] = (std::atof(v.c_str()) != 0.0);
}

// Parse a single CustomKeyboardMouseMappingV2[N]=(...) line. The inner shape is
// an unreal-style nested struct; we find each "GameKeyboardMouseMappings[i]=(...)"
// chunk, then pull AbstractButton + VirtualKeyCodes[0..4] out of it.
static void parse_kbm_mapping(const std::string& body,
                              s_mcc_user_settings::s_kbm_entry out[66]) {
	// Reset to "unset" so unparsed slots fall back to built-in defaults.
	for (int i = 0; i < 66; ++i) out[i] = s_mcc_user_settings::s_kbm_entry{};

	const char* tag = "GameKeyboardMouseMappings[";
	size_t pos = 0;
	while (true) {
		size_t hit = body.find(tag, pos);
		if (hit == std::string::npos) break;
		size_t i_begin = hit + std::strlen(tag);
		size_t i_end = body.find(']', i_begin);
		if (i_end == std::string::npos) break;

		int slot = std::atoi(body.c_str() + i_begin);

		// Walk to the matching '(' that opens this entry's body and find the
		// closing ')' (single-level — the entry itself has no nested parens).
		size_t open = body.find('(', i_end);
		if (open == std::string::npos) break;
		size_t close = body.find(')', open + 1);
		if (close == std::string::npos) break;

		std::string entry = body.substr(open + 1, close - open - 1);

		s_mcc_user_settings::s_kbm_entry e{};
		auto find_int = [&](const char* key, int* out_val) -> bool {
			size_t p = entry.find(key);
			if (p == std::string::npos) return false;
			p += std::strlen(key);
			if (p >= entry.size() || entry[p] != '=') return false;
			++p;
			*out_val = std::atoi(entry.c_str() + p);
			return true;
		};

		find_int("AbstractButton", &e.abstract_button);
		for (int k = 0; k < 5; ++k) {
			char key[32];
			std::snprintf(key, sizeof(key), "VirtualKeyCodes[%d]", k);
			find_int(key, &e.virtual_key_codes[k]);
		}

		if (slot >= 0 && slot < 66) out[slot] = e;
		pos = close + 1;
	}
}

bool load_mcc_user_settings(s_mcc_user_settings* out) {
	*out = s_mcc_user_settings{};

	auto path = resolve_settings_path();
	if (path.empty()) {
		CONSOLE_LOG_WARN("MCC GameUserSettings.ini: failed to resolve LocalAppDataLow");
		return false;
	}

	std::ifstream in(path);
	if (!in) {
		CONSOLE_LOG_WARN("MCC GameUserSettings.ini: not found at %ls", path.c_str());
		return false;
	}

	std::string line;
	bool in_section = false;
	while (std::getline(in, line)) {
		// Strip CR + leading/trailing whitespace
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
			line.pop_back();
		size_t lead = 0;
		while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
		if (lead) line.erase(0, lead);
		if (line.empty() || line[0] == ';') continue;

		if (line[0] == '[') {
			in_section = (line == "[/Script/MCC.MCCGameUserSettings]");
			continue;
		}
		if (!in_section) continue;

		int idx = 0;
		std::string val;
		if      (parse_indexed(line, "MouseSensitivity",            &idx, &val)) store_float(out->mouse_sensitivity,     idx, val);
		else if (parse_indexed(line, "MouseLookControlsInverted",   &idx, &val)) store_bool (out->mouse_look_inverted,   idx, val);
		else if (parse_indexed(line, "MouseSmoothing",              &idx, &val)) store_bool (out->mouse_smoothing,       idx, val);
		else if (parse_indexed(line, "MouseAcceleration",           &idx, &val)) store_bool (out->mouse_acceleration,    idx, val);
		else if (parse_indexed(line, "MouseAccelerationMinRate",    &idx, &val)) store_float(out->mouse_accel_min_rate,  idx, val);
		else if (parse_indexed(line, "MouseAccelerationMaxAccel",   &idx, &val)) store_float(out->mouse_accel_max_accel, idx, val);
		else if (parse_indexed(line, "MouseAccelerationScale",      &idx, &val)) store_float(out->mouse_accel_scale,     idx, val);
		else if (parse_indexed(line, "MouseAccelerationExp",        &idx, &val)) store_float(out->mouse_accel_exp,       idx, val);
		else if (parse_indexed(line, "FOVSetting",                  &idx, &val)) store_int  (out->fov,                  idx, val);
		else if (parse_indexed(line, "VehicleFOVSetting",           &idx, &val)) store_int  (out->vehicle_fov,          idx, val);
		else if (parse_indexed(line, "MouseAircraftControlsInverted",   &idx, &val)) store_bool(out->mouse_aircraft_inverted,    idx, val);
		else if (parse_indexed(line, "LookControlsInverted",        &idx, &val)) store_bool (out->gamepad_look_inverted,        idx, val);
		else if (parse_indexed(line, "GamepadAircraftControlsInverted", &idx, &val)) store_bool(out->gamepad_aircraft_inverted,  idx, val);
		else if (parse_indexed(line, "VibrationDisabled",           &idx, &val)) store_bool (out->vibration_disabled,           idx, val);
		else if (parse_indexed(line, "ImpulseTriggersDisabled",     &idx, &val)) store_bool (out->impulse_triggers_disabled,    idx, val);
		else if (parse_indexed(line, "AutoCenterEnabled",           &idx, &val)) store_bool (out->auto_center_enabled,          idx, val);
		else if (parse_indexed(line, "CrouchLockEnabled",           &idx, &val)) store_bool (out->crouch_lock_enabled,          idx, val);
		else if (parse_indexed(line, "MKCrouchLockEnabled",         &idx, &val)) store_bool (out->mk_crouch_lock_enabled,       idx, val);
		else if (parse_indexed(line, "ClenchProtectionEnabled",     &idx, &val)) store_bool (out->clench_protection_enabled,    idx, val);
		else if (parse_indexed(line, "UseFemaleVoice",              &idx, &val)) store_bool (out->use_female_voice,             idx, val);
		else if (parse_indexed(line, "HoldToZoom",                  &idx, &val)) store_bool (out->hold_to_zoom,                 idx, val);
		else if (parse_indexed(line, "UseEliteModel",               &idx, &val)) store_bool (out->use_elite_model,              idx, val);
		else if (parse_indexed(line, "UseModernAimControl",         &idx, &val)) store_bool (out->use_modern_aim_control,       idx, val);
		else if (parse_indexed(line, "KeyboardMouseButtonPreset",   &idx, &val)) store_int  (out->keyboard_mouse_button_preset, idx, val);
		else if (parse_indexed(line, "ButtonPreset",                &idx, &val)) store_int  (out->button_preset,                idx, val);
		else if (parse_indexed(line, "StickPreset",                 &idx, &val)) store_int  (out->stick_preset,                 idx, val);
		else if (parse_indexed(line, "LeftyToggle",                 &idx, &val)) store_bool (out->lefty_toggle,                 idx, val);
		else if (parse_indexed(line, "SwapTriggersAndBumpers",      &idx, &val)) store_bool (out->swap_triggers_and_bumpers,    idx, val);
		else if (parse_indexed(line, "VerticalLookSensitivity",     &idx, &val)) store_int  (out->vertical_look_sensitivity,    idx, val);
		else if (parse_indexed(line, "HorizontalLookSensitivity",   &idx, &val)) store_int  (out->horizontal_look_sensitivity,  idx, val);
		else if (parse_indexed(line, "LookAcceleration",            &idx, &val)) store_bool (out->look_acceleration,             idx, val);
		else if (parse_indexed(line, "LookAxialDeadZone",           &idx, &val)) store_float(out->look_axial_dead_zone,          idx, val);
		else if (parse_indexed(line, "LookRadialDeadZone",          &idx, &val)) store_float(out->look_radial_dead_zone,         idx, val);
		else if (parse_indexed(line, "ZoomLookSensitivityMultiplier",    &idx, &val)) store_float(out->zoom_look_sens_mult,    idx, val);
		else if (parse_indexed(line, "VehicleLookSensitivityMultiplier", &idx, &val)) store_float(out->vehicle_look_sens_mult, idx, val);
		else if (parse_indexed(line, "UseDoublePressJumpToJetpack",      &idx, &val)) store_bool(out->use_double_press_jump_to_jetpack, idx, val);
		else if (parse_indexed(line, "DualWieldInverted",                &idx, &val)) store_bool(out->dual_wield_inverted,                idx, val);
		else if (parse_indexed(line, "ControllerDualWieldInverted",      &idx, &val)) store_bool(out->controller_dual_wield_inverted,    idx, val);
		else if (parse_indexed(line, "ControllerHornetControlJoystick",  &idx, &val)) store_bool(out->controller_hornet_control_joystick, idx, val);
		else if (parse_indexed(line, "ControllerBansheeTrickButtonsSwapped", &idx, &val)) store_bool(out->controller_banshee_trick_buttons_swapped, idx, val);
		else if (parse_indexed(line, "FlyingCameraTurnSensitivity", &idx, &val)) store_int(out->flying_camera_turn_sensitivity, idx, val);
		else if (parse_indexed(line, "FlyingCameraPanning",         &idx, &val)) store_int(out->flying_camera_panning,         idx, val);
		else if (parse_indexed(line, "FlyingCameraSpeed",           &idx, &val)) store_int(out->flying_camera_speed,           idx, val);
		else if (parse_indexed(line, "FlyingCameraThrust",          &idx, &val)) store_int(out->flying_camera_thrust,          idx, val);
		else if (parse_indexed(line, "TheaterTurnSensitivity",      &idx, &val)) store_int(out->theater_turn_sensitivity,      idx, val);
		else if (parse_indexed(line, "TheaterPanning",              &idx, &val)) store_int(out->theater_panning,               idx, val);
		else if (parse_indexed(line, "TheaterSpeed",                &idx, &val)) store_int(out->theater_speed,                 idx, val);
		else if (parse_indexed(line, "TheaterThrust",               &idx, &val)) store_int(out->theater_thrust,                idx, val);
		else if (parse_indexed(line, "MKTheaterTurnSensitivity",    &idx, &val)) store_int(out->mk_theater_turn_sensitivity,   idx, val);
		else if (parse_indexed(line, "MKTheaterPanning",            &idx, &val)) store_int(out->mk_theater_panning,            idx, val);
		else if (parse_indexed(line, "MKTheaterSpeed",              &idx, &val)) store_int(out->mk_theater_speed,              idx, val);
		else if (parse_indexed(line, "MKTheaterThrust",             &idx, &val)) store_int(out->mk_theater_thrust,             idx, val);
		else if (parse_indexed(line, "CustomKeyboardMouseMappingV2", &idx, &val)) {
			if (idx >= 0 && idx < libmcc::k_game_count) parse_kbm_mapping(val, out->custom_kbm[idx]);
		}
	}

	out->loaded = true;
	CONSOLE_LOG_INFO(
		"MCC GameUserSettings.ini: loaded %ls",
		path.c_str());
	for (int i = 0; i < libmcc::k_game_count; ++i) {
		static const char* names[libmcc::k_game_count] = {
			"halo1", "halo2", "halo3", "halo4", "groundhog", "halo3odst", "haloreach"
		};
		CONSOLE_LOG_INFO(
			"  %-9s sens=%.3f invertY=%d FOV=%d vehicleFOV=%d",
			names[i], out->mouse_sensitivity[i],
			(int)out->mouse_look_inverted[i],
			out->fov[i], out->vehicle_fov[i]);
	}
	return true;
}

void mcc_user_settings_initialize() {
	load_mcc_user_settings(&g_settings);
}

void mcc_user_settings_reload() {
	load_mcc_user_settings(&g_settings);
}

void mcc_user_settings_stamp_profile(libmcc::e_module module, libmcc::s_player_profile* profile) {
	if (!profile) return;
	if (module < 0 || module >= libmcc::k_game_count) return;
	if (!g_settings.loaded) return;

	// FOV: 0 in the ini means "use the game's built-in default" — do NOT
	// stamp it on the profile or the projection matrix becomes degenerate
	// (which is what was breaking vehicle camera).
	if (g_settings.fov[module] > 0)
		profile->fov_setting = g_settings.fov[module];
	if (g_settings.vehicle_fov[module] > 0)
		profile->vehicle_fov_setting = g_settings.vehicle_fov[module];

	// Look inversion — separate gamepad/mouse channels.
	profile->look_controls_inverted           = g_settings.gamepad_look_inverted[module];
	profile->mouse_look_controls_inverted     = g_settings.mouse_look_inverted[module];
	profile->aircraft_controls_inverted       = g_settings.gamepad_aircraft_inverted[module];
	profile->mouse_aircraft_controls_inverted = g_settings.mouse_aircraft_inverted[module];

	// Customization toggles
	profile->vibration_disabled        = g_settings.vibration_disabled[module];
	profile->impulse_triggers_disabled = g_settings.impulse_triggers_disabled[module];
	profile->auto_center_enabled       = g_settings.auto_center_enabled[module];
	profile->crouch_lock_enabled       = g_settings.crouch_lock_enabled[module];
	profile->mk_crouch_lock_enabled    = g_settings.mk_crouch_lock_enabled[module];
	profile->clench_protection_enabled = g_settings.clench_protection_enabled[module];
	profile->use_female_voice          = g_settings.use_female_voice[module];
	profile->hold_to_zoom              = g_settings.hold_to_zoom[module];
	profile->use_elite_model           = g_settings.use_elite_model[module];
	profile->use_modern_aim_control    = g_settings.use_modern_aim_control[module];

	// Mirror mouse sensitivity onto the profile too. The raw-input pipeline
	// already scales by k_mcc_to_halox_mouse_scale; the game's own per-player
	// mouse sensitivity is a separate slider that vehicle code also reads, so
	// push the raw MCC value through unscaled.
	profile->mouse_sensitivity            = g_settings.mouse_sensitivity[module];
	profile->mouse_smoothing              = g_settings.mouse_smoothing[module];
	profile->mouse_acceleration           = g_settings.mouse_acceleration[module];
	profile->mouse_acceleration_min_rate  = g_settings.mouse_accel_min_rate[module];
	profile->mouse_acceleration_max_accel = g_settings.mouse_accel_max_accel[module];
	profile->mouse_acceleration_scale     = g_settings.mouse_accel_scale[module];
	profile->mouse_acceleration_exp       = g_settings.mouse_accel_exp[module];

	// Controller / look feel
	profile->keyboard_mouse_button_preset = g_settings.keyboard_mouse_button_preset[module];
	// libmcc declares these enum-like fields as bool; the ini holds 0/1/...
	// Cast preserves on/off; pure-int presets beyond 1 are lossy here, but
	// 0 (default) is by far the common case. Stamp the byte directly so a
	// non-zero preset index survives intact.
	*reinterpret_cast<uint8_t*>(&profile->button_preset)              = (uint8_t)g_settings.button_preset[module];
	*reinterpret_cast<uint8_t*>(&profile->stick_preset)               = (uint8_t)g_settings.stick_preset[module];
	profile->lefty_toggle                                             = g_settings.lefty_toggle[module];
	profile->swap_triggers_and_bumpers                                = g_settings.swap_triggers_and_bumpers[module];
	*reinterpret_cast<uint8_t*>(&profile->vertical_look_sensitivity)  = (uint8_t)g_settings.vertical_look_sensitivity[module];
	*reinterpret_cast<uint8_t*>(&profile->horizontal_look_sensitivity) = (uint8_t)g_settings.horizontal_look_sensitivity[module];
	profile->look_acceleration                                        = g_settings.look_acceleration[module];
	profile->look_axial_dead_zone                                     = g_settings.look_axial_dead_zone[module];
	profile->look_radial_dead_zone                                    = g_settings.look_radial_dead_zone[module];
	profile->zoom_look_sensitivity_multiplier                         = g_settings.zoom_look_sens_mult[module];
	profile->vehicle_look_sensitivity_multiplier                      = g_settings.vehicle_look_sens_mult[module];
	profile->use_double_press_jump_to_jetpack                         = g_settings.use_double_press_jump_to_jetpack[module];
	profile->dual_wield_inverted                                      = g_settings.dual_wield_inverted[module];
	profile->controller_dual_wield_inverted                           = g_settings.controller_dual_wield_inverted[module];
	profile->controller_hornet_control_joystick                       = g_settings.controller_hornet_control_joystick[module];
	profile->controller_banshee_trick_buttons_swapped                 = g_settings.controller_banshee_trick_buttons_swapped[module];
	*reinterpret_cast<uint8_t*>(&profile->flying_camera_turn_sensitivity) = (uint8_t)g_settings.flying_camera_turn_sensitivity[module];
	*reinterpret_cast<uint8_t*>(&profile->flying_camera_panning)         = (uint8_t)g_settings.flying_camera_panning[module];
	*reinterpret_cast<uint8_t*>(&profile->flying_camera_speed)           = (uint8_t)g_settings.flying_camera_speed[module];
	*reinterpret_cast<uint8_t*>(&profile->flying_camera_thrust)          = (uint8_t)g_settings.flying_camera_thrust[module];
	*reinterpret_cast<uint8_t*>(&profile->theater_turn_sensitivity)      = (uint8_t)g_settings.theater_turn_sensitivity[module];
	*reinterpret_cast<uint8_t*>(&profile->theater_panning)               = (uint8_t)g_settings.theater_panning[module];
	*reinterpret_cast<uint8_t*>(&profile->theater_speed)                 = (uint8_t)g_settings.theater_speed[module];
	*reinterpret_cast<uint8_t*>(&profile->theater_thrust)                = (uint8_t)g_settings.theater_thrust[module];
	*reinterpret_cast<uint8_t*>(&profile->mk_theater_turn_sensitivity)   = (uint8_t)g_settings.mk_theater_turn_sensitivity[module];
	*reinterpret_cast<uint8_t*>(&profile->mk_theater_panning)            = (uint8_t)g_settings.mk_theater_panning[module];
	*reinterpret_cast<uint8_t*>(&profile->mk_theater_speed)              = (uint8_t)g_settings.mk_theater_speed[module];
	*reinterpret_cast<uint8_t*>(&profile->mk_theater_thrust)             = (uint8_t)g_settings.mk_theater_thrust[module];

	// Custom KB/M binding table — only stamp slots that the ini actually
	// supplied (abstract_button >= 0); leave the rest at the game's
	// built-in default so missing entries don't wipe usable bindings.
	for (int i = 0; i < 66; ++i) {
		const auto& src = g_settings.custom_kbm[module][i];
		if (src.abstract_button < 0) continue;
		profile->custom_keyboard_mouse_mapping_v2[i].abstract_button =
			(libmcc::e_game_abstract_button)src.abstract_button;
		for (int k = 0; k < 5; ++k) {
			profile->custom_keyboard_mouse_mapping_v2[i].virtual_key_codes[k] = src.virtual_key_codes[k];
		}
	}
}

void apply_mcc_settings_for_module(libmcc::e_module module) {
	if (module < 0 || module >= libmcc::k_game_count) return;
	if (!g_settings.loaded) return;

	// Mouse sensitivity & invert-Y go to win32_input (our raw-input pipeline).
	const float sens = g_settings.mouse_sensitivity[module] * k_mcc_to_halox_mouse_scale;
	win32_input_set_mouse_sensitivity(sens);
	win32_input_set_invert_y(g_settings.mouse_look_inverted[module]);

	// FOV + customization fields go into the per-game player profile that the
	// c_game_manager hands back when halo3/etc. calls get_player_profile().
	// All four local players share the same profile since this is a
	// single-window launcher.
	libmcc::s_player_profile profile{};
	for (int p = 0; p < libmcc::k_local_player_count; ++p) {
		auto local = (libmcc::e_local_player)p;
		player_manager()->get_player_profile(local, module, &profile);
		mcc_user_settings_stamp_profile(module, &profile);
		player_manager()->set_player_profile(local, module, &profile);
	}

	CONSOLE_LOG_INFO(
		"Applied MCC settings for module=%d: sens=%.3f (raw %.3f * %.4f), invertY=%d, FOV=%d, vehicleFOV=%d, autoCenter=%d, mouseAircraftInv=%d",
		(int)module, sens,
		g_settings.mouse_sensitivity[module], k_mcc_to_halox_mouse_scale,
		(int)g_settings.mouse_look_inverted[module],
		g_settings.fov[module], g_settings.vehicle_fov[module],
		(int)g_settings.auto_center_enabled[module],
		(int)g_settings.mouse_aircraft_inverted[module]);
}

void mcc_user_settings_reload_module(libmcc::e_module module) {
	if (module < 0 || module >= libmcc::k_game_count) return;
	// Re-parse the entire ini into a scratch copy and copy the fields for the
	// requested module index back over our singleton. This keeps other games'
	// in-memory edits intact while letting the user "Reset this game" cleanly.
	s_mcc_user_settings tmp{};
	if (!load_mcc_user_settings(&tmp)) return;

	const int m = (int)module;
	g_settings.mouse_sensitivity[m]                  = tmp.mouse_sensitivity[m];
	g_settings.mouse_look_inverted[m]                = tmp.mouse_look_inverted[m];
	g_settings.mouse_smoothing[m]                    = tmp.mouse_smoothing[m];
	g_settings.mouse_acceleration[m]                 = tmp.mouse_acceleration[m];
	g_settings.mouse_accel_min_rate[m]               = tmp.mouse_accel_min_rate[m];
	g_settings.mouse_accel_max_accel[m]              = tmp.mouse_accel_max_accel[m];
	g_settings.mouse_accel_scale[m]                  = tmp.mouse_accel_scale[m];
	g_settings.mouse_accel_exp[m]                    = tmp.mouse_accel_exp[m];
	g_settings.mouse_aircraft_inverted[m]            = tmp.mouse_aircraft_inverted[m];
	g_settings.gamepad_look_inverted[m]              = tmp.gamepad_look_inverted[m];
	g_settings.gamepad_aircraft_inverted[m]          = tmp.gamepad_aircraft_inverted[m];
	g_settings.fov[m]                                = tmp.fov[m];
	g_settings.vehicle_fov[m]                        = tmp.vehicle_fov[m];
	g_settings.vibration_disabled[m]                 = tmp.vibration_disabled[m];
	g_settings.impulse_triggers_disabled[m]          = tmp.impulse_triggers_disabled[m];
	g_settings.auto_center_enabled[m]                = tmp.auto_center_enabled[m];
	g_settings.crouch_lock_enabled[m]                = tmp.crouch_lock_enabled[m];
	g_settings.mk_crouch_lock_enabled[m]             = tmp.mk_crouch_lock_enabled[m];
	g_settings.clench_protection_enabled[m]          = tmp.clench_protection_enabled[m];
	g_settings.use_female_voice[m]                   = tmp.use_female_voice[m];
	g_settings.hold_to_zoom[m]                       = tmp.hold_to_zoom[m];
	g_settings.use_elite_model[m]                    = tmp.use_elite_model[m];
	g_settings.use_modern_aim_control[m]             = tmp.use_modern_aim_control[m];
	g_settings.keyboard_mouse_button_preset[m]       = tmp.keyboard_mouse_button_preset[m];
	g_settings.button_preset[m]                      = tmp.button_preset[m];
	g_settings.stick_preset[m]                       = tmp.stick_preset[m];
	g_settings.lefty_toggle[m]                       = tmp.lefty_toggle[m];
	g_settings.swap_triggers_and_bumpers[m]          = tmp.swap_triggers_and_bumpers[m];
	g_settings.vertical_look_sensitivity[m]          = tmp.vertical_look_sensitivity[m];
	g_settings.horizontal_look_sensitivity[m]        = tmp.horizontal_look_sensitivity[m];
	g_settings.look_acceleration[m]                  = tmp.look_acceleration[m];
	g_settings.look_axial_dead_zone[m]               = tmp.look_axial_dead_zone[m];
	g_settings.look_radial_dead_zone[m]              = tmp.look_radial_dead_zone[m];
	g_settings.zoom_look_sens_mult[m]                = tmp.zoom_look_sens_mult[m];
	g_settings.vehicle_look_sens_mult[m]             = tmp.vehicle_look_sens_mult[m];
	g_settings.use_double_press_jump_to_jetpack[m]   = tmp.use_double_press_jump_to_jetpack[m];
	g_settings.dual_wield_inverted[m]                = tmp.dual_wield_inverted[m];
	g_settings.controller_dual_wield_inverted[m]     = tmp.controller_dual_wield_inverted[m];
	g_settings.controller_hornet_control_joystick[m] = tmp.controller_hornet_control_joystick[m];
	g_settings.controller_banshee_trick_buttons_swapped[m] = tmp.controller_banshee_trick_buttons_swapped[m];
	g_settings.flying_camera_turn_sensitivity[m]     = tmp.flying_camera_turn_sensitivity[m];
	g_settings.flying_camera_panning[m]              = tmp.flying_camera_panning[m];
	g_settings.flying_camera_speed[m]                = tmp.flying_camera_speed[m];
	g_settings.flying_camera_thrust[m]               = tmp.flying_camera_thrust[m];
	g_settings.theater_turn_sensitivity[m]           = tmp.theater_turn_sensitivity[m];
	g_settings.theater_panning[m]                    = tmp.theater_panning[m];
	g_settings.theater_speed[m]                      = tmp.theater_speed[m];
	g_settings.theater_thrust[m]                     = tmp.theater_thrust[m];
	g_settings.mk_theater_turn_sensitivity[m]        = tmp.mk_theater_turn_sensitivity[m];
	g_settings.mk_theater_panning[m]                 = tmp.mk_theater_panning[m];
	g_settings.mk_theater_speed[m]                   = tmp.mk_theater_speed[m];
	g_settings.mk_theater_thrust[m]                  = tmp.mk_theater_thrust[m];
	for (int i = 0; i < 66; ++i) g_settings.custom_kbm[m][i] = tmp.custom_kbm[m][i];
}
