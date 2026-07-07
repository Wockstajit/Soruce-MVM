---
name: codex-review
description: Use Codex CLI as a cost-controlled, read-only independent reviewer for code diffs, PRs, branches, commits, or specific implementations. Use this only when the user explicitly asks for Codex/gpt-5.5 review, when the model-routing policy calls for an independent review, or when a change is broad/risky enough that a second reviewer is worth the cost. For small local checks, use the normal review process instead.
---

# Codex Review

Use Codex as an independent reviewer, not as the final authority.

This skill is for review only. Codex must not edit files, commit, rebase, merge, delete files, close PRs, or run destructive commands. Treat Codex output as evidence that Claude must verify before reporting back.

## Cost rules

- Cost is a hard constraint.
- Do not use this skill for tiny edits, formatting-only changes, obvious typo fixes, or simple one-file checks.
- Prefer Claude's normal diff review for small local changes.
- Use Codex review when a second perspective is likely to save time or reduce risk.
- Keep the review target narrow. Review the exact diff, branch, commit, PR checkout, or files in question.
- Do not spawn multiple Codex reviews unless the user explicitly asks or the target is too large for one useful pass.

## When to use

Use this skill when one of these is true:

- The user asks for Codex, gpt-5.5, or an independent review.
- The implementation touches multiple systems.
- The change is risky, user-facing, or hard to reason about.
- Claude made a broad implementation and needs an outside reviewer.
- A PR, branch, or commit needs review before merge.
- There may be regressions, missing tests, security issues, or requirement mismatches.

Do not use this skill just to avoid reading the code yourself.

## Review stance

Ask Codex to prioritize findings over summary.

Each finding should include:

- severity: critical, high, medium, low
- file and line reference when possible
- concrete failure mode
- why the issue matters
- suggested fix direction
- whether the issue is proven by the diff or only suspected

Codex should also say clearly when there are no substantive findings and name the target it reviewed.

## Workflow

1. Identify the review target:
   - uncommitted changes
   - staged changes
   - current branch against a base branch
   - a specific commit SHA
   - a PR checkout
   - a small set of files
   - a specific implementation area

2. Inspect the target yourself first:
   - confirm the repo is clean or understand what is dirty
   - identify the base branch if needed
   - note risky files, user requirements, and expected behavior

3. Create a temporary artifact directory.

4. Write a focused prompt to a file.

5. Run Codex in read-only mode or use native Codex review if available.

6. Read the report.

7. Verify important Codex claims against the code or diff.

8. Report back with confirmed findings first, then unverified Codex suggestions.

## Windows PowerShell command shape

Use PowerShell first on this repo because the user works on Windows.

```powershell
$ArtifactDir = Join-Path ([System.IO.Path]::GetTempPath()) ("codex-review-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $ArtifactDir | Out-Null
$PromptPath = Join-Path $ArtifactDir "prompt.md"
$ReportPath = Join-Path $ArtifactDir "report.md"

@"
Review the specified code changes for bugs, regressions, missing tests, security issues, and requirement mismatches.

Review target:
<describe the exact target here>

User requirements:
<paste the user's relevant requirements here>

Risky areas to focus on:
<list risky files, systems, threading rules, build steps, or behavior concerns here>

Output format:
- Findings first, highest severity first.
- For each finding: severity, file/line, concrete failure mode, suggested fix direction.
- If there are no substantive findings, say so clearly and name the target reviewed.
- Do not edit files.
"@ | Set-Content -Path $PromptPath -Encoding UTF8

# Preferred safe fallback: ask Codex to review a supplied diff in read-only mode.
# This avoids depending on interactive /review UI behavior.
$DiffPath = Join-Path $ArtifactDir "diff.patch"
git diff --no-ext-diff --binary > $DiffPath

git diff --no-ext-diff --cached --binary >> $DiffPath

$ReviewInput = @"
$(Get-Content -Raw $PromptPath)

--- DIFF START ---
$(Get-Content -Raw $DiffPath)
--- DIFF END ---
"@

$ReviewInput | codex exec --sandbox read-only - > $ReportPath
Get-Content -Raw $ReportPath
```

## Native Codex review command shape

If the installed Codex CLI supports the native `review` subcommand in non-interactive mode, prefer it for normal Git diff review because it is purpose-built for review.

Check support first when unsure:

```powershell
codex review --help
```

Possible command shapes, depending on installed Codex version:

