"""CS2-specific cleanup for converted Better Particles VPCFs.

This does not create placeholder effects. It adjusts the converted Source 1
assets where Source 2 control-point / viewmodel behavior makes the original
values unusable in CS2.
"""

from __future__ import annotations

import argparse
import re
from collections import deque
from pathlib import Path


RESOURCE_RE = re.compile(r'resource:"([^"]+)"')
NUMBER_RE = re.compile(r"(-?\d+(?:\.\d+)?)")

VARIANTS = ("classic", "classic_updated", "less_impacts", "less_smoke")
MUZZLE_ROOTS = (
    "weapon_muzzle_flash_assaultrifle_fp.vpcf",
    "weapon_muzzle_flash_shotgun_fp.vpcf",
    "weapon_muzzle_flash_huntingrifle_fp.vpcf",
    "weapon_muzzle_flash_smg_fp.vpcf",
    "weapon_muzzle_flash_smg_silenced_fp.vpcf",
    "weapon_muzzle_flash_pistol_fp.vpcf",
)
MOLOTOV_GROUNDFIRE_ROOTS = (
    "molotov_groundfire_00high.vpcf",
    "molotov_fire01.vpcf",
)
MOLOTOV_EXPLOSION_ROOTS = (
    "molotov_explosion.vpcf",
)

