"""CS2-specific cleanup for converted Povarehok VPCFs. Also the pipeline's CLI
entry point: drives the shared repairs (postprocess_common) and Modern's own
gameplay composites (postprocess_modern) alongside Povarehok's own.

This does not create placeholder effects. It adjusts the converted Source 1
assets where Source 2 control-point / viewmodel behavior makes the original
values unusable in CS2.
"""

from __future__ import annotations

import argparse
import math
import re
from pathlib import Path

import postprocess_common as common
import postprocess_modern as modern


VARIANTS = ("regular", "less/impacts", "less/smoke")


def _fp_variant_res(res: str) -> str:
    assert res.endswith(".vpcf"), res
    return res[:-len(".vpcf")] + "_fp.vpcf"


def iter_povarehok_impact_vpcfs(root: Path):
    pvrh_root = root.joinpath("particles", "filmmaker", "povarehok")
    impact_dirs = {"impact_fx", "impact_fxmoney", "impact_fxsnow", "impact_fx_smoke"}
    for path in pvrh_root.rglob("*.vpcf"):
        if any(part.lower() in impact_dirs for part in path.parts):
            yield path


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
    # Run ONLY idempotent patch passes. The full pipeline is NOT rerunnable on
    # an already-processed tree (tone_down would compound), so these flags let
    # an existing converted tree be patched in place.
    parser.add_argument("--gameplay-composites-only", action="store_true")
    parser.add_argument("--runtime-impact-fixes-only", action="store_true")
    return parser.parse_args()


SMOKE_CHILD_HINT_RE = re.compile(r"(smoke|wisp|puff|plume)", re.IGNORECASE)


def tone_down_muzzle_closure(root: Path, muzzle_roots: list[str], *, alpha: float,
                              radius: float, overbright: float) -> int:
    """Tone down the (overbright) muzzle-flash roots' child-ref closure, but skip smoke
    children instead of blindly applying the flash's own harsh factors to them.

    Bug (2026-07-03, "On-mode barrel smoke is too dark"): collect_closure() walks the
    WHOLE child-ref graph from each flash root, which includes that flash's own per-shot
    smoke wisp child (weapon_muzzle_flash_smoke_small2 and friends -- see the
    patch_cs2_muzzle_smoke_alignment docstring). Applying alpha=0.45/overbright=0.35
    (tuned to tame the blown-out FLASH sprite) to that smoke child crushed it toward
    black. Smoke's own CS2 darkening (erroneous scene-lit shading) is already corrected
    separately by fix_lit_renderers' unlit/self-illum pass; it does not need -- and must
    not get -- the flash's darkening on top.
    """
    changed = 0
    for path in common.collect_closure(root, muzzle_roots):
        if SMOKE_CHILD_HINT_RE.search(path.name):
            continue
        if tone_down(path, alpha=alpha, radius=radius, overbright=overbright):
            changed += 1
    return changed


def tone_down(path: Path, *, alpha: float, radius: float, overbright: float) -> bool:
    text = path.read_text(encoding="utf-8")
    out: list[str] = []
    changed = False
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        new_line = line
        if stripped.startswith("m_flOverbrightFactor"):
            new_line = common.scale_number(line, overbright, cap=1.15)
        elif stripped.startswith("m_flAddSelfAmount"):
            new_line = common.scale_number(line, overbright, cap=0.5)
        elif stripped.startswith("m_flAlphaScale"):
            new_line = common.scale_number(line, alpha, cap=0.65)
        elif stripped.startswith("m_nAlphaMin") or stripped.startswith("m_nAlphaMax"):
            new_line = common.scale_number(line, alpha, integer=True)
        elif (
            stripped.startswith("m_flRadiusMin")
            or stripped.startswith("m_flRadiusMax")
            or stripped.startswith("m_flConstantRadius")
        ):
            new_line = common.scale_number(line, radius)
        if new_line != line:
            changed = True
        out.append(new_line)
    if changed:
        path.write_text("".join(out), encoding="utf-8")
    return changed


def variant_resource(variant: str, folder: str, name: str) -> str:
    return f"particles/filmmaker/povarehok/{variant}/{folder}/{name}"


IMPACT_SMOKE_HINT_RE = re.compile(
    r"(smoke|dust|puff|steam|vistasmoke|wd_gfx_steam|copyka228smoke|cursedgovno|burn)",
    re.IGNORECASE,
)
IMPACT_SMOKE_SKIP_RE = re.compile(r"(spark|glow|flare|star|tracer|ricochet)", re.IGNORECASE)


def _is_impact_smoke_renderer(path: Path, block: str) -> bool:
    resources = "\n".join(common.RESOURCE_RE.findall(block))
    haystack = f"{path.name}\n{resources}"
    if IMPACT_SMOKE_SKIP_RE.search(haystack):
        return False
    return bool(IMPACT_SMOKE_HINT_RE.search(haystack))


def fix_impact_smoke_blending(root: Path) -> tuple[int, int]:
    """Keep impact smoke/dust alpha-blended and neutral under heavy overlap.

    Source 1 impact dust relied on SpriteCard's alpha blending. Some converted
    impact smoke children come through additive and overbright, so stacked bullet
    impacts tint toward hot red/orange instead of just fading out.
    """
    changed_files = changed_renderers = 0
    for path in iter_povarehok_impact_vpcfs(root):
        text = path.read_text(encoding="utf-8")
        edits = []  # (start, end, replacement)
        for start, end in common.iter_renderer_blocks(text):
            block = text[start:end]
            if not _is_impact_smoke_renderer(path, block):
                continue
            new_block = common.ADD_BLEND_OUTPUT_LINE_RE.sub("", block)
            new_block = common.OVERBRIGHT_OUTPUT_LINE_RE.sub(
                r"\1m_flOverbrightFactor = 1.0", new_block
            )
            new_block = common.ADD_SELF_OUTPUT_LINE_RE.sub(r"\1m_flAddSelfAmount = 0.0", new_block)
            if new_block != block:
                edits.append((start, end, new_block))
                changed_renderers += 1
        if edits:
            for start, end, replacement in reversed(edits):
                text = text[:start] + replacement + text[end:]
            path.write_text(text, encoding="utf-8")
            changed_files += 1
    return changed_files, changed_renderers


