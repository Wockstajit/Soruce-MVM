// Console command surface for the filmmaker demo browser: mirv_filmmaker.
// Kept in its own translation unit; registers via the CON_COMMAND macro.

#include "Filmmaker.h"
#include "Demo/DemoLibrary.h"
#include "Demo/DemoEntry.h"
#include "Demo/PlayingDemoPath.h"
#include "Panorama/FilmmakerMenu.h"
#include "Panorama/PanoramaBridge.h"
#include "Panorama/CameraTimelineHud.h"
#include "Panorama/GraphEditorExperimentHud.h"
#include "Platform/TextEncoding.h"
#include "Movie/CameraBridge.h"
#include "Movie/CameraPath.h"
#include "Movie/MovieMode.h"

#include "../WrpConsole.h"
#include "../../shared/AfxConsole.h"

#include <string>
#include <vector>
#include <cstdlib>
#include <cstddef>
#include <cstdint>

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
		"   X = X-ray (when not in free cam); Shift+Scroll = cam speed;\n"
		"   Space = pause/resume; Left/Right = skip -/+15s.\n"
		"%s speedbar [on|off|toggle] - inline demo-bar speed buttons (off = native dropdown).\n"
		, cmd, cmd
	);
	advancedfx::Message(
		"Camera markers / dolly path (in free cam):\n"
		"   K = place, L = delete aimed, F = edit aimed.\n"
		"%s marker [...] - full marker/path control (run for sub-help).\n"
		"%s camtl [...] - camera timeline scrubber (scrub, keys, easing; run for sub-help).\n"
		"%s editor [on|off|toggle] - dedicated CAMERA EDITOR workspace (preview + inspector + graph editor).\n"
		"%s editor scale [on|off|toggle] - TRUE scaled preview viewport (whole frame shrunk, not a crop).\n"
		"%s editor curveeditor [graph|timeline|toggle] - bottom editor: graph (default) or timeline.\n"
		, cmd, cmd, cmd, cmd, cmd
	);
}

bool RegularCursorToggleAllowed() {
	using Mode = Filmmaker::MovieMode::Mode;
	const Mode mode = Filmmaker::MovieModeRef().GetMode();
	return mode == Mode::ThirdPerson || mode == Mode::FreeCam;
}