# These children are cinematic/screen-space Source 1 pieces that do not match
# CS2's sustained inferno control points. They produce the giant rectangle and
# particles visibly attracting back to the molotov control point.
MOLOTOV_REMOVE_CHILDREN = {
    "molotov_smoke_screen.vpcf",
    "extinguish_fire.vpcf",
    "molotov_groundfire_main_center.vpcf",
    "molotov_groundfire_main_fancy.vpcf",
    "ac_rpg_explosion_air_smoke_a_copy.vpcf",
    "ac_grenade_explosion_smoketrail_a.vpcf",
    "realistic_campfire_glow.vpcf",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--content-root", type=Path, required=True)
    # The staged Source 1 tree (for reading original VMTs). Defaults to the
    # converter's layout: <OutputRoot>/source1_game next to source2/.
    parser.add_argument("--source1-root", type=Path)
    return parser.parse_args()


def resource_path(root: Path, resource: str) -> Path:
    return root.joinpath(*resource.replace("\\", "/").split("/"))


def child_refs(path: Path) -> list[str]:
    if not path.is_file():
        return []
    text = path.read_text(encoding="utf-8")
    return [
        value.replace("\\", "/")
        for value in RESOURCE_RE.findall(text)
        if value.replace("\\", "/").endswith(".vpcf")
    ]


def collect_closure(root: Path, roots: list[str]) -> set[Path]:
    pending = deque(roots)
    seen: set[str] = set()
    paths: set[Path] = set()
    while pending:
        resource = pending.popleft().replace("\\", "/")
        if resource in seen:
            continue
        seen.add(resource)
        path = resource_path(root, resource)
        if not path.is_file():
            continue
        paths.add(path)
        pending.extend(child_refs(path))
    return paths


def scale_number(line: str, factor: float, cap: float | None = None, integer: bool = False) -> str:
    match = NUMBER_RE.search(line)
    if not match:
        return line
    value = float(match.group(1)) * factor
    if cap is not None and value > cap:
        value = cap
    if integer:
        replacement = str(max(0, int(round(value))))
    else:
        replacement = f"{value:.4f}".rstrip("0").rstrip(".")
    return line[: match.start(1)] + replacement + line[match.end(1) :]


def tone_down(path: Path, *, alpha: float, radius: float, overbright: float) -> bool:
    text = path.read_text(encoding="utf-8")
    out: list[str] = []
    changed = False
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        new_line = line
        if stripped.startswith("m_flOverbrightFactor"):
            new_line = scale_number(line, overbright, cap=1.15)
        elif stripped.startswith("m_flAddSelfAmount"):
            new_line = scale_number(line, overbright, cap=0.5)
        elif stripped.startswith("m_flAlphaScale"):
            new_line = scale_number(line, alpha, cap=0.65)
        elif stripped.startswith("m_nAlphaMin") or stripped.startswith("m_nAlphaMax"):
            new_line = scale_number(line, alpha, integer=True)
        elif (
            stripped.startswith("m_flRadiusMin")
            or stripped.startswith("m_flRadiusMax")
            or stripped.startswith("m_flConstantRadius")
        ):
            new_line = scale_number(line, radius)
        if new_line != line:
            changed = True
        out.append(new_line)
    if changed:
        path.write_text("".join(out), encoding="utf-8")
    return changed


CHILD_REF_PATTERN = re.compile(
    r"\n\t\t\{\n\t\t\tm_ChildRef = resource:\"(?P<resource>[^\"]+)\""
    r"(?:\n\t\t\tm_flDelay = [^\n]+)?\n\t\t\},",
    re.MULTILINE,
)


def remove_child_refs(path: Path, child_names: set[str]) -> bool:
    text = path.read_text(encoding="utf-8")

    def repl(match: re.Match[str]) -> str:
        name = match.group("resource").replace("\\", "/").rsplit("/", 1)[-1]
        return "" if name in child_names else match.group(0)

    new_text = CHILD_REF_PATTERN.sub(repl, text)
    if new_text != text:
        path.write_text(new_text, encoding="utf-8")
        return True
    return False


def strip_dead_child_refs(root: Path) -> int:
    """Remove child references to converted-mod particles that do not exist.

    The mod's Source 1 PCFs reference a handful of children that are absent
    from the mod itself (impact_armor_cheap, weapon_shell_casing_9mm_fallback,
    ...). After conversion those become dangling resource refs and CS2 logs
    'Failed loading resource ... ERROR_FILEOPEN' for each at load time. Only
    refs into the betterparticles namespace are considered -- anything else
    (stock CS2 resources) is left alone.
    """
    # Same problem, different fields: m_hFallback (low-quality fallback system)
    # and m_pszCullReplacementName also name mod particles that don't exist.
    # Dropping the line reverts to the engine default (no fallback/replacement).
    dead_line_pattern = re.compile(
        r"\n\t*(?:m_hFallback = resource|m_pszCullReplacementName = resource)"
        r":\"(?P<resource>[^\"]+)\"",
        re.MULTILINE,
    )

    changed = 0
    for path in root.joinpath("particles", "filmmaker", "betterparticles").rglob("*.vpcf"):
        text = path.read_text(encoding="utf-8")

        def missing(resource: str) -> bool:
            resource = resource.replace("\\", "/")
            return (
                resource.startswith("particles/filmmaker/betterparticles/")
                and not resource_path(root, resource).is_file()
            )

        def repl(match: re.Match[str]) -> str:
            return "" if missing(match.group("resource")) else match.group(0)

        new_text = CHILD_REF_PATTERN.sub(repl, text)
        new_text = dead_line_pattern.sub(repl, new_text)
        if new_text != text:
            path.write_text(new_text, encoding="utf-8")
            changed += 1
    return changed


def iter_renderer_blocks(text: str):
    """Yield (start, end) spans of elements inside m_Renderers = [ ... ] arrays."""
    for arr in re.finditer(r"m_Renderers = \n(\t*)\[", text):
        depth = 0
        i = arr.end()
        start = None
        while i < len(text):
            c = text[i]
            if c == "{":
                if depth == 0:
                    start = i
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0 and start is not None:
                    end = i + 1
                    if i + 1 < len(text) and text[i + 1] == ",":
                        end += 1
                    yield (start, end)
                    start = None
            elif c == "]" and depth == 0:
                break
            i += 1


VMT_BASETEXTURE_RE = re.compile(r'"?\$basetexture"?\s+"([^"\n]+)"', re.IGNORECASE)


def fix_textureless_renderers(root: Path, source1_root: Path) -> tuple[int, int]:
    """Sprite renderers with a material reference but NO m_hTexture draw solid
    white quads in CS2: the modern sprite renderer uses m_hTexture only and
    ignores the legacy m_hMaterial (converted-or-not -- effects with dangling
    materials but valid textures render correctly). This is the molotov /
    explosion 'huge white square': Source 1 heat-distortion (warp/refract)
    quads and screen overlays. Repair strategy, in order:
      1. If the original VMT names a $basetexture whose converted .vtex
         exists, inject m_hTexture so the REAL mod texture renders.
      2. Else remove the renderer block (an invisible emitter beats a white
         quad; distortion effects have no Source 2 equivalent here anyway).
    """
    repaired = removed = 0
    for path in root.joinpath("particles", "filmmaker", "betterparticles").rglob("*.vpcf"):
        text = path.read_text(encoding="utf-8")
        edits = []  # (start, end, replacement)
        for start, end in iter_renderer_blocks(text):
            block = text[start:end]
            # exact key only: m_hNormalTexture (refract quads) must NOT count
            if re.search(r"(?<![A-Za-z_])m_hTexture\s*=", block):
                continue
            mat = re.search(r'm_hMaterial = resource:"([^"]+)"', block)
            if not mat:
                continue
            mat_res = mat.group(1).replace("\\", "/")
            vmt = source1_root / Path(mat_res).with_suffix(".vmt")
            new_tex = None
            if vmt.is_file():
                base = VMT_BASETEXTURE_RE.search(vmt.read_text(encoding="utf-8", errors="ignore"))
                if base:
                    tex_res = "materials/" + base.group(1).replace("\\", "/").lower().strip("/") + ".vtex"
                    if resource_path(root, tex_res).is_file():
                        new_tex = tex_res
            if new_tex:
                insert_at = mat.end()
                indent = block[: mat.start()].rsplit("\n", 1)[-1]
                block = (
                    block[:insert_at]
                    + f'\n{indent}m_hTexture = resource:"{new_tex}"'
                    + block[insert_at:]
                )
                edits.append((start, end, block))
                repaired += 1
            else:
                edits.append((start, end, ""))
                removed += 1
        if edits:
            for start, end, replacement in reversed(edits):
                text = text[:start] + replacement + text[end:]
            path.write_text(text, encoding="utf-8")
    return repaired, removed


# $DUALSEQUENCE spritecards blend a second sheet sequence into the first
# (alpha-from-0 / rgb-from-1 + max-luminance modes). The reconstructed .mks
# sheets are single-mode, so the converted combine keys sample the WRONG
# frames and produce dark fringes ("black ring" smoke). Dropping the keys
# reverts the renderer to plain sequence-0 sampling.
DUAL_SEQUENCE_LINE_RE = re.compile(
    r"\n\t*(?:m_nSequenceCombineMode|m_bMaxLuminanceBlendingSequence0"
    r"|m_bMaxLuminanceBlendingSequence1|m_flZoomAmount1) = [^\n]+",
    re.MULTILINE,
)


def strip_dual_sequence_keys(root: Path) -> int:
    changed = 0
    for path in root.joinpath("particles", "filmmaker", "betterparticles").rglob("*.vpcf"):
        text = path.read_text(encoding="utf-8")
        new_text = DUAL_SEQUENCE_LINE_RE.sub("", text)
        if new_text != text:
            path.write_text(new_text, encoding="utf-8")
            changed += 1
    return changed


def variant_resource(variant: str, folder: str, name: str) -> str:
    return f"particles/filmmaker/betterparticles/{variant}/{folder}/{name}"


def main() -> int:
    args = parse_args()
    root = args.content_root.resolve()
    source1_root = (args.source1_root or root.parents[2] / "source1_game").resolve()
    changed_muzzle = 0
    changed_molotov = 0

    for variant in VARIANTS:
        muzzle_roots = [
            variant_resource(variant, "weapons/cs_weapon_fx", name)
            for name in MUZZLE_ROOTS
        ]
        for path in collect_closure(root, muzzle_roots):
            if tone_down(path, alpha=0.45, radius=0.72, overbright=0.35):
                changed_muzzle += 1

        molotov_roots = [
            variant_resource(variant, "inferno_fx", name)
            for name in MOLOTOV_GROUNDFIRE_ROOTS + MOLOTOV_EXPLOSION_ROOTS
        ]
        for resource in molotov_roots:
            path = resource_path(root, resource)
            if path.is_file() and remove_child_refs(path, MOLOTOV_REMOVE_CHILDREN):
                changed_molotov += 1
        for path in collect_closure(root, molotov_roots):
            # Molotov fire still comes from the real mod, but Source 2's lighting
            # makes the Source 1 overbright values dominate the frame.
            if tone_down(path, alpha=0.75, radius=0.82, overbright=0.55):
                changed_molotov += 1

    dead_children = strip_dead_child_refs(root)
    repaired, removed = fix_textureless_renderers(root, source1_root)
    dual_seq = strip_dual_sequence_keys(root)
    print(
        f"Post-processed Better Particles: {changed_muzzle} muzzle files, "
        f"{changed_molotov} molotov files, {dead_children} dead child refs stripped, "
        f"{repaired} textureless renderers repaired / {removed} removed, "
        f"{dual_seq} files de-dual-sequenced."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