VISTASMOKES_RE = re.compile(r"vistasmoke", re.IGNORECASE)
TEXTURE_LINE_RE = re.compile(r'm_hTexture\s*=\s*resource:"[^"]+"')
MATERIAL_LINE_RE = re.compile(r'm_hMaterial\s*=\s*resource:"[^"]+"')

# 21 unique systems (regular + less/impacts|smoke) — fire/explosion vistasmoke is untouched.
NON_FIRE_VISTASMOKESYSTEMS: dict[str, str] = {
    "impact_concrete_child_smoke": "impact_dust",
    "impact_dirt_child_smoke_puff": "impact_thin",
    "impact_generic_smoke_small": "impact_thin",
    "impact_generic_smoke_medium": "impact_thin",
    "impact_generic_smoke_large": "impact_thin",
    "impact_screen_smoke_small": "impact_thin",
    "impact_screen_smoke_medium": "impact_thin",
    "impact_screen_smoke_large": "impact_thin",
    "weapon_muzzle_flash_smoke_small": "muzzle",
    "weapon_muzzle_flash_smoke_small2": "muzzle",
    "weapon_muzzle_flash_smoke_small4": "muzzle",
    "weapon_shell_eject_smoke_assrifle2": "shell",
    "weapon_shell_eject_smoke_assrifle3": "shell",
    "weapon_shell_eject_smoke_awp3": "shell",
    "weapon_shell_eject_smoke_para3": "shell",
    "weapon_shell_eject_smoke_pistol3": "shell",
    "weapon_shell_eject_smoke_shotgun1": "shell",
    "weapon_shell_eject_smoke_shotgun2": "shell",
    "weapon_shell_eject_smoke_shotgun3": "shell",
    "weapon_shell_eject_smoke_smg1": "shell",
    "weapon_shell_eject_smoke_smg3": "shell",
}

INSURGENCY_MATERIAL_NAMES = {
    "impact_dust": "dust_puff",
    "impact_thin": "ins_animsmokethin_01",
    "muzzle": "ins_muzzle_smoke",
    "shell": "particle_shells",
}

FALLBACK_MATERIALS = {
    "impact_dust": "materials/particle/ac/insandstorm_t_thinsmoke_05_bc",
    "impact_thin": "materials/particle/ac/insandstorm_t_thinsmoke_01_bc",
    "muzzle": "materials/particle/smoke1/smoke1",
    "shell": "materials/particle/ac/sq_fulldustfront1_2",
}


def _material_exists(root: Path, resource: str) -> bool:
    rel = resource.replace("\\", "/")
    return (root / f"{rel}.vmat").is_file() or (root / f"{rel}.vtex").is_file()


def _resolve_smoke_material(root: Path, role: str) -> str:
    insurgency = f"materials/particle/insurgency/{INSURGENCY_MATERIAL_NAMES[role]}"
    if _material_exists(root, insurgency):
        return insurgency
    return FALLBACK_MATERIALS[role]


def _set_renderer_field(block: str, key: str, value: str) -> str:
    match = re.search(rf"(?m)^(\s*){re.escape(key)} = ", block)
    if match:
        indent = match.group(1)
        return re.sub(
            rf"(?m)^{re.escape(indent)}{re.escape(key)} = [^\n]+",
            f"{indent}{key} = {value}",
            block,
            count=1,
        )
    return re.sub(
        r'(_class = "C_OP_RenderSprites")',
        rf'\1\n\t\t\t{key} = {value}',
        block,
        count=1,
    )


def _remove_renderer_field(block: str, key: str) -> str:
    return re.sub(rf"(?m)^\s*{re.escape(key)} = [^\n]+\n?", "", block)


def _normalize_impact_smoke_renderer(block: str) -> str:
    block = _set_renderer_field(block, "m_bBlendFramesSeq0", "true")
    block = _set_renderer_field(block, "m_flOverbrightFactor", "1.0")
    block = _set_renderer_field(block, "m_flSelfIllumAmount", "0.0")
    block = _set_renderer_field(block, "m_nAnimationType", '"ANIMATION_TYPE_FIT_LIFETIME"')
    block = _set_renderer_field(block, "m_flAnimationRate", "0.6")
    block = _remove_renderer_field(block, "m_flAnimationRate2")
    return block


def _normalize_muzzle_smoke_renderer(block: str) -> str:
    block = _set_renderer_field(block, "m_bBlendFramesSeq0", "true")
    block = _remove_renderer_field(block, "m_flAnimationRate2")
    if "m_flAnimationRate" not in block:
        block = _set_renderer_field(block, "m_flAnimationRate", "8.0")
    return block


def _normalize_shell_smoke_renderer(block: str) -> str:
    block = _set_renderer_field(block, "m_bBlendFramesSeq0", "true")
    block = _set_renderer_field(block, "m_flOverbrightFactor", "2.0")
    block = _set_renderer_field(block, "m_nAnimationType", '"ANIMATION_TYPE_FIT_LIFETIME"')
    block = _set_renderer_field(block, "m_flAnimationRate", "0.5")
    block = _remove_renderer_field(block, "m_flAnimationRate2")
    return block


def _normalize_swapped_renderer(block: str, role: str) -> str:
    if role in ("impact_dust", "impact_thin"):
        return _normalize_impact_smoke_renderer(block)
    if role == "muzzle":
        return _normalize_muzzle_smoke_renderer(block)
    return _normalize_shell_smoke_renderer(block)


def _swap_renderer_textures(block: str, material_base: str) -> str:
    tex = f'resource:"{material_base}.vtex"'
    mat = f'resource:"{material_base}.vmat"'
    block = TEXTURE_LINE_RE.sub(f"m_hTexture = {tex}", block)
    block = MATERIAL_LINE_RE.sub(f"m_hMaterial = {mat}", block)
    return block


