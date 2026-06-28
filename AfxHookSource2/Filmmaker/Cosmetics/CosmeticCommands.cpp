// Console command surface for the SteamID-keyed cosmetic override system: "mirv_filmmaker
// cosmetics <subcommand> ...". Mirrors the existing DoCosmetics() style in FilmmakerCommand.cpp
// (advancedfx::Message/Warning, args->ArgV(i)) but drives CosmeticOverrideSystem (CosmeticsRef())
// instead of the legacy pawn-indexed MirvCosmetics backend.
//
// Kept in its own translation unit per the Cosmetics/ folder layout (CosmeticOverrideSystem.cpp /
// CosmeticProfile.cpp / CosmeticCatalog.cpp already split this way).

#include "CosmeticOverrideSystem.h"

#include "../../../shared/AfxConsole.h"

#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>

namespace Filmmaker {

namespace {

// Case-insensitive token compare, matching the _stricmp idiom used throughout FilmmakerCommand.cpp.
bool TokenIs(const char* token, const char* literal) {
	return token && 0 == _stricmp(token, literal);
}

// Resolves a steamId argument that is either the literal "current" (the player the local viewer is
// currently spectating) or a decimal SteamID64. Returns 0 and prints a Warning on failure.
uint64_t ResolveSteamIdArg(const char* cmd, const char* token) {
	if (!token || !*token) {
		advancedfx::Warning("%s cosmetics: missing steamId.\n", cmd);
		return 0;
	}
	if (TokenIs(token, "current")) {
		uint64_t id = CosmeticsRef().CurrentSpectatedSteamId();
		if (id == 0)
			advancedfx::Warning("%s cosmetics: no spectated player to resolve 'current' to.\n", cmd);
		return id;
	}
	uint64_t id = _strtoui64(token, nullptr, 10);
	if (id == 0)
		advancedfx::Warning("%s cosmetics: invalid steamId '%s'.\n", cmd, token);
	return id;
}

// After a successful profile mutation: persist, and surface the two gating hints (disabled /
// offsets unresolved) so the user understands why they might not see a change. The confirmation
// line itself is printed by the caller (so it can use advancedfx::Message's printf-style
// formatting directly, matching the existing DoCosmetics() style).
void AfterMutation(const char* cmd) {
	CosmeticsRef().Save();
	if (!CosmeticsRef().Enabled())
		advancedfx::Message("cosmetics are disabled -- run '%s cosmetics enabled 1' to see them.\n", cmd);
	if (!CosmeticsRef().OffsetsAvailable())
		advancedfx::Warning("%s cosmetics: econ offsets did not resolve; cosmetics will not render.\n", cmd);
}

void PrintUsage(const char* cmd) {
	advancedfx::Warning("usage: %s cosmetics <subcommand> ...\n", cmd);
	advancedfx::Message(
		"  %s cosmetics enabled [0|1]\n"
		"  %s cosmetics status\n"
		"  %s cosmetics debug [0|1]\n"
		"  %s cosmetics clear\n"
		"  %s cosmetics clearPlayer <steamId|current>\n"
		"  %s cosmetics target current\n"
		"  %s cosmetics player <steamId|current> weapon <defIndex> [paint=N] [wear=F] [seed=N] [stattrak=N]\n"
		"  %s cosmetics player <steamId|current> knife <defIndex> [paint=N] [wear=F] [seed=N]\n"
		"  %s cosmetics player <steamId|current> gloves <defIndex> [paint=N] [wear=F] [seed=N]\n"
		"  %s cosmetics player <steamId|current> agent <modelPath|default>\n"
			"  %s cosmetics diag   (dump the spectated player's live weapon econ state)\n"
			"  %s cosmetics recompose [0|1]   (force material re-composite after writes)\n"
			"  %s cosmetics vtidx <comp> <sec>   (tune UpdateComposite vtable indices, -1=skip)\n",
		cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
}

// Parses ORDER-INDEPENDENT key/value tokens of the form "key=value" (or "key value", to be lenient)
// starting at argv index `from`. Recognized keys: paint, wear, seed, stattrak. Unknown keys are
// ignored. Values keep their caller-supplied defaults when the key is absent.
void ParseItemKeyValueArgs(int argc, advancedfx::ICommandArgs* args, int from,
	int* paintKit, float* wear, int* seed, int* statTrak) {
	for (int i = from; i < argc; ++i) {
		const char* tok = args->ArgV(i);
		if (!tok || !*tok)
			continue;
		const char* eq = std::strchr(tok, '=');
		std::string key;
		std::string value;
		if (eq) {
			key.assign(tok, eq - tok);
			value.assign(eq + 1);
		} else {
			// Lenient "key value" form: consume the next token as the value if present.
			key.assign(tok);
			if (i + 1 < argc) {
				value.assign(args->ArgV(i + 1));
				++i;
			}
		}
		if (key.empty() || value.empty())
			continue;
		if (0 == _stricmp(key.c_str(), "paint")) {
			if (paintKit) *paintKit = atoi(value.c_str());
		} else if (0 == _stricmp(key.c_str(), "wear")) {
			if (wear) *wear = (float)atof(value.c_str());
		} else if (0 == _stricmp(key.c_str(), "seed")) {
			if (seed) *seed = atoi(value.c_str());
		} else if (0 == _stricmp(key.c_str(), "stattrak")) {
			if (statTrak) *statTrak = atoi(value.c_str());
		}
	}
}

void DoEnabled(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 4) {
		advancedfx::Message("%s cosmetics enabled = %d.\n", cmd, CosmeticsRef().Enabled() ? 1 : 0);
		return;
	}
	bool enabled = 0 != atoi(args->ArgV(3));
	CosmeticsRef().SetEnabled(enabled);
	CosmeticsRef().Save();
	advancedfx::Message("%s cosmetics enabled = %d.\n", cmd, CosmeticsRef().Enabled() ? 1 : 0);
}

void DoDebug(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 4) {
		advancedfx::Message("%s cosmetics debug = %d.\n", cmd, CosmeticsRef().Debug() ? 1 : 0);
		return;
	}
	CosmeticsRef().SetDebug(0 != atoi(args->ArgV(3)));
	advancedfx::Message("%s cosmetics debug = %d.\n", cmd, CosmeticsRef().Debug() ? 1 : 0);
}

void DoClear(const char* cmd) {
	CosmeticsRef().ClearAll();
	CosmeticsRef().Save();
	advancedfx::Message("%s cosmetics: cleared all profiles.\n", cmd);
}

void DoClearPlayer(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 4) {
		advancedfx::Warning("usage: %s cosmetics clearPlayer <steamId|current>\n", cmd);
		return;
	}
	uint64_t id = ResolveSteamIdArg(cmd, args->ArgV(3));
	if (id == 0)
		return;
	CosmeticsRef().ClearPlayer(id);
	CosmeticsRef().Save();
	advancedfx::Message("%s cosmetics: cleared profile for steamId %llu.\n", cmd, (unsigned long long)id);
}

