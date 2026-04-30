#include "halo1_native_overrides.h"

#include "game_instance_manager.h"
#include "../logging/logging.h"
#include "../rasterizer/rasterizer.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <thread>
#include <d3d11.h>
#include <dxgi.h>

#include <MinHook.h>

using namespace libmcc;

// --- halo1.dll render-config div-by-zero fix --------------------------------
//
// FUN_1801fcd40(uint width, uint height) at RVA 0x1FCD40 sets up the
// depth-mip-LBUF buffer ladder. Its body opens with:
//
//     uVar2 = max(width, height);
//     uVar2 = uVar2 / 0x1e - 1;       // <-- IDIV — faults when uVar2 < 0x1e
//     ... (bitwise OR cascade computes mip count)
//
// Our launcher path leaves the render-config global at *(0x182E3BDD8 + 0x118)
// uninitialized, so the direct caller (FUN_180453880, RVA 0x453880) reads
// width/height as 0 from offsets +0x20/+0x24 and forwards (0, 0) here. The
// real fix is to identify whatever halo3+ does that halo1 misses to populate
// that global; until that's tracked down, substituting sane non-zero
// dimensions in the hook lets the depth-mip ladder size itself successfully.
// Halo1 doesn't appear to read these dims after this call (it overrides them
// from its own framebuffer once the render thread takes over), so 1280x720 is
// safe.

static constexpr uintptr_t k_h1_div_zero_target_rva = 0x1FCD40;

using DivZeroFn = void (*)(uint32_t width, uint32_t height);
static DivZeroFn g_div_zero_orig    = nullptr;
static HMODULE   g_div_zero_module  = nullptr;

// Resolve a usable (width, height) pair when halo1 hands us zeros. Prefer the
// live swap-chain back-buffer; fall back to 1280x720 if that fails.
static void resolve_default_dims(uint32_t* out_w, uint32_t* out_h) {
	*out_w = 1280;
	*out_h = 720;

	auto* r = rasterizer();
	if (!r) return;
	auto* swap = r->get_swap_chain();
	if (!swap) return;

	DXGI_SWAP_CHAIN_DESC desc = {};
	if (SUCCEEDED(swap->GetDesc(&desc))) {
		if (desc.BufferDesc.Width  > 0) *out_w = desc.BufferDesc.Width;
		if (desc.BufferDesc.Height > 0) *out_h = desc.BufferDesc.Height;
	}
}

static void __fastcall h1_div_zero_detour(uint32_t width, uint32_t height) {
	uint32_t w = width;
	uint32_t h = height;

	// Two crash modes from the same root cause (uninitialized render-config
	// descriptor at *(DAT_182e3bdd8+0x118)+0x20/+0x24):
	//   (a) zeros — caller reads 0/0 from a missing struct
	//   (b) 0xFFFFFFFF — descriptor present but height is the "not found"
	//       sentinel; the bit-fold `((edx>>16)|edx)+1` overflows to 0 and DIVs
	// Live-snoop of MCC's running halo1 (2026-04-29) confirmed valid passes
	// look like (3840, 2160), so anything ≥ 0x1e is real. Substitute only on
	// the two known-bad sentinels.
	const bool bad_w = (w == 0) || (w == 0xFFFFFFFFu);
	const bool bad_h = (h == 0) || (h == 0xFFFFFFFFu);
	if (bad_w || bad_h) {
		uint32_t fw = 0, fh = 0;
		resolve_default_dims(&fw, &fh);
		if (fw < 0x1e) fw = 1280;
		if (fh < 0x1e) fh = 720;
		CONSOLE_LOG_WARN(
			"halo1 div-zero fix: caller passed (0x%X, 0x%X) — substituting (%u, %u)",
			width, height, fw, fh);
		if (bad_w) w = fw;
		if (bad_h) h = fh;
	}

	if (g_div_zero_orig) {
		g_div_zero_orig(w, h);
	}
}

void halo1_install_div_zero_fix() {
	auto im = game_instance_manager();
	if (!im) return;
	if (im->get_game() != _module_halo1) return;

	HMODULE mod = GetModuleHandleW(L"halo1.dll");
	if (!mod) {
		CONSOLE_LOG_WARN("halo1 div-zero fix: halo1.dll not loaded");
		return;
	}

	if (g_div_zero_module == mod) return;          // already hooked this load
	if (g_div_zero_module != nullptr) return;      // hooked a previous module — module rebase across launches not handled here

	static bool s_minhook_initialized = false;
	if (!s_minhook_initialized) {
		MH_STATUS st = MH_Initialize();
		if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
			CONSOLE_LOG_ERROR("halo1 div-zero fix: MH_Initialize failed (status=%d) — game will crash", (int)st);
			return;
		}
		s_minhook_initialized = true;
	}

	void* target = reinterpret_cast<void*>(
		reinterpret_cast<uintptr_t>(mod) + k_h1_div_zero_target_rva);
	void* trampoline = nullptr;
	MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&h1_div_zero_detour), &trampoline);
	if (st != MH_OK) {
		CONSOLE_LOG_ERROR("halo1 div-zero fix: MH_CreateHook @ %p failed (status=%d) — game will crash", target, (int)st);
		return;
	}
	g_div_zero_orig = reinterpret_cast<DivZeroFn>(trampoline);

	st = MH_EnableHook(target);
	if (st != MH_OK) {
		CONSOLE_LOG_ERROR("halo1 div-zero fix: MH_EnableHook @ %p failed (status=%d) — game will crash", target, (int)st);
		g_div_zero_orig = nullptr;
		return;
	}

	g_div_zero_module = mod;
	CONSOLE_LOG_INFO("halo1 div-zero fix installed @ %p (halo1+0x%llX)",
		target, (unsigned long long)k_h1_div_zero_target_rva);
}