def _neutralize_impact_colors(text: str) -> str:
    text = re.sub(
        r"m_ColorMin = \[\d+, \d+, \d+, 255\]",
        "m_ColorMin = [120, 120, 120, 255]",
        text,
    )
    text = re.sub(
        r"m_ColorMax = \[\d+, \d+, \d+, 255\]",
        "m_ColorMax = [170, 170, 170, 255]",
        text,
    )
    return text


def iter_non_fire_vistasmoke_vpcfs(root: Path):
    pvrh_root = root / "particles" / "filmmaker" / "povarehok"
    if not pvrh_root.is_dir():
        return
    for path in pvrh_root.rglob("*.vpcf"):
        if any(part in ("inferno_fx", "explosions_fx", "explosions_fx2") for part in path.parts):
            continue
        role = NON_FIRE_VISTASMOKESYSTEMS.get(path.stem)
        if role is None:
            continue
        rel = path.relative_to(pvrh_root).as_posix()
        if "impact_fx/" not in rel and "weapons/cs_weapon_fx/" not in rel:
            continue
        yield path, role


def patch_non_fire_vistasmoke_replacements(root: Path) -> tuple[int, int]:
    """Retarget bullet-impact and weapon wisps off the volumetric vistasmoke sheet."""
    changed_files = changed_renderers = 0
    for path, role in iter_non_fire_vistasmoke_vpcfs(root):
        text = path.read_text(encoding="utf-8")
        if not VISTASMOKES_RE.search(text):
            continue
        material_base = _resolve_smoke_material(root, role)
        edits = []
        for start, end in common.iter_renderer_blocks(text):
            block = text[start:end]
            if not VISTASMOKES_RE.search(block):
                continue
            new_block = _swap_renderer_textures(block, material_base)
            new_block = _normalize_swapped_renderer(new_block, role)
            if new_block != block:
                edits.append((start, end, new_block))
                changed_renderers += 1
        if not edits:
            continue
        for start, end, replacement in reversed(edits):
            text = text[:start] + replacement + text[end:]
        if role.startswith("impact"):
            text = _neutralize_impact_colors(text)
        text = common.DUAL_SEQUENCE_LINE_RE.sub("", text)
        path.write_text(text, encoding="utf-8")
        changed_files += 1
    return changed_files, changed_renderers


# Animated smoke/dust SHEETS whose animation must play ONCE across the particle lifetime.
# Source 1 authored these with a fixed C_OP_RenderSprites frame rate (m_flAnimationRate) that
# lined up with the particle's lifetime; CS2 + the reconstructed .mks sheets instead LOOP the
# sheet (frame = age*rate mod frames), so a single shot's muzzle smoke -- and a single wall
# hit's impact smoke, which shares these same sheets on some weapons -- visibly re-plays the
# sprite over and over (user report 2026-07-06 "the smoke replays / plays the sprite over and
# over"). ANIMATION_TYPE_FIT_LIFETIME maps the sheet to [0..1] of the lifetime = plays exactly
# once, no wrap (this is what the already-correct impact_generic_smoke_* systems use). Scoped
# by TEXTURE to smoke/dust sheets only: muzzle-flash flames (snipermuzzle/pistolmuzzle/...),
# sparks, fire, and blood in the same folders keep their intentional fast fixed-rate flicker.
LOOPING_SMOKE_SHEET_RE = re.compile(
    r"(thinsmoke|insandstorm|wd_gfx_steam|ins_muzzle_smoke|ins_animsmoke|"
    r"beam_smoke|copyka228smoke|dust_puff|fas_dust|/smoke1/|sq_fulldustfront|"
    r"water_splash)",
    re.IGNORECASE,
)

# Sheets whose .mks sequences get their LOOP flag dropped (clamp_sheet_sequences):
# one-shot smoke/dust/steam/splash ONLY. Deliberately narrower than
# LOOPING_SMOKE_SHEET_RE's renderer-texture match: a bare "insandstorm" would also
# de-loop the pack's spark/blood/debris/airburst sheets, whose fast looping flicker is
# intentional (caught 2026-07-06 -- the first clamp run swept those in and had to be
# restored).
CLAMP_SMOKE_SHEET_RE = re.compile(
    r"(thinsmoke|animsmoke|ins_muzzle_smoke|wd_gfx_steam|copyka228smoke|dust_puff|"
    r"fas_dust|/smoke1/|smoke1\.mks|sq_fulldustfront|water_splash)",
    re.IGNORECASE,
)


def _iter_weapon_and_impact_vpcfs(root: Path):
    pvrh_root = root.joinpath("particles", "filmmaker", "povarehok")
    if pvrh_root.is_dir():
        for path in pvrh_root.rglob("*.vpcf"):
            rel = path.relative_to(pvrh_root).as_posix()
            if "weapons/cs_weapon_fx/" in rel or "impact_fx" in rel:
                yield path
    # The Modern pack's muzzle smoke (fas_dust_* sheets on muzzle_smoke_c,
    # muzzle_supressed_smoke_big, the suppressed flash, ...) replayed exactly the same
    # way but was missed by the original Povarehok-only walk (found 2026-07-06 while the
    # user still saw double-playing smoke after the first fix).
    modern_root = root.joinpath("particles", "filmmaker", "modern", "arc9_fas_muzzleflashes")
    if modern_root.is_dir():
        yield from modern_root.rglob("*.vpcf")


