// Console command surface for the filmmaker demo browser: mirv_filmmaker.
// Kept in its own translation unit; registers via the CON_COMMAND macro.

#include "FilmmakerDebugCommand.h" // MvmCommandTrace (the mvm_debug post-command state dumper)
#include "Filmmaker.h"
#include "Demo/DemoLibrary.h"
#include "Demo/DemoEntry.h"
#include "Demo/PlayingDemoPath.h"
#include "Panorama/FilmmakerMenu.h"
#include "Panorama/PanoramaBridge.h"
#include "Panorama/CameraTimelineHud.h"
#include "Panorama/CameraEditorHud.h"
#include "Panorama/ConfigHud.h"
#include "Panorama/GraphEditorExperimentHud.h"
#include "Platform/TextEncoding.h"
#include "Movie/CameraPath.h"
#include "Movie/FollowCamera.h"
#include "Movie/ActionCam.h"
#include "Movie/BodyCam.h"
#include "Movie/ViewFx.h"
#include "Movie/ParticleFx.h"
#include "Movie/RagdollFx.h"
#include "Movie/MovieMode.h"
#include "Movie/DemoEndHold.h"
#include "Movie/ThirdPersonCamera.h"
#include "Cosmetics/CosmeticOverrideSystem.h"

#include "../ClientEntitySystem.h"
#include "../WrpConsole.h"
#include "../../shared/AfxConsole.h"

#include <string>
#include <vector>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

std::string FormatDuration(int seconds) {
	if (seconds <= 0)
		return "?";
	char buf[32];
	snprintf(buf, sizeof(buf), "%d:%02d", seconds / 60, seconds % 60);
	return buf;
}

void PrintHelp(const char* cmd) {
	advancedfx::Message(
		"%s scan - (re)scan the CS2 install + your folders for demos.\n"
		"%s list - list discovered demos with their index.\n"
		"%s folders - show the scanned folders.\n"
		"%s addfolder - open a folder picker and add a demo folder.\n"
		"%s removefolder <index> - remove a saved folder (see 'folders').\n"
		"%s watch <index> - play the demo at <index> (see 'list').\n"
		, cmd, cmd, cmd, cmd, cmd, cmd
	);
	advancedfx::Message(
		"Panorama UI:\n"
		"%s ui - open the demos page (while a demo/map is loaded).\n"
		"%s ui_status - show Panorama bridge status.\n"
		"%s ui_rebuild - rebuild the Panorama UI next frame.\n"
		"%s ui_eval <panorama js> - run Panorama JS in the UI context (live REPL).\n"
		"%s ui_context <hex> - (advanced) pin a context CUIPanel* by address.\n"
		, cmd, cmd, cmd, cmd, cmd
	);
	advancedfx::Message(
		"Movie director (in a demo):\n"
		"%s hud [on|off|toggle] - show/hide the camera help/status panel (also F8).\n"
		"   Scroll = cycle First/Third/Free cam; LMB/RMB = next/prev player;\n"
		"   X = X-ray (when not in free cam); Shift+Scroll = cam speed / third-person distance;\n"
		"   Space = pause/resume; Left/Right = skip -/+15s.\n"
		"%s thirdperson on|off|toggle|state / preset front|back|left|right / angles <yaw> <pitch> / distance <30-200>.\n"
		"%s speedbar [on|off|toggle] - inline demo-bar speed buttons (off = native dropdown).\n"
		"%s endhold [on|off|toggle] - hold on the demo's last tick instead of exiting to menu (play again = restart).\n"
		, cmd, cmd, cmd, cmd
	);
	advancedfx::Message(
		"Camera markers / dolly path (in free cam):\n"
		"   K = place, L = delete aimed, F = edit aimed.\n"
		"%s marker [...] - full marker/path control (run for sub-help).\n"
		"%s camtl [...] - camera timeline scrubber (scrub, keys, easing; run for sub-help).\n"
		"%s editor [on|off|toggle] - dedicated CAMERA EDITOR workspace (preview + inspector + graph editor).\n"
		"%s editor scale [on|off|toggle] - TRUE scaled preview viewport (whole frame shrunk, not a crop).\n"
		"%s editor curveeditor [native|graph|timeline|camera|toggle] - bottom editor: native CS2 timeline, graph, or camera timeline.\n"
		"%s editor hud [hidden|game|full|cycle] - game UI behind the editor: hide all, in-game (radar+HP/ammo, no spectator panel), or full.\n"
		"%s editor debug [on|off|toggle] - viewport/HUD debug overlay (window/render-target/viewport numbers; compare vs normal game viewport).\n"
		"%s config [on|off|toggle] - lightweight CONFIG panel (general UI / display settings, no camera tools).\n"
		"%s viewfx roll|bob|sway|deadzone [<0-150>|off] - camera-feel modifiers (strafe roll, camera walk-bob, weapon sway, decoupled-viewmodel aim deadzone), as an intensity percent.\n"
		"%s fx [...] - particle-effect toggles: impacts/tracers/muzzle/blood/explosions/molotov per-category On|Less|Off (run for sub-help; also in the Config panel).\n"
		"%s ragdoll [on|off|toggle|status] - improved 21-body player ragdolls (default off; also in Config).\n"
		"%s bodycam [on|off|toggle] - one-click chest-cam preset on the spectated player (uses the Follow system).\n"
		"%s actioncam [on|off|toggle] - one-click head-mounted GoPro-style preset on the spectated player; 'actioncam fisheye on|off' toggles the lens distortion.\n"
		"%s follow [...] - place and control a Follow / Lock-On camera.\n"
		, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd
	);
}

