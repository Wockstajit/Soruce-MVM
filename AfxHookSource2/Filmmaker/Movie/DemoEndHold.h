#pragma once

// End-of-demo hold: stops CS2 from kicking back to the main menu when a demo's
// last tick is reached. Just before the engine would process the demo's stop
// record, playback is paused and held on (effectively) the last tick. If the
// user presses play again while held at the end, playback restarts from the
// beginning of the demo instead of running off the end into the menu.
//
// Driven once per frame from Filmmaker::RunMainThreadFrame(). Always on while
// a demo is active; `mirv_filmmaker endhold 0/1` toggles it at runtime.

namespace Filmmaker {

void DemoEndHold_RunFrame();

void DemoEndHold_SetEnabled(bool enabled);
bool DemoEndHold_Enabled();

} // namespace Filmmaker