def fix_looping_smoke_animation(root: Path) -> tuple[int, int]:
    """Make weapon + bullet-impact smoke sprite sheets play ONCE (loop -> FIT_LIFETIME).

    The animation rate still MULTIPLIES under FIT_LIFETIME (frame = normalizedAge *
    rate * sheet time), so any rate > 1.0 overshoots the sheet and -- on a LOOP-flagged
    sequence -- wraps back to frame 0 and replays. Cap the rate at 1.0 on these smoke
    renderers (exactly one pass over the particle lifetime); rates <= 1.0 are authored
    pacing and stay. The sheets themselves are additionally clamped by
    common.clamp_sheet_sequences so fixed-rate overshoots hold the last frame too.

    Idempotent: FIT_LIFETIME and the rate cap are fixed literals. C_OP_RenderRopes wisps
    (weapon_muzzle_smoke_long's beam) are untouched -- only sprite renderers loop a sheet.
    """
    rate_re = re.compile(r"(?m)^(\s*)m_flAnimationRate = (\d+(?:\.\d+)?)(\s*)$")
    changed_files = changed_renderers = 0
    for path in _iter_weapon_and_impact_vpcfs(root):
        text = path.read_text(encoding="utf-8")
        edits = []
        for start, end in common.iter_renderer_blocks(text):
            block = text[start:end]
            if '_class = "C_OP_RenderSprites"' not in block:
                continue
            tex = re.search(r'm_hTexture = resource:"([^"]+)"', block)
            if not tex or not LOOPING_SMOKE_SHEET_RE.search(tex.group(1)):
                continue
            new_block = block
            if "ANIMATION_TYPE_FIT_LIFETIME" not in new_block:
                new_block = _set_renderer_field(
                    new_block, "m_nAnimationType", '"ANIMATION_TYPE_FIT_LIFETIME"'
                )
            new_block = rate_re.sub(
                lambda m: m.group(0) if float(m.group(2)) <= 1.0
                else f"{m.group(1)}m_flAnimationRate = 1.0{m.group(3)}",
                new_block,
            )
            if new_block != block:
                edits.append((start, end, new_block))
                changed_renderers += 1
        if edits:
            for start, end, replacement in reversed(edits):
                text = text[:start] + replacement + text[end:]
            path.write_text(text, encoding="utf-8")
            changed_files += 1
    return changed_files, changed_renderers


IMPACT_SETTLE_HINT_RE = re.compile(
    r"(clique|clump|chunk|chunks|debris|bits|gib|child_bounce|concrete_b|tile_b)",
    re.IGNORECASE,
)
IMPACT_SETTLE_SKIP_RE = re.compile(
    r"(spark|glow|trail|smoke|dust|blood|spurt|splash|water)",
    re.IGNORECASE,
)
BOUNCE_AMOUNT_LINE_RE = re.compile(r"(?m)^(\s*)m_flBounceAmount = [^\n]+")
IMPACT_SPIN_CLASS_RE = re.compile(r'_class = "C_OP_Spin(?:Yaw)?"')
SPIN_STOP_LINE_RE = re.compile(r"(?m)^(\s*)m_fSpinRateStopTime = (-?\d+(?:\.\d+)?)")
MAX_IMPACT_SPIN_SECONDS = 0.5


def _is_impact_debris_that_should_settle(path: Path, text: str) -> bool:
    name = path.name
    if IMPACT_SETTLE_SKIP_RE.search(name):
        return False
    collision_driven = "C_OP_WorldTraceConstraint" in text and (
        "C_OP_RenderModels" in text or "C_OP_Spin" in text
    )
    return collision_driven or bool(IMPACT_SETTLE_HINT_RE.search(name))


def _cap_spin_stop_time(block: str) -> str:
    match = SPIN_STOP_LINE_RE.search(block)
    if match:
        current = float(match.group(2))
        if current <= MAX_IMPACT_SPIN_SECONDS:
            return block
        return SPIN_STOP_LINE_RE.sub(
            rf"\1m_fSpinRateStopTime = {MAX_IMPACT_SPIN_SECONDS}", block, count=1
        )
    return re.sub(
        r'(_class = "C_OP_Spin(?:Yaw)?")',
        rf"\1\n\t\t\tm_fSpinRateStopTime = {MAX_IMPACT_SPIN_SECONDS}",
        block,
        count=1,
    )


def settle_impact_debris(root: Path) -> tuple[int, int, int]:
    """Stop physical bullet-impact chunks from bouncing or spinning indefinitely."""
    changed_files = changed_constraints = changed_spin_ops = 0
    for path in iter_povarehok_impact_vpcfs(root):
        text = path.read_text(encoding="utf-8")
        if not _is_impact_debris_that_should_settle(path, text):
            continue
        edits = []  # (start, end, replacement)
        for match in common.WORLD_TRACE_CLASS_RE.finditer(text):
            start, end = common.block_span(text, match.start())
            block = text[start:end]
            new_block = BOUNCE_AMOUNT_LINE_RE.sub(r"\1m_flBounceAmount = 0.0", block)
            if new_block != block:
                edits.append((start, end, new_block))
                changed_constraints += 1
        for match in IMPACT_SPIN_CLASS_RE.finditer(text):
            start, end = common.block_span(text, match.start())
            block = text[start:end]
            new_block = _cap_spin_stop_time(block)
            if new_block != block:
                edits.append((start, end, new_block))
                changed_spin_ops += 1
        if edits:
            for start, end, replacement in sorted(edits, reverse=True):
                text = text[:start] + replacement + text[end:]
            path.write_text(text, encoding="utf-8")
            changed_files += 1
    return changed_files, changed_constraints, changed_spin_ops


DIRECT_EMIT_COUNT_RE = re.compile(r"(?m)^(\s*m_nParticlesToEmit = )(\d+)(\s*)$")
RANDOM_EMIT_COUNT_RE = re.compile(r"(?m)^(\s*m_flRandom(?:Min|Max) = )(-?\d+(?:\.\d+)?)(\s*)$")


def _format_scaled_number(source: str, value: float) -> str:
    if "." not in source and value.is_integer():
        return str(int(value))
    return f"{value:.6f}".rstrip("0").rstrip(".")


def _halve_debris_emitter(block: str) -> str:
    def direct_repl(match: re.Match[str]) -> str:
        value = max(1, math.ceil(int(match.group(2)) * 0.5))
        return f"{match.group(1)}{value}{match.group(3)}"

    new = DIRECT_EMIT_COUNT_RE.sub(direct_repl, block)
    if re.search(r"m_nParticlesToEmit =\s*\n\s*\{", block):
        def random_repl(match: re.Match[str]) -> str:
            value = float(match.group(2)) * 0.5
            return f"{match.group(1)}{_format_scaled_number(match.group(2), value)}{match.group(3)}"

        new = RANDOM_EMIT_COUNT_RE.sub(random_repl, new)
    return new


