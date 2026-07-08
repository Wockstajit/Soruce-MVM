#!/usr/bin/env python3
"""Port jahpeg's Source 1 player ragdoll metadata into CS2 ModelDoc files.

VRF supplies each current CS2 agent's meshes, skeleton and stock physics shapes.  The
Source 1 PHY supplies the authored body masses, damping and joint limits.  This tool
combines them without shipping the legacy PHY sidecars or touching hostage/audio data.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
DEFAULT_ADDON = REPO / "fx" / "sources" / "improved-ragdolls"
DEFAULT_VRF = Path(r"C:\Users\ayden\Documents\Github Projects\ValveResourceFormat\CLI\bin\Release\Source2Viewer-CLI.exe")
DEFAULT_VPK = Path(r"F:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\csgo\pak01_dir.vpk")

PLAYER_MODEL_RE = re.compile(r"^agents/models/(?:ctm|tm)_[^/]+/(?:ctm|tm)_[^/]+\.vmdl$", re.I)
OUTPUT_PREFIX = Path("models/filmmaker/improved_ragdolls")
CS2_TARGET_TOTAL_MASS = 272.0
BLOCK_RE = re.compile(r"(?P<kind>solid|ragdollconstraint)\s*\{(?P<body>.*?)\}", re.I | re.S)
PAIR_RE = re.compile(r'"([^\"]+)"\s+"([^\"]*)"')


@dataclass(frozen=True)
class Body:
    index: int
    name: str
    parent: str
    mass: float
    damping: float
    angular_damping: float
    inertia: float


@dataclass(frozen=True)
class Joint:
    parent: int
    child: int
    xmin: float
    xmax: float
    ymin: float
    ymax: float
    zmin: float
    zmax: float
    friction: float


def canonical(name: str) -> str:
    return name.lower()


def read_phy_metadata(path: Path) -> tuple[list[Body], list[Joint], float]:
    text = path.read_bytes().decode("ascii", "ignore")
    start = text.find("solid {")
    if start < 0:
        raise ValueError(f"No ragdoll metadata found in {path}")
    text = text[start:]
    bodies: list[Body] = []
    joints: list[Joint] = []
    for match in BLOCK_RE.finditer(text):
        values = dict(PAIR_RE.findall(match.group("body")))
        if match.group("kind").lower() == "solid":
            bodies.append(Body(
                index=int(values["index"]), name=canonical(values["name"]),
                parent=canonical(values.get("parent", "")), mass=float(values["mass"]),
                damping=float(values["damping"]), angular_damping=float(values["rotdamping"]),
                inertia=float(values["inertia"]),
            ))
        else:
            frictions = [float(values[f"{axis}friction"]) for axis in "xyz"]
            joints.append(Joint(
                parent=int(values["parent"]), child=int(values["child"]),
                xmin=float(values["xmin"]), xmax=float(values["xmax"]),
                ymin=float(values["ymin"]), ymax=float(values["ymax"]),
                zmin=float(values["zmin"]), zmax=float(values["zmax"]),
                friction=max(frictions),
            ))
    total_match = re.search(r'"totalmass"\s+"([0-9.]+)"', text, re.I)
    total_mass = float(total_match.group(1)) if total_match else sum(body.mass for body in bodies)
    if len(bodies) != 21 or len(joints) != 20:
        raise ValueError(f"Expected 21 bodies/20 joints in {path}, got {len(bodies)}/{len(joints)}")
    scale = CS2_TARGET_TOTAL_MASS / total_mass
    normalized = [Body(
        index=body.index, name=body.name, parent=body.parent,
        mass=body.mass * scale, damping=body.damping,
        angular_damping=body.angular_damping, inertia=body.inertia,
    ) for body in bodies]
    return sorted(normalized, key=lambda body: body.index), joints, total_mass


def select_shared_player_phy(addon: Path) -> tuple[Path, int]:
    canonical_profile = addon / "player-profile.phy"
    if canonical_profile.is_file():
        digest = hashlib.sha256(canonical_profile.read_bytes()).hexdigest()
        expected = "3efdd600926fbde8e6f6c5ea4b6460d1f6dee3732d1bd109a55ccdd5da84b27c"
        if digest != expected:
            raise ValueError(f"Canonical player PHY hash changed: {digest}")
        return canonical_profile, 239

    candidates = list((addon / "models" / "player" / "custom_player" / "legacy").glob("*.phy"))
    if not candidates:
        raise FileNotFoundError(f"No legacy player PHY files under {addon}")
    groups: dict[str, list[Path]] = {}
    for path in candidates:
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        groups.setdefault(digest, []).append(path)
    shared = max(groups.values(), key=len)
    if len(shared) < 200:
        raise ValueError(f"Shared player PHY group is unexpectedly small ({len(shared)})")
    return shared[0], len(shared)


def indent_block(text: str, tabs: int) -> str:
    prefix = "\t" * tabs
    return "\n".join(prefix + line if line else line for line in text.splitlines())


def body_markup_node(bodies: list[Body]) -> str:
    children = []
    for body in bodies:
        children.append(f'''{{
\t_class = "PhysicsBodyMarkup"
\tname = "{body.name}"
\ttarget_body = "{body.name}"
\ttag = "improved_ragdoll"
\tmass_override = {body.mass:.6f}
\tinertia_scale = {body.inertia:.6f}
\tlinear_damping = {body.damping:.6f}
\tangular_damping = {body.angular_damping:.6f}
}}''')
    joined = ",\n".join(indent_block(child, 3) for child in children)
    return f'''{{
\t_class = "PhysicsBodyMarkupList"
\tchildren =
\t[
{joined},
\t]
}}'''


def joint_node(bodies: list[Body], joints: list[Joint]) -> str:
    by_index = {body.index: body for body in bodies}
    children = []
    for joint in joints:
        parent = by_index[joint.parent].name
        child = by_index[joint.child].name
        swing = max(abs(joint.ymin), abs(joint.ymax), abs(joint.zmin), abs(joint.zmax))
        children.append(f'''{{
\t_class = "PhysicsJointConical"
\tname = "{parent}_to_{child}"
\tparent_body = "{parent}"
\tchild_body = "{child}"
\tcollision_enabled = false
\tenable_swing_limit = true
\tswing_limit = {swing:.6f}
\tenable_twist_limit = true
\tmin_twist_angle = {joint.xmin:.6f}
\tmax_twist_angle = {joint.xmax:.6f}
\tfriction = {joint.friction:.6f}
}}''')
    joined = ",\n".join(indent_block(child, 3) for child in children)
    return f'''{{
\t_class = "PhysicsJointList"
\tchildren =
\t[
{joined},
\t]
}}'''


def balanced_block(text: str, class_name: str) -> tuple[int, int]:
    marker = f'_class = "{class_name}"'
    marker_at = text.find(marker)
    if marker_at < 0:
        raise ValueError(f"Missing {class_name}")
    start = text.rfind("{", 0, marker_at)
    depth = 0
    quoted = False
    escaped = False
    for pos in range(start, len(text)):
        char = text[pos]
        if quoted:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
            continue
        if char == '"':
            quoted = True
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return start, pos + 1
    raise ValueError(f"Unterminated {class_name}")


def extract_hitbox_shape(text: str, bone: str) -> str | None:
    for class_name in ("HitboxCapsule", "HitboxSphere"):
        offset = 0
        while True:
            found = text.find(f'_class = "{class_name}"', offset)
            if found < 0:
                break
            start = text.rfind("{", 0, found)
            _, end = balanced_block(text[start:], class_name)
            block = text[start:start + end]
            parent = re.search(r'parent_bone\s*=\s*"([^\"]+)"', block)
            if parent and canonical(parent.group(1)) == bone:
                radius = re.search(r'radius\s*=\s*([-0-9.]+)', block)
                point0 = re.search(r'point0\s*=\s*(\[[^\]]+\])', block)
                point1 = re.search(r'point1\s*=\s*(\[[^\]]+\])', block)
                if radius and point0 and point1:
                    return f'''{{
\t_class = "PhysicsShapeCapsule"
\tparent_bone = "{bone}"
\tsurface_prop = "playerflesh"
\tcollision_tags = ""
\tradius = {radius.group(1)}
\tpoint0 = {point0.group(1)}
\tpoint1 = {point1.group(1)}
\tname = "{bone}"
}}'''
            offset = start + end
    return None


def add_missing_shapes(text: str, bodies: list[Body]) -> str:
    start, end = balanced_block(text, "PhysicsShapeList")
    shape_block = text[start:end]
    existing = {canonical(name) for name in re.findall(r'parent_bone\s*=\s*"([^\"]+)"', shape_block)}
    missing_nodes = []
    for body in bodies:
        if body.name in existing:
            continue
        node = extract_hitbox_shape(text, body.name)
        if node is None and body.name.startswith("clavicle_"):
            node = f'''{{
\t_class = "PhysicsShapeSphere"
\tparent_bone = "{body.name}"
\tsurface_prop = "playerflesh"
\tcollision_tags = ""
\tradius = 2.5
\tcenter = [ 0.0, 0.0, 0.0 ]
\tname = "{body.name}"
}}'''
        if node is None:
            raise ValueError(f"Cannot derive physics shape for missing body {body.name}")
        missing_nodes.append(indent_block(node, 4) + ",")
    if not missing_nodes:
        return text
    children_close = shape_block.rfind("]")
    updated = shape_block[:children_close] + "\n" + "\n".join(missing_nodes) + "\n\t\t\t" + shape_block[children_close:]
    return text[:start] + updated + text[end:]


def remove_vrf_body_markup(text: str) -> str:
    """Remove VRF's generic rendering of Valve's existing body markup.

    ModelDoc permits only one markup per target body. VRF emits compiled
    CPhysicsBodyGameMarkupData as GenericGameData, while this converter emits editable
    PhysicsBodyMarkup nodes, so the generic predecessor must be replaced.
    """
    marker = 'game_class = "CPhysicsBodyGameMarkupData"'
    marker_at = text.find(marker)
    if marker_at < 0:
        raise ValueError("Missing Valve CPhysicsBodyGameMarkupData block")
    start = text.rfind("{", 0, marker_at)
    depth = 0
    quoted = False
    for pos in range(start, len(text)):
        char = text[pos]
        if char == '"':
            quoted = not quoted
        elif not quoted and char == "{":
            depth += 1
        elif not quoted and char == "}":
            depth -= 1
            if depth == 0:
                end = pos + 1
                while end < len(text) and text[end] in " \t\r\n,":
                    end += 1
                return text[:start] + text[end:]
    raise ValueError("Unterminated CPhysicsBodyGameMarkupData block")


def patch_model(path: Path, bodies: list[Body], joints: list[Joint]) -> None:
    text = path.read_text(encoding="utf-8-sig")
    text = remove_vrf_body_markup(text)
    text = add_missing_shapes(text, bodies)
    shape_start, _ = balanced_block(text, "PhysicsShapeList")
    insertion = indent_block(body_markup_node(bodies), 3) + ",\n" + indent_block(joint_node(bodies, joints), 3) + ",\n\t\t\t"
    text = text[:shape_start] + insertion + text[shape_start:]
    body_count = text.count('_class = "PhysicsBodyMarkup"')
    joint_count = text.count('_class = "PhysicsJointConical"')
    shape_count = len(re.findall(r'_class = "PhysicsShape(?:Capsule|Sphere)"', text))
    if (body_count, joint_count, shape_count) != (21, 20, 21):
        raise ValueError(
            f"{path}: expected 21 body markups/20 joints/21 shapes, "
            f"got {body_count}/{joint_count}/{shape_count}"
        )
    path.write_text(text, encoding="utf-8", newline="\n")


def run_vrf(vrf: Path, vpk: Path, output: Path) -> None:
    command = [str(vrf), "-i", str(vpk), "-d", "-f", "agents/models/", "-e", "vmdl_c", "-o", str(output)]
    subprocess.run(command, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--content-root", type=Path, required=True)
    parser.add_argument("--addon-root", type=Path, default=DEFAULT_ADDON)
    parser.add_argument("--vrf-cli", type=Path, default=DEFAULT_VRF)
    parser.add_argument("--cs2-vpk", type=Path, default=DEFAULT_VPK)
    args = parser.parse_args()

    phy, duplicate_count = select_shared_player_phy(args.addon_root)
    bodies, joints, total_mass = read_phy_metadata(phy)
    args.content_root.mkdir(parents=True, exist_ok=True)
    run_vrf(args.vrf_cli, args.cs2_vpk, args.content_root)
    shared_models = args.content_root / "agents" / "models" / "shared"
    if shared_models.exists():
        shutil.rmtree(shared_models)

    models = []
    for path in args.content_root.glob("agents/models/**/*.vmdl"):
        relative = path.relative_to(args.content_root).as_posix()
        if PLAYER_MODEL_RE.match(relative):
            patch_model(path, bodies, joints)
            output_relative = OUTPUT_PREFIX / Path(relative)
            output_path = args.content_root / output_relative
            output_path.parent.mkdir(parents=True, exist_ok=True)
            if output_path.exists():
                output_path.unlink()
            shutil.move(str(path), str(output_path))
            models.append(output_relative.as_posix())
    if not models:
        raise RuntimeError("VRF produced no CT/T player ModelDoc files")

    report = args.content_root / "improved-ragdolls-report.txt"
    report.write_text(
        f"source_phy={phy}\nshared_duplicates={duplicate_count}\n"
        f"body_count={len(bodies)}\njoint_count={len(joints)}\n"
        f"source_total_mass={total_mass:.6f}\ntarget_total_mass={CS2_TARGET_TOTAL_MASS:.6f}\n"
        f"model_count={len(models)}\n" + "\n".join(models) + "\n",
        encoding="utf-8",
    )
    print(f"Prepared {len(models)} CS2 player models from {duplicate_count} identical legacy PHY files.")
    print(
        f"Ragdoll profile: {len(bodies)} bodies, {len(joints)} joints, "
        f"mass {total_mass:g} -> {CS2_TARGET_TOTAL_MASS:g}."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
