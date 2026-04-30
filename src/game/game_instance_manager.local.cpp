#include "game_instance_manager.h"

#include "../logging/logging.h"

#include <Windows.h>
#include <ShlObj.h>
#include <KnownFolders.h>
#include <string>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

using namespace libmcc;

// Resolve an MCC content root we can read variants from. We try in order:
//   1. $HALOX_MCC_ROOT       — explicit user override
//   2. Steam library default (Program Files (x86)/Steam/...)
//   3. Common Steam library drive paths (D:\, E:\, F:\)
// Cached after the first call. Returns nullptr if no candidate exists.
const wchar_t* resolve_mcc_root_impl();
extern "C" const wchar_t* halox_resolve_mcc_root() { return resolve_mcc_root_impl(); }

const wchar_t* resolve_mcc_root_impl() {
	static std::wstring cached;
	static bool resolved = false;
	if (resolved) return cached.empty() ? nullptr : cached.c_str();
	resolved = true;

	auto looks_like_mcc = [](const wchar_t* dir) -> bool {
		// Sanity check: real MCC roots have a halo3 subdirectory.
		std::wstring probe = dir;
		probe += L"\\halo3";
		DWORD attrs = GetFileAttributesW(probe.c_str());
		return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
	};

	wchar_t buf[MAX_PATH];
	DWORD len = GetEnvironmentVariableW(L"HALOX_MCC_ROOT", buf, MAX_PATH);
	if (len > 0 && len < MAX_PATH && looks_like_mcc(buf)) {
		cached = buf;
		return cached.c_str();
	}

	static const wchar_t* candidates[] = {
		L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Halo The Master Chief Collection",
		L"D:\\SteamLibrary\\steamapps\\common\\Halo The Master Chief Collection",
		L"D:\\Steam\\steamapps\\common\\Halo The Master Chief Collection",
		L"E:\\SteamLibrary\\steamapps\\common\\Halo The Master Chief Collection",
		L"E:\\Steam\\steamapps\\common\\Halo The Master Chief Collection",
		L"F:\\SteamLibrary\\steamapps\\common\\Halo The Master Chief Collection",
		L"G:\\SteamLibrary\\steamapps\\common\\Halo The Master Chief Collection",
	};
	for (auto* c : candidates) {
		if (looks_like_mcc(c)) {
			cached = c;
			return cached.c_str();
		}
	}

	return nullptr;
}

// Enumerate variant files in `dir` matching `pattern` (e.g. "*.bin"). Returns
// true if at least one file was found. Stores filenames in `out`.
static bool enumerate_variants(const wchar_t* dir, const wchar_t* pattern,
                               std::vector<std::string>* out) {
	wchar_t buffer[1024];
	swprintf(buffer, 1024, L"%s\\%s", dir, pattern);

	WIN32_FIND_DATAW find;
	HANDLE handle = FindFirstFileW(buffer, &find);
	if (handle == INVALID_HANDLE_VALUE) return false;

	do {
		// Convert filename UTF-16 → UTF-8 for storage (matches the existing
		// std::string vector type and is fine for ASCII filenames; full UTF-8
		// path is reconstructed in get_saved_game_file from game_variant_dir).
		char utf8[MAX_PATH];
		int n = WideCharToMultiByte(CP_UTF8, 0, find.cFileName, -1,
		                            utf8, sizeof(utf8), nullptr, nullptr);
		if (n > 0) out->emplace_back(utf8);
	} while (FindNextFileW(handle, &find));
	FindClose(handle);
	return !out->empty();
}