def halve_less_impact_debris(root: Path) -> tuple[int, int]:
    """Halve physical debris emission in the Less impacts variant only.

    This runs only in the clean conversion pipeline, which is already documented as
    non-rerunnable because its other tone passes are multiplicative too.
    """
    changed_files = changed_emitters = 0
    for path in iter_povarehok_impact_vpcfs(root):
        if "less/impacts" not in path.as_posix():
            continue
        text = path.read_text(encoding="utf-8")
        if not _is_impact_debris_that_should_settle(path, text):
            continue
        edits = []
        for match in re.finditer(r'_class = "C_OP_InstantaneousEmitter"', text):
            start, end = common.block_span(text, match.start())
            block = text[start:end]
            new_block = _halve_debris_emitter(block)
            if new_block != block:
                edits.append((start, end, new_block))
                changed_emitters += 1
        if edits:
            for start, end, replacement in sorted(edits, reverse=True):
                text = text[:start] + replacement + text[end:]
            path.write_text(text, encoding="utf-8")
            changed_files += 1
    return changed_files, changed_emitters


IMPACT_WARM_SMOKE_FILES = (
    "impact_fx/copyka228smoke2.vpcf",
    "impact_fx/cursedgovno1.vpcf",
)


def fix_warm_impact_smoke_tints(root: Path) -> int:
    """Neutralize brown/orange impact-smoke tints that read as red when stacked."""
    changed = 0
    for rel in IMPACT_WARM_SMOKE_FILES:
        for variant in VARIANTS:
            path = common.resource_path(root, f"particles/filmmaker/povarehok/{variant}/{rel}")
            if not path.is_file():
                continue
            text = new = path.read_text(encoding="utf-8")
            new = re.sub(
                r"m_ColorMin = \[\d+, \d+, \d+, 255\]",
                "m_ColorMin = [120, 120, 120, 255]",
                new,
            )
            new = re.sub(
                r"m_ColorMax = \[\d+, \d+, \d+, 255\]",
                "m_ColorMax = [170, 170, 170, 255]",
                new,
            )
            new = re.sub(
                r"m_ConstantColor = \[\d+, \d+, \d+, 255\]",
                "m_ConstantColor = [140, 140, 140, 255]",
                new,
            )
            new = re.sub(r"m_flSelfIllumAmount = 1\.0", "m_flSelfIllumAmount = 0.0", new)
            new = common.ADD_BLEND_OUTPUT_LINE_RE.sub("", new)
            if new != text:
                path.write_text(new, encoding="utf-8")
                changed += 1
    return changed


PVRH_WEAPON_VARIANTS = ("regular", "less/smoke")


def _pvrh_weapon_dir(variant: str) -> str:
    return f"particles/filmmaker/povarehok/{variant}/weapons/cs_weapon_fx"


LESS_MUZZLE_SMOKE_SCALE = 0.9
MUZZLE_SMOKE_ALPHA_FIELDS = ("m_nAlphaMin", "m_nAlphaMax")
MUZZLE_SMOKE_RADIUS_FIELDS = (
    "m_flRadiusMin", "m_flRadiusMax", "m_flStartScale", "m_flEndScale"
)


def _scale_fields(block: str, fields: tuple[str, ...], factor: float, *, integer: bool) -> str:
    field_group = "|".join(re.escape(field) for field in fields)
    pattern = re.compile(rf"(?m)^(\s*(?:{field_group}) = )(-?\d+(?:\.\d+)?)(\s*)$")

    def repl(match: re.Match[str]) -> str:
        scaled = float(match.group(2)) * factor
        value = str(max(0, round(scaled))) if integer else _format_scaled_number(match.group(2), scaled)
        return f"{match.group(1)}{value}{match.group(3)}"

    return pattern.sub(repl, block)


def _reduce_muzzle_smoke_text(text: str) -> str:
    edits = []
    class_fields = {
        "C_INIT_RandomAlpha": (MUZZLE_SMOKE_ALPHA_FIELDS, True),
        "C_INIT_RandomRadius": (MUZZLE_SMOKE_RADIUS_FIELDS, False),
        "C_OP_InterpolateRadius": (MUZZLE_SMOKE_RADIUS_FIELDS, False),
    }
    for class_name, (fields, integer) in class_fields.items():
        for match in re.finditer(rf'_class = "{class_name}"', text):
            start, end = common.block_span(text, match.start())
            block = text[start:end]
            new_block = _scale_fields(block, fields, LESS_MUZZLE_SMOKE_SCALE, integer=integer)
            if new_block != block:
                edits.append((start, end, new_block))
    for start, end, replacement in sorted(edits, reverse=True):
        text = text[:start] + replacement + text[end:]
    return text


def _is_muzzle_smoke_leaf(path: Path) -> bool:
    name = path.name.lower()
    return "smoke" in name or ("ac_muzzle" in name and "trail" in name)


def write_less_muzzle_smoke_variant(root: Path, changed: list[str]) -> None:
    """Derive Less smoke leaves from repaired Regular leaves, then scale by 10%."""
    regular_dir = common.resource_path(root, _pvrh_weapon_dir("regular"))
    less_dir = common.resource_path(root, _pvrh_weapon_dir("less/smoke"))
    if not regular_dir.is_dir() or not less_dir.is_dir():
        return
    for regular_path in regular_dir.glob("*.vpcf"):
        if not _is_muzzle_smoke_leaf(regular_path):
            continue
        less_path = less_dir / regular_path.name
        if not less_path.is_file():
            continue
        text = regular_path.read_text(encoding="utf-8").replace(
            "/povarehok/regular/", "/povarehok/less/smoke/"
        )
        new_text = _reduce_muzzle_smoke_text(text)
        old_text = less_path.read_text(encoding="utf-8")
        if new_text != old_text:
            less_path.write_text(new_text, encoding="utf-8")
            changed.append(less_path.relative_to(root).as_posix())