void DoUiStatus() {
	auto& b = Filmmaker::Menu().Bridge();
	advancedfx::Message(
		"filmmaker Panorama UI:\n"
		"  engine resolved   : %s\n"
		"  RunScript resolved: %s\n"
		"  context panel     : %p %s\n"
		"  built             : %s\n",
		b.HasEngine() ? "yes" : "no",
		b.HasRunScript() ? "yes" : "no",
		b.ContextPanel(),
		b.ContextPanel() ? "" : "(none yet - load a demo/map so the HUD panel exists)",
		Filmmaker::Menu().IsBuilt() ? "yes" : "no");
}

void DoList() {
	auto entries = Filmmaker::Library().Snapshot();
	if (Filmmaker::Library().IsScanning())
		advancedfx::Message("(scan in progress, list may be incomplete)\n");
	if (entries.empty()) {
		advancedfx::Message("No demos found yet. Try 'mirv_filmmaker scan'.\n");
		return;
	}
	advancedfx::Message("idx | map | length | scoreboard | file\n");
	for (size_t i = 0; i < entries.size(); ++i) {
		const Filmmaker::DemoEntry& e = entries[i];
		advancedfx::Message("%zu | %s | %s | %s | %s\n",
			i,
			e.map.empty() ? "?" : e.map.c_str(),
			FormatDuration(e.durationSeconds).c_str(),
			e.hasScoreboard ? "yes" : "no",
			e.fileName.c_str());
	}
	advancedfx::Message("%zu demo(s).\n", entries.size());
}

void DoFolders() {
	advancedfx::Message("install root (always scanned): %s\n",
		Filmmaker::WideToUtf8(Filmmaker::Library().InstallRoot()).c_str());
	auto folders = Filmmaker::Library().Folders();
	if (folders.empty()) {
		advancedfx::Message("no extra folders added.\n");
		return;
	}
	for (size_t i = 0; i < folders.size(); ++i)
		advancedfx::Message("%zu | %s\n", i, Filmmaker::WideToUtf8(folders[i]).c_str());
}

// Offline demo-only cosmetics override. All parsing + application now lives in the Cosmetics
// module (Filmmaker/Cosmetics/*). The override store is keyed by SteamID so cosmetics follow the
// player across pawn recreation / round resets / death / observer switches / demo seeks. See
// Filmmaker::Cosmetics_RunCommand for the full grammar
// (enabled|status|debug|clear|clearPlayer|target|player <steamId> weapon|knife|gloves|agent ...).
void DoCosmetics(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	Filmmaker::Cosmetics_RunCommand(argc, args, cmd);
}

