# Automation

Repository automation, verification harnesses, launchers, configs, screenshots, and automation-only test drivers belong in this directory.

- `docs/` — automation documentation, including `AUTOMATION_HYGIENE.md` and the live UI editing guide.
- `launch/` — CS2 launchers and the live dashboard. `launch/launch-cs2-netcon.ps1` consistently launches CS2 windowed at 1600x1200, and `launch/live.bat` starts CS2 plus the dashboard.
- `capture/` — screenshot helpers. Prefer `capture/capture-game-window.ps1` for UI detail and use `capture/capture-main-monitor.ps1` only when desktop context matters.
- `netcon/` — low-level netcon command helpers.
- `verify/` — live and static verification harnesses.
- `tools/` — generators and one-off reusable maintenance scripts, such as `tools/generate_cosmetics_catalog.py`.
- `tests/` — automation-only test drivers built by CMake.
- `config/` — config files copied into CS2 for automation runs.
- `lib/` — shared PowerShell helpers such as `lib/AutomationCommon.ps1`.

Generated screenshots and logs should be written to `automation/runs/<automation>/<timestamp>/` or `automation/output/<feature>/`, and cleaned up when no longer needed.
