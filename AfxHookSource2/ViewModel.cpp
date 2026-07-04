#include "ViewModel.h"
#include "Globals.h"
#include "Filmmaker/Movie/ViewFx.h"
#include "Filmmaker/Movie/ViewFxVm.h"

#include <cstddef>
#include <windows.h>
#include "../deps/release/Detours/src/detours.h"

namespace {
// Self-contained wall-clock seconds for the sway phase clock -- deliberately NOT tied to
// g_MirvTime (demo time), so idle sway keeps animating the same way whether the demo is
// playing or paused, matching the free-cam wall-clock trick main.cpp's view-setup trampoline
// already uses for the same reason. Same QueryPerformanceCounter pattern, kept local since
// this is the only place in this file that needs a clock.
float WallClockSeconds() {
	static LARGE_INTEGER s_freq = {};
	static LARGE_INTEGER s_start = {};
	if (s_freq.QuadPart == 0) {
		QueryPerformanceFrequency(&s_freq);
		QueryPerformanceCounter(&s_start);
	}
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (float)((double)(now.QuadPart - s_start.QuadPart) / (double)s_freq.QuadPart);
}
} // namespace

struct MirvViewmodel 
{
	bool enabled = false;
	bool hooked = false;
	bool leftHandedValue = false;
	float offsetX = 0.0f, offsetY = 0.0f, offsetZ = 0.0f;
	float fovValue = 68.0f;
	bool enabledX = false, enabledY = false, enabledZ = false, enabledFOV = false, enabledLeftHanded = false;

	void setViewmodel
	(
		float x, float y, float z, float pFov, bool leftHanded, 
		bool setX = false, bool setY = false, bool setZ = false, bool setFOV = false, bool setLeftHanded = false
	)
	{
		if(setX) offsetX = x;
		if(setY) offsetY = y;
		if(setZ) offsetZ = z;
		if(setFOV) fovValue = pFov;
		if(setLeftHanded) leftHandedValue = leftHanded;

		enabledX = setX;
		enabledY = setY;
		enabledZ = setZ;
		enabledFOV = setFOV;
		enabledLeftHanded = setLeftHanded;
	
	};
} g_MirvViewmodel;

typedef void(__fastcall *g_OriginalViewmodelFunc_t)(void* param_1, float* pViewmodelOffsets, float* pFov);
g_OriginalViewmodelFunc_t g_OriginalViewmodelFunc = nullptr;

void __fastcall setViewmodel(void* param_1, float* pViewmodelOffsets, float* pFov)
{
    g_OriginalViewmodelFunc(param_1, pViewmodelOffsets, pFov);

    if (pViewmodelOffsets != nullptr && g_MirvViewmodel.enabled) {
        if (g_MirvViewmodel.enabledX) pViewmodelOffsets[0] = g_MirvViewmodel.offsetX; // x
        if (g_MirvViewmodel.enabledY) pViewmodelOffsets[1] = g_MirvViewmodel.offsetY; // y
        if (g_MirvViewmodel.enabledZ) pViewmodelOffsets[2] = g_MirvViewmodel.offsetZ; // z
		if (g_MirvViewmodel.enabledFOV) pFov[0] = g_MirvViewmodel.fovValue;
    }

    // ViewFx viewmodel modifiers: ADD on top of whichever offset is already in the buffer --
    // the engine's own default, or the static override just above -- so they compose with
    // mirv_viewmodel instead of fighting it. All are no-ops (all-zero) while Off.
    //   * sway: movement-scaled walk bob/drift.
    //   * deadzone: shifts the weapon toward the TRUE aim while the camera lags it (the
    //     "weapon moves first inside an aim deadzone" decoupled-viewmodel effect; the camera
    //     half lives in main.cpp's view-setup trampoline).
    //   * bob follow: the view bob's camera half (also main.cpp) raises the world-render
    //     origin, but the engine anchors the viewmodel to the pawn's unmodified eye pos --
    //     without this the gun visually bobs INVERSELY while the world reads as still.
    //     Follows GoldSrc V_CalcNormalRefdef: gun up by bob, forward by 0.4*bob.
    if (pViewmodelOffsets != nullptr) {
        float swayX = 0.0f, swayY = 0.0f, swayZ = 0.0f;
        Filmmaker::ViewFxRef().SwayOffset(WallClockSeconds(), swayX, swayY, swayZ);
        float dzX = 0.0f, dzY = 0.0f, dzZ = 0.0f;
        Filmmaker::ViewFxRef().DeadzoneViewmodelShift(dzX, dzY, dzZ);
        float bobFwd = 0.0f, bobUp = 0.0f;
        Filmmaker::ViewFxRef().BobViewmodelFollow(bobFwd, bobUp);
        pViewmodelOffsets[0] += swayX + dzX;
        pViewmodelOffsets[1] += swayY + dzY + bobFwd;
        pViewmodelOffsets[2] += swayZ + dzZ + bobUp;
    }

    // ViewFxVm write-site 1 (helper-post): we are INSIDE CS2's viewmodel calc right now.
    Filmmaker::ViewFxVm_OnViewmodelCalc();
};

typedef bool(__fastcall *g_OriginalHandFunc_t)(int64_t param_1);
g_OriginalHandFunc_t g_OriginalHandFunc = nullptr;

bool __fastcall setHand(int64_t param_1)
{
	bool res = g_OriginalHandFunc(param_1);

	if (g_MirvViewmodel.enabled && g_MirvViewmodel.enabledLeftHanded) {
		return g_MirvViewmodel.leftHandedValue;
	}

	return res;
};