// --- halo1.dll render-config field belt-and-suspenders stub ----------------
//
// FUN_1800ae410 (RVA 0xAE410), called via the per-engine init reached from
// initialize_game (FUN_18008c800), walks an 8-entry loop that reads:
//
//   (a) A render-config struct via:
//         *(longlong *)(DAT_182e3c090 + 0x118)
//           +0x10/+0x14 (low w/h) or +0x20/+0x24 (hi w/h)
//
//   (b) A named-RT lookup via:
//         FUN_1801f6fe0(DAT_181bea6b8, name)
//       where name comes from PTR_DAT_181b85e60[i*6] — a static table of
//       strings: "__FULL_8888_00__", "__FULL_8888_02__", "__HALO_RT_TEX_*"...
//
// (b) is the dominant cause of the AV at halo1+0xAE635 — the resource manager
// is empty until the engine's preload chain runs. The proper fix is to call
// PreloadCommonBegin (i_game_engine vtable slot 4) from the launcher, which
// Blam-Creation-Suite's MCC launcher does between create_game_engine and
// InitGraphics. Halo3+ stubbed slot 4 to a no-op so HaloX's launcher path
// never noticed the difference — but halo1's slot 4 still does real work.
//
// (a) is independently nice-to-have: stamping the render-config field with
// 1280x720 prevents *any* downstream null-pointer-deref on +0x10/+0x20 reads
// if some other code path zeros the field. It's purely data, not a hook.

static constexpr uintptr_t k_h1_global_struct_rva    = 0x2E3C090;
static constexpr uintptr_t k_h1_render_config_field  = 0x118;

// 256 bytes of zero — the engine reads the four (w,h) ints at +0x10..+0x27
// and we have no evidence it touches anything past +0x40, but we pad out
// generously so any other field we missed reads 0 (safe default).
struct s_h1_render_config_stub {
	uint32_t pad_00_to_0F[4];   // +0x00..+0x0F
	uint32_t w_lo;               // +0x10
	uint32_t h_lo;               // +0x14
	uint32_t pad_18_to_1F[2];    // +0x18..+0x1F
	uint32_t w_hi;               // +0x20
	uint32_t h_hi;               // +0x24
	uint8_t  pad_tail[256 - 0x28];
};
static_assert(sizeof(s_h1_render_config_stub) == 256, "stub size mismatch");
static s_h1_render_config_stub g_h1_render_config_stub = {};
static bool                    g_h1_render_config_installed = false;

void halo1_install_render_config_stub() {
	auto im = game_instance_manager();
	if (!im) return;
	if (im->get_game() != _module_halo1) return;

	HMODULE mod = GetModuleHandleW(L"halo1.dll");
	if (!mod) {
		CONSOLE_LOG_WARN("halo1 render-config stub: halo1.dll not loaded");
		return;
	}

	// Resolve the global struct's render-config-pointer field.
	uintptr_t global_struct = reinterpret_cast<uintptr_t>(mod) + k_h1_global_struct_rva;
	uintptr_t* field = reinterpret_cast<uintptr_t*>(global_struct + k_h1_render_config_field);

	// Populate the stub with sane dimensions. Prefer live swap-chain dims so
	// the rest of the engine (reading these later for camera setup, etc.)
	// gets the actual window size; fall back to 1280x720.
	uint32_t w = 1280, h = 720;
	if (auto* r = rasterizer()) {
		if (auto* sc = r->get_swap_chain()) {
			DXGI_SWAP_CHAIN_DESC desc = {};
			if (SUCCEEDED(sc->GetDesc(&desc))) {
				if (desc.BufferDesc.Width  > 0) w = desc.BufferDesc.Width;
				if (desc.BufferDesc.Height > 0) h = desc.BufferDesc.Height;
			}
		}
	}
	g_h1_render_config_stub.w_lo = w;
	g_h1_render_config_stub.h_lo = h;
	g_h1_render_config_stub.w_hi = w;
	g_h1_render_config_stub.h_hi = h;

	// Only patch if the field is currently NULL; if the engine populated it
	// (unlikely on our path, but possible after a re-launch), leave it alone.
	if (*field == 0) {
		*field = reinterpret_cast<uintptr_t>(&g_h1_render_config_stub);
		CONSOLE_LOG_INFO("halo1 render-config stub installed @ %p (halo1+0x%llX field) → struct=%p (%ux%u)",
			(void*)field, (unsigned long long)(k_h1_global_struct_rva + k_h1_render_config_field),
			(void*)&g_h1_render_config_stub, w, h);
	} else {
		CONSOLE_LOG_INFO("halo1 render-config stub: field already populated (=0x%llX), skipping",
			(unsigned long long)*field);
	}

	g_h1_render_config_installed = true;
}