void PlayCameraTimeline(Filmmaker::CameraPath& cp) {
	if (Filmmaker::CameraEditor_Active())
		cp.PlayFromEditor();
	else
		cp.PlayFromTimeline();
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

// Camera-marker / dolly path subcommands (mirv_filmmaker marker ...). Mirrors the
// BO2 Theater dolly workflow; also the back-end the K/J/L/F hotkeys + the Panorama
// marker menu issue. All actions run on the main thread (console dispatch).
void DoMarker(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	using CP = Filmmaker::CameraPath;
	Filmmaker::CameraPath& cp = Filmmaker::CameraPathRef();

	if (argc < 3) {
		advancedfx::Message(
			"%s marker place - add a marker at the current camera pose (also K).\n"
			"%s marker preview|arm - jump to first marker and arm playback.\n"
			"%s marker play|previewplay - play the armed path.\n"
			"%s marker previewstop - stop playback (X).\n"
			"%s marker delete <i> | deleteall confirm - remove markers (L = aimed).\n"
			"%s marker select <i> | next | prev - select (menu arrows teleport).\n"
			"%s marker edit <i> | close - open/close the settings menu (F = aimed).\n"
			"%s marker reposition | repositionplace | repositioncancel - move the\n"
			"   selected marker (left-click places, X/Esc cancels).\n"
			"%s marker speedmode manual|constant|persegment|cycle.\n"
			"%s marker interp linear|bezier|cycle  (path shape, separate from speed).\n"
			"%s marker timing live|freeze|toggle  (global - all cameras).\n"
			"%s marker speedmul <0.2..1.0> - selected marker's OUTGOING segment speed.\n"
			"%s marker constspeed <0.2..1.0> | cycle - global Constant-mode speed.\n"
			"%s marker autosnap on|off|toggle - snap viewer to a marker when selected.\n"
			"%s marker list | save | load.\n",
			cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
		return;
	}

	const char* a = args->ArgV(2);
	const char* a3 = (argc >= 4) ? args->ArgV(3) : "";

	if (0 == _stricmp(a, "place")) cp.PlaceMarker();
	else if (0 == _stricmp(a, "preview") || 0 == _stricmp(a, "arm")) cp.ArmPreview();
	else if (0 == _stricmp(a, "play") || 0 == _stricmp(a, "previewplay")) cp.StartPreviewPlay();
	else if (0 == _stricmp(a, "previewstop") || 0 == _stricmp(a, "stop")) cp.StopPreview();
	else if (0 == _stricmp(a, "hudtoggle")) advancedfx::Message("mirv_filmmaker: camera path HUD toggle is disabled.\n");
	else if (0 == _stricmp(a, "repositionplace")) cp.PlaceReposition();
	else if (0 == _stricmp(a, "repositioncancel")) cp.CancelReposition();
	else if (0 == _stricmp(a, "delete")) {
		if (argc < 4) { advancedfx::Warning("usage: %s marker delete <i>\n", cmd); return; }
		cp.DeleteIndex(atoi(a3));
	}
	else if (0 == _stricmp(a, "deleteall")) cp.DeleteAll(argc >= 4 && 0 == _stricmp(a3, "confirm"));
	else if (0 == _stricmp(a, "select")) {
		if (argc < 4) { advancedfx::Warning("usage: %s marker select <i>\n", cmd); return; }
		cp.SelectIndex(atoi(a3), /*teleport*/ true);
	}
	else if (0 == _stricmp(a, "next")) cp.SelectDelta(+1);
	else if (0 == _stricmp(a, "prev")) cp.SelectDelta(-1);
	else if (0 == _stricmp(a, "edit")) {
		if (argc < 4) { advancedfx::Warning("usage: %s marker edit <i>\n", cmd); return; }
		cp.OpenMenu(atoi(a3));
	}
	else if (0 == _stricmp(a, "close") || 0 == _stricmp(a, "done")) cp.CloseMenu();
	else if (0 == _stricmp(a, "reposition")) cp.BeginReposition();
	else if (0 == _stricmp(a, "speedmode")) {
		if (0 == _stricmp(a3, "manual")) cp.SetSpeedMode(CP::SpeedMode::Manual);
		else if (0 == _stricmp(a3, "constant")) cp.SetSpeedMode(CP::SpeedMode::Constant);
		else if (0 == _stricmp(a3, "persegment")) cp.SetSpeedMode(CP::SpeedMode::PerSegment);
		else cp.CycleSpeedMode();
	}
	else if (0 == _stricmp(a, "interp")) {
		if (0 == _stricmp(a3, "linear")) cp.SetInterp(CP::Interp::Linear);
		else if (0 == _stricmp(a3, "bezier")) cp.SetInterp(CP::Interp::Bezier);
		else cp.CycleInterp();
	}
	else if (0 == _stricmp(a, "timing")) {
		if (0 == _stricmp(a3, "live")) cp.SetTiming(CP::Timing::Live);
		else if (0 == _stricmp(a3, "freeze")) cp.SetTiming(CP::Timing::Freeze);
		else cp.ToggleTiming();
	}
	else if (0 == _stricmp(a, "speedmul")) {
		if (argc < 4) { advancedfx::Warning("usage: %s marker speedmul <0.2..1.0>\n", cmd); return; }
		cp.SetSelectedSpeedMul((float)atof(a3));
	}
	else if (0 == _stricmp(a, "constspeed")) {
		if (argc >= 4 && 0 == _stricmp(a3, "cycle")) cp.CycleConstSpeed();
		else if (argc >= 4) cp.SetConstSpeed((float)atof(a3));
		else cp.CycleConstSpeed();
	}
	else if (0 == _stricmp(a, "autosnap")) {
		if (0 == _stricmp(a3, "on")) cp.SetAutoSnap(true);
		else if (0 == _stricmp(a3, "off")) cp.SetAutoSnap(false);
		else cp.ToggleAutoSnap();
	}
	else if (0 == _stricmp(a, "list")) {
		advancedfx::Message("[campath] %d marker(s), selected #%d, speed=%s, timing=%s, interp=%s.\n",
			cp.Count(), cp.Selected(), cp.SpeedModeName(), cp.TimingName(), cp.InterpName());
	}
	else if (0 == _stricmp(a, "save")) cp.Save();
	else if (0 == _stricmp(a, "load")) cp.Load();
	else advancedfx::Warning("%s marker: unknown action '%s'\n", cmd, a);
}

// Camera timeline subcommands (mirv_filmmaker camtl ...). The
// Panorama panel (CameraTimelineHud) issues these; they also work from the
// console. Panel show/hide go to the HUD; data ops go to CameraPath.
void DoCamTimeline(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	using CP = Filmmaker::CameraPath;
	Filmmaker::CameraTimelineHud& tl = Filmmaker::CameraTimelineHudRef();
	Filmmaker::CameraPath& cp = Filmmaker::CameraPathRef();

	if (argc < 3) {
		advancedfx::Message(
			"%s camtl open|close|toggle - show/hide the camera timeline panel.\n"
			"%s camtl scrub <tick> - tick-perfect scrub to <tick> (paused).\n"
			"%s camtl play - editor: seek to playhead and play; timeline: play from first marker.\n"
			"%s camtl playtest - seek to the current editor tick and play immediately.\n"
			"%s camtl playpath - play demo normally; Live dolly engages inside marker range.\n"
			"%s camtl pause - pause camera-path playback at the current tick.\n"
			"%s camtl stop - stop any lingering path playback and scrub.\n"
			"%s camtl addkey | delkey <i> | movekey <i> <tick>.\n"
			"%s camtl setval <i> <ch 0..6> <v> - 0=x 1=y 2=z 3=pitch 4=yaw 5=tilt 6=fov.\n"
			"%s camtl undo - undo the last curve value/retime edit (Ctrl+Z).\n"
			"%s camtl ease <i> none|in|out|inout  |  speed <i> <0.2..1.0>.\n"
			"%s camtl interp linear|cubic|cycle.\n"
			"%s camtl cursor on|off|toggle - regular UI-mouse mode (third-person/freecam; forced on while editor is open).\n"
			"%s camtl clear - remove ALL keyframes.\n"
			"%s camtl eval <panorama js>.\n",
			cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
		return;
	}

	const char* a = args->ArgV(2);
	const char* a3 = (argc >= 4) ? args->ArgV(3) : "";
	const char* a4 = (argc >= 5) ? args->ArgV(4) : "";
	const char* a5 = (argc >= 6) ? args->ArgV(5) : "";

	if (0 == _stricmp(a, "open")) {
		tl.SetVisible(true);
		Filmmaker::CameraBridge_SetFreeCamEnabled(true);
		if (cp.Count() > 0) cp.SelectForEditor(cp.Selected() >= 0 ? cp.Selected() : 0);
		advancedfx::Message("mirv_filmmaker: camera timeline shown (must be in a demo).\n");
	}
	else if (0 == _stricmp(a, "close")) { tl.SetVisible(false); cp.StopScrub(); }
	else if (0 == _stricmp(a, "toggle")) {
		const bool opening = !tl.Visible();
		tl.Toggle();
		if (opening) {
			Filmmaker::CameraBridge_SetFreeCamEnabled(true);
			if (cp.Count() > 0) cp.SelectForEditor(cp.Selected() >= 0 ? cp.Selected() : 0);
		}
		else if (!opening) cp.StopScrub();
	}
	else if (0 == _stricmp(a, "view")) {
		tl.SetView(0);
		if (0 == _stricmp(a3, "curve"))
			advancedfx::Warning("%s camtl view curve was removed; use %s editor curveeditor graph.\n", cmd, cmd);
	}
	else if (0 == _stricmp(a, "scrub")) { // slider release: seek the world too
		if (argc < 4) { advancedfx::Warning("usage: %s camtl scrub <tick>\n", cmd); return; }
		cp.ScrubToTick(atof(a3), true);
	}
	else if (0 == _stricmp(a, "scrubpreview")) { // slider drag: glide the camera only (no seek)
		if (argc < 4) { advancedfx::Warning("usage: %s camtl scrubpreview <tick>\n", cmd); return; }
		cp.ScrubToTick(atof(a3), false);
	}
	else if (0 == _stricmp(a, "select")) {
		if (argc < 4) { advancedfx::Warning("usage: %s camtl select <i>\n", cmd); return; }
		cp.SelectForEditor(atoi(a3));
	}
	else if (0 == _stricmp(a, "selectdelta")) {
		cp.SelectEditorDelta((argc >= 4) ? atoi(a3) : 1);
	}
	else if (0 == _stricmp(a, "play")) PlayCameraTimeline(cp);
	else if (0 == _stricmp(a, "playtest")) cp.PlayFromEditor();
	else if (0 == _stricmp(a, "playtimeline")) cp.PlayFromTimeline();
	else if (0 == _stricmp(a, "playpath")) cp.PlayPath();
	else if (0 == _stricmp(a, "pause")) cp.PausePreview();
	else if (0 == _stricmp(a, "debug")) {
		bool on = (argc >= 4) ? (atoi(a3) != 0) : !cp.Debug();
		cp.SetDebug(on);
		advancedfx::Warning("%s camtl debug %d ([campath]/[setupview] instrumentation %s).\n",
			cmd, on ? 1 : 0, on ? "ON" : "OFF");
	}
	else if (0 == _stricmp(a, "stop")) { cp.StopPreview(); cp.StopScrub(); }
	else if (0 == _stricmp(a, "addkey")) cp.PlaceMarker();
	else if (0 == _stricmp(a, "delkey")) {
		if (argc < 4) { advancedfx::Warning("usage: %s camtl delkey <i>\n", cmd); return; }
		cp.DeleteIndex(atoi(a3));
	}
	else if (0 == _stricmp(a, "movekey")) {
		if (argc < 5) { advancedfx::Warning("usage: %s camtl movekey <i> <tick>\n", cmd); return; }
		cp.MoveKey(atoi(a3), atoi(a4));
	}
	else if (0 == _stricmp(a, "setval")) {
		if (argc < 6) { advancedfx::Warning("usage: %s camtl setval <i> <ch> <v>\n", cmd); return; }
		cp.SetChannelValue(atoi(a3), atoi(a4), atof(a5));
	}
	else if (0 == _stricmp(a, "editbegin")) cp.BeginCurveValueEdit();
	else if (0 == _stricmp(a, "setvalpreview")) {
		if (argc < 6) { advancedfx::Warning("usage: %s camtl setvalpreview <i> <ch> <v>\n", cmd); return; }
		cp.PreviewChannelValue(atoi(a3), atoi(a4), atof(a5));
	}
	else if (0 == _stricmp(a, "editend")) cp.EndCurveValueEdit();
	else if (0 == _stricmp(a, "undo")) cp.UndoCurveEdit();
	else if (0 == _stricmp(a, "redo")) cp.RedoCurveEdit();
	else if (0 == _stricmp(a, "ease")) {
		if (argc < 5) { advancedfx::Warning("usage: %s camtl ease <i> none|in|out|inout\n", cmd); return; }
		Filmmaker::Ease e = Filmmaker::Ease::None;
		if (0 == _stricmp(a4, "in")) e = Filmmaker::Ease::In;
		else if (0 == _stricmp(a4, "out")) e = Filmmaker::Ease::Out;
		else if (0 == _stricmp(a4, "inout")) e = Filmmaker::Ease::InOut;
		cp.SetEaseIndex(atoi(a3), e);
	}
	else if (0 == _stricmp(a, "speed")) {
		if (argc < 5) { advancedfx::Warning("usage: %s camtl speed <i> <0.2..1.0>\n", cmd); return; }
		cp.SetSpeedMulIndex(atoi(a3), (float)atof(a4));
	}
	else if (0 == _stricmp(a, "interp")) {
		if (0 == _stricmp(a3, "linear")) cp.SetInterp(CP::Interp::Linear);
		else if (0 == _stricmp(a3, "cubic") || 0 == _stricmp(a3, "smooth") || 0 == _stricmp(a3, "bezier"))
			cp.SetInterp(CP::Interp::Bezier);
		else cp.CycleInterp();
	}
	else if (0 == _stricmp(a, "zoom") || 0 == _stricmp(a, "pan")) {
		advancedfx::Warning("%s camtl %s was removed with the old embedded curve view; use %s editor curveeditor graph.\n", cmd, a, cmd);
	}
	else if (0 == _stricmp(a, "gamehud")) {
		advancedfx::Message("mirv_filmmaker: camera timeline HUD toggle is disabled.\n");
	}
	else if (0 == _stricmp(a, "cursor")) {
		// In Camera Editor Mode the free cam is always on and the cursor is the user's
		// way to flip between clicking the inspector and flying, so allow it regardless
		// of the spectator mode gate.
		if (!tl.EditorHosted() && !RegularCursorToggleAllowed()) {
			tl.SetCursor(false);
			advancedfx::Message("mirv_filmmaker: cursor toggle is available in third-person/freecam.\n");
			return;
		}
		if (tl.CursorForced()) {
			advancedfx::Message("mirv_filmmaker: cursor is forced on while the camera timeline is open.\n");
			return;
		}
		if (0 == _stricmp(a3, "on")) tl.SetCursor(true);
		else if (0 == _stricmp(a3, "off")) tl.SetCursor(false);
		else {
			tl.ToggleCursor();
		}
		// Returning to free-cam look (cursor off) releases any active scrub so the
		// camera stops being pinned to the scrub tick and the user can fly again.
		if (!tl.Cursor()) cp.StopScrub();
	}
	else if (0 == _stricmp(a, "clear")) cp.DeleteAll(true); // remove all keyframes
	else if (0 == _stricmp(a, "eval")) {
		if (argc < 4) { advancedfx::Warning("usage: %s camtl eval <panorama js>\n", cmd); return; }
		std::string js;
		for (int i = 3; i < argc; ++i) { if (i > 3) js += ' '; js += args->ArgV(i); }
		tl.RequestEval(js);
	}
	else advancedfx::Warning("%s camtl: unknown action '%s'\n", cmd, a);
}

// Experimental After-Effects-style graph editor (mirv_filmmaker grapheditor ...). Entirely
// separate from the stable camtl/marker back-end: it drives GraphEditorExperimentHud's own
// isolated model. The JS overlay issues these; they are also bind-able for power users.
void DoGraphEditor(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	auto& ge = Filmmaker::GraphEditorExperimentHudRef();
	if (argc < 3) {
		advancedfx::Message(
			"%s grapheditor on|off|toggle - EXPERIMENTAL AE-style camera graph editor (must be in a demo).\n"
			"%s grapheditor drive on|off|toggle - live-drive the camera from the experimental curves.\n"
			"%s grapheditor reseed | undo | redo | ease in|out|inout [all] | smooth|linear [sel].\n"
			"%s grapheditor chan <0..6> show|hide|solo|unsolo  (0=x 1=y 2=z 3=pitch 4=yaw 5=roll 6=fov).\n"
			"%s grapheditor select <ch> <id> [add] | selset <ch> <id> [<ch> <id> ...] | selclear.\n"
			"%s grapheditor editbegin | movesel <dTick> <dVal> | movekey <ch> <id> <tick> <val>.\n"
			"%s grapheditor setval <ch> <id> <val> | addkey <ch> <tick> <val> | delkey <ch> <id> | delsel.\n"
			"%s grapheditor handle <ch> <id> left|right <tx> <dv> [reflect] | clearhandles <ch> <id>.\n"
			"%s grapheditor playhead <tick> | playhead release | eval <panorama js>.\n",
			cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
		return;
	}
	const char* a = args->ArgV(2);
	const char* a3 = (argc >= 4) ? args->ArgV(3) : "";
	const char* a4 = (argc >= 5) ? args->ArgV(4) : "";
	const char* a5 = (argc >= 6) ? args->ArgV(5) : "";
	const char* a6 = (argc >= 7) ? args->ArgV(6) : "";
	const char* a7 = (argc >= 8) ? args->ArgV(7) : "";
	const char* a8 = (argc >= 9) ? args->ArgV(8) : "";

	if (0 == _stricmp(a, "on") || 0 == _stricmp(a, "open") || 0 == _stricmp(a, "1")) {
		ge.SetEnabled(true);
		advancedfx::Message("mirv_filmmaker: experimental graph editor ON (must be in a demo).\n");
	} else if (0 == _stricmp(a, "off") || 0 == _stricmp(a, "close") || 0 == _stricmp(a, "0")) {
		ge.SetEnabled(false);
		advancedfx::Message("mirv_filmmaker: experimental graph editor off.\n");
	} else if (0 == _stricmp(a, "toggle")) {
		ge.Toggle();
		advancedfx::Message("mirv_filmmaker: experimental graph editor %s.\n", ge.Enabled() ? "ON" : "off");
	} else if (0 == _stricmp(a, "drive")) {
		if (0 == _stricmp(a3, "on") || 0 == _stricmp(a3, "1")) ge.SetDrive(true);
		else if (0 == _stricmp(a3, "off") || 0 == _stricmp(a3, "0")) ge.SetDrive(false);
		else ge.ToggleDrive();
	} else if (0 == _stricmp(a, "reseed")) ge.CmdReseed();
	else if (0 == _stricmp(a, "undo")) ge.CmdUndo();
	else if (0 == _stricmp(a, "redo")) ge.CmdRedo();
	else if (0 == _stricmp(a, "ease")) {
		// Easing preset: in|out|inout. Default = current selection (right-click menu); trailing
		// "all" applies it to every keyframe (the inspector's path-wide Ease button).
		int mode = (0 == _stricmp(a3, "out")) ? 1 : (0 == _stricmp(a3, "inout") ? 2 : 0);
		ge.CmdEase(mode, 0 != _stricmp(a4, "all"));
	} else if (0 == _stricmp(a, "smooth") || 0 == _stricmp(a, "linear")) {
		// Graph-wide Smooth/Linear (its own interp, independent of the camera path). Optional
		// "sel" argument restricts it to the current selection.
		bool sel = (0 == _stricmp(a3, "sel"));
		ge.CmdSetInterp(0 == _stricmp(a, "smooth"), sel);
	} else if (0 == _stricmp(a, "chan")) {
		if (argc < 5) { advancedfx::Warning("usage: %s grapheditor chan <0..6> show|hide|solo|unsolo\n", cmd); return; }
		int op = 1;
		if (0 == _stricmp(a4, "hide")) op = 0; else if (0 == _stricmp(a4, "show")) op = 1;
		else if (0 == _stricmp(a4, "solo")) op = 2; else if (0 == _stricmp(a4, "unsolo")) op = 3;
		ge.CmdChannel(atoi(a3), op);
	} else if (0 == _stricmp(a, "select")) {
		if (argc < 5) { advancedfx::Warning("usage: %s grapheditor select <ch> <id> [add]\n", cmd); return; }
		ge.CmdSelect(atoi(a3), atoi(a4), (argc >= 6 && 0 == _stricmp(a5, "add")));
	} else if (0 == _stricmp(a, "selall")) ge.CmdSelectAll();
	else if (0 == _stricmp(a, "selset")) {
		// Flat space-separated <ch> <id> pairs starting at ArgV(3). Uses the engine tokenizer
		// (one int per token) instead of a single "ch:id,ch:id" token, which the console
		// splits on the comma -- that dropped all but the first pair and broke box-select.
		// First pair replaces the selection (additive=false); the rest extend it.
		bool first = true;
		for (int i = 3; i + 1 < argc; i += 2) {
			ge.CmdSelect(atoi(args->ArgV(i)), atoi(args->ArgV(i + 1)), !first);
			first = false;
		}
	}
	else if (0 == _stricmp(a, "selclear")) ge.CmdSelectClear();
	else if (0 == _stricmp(a, "editbegin")) ge.CmdEditBegin();
	else if (0 == _stricmp(a, "movesel")) {
		if (argc < 5) { advancedfx::Warning("usage: %s grapheditor movesel <dTick> <dVal>\n", cmd); return; }
		ge.CmdMoveSelectedBy(atof(a3), atof(a4));
	} else if (0 == _stricmp(a, "movekey")) {
		if (argc < 7) { advancedfx::Warning("usage: %s grapheditor movekey <ch> <id> <tick> <val>\n", cmd); return; }
		ge.CmdMoveKeyAbs(atoi(a3), atoi(a4), atof(a5), atof(a6));
	} else if (0 == _stricmp(a, "setval")) {
		if (argc < 6) { advancedfx::Warning("usage: %s grapheditor setval <ch> <id> <val>\n", cmd); return; }
		ge.CmdSetValue(atoi(a3), atoi(a4), atof(a5));
	} else if (0 == _stricmp(a, "addkey")) {
		if (argc < 6) { advancedfx::Warning("usage: %s grapheditor addkey <ch> <tick> <val>\n", cmd); return; }
		ge.CmdAddKey(atoi(a3), atof(a4), atof(a5));
	} else if (0 == _stricmp(a, "delkey")) {
		if (argc < 5) { advancedfx::Warning("usage: %s grapheditor delkey <ch> <id>\n", cmd); return; }
		ge.CmdDeleteKey(atoi(a3), atoi(a4));
	} else if (0 == _stricmp(a, "delsel")) ge.CmdDeleteSelected();
	else if (0 == _stricmp(a, "handle")) {
		if (argc < 8) { advancedfx::Warning("usage: %s grapheditor handle <ch> <id> left|right <tx> <dv> [reflect]\n", cmd); return; }
		int side = (0 == _stricmp(a5, "left")) ? -1 : 1;
		bool reflect = (argc >= 9) ? (atoi(a8) != 0) : true;
		ge.CmdSetHandle(atoi(a3), atoi(a4), side, atof(a6), atof(a7), reflect);
	} else if (0 == _stricmp(a, "clearhandles")) {
		if (argc < 5) { advancedfx::Warning("usage: %s grapheditor clearhandles <ch> <id>\n", cmd); return; }
		ge.CmdClearHandles(atoi(a3), atoi(a4));
	} else if (0 == _stricmp(a, "playhead")) {
		if (argc >= 4 && 0 == _stricmp(a3, "release")) ge.CmdPlayhead(0.0, true);
		else if (argc >= 4) ge.CmdPlayhead(atof(a3), false);
		else advancedfx::Warning("usage: %s grapheditor playhead <tick> | release\n", cmd);
	} else if (0 == _stricmp(a, "eval")) {
		if (argc < 4) { advancedfx::Warning("usage: %s grapheditor eval <panorama js>\n", cmd); return; }
		std::string js;
		for (int i = 3; i < argc; ++i) { if (i > 3) js += ' '; js += args->ArgV(i); }
		ge.RequestEval(js);
	} else advancedfx::Warning("%s grapheditor: unknown action '%s'\n", cmd, a);
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
	} else if (0 == _stricmp(sub, "marker")) {
		DoMarker(argc, args, cmd);
	} else if (0 == _stricmp(sub, "camtl")) {
		DoCamTimeline(argc, args, cmd);
	} else if (0 == _stricmp(sub, "grapheditor")) {
		DoGraphEditor(argc, args, cmd);
	} else if (0 == _stricmp(sub, "editor")) {
		// Dedicated camera-editor workspace. Bind it to a key, e.g.
		//   bind "F9" "mirv_filmmaker editor toggle"
		const char* arg = (argc >= 3) ? args->ArgV(2) : "toggle";
		if (0 == _stricmp(arg, "scale")) {
			// TRUE scaled preview viewport (render-layer). Falls back to the crop when off.
			const char* a2 = (argc >= 4) ? args->ArgV(3) : "toggle";
			if (0 == _stricmp(a2, "on") || 0 == _stricmp(a2, "1")) Filmmaker::CameraEditor_SetScale(true);
			else if (0 == _stricmp(a2, "off") || 0 == _stricmp(a2, "0")) Filmmaker::CameraEditor_SetScale(false);
			else Filmmaker::CameraEditor_ToggleScale();
			advancedfx::Message("mirv_filmmaker: camera editor scaled preview %s (auto-off while recording).\n",
				Filmmaker::CameraEditor_ScaleActive() ? "ON" : "off");
		} else if (0 == _stricmp(arg, "curveeditor")) {
			// Pick the bottom editor: graph (default) or the compact camera timeline.
			const char* a2 = (argc >= 4) ? args->ArgV(3) : "toggle";
			if (0 == _stricmp(a2, "timeline")) Filmmaker::CameraEditor_SetUseTimeline(true);
			else if (0 == _stricmp(a2, "graph")) Filmmaker::CameraEditor_SetUseTimeline(false);
			else Filmmaker::CameraEditor_ToggleUseTimeline();
		} else {
			if (0 == _stricmp(arg, "on") || 0 == _stricmp(arg, "open") || 0 == _stricmp(arg, "1")) Filmmaker::CameraEditor_Set(true);
			else if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "close") || 0 == _stricmp(arg, "0")) Filmmaker::CameraEditor_Set(false);
			else Filmmaker::CameraEditor_Toggle();
			advancedfx::Message("mirv_filmmaker: camera editor mode %s (must be in a demo).\n",
				Filmmaker::CameraEditor_Active() ? "ON" : "off");
		}
	} else {
		PrintHelp(cmd);
	}
}