void HookViewmodel(HMODULE clientDll)
{
	if (g_MirvViewmodel.hooked) return;

	// This function references the cvars viewmodel_offset_x, viewmodel_offset_y, viewmodel_offset_z, viewmodel_offset_fov (usually 3rd reference).
	// can be also found with 00 00 88 42 (68.0 in float), which is max limit for fov
	// there are 2 similiar functions, make sure to test if it breakes again to not confuse them
	size_t viewmodelAddr = getAddress(clientDll, "40 55 53 56 41 56 41 57 48 8B EC 48 83 EC 20 4D 8B F8 4C 8B F2 48 8B F1"); 
	if (0 == viewmodelAddr) {
		ErrorBox(MkErrStr(__FILE__, __LINE__));	//TODO
		return;
	}
	// vtable byte offset 0xac8 for "C_CSGO_TeamPreviewModel" class (4th from end.).
	// This function is called right after the first call to viewmodelAddr function.
	size_t handAddr = getAddress(clientDll, "40 53 48 83 EC ?? 80 B9 ?? ?? ?? ?? ?? 48 8B D9 0F 84 ?? ?? ?? ?? 48 8B 89 ?? ?? ?? ?? 48 85 C9 75");
	if (0 == handAddr) {
		ErrorBox(MkErrStr(__FILE__, __LINE__));	
		return;
	}

    g_OriginalViewmodelFunc = (g_OriginalViewmodelFunc_t)(viewmodelAddr);
	g_OriginalHandFunc = (g_OriginalHandFunc_t)(handAddr);

	// Share the (pre-detour) helper address so ViewFxVm's site 4 can locate + detour its CALLER
	// (CS2's CalcViewModelView) -- Detours patches the function body, not call sites, so scanning
	// for E8 calls to this address keeps working after the detour below.
	Filmmaker::ViewFxVm_NoteViewmodelHelper(viewmodelAddr);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    DetourAttach(&(PVOID&)g_OriginalViewmodelFunc, setViewmodel);
	DetourAttach(&(PVOID&)g_OriginalHandFunc, setHand);

	if(NO_ERROR != DetourTransactionCommit()) {
		ErrorBox("Failed to detour Viewmodel functions.");
		return;
	};

	g_MirvViewmodel.hooked = true;
};

void ViewModel_Console(advancedfx::ICommandArgs* args)
{
	int argc = args->ArgC();
	const auto cmd = args->ArgV(0);

	if (argc > 1) {
		char const* subcmd = args->ArgV(1);
		if (0 == _stricmp("enabled", subcmd) ) {
			if (3 == argc) {
				g_MirvViewmodel.enabled = 0 != atoi(args->ArgV(2));
				return;
			};
			advancedfx::Message(
				"%s enabled 0|1 - enable (1) / disable (0) custom viewmodel.\n"
				"Current value: %s\n"
				, cmd
				, g_MirvViewmodel.enabled ? "1 (enabled)" : "0 (disabled)"
			);
		} else
		if (0 == _stricmp("set", subcmd)) {
			if (argc == 7) {
				bool setX = 0 != strcmp("*", args->ArgV(2));
				bool setY = 0 != strcmp("*", args->ArgV(3));
				bool setZ = 0 != strcmp("*", args->ArgV(4));
				bool setFOV = 0 != strcmp("*", args->ArgV(5));
				bool setLeftHanded = 0 != strcmp("*", args->ArgV(6));
				g_MirvViewmodel.setViewmodel(
					atof(args->ArgV(2)),
					atof(args->ArgV(3)),
					atof(args->ArgV(4)),
					atof(args->ArgV(5)),
					atoi(args->ArgV(6)) == 0 ? false : true,
					setX,
					setY,
					setZ,
					setFOV,
					setLeftHanded
				);
				return;
			};
			advancedfx::Message(
				"%s set <OffsetX|*> <OffsetY|*> <OffsetZ|*> <FOV|*> <Hand|*>\n"
				"Set viewmodel. Use * to indicate to not change (passthrough).\n"
				"Hand: 0 = Right Handed, 1 = Left Handed\n"
				"Current value: OffsetX: %.1f, OffsetY: %.1f, OffsetZ: %.1f, FOV: %.1f, Hand: %i\n"
				"Example:\n"
				"%s set 2 2.5 -2 68 0\n"
				, cmd
				, g_MirvViewmodel.offsetX
				, g_MirvViewmodel.offsetY
				, g_MirvViewmodel.offsetZ
				, g_MirvViewmodel.fovValue
				, g_MirvViewmodel.leftHandedValue
				, cmd
			);
		};
	} else {
		advancedfx::Message(
			"%s enabled <0|1> - enable (1) / disable (0) custom viewmodel\n"
			"%s set <OffsetX|*> <OffsetY|*> <OffsetZ|*> <FOV|*> <Hand|*>\n"
			"Set viewmodel. Use * to indicate passthrough, which means value will depend on engine.\n"
			"Hand: 0 = Right Handed, 1 = Left Handed\n"
			"\n"
			"Example 1 - set custom viewmodel\n"
			"%s set 2 2.5 -2 68 0\n"
			"%s enabled 1\n"
			"\n"
			"Example 2 - set custom viewmodel partially\n"
			"%s set 2 2.5 -2 68 *\n"
			"%s enabled 1\n"
			"Note the * in the end, it means in this case right/left hand state will depend on engine.\n"
			, cmd, cmd, cmd, cmd, cmd, cmd	
		);
	};
};

CON_COMMAND(mirv_viewmodel, "Set custom viewmodel")
{
	ViewModel_Console(args);
};