// --- halo1.dll DAT_182e3bdd8 engine-context replica (snooped from MCC) -------
//
// MCC's normal launch path constructs a c_engine_context object on its heap
// via halo1!CreateGameEngine + slot 1 (initialize), then halo1's internal
// FUN_1801EEA10 stamps the pointer into halo1's DAT_182e3bdd8 global. From
// then on every halo1 subsystem reads that global to reach engine state.
//
// In halox's launch, slot 1 doesn't always populate the descriptor at +0x118
// with valid (width, height) — the field at +0x24 ends up as 0xFFFFFFFF
// (sentinel), which DIVs to 0 inside FUN_1801fcd40 (halo1+0x1FCDD9).
//
// Live-snoop of MCC's running halo1 (2026-04-29 via reverseme attach) gave us
// the exact 0x460-byte engine_ctx layout:
//   +0x000  vtable          → halo1+0x17F0C98
//   +0x008  ASCII GPU name  ("NVIDIA GeForce RTX 4090\0", 24 bytes)
//   +0x118  ptr to render-target descriptor  ← key field — valid w/h here
//   +0x138/+0x140/+0x148  3× pooled subsystem objects (vtable=halo1+0x17F9D10)
//   +0x178  duplicate of +0x118
//   +0x224/+0x228  resolution mirror (u32 width, u32 height)
//   +0x340  UTF-16 GPU name
// Other fields are inert data (refresh, gamma, etc.) or richer object graphs
// at +0x260/+0x270/+0x330/+0x338 which we leave NULL until something faults.
//
// The descriptor (~256 bytes) needs only:
//   +0x10/+0x14  display mode w/h
//   +0x20/+0x24  the w/h that FUN_1801fcd40 reads
//   +0x4C        ASCII GPU name copy
// Everything else can stay zero — the snoop showed the rest is mostly internal
// halo1 state pointers populated lazily.
//
// Defensive policy: only stamp DAT_182e3bdd8 if it's currently NULL, OR if
// the existing descriptor's +0x24 reads as the 0xFFFFFFFF sentinel. We never
// overwrite a fully-populated engine_ctx that slot 1 successfully built.

static constexpr uintptr_t k_h1_engine_ctx_global_rva = 0x2E3BDD8;
// Sibling engine-state global. FUN_1800AE280 reads (DAT_182e3c090+0x118) as
// a descriptor pointer, identical layout to (DAT_182e3bdd8+0x118). It's also
// the global the spinwait reaches +0x500 through. Stamping it with the same
// engine_ctx blob makes both reads return our valid descriptor.
static constexpr uintptr_t k_h1_engine_state_global_rva = 0x2E3C090;
static constexpr uintptr_t k_h1_engine_ctx_vtable_rva = 0x17F0C98;
static constexpr uintptr_t k_h1_pooled_obj_vtable_rva = 0x17F9D10;
// CreateGameEngine allocates 0x460 but the real class is larger — live snoop
// of MCC (2026-04-29) showed populated heap pointers at +0x4D8 and +0x4E8,
// and FUN_1801EEA10 crashed on us at +0x19F because it reads this->[0x4D8]
// and our 0x460-sized stub leaked into adjacent halox BSS (non-zero garbage
// failed the `test rcx,rcx; jz` skip). Oversize to 0x1000 with zero-pad so
// any "load past 0x460" reads zero and the implicit-null guards short-circuit.
static constexpr size_t    k_h1_engine_ctx_size       = 0x1000;
static constexpr size_t    k_h1_descriptor_size       = 0x400;
static constexpr size_t    k_h1_pooled_obj_size       = 0x200;

struct alignas(32) s_h1_engine_ctx_blob {
	uint8_t bytes[k_h1_engine_ctx_size];
};
struct alignas(16) s_h1_descriptor_blob {
	uint8_t bytes[k_h1_descriptor_size];
};
struct alignas(16) s_h1_pooled_obj_blob {
	uint8_t bytes[k_h1_pooled_obj_size];
};

static s_h1_engine_ctx_blob g_h1_engine_ctx_storage   = {};
static s_h1_descriptor_blob g_h1_descriptor_storage   = {};
static s_h1_pooled_obj_blob g_h1_pooled_obj_storage[3] = {};
static bool                 g_h1_engine_ctx_installed = false;