# Former per-shot direct-child smoke flashes; kept only so the apply loop can strip the
# obsolete smoke_long child ref from already-patched trees (now spray-pair bases).
PVRH_PER_SHOT_SMOKE_FLASHES = (
    "weapon_muzzle_flash_awp.vpcf",
    "weapon_muzzle_flash_huntingrifle_fp.vpcf",
)

WEAPON_MUZZLE_SMOKE_FILES = tuple(
    f"particles/filmmaker/povarehok/{variant}/weapons/cs_weapon_fx/{name}"
    for variant in VARIANTS
    for name in (
        "weapon_muzzle_smoke_long.vpcf",
        "weapon_muzzle_smoke_long_b.vpcf",
        "weapon_muzzle_smoke.vpcf",
        "weapon_muzzle_smoke_b.vpcf",
        "weapon_muzzle_smoke_b_version_#2.vpcf",
    )
)

# Converted explosion roots -> stock CS2 distortion child restoring the native
# heat-shimmer the vanilla systems had.
POVAREHOK_DISTORT_CHILDREN = {
    "particles/filmmaker/povarehok/regular/explosions_fx/explosion_basic.vpcf": common.STOCK_HE_DISTORT,
    "particles/filmmaker/povarehok/regular/explosions_fx/explosion_hegrenade_interior.vpcf": common.STOCK_HE_DISTORT,
    "particles/filmmaker/povarehok/less/smoke/explosions_fx/explosion_basic.vpcf": common.STOCK_HE_DISTORT,
    "particles/filmmaker/povarehok/less/smoke/explosions_fx/explosion_hegrenade_interior.vpcf": common.STOCK_HE_DISTORT,
    "particles/filmmaker/povarehok/regular/explosions_fx/explosion_c4_500.vpcf": common.STOCK_C4_DISTORT,
    "particles/filmmaker/povarehok/less/smoke/explosions_fx/explosion_c4_500.vpcf": common.STOCK_C4_DISTORT,
}


# The FAS barrel wisp pair only (ac_muzzle_shotgun_alt_barrel_smoke_trail{,_b}).
# GOTCHA (found 2026-07-06, the "clumped/warped muzzle smoke" screenshot): an earlier
# "ac_muzzle*trail*" name match also swept up ac_muzzle_smg_trail{,5} and
# ac_muzzle_shotgun_trail -- short per-shot smoke TRACES, not barrel wisps -- and
# attached all five to weapon_muzzle_smoke_long, so every spray shot stacked five
# overlapping ribbons at the muzzle. Only the real barrel wisps belong here; the traces
# stay untouched as native children of their own flash systems.
def iter_muzzle_trail_wisp_vpcfs(root: Path):
    pvrh_root = root.joinpath("particles", "filmmaker", "povarehok")
    for path in pvrh_root.rglob("*.vpcf"):
        name = path.name.lower()
        if "ac_muzzle" in name and "barrel_smoke_trail" in name and "weapons" in path.parts:
            yield path


# Wrongly-attached smoke_long children to strip from already-patched trees (see above).
WRONG_SMOKE_LONG_CHILDREN = {
    "ac_muzzle_smg_trail.vpcf",
    "ac_muzzle_smg_trail5.vpcf",
    "ac_muzzle_shotgun_trail.vpcf",
}


# Stock CS2 weapon_muzzle_smoke_long's own brief lock (decompiled from pak01 2026-07-06):
# newborn smoke anchors to the muzzle for the first 0.1s, then rises freely. The plume's
# base keeps tracking the gun via CONTINUOUS EMISSION at the engine-driven muzzle CP,
# exactly like Valve's own asset -- do NOT full-lock the whole rising column to the gun
# (rigid ride) and do NOT inject CP configs (froze the engine's own driving).
_CS2_STOCK_BRIEF_POSITION_LOCK = (
    "\t\t{\n"
    '\t\t\t_class = "C_OP_PositionLock"\n'
    "\t\t\tm_flStartTime_min = 0.0\n"
    "\t\t\tm_flStartTime_max = 0.0\n"
    "\t\t\tm_flEndTime_min = 0.1\n"
    "\t\t\tm_flEndTime_max = 0.1\n"
    "\t\t},\n"
)


