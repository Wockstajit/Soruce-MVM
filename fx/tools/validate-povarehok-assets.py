"""Prepare and validate the runtime-selected Better Particles resource closure."""

from __future__ import annotations

import argparse
import json
import re
from collections import deque
from pathlib import Path


ARRAY_RE = re.compile(
    r"const\s+VariantRule\s+kVariant(?P<category>\w+)\[\]\s*=\s*\{(?P<body>.*?)\n\};",
    re.DOTALL,
)
RULE_RE = re.compile(r'FXRULE\(\s*"[^"]+"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\)')
MONEY_RE = re.compile(r'kMoneyBurst\s*=\s*"([^"]+\.vpcf)"')
RESOURCE_RE = re.compile(r'resource:"([^"]+)"')
COMPILED_EXTENSIONS = {".vpcf", ".vmat", ".vtex", ".vmdl", ".vsnap"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--particle-fx-cpp", type=Path, required=True)
    parser.add_argument("--content-root", type=Path, required=True)
    parser.add_argument("--game-root", type=Path, required=True)
    parser.add_argument("--file-list", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--validate-compiled", action="store_true")
    return parser.parse_args()


def runtime_targets(cpp_path: Path) -> list[str]:
    text = cpp_path.read_text(encoding="utf-8")
    targets: set[str] = set()
    for table in ARRAY_RE.finditer(text):
        category = table.group("category")
        less_variant = "less_impacts" if category == "Impacts" else "less_smoke"
        for pack, name in RULE_RE.findall(table.group("body")):
            for variant in ("classic", "classic_updated", less_variant):
                targets.add(
                    f"particles/filmmaker/betterparticles/{variant}/{pack}/{name}.vpcf"
                )
    money = MONEY_RE.search(text)
    if money:
        targets.add(money.group(1))
    if not targets:
        raise RuntimeError(f"No FXRULE targets found in {cpp_path}")
    return sorted(targets)


def source_path(root: Path, resource: str) -> Path:
    return root.joinpath(*resource.replace("\\", "/").split("/"))


def compiled_path(root: Path, resource: str) -> Path:
    return source_path(root, resource + "_c")


def inspect_closure(content_root: Path, targets: list[str]) -> tuple[set[str], set[str], list[str], list[str]]:
    particle_closure: set[str] = set()
    references: set[str] = set()
    missing_root_targets: list[str] = []
    missing_child_references: list[str] = []
    pending = deque((target, True) for target in targets)

    while pending:
        resource, is_root = pending.popleft()
        if resource in particle_closure:
            continue
        particle_closure.add(resource)
        path = source_path(content_root, resource)
        if not path.is_file():
            if is_root:
                missing_root_targets.append(resource)
            else:
                missing_child_references.append(resource)
            continue
        for reference in RESOURCE_RE.findall(path.read_text(encoding="utf-8")):
            normalized = reference.replace("\\", "/")
            references.add(normalized)
            if normalized.startswith("particles/filmmaker/betterparticles/") and normalized.endswith(".vpcf"):
                pending.append((normalized, False))
    return particle_closure, references, missing_root_targets, missing_child_references


def main() -> int:
    args = parse_args()
    content_root = args.content_root.resolve()
    game_root = args.game_root.resolve()
    targets = runtime_targets(args.particle_fx_cpp.resolve())
    closure, references, missing_root_targets, missing_child_references = inspect_closure(content_root, targets)

    local_resources: set[str] = set()
    external_resources: set[str] = set()
    legacy_models: list[str] = []
    for resource in closure | references:
        path = source_path(content_root, resource)
        if path.is_file():
            local_resources.add(resource)
            if path.suffix.lower() == ".vmdl":
                first_line = path.open("r", encoding="utf-8").readline()
                if "format:source1imported" in first_line:
                    legacy_models.append(resource)
        elif Path(resource).suffix.lower() in COMPILED_EXTENSIONS:
            external_resources.add(resource)
    external_models = sorted(
        resource for resource in external_resources
        if Path(resource).suffix.lower() == ".vmdl"
    )

    missing_compiled: list[str] = []
    if args.validate_compiled:
        for resource in sorted(local_resources):
            if Path(resource).suffix.lower() not in COMPILED_EXTENSIONS:
                continue
            if not compiled_path(game_root, resource).is_file():
                missing_compiled.append(resource)

    args.file_list.parent.mkdir(parents=True, exist_ok=True)
    args.file_list.write_text(
        "\n".join(str(source_path(content_root, target)) for target in targets) + "\n",
        encoding="utf-8",
    )

    report = {
        "targets": targets,
        "targetCount": len(targets),
        "particleClosureCount": len(closure),
        "localResourceCount": len(local_resources),
        "externalResources": sorted(external_resources),
        "externalModels": external_models,
        "missingRootTargets": sorted(missing_root_targets),
        "missingChildReferences": sorted(missing_child_references),
        "legacyModels": sorted(legacy_models),
        "missingCompiled": missing_compiled,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2), encoding="utf-8")

    print(
        f"Better Particles closure: {len(targets)} roots, {len(closure)} particles, "
        f"{len(local_resources)} local resources, {len(external_resources)} external references."
    )
    if missing_root_targets:
        print("Missing runtime targets:")
        for resource in sorted(missing_root_targets):
            print(f"  {resource}")
    if missing_child_references:
        print("Missing child particle references inside converted mod assets:")
        for resource in sorted(missing_child_references):
            print(f"  {resource}")
    if legacy_models:
        print("Legacy VMDL wrappers rejected by current CS2:")
        for resource in sorted(legacy_models):
            print(f"  {resource}")
    if external_models:
        print("External model resources required by the converted mod closure:")
        for resource in external_models:
            print(f"  {resource}")
    if missing_compiled:
        print("Local resources missing compiled output:")
        for resource in missing_compiled:
            print(f"  {resource}")
    return 1 if missing_root_targets or legacy_models or external_models or missing_compiled else 0


if __name__ == "__main__":
    raise SystemExit(main())