void halo1_install_engine_ctx_stub() {
	auto im = game_instance_manager();
	if (!im) return;
	if (im->get_game() != _module_halo1) return;
	if (g_h1_engine_ctx_installed) return;

	HMODULE mod = GetModuleHandleW(L"halo1.dll");
	if (!mod) {
		CONSOLE_LOG_WARN("halo1 engine_ctx stub: halo1.dll not loaded");
		return;
	}
	uintptr_t base = reinterpret_cast<uintptr_t>(mod);

	// Check current state of DAT_182e3bdd8.
	auto** global_slot = reinterpret_cast<uint8_t**>(base + k_h1_engine_ctx_global_rva);
	uint8_t* existing  = *global_slot;
	bool needs_stamp = (existing == nullptr);
	if (!needs_stamp) {
		// Slot 1 ran. Check whether its descriptor at +0x118 has the bad
		// sentinel — if so, our stamp still wins; otherwise leave it alone.
		auto** desc_ptr = reinterpret_cast<uint8_t**>(existing + 0x118);
		uint8_t* desc   = *desc_ptr;
		if (desc) {
			uint32_t h = *reinterpret_cast<uint32_t*>(desc + 0x24);
			if (h == 0xFFFFFFFFu || h == 0) {
				CONSOLE_LOG_WARN("halo1 engine_ctx stub: existing descriptor has bad height=0x%X — overriding", h);
				needs_stamp = true;
			}
		} else {
			CONSOLE_LOG_WARN("halo1 engine_ctx stub: existing engine_ctx has NULL descriptor — overriding");
			needs_stamp = true;
		}
	}
	if (!needs_stamp) {
		CONSOLE_LOG_INFO("halo1 engine_ctx stub: DAT_182e3bdd8 already populated with valid descriptor — skipping");
		g_h1_engine_ctx_installed = true;
		return;
	}

	// Resolve real screen dims from the swapchain (or fall back).
	uint32_t w = 1280, h = 720;
	resolve_default_dims(&w, &h);
	if (w < 0x1e) w = 1280;
	if (h < 0x1e) h = 720;

	// Build the descriptor (~256 bytes). Mirror MCC's snoop layout for the
	// fields that matter; leave the rest zero.
	uint8_t* desc = g_h1_descriptor_storage.bytes;
	memset(desc, 0, k_h1_descriptor_size);
	*reinterpret_cast<uint32_t*>(desc + 0x10) = w;
	*reinterpret_cast<uint32_t*>(desc + 0x14) = h;
	*reinterpret_cast<uint32_t*>(desc + 0x18) = 0x20;       // BPP/format hint
	*reinterpret_cast<float*>   (desc + 0x1C) = 1.0f;
	*reinterpret_cast<uint32_t*>(desc + 0x20) = w;          // ← read by FUN_1801fcd40
	*reinterpret_cast<uint32_t*>(desc + 0x24) = h;          // ← read by FUN_1801fcd40
	memcpy(desc + 0x4C, "halox-stub-display\0", 19);

	// Build the 3 pooled subsystem objects (+0x138/+0x140/+0x148 targets).
	// Live snoop showed them all as: vtable @ halo1+0x17F9D10, u32 2 at +0x18,
	// rest zero. Stamp the same.
	for (int i = 0; i < 3; i++) {
		uint8_t* obj = g_h1_pooled_obj_storage[i].bytes;
		memset(obj, 0, k_h1_pooled_obj_size);
		*reinterpret_cast<void**>   (obj + 0x00) = reinterpret_cast<void*>(base + k_h1_pooled_obj_vtable_rva);
		*reinterpret_cast<uint32_t*>(obj + 0x18) = 2;
	}

	// Build the engine_ctx itself.
	uint8_t* ctx = g_h1_engine_ctx_storage.bytes;
	memset(ctx, 0, k_h1_engine_ctx_size);
	*reinterpret_cast<void**>(ctx + 0x000) = reinterpret_cast<void*>(base + k_h1_engine_ctx_vtable_rva);
	memcpy(ctx + 0x008, "halox-stub-display\0", 19);

	*reinterpret_cast<void**>(ctx + 0x118) = desc;          // descriptor ptr (primary)
	*reinterpret_cast<void**>(ctx + 0x178) = desc;          // descriptor ptr (mirror)
	*reinterpret_cast<void**>(ctx + 0x138) = g_h1_pooled_obj_storage[0].bytes;
	*reinterpret_cast<void**>(ctx + 0x140) = g_h1_pooled_obj_storage[1].bytes;
	*reinterpret_cast<void**>(ctx + 0x148) = g_h1_pooled_obj_storage[2].bytes;
	*reinterpret_cast<uint32_t*>(ctx + 0x224) = w;          // resolution mirror
	*reinterpret_cast<uint32_t*>(ctx + 0x228) = h;
	*reinterpret_cast<float*>   (ctx + 0x1A0) = 0.95f;      // gamma/sens-ish floats from snoop
	*reinterpret_cast<float*>   (ctx + 0x1A8) = 1.0f;

	*global_slot = ctx;

	// Also stamp the sibling DAT_182e3c090 if it's NULL — FUN_1800AE280
	// reads (DAT_182e3c090+0x118) for the same descriptor; without this it
	// AVs at halo1+0xAE31D on the first iteration. Other halo1 paths use
	// (DAT_182e3c090+0x500) which our zero-padded ctx returns as NULL —
	// those callers either NULL-check or get caught by the existing
	// spinwait/vtable-swap belt-and-suspenders.
	auto** state_slot = reinterpret_cast<uint8_t**>(base + k_h1_engine_state_global_rva);
	if (*state_slot == nullptr) {
		*state_slot = ctx;
		CONSOLE_LOG_INFO("halo1 engine_state stub installed @ DAT_182e3c090=%p (sharing ctx=%p)",
			(void*)state_slot, (void*)ctx);
	}

	g_h1_engine_ctx_installed = true;

	CONSOLE_LOG_INFO("halo1 engine_ctx stub installed @ DAT_182e3bdd8=%p ctx=%p desc=%p (%ux%u)",
		(void*)global_slot, (void*)ctx, (void*)desc, w, h);
}