def patch_cs2_muzzle_smoke_alignment(text: str, *, sustained: bool = False) -> str:
    """CS2 muzzle alignment for the sustained Povarehok barrel plume.

    2026-07-06 night rework, calibrated against the DECOMPILED stock CS2
    weapon_muzzle_smoke_long (Valve's own barrel smoke, which the mod's version is a
    copy of): world-space, barrel-tip spawn, stock initial-velocity spread
    ([-10,-10,10]..[10,10,15] -- an earlier +/-1.5 "de-clump" over-tightened the rope
    into the dense dark column of the user's screenshot), stock brief 0.1s PositionLock
    (base tracking comes from emission at the CP-followed muzzle, not from locking the
    risen column), and the stock ADDITIVE rope blend on beam_smoke_01 (the earlier
    blanket ADD-strip in this function alpha-blended the dark-gray rope into a BLACK
    smoke column; stock renders those 46-83 grays additively = a soft translucent wisp).
    """
    new = re.sub(
        r'\{\s*_class = "C_OP_PositionLock"\s*(?:m_bLockRot = true\s*)?(?:m_flStartTime_min = [^\}]*)?\},?\s*',
        "",
        text,
        flags=re.MULTILINE | re.DOTALL,
    )
    if sustained and "C_OP_PositionLock" not in new:
        anchor = re.search(r"m_Operators = \n\t\[", new)
        if anchor:
            new = new[: anchor.end()] + "\n" + _CS2_STOCK_BRIEF_POSITION_LOCK + new[anchor.end():]

    new = new.replace("m_bLocalCoords = false", "m_bLocalCoords = true")
    # FINAL (2026-07-06 third round): WORLD-pass like the stock asset -- viewmodel-pass
    # smoke rides the camera rigidly and never lags in the air (the "doesn't move like
    # it would in real life" complaint). The engine drives the muzzle CP itself; no
    # config injection (that froze it -- see remove_muzzle_follow_config).
    new = new.replace("m_bViewModelEffect = true", "m_bViewModelEffect = false")
    new = common.remove_muzzle_follow_config(new)

    # Zero any converted/previously-patched emission kick; the plume must not be shot
    # forward out of the barrel (stock CreateWithinSphereTransform has no speed at all).
    new = re.sub(
        r"m_LocalCoordinateSystemSpeedMin = \[[^\]]*\]",
        "m_LocalCoordinateSystemSpeedMin = [0.0, 0.0, 0.0]",
        new,
    )
    new = re.sub(
        r"m_LocalCoordinateSystemSpeedMax = \[[^\]]*\]",
        "m_LocalCoordinateSystemSpeedMax = [0.0, 0.0, 0.0]",
        new,
    )
    # Stock spread: mostly-upward drift with enough lateral variance that the rope
    # doesn't collapse into a single dense column.
    new = re.sub(r"(?m)^(\s*)m_vecOutputMin = \[[^\]]*\]", r"\1m_vecOutputMin = [-10.0, -10.0, 10.0]", new)
    new = re.sub(r"(?m)^(\s*)m_vecOutputMax = \[[^\]]*\]", r"\1m_vecOutputMax = [10.0, 10.0, 15.0]", new)

    new = new.replace('_class = "C_OP_RenderTrails"', '_class = "C_OP_RenderSprites"\n\t\t\tm_bBlendFramesSeq0 = true')
    new = re.sub(r"\tm_bAdditive = 1\n", "", new)
    new = common.OVERBRIGHT_OUTPUT_LINE_RE.sub(r"\1m_flOverbrightFactor = 1.0", new)

    # Stock additive blend on the beam_smoke_01 rope (see docstring). Targeted by
    # texture so translucent sheet smoke in the same family never goes additive
    # (additive ignores the alpha mask -> square frames).
    edits = []
    for start, end in common.iter_renderer_blocks(new):
        block = new[start:end]
        if "beam_smoke_01" not in block:
            continue
        patched = block
        if "m_nOutputBlendMode" not in patched:
            patched = patched.replace(
                '_class = "C_OP_RenderRopes"',
                '_class = "C_OP_RenderRopes"\n\t\t\t' + common.ADDITIVE_BLEND_LINE,
                1,
            )
        # Unlit like every other additive renderer (fix_lit_renderers' rule); undoes
        # the 0.0 an earlier version of this function baked in.
        patched = patched.replace("m_flSelfIllumAmount = 0.0", "m_flSelfIllumAmount = 1.0")
        if patched != block:
            edits.append((start, end, patched))
    for start, end, replacement in reversed(edits):
        new = new[:start] + replacement + new[end:]

    new = new.replace("m_flMaxLength = 11.0", "m_flMaxLength = 24.0")
    new = new.replace("m_flMinLength = 30.0", "m_flMinLength = 12.0")

    new = common._ensure_cs2_muzzle_position_offset(new, common.MUZZLE_OFFSET_BARREL_TIP)

    # Close the puff-to-plume visibility gap: the plume must be visible while the
    # per-shot muzzle puff (0.2s life) still is, or one shot reads as two smokes.
    new = re.sub(r"m_flFadeInTimeMax = 0\.6", "m_flFadeInTimeMax = 0.15", new)
    new = re.sub(r"m_flFadeInTimeMin = 0\.5", "m_flFadeInTimeMin = 0.1", new)

    new = new.replace(
        '{\n\t\t\t_class = "C_OP_BasicMovement"\n\t\t},',
        '{\n\t\t\t_class = "C_OP_BasicMovement"\n\t\t\tm_fDrag = 0.05\n\t\t\tm_Gravity = [ 0.0, 0.0, 25.0 ]\n\t\t},',
    )
    # The RenderTrails->RenderSprites rewrite above inserts m_bBlendFramesSeq0
    # blindly; drop the insert again wherever the converted block already
    # carried the key, or resourcecompiler fails on the duplicate.
    new = common.dedup_blend_frames_key(new)
    return new


def apply_povarehok_gameplay_composites(root: Path) -> list[str]:
    """Returns the content-relative resources that were written/changed."""
    changed: list[str] = []

    for variant in PVRH_WEAPON_VARIANTS:
        weapon_dir = _pvrh_weapon_dir(variant)
        barrel_smoke = f"{weapon_dir}/weapon_muzzle_smoke_long.vpcf"
        if not common.resource_path(root, barrel_smoke).is_file():
            continue
        removed_smoke_children = set(WRONG_SMOKE_LONG_CHILDREN)
        for wisp_path in iter_muzzle_trail_wisp_vpcfs(root):
            try:
                wisp_path.relative_to(common.resource_path(root, weapon_dir))
            except ValueError:
                continue
            removed_smoke_children.add(wisp_path.name)
            removed_smoke_children.add(_fp_variant_res(wisp_path.name))
        if common.remove_child_refs(common.resource_path(root, barrel_smoke), removed_smoke_children):
            changed.append(barrel_smoke)

    for res, distort in POVAREHOK_DISTORT_CHILDREN.items():
        if common._add_child_once(common.resource_path(root, res), distort):
            changed.append(res)

    return changed