```powershell
# Review uncommitted changes.
codex -C (Get-Location).Path review --uncommitted

# Review current branch against main.
codex -C (Get-Location).Path review --base main

# Review one commit.
codex -C (Get-Location).Path review --commit <sha>
```

If any native review command fails, fall back to `codex exec --sandbox read-only` with an explicit diff and prompt. Do not waste time repeatedly retrying broken command syntax.

## Bash fallback command shape

Use this only when the environment is Bash, Git Bash, or WSL.

```bash
ARTIFACT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/codex-review.XXXXXX")"
PROMPT="$ARTIFACT_DIR/prompt.md"
REPORT="$ARTIFACT_DIR/report.md"
DIFF="$ARTIFACT_DIR/diff.patch"

cat > "$PROMPT" <<'PROMPT'
Review the specified code changes for bugs, regressions, missing tests, security issues, and requirement mismatches.

Review target:
<describe the exact target here>

User requirements:
<paste the user's relevant requirements here>

Risky areas to focus on:
<list risky files, systems, threading rules, build steps, or behavior concerns here>

Output format:
- Findings first, highest severity first.
- For each finding: severity, file/line, concrete failure mode, suggested fix direction.
- If there are no substantive findings, say so clearly and name the target reviewed.
- Do not edit files.
PROMPT

git diff --no-ext-diff --binary > "$DIFF"
git diff --no-ext-diff --cached --binary >> "$DIFF"

{
  cat "$PROMPT"
  printf '\n--- DIFF START ---\n'
  cat "$DIFF"
  printf '\n--- DIFF END ---\n'
} | codex exec --sandbox read-only - > "$REPORT"

cat "$REPORT"
```

## Branch or commit review without huge prompts

For a branch review, prefer generating a compact diff against the merge base.

PowerShell:

```powershell
$Base = "main"
$MergeBase = git merge-base HEAD $Base
$DiffPath = Join-Path $ArtifactDir "branch.diff.patch"
git diff --no-ext-diff --binary $MergeBase..HEAD > $DiffPath
```

Bash:

```bash
BASE="main"
MERGE_BASE="$(git merge-base HEAD "$BASE")"
git diff --no-ext-diff --binary "$MERGE_BASE..HEAD" > "$DIFF"
```

For a single commit:

PowerShell:

```powershell
git show --format=fuller --stat --patch --no-ext-diff <sha> > $DiffPath
```

Bash:

```bash
git show --format=fuller --stat --patch --no-ext-diff <sha> > "$DIFF"
```

## Prompt template

Use a focused prompt like this:

```text
You are reviewing code changes. Do not edit files.

Goal:
Find real bugs, regressions, missing tests, security issues, and requirement mismatches.

Project context:
This is a Windows-first CS2 / Source 2 movie-making tool based on HLAE / advancedfx. Most active work is in AfxHookSource2/ and AfxHookSource2/Filmmaker/. Be careful with threading, Panorama UI, build staging, particle FX paths, and live-game verification assumptions.

Review target:
<exact target>

User requirements:
<requirements>

Focus areas:
<risky areas>

Output:
Findings first. For each finding include severity, file/line, failure mode, and suggested fix direction. If there are no substantive findings, say that clearly and mention any residual test gaps.
```

## SOURCE:MVM review focus

For this repo, ask Codex to pay extra attention to:

- `AfxHookSource2/Filmmaker/` changes
- Panorama calls staying on the main/UI thread
- render-thread code avoiding `RunScript`
- input/cursor state not breaking freecam or UI control
- camera timeline, follow camera, graph editor, and config panel state transitions
- particle FX mode names, swap tables, materials, texture paths, and staged files
- schema/cache compatibility between Go helper output and C++ readers
- build/staging assumptions in `build.bat`, CMake presets, and `automation/launch/`
- generated files accidentally committed from `build/`, `automation/runs/`, `automation/output/`, `.agents/`, or `.claude/`

## Reporting back

Before relaying a Codex finding, inspect the cited code or diff enough to decide whether the finding is real.

Report in this structure:

```text
Codex review target: <target>

Confirmed issues:
1. <issue, severity, file/line, why it matters, fix direction>

Codex suggestions I did not fully verify:
1. <suggestion and why it is uncertain>

No-action findings:
1. <anything Codex flagged that appears wrong or not worth changing>

Residual risk / testing gaps:
- <what still needs build, test, or live CS2 verification>
```

If Codex finds nothing, say that clearly and mention what it inspected.

If `codex` is not installed or the command fails, report the error and review the changes directly instead.
