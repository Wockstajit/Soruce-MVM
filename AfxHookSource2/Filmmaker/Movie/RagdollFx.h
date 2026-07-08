#pragma once

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Main-thread player-model selection for future ragdolls. Existing dead bodies are never swapped.
void RagdollFx_RunFrame();
bool RagdollFx_Enabled();
void RagdollFx_SetEnabled(bool enabled);
void RagdollFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