void DoTarget(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* sub = (argc >= 4) ? args->ArgV(3) : "";
	if (!TokenIs(sub, "current")) {
		advancedfx::Warning("usage: %s cosmetics target current\n", cmd);
		return;
	}
	uint64_t id = CosmeticsRef().CurrentSpectatedSteamId();
	if (id == 0) {
		advancedfx::Warning("%s cosmetics target: no spectated player.\n", cmd);
		return;
	}
	std::string name = CosmeticsRef().NameForSteamId(id);
	advancedfx::Message("%s cosmetics target: steamId=%llu name='%s'.\n",
		cmd, (unsigned long long)id, name.c_str());
}

void DoPlayer(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 6) {
		advancedfx::Warning(
			"usage: %s cosmetics player <steamId|current> <weapon|knife|gloves|agent> <defIndex|model> "
			"[paint=N] [wear=F] [seed=N] [stattrak=N]\n", cmd);
		return;
	}
	uint64_t id = ResolveSteamIdArg(cmd, args->ArgV(3));
	if (id == 0)
		return;

	const char* slot = args->ArgV(4);

	if (TokenIs(slot, "weapon")) {
		int defIndex = atoi(args->ArgV(5));
		int paintKit = 0;
		float wear = 0.0f;
		int seed = 0;
		int statTrak = -1;
		ParseItemKeyValueArgs(argc, args, 6, &paintKit, &wear, &seed, &statTrak);
		CosmeticsRef().SetWeapon(id, defIndex, paintKit, wear, seed, statTrak);
		advancedfx::Message("%s cosmetics: weapon steamId=%llu def=%d paint=%d wear=%.4f seed=%d stattrak=%d.\n",
			cmd, (unsigned long long)id, defIndex, paintKit, wear, seed, statTrak);
		AfterMutation(cmd);
	} else if (TokenIs(slot, "knife")) {
		int defIndex = atoi(args->ArgV(5));
		int paintKit = 0;
		float wear = 0.0f;
		int seed = 0;
		int statTrak = -1; // unused for knives, parsed for symmetry/leniency only
		ParseItemKeyValueArgs(argc, args, 6, &paintKit, &wear, &seed, &statTrak);
		CosmeticsRef().SetKnife(id, defIndex, paintKit, wear, seed);
		advancedfx::Message("%s cosmetics: knife steamId=%llu def=%d paint=%d wear=%.4f seed=%d.\n",
			cmd, (unsigned long long)id, defIndex, paintKit, wear, seed);
		AfterMutation(cmd);
	} else if (TokenIs(slot, "gloves")) {
		int defIndex = atoi(args->ArgV(5));
		int paintKit = 0;
		float wear = 0.0f;
		int seed = 0;
		int statTrak = -1; // unused for gloves, parsed for symmetry/leniency only
		ParseItemKeyValueArgs(argc, args, 6, &paintKit, &wear, &seed, &statTrak);
		CosmeticsRef().SetGloves(id, defIndex, paintKit, wear, seed);
		advancedfx::Message("%s cosmetics: gloves steamId=%llu def=%d paint=%d wear=%.4f seed=%d.\n",
			cmd, (unsigned long long)id, defIndex, paintKit, wear, seed);
		AfterMutation(cmd);
	} else if (TokenIs(slot, "agent")) {
		const char* model = args->ArgV(5);
		CosmeticsRef().SetAgent(id, model, 0);
		advancedfx::Message("%s cosmetics: agent steamId=%llu model='%s'.\n", cmd, (unsigned long long)id, model);
		AfterMutation(cmd);
	} else {
		advancedfx::Warning("%s cosmetics player: unknown slot '%s' (expected weapon|knife|gloves|agent).\n", cmd, slot);
	}
}