// --- halo1.dll PreloadCommonBegin / PreloadLevelBegin direct calls ----------
//
// These are the missing init steps in HaloX's launcher. Blam-Creation-Suite's
// MCC launcher (Framework/GameFramework/MCC/game_launcher.cpp,
// c_game_launcher::launch_mcc_game) invokes them between create_game_engine
// and InitGraphics:
//
//     game_engine->PreloadCommonBegin(c_render::s_device);
//     game_engine->PreloadLevelBegin(game_options->map_id);
//     game_engine->InitGraphics(device, ctx, swap, swap);
//     game_engine->PlayGame(host, options);
//
// On the IGameEngine V2 vtable layout (build >= 1367), slots 4 / 5 are
// PreloadCommonBegin / PreloadLevelBegin. The libmcc::i_game_engine binding
// declares the same vtable shape but names the two slots `_` and `__` because
// halo3+ stubbed them to no-ops. halo1 still does the real preload work in
// those slots — and it's substantial:
//   slot 4 (FUN_180095700): allocates DAT_182e3d2f8 (preload state struct),
//                          reads game.cfg [Preload.usePaks/usePreloadUui],
//                          spins up the UUI Saber Preload thread.
//   slot 5 (FUN_180095910): per-map preload setup, sets the
//                          DAT_182e3d2f8 + 0xcd01a8 "level-preloaded" flag
//                          that PlayGame's first if-block reads.
//
// Without slot 4 having run, PlayGame's leading branch is skipped and a chain
// of subsequent init code paths leave the resource manager (DAT_181bea6b8)
// empty — which is what makes FUN_1801f6fe0 return NULL inside FUN_1800ae410
// and produce the AV at halo1+0xAE635.
//
// We invoke them via a manual vtable lookup (no MinHook involvement). Pure
// virtual call.

void halo1_call_preload_common_begin(void* engine, void* device) {
	if (engine == nullptr) return;
	auto vtable = *reinterpret_cast<void***>(engine);
	if (vtable == nullptr) return;
	using Slot4Fn = void(__fastcall*)(void* /*this*/, void* /*device*/);
	auto fn = reinterpret_cast<Slot4Fn>(vtable[4]);
	CONSOLE_LOG_INFO("halo1_call_preload_common_begin: this=%p vt=%p slot4=%p device=%p",
		engine, (void*)vtable, (void*)fn, device);
	fn(engine, device);
	CONSOLE_LOG_INFO("halo1_call_preload_common_begin: returned");
}

void halo1_call_preload_level_begin(void* engine, int map_index) {
	if (engine == nullptr) return;
	auto vtable = *reinterpret_cast<void***>(engine);
	if (vtable == nullptr) return;
	// halo1's slot 5 takes (this, int map_index). Negative map_index is a
	// safe no-op path inside the function (the body is gated on
	// `-1 < param_2 && DAT_182e3d2f8 != 0`).
	using Slot5Fn = void(__fastcall*)(void* /*this*/, int /*map_index*/);
	auto fn = reinterpret_cast<Slot5Fn>(vtable[5]);
	CONSOLE_LOG_INFO("halo1_call_preload_level_begin: this=%p vt=%p slot5=%p map_index=%d",
		engine, (void*)vtable, (void*)fn, map_index);
	fn(engine, map_index);
	CONSOLE_LOG_INFO("halo1_call_preload_level_begin: returned");
}

// --- Heap-only vtable swap on RT instances -----------------------------------
//
// See halo1_native_overrides.h for the full rationale. Short version:
// FUN_1800AE410 dereferences a NULL subresource pointer at AE635 in our
// launcher path. We override `vt[0x1A]` (GetSubResource) and `vt[0x1B]`
// (GetMipSubResource) on each RT instance by rewriting its vtable pointer to
// a writable copy whose two affected slots return a stub object with no-op
// AddRef/Release.

static constexpr uintptr_t k_h1_rt_manager_global_rva = 0x1BEA6B8;
static constexpr uintptr_t k_h1_rt_manager_cs_rva     = 0x1BAA028;

// Stub vtable for the subresource we hand back from the overridden slots.
// We don't know every slot the engine might call on the result, so we fill
// generously with a no-op-returning-zero default.
static constexpr int k_stub_vtable_slots = 64;
static void* g_stub_sub_vtable[k_stub_vtable_slots] = {};

struct s_stub_subresource {
	void**   vtable;
	uint32_t refcount;
	uint32_t pad1;
	uint8_t  pad[256 - 16];
};
static_assert(sizeof(s_stub_subresource) == 256, "stub size mismatch");
static s_stub_subresource g_stub_subresource = {};

