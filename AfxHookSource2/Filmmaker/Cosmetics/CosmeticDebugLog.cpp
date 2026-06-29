#include "CosmeticDebugLog.h"

#include "../../hlaeFolder.h"          // GetHlaeRoamingAppDataFolderW
#include "../../MirvTime.h"
#include "../Platform/TextEncoding.h"  // WideToUtf8
#include "../../../shared/AfxConsole.h"

// Isolated translation unit so <windows.h> (GetLocalTime / CreateDirectoryW) does not leak its
// min/max macros into the SDK-heavy cosmetics files. NOMINMAX/LEAN keep it tidy regardless.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdarg>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

namespace Filmmaker {

namespace {
FILE* g_file = nullptr;
std::wstring g_folderW;
std::wstring g_fileW;

struct EventMeta {
	SYSTEMTIME time = {};
	DWORD threadId = 0;
	int tick = -1;
	int frame = 0;
	uint64_t sequence = 0;
};

struct PendingEvent {
	std::string category;
	std::string message;
	EventMeta first;
	EventMeta last;
	uint64_t occurrences = 0;
};

std::mutex g_mutex;
std::atomic<bool> g_active(false);
std::map<std::string, PendingEvent> g_eventsByIdentity;
MvmDebugLogStats g_stats;
uint64_t g_sequence = 0;

EventMeta CaptureMetaLocked() {
	EventMeta out;
	GetLocalTime(&out.time);
	out.threadId = GetCurrentThreadId();
	int tick = -1;
	if (g_MirvTime.GetCurrentDemoTick(tick))
		out.tick = tick;
	out.frame = g_MirvTime.framecount_get();
	out.sequence = ++g_sequence;
	return out;
}

void PrintMetaLocked(const EventMeta& meta, const char* category) {
	fprintf(g_file,
		"[%02d:%02d:%02d.%03d] [seq=%06llu] [%s] [tick=%d frame=%d thread=%lu] ",
		meta.time.wHour, meta.time.wMinute, meta.time.wSecond, meta.time.wMilliseconds,
		(unsigned long long)meta.sequence, category, meta.tick, meta.frame,
		(unsigned long)meta.threadId);
}

void FlushRepeatLocked(PendingEvent& pending) {
	if (!g_file || pending.occurrences <= 1)
		return;
	PrintMetaLocked(pending.last, "repeat");
	fprintf(g_file,
		"category=%s repeated=%llu total=%llu first=%02d:%02d:%02d.%03d seq=%llu "
		"last=%02d:%02d:%02d.%03d seq=%llu tick=%d..%d frame=%d..%d\n",
		pending.category.c_str(),
		(unsigned long long)(pending.occurrences - 1),
		(unsigned long long)pending.occurrences,
		pending.first.time.wHour, pending.first.time.wMinute, pending.first.time.wSecond,
		pending.first.time.wMilliseconds, (unsigned long long)pending.first.sequence,
		pending.last.time.wHour, pending.last.time.wMinute, pending.last.time.wSecond,
		pending.last.time.wMilliseconds, (unsigned long long)pending.last.sequence,
		pending.first.tick, pending.last.tick, pending.first.frame, pending.last.frame);
}

void FlushAllRepeatsLocked() {
	for (auto& entry : g_eventsByIdentity)
		FlushRepeatLocked(entry.second);
}

std::string FormatMessage(const char* fmt, va_list args) {
	va_list sizeArgs;
	va_copy(sizeArgs, args);
	const int required = _vscprintf(fmt, sizeArgs);
	va_end(sizeArgs);
	if (required < 0)
		return "<formatting failed>";
	std::string out((size_t)required + 1, '\0');
	vsnprintf(&out[0], out.size(), fmt, args);
	out.resize((size_t)required);
	return out;
}

std::string SingleLine(const char* text) {
	std::string out;
	if (!text)
		return out;
	for (const char* p = text; *p; ++p) {
		if (*p == '\r')
			continue;
		if (*p == '\n') {
			if (!out.empty() && out.back() != ' ')
				out += " | ";
			continue;
		}
		out += *p;
	}
	while (!out.empty() && (out.back() == ' ' || out.back() == '\t'))
		out.pop_back();
	return out;
}

void LineV(const char* category, const char* fmt, va_list args) {
	if (!g_active.load(std::memory_order_acquire))
		return;
	std::string message = FormatMessage(fmt, args);
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_file)
		return;

	const EventMeta meta = CaptureMetaLocked();
	++g_stats.eventsReceived;
	const char* safeCategory = category ? category : "event";
	std::string identity(safeCategory);
	identity.push_back('\n');
	identity += message;
	auto found = g_eventsByIdentity.find(identity);
	if (found != g_eventsByIdentity.end()) {
		PendingEvent& pending = found->second;
		pending.last = meta;
		++pending.occurrences;
		++g_stats.repeatsCombined;
		return;
	}