int c_game_instance_manager::load_local() {
	const wchar_t* mcc_root = resolve_mcc_root_impl();
	if (mcc_root) {
		CONSOLE_LOG_INFO("MCC content root: %ls", mcc_root);
	} else {
		CONSOLE_LOG_INFO("MCC content root: not found (set HALOX_MCC_ROOT env var to override). "
			"Variants will only be visible if the game subdirs are junctioned next to halox.exe.");
	}

	for (int i = _module_halo1; i < k_game_count; ++i) {
		auto local = s_game_locals + i;

		local->hopper_game_variants.clear();
		local->hopper_map_variants.clear();
		local->game_variant_dir.clear();
		local->map_variant_dir.clear();

		// Don't gate variant enumeration on DLL load — variants are content
		// files. We want the dropdowns populated even before a successful
		// DLL load so users can see what's available; launch will fail with
		// a clear "data_access null" if the DLL really isn't there.

		auto title = k_game_names[i];
		wchar_t wtitle[64];
		MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 64);

		// Try cwd-relative first (junctioned setups), then MCC root.
		auto try_dir = [](const std::wstring& dir, const wchar_t* pattern,
		                  std::vector<std::string>* out, std::wstring* out_dir) -> bool {
			if (enumerate_variants(dir.c_str(), pattern, out)) {
				*out_dir = dir;
				return true;
			}
			return false;
		};

		// Game variants
		{
			std::wstring cwd_dir = std::wstring(L".\\") + wtitle + L"\\hopper_game_variants";
			if (!try_dir(cwd_dir, L"*.bin", &local->hopper_game_variants, &local->game_variant_dir) && mcc_root) {
				std::wstring mcc_dir = std::wstring(mcc_root) + L"\\" + wtitle + L"\\hopper_game_variants";
				try_dir(mcc_dir, L"*.bin", &local->hopper_game_variants, &local->game_variant_dir);
			}
		}

		// Map variants
		{
			std::wstring cwd_dir = std::wstring(L".\\") + wtitle + L"\\hopper_map_variants";
			if (!try_dir(cwd_dir, L"*.mvar", &local->hopper_map_variants, &local->map_variant_dir) && mcc_root) {
				std::wstring mcc_dir = std::wstring(mcc_root) + L"\\" + wtitle + L"\\hopper_map_variants";
				try_dir(mcc_dir, L"*.mvar", &local->hopper_map_variants, &local->map_variant_dir);
			}
		}

		CONSOLE_LOG_INFO(
			"  %-9s game_variants=%zu map_variants=%zu",
			title,
			local->hopper_game_variants.size(),
			local->hopper_map_variants.size());
	}

	return 0;
}

i_unknown_ptr<i_unknown> c_game_instance_manager::get_saved_game_file(
	e_module game,
	int game_file_type,
	int game_file_index
) {
	int status;
	HANDLE handle;
	i_unknown* result;
	wchar_t buffer[1024];
	auto local = s_game_locals + game;
	auto variants = game_file_type ? &local->hopper_map_variants : &local->hopper_game_variants;
	const std::wstring& dir = game_file_type ? local->map_variant_dir : local->game_variant_dir;

	if (game_file_index < 0 || game_file_index >= (int)variants->size()) {
		return nullptr;
	}
	if (dir.empty()) {
		CONSOLE_LOG_WARN("get_saved_game_file: no resolved directory for %s/%s",
			k_game_names[game],
			game_file_type ? "hopper_map_variants" : "hopper_game_variants");
		return nullptr;
	}

	auto data_access = get_module_data_access(game);
	if (!data_access) {
		return nullptr;
	}

	auto& name = variants->at(game_file_index);
	swprintf(buffer, 1024, L"%s\\%hs", dir.c_str(), name.c_str());

	handle = CreateFileW(
		buffer,
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr
	);

	if (handle == INVALID_HANDLE_VALUE) {
		CONSOLE_LOG_WARN("get_saved_game_file: open failed (%lu) %ls",
			GetLastError(), buffer);
		return nullptr;
	}

	auto file_size = GetFileSize(handle, nullptr);
	if (file_size == INVALID_FILE_SIZE) {
		CloseHandle(handle);
		return nullptr;
	}

	std::vector<char> file_data(file_size);
	status = ReadFile(handle, file_data.data(), file_size, nullptr, nullptr);
	CloseHandle(handle);

	if (status == 0) {
		return nullptr;
	}

	if (game_file_type) {
		result = data_access->create_scenario_map_variant_from_file(
			file_data.data(),
			file_size
		);
	} else {
		result = data_access->create_game_engine_variant_from_file(
			file_data.data(),
			file_size
		);
	}

	return i_unknown_ptr<i_unknown>(result);
}

i_unknown_ptr<i_game_engine_variant> c_game_instance_manager::get_game_variant(
	e_module game,
	int hopper_game_variant
) {
	auto result = get_saved_game_file(game, 0, hopper_game_variant).release();
	return i_unknown_ptr<i_game_engine_variant>(reinterpret_cast<i_game_engine_variant*>(result));
}

i_unknown_ptr<i_scenario_map_variant> c_game_instance_manager::get_map_variant(
	e_module game,
	int hopper_map_variant
) {
	auto result = get_saved_game_file(game, 1, hopper_map_variant).release();
	return i_unknown_ptr<i_scenario_map_variant>(reinterpret_cast<i_scenario_map_variant*>(result));
}