// Default no-op vtable handlers. x64 MSVC calling convention puts `this` in
// RCX, args in RDX, R8, R9 — so a plain function with `void*` first arg gets
// the engine's `this` pointer correctly.
static void     stub_dtor(void*)                      {}
static uint32_t stub_addref(void*)                    { return 1; }
static uint32_t stub_release(void*)                   { return 1; }
static void*    stub_return_null(void*, ...)          { return nullptr; }
static int64_t  stub_return_zero_i(void*, ...)        { return 0; }

// Per-original-vtable cache: we need to keep a writable copy per unique
// halo1 RT vtable seen, with slots 0x1A and 0x1B overridden. Multiple RT
// instances can share the same original vtable; one copy serves all.
struct s_swapped_vt_entry {
	void*  original;
	void** copy;
};
static constexpr int k_max_unique_vts = 8;
static s_swapped_vt_entry g_swapped_vts[k_max_unique_vts] = {};
static int                g_swapped_vt_count = 0;
static SRWLOCK            g_swapped_vt_lock = SRWLOCK_INIT;

// RT-class slot overrides. Engine calls `(this->vt[0xD0/8])(this, idx)` for
// the GetSubResource accessor and `(this->vt[0xD8/8])(this, face, mip, ?)`
// for GetMipSubResource. We return a pointer to our stub object in both
// cases so the downstream `vt[1](result)` AddRef call doesn't AV.
static void* stub_rt_get_subresource(void* /*self*/, int /*idx*/) {
	return &g_stub_subresource;
}
static void* stub_rt_get_mip_subresource(void* /*self*/, int /*face*/, int /*mip*/, int /*samples*/) {
	return &g_stub_subresource;
}

static void initialize_stub_subresource_once() {
	static bool s_initialized = false;
	if (s_initialized) return;
	for (int i = 0; i < k_stub_vtable_slots; i++) {
		g_stub_sub_vtable[i] = reinterpret_cast<void*>(&stub_return_zero_i);
	}
	g_stub_sub_vtable[0] = reinterpret_cast<void*>(&stub_dtor);
	g_stub_sub_vtable[1] = reinterpret_cast<void*>(&stub_addref);
	g_stub_sub_vtable[2] = reinterpret_cast<void*>(&stub_release);
	g_stub_subresource.vtable   = g_stub_sub_vtable;
	g_stub_subresource.refcount = 1;
	s_initialized = true;
}