	PendingEvent& pending = g_eventsByIdentity[identity];
	pending.category = safeCategory;
	pending.message = message;
	pending.first = meta;
	pending.last = meta;
	pending.occurrences = 1;

	PrintMetaLocked(meta, safeCategory);
	fprintf(g_file, "%s\n", message.c_str());
	fflush(g_file);
	++g_stats.uniqueEventsWritten;
}
}

bool MvmDebugLog_Active() { return g_active.load(std::memory_order_acquire); }

bool MvmDebugLog_Start(std::string* outFile) {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_file)
		return false;

	std::wstring folder = GetHlaeRoamingAppDataFolderW();
	if (!folder.empty() && folder.back() != L'\\' && folder.back() != L'/')
		folder += L'\\';
	folder += L"debuglogs";
	CreateDirectoryW(folder.c_str(), nullptr); // ok if it already exists

	SYSTEMTIME st;
	GetLocalTime(&st);
	wchar_t name[160];
	swprintf(name, 160, L"\\mvm_debug_%04d%02d%02d_%02d%02d%02d.log",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

	g_folderW = folder;
	g_fileW = folder + name;
	g_file = _wfopen(g_fileW.c_str(), L"w");
	if (!g_file)
		return false;

	g_eventsByIdentity.clear();
	g_stats = {};
	g_sequence = 0;
	fprintf(g_file, "=== SOURCE:MVM debug log started %04d-%02d-%02d %02d:%02d:%02d ===\n",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	fprintf(g_file,
		"line format: [time] [sequence] [category] [demo tick, engine frame, thread] message\n"
		"compression: identical category+payload events are written once per session; repeat summaries include count and time range\n\n");
	fflush(g_file);
	g_active.store(true, std::memory_order_release);

	if (outFile)
		*outFile = WideToUtf8(g_fileW);
	return true;
}

bool MvmDebugLog_Stop(std::string* outFolder, std::string* outFile, MvmDebugLogStats* outStats) {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_file)
		return false;
	g_active.store(false, std::memory_order_release);
	FlushAllRepeatsLocked();

	SYSTEMTIME st;
	GetLocalTime(&st);
	fprintf(g_file,
		"\n=== stopped %02d:%02d:%02d events=%llu unique=%llu repeats_combined=%llu ===\n",
		st.wHour, st.wMinute, st.wSecond,
		(unsigned long long)g_stats.eventsReceived,
		(unsigned long long)g_stats.uniqueEventsWritten,
		(unsigned long long)g_stats.repeatsCombined);
	fclose(g_file);
	g_file = nullptr;

	if (outFolder)
		*outFolder = WideToUtf8(g_folderW);
	if (outFile)
		*outFile = WideToUtf8(g_fileW);
	if (outStats)
		*outStats = g_stats;
	return true;
}

MvmDebugLogStats MvmDebugLog_GetStats() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_stats;
}

void MvmDebugLog_Linef(const char* category, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	LineV(category, fmt, ap);
	va_end(ap);
}

void MvmDebugLog_Command(advancedfx::ICommandArgs* args) {
	if (!args || !MvmDebugLog_Active())
		return;
	std::ostringstream command;
	for (int i = 0; i < args->ArgC(); ++i) {
		if (i > 0)
			command << ' ';
		const char* arg = args->ArgV(i);
		const bool quote = arg && (std::strchr(arg, ' ') || std::strchr(arg, '\t') || std::strchr(arg, '"'));
		if (quote)
			command << '"';
		for (const char* p = arg ? arg : ""; *p; ++p) {
			if (*p == '"' || *p == '\\')
				command << '\\';
			command << *p;
		}
		if (quote)
			command << '"';
	}
	MvmDebugLog_Linef("command", "%s", command.str().c_str());
}

void MvmDebugLog_ConsoleLine(const char* level, int devLevel, const char* text) {
	if (!MvmDebugLog_Active())
		return;
	const std::string line = SingleLine(text);
	if (line.empty())
		return;
	char category[48];
	if (devLevel >= 0)
		snprintf(category, sizeof(category), "console.%s.dev%d", level ? level : "message", devLevel);
	else
		snprintf(category, sizeof(category), "console.%s", level ? level : "message");
	MvmDebugLog_Linef(category, "%s", line.c_str());
}

bool CosmeticsDebugLog_Start(std::string* outFile) {
	return MvmDebugLog_Start(outFile);
}

bool CosmeticsDebugLog_Stop(std::string* outFolder, std::string* outFile) {
	return MvmDebugLog_Stop(outFolder, outFile);
}

bool CosmeticsDebugLog_Active() {
	return MvmDebugLog_Active();
}

void CosmeticsDebugLog_Linef(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	LineV("cosmetics", fmt, ap);
	va_end(ap);
}

} // namespace Filmmaker
