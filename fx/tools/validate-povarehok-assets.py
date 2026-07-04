"""Prepare and validate the runtime-selected Povarehok resource closure."""

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
# FXRULE, FXRULE_MODERN, and FXRULE_MODERN_BP share the first three args (match, pack,
# name -> the Povarehok variant targets); FXRULE_MODERN adds a modern-relative
# fourth, FXRULE_MODERN_BP reuses the Povarehok target as its Modern target.
RULE_RE = re.compile(r'FXRULE(?:_MODERN(?:_BP)?)?\(\s*"[^"]+"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"')
MODERN_RULE_RE = re.compile(
    r'FXRULE_MODERN\(\s*"[^"]+"\s*,\s*"[^"]+"\s*,\s*"[^"]+"\s*,\s*"([^"]+)"\s*\)'
)
# Raw table entries (non-macro) that name a modern target directly.
MODERN_PATH_RE = re.compile(r'"(particles/filmmaker/modern/[^"]+\.vpcf)"')
MONEY_RE = re.compile(r'kMoneyBurst\s*=\s*"([^"]+\.vpcf)"')
RESOURCE_RE = re.compile(r'resource:"([^"]+)"')
# Spray-gated barrel-smoke wrappers (kSprayPairs in ParticleFxSpray.cpp): these swap
# TARGETS are only ever assembled at runtime via string concatenation inside the
# MODSPRAY/BPSPRAY macros (a literal + a macro-arg identifier + a literal), so
# RULE_RE's whole-string match never sees them and they were silently pruned as
# "unreferenced" before compilation (bug 2026-07-03: "mvm_spray_*.vpcf was not
# precached correctly" in-game, despite postprocess_povarehok.py/postprocess_modern.py
# having just generated them). Seed them explicitly from the macro invocations instead.
MODSPRAY_RE = re.compile(r'MODSPRAY\("([^"]+)"\)')
BPSPRAY_RE = re.compile(r'BPSPRAY\("[^"]+"\s*,\s*"([^"]+)"\)')
MODERN_MUZZLE_DIR = "particles/filmmaker/modern/arc9_fas_muzzleflashes"
PVRH_WEAPON_DIR = "particles/filmmaker/povarehok/regular/weapons/cs_weapon_fx"
COMPILED_EXTENSIONS = {".vpcf", ".vmat", ".vtex", ".vmdl", ".vsnap"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--particle-fx-cpp", type=Path, required=True)
    parser.add_argument("--content-root", type=Path, required=True)
    parser.add_argument("--game-root", type=Path, required=True)
    parser.add_argument("--file-list", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--validate-compiled", action="store_true")
    parser.add_argument("--require-modern", action="store_true",
                        help="also validate the MW2019 modern-pack targets (pass when the "
                             "GMod sources were available to the converter)")
    parser.add_argument("--emit-closure", type=Path, default=None,
                        help="write the full keep-set of content-relative source files the "
                             "runtime needs (particle closure + referenced vmat/vtex/vmdl, "
                             "expanded through model/material resource refs). The converter "
                             "uses this pre-compile to prune everything else, so only ~15%% "
                             "of the mod's assets get compiled/shipped.")
    return parser.parse_args()


def runtime_targets(cpp_path: Path, require_modern: bool) -> list[str]:
    # The ParticleFx subsystem is split across focused translation units (2026-07-04):
    # the FXRULE variant tables live in ParticleFxRules.cpp, MODSPRAY/BPSPRAY in
    # ParticleFxSpray.cpp, kMoneyBurst in ParticleFxInternal.h. The converter still
    # passes ParticleFx.cpp; scan every sibling ParticleFx* source so the closure
    # covers the whole subsystem no matter which file a table lives in.
    siblings = sorted(cpp_path.parent.glob("ParticleFx*.cpp")) + sorted(
        cpp_path.parent.glob("ParticleFx*.h")
    )
    files = siblings if siblings else [cpp_path]
    text = "\n".join(f.read_text(encoding="utf-8") for f in files)
    targets: set[str] = set()
    for table in ARRAY_RE.finditer(text):
        category = table.group("category")
        less_variant = "less/impacts" if category == "Impacts" else "less/smoke"
        for pack, name in RULE_RE.findall(table.group("body")):
            # "classic" was dropped 2026-07-02 (byte-identical to regular; source
            # folder deleted); On now targets regular directly.
            for variant in ("regular", less_variant):
                targets.add(
                    f"particles/filmmaker/povarehok/{variant}/{pack}/{name}.vpcf"
                )
    if require_modern:
        for rel in MODERN_RULE_RE.findall(text):
            targets.add(f"particles/filmmaker/modern/{rel}.vpcf")
        for path in MODERN_PATH_RE.findall(text):
            targets.add(path)
        for name in MODSPRAY_RE.findall(text):
            targets.add(f"{MODERN_MUZZLE_DIR}/mvm_spray_{name}.vpcf")
    # BPSPRAY wrappers always resolve under povarehok/regular (see the BPSPRAY macro
    # in ParticleFxSpray.cpp), regardless of which On variant table invokes them.
    for name in BPSPRAY_RE.findall(text):
        targets.add(f"{PVRH_WEAPON_DIR}/mvm_spray_{name}.vpcf")
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
            if normalized.startswith("particles/filmmaker/") and normalized.endswith(".vpcf"):
                pending.append((normalized, False))
    return particle_closure, references, missing_root_targets, missing_child_references


def main() -> int:
    args = parse_args()
    content_root = args.content_root.resolve()
    game_root = args.game_root.resolve()
    targets = runtime_targets(args.particle_fx_cpp.resolve(), args.require_modern)
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

    if args.emit_closure is not None:
        # Expand the keep-set through refs the vpcf walk does not chase: converted
        # models (ModelDoc kv3) and materials carry their own resource:"..." refs
        # (a model's materials, a material's textures). Without this, pruning
        # would delete e.g. the gib models' vmats and they'd compile textureless.
        keep = set(local_resources)
        pending = deque(keep)
        while pending:
            resource = pending.popleft()
            path = source_path(content_root, resource)
            if not path.is_file() or path.suffix.lower() not in {".vmdl", ".vmat", ".vpcf"}:
                continue
            for reference in RESOURCE_RE.findall(
                    path.read_text(encoding="utf-8", errors="replace")):
                normalized = reference.replace("\\", "/")
                if normalized in keep:
                    continue
                if source_path(content_root, normalized).is_file():
                    keep.add(normalized)
                    pending.append(normalized)
        args.emit_closure.parent.mkdir(parents=True, exist_ok=True)
        args.emit_closure.write_text("\n".join(sorted(keep)) + "\n", encoding="utf-8")
        print(f"Closure keep-set: {len(keep)} content files -> {args.emit_closure}")

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
        f"Povarehok closure: {len(targets)} roots, {len(closure)} particles, "
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
