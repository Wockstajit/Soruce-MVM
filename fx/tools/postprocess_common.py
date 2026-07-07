"""Shared VPCF text/file primitives used by both the Povarehok and Modern
post-process passes.

Generic only: no pack-specific file lists, weapon names, or tuned offset
literals live here. Those belong in postprocess_povarehok.py / postprocess_modern.py.
"""

from __future__ import annotations

import re
from collections import deque
from pathlib import Path


RESOURCE_RE = re.compile(r'resource:"([^"]+)"')
NUMBER_RE = re.compile(r"(-?\d+(?:\.\d+)?)")

# Converted-pack namespaces the generic CS2 repairs apply to. povarehok = the
# CS:GO Povarehok variants; modern = the MW2019 (ARC9/GMod) pack.
NAMESPACE_ROOTS = ("povarehok", "modern")

STOCK_HE_DISTORT = "particles/explosions_fx/explosion_hegrenade_distort.vpcf"
STOCK_C4_DISTORT = "particles/explosions_fx/explosion_c4_distort01d_1k.vpcf"
STOCK_WARP_NORMAL = "materials/particle/warp_ripple3_normal.vtex"


def iter_namespace_vpcfs(root: Path):
    for namespace in NAMESPACE_ROOTS:
        yield from root.joinpath("particles", "filmmaker", namespace).rglob("*.vpcf")


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


