# Model Routing and Cost Policy

This file explains how coding agents should choose models, tools, subagents, workflows, and verification depth for this repo.

The goal is not to copy another creator's high-usage workflow. The goal is to get reliable work done on Windows while keeping cost under control.

## Core rule

Cost is a hard constraint.

Agents must start with the cheapest reliable path, then escalate only when the task proves it needs more reasoning, more context, or another reviewer.

Do not treat cost as a minor tie-breaker. A small correct fix is better than an expensive overbuilt fix.

## Platform assumptions

- The user is on Windows.
- Do not assume macOS tools, paths, apps, or computer-use workflows.
- Prefer PowerShell-compatible commands.
- Use Git, ripgrep, CMake, Visual Studio build tools, Python, Go, and repo scripts when appropriate.
- Use WSL only when the repo or toolchain clearly benefits from it.
- If visual verification is needed, ask for screenshots, logs, or specific reproduction steps instead of assuming desktop computer use.

## Reasoning defaults

Keep normal Claude work on High only when the selected environment already uses High by default. Do not escalate above High unless the user explicitly asks.

Use lower-cost approaches inside subagents and helper tasks whenever possible.

| Task type | Default effort | Notes |
|---|---:|---|
| Search, file discovery, reading docs | Low | Do not scan the whole repo unless needed. |
| Summaries, log extraction, simple explanations | Low | Keep outputs focused. |
| Small edits, formatting, rename, obvious config changes | Low or Medium | Prefer a targeted diff. |
| Normal bug fixes or build errors | Medium | Investigate first, then edit. |
| Multi-file feature work | Medium, then High review | Do not spawn workflows by default. |
| Architecture, threading, Panorama, hook behavior, particle pipeline | High | These are easy to break. |
| Final review before risky merge | High | Review the diff and verification results. |
| X-High, Max, Ultra, or equivalent modes | Disabled by default | Only use with explicit permission. |

## Model and agent routing

These are defaults, not permission to spend freely.

| Worker type | Relative cost | Strength | Use for |
|---|---:|---|---|
| Cheap/basic helper | Low | Fast mechanical work | Search, summaries, narrow reviews, log reading. |
| Normal coding model | Medium | Balanced | Most implementation work. |
| High-reasoning model | High | Best judgment | Hard debugging, architecture, risky changes, final review. |
| Codex or external helper | Varies | Useful for independent checks | Narrow review, read-only analysis, targeted implementation, command output analysis. |
| Multi-agent workflow | Highest total cost | Useful only for broad parallel work | Large PR triage, many independent files, or broad review with real payoff. |

## How to apply model routing

- Start cheap.
- Escalate only if the cheaper path fails or the risk justifies it.
- Prefer one focused agent over many agents.
- Prefer a narrow read-only pass before an editing pass.
- Do not run broad workflows for small bugs.
- Do not create several worktrees unless the task truly benefits from parallel work.
- If a task may burn a lot of tokens or run a long time, stop and explain the expected cost/risk before continuing.
- If the current task is too broad, break it into smaller options and ask which one to do first.

## Codex or external helper usage

Use Codex or another helper only when it reduces total cost or improves verification.

Good uses:

- Independent read-only code review.
- Summarizing long logs or build output.
- Searching for references across the repo.
- Comparing old vs new files.
- Checking particle/material/texture path closure.
- Running a targeted command and reporting the result.
- Implementing a small bounded fix in an isolated worktree.

Bad uses:

- Spawning a helper because it is available.
- Sending vague prompts like "fix everything."
- Running broad repo scans without a reason.
- Creating parallel implementation agents for a small bug.
- Doing UI computer use when screenshots or logs would be cheaper.

## Codex command policy

If Codex CLI is available, use it conservatively.

Preferred read-only pattern:

```bash
codex exec -s read-only "<self-contained prompt>"
```

Preferred implementation pattern:

```bash
codex exec "<self-contained prompt with exact scope, files, and stopping point>"
```

Rules:

- Prompts must be self-contained.
- Prompts must name the repo, task, target files or folders, and expected output.
- For reviews, Codex should not edit files.
- For implementation, Codex should make the smallest change that solves the stated problem.
- If Codex runs longer than expected, stop and report progress instead of blindly continuing.
- If Codex output is questionable, verify claims against the actual files before trusting it.

## Subagents and workflows

Subagents are allowed, but they are not the default.

Use a subagent when:

- The task is clearly separable.
- A second perspective is useful.
- The subagent can finish with a small report.
- The result can be checked cheaply.

Use a workflow only when:

- There are many independent items to inspect.
- The work can be split cleanly.
- The workflow will reduce total time/cost compared to manual serial work.
- The user has approved the scope.

Do not use a workflow when:

- The task is one bug.
- The fix is likely in one or two files.
- The work needs constant product judgment.
- The agent would be guessing about requirements.

## Worktree policy

Use worktrees only for isolation or parallel work that is actually useful.

- One focused worktree is fine for risky implementation.
- Multiple worktrees require a clear reason.
- Do not let parallel agents edit the same files unless explicitly planned.
- Do not merge, rebase, close PRs, delete branches, or delete files unless the user explicitly allows it.
- Always summarize branch/worktree state before asking the user to review.

## Review policy

For small changes:

- Inspect the diff.
- Run the smallest relevant verification.
- Report files touched and risks.

For medium changes:

- Inspect the diff.
- Run build/test/typecheck commands that match the touched area.
- Check that no unrelated files changed.

For risky changes:

- Do a plan first.
- Make small commits or clearly separated changes.
- Run targeted verification.
- Use a high-reasoning review pass.
- Consider one independent read-only helper review.

## CS2 / SOURCE:MVM cost-aware verification

For this repo, prefer file-level and command-level verification before visual or live-game verification.

Cheap checks first:

- Search exact particle system names.
- Check referenced files exist.
- Compare original and modified assets.
- Inspect `.vpcf`, `.vmat`, `.vtex`, `.json`, `.cpp`, `.h`, `.ps1`, and `.bat` references.
- Confirm staged output paths.
- Run targeted build commands when needed.
- Run only the matching verifier script instead of all verification scripts.

Expensive checks only when needed:

- Full `build.bat`.
- FX asset reconversion.
- Live CS2 launch.
- Long demo playback testing.
- Broad multi-agent review.

## Required stop points

Stop and ask before doing any of these:

- Running expensive full builds repeatedly.
- Recompiling FX packs unless the task requires it.
- Editing broad architecture without a plan.
- Touching Source 1 or GoldSource code.
- Changing public behavior outside the requested feature.
- Deleting files or branches.
- Merging PRs.
- Closing PRs.
- Any action that could affect production, releases, or user data.

## Reporting format

At the end of work, report:

1. What changed.
2. Files touched.
3. What was verified.
4. What was not verified.
5. Cost/risk notes.
6. Recommended next step.

Keep the report short unless the user asks for more detail.

## Short version for agents

- Windows-first.
- Cost is a hard constraint.
- Start cheap.
- High is the normal ceiling.
- Never use Max/X-High/Ultra unless asked.
- Do not spawn workflows by default.
- Use Codex/helpers only for narrow, useful tasks.
- Prefer file-level verification for CS2 mod work.
- Ask before expensive, destructive, or broad work.
