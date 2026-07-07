# CLAUDE.md

This file gives Claude Code the rules for working in this repo. Keep this file short. Detailed project background and optional agent workflows live in `docs/agent/`.

## Read this first

- Before changing code, read `docs/agent/project-context.md`.
- If the task touches a specific feature, also read the feature doc listed in that context file.
- Do not rely on memory when the repo docs or code can answer the question.
- When docs and code disagree, trust the code and update the relevant doc as part of the work.

## Agent docs

Longer instructions live in `docs/agent/`. Read these only when relevant:

- `docs/agent/project-context.md` — SOURCE:MVM architecture, repo layout, build flow, feature docs, threading rules, Panorama rules, FX pipeline, and gotchas.
- `docs/agent/model-routing-cost-policy.md` — use before subagents, workflows, helper models, expensive reasoning, or long-running tasks.
- `docs/agent/codex-review.md` — use before asking Codex/GPT-5.5 for an independent review.
- `docs/agent/codex-implementation.md` — use before asking Codex/GPT-5.5 to implement a bounded patch.
- `docs/agent/codex-computer-use.md` — use before asking Codex/GPT-5.5 to launch apps, use a browser, capture screenshots, inspect runtime behavior, or verify UI flows with computer use.

Do not read every agent doc by default. Pick the smallest relevant doc for the task.

## Core project direction

- This repo is SOURCE:MVM, a CS2 offline demo/movie-making tool built from HLAE/advancedfx.
- Active work targets Counter-Strike 2 only.
- Most new work belongs in `AfxHookSource2/`.
- Most filmmaker/tooling work belongs in `AfxHookSource2/Filmmaker/`.
- Do not modify inherited GoldSource or Source 1 code unless the task explicitly requires it.
- This DLL is for offline demo/movie work only. Do not wire it up for VAC-protected or online servers.

## Cost and model usage

- Cost is a hard constraint.
- Start with the cheapest reliable approach.
- Prefer focused investigation over broad exploration.
- Do not spawn many agents, workflows, or long-running tasks unless the task clearly needs it.
- Do not use Max, X-High, Ultra, or equivalent high-cost modes unless explicitly requested.
- If a task may use a lot of tokens or take a long time, stop and explain the risk before continuing.
- Escalate only when the cheaper path fails or the risk justifies it.
- Before using Codex, subagents, workflows, helper models, expensive reasoning, or long-running tasks, read `docs/agent/model-routing-cost-policy.md`.

## Reasoning defaults

Use low effort for:

- Reading files.
- Searching the repo.
- Summarizing logs.
- Simple edits.
- Formatting.
- Renaming.
- Small config changes.

Use medium effort for:

- Normal bug fixes.
- Moderate refactors.
- Build errors.
- Test failures.
- Small feature additions.

Use high effort only for:

- Hard debugging.
- Risky code changes.
- Architecture issues.
- Cross-system changes.
- Final review before merge.
- Anything that could break core behavior.

## Windows environment

- The user is on Windows.
- Prefer PowerShell-compatible commands unless the repo clearly requires Bash or WSL.
- Use WSL only when it is already part of the project setup or clearly beneficial.
- Do not assume macOS apps, paths, shortcuts, or Mac-only computer-use workflows.
- For visual or runtime verification, prefer screenshots, logs, in-game observations, targeted commands, and file-level checks first.
- If real local app/browser/UI interaction would help, read `docs/agent/codex-computer-use.md` before using Codex computer use.

## Work style

- Investigate before editing.
- Make the smallest correct change.
- Do not rewrite large areas unless necessary.
- Do not make unrelated improvements.
- Do not invent missing requirements.
- Do not create god files.
- Keep features separated by responsibility.
- Match the existing code style.
- Avoid new dependencies unless necessary.
- Ask before destructive actions.
- Never merge, delete branches, delete files, close PRs, or rewrite history unless explicitly allowed.

## Codex review

Before using Codex for independent review, read:

- `docs/agent/codex-review.md`

Use Codex review only when:

- The user asks for a Codex/GPT-5.5 review.
- A change is risky enough to need an independent second pass.
- A diff needs to be compared against requirements.

Do not use Codex review for small local checks, simple edits, or markdown-only changes. Codex review is review-only. Do not let it edit files.

## Codex implementation

Before delegating implementation work to Codex, read:

- `docs/agent/codex-implementation.md`

Use Codex implementation only for bounded implementation tasks where Codex can produce a patch.

Claude must still:

- Scope the task.
- Protect existing user changes.
- Inspect `git status --short` before Codex edits anything.
- Review `git diff` after Codex finishes.
- Reject unrelated or overbuilt edits.
- Run the cheapest useful verification.
- Report what Codex changed, what Claude verified, and what risks remain.

Do not use Codex implementation for tiny edits. Do not let Codex commit, push, deploy, delete branches, or edit global config unless explicitly asked.

## Codex computer use

Before using Codex for local app/browser/simulator verification, read:

- `docs/agent/codex-computer-use.md`

Use Codex computer use only when real UI interaction is useful:

- Testing a flow.
- Launching a local app.
- Using a browser.
- Capturing screenshots.
- Inspecting runtime behavior.
- Verifying UI behavior.

Do not use Codex computer use for normal code reading, typechecking, linting, or tests Claude can run directly.

Launching local apps, simulators, or browsers to verify the requested behavior is allowed. Ask first before doing anything that could disrupt the user's environment, close apps, change system settings, spend money, send messages, delete data, or act on real account data.

## Build and verification

- `build.bat` is the normal full build path.
- For CS2 hook-only iteration, prefer the faster x64 release target described in `docs/agent/project-context.md`.
- Do not run dev servers unless explicitly asked.
- Assume CS2 or HLAE may already be running.
- Close CS2 before building when the staged DLL may be locked.
- Prefer safe verification commands first: build, typecheck, tests, grep/ripgrep, and diff inspection.
- Before running expensive or long commands, explain why they are needed.

## CS2 and FX work

- Prioritize exact file-level verification.
- Do not guess particle names, material paths, texture paths, or asset references.
- Compare original files against modified files when possible.
- Use the actual provided assets when the task asks for them.
- Do not randomly clone effects just because names are similar.
- Preserve existing file structure unless there is a clear reason to change it.
- Report exact files touched and what still needs in-game testing.

## Threading and Panorama safety

- Never call Panorama `RunScript` from the render thread.
- Panorama work belongs on the main/UI thread.
- Backend or thread-safe work can run from the render-thread pump.
- Keep the existing main-thread and render-thread split intact.

## Output format after changes

When finishing work, report:

1. What changed.
2. Files touched.
3. Verification performed.
4. What was not verified.
5. Risks or next steps.
