---
name: codex-computer-use
description: Ask the Codex CLI to run local app verification that needs computer use, browser automation, simulators, screenshot apps, app launching, or independent runtime inspection. This is how gpt-5.5 is invoked for computer-use work when the user asks Codex to test a flow, verify UI behavior, inspect a running app, capture screenshots, or report confirmation and feedback about implemented behavior that benefits from computer use.
---

# Codex Computer Use

Use Codex as a separate local verification agent when the task needs real UI interaction, screenshots, a browser, simulator/device state, app launching, or independent runtime inspection outside Claude's current context.

Do not use this for ordinary code reading, typechecking, linting, or tests Claude can run directly.

Launching app simulators, local apps, or browsers to verify requested behavior is allowed without asking first. Ask first if the run could disrupt the user's environment beyond that, such as closing their apps, changing system settings, spending money, sending messages, deleting data, or acting on real account data.

## Workflow

1. Define the exact behavior to verify.
2. Create an artifact directory for screenshots, logs, and the report.
3. Write a self-contained Codex prompt.
4. Run Codex with computer-use/full-access only for the verification task.
5. Read the report and inspect any screenshots/logs.
6. Report pass, fail, or blocked with what Codex observed.

Use this command shape:

```bash
ARTIFACT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/codex-computer-use.XXXXXX")"
REPORT="$ARTIFACT_DIR/report.md"
PROMPT="$ARTIFACT_DIR/prompt.md"

# Write a self-contained prompt to $PROMPT, then run:
codex exec \
  -C "$PWD" \
  --add-dir "$ARTIFACT_DIR" \
  -s danger-full-access \
  -o "$REPORT" \
  "$(cat "$PROMPT")"
```

Use `-s danger-full-access` for GUI automation, desktop app launching, browser interaction, screenshots, simulator/device checks, or access outside the repo.

For checks that only need repo files and the artifact directory, prefer:

```bash
codex exec \
  -C "$PWD" \
  --add-dir "$ARTIFACT_DIR" \
  -s workspace-write \
  -o "$REPORT" \
  "$(cat "$PROMPT")"
```

Add `--skip-git-repo-check` when the working directory is not a git repository.

## Prompt Requirements

Tell Codex:

- The exact behavior to verify.
- The platform and app type, such as Windows, web, Electron, CLI, desktop app, emulator, or simulator.
- Known launch commands, URLs, test credentials, seed data, deep links, fixtures, or demo files.
- Whether source edits are allowed. Default to no edits.
- Where screenshots, logs, and the final report should be saved.
- What counts as pass, fail, or blocked.
- To include steps performed, observed behavior, screenshot paths, console/log errors, and actionable feedback.

Keep the prompt specific enough that Codex does not need the surrounding Claude conversation.

## Default Prompt Shape

```text
Use computer use to verify this local behavior.

Do not edit source files unless explicitly needed and allowed below.
Do not commit, push, deploy, delete data, change system settings, send messages, or act on real account data.

Task:
<exact behavior to verify>

Environment:
- Platform/app type: <Windows/web/Electron/desktop/simulator/etc>
- Launch command or URL: <command/url>
- Test data or credentials: <only safe test info>
- Screenshots/logs/report directory: <artifact dir>

Pass criteria:
- <what must be true>

Fail criteria:
- <what would prove it is broken>

Return:
- pass, fail, or blocked
- steps performed
- observed behavior
- screenshot paths
- relevant logs/errors
- actionable feedback
```

## Reporting Back

Report:

- What Codex tested.
- Whether it passed, failed, or was blocked.
- What screenshots/logs were saved.
- What behavior Codex observed.
- Any manual checks still needed.
- Any risk if Codex had to use full access.