// Forced material re-composite tuning (the refresh path). OFF by default; the correct CS2
// UpdateComposite vtable index is build-specific, so it is live-tunable here. See
// CosmeticOverrideSystem.h / SetRecompose.
void DoRecompose(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc >= 4)
		CosmeticsRef().SetRecompose(0 != atoi(args->ArgV(3)));
	advancedfx::Message("%s cosmetics: recompose=%d faulted=%d.\n", cmd,
		CosmeticsRef().Recompose() ? 1 : 0, CosmeticsRef().RecomposeFaulted() ? 1 : 0);
}

void DoVtIdx(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 5) {
		advancedfx::Warning("usage: %s cosmetics vtidx <compositeIdx> <secIdx>  (-1 = skip a call)\n", cmd);
		return;
	}
	CosmeticsRef().SetVtIdx(atoi(args->ArgV(3)), atoi(args->ArgV(4)));
	int comp = 0, sec = 0;
	CosmeticsRef().GetVtIdx(&comp, &sec);
	advancedfx::Message("%s cosmetics: vtidx comp=%d sec=%d (recompose re-armed).\n", cmd, comp, sec);
}

// Opt-in legacy fallback-id mode: forces C_EconItemView::m_iItemIDHigh = -1. Default OFF -- it breaks
// the UI inventory read and is unnecessary now that the networked attributes are overwritten directly.
void DoFallback(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc >= 4)
		CosmeticsRef().SetFallbackId(0 != atoi(args->ArgV(3)));
	advancedfx::Message("%s cosmetics: fallback (itemIDHigh=-1) = %d.\n", cmd, CosmeticsRef().FallbackId() ? 1 : 0);
}

// Sets the argument passed to the recompose vtable call. 1 = default; 0 lets us test
// OnDataChanged(DATA_UPDATE_CREATED), the candidate client-side skin-rebuild trigger.
void DoVtArg(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc >= 4)
		CosmeticsRef().SetVtArg(atoi(args->ArgV(3)));
	advancedfx::Message("%s cosmetics: vtarg = %d (vtable call arg; 0 = DATA_UPDATE_CREATED).\n",
		cmd, CosmeticsRef().VtArg());
}

} // namespace

void Cosmetics_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* sub = (argc >= 3) ? args->ArgV(2) : "";

	if (TokenIs(sub, "enabled")) {
		DoEnabled(argc, args, cmd);
	} else if (TokenIs(sub, "status")) {
		Cosmetics_PrintStatus(cmd);
	} else if (TokenIs(sub, "debug")) {
		DoDebug(argc, args, cmd);
	} else if (TokenIs(sub, "clear")) {
		DoClear(cmd);
	} else if (TokenIs(sub, "clearPlayer")) {
		DoClearPlayer(argc, args, cmd);
	} else if (TokenIs(sub, "target")) {
		DoTarget(argc, args, cmd);
	} else if (TokenIs(sub, "player")) {
		DoPlayer(argc, args, cmd);
	} else if (TokenIs(sub, "diag")) {
		Cosmetics_PrintSpectatedDebug(cmd);
	} else if (TokenIs(sub, "recompose")) {
		DoRecompose(argc, args, cmd);
	} else if (TokenIs(sub, "vtidx")) {
		DoVtIdx(argc, args, cmd);
	} else if (TokenIs(sub, "fallback")) {
		DoFallback(argc, args, cmd);
	} else if (TokenIs(sub, "vtarg")) {
		DoVtArg(argc, args, cmd);
	} else {
		PrintUsage(cmd);
	}
}

} // namespace Filmmaker
