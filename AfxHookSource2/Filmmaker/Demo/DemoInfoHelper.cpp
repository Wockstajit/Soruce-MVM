#include "DemoInfoHelper.h"

#include "../Platform/JsonParser.h"

#include <Windows.h>
#include <fstream>
#include <sstream>

namespace Filmmaker {

namespace {

// Must match FilmmakerDemoInfo's DemoInfo.SchemaVersion. A cached "<demo>.fmjson"
// whose "v" differs was produced by an older helper and is discarded.
constexpr int kSchemaVersion = 4;

// Directory containing this DLL (HLAE's bin\x64 when injected from staging).
std::wstring SelfModuleDir() {
	HMODULE h = nullptr;
	GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&SelfModuleDir), &h);
	wchar_t buf[MAX_PATH];
	DWORD n = GetModuleFileNameW(h, buf, MAX_PATH);
	std::wstring path(buf, n);
	size_t slash = path.find_last_of(L"\\/");
	return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool FileExists(const std::wstring& p) {
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Locates FilmmakerDemoInfo.exe. An explicit override wins, then a few staging
// layouts relative to this DLL are tried.
std::wstring FindHelperExe() {
	wchar_t over[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(L"MIRV_FILMMAKER_DEMOINFO", over, MAX_PATH);
	if (n > 0 && n < MAX_PATH && FileExists(over))
		return std::wstring(over, n);

	std::wstring dir = SelfModuleDir();
	if (dir.empty())
		return std::wstring();

	const wchar_t* rels[] = {
		L"\\FilmmakerDemoInfo\\FilmmakerDemoInfo.exe", // <dir>\FilmmakerDemoInfo\...
		L"\\FilmmakerDemoInfo.exe",                    // <dir>\FilmmakerDemoInfo.exe
		L"\\..\\FilmmakerDemoInfo\\FilmmakerDemoInfo.exe", // <dir>\..\FilmmakerDemoInfo\...
	};
	for (const wchar_t* rel : rels) {
		std::wstring cand = dir + rel;
		if (FileExists(cand))
			return cand;
	}
	return std::wstring();
}

bool GetWriteTime(const std::wstring& path, ULONGLONG& out) {
	WIN32_FILE_ATTRIBUTE_DATA fad;
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
		return false;
	ULARGE_INTEGER u;
	u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
	u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
	out = u.QuadPart;
	return true;
}

std::string ReadFileUtf8(const std::wstring& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return std::string();
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

void WriteFileUtf8(const std::wstring& path, const std::string& data) {
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (f)
		f.write(data.data(), (std::streamsize)data.size());
}

// Runs exe with one or two arguments, returns its stdout. Empty on failure.
// arg2 is appended (quoted) only when non-empty.
std::string RunCaptureStdout(const std::wstring& exe, const std::wstring& arg, const std::wstring& arg2) {
	SECURITY_ATTRIBUTES sa{};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	HANDLE readPipe = nullptr, writePipe = nullptr;
	if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
		return std::string();
	SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

	std::wstring cmd = L"\"" + exe + L"\" \"" + arg + L"\"";
	if (!arg2.empty())
		cmd += L" \"" + arg2 + L"\"";
	std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
	cmdBuf.push_back(L'\0');

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	si.hStdOutput = writePipe;
	si.hStdError = writePipe;
	si.hStdInput = nullptr;

	PROCESS_INFORMATION pi{};
	BOOL ok = CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	CloseHandle(writePipe); // parent keeps only the read end
	if (!ok) {
		CloseHandle(readPipe);
		return std::string();
	}

	std::string out;
	char buf[4096];
	DWORD got = 0;
	while (ReadFile(readPipe, buf, sizeof(buf), &got, nullptr) && got > 0)
		out.append(buf, got);

	WaitForSingleObject(pi.hProcess, INFINITE);
	CloseHandle(readPipe);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return out;
}

bool ParseJson(const std::string& json, DemoHelperResult& out) {
	JsonValue root;
	if (!JsonParse(json, root) || root.type != JsonValue::Type::Object)
		return false;
	const JsonValue* okv = root.Find("ok");
	if (!okv || !okv->AsBool(false))
		return false;

	out.ok = true;
	if (const JsonValue* v = root.Find("v")) out.schemaVersion = v->AsInt();
	if (const JsonValue* v = root.Find("namesOnly")) out.namesOnly = v->AsBool();
	if (const JsonValue* v = root.Find("map")) out.map = v->AsString();
	if (const JsonValue* v = root.Find("durationSeconds")) out.durationSeconds = v->AsInt();
	if (const JsonValue* v = root.Find("teamScore0")) out.teamScore0 = v->AsInt();
	if (const JsonValue* v = root.Find("teamScore1")) out.teamScore1 = v->AsInt();
	if (const JsonValue* v = root.Find("hasScoreboard")) out.hasScoreboard = v->AsBool();

	if (const JsonValue* players = root.Find("players"); players && players->type == JsonValue::Type::Array) {
		for (const auto& pj : players->arr) {
			if (pj.type != JsonValue::Type::Object) continue;
			ScoreboardPlayer p;
			if (const JsonValue* v = pj.Find("name")) p.name = v->AsString("[unknown]");
			if (const JsonValue* v = pj.Find("accountId")) p.accountId = (uint32_t)v->AsNumber();
			if (const JsonValue* v = pj.Find("k")) p.kills = v->AsInt();
			if (const JsonValue* v = pj.Find("a")) p.assists = v->AsInt();
			if (const JsonValue* v = pj.Find("d")) p.deaths = v->AsInt();
			if (const JsonValue* v = pj.Find("score")) p.score = v->AsInt();
			if (const JsonValue* v = pj.Find("mvps")) p.mvps = v->AsInt();
			if (const JsonValue* v = pj.Find("team")) p.teamIndex = v->AsInt();
			if (const JsonValue* pr = pj.Find("perRound"); pr && pr->type == JsonValue::Type::Array) {
				p.perRound.reserve(pr->arr.size());
				for (const auto& rj : pr->arr) {
					if (rj.type != JsonValue::Type::Object) continue;
					RoundStat rs;
					if (const JsonValue* v = rj.Find("k")) rs.kills = v->AsInt();
					if (const JsonValue* v = rj.Find("hs")) rs.headshots = v->AsInt();
					if (const JsonValue* v = rj.Find("d")) rs.died = v->AsInt();
					if (const JsonValue* v = rj.Find("w")) rs.won = v->AsInt();
					if (const JsonValue* v = rj.Find("side")) rs.side = v->AsInt();
					if (const JsonValue* v = rj.Find("mvp")) rs.mvp = v->AsInt();
					p.perRound.push_back(rs);
				}
			}
			out.players.push_back(std::move(p));
		}
	}
	if (!out.players.empty())
		out.hasScoreboard = true;
	return true;
}

} // namespace

DemoHelperResult ReadDemoInfoViaHelper(const std::wstring& demoPath,
	const std::vector<uint32_t>& wantedAccountIds) {
	DemoHelperResult result;

	const bool wantNamesFast = !wantedAccountIds.empty();
	const std::wstring cachePath = demoPath + L".fmjson";
	const std::wstring exe = FindHelperExe();

	ULONGLONG demoTime = 0, cacheTime = 0, helperTime = 0;
	const bool haveDemoTime = GetWriteTime(demoPath, demoTime);
	const bool haveHelperTime = !exe.empty() && GetWriteTime(exe, helperTime);

	// Reuse the cache only if it is newer than BOTH the demo and the helper exe
	// (so rebuilding/updating the helper transparently regenerates stale results),
	// matches the current schema, and actually has the data this call needs (a
	// names-only cache cannot satisfy a full-parse request from a demo that has no
	// matchmaking sidecar).
	if (GetWriteTime(cachePath, cacheTime)
		&& (!haveDemoTime || cacheTime >= demoTime)
		&& (!haveHelperTime || cacheTime >= helperTime)) {
		DemoHelperResult cached;
		std::string cachedJson = ReadFileUtf8(cachePath);
		if (!cachedJson.empty() && ParseJson(cachedJson, cached)
			&& cached.schemaVersion == kSchemaVersion
			&& (wantNamesFast || !cached.namesOnly)) {
			return cached;
		}
	}

	if (exe.empty())
		return result; // no helper -> caller falls back to the sidecar

	std::wstring idsArg;
	if (wantNamesFast) {
		std::string ids;
		for (size_t i = 0; i < wantedAccountIds.size(); ++i) {
			if (i) ids.push_back(',');
			ids += std::to_string(wantedAccountIds[i]);
		}
		idsArg.assign(ids.begin(), ids.end()); // decimal digits/commas: ASCII-safe
	}

	std::string json = RunCaptureStdout(exe, demoPath, idsArg);
	if (json.empty())
		return result;

	// Cache whatever the helper produced (including ok:false negative results, so
	// unparseable demos are not re-run every scan).
	WriteFileUtf8(cachePath, json);
	ParseJson(json, result);
	return result;
}

} // namespace Filmmaker