void DoRemoveFolder(const char* arg) {
	int index = atoi(arg);
	auto folders = Filmmaker::Library().Folders();
	if (index < 0 || index >= (int)folders.size()) {
		advancedfx::Warning("mirv_filmmaker: no folder at index %d (see 'folders')\n", index);
		return;
	}
	if (Filmmaker::Library().RemoveFolder(folders[index])) {
		advancedfx::Message("removed folder %s\n", Filmmaker::WideToUtf8(folders[index]).c_str());
		Filmmaker::RequestRescan();
	}
}

} // namespace

CON_COMMAND(mirv_filmmaker, "Browse and play CS2 demos (filmmaker tool).") {
	const char* cmd = args->ArgV(0);
	const int argc = args->ArgC();
	Filmmaker::MvmCommandTrace trace(args);

	if (argc < 2) {
		PrintHelp(cmd);
		return;
	}

	const char* sub = args->ArgV(1);

	if (0 == _stricmp(sub, "scan") || 0 == _stricmp(sub, "rescan")) {
		Filmmaker::RequestRescan();
		advancedfx::Message("mirv_filmmaker: scan requested.\n");
	} else if (0 == _stricmp(sub, "list")) {
		DoList();
	} else if (0 == _stricmp(sub, "folders")) {
		DoFolders();
	} else if (0 == _stricmp(sub, "addfolder")) {
		Filmmaker::RequestAddFolder();
		advancedfx::Message("mirv_filmmaker: opening folder picker...\n");
	} else if (0 == _stricmp(sub, "removefolder")) {
		if (argc < 3) { advancedfx::Warning("usage: %s removefolder <index>\n", cmd); return; }
		DoRemoveFolder(args->ArgV(2));
	} else if (0 == _stricmp(sub, "watch")) {
		if (argc < 3) { advancedfx::Warning("usage: %s watch <index>\n", cmd); return; }
		Filmmaker::Watch((size_t)atoi(args->ArgV(2)));
	} else if (0 == _stricmp(sub, "ui")) {
		Filmmaker::Menu().RequestShow();
		advancedfx::Message("mirv_filmmaker: opening demos page (must be in a demo/map).\n");
	} else if (0 == _stricmp(sub, "demoprobe")) {
		// Diagnostic: dump the demo-path candidates the engine-scan finds for the playing demo,
		// plus the final resolved path. Used to verify / tune ResolvePlayingDemoPath().
		Filmmaker::DebugProbePlayingDemoPath();
	} else if (0 == _stricmp(sub, "ui_status")) {
		DoUiStatus();
	} else if (0 == _stricmp(sub, "ui_context")) {
		if (argc < 3) { advancedfx::Warning("usage: %s ui_context <hex>\n", cmd); return; }
		Filmmaker::Menu().Bridge().SetContextPanel((void*)(uintptr_t)strtoull(args->ArgV(2), nullptr, 16));
		Filmmaker::Menu().RequestRebuild();
		DoUiStatus();
	} else if (0 == _stricmp(sub, "ui_rebuild")) {
		Filmmaker::Menu().RequestRebuild();
		advancedfx::Message("mirv_filmmaker: UI rebuild requested.\n");
	} else if (0 == _stricmp(sub, "ui_eval")) {
		if (argc < 3) { advancedfx::Warning("usage: %s ui_eval <panorama js>\n", cmd); return; }
		std::string js;
		for (int i = 2; i < argc; ++i) { if (i > 2) js += ' '; js += args->ArgV(i); }
		Filmmaker::Menu().RequestEval(js);
		advancedfx::Message("mirv_filmmaker: queued ui_eval (%zu chars).\n", js.size());
	} else if (0 == _stricmp(sub, "hud")) {
		// Show/hide the in-game movie-director help/status panel.
		// Bind it to a key, e.g.  bind "h" "mirv_filmmaker hud toggle"
		const char* arg = (argc >= 3) ? args->ArgV(2) : "toggle";
		if (0 == _stricmp(arg, "on") || 0 == _stricmp(arg, "1")) Filmmaker::MovieHud_Set(true);
		else if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "0")) Filmmaker::MovieHud_Set(false);
		else Filmmaker::MovieHud_Toggle();
		advancedfx::Message("mirv_filmmaker: movie HUD %s.\n", Filmmaker::MovieHud_Visible() ? "shown" : "hidden");
	} else if (0 == _stricmp(sub, "hud_eval")) {
		if (argc < 3) { advancedfx::Warning("usage: %s hud_eval <panorama js>\n", cmd); return; }
		std::string js;
		for (int i = 2; i < argc; ++i) { if (i > 2) js += ' '; js += args->ArgV(i); }
		Filmmaker::MovieHud_Eval(js);
		advancedfx::Message("mirv_filmmaker: queued hud_eval (%zu chars).\n", js.size());
	} else if (0 == _stricmp(sub, "speedbar")) {
		// Inline demo-bar speed buttons; off restores the native timescale dropdown.
		const char* arg = (argc >= 3) ? args->ArgV(2) : "toggle";
		if (0 == _stricmp(arg, "on") || 0 == _stricmp(arg, "1")) Filmmaker::DemoSpeedBar_Set(true);
		else if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "0")) Filmmaker::DemoSpeedBar_Set(false);
		else Filmmaker::DemoSpeedBar_Toggle();
		advancedfx::Message("mirv_filmmaker: demo speed buttons %s.\n", Filmmaker::DemoSpeedBar_Enabled() ? "on" : "off");
	} else if (0 == _stricmp(sub, "endhold")) {
		// End-of-demo hold: pause on the last tick instead of exiting to the main menu;
		// pressing play while held restarts the demo from the beginning. Default: on.
		const char* arg = (argc >= 3) ? args->ArgV(2) : "toggle";
		if (0 == _stricmp(arg, "on") || 0 == _stricmp(arg, "1")) Filmmaker::DemoEndHold_SetEnabled(true);
		else if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "0")) Filmmaker::DemoEndHold_SetEnabled(false);
		else Filmmaker::DemoEndHold_SetEnabled(!Filmmaker::DemoEndHold_Enabled());
		advancedfx::Message("mirv_filmmaker: end-of-demo hold %s.\n", Filmmaker::DemoEndHold_Enabled() ? "on" : "off");
	} else if (0 == _stricmp(sub, "thirdperson")) {
		Filmmaker::ThirdPerson_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "marker")) {
		Filmmaker::Marker_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "camtl")) {
		Filmmaker::CameraTimeline_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "follow")) {
		Filmmaker::Follow_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "cosmetics")) {
		DoCosmetics(argc, args, cmd);
	} else if (0 == _stricmp(sub, "grapheditor")) {
		Filmmaker::GraphEditor_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "editor")) {
		// Dedicated camera-editor workspace. Bind it to a key, e.g.
		//   bind "F9" "mirv_filmmaker editor toggle"
		Filmmaker::CameraEditor_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "config")) {
		// Lightweight CONFIG panel: general UI / game-display settings only (no camera tools).
		Filmmaker::ConfigHud_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "viewfx")) {
		// Camera "feel" modifiers: Quake/Doom-style strafe roll + weapon sway/bob.
		Filmmaker::ViewFx_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "fx")) {
		// Particle-effect modifiers: per-category On/Less/Off over impacts, tracers, muzzle
		// fx, blood, explosions, molotov, map ambience (runtime particle-create hook).
		Filmmaker::ParticleFx_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "ragdoll")) {
		Filmmaker::RagdollFx_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "bodycam")) {
		// One-click chest-cam preset over the existing Follow/Attach system.
		Filmmaker::BodyCam_RunCommand(argc, args, cmd);
	} else if (0 == _stricmp(sub, "actioncam")) {
		// One-click head-mounted "GoPro" preset (Follow/Attach) + optional fisheye lens pass.
		Filmmaker::ActionCam_RunCommand(argc, args, cmd);
	} else {
		PrintHelp(cmd);
	}
}
