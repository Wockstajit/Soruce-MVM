#include "RagdollFx.h"

#include "../Cosmetics/CosmeticModelSwap.h"
#include "../../ClientEntitySystem.h"
#include "../../SchemaSystem.h"
#include "../../../shared/AfxConsole.h"
#include "../../../deps/release/prop/AfxHookSource/SourceSdkShared.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace Filmmaker {
namespace {

constexpr char kImprovedPrefix[] = "models/filmmaker/improved_ragdolls/";
bool g_enabled = false;

CEntityInstance* EntityAt(int index) {
	if (index < 0 || index > GetHighestEntityIndex() || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index);
}

void ReadModelPath(CEntityInstance* entity, char* out, size_t outSize) {
	if (out && outSize) out[0] = '\0';
	if (!entity || !out || outSize == 0)
		return;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (o.ModelChain.m_CBodyComponent == 0 || o.ModelChain.m_skeletonInstance == 0
		|| o.ModelChain.m_modelState == 0 || o.ModelChain.m_ModelName == 0)
		return;
	unsigned char* base = (unsigned char*)entity;
	__try {
		unsigned char* body = *(unsigned char**)(base + o.ModelChain.m_CBodyComponent);
		if ((uintptr_t)body <= 0x10000) return;
		unsigned char* state = body + o.ModelChain.m_skeletonInstance + o.ModelChain.m_modelState;
		const char* name = *(const char**)(state + o.ModelChain.m_ModelName);
		if ((uintptr_t)name <= 0x10000) return;
		size_t i = 0;
		for (; name[i] && i + 1 < outSize; ++i) out[i] = name[i];
		out[i] = '\0';
	} __except (1) {
		out[0] = '\0';
	}
}

bool StartsWith(const char* value, const char* prefix) {
	return value && prefix && 0 == _strnicmp(value, prefix, std::strlen(prefix));
}

bool BuildImprovedPath(const char* original, char* out, size_t outSize) {
	if (!StartsWith(original, "agents/models/") || !out || outSize == 0)
		return false;
	int written = std::snprintf(out, outSize, "%s%s", kImprovedPrefix, original);
	return written > 0 && (size_t)written < outSize;
}

const char* OriginalFromImproved(const char* model) {
	return StartsWith(model, kImprovedPrefix) ? model + std::strlen(kImprovedPrefix) : nullptr;
}

} // namespace

bool RagdollFx_Enabled() { return g_enabled; }

void RagdollFx_SetEnabled(bool enabled) {
	g_enabled = enabled;
}

void RagdollFx_RunFrame() {
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* entity = EntityAt(i);
		// Never SetModel an existing ragdoll. Doing so after CS2 has entered its death
		// state replaces the model without rebuilding the ragdoll and leaves it frozen
		// upright. The toggle therefore applies to future deaths: living pawns carry the
		// selected model and CS2 creates physics from it normally when they die.
		if (!entity || !entity->IsPlayerPawn() || entity->GetHealth() == 0)
			continue;

		char current[512];
		ReadModelPath(entity, current, sizeof(current));
		if (!current[0])
			continue;

		const char* original = OriginalFromImproved(current);
		if (!g_enabled) {
			if (original)
				ApplyAgentModel((unsigned char*)entity, original);
			continue;
		}
		if (original)
			continue;

		char improved[640];
		if (BuildImprovedPath(current, improved, sizeof(improved)))
			ApplyAgentModel((unsigned char*)entity, improved);
	}
}

void RagdollFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* arg = argc >= 3 ? args->ArgV(2) : "toggle";
	if (0 == _stricmp(arg, "on") || 0 == _stricmp(arg, "1")) RagdollFx_SetEnabled(true);
	else if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "0")) RagdollFx_SetEnabled(false);
	else if (0 != _stricmp(arg, "state") && 0 != _stricmp(arg, "status")) RagdollFx_SetEnabled(!RagdollFx_Enabled());
	advancedfx::Message("%s ragdoll: %s (applies to future deaths; existing bodies are unchanged).\n",
		cmd, RagdollFx_Enabled() ? "ON" : "off");
}

} // namespace Filmmaker