CHILD_REF_PATTERN = re.compile(
    r"\n\t\t\{\n\t\t\tm_ChildRef = resource:\"(?P<resource>[^\"]+)\""
    r"(?:\n\t\t\tm_flDelay = [^\n]+)?"
    r"(?:\n\t\t\tm_bEndCap = [^\n]+)?"
    r"\n\t\t\},",
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
    refs into the povarehok/modern namespaces are considered -- anything else
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
    for path in iter_namespace_vpcfs(root):
        text = path.read_text(encoding="utf-8")

        def missing(resource: str) -> bool:
            resource = resource.replace("\\", "/")
            return (
                resource.startswith("particles/filmmaker/")
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


def block_span(text: str, class_pos: int) -> tuple[int, int]:
    """Span of the { ... } operator block containing the _class match."""
    start = text.rfind("{", 0, class_pos)
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return start, i + 1
    return start, len(text)


VMT_BASETEXTURE_RE = re.compile(r'"?\$basetexture"?\s+"([^"\n]+)"', re.IGNORECASE)

VMT_COMMENT_RE = re.compile(r"//[^\n]*")

VMT_ADDITIVE_RE = re.compile(r'"?\$additive"?\s+"?1', re.IGNORECASE)


def stripped_vmt_text(path: Path) -> str:
    """VMT text with // comments removed.

    LESSON (2026-07-03, the molotov/C4 gray squares): several mod VMTs carry
    `// "$additive" "1"` -- additive tried and DISABLED by the author. Matching
    $additive against raw text turned those translucent smoke sprites additive,
    and additive ignores the alpha mask, so every soft cloud rendered its full
    square sheet-frame. Always match VMT parameters against comment-stripped
    text.
    """
    return VMT_COMMENT_RE.sub("", path.read_text(encoding="utf-8", errors="ignore"))


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
    for path in iter_namespace_vpcfs(root):
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
    for path in iter_namespace_vpcfs(root):
        text = path.read_text(encoding="utf-8")
        new_text = DUAL_SEQUENCE_LINE_RE.sub("", text)
        if new_text != text:
            path.write_text(new_text, encoding="utf-8")
            changed += 1
    return changed


# Source 1's SpriteCard shader is UNLIT: particles never received scene
# lighting (any darkening was baked into per-particle color at spawn). The
# converted renderers omit m_flSelfIllumAmount, so CS2 defaults to scene-lit
# sprites -- in shaded areas the whole sprite (including its wide alpha halo)
# is multiplied toward black, which dims flashes and draws a dark box around
# glow quads. Native CS2 muzzle flashes set m_flSelfIllumAmount = 1.0 and an
# explicit additive blend; mirror that, using the original VMTs to decide
# which materials were additive ($additive 1) vs plain translucent.
RENDERER_CLASS_RE = re.compile(
    r'_class = "(?:C_OP_RenderSprites|C_OP_RenderTrails|C_OP_RenderRopes)"'
)
SELF_ILLUM_LINE = "m_flSelfIllumAmount = 1.0"
ADDITIVE_BLEND_LINE = 'm_nOutputBlendMode = "PARTICLE_OUTPUT_BLEND_MODE_ADD"'


def fix_lit_renderers(root: Path, source1_root: Path) -> tuple[int, int]:
    unlit = additive = 0
    for path in iter_namespace_vpcfs(root):
        text = path.read_text(encoding="utf-8")
        edits = []  # (insert_pos, insert_text)
        for match in RENDERER_CLASS_RE.finditer(text):
            start, end = block_span(text, match.start())
            block = text[start:end]
            indent = text[: match.start()].rsplit("\n", 1)[-1]
            lines = []
            if "m_flSelfIllumAmount" not in block:
                lines.append(SELF_ILLUM_LINE)
                unlit += 1
            if "m_nOutputBlendMode" not in block:
                mat = re.search(r'm_hMaterial = resource:"([^"]+)"', block)
                if mat:
                    vmt = source1_root / Path(mat.group(1).replace("\\", "/")).with_suffix(".vmt")
                    if vmt.is_file() and VMT_ADDITIVE_RE.search(stripped_vmt_text(vmt)):
                        lines.append(ADDITIVE_BLEND_LINE)
                        additive += 1
            if lines:
                insert = "".join(f"\n{indent}{line}" for line in lines)
                edits.append((match.end(), insert))
        if edits:
            for pos, insert in reversed(edits):
                text = text[:pos] + insert + text[pos:]
            path.write_text(text, encoding="utf-8")
    return unlit, additive


ADD_BLEND_LINE_RE = re.compile(
    r'(?m)^\s*m_nOutputBlendMode = "PARTICLE_OUTPUT_BLEND_MODE_ADD"\n?'
)


def repair_wrongly_additive_renderers(root: Path, source1_root: Path) -> int:
    """Remove ADD blend from renderers whose (comment-stripped) VMT is not additive.

    Undoes the damage a comment-blind $additive match already did to a converted
    tree (see stripped_vmt_text): translucent smoke rendered additive shows its
    whole square sheet frame. Idempotent -- the ADD line is gone after one run.
    """
    changed = 0
    for path in iter_namespace_vpcfs(root):
        text = path.read_text(encoding="utf-8")
        edits = []
        for start, end in iter_renderer_blocks(text):
            block = text[start:end]
            if "PARTICLE_OUTPUT_BLEND_MODE_ADD" not in block:
                continue
            mat = re.search(r'm_hMaterial = resource:"([^"]+)"', block)
            if not mat:
                continue
            vmt = source1_root / Path(mat.group(1).replace("\\", "/")).with_suffix(".vmt")
            if not vmt.is_file() or VMT_ADDITIVE_RE.search(stripped_vmt_text(vmt)):
                continue
            new_block = ADD_BLEND_LINE_RE.sub("", block)
            if new_block != block:
                edits.append((start, end, new_block))
        if edits:
            for start, end, replacement in reversed(edits):
                text = text[:start] + replacement + text[end:]
            path.write_text(text, encoding="utf-8")
            changed += 1
    return changed


def _vtex_frame_tgas(root: Path, vtex_res: str) -> list[Path]:
    """The TGA(s) behind a .vtex: the plain image, or every frame of its .mks sheet."""
    vtex = resource_path(root, vtex_res)
    if not vtex.is_file():
        return []
    m = re.search(r'"([^"]+\.(?:tga|mks))"', vtex.read_text(encoding="utf-8", errors="ignore"))
    if not m:
        return []
    ref = re.sub(r"^\[[^\]]*\]", "", m.group(1)).strip("/\\")
    src = root / Path(ref.replace("\\", "/"))
    if not src.is_file():
        return []
    if src.suffix.lower() == ".mks":
        frames = []
        for fm in re.finditer(r"(\S+\.tga)", src.read_text(encoding="utf-8", errors="ignore")):
            frame = src.parent / fm.group(1)
            if frame.is_file():
                frames.append(frame)
        return frames
    return [src]


def premultiply_white_additive_textures(root: Path) -> list[str]:
    """Bake the alpha mask into solid-white-RGB textures used ONLY additively.

    Source 1 exports store some sprites as white RGB + the shape in alpha (lens
    flares, modulate sprites). CS2's PARTICLE_OUTPUT_BLEND_MODE_ADD ignores the
    alpha mask, so such a texture renders as a solid white square (2026-07-03:
    particle_anamorphic_lens on explosion_child_flash03b_1k = THE big white
    square on molotov + C4). Multiplying RGB by alpha reconstructs the shape
    under pure additive blending. Textures also used by non-additive renderers
    are skipped (premultiplying would dim their alpha-blended look). A `.premul`
    marker beside each TGA (mtime-compared) keeps the pass idempotent across
    in-place runs while re-triggering after a fresh conversion rewrites the TGA.
    Returns the changed .vtex resources (their compiled vtex_c must be rebuilt).
    """
    from PIL import Image  # the conversion pipeline already requires Pillow

    add_tex: set[str] = set()
    other_tex: set[str] = set()
    for path in iter_namespace_vpcfs(root):
        text = path.read_text(encoding="utf-8")
        for start, end in iter_renderer_blocks(text):
            block = text[start:end]
            m = re.search(r'm_hTexture = resource:"([^"]+)"', block)
            if not m:
                continue
            res = m.group(1).replace("\\", "/")
            (add_tex if "PARTICLE_OUTPUT_BLEND_MODE_ADD" in block else other_tex).add(res)

    changed: list[str] = []
    for res in sorted(add_tex - other_tex):
        frames = _vtex_frame_tgas(root, res)
        if not frames:
            continue
        touched = False
        for tga in frames:
            marker = tga.with_suffix(tga.suffix + ".premul")
            if marker.is_file() and marker.stat().st_mtime >= tga.stat().st_mtime:
                continue
            im = Image.open(tga).convert("RGBA")
            px = im.load()
            w, h = im.size
            # sample: near-solid-white RGB with a real alpha mask?
            rgb_min = 255
            alpha_max = 0
            for y in range(0, h, max(1, h // 16)):
                for x in range(0, w, max(1, w // 16)):
                    r, g, b, a = px[x, y]
                    rgb_min = min(rgb_min, r, g, b)
                    alpha_max = max(alpha_max, a)
            if rgb_min < 250 or alpha_max == 0:
                continue
            for y in range(h):
                for x in range(w):
                    r, g, b, a = px[x, y]
                    px[x, y] = (r * a // 255, g * a // 255, b * a // 255, a)
            im.save(tga)
            marker.write_text("premultiplied by postprocess_common.py\n", encoding="ascii")
            touched = True
        if touched:
            changed.append(res)
    return changed


BLEND_FRAMES_LINE = "m_bBlendFramesSeq0 = true"
BLEND_FRAMES_KEY_RE = re.compile(r"m_bBlendFramesSeq0 = \S+")
BLEND_FRAMES_TRUE_LINE_RE = re.compile(r"(?m)^\s*m_bBlendFramesSeq0 = true\n?")


def dedup_blend_frames_key(text: str) -> str:
    """Drop surplus `m_bBlendFramesSeq0 = true` lines from renderer blocks.

    Rope/trail -> RenderSprites rewrites (in the pack-specific alignment
    patchers) prepend `m_bBlendFramesSeq0 = true` blindly; when the converted
    block ALREADY carries the key (`= 1` from source1import), that produces a
    "duplicate member" resourcecompiler error. This cannot be left to
    add_sheet_frame_blending's self-repair because in the full pipeline that
    pass runs BEFORE the gameplay-composites pass -- so every rewrite that
    inserts the key must dedup its own output. Keeps the converter's original
    spelling, drops the inserted `true` line(s).
    """
    ops = []
    for match in re.finditer(r'_class = "C_OP_RenderSprites"', text):
        start, end = block_span(text, match.start())
        block = text[start:end]
        occurrences = BLEND_FRAMES_KEY_RE.findall(block)
        if len(occurrences) > 1:
            new_block = BLEND_FRAMES_TRUE_LINE_RE.sub("", block, count=len(occurrences) - 1)
            if new_block != block:
                ops.append((start, end, new_block))
    for start, end, replacement in sorted(ops, reverse=True):
        text = text[:start] + replacement + text[end:]
    return text


def add_sheet_frame_blending(root: Path) -> int:
    """Enable cross-frame blending on sprite renderers backed by .mks sheets.

    Source 1 SpriteCard blends sheet frames by default ($blendframes defaults
    on); many converted renderers lost that, so multi-frame smoke steps through
    discrete frames -- the "jittery/pixelated" smoke report (2026-07-03).
    GOTCHA: some conversions DO carry the key already, as `m_bBlendFramesSeq0 =
    1` (or an intentional `= 0`) -- match the KEY, not one spelling, or the
    insert produces a "duplicate member" compile error. Blocks that ended up
    with both spellings (the 2026-07-03 bad run) are self-repaired by dropping
    the inserted `true` line and keeping the converter's numeric one.
    """
    sheet_cache: dict[str, bool] = {}

    def is_sheet(res: str) -> bool:
        if res not in sheet_cache:
            vtex = resource_path(root, res)
            backed = False
            if vtex.is_file():
                backed = ".mks" in vtex.read_text(encoding="utf-8", errors="ignore")
            sheet_cache[res] = backed
        return sheet_cache[res]

    changed = 0
    for path in iter_namespace_vpcfs(root):
        text = path.read_text(encoding="utf-8")
        ops = []  # (start, end, replacement); inserts use start == end
        for match in re.finditer(r'_class = "C_OP_RenderSprites"', text):
            start, end = block_span(text, match.start())
            block = text[start:end]
            occurrences = BLEND_FRAMES_KEY_RE.findall(block)
            if len(occurrences) > 1:
                new_block = BLEND_FRAMES_TRUE_LINE_RE.sub("", block, count=len(occurrences) - 1)
                if new_block != block:
                    ops.append((start, end, new_block))
                continue
            if occurrences:
                continue
            tex = re.search(r'm_hTexture = resource:"([^"]+)"', block)
            if not tex or not is_sheet(tex.group(1).replace("\\", "/")):
                continue
            indent = text[: match.start()].rsplit("\n", 1)[-1]
            ops.append((match.end(), match.end(), f"\n{indent}{BLEND_FRAMES_LINE}"))
        if ops:
            # back to front so earlier offsets stay valid; block ranges never overlap
            for start, end, replacement in sorted(ops, reverse=True):
                text = text[:start] + replacement + text[end:]
            path.write_text(text, encoding="utf-8")
            changed += 1
    return changed


# Converted C_OP_WorldTraceConstraint blocks come through with no collision
# mode (Source 2 default) or COLLISION_MODE_USE_NEAREST_TRACE -- both are
# plane-cache modes: one trace near the effect origin becomes an infinite
# collision plane. On a ledge/stair edge that plane extends into open air, so
# debris drifting past the edge lands on the invisible plane and floats.
# Per-particle tracing raycasts each particle against the real world, letting
# chunks fall off edges correctly. Slower, but this is an offline movie tool.
WORLD_TRACE_CLASS_RE = re.compile(r'_class = "C_OP_WorldTraceConstraint"')
COLLISION_MODE_LINE_RE = re.compile(r'm_nCollisionMode = "[^"]*"')
PER_PARTICLE_TRACE = 'm_nCollisionMode = "COLLISION_MODE_PER_PARTICLE_TRACE"'


def force_per_particle_collision(root: Path) -> int:
    changed = 0
    for path in iter_namespace_vpcfs(root):
        text = path.read_text(encoding="utf-8")
        edits = []  # (start, end, replacement)
        for match in WORLD_TRACE_CLASS_RE.finditer(text):
            start, end = block_span(text, match.start())
            block = text[start:end]
            if PER_PARTICLE_TRACE in block:
                continue
            if COLLISION_MODE_LINE_RE.search(block):
                block = COLLISION_MODE_LINE_RE.sub(PER_PARTICLE_TRACE, block)
            else:
                class_end = start + block.find('"C_OP_WorldTraceConstraint"') + len('"C_OP_WorldTraceConstraint"')
                indent = text[: match.start()].rsplit("\n", 1)[-1]
                rel = class_end - start
                block = block[:rel] + f"\n{indent}{PER_PARTICLE_TRACE}" + block[rel:]
            edits.append((start, end, block))
        if edits:
            for start, end, replacement in reversed(edits):
                text = text[:start] + replacement + text[end:]
            path.write_text(text, encoding="utf-8")
            changed += 1
    return changed


ADD_BLEND_OUTPUT_LINE_RE = re.compile(
    r'(?m)^\s*m_nOutputBlendMode = "PARTICLE_OUTPUT_BLEND_MODE_ADD"\n?'
)
OVERBRIGHT_OUTPUT_LINE_RE = re.compile(r"(?m)^(\s*)m_flOverbrightFactor = [^\n]+")
ADD_SELF_OUTPUT_LINE_RE = re.compile(r"(?m)^(\s*)m_flAddSelfAmount = [^\n]+")


VPCF_HEADER = (
    "<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} "
    "format:vpcf26:version{26288658-411e-4f14-b698-2e1e5d00dec6} -->"
)

# Header for authored behavior-12 systems (mirrors a Source2Viewer decompile of
# stock CS2 particles, which resourcecompiler accepts as input).
VPCF12_HEADER = (
    "<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} "
    "format:vpcf58:version{9bada39c-a931-42d0-abdd-e5c1b13d37a6} -->"
)

CHILD_ELEMENT = '\t\t{{\n\t\t\tm_ChildRef = resource:"{res}"\n\t\t\tm_flDelay = {delay}\n\t\t}},\n'

# GMod ARC9's AfterShotParticle spawns ~one RPM interval AFTER the last shot
# (sh_think.lua: NextPrimaryFire + 60/RPM + AfterShotParticleDelay). The wrappers
# emulate that with a delayed smoke child: short bursts (1-3 rounds) finish before
# the smoke blooms, so the wisp appears just after firing stops, like GMod.
AFTERSHOT_SMOKE_DELAY = 0.45


def _composition_text(children: tuple) -> str:
    """children: resource strings, or (resource, delaySeconds) tuples."""
    elements = "".join(
        CHILD_ELEMENT.format(res=c[0], delay=f"{float(c[1]):.2f}")
        if isinstance(c, tuple) else CHILD_ELEMENT.format(res=c, delay="0.0")
        for c in children
    )
    return (
        f"{VPCF_HEADER}\n"
        "{\n"
        "\tm_nBehaviorVersion = 8\n"
        '\t_class = "CParticleSystemDefinition"\n'
        "\tm_Children = \n"
        "\t[\n"
        f"{elements}"
        "\t]\n"
        "\tm_bPreventnamebasedlookup = false\n"
        "\tm_nMaxParticles = 1000\n"
        "}\n"
    )


def _add_child_once(path: Path, resource: str) -> bool:
    """Insert resource into the file's m_Children array (created if absent)."""
    if not path.is_file():
        return False
    text = path.read_text(encoding="utf-8")
    if resource in text:
        return False
    element = CHILD_ELEMENT.format(res=resource, delay="0.0")
    match = re.search(r"m_Children = \n(\t*)\[\n", text)
    if match:
        text = text[: match.end()] + element + text[match.end():]
    else:
        anchor = re.search(r'_class = "CParticleSystemDefinition"\n', text)
        if not anchor:
            return False
        block = "\tm_Children = \n\t[\n" + element + "\t]\n\n"
        text = text[: anchor.end()] + block + text[anchor.end():]
    path.write_text(text, encoding="utf-8")
    return True


def write_if_different(root: Path, res: str, text: str, changed: list[str]) -> None:
    path = resource_path(root, res)
    if not path.is_file() or path.read_text(encoding="utf-8") != text:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        changed.append(res)


def _spray_wrapper_res(flash_res: str) -> str:
    folder, name = flash_res.rsplit("/", 1)
    return f"{folder}/mvm_spray_{name}"


def _ensure_viewmodel_effect_flag(text: str) -> str:
    if "m_bViewModelEffect = true" in text:
        return text
    if "m_bViewModelEffect = false" in text:
        return text.replace("m_bViewModelEffect = false", "m_bViewModelEffect = true", 1)
    return text.replace(
        '_class = "CParticleSystemDefinition"',
        '_class = "CParticleSystemDefinition"\n\tm_bViewModelEffect = true',
        1,
    )


# CS2 muzzle spawn offsets. The default (Povarehok forward/up kick) was copied from the
# working Povarehok per-shot child weapon_muzzle_flash_smoke_small2 (local +X forward);
# the barrel-tip values are what Modern's live barrel smoke uses (postprocess_modern's
# _CS2_MODERN_ROPE_TRAIL_OFFSET_BLOCK) -- the sustained/wisp barrel smoke of BOTH packs
# now spawns there so the smoke visibly starts AT the muzzle (user report 2026-07-06:
# "Povarehok barrel smoke doesn't sit at the barrel like Modern"). This is the SHARED
# base recipe both packs' alignment passes build on, so it lives here rather than in
# either pack-specific file. Keep the barrel-tip values in sync with FxAlign.cpp's
# kModernCfgOffset and postprocess_modern._CS2_MODERN_ROPE_TRAIL_OFFSET_BLOCK.
MUZZLE_OFFSET_FORWARD = ("[1.0, 0.0, 2.0]", "[6.0, 0.0, 4.0]")
MUZZLE_OFFSET_BARREL_TIP = ("[0.0, 0.0, -0.5]", "[0.5, 0.0, 0.0]")


def _muzzle_offset_block(offsets: tuple[str, str]) -> str:
    return (
        "\t\t{\n"
        '\t\t\t_class = "C_INIT_PositionOffset"\n'
        "\t\t\tm_bLocalCoords = true\n"
        f"\t\t\tm_OffsetMin = {offsets[0]}\n"
        f"\t\t\tm_OffsetMax = {offsets[1]}\n"
        "\t\t},\n"
    )


def _force_cs2_muzzle_offset_block(block: str, offsets: tuple[str, str] = MUZZLE_OFFSET_FORWARD) -> str:
    """Rewrite one C_INIT_PositionOffset block to the given CS2-calibrated values,
    UNCONDITIONALLY (not just when the source pack's original numbers happen to be
    byte-identical to Povarehok's own pre-patch literals).

    Bug (2026-07-03, "Modern's muzzle attach doesn't follow Povarehok's"): the previous
    version only special-cased two exact literal strings (Povarehok's OWN un-patched
    m_OffsetMin/Max). Any file whose original offset differs -- e.g. every MW2019/ARC9
    file, authored for GMod's own attach convention -- silently kept its own numbers,
    so "the same fix Povarehok uses" never actually reached Modern's assets.
    """
    block = re.sub(r"m_bLocalCoords = (?:true|false)", "m_bLocalCoords = true", block)
    if "m_bLocalCoords" not in block:
        block = block.replace(
            '_class = "C_INIT_PositionOffset"',
            '_class = "C_INIT_PositionOffset"\n\t\t\tm_bLocalCoords = true',
            1,
        )
    block = re.sub(r"m_OffsetMin = \[[^\]]*\]", f"m_OffsetMin = {offsets[0]}", block)
    block = re.sub(r"m_OffsetMax = \[[^\]]*\]", f"m_OffsetMax = {offsets[1]}", block)
    return block


def _ensure_cs2_muzzle_position_offset(text: str, offsets: tuple[str, str] = MUZZLE_OFFSET_FORWARD) -> str:
    if "C_INIT_PositionOffset" in text:
        edits = []
        for match in re.finditer(r'_class = "C_INIT_PositionOffset"', text):
            start, end = block_span(text, match.start())
            edits.append((start, end, _force_cs2_muzzle_offset_block(text[start:end], offsets)))
        for start, end, replacement in reversed(edits):
            text = text[:start] + replacement + text[end:]
        return text
    anchor = re.search(r'_class = "C_INIT_CreateWithinSphere"', text)
    if not anchor:
        return text
    insert_at = text.find("\n\t\t},", anchor.start())
    if insert_at < 0:
        return text
    return text[: insert_at + len("\n\t\t},")] + "\n" + _muzzle_offset_block(offsets) + text[insert_at + len("\n\t\t},") :]


# C_OP_PositionLock handling. LESSONS (2026-07-06, three rounds):
# - Rewriting lock times to huge values (start=end=1e6, then 0/1e6) made smoke read as
#   FROZEN rigid in game ("gun goes up, smoke stays down").
# - The bare `m_bLockRot = true` lock (the GMod original) rides the gun rigidly too --
#   the user's final call: smoke must LAG like real smoke ("you'll see it draw like a
#   sheet with the motion of the gun"), i.e. NOT be locked after birth.
# - Stock CS2's own weapon_muzzle_smoke_long is the reference behavior: a brief
#   0 -> 0.1s lock (newborn puff anchored to the muzzle for one beat, then free in the
#   world) while CONTINUOUS EMISSION at the engine-driven muzzle CP keeps the smoke
#   column's base on the gun. That combination is what draws the lagging sheet.
_BRIEF_POSITION_LOCK_BODY = (
    '\t\t\t_class = "C_OP_PositionLock"\n'
    "\t\t\tm_flStartTime_min = 0.0\n"
    "\t\t\tm_flStartTime_max = 0.0\n"
    "\t\t\tm_flEndTime_min = 0.1\n"
    "\t\t\tm_flEndTime_max = 0.1\n"
)


def ensure_brief_position_lock(text: str) -> str:
    """Rewrite every C_OP_PositionLock to the stock brief 0->0.1s lock (insert if none)."""
    if "C_OP_PositionLock" in text:
        edits = []
        for match in re.finditer(r'_class = "C_OP_PositionLock"', text):
            start, end = block_span(text, match.start())
            edits.append((start, end, "{\n" + _BRIEF_POSITION_LOCK_BODY + "\t\t}"))
        for start, end, replacement in reversed(edits):
            text = text[:start] + replacement + text[end:]
        return text
    match = re.search(r"m_Operators = \n(\t*)\[\n", text)
    if match:
        block = "\t\t{\n" + _BRIEF_POSITION_LOCK_BODY + "\t\t},\n"
        return text[: match.end()] + block + text[match.end():]
    return text


# FULL-lifetime lock -- the deliberate OPPOSITE of the brief world-pass lock above, ONLY
# for the FIRST-PERSON viewmodel (_fp) barrel-smoke twins. The world/third-person twins
# use the brief lock so old smoke lags in the air (real-smoke look). But in first person
# the viewmodel swings around fast during reload/inspect; brief-locked smoke goes free in
# world space after 0.1s and floats where the barrel WAS (user report 2026-07-06 pm:
# "reload/inspect -- barrel smoke doesn't follow the gun, floats where the barrel was").
# The _fp twins render in the viewmodel pass, so locking them to the muzzle CP for their
# whole life makes them ride the barrel through the animation. The lock ADDS the muzzle
# CP's translation on top of the particle's own buoyant rise/noise (it does NOT freeze
# them), so the wisp still lifts -- it just lifts relative to the moving barrel. The
# "rigid/frozen smoke" reverts documented above were the WORLD twin; a viewmodel twin
# that tracks the gun is exactly the wanted first-person behavior.
_FULL_POSITION_LOCK_BODY = (
    '\t\t\t_class = "C_OP_PositionLock"\n'
    "\t\t\tm_flStartTime_min = 0.0\n"
    "\t\t\tm_flStartTime_max = 0.0\n"
    "\t\t\tm_flEndTime_min = 1000000.0\n"
    "\t\t\tm_flEndTime_max = 1000000.0\n"
)


def ensure_full_position_lock(text: str) -> str:
    """Rewrite every C_OP_PositionLock to a full-lifetime 0->1e6 lock (insert if none).

    Use ONLY on first-person viewmodel (_fp) barrel-smoke twins so the wisp tracks the
    barrel during reload/inspect (see _FULL_POSITION_LOCK_BODY). World twins keep
    ensure_brief_position_lock. Idempotent."""
    if "C_OP_PositionLock" in text:
        edits = []
        for match in re.finditer(r'_class = "C_OP_PositionLock"', text):
            start, end = block_span(text, match.start())
            edits.append((start, end, "{\n" + _FULL_POSITION_LOCK_BODY + "\t\t}"))
        for start, end, replacement in reversed(edits):
            text = text[:start] + replacement + text[end:]
        return text
    match = re.search(r"m_Operators = \n(\t*)\[\n", text)
    if match:
        block = "\t\t{\n" + _FULL_POSITION_LOCK_BODY + "\t\t},\n"
        return text[: match.end()] + block + text[match.end():]
    return text


# REVERTED (2026-07-06 night): a m_controlPointConfigurations "game" block with
# PATTACH_POINT_FOLLOW was briefly injected into every muzzle swap target (copied from
# the stock uweapon_muzflsh_ak47_fps). In game it made things WORSE -- the engine's own
# dispatch was already driving the control points correctly (the old pack's smoke
# followed a thrown weapon), and the injected config froze/overrode that binding.
# remove_muzzle_follow_config strips the block from already-patched trees.
def remove_muzzle_follow_config(text: str) -> str:
    match = re.search(r"(?m)^\tm_controlPointConfigurations = \n\t\[\n", text)
    if not match:
        return text
    depth = 0
    i = match.end() - 2  # at the '['
    while i < len(text):
        c = text[i]
        if c == "[":
            depth += 1
        elif c == "]":
            depth -= 1
            if depth == 0:
                end = i + 1
                if end < len(text) and text[end] == "\n":
                    end += 1
                return text[: match.start()] + text[end:]
        i += 1
    return text


def clamp_sheet_sequences(root: Path, name_re: re.Pattern[str]) -> list[str]:
    """Drop the LOOP flag from matching one-shot .mks sprite sheets (loop -> clamp).

    The engine wraps a non-clamped sequence when the computed frame index passes the
    last frame (frame %= count), so any renderer whose age*rate overshoots the sheet
    visibly REPLAYS it -- the "smoke plays, fades, then plays again" report (2026-07-06;
    setting ANIMATION_TYPE_FIT_LIFETIME was not sufficient because the animation rate
    still multiplies under FIT and several converted smoke renderers overshoot).
    Clamping holds the last frame instead, and the systems' own FadeOut/Decay handles
    the tail. Scoped by sheet NAME to one-shot smoke/dust/splash sheets -- fire flicker
    and other intentional loops keep their flag. Idempotent (the LOOP line is gone
    after one run). Returns the changed .vtex resources; their compiled .vtex_c must
    be rebuilt with resourcecompiler.
    """
    changed: list[str] = []
    materials = root / "materials"
    if not materials.is_dir():
        return changed
    for mks in materials.rglob("*.mks"):
        rel = mks.relative_to(root).as_posix()
        if not name_re.search(rel):
            continue
        text = mks.read_text(encoding="ascii")
        new_text = re.sub(r"(?m)^LOOP\n", "", text)
        if new_text == text:
            continue
        mks.write_text(new_text, encoding="ascii")
        vtex = mks.with_suffix(".vtex")
        if vtex.is_file():
            changed.append(vtex.relative_to(root).as_posix())
    return changed


def patch_cs2_muzzle_rope_trail_alignment(text: str) -> str:
    """CS2 alignment for Povarehok's barrel ROPE wisps (GMod-style trail on the gun).

    User report 2026-07-03: converting RenderRopes -> RenderSprites removed the follow-the-
    barrel ribbon entirely; smoke read as a floating world puff in FP. Keep rope/trail
    renderers and restore ropes on already-sprited trees.

    FINAL recipe (2026-07-06, third round -- user: "when I move the gun the smoke should
    lag behind and draw like a sheet in the air, like real life"): mirror stock CS2's own
    weapon_muzzle_smoke_long. WORLD-pass rendering (a viewmodel-pass wisp rides the
    camera rigidly -- exactly the "doesn't move like in air" complaint) + the stock
    brief 0->0.1s PositionLock (newborn anchored one beat, then free). The engine keeps
    driving the instance's muzzle CP (needs NO config injection -- proven by the old
    pack's smoke following a thrown gun), so continuous emission paints the lagging
    sheet while old smoke hangs in the world. Barrel-TIP spawn offset kept.
    """
    new = text.replace("m_bLocalCoords = false", "m_bLocalCoords = true")
    new = new.replace("m_bViewModelEffect = true", "m_bViewModelEffect = false")
    # Trees already processed by the trail->sprite rewrite: put the rope renderer back.
    new = new.replace(
        '_class = "C_OP_RenderSprites"\n\t\t\tm_bBlendFramesSeq0 = true',
        '_class = "C_OP_RenderRopes"',
    )
    new = new.replace('_class = "C_OP_RenderSprites"', '_class = "C_OP_RenderRopes"', 1)
    new = _ensure_cs2_muzzle_position_offset(new, MUZZLE_OFFSET_BARREL_TIP)
    new = ensure_brief_position_lock(new)
    new = remove_muzzle_follow_config(new)
    new = dedup_blend_frames_key(new)
    return new
