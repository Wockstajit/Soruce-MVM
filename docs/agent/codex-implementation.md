---
name: codex-implementation
description: Ask Codex CLI (gpt-5.5) to implement scoped code changes in the current repository, then have Claude inspect the resulting diff and verification. Use when the user asks Claude to delegate implementation to Codex or gpt-5.5, when the model-routing policy routes the work to gpt-5.5, or when a bounded task would benefit from another coding agent producing a patch.
---

# Codex Implementation

Use Codex as a separate implementation agent for bounded code changes. Claude remains responsible for scoping the task, reviewing the diff, running or checking verification, and explaining the final result.

Use this when the user asks for Codex or delegation, or when a bounded task would benefit from another implementation agent producing a patch. Do not let Codex commit, push, deploy, or edit global config unless the user explicitly asked for that.

## Workflow

1. Pin the current state with `git status --short` and note any user changes already present.
2. Define the implementation scope: files or behavior to change, files to avoid, constraints, and verification commands.
3. Create a temporary artifact directory for Codex's report.
4. Run `codex exec` with repo write access.
5. After Codex exits, inspect `git status` and `git diff`.
6. Run the cheapest reliable verification yourself when practical.
7. Report what Codex changed, what Claude verified, and any remaining risks.

Use this command shape:

```bash
ARTIFACT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/codex-implementation.XXXXXX")"
REPORT="$ARTIFACT_DIR/report.md"
PROMPT="$ARTIFACT_DIR/prompt.md"

# Write a self-contained prompt to $PROMPT, then run:
codex exec \
  -C "$PWD" \
  --add-dir "$ARTIFACT_DIR" \
  -s workspace-write \
  -o "$REPORT" \
  "$(cat "$PROMPT")"
```

Use `-s workspace-write` for normal repo edits.

Use `-s danger-full-access` only when the task truly needs GUI automation, desktop app launching, screenshots, access outside the repo, or the user explicitly approved it.

Add `--skip-git-repo-check` when the working directory is not a git repository.

## Prompt Requirements

Tell Codex:

- The exact behavior to implement or verify.
- The platform and app type, such as Windows, web, Electron, CLI, or desktop.
- Known launch commands, test credentials, seed data, deep links, or fixtures.
- Whether source edits are allowed. Default to scoped edits only.
- Files or folders to avoid.
- Where screenshots, logs, and the final report should be saved.
- To return pass, fail, or blocked, plus steps performed, observed behavior, changed files, and actionable feedback.

Keep the prompt specific enough that Codex does not need the surrounding Claude conversation.

## Reporting Back

Before presenting Codex's result, Claude must inspect the diff.

Report:

- What Codex was asked to do.
- What files changed.
- What Claude verified.
- What failed or was blocked.
- Any risks or manual checks still needed.
