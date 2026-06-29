#pragma once

// Self-service debug logger for SOURCE:MVM. The implementation remains in the Cosmetics folder for
// source compatibility with the original cosmetics-only logger, but the log now covers the complete
// filmmaker command/UI surface, console output, camera state, and cosmetics runtime events.
//
// Console: `mvm_debug start` / `mvm_debug stop|status` (short alias) OR
//          `mirv_filmmaker cosmetics debuglog start|stop|status`.
// Identical events are written once per session. A repeat summary records the count and first/last
// occurrence instead of writing one line per frame, even when different events occur in between.
// Logs are written to %APPDATA%\HLAE\debuglogs\mvm_debug_<YYYYMMDD_HHMMSS>.log.

#include <cstdint>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

struct MvmDebugLogStats {
	uint64_t eventsReceived = 0;
	uint64_t uniqueEventsWritten = 0;
	uint64_t repeatsCombined = 0;
};

// Begins a new log file. Returns false if one is already running or the file could not be opened.
// On success, outFile (optional) receives the full UTF-8 file path.
bool MvmDebugLog_Start(std::string* outFile);

// Stops the active log. Returns false if none was running. On success, outFolder/outFile (optional)
// receive the UTF-8 folder and file paths (paste the folder into File Explorer).
bool MvmDebugLog_Stop(std::string* outFolder, std::string* outFile,
	MvmDebugLogStats* outStats = nullptr);

// True while a log is open.
bool MvmDebugLog_Active();

// Snapshot of the current compression counters. Safe to call from any thread.
MvmDebugLogStats MvmDebugLog_GetStats();

// Full CS2 tier0 console capture. Registration is deliberately deferred until mvm_debug starts so
// no logging-state APIs run while Windows is initializing tier0/engine2 under the loader lock.
bool MvmConsoleCapture_Start();
void MvmConsoleCapture_Stop();
bool MvmConsoleCapture_Active();

// Appends a categorized event. The category + exact payload form the deduplication identity.
void MvmDebugLog_Linef(const char* category, const char* fmt, ...);

// Logs an engine-tokenized command without losing quoted/space-containing arguments.
void MvmDebugLog_Command(advancedfx::ICommandArgs* args);

// Captures text emitted through the Source 2 advancedfx console wrapper.
void MvmDebugLog_ConsoleLine(const char* level, int devLevel, const char* text);

// Compatibility aliases for the original cosmetics-only call sites.
bool CosmeticsDebugLog_Start(std::string* outFile);
bool CosmeticsDebugLog_Stop(std::string* outFolder, std::string* outFile);
bool CosmeticsDebugLog_Active();
void CosmeticsDebugLog_Linef(const char* fmt, ...);

} // namespace Filmmaker