def main() -> int:
    args = parse_args()
    root = args.content_root.resolve()
    source1_root = (args.source1_root or root.parents[2] / "source1_game").resolve()

    if args.gameplay_composites_only:
        # In-place patch entry: also run the idempotent material repairs so an
        # already-converted tree picks them up without a full (non-rerunnable)
        # pipeline pass. Changed textures/particles need a resourcecompiler run.
        wrong_add = common.repair_wrongly_additive_renderers(root, source1_root)
        premul = common.premultiply_white_additive_textures(root)
        blend = common.add_sheet_frame_blending(root)
        # tune_tracer_brightness=False: the brightness pass is a one-shot tone pass
        # (multiplicative, like tone_down); re-running it in place dims tracers again.
        composites = apply_povarehok_gameplay_composites(root) + modern.apply_modern_gameplay_composites(
            root, tune_tracer_brightness=False)
        warm = fix_warm_impact_smoke_tints(root)
        loop_files, loop_renderers = fix_looping_smoke_animation(root)
        clamped = common.clamp_sheet_sequences(root, CLAMP_SMOKE_SHEET_RE)
        print(
            f"Repairs: {wrong_add} wrongly-additive files fixed, "
            f"{len(premul)} textures premultiplied, {blend} files frame-blended, "
            f"{warm} warm impact-smoke files neutralized, "
            f"{loop_files} looping-smoke files / {loop_renderers} renderers set to play once, "
            f"{len(clamped)} smoke sheets clamped (loop -> play once)."
        )
        for res in premul + clamped:
            print(f"  recompile texture: {res}")
        print(f"Gameplay composites: {len(composites)} file(s) written/changed:")
        for res in composites:
            print(f"  {res}")
        return 0

    if args.runtime_impact_fixes_only:
        collision = common.force_per_particle_collision(root)
        debris_files, debris_constraints, debris_spin = settle_impact_debris(root)
        smoke_files, smoke_renderers = fix_impact_smoke_blending(root)
        loop_files, loop_renderers = fix_looping_smoke_animation(root)
        clamped = common.clamp_sheet_sequences(root, CLAMP_SMOKE_SHEET_RE)
        print(
            f"Runtime impact fixes: {collision} files forced to per-particle collision, "
            f"{debris_files} debris files / {debris_constraints} constraints / "
            f"{debris_spin} spin operators settled, "
            f"{smoke_files} smoke files / {smoke_renderers} renderers neutralized, "
            f"{loop_files} looping-smoke files / {loop_renderers} renderers set to play once, "
            f"{len(clamped)} smoke sheets clamped (loop -> play once)."
        )
        for res in clamped:
            print(f"  recompile texture: {res}")
        return 0

    changed_muzzle = 0
    changed_molotov = 0

    for variant in VARIANTS:
        muzzle_roots = [
            variant_resource(variant, "weapons/cs_weapon_fx", name)
            for name in MUZZLE_ROOTS
        ]
        changed_muzzle += tone_down_muzzle_closure(
            root, muzzle_roots, alpha=0.45, radius=0.72, overbright=0.35
        )

        molotov_roots = [
            variant_resource(variant, "inferno_fx", name)
            for name in MOLOTOV_GROUNDFIRE_ROOTS + MOLOTOV_EXPLOSION_ROOTS
        ]
        for resource in molotov_roots:
            path = common.resource_path(root, resource)
            if path.is_file() and common.remove_child_refs(path, MOLOTOV_REMOVE_CHILDREN):
                changed_molotov += 1
        for path in common.collect_closure(root, molotov_roots):
            # Molotov fire still comes from the real mod, but Source 2's lighting
            # makes the Source 1 overbright values dominate the frame.
            if tone_down(path, alpha=0.75, radius=0.82, overbright=0.55):
                changed_molotov += 1

    dead_children = common.strip_dead_child_refs(root)
    repaired, removed = common.fix_textureless_renderers(root, source1_root)
    dual_seq = common.strip_dual_sequence_keys(root)
    collision = common.force_per_particle_collision(root)
    unlit, additive = common.fix_lit_renderers(root, source1_root)
    wrong_add = common.repair_wrongly_additive_renderers(root, source1_root)
    # Must run AFTER fix_lit_renderers so the final ADD/translucent state is known.
    premul = common.premultiply_white_additive_textures(root)
    blend = common.add_sheet_frame_blending(root)
    debris_files, debris_constraints, debris_spin = settle_impact_debris(root)
    less_debris_files, less_debris_emitters = halve_less_impact_debris(root)
    smoke_files, smoke_renderers = fix_impact_smoke_blending(root)
    vistasmoke_files, vistasmoke_renderers = patch_non_fire_vistasmoke_replacements(root)
    warm_smoke = fix_warm_impact_smoke_tints(root)
    # Must run AFTER patch_non_fire_vistasmoke_replacements (which already sets FIT_LIFETIME on
    # the systems it retargets) so this only touches the remaining looping smoke sheets.
    loop_files, loop_renderers = fix_looping_smoke_animation(root)
    # One-shot smoke sheets: loop -> clamp so frame overshoot holds instead of replaying
    # (the sheets compile after this in the full pipeline, so no separate recompile step).
    clamped = common.clamp_sheet_sequences(root, CLAMP_SMOKE_SHEET_RE)
    # Must run AFTER fix_textureless_renderers: it fully rewrites the heatwave
    # systems whose (Source 1 refract) renderers that pass just emptied.
    composites = apply_povarehok_gameplay_composites(root) + modern.apply_modern_gameplay_composites(root)
    print(
        f"Post-processed Povarehok: {changed_muzzle} muzzle files, "
        f"{changed_molotov} molotov files, {dead_children} dead child refs stripped, "
        f"{repaired} textureless renderers repaired / {removed} removed, "
        f"{dual_seq} files de-dual-sequenced, "
        f"{collision} files forced to per-particle collision, "
        f"{unlit} renderers made unlit / {additive} made additive "
        f"({wrong_add} wrongly-additive fixed), "
        f"{len(premul)} additive textures premultiplied, {blend} files frame-blended, "
        f"{debris_files} impact-debris files settled ({debris_constraints} constraints, "
        f"{debris_spin} spin operators), "
        f"{less_debris_files} Less debris files halved ({less_debris_emitters} emitters), "
        f"{smoke_files} impact-smoke files neutralized ({smoke_renderers} renderers), "
        f"{vistasmoke_files} non-fire vistasmoke files retargeted ({vistasmoke_renderers} renderers), "
        f"{warm_smoke} warm impact-smoke tints fixed, "
        f"{loop_files} looping-smoke files set to play once ({loop_renderers} renderers), "
        f"{len(clamped)} smoke sheets clamped, "
        f"{len(composites)} gameplay-composite files."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