// Look up (or allocate) a writable copy of the given original vtable, with
// slots 0x1A / 0x1B overridden.
static void** get_or_create_vt_copy(void* original_vt) {
	AcquireSRWLockExclusive(&g_swapped_vt_lock);

	for (int i = 0; i < g_swapped_vt_count; i++) {
		if (g_swapped_vts[i].original == original_vt) {
			void** ret = g_swapped_vts[i].copy;
			ReleaseSRWLockExclusive(&g_swapped_vt_lock);
			return ret;
		}
	}

	if (g_swapped_vt_count >= k_max_unique_vts) {
		CONSOLE_LOG_WARN("halo1 RT vtable swap: exceeded %d unique vtables", k_max_unique_vts);
		ReleaseSRWLockExclusive(&g_swapped_vt_lock);
		return nullptr;
	}

	// Vtables can be large. 256 slots = 2KB is generous for any RT class.
	constexpr size_t copy_bytes = 256 * sizeof(void*);
	void** copy = static_cast<void**>(VirtualAlloc(
		nullptr, copy_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	if (!copy) {
		ReleaseSRWLockExclusive(&g_swapped_vt_lock);
		return nullptr;
	}

	memcpy(copy, original_vt, copy_bytes);
	copy[0x1A] = reinterpret_cast<void*>(&stub_rt_get_subresource);    // offset 0xD0
	copy[0x1B] = reinterpret_cast<void*>(&stub_rt_get_mip_subresource); // offset 0xD8

	g_swapped_vts[g_swapped_vt_count++] = { original_vt, copy };
	int idx = g_swapped_vt_count - 1;
	ReleaseSRWLockExclusive(&g_swapped_vt_lock);

	CONSOLE_LOG_INFO("halo1 RT vtable swap: cached new vtable copy (#%d) original=%p copy=%p",
		idx, original_vt, (void*)copy);
	return copy;
}

// Returns true if `vt` is one of our known writable copies (i.e., already
// swapped — skip).
static bool is_already_swapped(void* vt) {
	AcquireSRWLockShared(&g_swapped_vt_lock);
	bool found = false;
	for (int i = 0; i < g_swapped_vt_count; i++) {
		if (g_swapped_vts[i].copy == vt) { found = true; break; }
	}
	ReleaseSRWLockShared(&g_swapped_vt_lock);
	return found;
}

int halo1_swap_rt_vtables_once() {
	initialize_stub_subresource_once();

	auto im = game_instance_manager();
	if (!im) return 0;
	if (im->get_game() != _module_halo1) return 0;

	HMODULE mod = GetModuleHandleW(L"halo1.dll");
	if (!mod) return 0;

	uintptr_t mod_base = reinterpret_cast<uintptr_t>(mod);
	void* manager = *reinterpret_cast<void**>(mod_base + k_h1_rt_manager_global_rva);
	if (!manager) return -1; // not allocated yet

	LPCRITICAL_SECTION cs = reinterpret_cast<LPCRITICAL_SECTION>(mod_base + k_h1_rt_manager_cs_rva);
	EnterCriticalSection(cs);

	void** array = *reinterpret_cast<void***>((uintptr_t)manager + 0x1B8);
	int    count = *reinterpret_cast<int*>((uintptr_t)manager + 0x1C0);

	int swapped = 0;
	if (array && count > 0 && count < 4096) { // sanity bound
		for (int i = 0; i < count; i++) {
			void* entry = array[i];
			if (!entry) continue;
			void* current_vt = *reinterpret_cast<void**>(entry);
			if (!current_vt) continue;
			if (is_already_swapped(current_vt)) continue;
			void** copy = get_or_create_vt_copy(current_vt);
			if (!copy) continue;
			*reinterpret_cast<void***>(entry) = copy;
			swapped++;
		}
	}

	LeaveCriticalSection(cs);

	if (swapped > 0) {
		CONSOLE_LOG_INFO("halo1 RT vtable swap: swapped %d new entries (total table count=%d)",
			swapped, count);
	}
	return swapped;
}

void halo1_start_rt_vtable_swap_poller() {
	static bool s_started = false;
	if (s_started) return;
	s_started = true;

	std::thread([](){
		// Poll for up to 5 seconds total at 25ms cadence (200 iterations).
		// Each iteration is an idempotent walk that only swaps NEW entries.
		// We don't stop early — entries can appear over time as the engine
		// adds them in stages (preload, level setup, etc.).
		for (int i = 0; i < 200; i++) {
			halo1_swap_rt_vtables_once();
			Sleep(25);
		}
		CONSOLE_LOG_INFO("halo1 RT vtable swap: poller exiting after 5s");
	}).detach();
}

// --- halo1.dll runtime byte-patch (RT subresource AddRef NOP) ----------------

static constexpr uintptr_t k_h1_rt_addref_patch_rva = 0xAE635;
// Bytes to replace: `48 8b 10 ff 52 08` (mov rdx,[rax]; call [rdx+8])
static constexpr size_t    k_h1_rt_addref_patch_len = 6;

void halo1_install_rt_subresource_addref_nop() {
	auto im = game_instance_manager();
	if (!im) return;
	if (im->get_game() != _module_halo1) return;

	HMODULE mod = GetModuleHandleW(L"halo1.dll");
	if (!mod) {
		CONSOLE_LOG_WARN("halo1 RT addref NOP: halo1.dll not loaded");
		return;
	}

	auto* patch_addr = reinterpret_cast<uint8_t*>(
		reinterpret_cast<uintptr_t>(mod) + k_h1_rt_addref_patch_rva);

	// Idempotent: if the bytes are already NOPs, skip.
	bool already_patched = true;
	for (size_t i = 0; i < k_h1_rt_addref_patch_len; i++) {
		if (patch_addr[i] != 0x90) { already_patched = false; break; }
	}
	if (already_patched) {
		CONSOLE_LOG_INFO("halo1 RT addref NOP: already applied @ %p", patch_addr);
		return;
	}

	// Verify the original bytes match what we expect, otherwise abort —
	// this guards against a halo1.dll update that shifted the offset.
	static const uint8_t expected[k_h1_rt_addref_patch_len] = {
		0x48, 0x8b, 0x10, 0xff, 0x52, 0x08
	};
	for (size_t i = 0; i < k_h1_rt_addref_patch_len; i++) {
		if (patch_addr[i] != expected[i]) {
			CONSOLE_LOG_ERROR(
				"halo1 RT addref NOP: byte mismatch @ %p offset %zu — got 0x%02X, expected 0x%02X. "
				"halo1.dll layout may have shifted; aborting patch.",
				patch_addr, i, patch_addr[i], expected[i]);
			return;
		}
	}

	DWORD old_protect = 0;
	if (!VirtualProtect(patch_addr, k_h1_rt_addref_patch_len, PAGE_EXECUTE_READWRITE, &old_protect)) {
		CONSOLE_LOG_ERROR("halo1 RT addref NOP: VirtualProtect(RWX) failed: %lu", GetLastError());
		return;
	}

	for (size_t i = 0; i < k_h1_rt_addref_patch_len; i++) {
		patch_addr[i] = 0x90;
	}

	DWORD restored = 0;
	VirtualProtect(patch_addr, k_h1_rt_addref_patch_len, old_protect, &restored);
	FlushInstructionCache(GetCurrentProcess(), patch_addr, k_h1_rt_addref_patch_len);

	CONSOLE_LOG_INFO("halo1 RT addref NOP installed @ %p (halo1+0x%llX, %zu bytes)",
		patch_addr, (unsigned long long)k_h1_rt_addref_patch_rva, k_h1_rt_addref_patch_len);
}

// --- halo1.dll FUN_1800AE280 — named-RT registration --------------------------

static constexpr uintptr_t k_h1_register_named_rts_rva = 0xAE280;

void halo1_call_register_named_render_targets() {
	auto im = game_instance_manager();
	if (!im) return;
	if (im->get_game() != _module_halo1) return;

	HMODULE mod = GetModuleHandleW(L"halo1.dll");
	if (!mod) {
		CONSOLE_LOG_WARN("halo1_call_register_named_render_targets: halo1.dll not loaded");
		return;
	}

	using Fn = void(*)();
	auto fn = reinterpret_cast<Fn>(
		reinterpret_cast<uintptr_t>(mod) + k_h1_register_named_rts_rva);
	CONSOLE_LOG_INFO("halo1_call_register_named_render_targets: calling halo1+0x%llX (%p)",
		(unsigned long long)k_h1_register_named_rts_rva, (void*)fn);
	// SEH-wrap because AE280 may still trip on later iterations even with
	// DAT_182e3c090 stamped — registering RTs partially is still better
	// than not at all (reduces dangling-SRV count downstream).
	__try {
		fn();
		CONSOLE_LOG_INFO("halo1_call_register_named_render_targets: returned");
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		CONSOLE_LOG_WARN("halo1_call_register_named_render_targets: SEH 0x%08lX inside AE280 — partial registration may have completed",
			GetExceptionCode());
	}
}

// --- halo1.dll FUN_1800AE410 entry spin-wait detour --------------------------

static constexpr uintptr_t k_h1_init_rts_rva                = 0xAE410;
static constexpr uintptr_t k_h1_dat_182e3c090_rva           = 0x2E3C090;
static constexpr ptrdiff_t k_h1_engine_state_rt_root_offset = 0x500;
static constexpr ptrdiff_t k_h1_rt_root_pool_a_offset       = 0xE8;

using InitRenderTargetsFn = void(*)();
static InitRenderTargetsFn g_h1_init_rts_orig   = nullptr;
static HMODULE             g_h1_init_rts_module = nullptr;

static void h1_init_render_targets_detour() {
	auto base = reinterpret_cast<uintptr_t>(g_h1_init_rts_module);
	auto** engine_state_global =
		reinterpret_cast<unsigned char**>(base + k_h1_dat_182e3c090_rva);

	// Bounded spin: 500 iterations × 10ms = 5s cap. Long enough for the
	// engine init thread to finish (typically <100ms in practice), short
	// enough to fail loudly if something is structurally wrong.
	constexpr int  k_max_iters = 500;
	constexpr DWORD k_sleep_ms = 10;

	int iters = 0;
	void* pool_a = nullptr;
	for (; iters < k_max_iters; iters++) {
		auto* engine_state = *engine_state_global;
		if (engine_state) {
			auto* rt_root = *reinterpret_cast<unsigned char**>(
				engine_state + k_h1_engine_state_rt_root_offset);
			if (rt_root) {
				pool_a = *reinterpret_cast<void**>(
					rt_root + k_h1_rt_root_pool_a_offset);
				if (pool_a) break;
			}
		}
		Sleep(k_sleep_ms);
	}

	if (pool_a) {
		CONSOLE_LOG_INFO("halo1 RT init spinwait: pool ready after %d iters (%dms), pool=%p",
			iters, iters * (int)k_sleep_ms, pool_a);
	} else {
		CONSOLE_LOG_WARN("halo1 RT init spinwait: TIMEOUT after %d iters (%dms) — "
			"engine_state=%p; original AV at halo1+0x22B81F likely to follow",
			iters, iters * (int)k_sleep_ms, (void*)*engine_state_global);
	}

	g_h1_init_rts_orig();
}

void halo1_install_init_render_targets_spinwait() {
	auto im = game_instance_manager();
	if (!im) return;
	if (im->get_game() != _module_halo1) return;

	if (g_h1_init_rts_orig != nullptr) return;  // idempotent

	HMODULE mod = GetModuleHandleW(L"halo1.dll");
	if (!mod) {
		CONSOLE_LOG_WARN("halo1 RT init spinwait: halo1.dll not loaded");
		return;
	}
	g_h1_init_rts_module = mod;

	auto* target = reinterpret_cast<void*>(
		reinterpret_cast<uintptr_t>(mod) + k_h1_init_rts_rva);

	if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
		// CreateHook will fail below if MH isn't ready; let it report.
	}

	void* trampoline = nullptr;
	auto st = MH_CreateHook(target,
		reinterpret_cast<void*>(&h1_init_render_targets_detour),
		&trampoline);
	if (st != MH_OK) {
		CONSOLE_LOG_ERROR("halo1 RT init spinwait: MH_CreateHook failed (status=%d) @ %p",
			(int)st, target);
		return;
	}
	g_h1_init_rts_orig = reinterpret_cast<InitRenderTargetsFn>(trampoline);

	st = MH_EnableHook(target);
	if (st != MH_OK) {
		CONSOLE_LOG_ERROR("halo1 RT init spinwait: MH_EnableHook failed (status=%d) @ %p",
			(int)st, target);
		return;
	}

	CONSOLE_LOG_INFO("halo1 RT init spinwait installed @ %p (halo1+0x%llX)",
		target, (unsigned long long)k_h1_init_rts_rva);
}

