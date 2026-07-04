"""Modern (MW2019/ARC9) pack-specific CS2 VPCF gameplay composites and alignment.

Imports only from postprocess_common -- never from postprocess_povarehok, so the two
pack files stay independent of each other.
"""

from __future__ import annotations

import re
from pathlib import Path

import postprocess_common as common

# ---------------------------------------------------------------------------
# Gameplay composites (2026-07-03): compose REAL pack assets (plus stock CS2
# distortion systems) into the runtime shapes the GMod mod produced with Lua,
# which PCF conversion alone cannot express:
#   - per-shot barrel smoke (ARC9 AfterShotParticle) as children of the modern
#     muzzle flashes;
#   - sniper shots (AWP + autosnipers) get the pack's M82 treatment: shock-dust
#     ring around the shooter + heavy barrel plume + heat distortion;
#   - a grenade flight smoke trail (synthesized from the pack's own
#     barrel_smoke_trail with un-capped emission) swapped over CS2's
#     spectator utility-trail system;
#   - native CS2 refraction restored: converted explosions get the STOCK CS2
#     distort systems as children (the originals' Source 1 refract quads were
#     removed by fix_textureless_renderers), and the pack's muzzle_heatwave
#     files are REWRITTEN as behavior-version-12 systems cloned from the stock
#     explosion_hegrenade_distort renderer. (Lesson 2026-07-03: injecting bare
#     m_flRefractAmount keys into the converted behavior-8 files compiled fine
#     but never engaged the refract shader path -- the warp NORMAL MAP rendered
#     as plain color, the user-reported purple/rainbow blob.)
# This pass is idempotent (safe to re-run on an already-patched tree).
# ---------------------------------------------------------------------------

MODERN_MUZZLE_DIR = "particles/filmmaker/modern/arc9_fas_muzzleflashes"
MODERN_EXPLOSION_DIR = "particles/filmmaker/modern/arc9_fas_explosions"

# Spray-gated barrel smoke (2026-07-03 rework, user: "smoke should only appear
# after multiple shots in a short time, not after one shot"): the per-shot
# barrel_smoke children previously added to the class flashes are REMOVED, and
# per-flash `mvm_spray_*` composition wrappers (flash + smoke) are written
# instead. The C++ hook counts recent creations of each muzzle-flash name and
# upgrades the swap target to the spray wrapper only once a spray is detected
# (kSprayPairs in ParticleFxSpray.cpp). Snipers keep their per-shot plume via the
# mvm_muzzleflash_sniper_* compositions -- authentic .50-cal behavior.
MODERN_SPRAY_SMOKE = {
    "muzzleflash_ar.vpcf": "barrel_smoke.vpcf",
    "muzzleflash_smg.vpcf": "barrel_smoke.vpcf",
    "muzzleflash_shotgun.vpcf": "barrel_smoke.vpcf",
    "muzzleflash_pistol.vpcf": "barrel_smoke.vpcf",
    "muzzleflash_pistol_deagle.vpcf": "barrel_smoke.vpcf",
    "muzzleflash_lmg.vpcf": "barrel_smoke_plume.vpcf",
    "muzzleflash_dmr.vpcf": "barrel_smoke_plume.vpcf",
}

# Synthesized composition systems (children-only files, same structure as the
# pack's own barrel_smoke.vpcf). Sniper comps reproduce the mod's .50-cal look.
MVM_COMPOSITIONS = {
    f"{MODERN_MUZZLE_DIR}/mvm_muzzleflash_sniper_awp.vpcf": (
        f"{MODERN_MUZZLE_DIR}/muzzleflash_smg.vpcf",
        f"{MODERN_MUZZLE_DIR}/m82_shocksmoke.vpcf",
        f"{MODERN_MUZZLE_DIR}/barrel_smoke_plume.vpcf",
        f"{MODERN_MUZZLE_DIR}/muzzle_heatwave.vpcf",
    ),
    f"{MODERN_MUZZLE_DIR}/mvm_muzzleflash_sniper_auto.vpcf": (
        f"{MODERN_MUZZLE_DIR}/muzzleflash_dmr.vpcf",
        f"{MODERN_MUZZLE_DIR}/m82_shocksmoke.vpcf",
        f"{MODERN_MUZZLE_DIR}/barrel_smoke_plume.vpcf",
        f"{MODERN_MUZZLE_DIR}/muzzle_heatwave.vpcf",
    ),
    f"{MODERN_MUZZLE_DIR}/mvm_grenade_trail.vpcf": (
        f"{MODERN_MUZZLE_DIR}/mvm_grenade_smoke_trail.vpcf",
        f"{MODERN_MUZZLE_DIR}/mvm_grenade_smoke_trail_b.vpcf",
    ),
}

# First-person twins of the sniper compositions above: identical except the flash CHILD is
# the viewmodel-effect _fp leaf (the shock-dust / plume / heat children stay world-space, as
# they do for the world composition -- only the flash needs the viewmodel pass to align in
# first person). The _fps sniper rows in kVariantWeaponFx route here.
MVM_COMPOSITIONS_FP = {
    f"{MODERN_MUZZLE_DIR}/mvm_muzzleflash_sniper_awp_fp.vpcf": (
        f"{MODERN_MUZZLE_DIR}/muzzleflash_smg_fp.vpcf",
        f"{MODERN_MUZZLE_DIR}/m82_shocksmoke.vpcf",
        f"{MODERN_MUZZLE_DIR}/barrel_smoke_plume.vpcf",
        f"{MODERN_MUZZLE_DIR}/muzzle_heatwave.vpcf",
    ),
    f"{MODERN_MUZZLE_DIR}/mvm_muzzleflash_sniper_auto_fp.vpcf": (
        f"{MODERN_MUZZLE_DIR}/muzzleflash_dmr_fp.vpcf",
        f"{MODERN_MUZZLE_DIR}/m82_shocksmoke.vpcf",
        f"{MODERN_MUZZLE_DIR}/barrel_smoke_plume.vpcf",
        f"{MODERN_MUZZLE_DIR}/muzzle_heatwave.vpcf",
    ),
}

# Grenade-trail systems, authored from scratch (see _trail_system_text). The old
# approach cloned barrel_smoke_trail with un-capped emission, which kept puffing
# forever once the grenade landed or stuck in a wall (user report 2026-07-03);
# these gate the emit rate on control-point SPEED so a resting grenade emits
# nothing while a flying one leaves the MW-style puff trail.
MVM_GRENADE_TRAILS = {
    f"{MODERN_MUZZLE_DIR}/mvm_grenade_smoke_trail.vpcf": dict(
        texture="materials/effects/fas_dust_a.vtex", rate=16.0,
        radius_min=3.0, radius_max=6.0, grey=150, alpha=0.45),
    f"{MODERN_MUZZLE_DIR}/mvm_grenade_smoke_trail_b.vpcf": dict(
        texture="materials/effects/fas_dust_b.vtex", rate=10.0,
        radius_min=4.0, radius_max=8.0, grey=120, alpha=0.35),
}

# Converted explosion roots -> stock CS2 distortion child restoring the native
# heat-shimmer the vanilla systems had.
MODERN_DISTORT_CHILDREN = {
    f"{MODERN_EXPLOSION_DIR}/explosion_grenade.vpcf": common.STOCK_HE_DISTORT,
}

# The pack's per-shot heat distortion, rewritten as full behavior-12 systems
# (size/lifetime per file). REVERTED to mod-authentic scope (2026-07-03 night,
# user report): only the .50-cal / auto-sniper class (AWP + the two autosniper
# compositions) gets this in the mod's own Lua -- it was briefly added as a
# child of every unsuppressed class flash and the user asked for that undone.
# muzzle_heatwave_long is unused by any current rule but kept authored/compiled
# in case a future composition wants the longer variant.
MVM_HEATWAVES = {
    f"{MODERN_MUZZLE_DIR}/muzzle_heatwave.vpcf": dict(
        particles=3, lifetime=0.4, end_scale=10.0, offset_lo=6.0, offset_hi=16.0),
    f"{MODERN_MUZZLE_DIR}/muzzle_heatwave_long.vpcf": dict(
        particles=4, lifetime=0.7, end_scale=16.0, offset_lo=8.0, offset_hi=24.0),
}

# Flash files that ONCE had muzzle_heatwave added as a child (2026-07-03 pm) and
# must have it stripped from an already-patched tree. Kept only for that one-time
# cleanup pass; do not add new entries here -- the sniper compositions
# (mvm_muzzleflash_sniper_awp/auto in kVariantWeaponFx) are the only intended users.
MODERN_HEATWAVE_CHILDREN_TO_REMOVE = (
    "muzzleflash_ar.vpcf",
    "muzzleflash_smg.vpcf",
    "muzzleflash_dmr.vpcf",
    "muzzleflash_lmg.vpcf",
    "muzzleflash_shotgun.vpcf",
    "muzzleflash_pistol.vpcf",
    "muzzleflash_pistol_deagle.vpcf",
)


def _distort_system_text(*, particles: int, lifetime: float, end_scale: float,
                         offset_lo: float, offset_hi: float) -> str:
    """A muzzle-sized clone of CS2's stock explosion_hegrenade_distort.

    Renderer keys (refract amount/blur + warp normal map) match the stock
    system; everything else is a minimal spawn: a few short-lived quads just
    past the muzzle that grow, spin, and fade.

    LESSON (2026-07-03 night, user report "just a circle that gets bigger, not
    actually distorting anything"): the stock file's m_vecColorScale is a warm
    tan tint ([255,188,154]) added on top of the refracted background via
    PARTICLE_OUTPUT_BLEND_MODE_ADD. On a large HE-grenade blast that tint reads
    as part of the fire glow; at muzzle scale against a flat wall (no texture
    detail to visibly bend) it's the ONLY thing visible -- an expanding warm
    blob with no apparent displacement. Color scale here is [0,0,0]: additive
    zero adds nothing, so the quad is invisible except for the refraction
    offset it applies to whatever is drawn behind it -- real heat-shimmer
    (only visible where the background has contrast/detail to warp).
    """
    return f"""{common.VPCF12_HEADER}
{{
	_class = "CParticleSystemDefinition"
	m_nBehaviorVersion = 12
	m_nMaxParticles = 8
	m_Emitters =
	[
		{{
			_class = "C_OP_InstantaneousEmitter"
			m_nParticlesToEmit =
			{{
				m_nType = "PF_TYPE_LITERAL"
				m_flLiteralValue = {float(particles)}
			}}
		}},
	]
	m_Initializers =
	[
		{{
			_class = "C_INIT_CreateWithinSphereTransform"
			m_flRadius =
			{{
				m_nType = "PF_TYPE_RANDOM_UNIFORM"
				m_flRandomMin = 0.0
				m_flRandomMax = 4.0
			}}
		}},
		{{
			_class = "C_INIT_PositionOffset"
			m_bLocalCoords = true
			m_OffsetMin = [ {offset_lo}, 0.0, 0.0 ]
			m_OffsetMax = [ {offset_hi}, 0.0, 0.0 ]
		}},
		{{
			_class = "C_INIT_InitFloat"
			m_InputValue =
			{{
				m_nType = "PF_TYPE_LITERAL"
				m_flLiteralValue = 2.0
			}}
		}},
		{{
			_class = "C_INIT_InitFloat"
			m_InputValue =
			{{
				m_nType = "PF_TYPE_LITERAL"
				m_flLiteralValue = {lifetime}
			}}
			m_nOutputField = 1
		}},
		{{
			_class = "C_INIT_InitFloat"
			m_InputValue =
			{{
				m_nType = "PF_TYPE_RANDOM_UNIFORM"
				m_flRandomMin = 0.0
				m_flRandomMax = 360.0
			}}
			m_nOutputField = 4
		}},
		{{
			_class = "C_INIT_InitFloat"
			m_InputValue =
			{{
				m_nType = "PF_TYPE_RANDOM_UNIFORM"
				m_flRandomMin = 20.0
				m_flRandomMax = 60.0
				m_bHasRandomSignFlip = true
			}}
			m_nOutputField = 5
		}},
	]
	m_Operators =
	[
		{{
			_class = "C_OP_BasicMovement"
			m_fDrag = 0.15
		}},
		{{
			_class = "C_OP_SpinUpdate"
		}},
		{{
			_class = "C_OP_Decay"
		}},
		{{
			_class = "C_OP_FadeOut"
			m_flFadeOutTimeMin = 0.6
			m_flFadeOutTimeMax = 0.6
			m_bEaseInAndOut = false
		}},
		{{
			_class = "C_OP_InterpolateRadius"
			m_flStartScale = 3.0
			m_flEndScale = {end_scale}
			m_flBias = 0.667
		}},
	]
	m_Renderers =
	[
		{{
			_class = "C_OP_RenderSprites"
			m_bDisableZBuffering = true
			m_flRefractAmount = -0.5
			m_nRefractBlurRadius = 3
			m_nRefractBlurType = "BLURFILTER_BOX"
			m_flAnimationRate = 2.0
			m_vecTexturesInput =
			[
				{{
					m_hTexture = resource:"{common.STOCK_WARP_NORMAL}"
					m_nTextureBlendMode = "SPRITECARD_TEXTURE_BLEND_LUMINANCE"
				}},
			]
			m_vecColorScale =
			{{
				m_nType = "PVEC_TYPE_LITERAL_COLOR"
				m_LiteralColor = [ 0, 0, 0 ]
			}}
			m_bUseMixedResolutionRendering = true
			m_nOutputBlendMode = "PARTICLE_OUTPUT_BLEND_MODE_ADD"
		}},
	]
}}
"""


def _trail_system_text(*, texture: str, rate: float, radius_min: float,
                       radius_max: float, grey: int, alpha: float) -> str:
    """A grenade-flight puff trail whose EMISSION is gated on CP0 speed.

    CP0 is the projectile (the spectator_utility_trail anchor), so a grenade in
    flight emits puffs and one at rest (landed / stuck in a wall) emits nothing
    -- the whole point of authoring these instead of cloning barrel_smoke_trail.
    Discrete sprites, not ropes: rope ribbons stretch across the flight path's
    corners and read as smoke floating in mid-air.
    """
    g = grey / 255.0
    return f"""{common.VPCF12_HEADER}
{{
	_class = "CParticleSystemDefinition"
	m_nBehaviorVersion = 12
	m_nMaxParticles = 128
	m_Emitters =
	[
		{{
			_class = "C_OP_ContinuousEmitter"
			m_flEmitRate =
			{{
				m_nType = "PF_TYPE_CONTROL_POINT_SPEED"
				m_nControlPoint = 0
				m_nMapType = "PF_MAP_TYPE_REMAP"
				m_nInputMode = "PF_INPUT_MODE_CLAMPED"
				m_flInput0 = 30.0
				m_flInput1 = 500.0
				m_flOutput0 = 0.0
				m_flOutput1 = {rate}
			}}
		}},
	]
	m_Initializers =
	[
		{{
			_class = "C_INIT_CreateWithinSphereTransform"
			m_flRadius =
			{{
				m_nType = "PF_TYPE_RANDOM_UNIFORM"
				m_flRandomMin = 0.0
				m_flRandomMax = 3.0
			}}
		}},
		{{
			_class = "C_INIT_InitFloat"
			m_InputValue =
			{{
				m_nType = "PF_TYPE_RANDOM_UNIFORM"
				m_flRandomMin = {radius_min}
				m_flRandomMax = {radius_max}
			}}
		}},
		{{
			_class = "C_INIT_InitFloat"
			m_InputValue =
			{{
				m_nType = "PF_TYPE_RANDOM_UNIFORM"
				m_flRandomMin = 1.5
				m_flRandomMax = 2.5
			}}
			m_nOutputField = 1
		}},
		{{
			_class = "C_INIT_InitFloat"
			m_InputValue =
			{{
				m_nType = "PF_TYPE_LITERAL"
				m_flLiteralValue = {alpha}
			}}
			m_nOutputField = 7
		}},
		{{
			_class = "C_INIT_InitFloat"
			m_InputValue =
			{{
				m_nType = "PF_TYPE_RANDOM_UNIFORM"
				m_flRandomMin = 0.0
				m_flRandomMax = 360.0
			}}
			m_nOutputField = 4
		}},
		{{
			_class = "C_INIT_InitFloat"
			m_InputValue =
			{{
				m_nType = "PF_TYPE_RANDOM_UNIFORM"
				m_flRandomMin = 10.0
				m_flRandomMax = 30.0
				m_bHasRandomSignFlip = true
			}}
			m_nOutputField = 5
		}},
	]
	m_Operators =
	[
		{{
			_class = "C_OP_BasicMovement"
			m_Gravity = [ 0.0, 0.0, 10.0 ]
			m_fDrag = 0.05
		}},
		{{
			_class = "C_OP_SpinUpdate"
		}},
		{{
			_class = "C_OP_Decay"
		}},
		{{
			_class = "C_OP_FadeOut"
			m_flFadeOutTimeMin = 0.8
			m_flFadeOutTimeMax = 0.8
			m_bEaseInAndOut = false
		}},
		{{
			_class = "C_OP_InterpolateRadius"
			m_flStartScale = 1.0
			m_flEndScale = 2.5
		}},
	]
	m_Renderers =
	[
		{{
			_class = "C_OP_RenderSprites"
			m_bBlendFramesSeq0 = true
			m_vecTexturesInput =
			[
				{{
					m_hTexture = resource:"{texture}"
				}},
			]
			m_vecColorScale =
			{{
				m_nType = "PVEC_TYPE_LITERAL"
				m_vLiteralValue = [ {g:.3f}, {g:.3f}, {g:.3f} ]
			}}
			m_flSelfIllumAmount =
			{{
				m_nType = "PF_TYPE_LITERAL"
				m_flLiteralValue = 1.0
			}}
		}},
	]
}}
"""


# The pack's per-shot barrel smoke ribbons. Converted as C_OP_RenderRopes, the
# engine stretches ONE ribbon across the muzzle's whole sweep between puffs --
# the "smoke floating in mid-air far from the muzzle" arcs (user report
# 2026-07-03). Discrete sprites at a higher rate/size read as the same smoke
# stream without the stretched-ribbon artifact; lifetime comes down from the
# mod's 5s so spent smoke doesn't hang around whole fights. Exact-string
# replacements: idempotent by construction.
# Modern sustained barrel smoke (spray-gated). Regular On/Less uses weapon_muzzle_smoke_long
# instead (see postprocess_povarehok); both share the shared muzzle-smoke alignment recipe.
MODERN_BARREL_TRAIL_FILES = (
    f"{MODERN_MUZZLE_DIR}/barrel_smoke_trail.vpcf",
    f"{MODERN_MUZZLE_DIR}/barrel_smoke_trail_b.vpcf",
)
# The files ACTUALLY swapped to at runtime (kVariantWeaponFx's Modern targets for CS2's
# own weapon_muzzle_smoke/weapon_muzzle_smoke_long, and the smoke half of every
# MODERN_SPRAY_SMOKE composition below). Bug (2026-07-03, "Modern's muzzle attach doesn't
# follow Povarehok's"): only the *_trail files above ever received the CS2 muzzle
# alignment patch, but nothing in the ParticleFx swap tables (ParticleFxRules.cpp) ever swaps to them -- they are dead
# output. These two are the ones that actually spawn on screen; route them through the
# identical alignment recipe Povarehok's own weapon_muzzle_smoke_long already gets.
MODERN_LIVE_BARREL_SMOKE_FILES = (
    f"{MODERN_MUZZLE_DIR}/barrel_smoke.vpcf",
    f"{MODERN_MUZZLE_DIR}/barrel_smoke_plume.vpcf",
)

MODERN_MUZZLEFLASH_FILES = (
    f"{MODERN_MUZZLE_DIR}/muzzleflash_ar.vpcf",
    f"{MODERN_MUZZLE_DIR}/muzzleflash_smg.vpcf",
    f"{MODERN_MUZZLE_DIR}/muzzleflash_shotgun.vpcf",
    f"{MODERN_MUZZLE_DIR}/muzzleflash_pistol.vpcf",
    f"{MODERN_MUZZLE_DIR}/muzzleflash_pistol_deagle.vpcf",
    f"{MODERN_MUZZLE_DIR}/muzzleflash_lmg.vpcf",
    f"{MODERN_MUZZLE_DIR}/muzzleflash_dmr.vpcf",
    f"{MODERN_MUZZLE_DIR}/muzzleflash_suppressed.vpcf",
)

MODERN_FLASH_BURST_OLD_TEXTURE = "materials/effects/hl2_muzzleflash.vtex"
MODERN_FLASH_BURST_NEW_TEXTURE = "materials/effects/fas_muzzleflash_test_b.vtex"

# First-person (_fp) muzzle-flash twins. Bug (user report 2026-07-04, "modern first-person
# muzzle flashes don't align -- they float off to the side of the gun; in third person the
# flash sits right"): the pack ships ONE flash asset per weapon and kVariantWeaponFx routed
# BOTH the first-person (_fps) and the world CS2 muzzle systems to it. A world-space flash
# (m_bViewModelEffect = false) anchors correctly on the weapon in third person / free cam,
# but the first-person viewmodel is drawn in a separate pass with viewmodel FOV, so a
# world-space flash placed at the viewmodel muzzle floats out of line. Povarehok solves this
# by shipping a SEPARATE _fp flash with m_bViewModelEffect = true and routing the _fps
# systems to it (see weapon_muzzle_flash_*_fp in kVariantWeaponFx / kSprayPairs). We do the
# same: apply_modern_gameplay_composites writes a viewmodel-effect twin of every world flash
# above, and the _fps rows in kVariantWeaponFx point at the _fp twin (barrel smoke already
# gets its own viewmodel attach, which is why it aligned while the flash did not).

# Modern tracer parents actually routed to at runtime (kVariantTracers + kTracerFallbacks in
# ParticleFxRules.cpp). Bug (user report 2026-07-04, "tracers don't work on every gun -- some
# pistols shoot the tracer in a random direction, and on the AK/M249 the tracer is locked to
# the gun instead of flying toward what you're shooting"): converted from GMod, these fly via
# C_INIT_MoveBetweenPoints from CP0 (muzzle) to CP1. CS2's own weapon-tracer dispatch never
# sets a tracer END control point -- stock weapon_tracers.vpcf flies FORWARD on a local-space
# velocity and ignores CP1. With CP1 unset the converted tracer interpolates toward the world
# origin (a fixed wrong direction) or, where a stray local-forward velocity also survived
# conversion (mw2019_tracer_small), fights it. Rebuild them on the SAME native chassis
# Povarehok's own working tracer uses: muzzle-local forward velocity + a velocity-traced
# lifespan, no CP1 dependency.
MODERN_TRACER_DIR = "particles/filmmaker/modern/mw2019_tracer"
MODERN_TRACER_PARENTS = (
    f"{MODERN_TRACER_DIR}/mw2019_tracer.vpcf",
    f"{MODERN_TRACER_DIR}/mw2019_tracer_fast.vpcf",
    f"{MODERN_TRACER_DIR}/mw2019_tracer_slow.vpcf",
    f"{MODERN_TRACER_DIR}/mw2019_tracer_small.vpcf",
)
# Local +X (muzzle-forward) tracer speed, matched to CS2 stock weapon_tracers and Povarehok's
# converted tracer (both 2400 u/s + a 2048u velocity-traced lifespan).
_TRACER_LOCAL_FORWARD = "2400.0"

# Modern rope wisps attach to the barrel and trace gun motion (§6b). The Povarehok
# plume offset (postprocess_common._CS2_MUZZLE_OFFSET_BLOCK) pushes spawn 1-6 units
# forward and 2-4 up -- user report 2026-07-03 night: Modern FP rope reads in front of
# and above every weapon's muzzle.
_CS2_MODERN_ROPE_TRAIL_OFFSET_BLOCK = (
    "\t\t{\n"
    '\t\t\t_class = "C_INIT_PositionOffset"\n'
    "\t\t\tm_bLocalCoords = true\n"
    "\t\t\tm_OffsetMin = [0.0, 0.0, -0.5]\n"
    "\t\t\tm_OffsetMax = [0.5, 0.0, 0.0]\n"
    "\t\t},\n"
)


def _force_cs2_modern_rope_trail_offset_block(block: str) -> str:
    """Barrel-tip spawn for Modern rope wisps (not Povarehok plume forward/up kick)."""
    block = re.sub(r"m_bLocalCoords = (?:true|false)", "m_bLocalCoords = true", block)
    if "m_bLocalCoords" not in block:
        block = block.replace(
            '_class = "C_INIT_PositionOffset"',
            '_class = "C_INIT_PositionOffset"\n\t\t\tm_bLocalCoords = true',
            1,
        )
    block = re.sub(r"m_OffsetMin = \[[^\]]*\]", "m_OffsetMin = [0.0, 0.0, -0.5]", block)
    block = re.sub(r"m_OffsetMax = \[[^\]]*\]", "m_OffsetMax = [0.5, 0.0, 0.0]", block)
    return block


def _ensure_cs2_modern_rope_trail_position_offset(text: str) -> str:
    if "C_INIT_PositionOffset" in text:
        edits = []
        for match in re.finditer(r'_class = "C_INIT_PositionOffset"', text):
            start, end = common.block_span(text, match.start())
            edits.append((start, end, _force_cs2_modern_rope_trail_offset_block(text[start:end])))
        for start, end, replacement in reversed(edits):
            text = text[:start] + replacement + text[end:]
        return text
    anchor = re.search(r'_class = "C_INIT_CreateWithinSphere"', text)
    if not anchor:
        return text
    insert_at = text.find("\n\t\t},", anchor.start())
    if insert_at < 0:
        return text
    return (
        text[: insert_at + len("\n\t\t},")]
        + "\n"
        + _CS2_MODERN_ROPE_TRAIL_OFFSET_BLOCK
        + text[insert_at + len("\n\t\t},") :]
    )


def _tune_modern_rope_trail_particles(text: str) -> str:
    """Keep C_OP_RenderRopes; reduce world-up velocity bias and soften the ribbon."""
    new = text
    # World-space noise with m_vecOutput Z=7..10 lifts the ribbon above the barrel.
    #
    # BUG (found 2026-07-04, user report "wisp sits around, looks like you move your
    # gun"): this used to force false->true, i.e. LOCAL space, the opposite of what the
    # comment above says and wants. In local space the noise/movement operator's output
    # is recomputed relative to the (moving, viewmodel-attached) control point every
    # frame, so an already-spawned puff keeps re-anchoring to the current muzzle
    # position/orientation instead of drifting freely -- it reads as glued to the barrel
    # and dragged around as the gun moves. Force WORLD space instead: the initial
    # C_INIT_PositionOffset spawn point stays local (muzzle-anchored, unchanged), but the
    # wisp's post-spawn rise is independent world-space motion, like real smoke.
    new = re.sub(r"m_bLocalSpace = (?:true|false)", "m_bLocalSpace = false", new, count=1)
    new = re.sub(
        r"m_vecOutputMin = \[[^\]]*\]",
        "m_vecOutputMin = [-1.0, -1.0, -0.5]",
        new,
        count=1,
    )
    new = re.sub(
        r"m_vecOutputMax = \[[^\]]*\]",
        "m_vecOutputMax = [1.0, 1.0, 0.5]",
        new,
        count=1,
    )
    new = new.replace("m_flTessScale = 10", "m_flTessScale = 6")
    new = new.replace("m_flConstantRadius = 5.0", "m_flConstantRadius = 3.5")
    new = re.sub(r"m_flEndScale = 3\.0", "m_flEndScale = 2.0", new)
    new = re.sub(r"m_nAlphaMax = 50", "m_nAlphaMax = 38", new)
    new = re.sub(r"m_flSelfIllumAmount = 1\.0", "m_flSelfIllumAmount = 0.35", new, count=1)
    return new


def patch_cs2_modern_rope_trail_alignment(text: str) -> str:
    """Modern-only barrel rope wisps: spawn at muzzle tip, not Povarehok plume offset."""
    new = common.patch_cs2_muzzle_rope_trail_alignment(text)
    new = _ensure_cs2_modern_rope_trail_position_offset(new)
    new = _tune_modern_rope_trail_particles(new)
    return new


def patch_cs2_modern_barrel_smoke_alignment(text: str) -> str:
    """Modern barrel_smoke_plume + wrapper: viewmodel attach at barrel tip (not plume [1-6,+Z])."""
    new = text.replace("m_bLocalCoords = false", "m_bLocalCoords = true")
    new = new.replace("m_bViewModelEffect = false", "m_bViewModelEffect = true")
    new = common._ensure_viewmodel_effect_flag(new)
    new = _ensure_cs2_modern_rope_trail_position_offset(new)
    # Same bug as _tune_modern_rope_trail_particles (found 2026-07-04): forcing LOCAL
    # space re-anchors the plume's noise/movement to the moving muzzle every frame, so
    # the LMG/DMR black smoke plume sits glued to the barrel instead of drifting up into
    # world space. Force world space so the spawn point stays muzzle-anchored but the
    # post-spawn rise is independent.
    new = re.sub(r"m_bLocalSpace = (?:true|false)", "m_bLocalSpace = false", new)
    new = re.sub(
        r"m_vecOutputMin = \[[^\]]*\]",
        "m_vecOutputMin = [-1.0, -1.0, -0.5]",
        new,
        count=1,
    )
    new = re.sub(
        r"m_vecOutputMax = \[[^\]]*\]",
        "m_vecOutputMax = [1.0, 1.0, 0.5]",
        new,
        count=1,
    )
    return new


def patch_cs2_modern_muzzleflash_alignment(text: str) -> str:
    """Modern per-shot muzzle flash: WORLD-space, muzzle-attachment anchored.

    Bug (user report 2026-07-04, "modern muzzle flash unaligns when you move the
    camera; there's no third-person flash at all"): this used to force
    m_bViewModelEffect = true. A viewmodel-effect system renders ONLY in the
    first-person viewmodel pass -- it is drawn at the viewmodel's position with
    viewmodel FOV, which is fine while the camera sits on the spectated eye but
    floats out into mid-air the instant the view detaches (third-person orbit /
    free cam), and never produces a world-anchored flash on the actual weapon.
    Because the SAME asset is the swap target for both the first-person (_fps) and
    the third-person (world) CS2 muzzle systems (kVariantWeaponFx), forcing the
    viewmodel flag broke BOTH camera-detached cases at once.

    Povarehok's own world muzzle flashes (weapon_muzzle_flash_*.vpcf) never set
    this flag -- they render in WORLD space and anchor to the weapon's muzzle
    control point, so they look right from every camera (first person, third
    person, free cam) with no extra machinery. Match that here: leave the flash
    world-space (viewmodel effect OFF) and keep only the muzzle-local spawn offset
    so it still sits at the barrel tip. First-person in-eye spectating still shows
    it correctly (the control point is at the viewmodel muzzle then, exactly as it
    is for Povarehok); detached cameras now get a real world flash via the existing
    _fps-suppress + spec_mode-3 chase path (see Filmmaker.cpp RunMainThreadFrame).
    """
    new = text.replace("m_bLocalCoords = false", "m_bLocalCoords = true")
    # World-space, NOT a viewmodel effect (see the docstring). Clear any inherited
    # true and never re-add the flag; an absent key defaults to false = world-space,
    # which is exactly what Povarehok's flashes carry.
    new = new.replace("m_bViewModelEffect = true", "m_bViewModelEffect = false")
    new = _ensure_cs2_modern_rope_trail_position_offset(new)
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
    return new


def _fp_variant_res(res: str) -> str:
    """particles/.../muzzleflash_ar.vpcf -> particles/.../muzzleflash_ar_fp.vpcf."""
    assert res.endswith(".vpcf"), res
    return res[:-len(".vpcf")] + "_fp.vpcf"


def make_modern_muzzleflash_fp(world_text: str) -> str:
    """First-person viewmodel twin of a world muzzle flash.

    Identical to the world flash except drawn in the viewmodel pass
    (m_bViewModelEffect = true) so it lines up with the foreshortened first-person weapon,
    exactly how CS2 (and Povarehok) ship a separate _fp flash. The world twin keeps
    viewmodel OFF so it anchors on the weapon for third person / free cam. The _fps CS2
    source systems route here; the world systems route to the world twin.
    """
    new = world_text.replace("m_bViewModelEffect = false", "m_bViewModelEffect = true")
    return common._ensure_viewmodel_effect_flag(new)


def _ensure_modern_flash_position_lock(text: str) -> str:
    if 'C_OP_PositionLock' in text:
        return text
    element = (
        "\t\t{\n"
        '\t\t\t_class = "C_OP_PositionLock"\n'
        "\t\t},\n"
    )
    match = re.search(r"m_Operators = \n(\t*)\[\n", text)
    if match:
        return text[:match.end()] + element + text[match.end():]
    anchor = re.search(r'_class = "CParticleSystemDefinition"\n', text)
    if not anchor:
        return text
    block = "\tm_Operators = \n\t[\n" + element + "\t]\n\n"
    return text[:anchor.end()] + block + text[anchor.end():]


def patch_modern_flash_burst(text: str) -> str:
    """Retarget old HL2 sprite bursts and keep them locked to the muzzle."""
    edits = []
    for start, end in common.iter_renderer_blocks(text):
        block = text[start:end]
        if '_class = "C_OP_RenderSprites"' not in block:
            continue
        if MODERN_FLASH_BURST_OLD_TEXTURE not in block:
            continue
        replacement = block.replace(
            MODERN_FLASH_BURST_OLD_TEXTURE,
            MODERN_FLASH_BURST_NEW_TEXTURE,
        )
        edits.append((start, end, replacement))
    for start, end, replacement in reversed(edits):
        text = text[:start] + replacement + text[end:]
    return _ensure_modern_flash_position_lock(text) if edits else text


def _remove_init_blocks(text: str, class_name: str) -> str:
    """Delete every initializer/operator array element of the given _class, comma and all."""
    needle = f'_class = "{class_name}"'
    while True:
        idx = text.find(needle)
        if idx < 0:
            return text
        start, end = common.block_span(text, idx)
        line_start = text.rfind("\n", 0, start) + 1
        j = end
        if j < len(text) and text[j] == ",":
            j += 1
        if j < len(text) and text[j] == "\n":
            j += 1
        text = text[:line_start] + text[j:]


def patch_modern_tracer_forward(text: str) -> str:
    """Native-chassis forward-flying tracer (see MODERN_TRACER_PARENTS).

    Drops the CP1-dependent C_INIT_MoveBetweenPoints (CS2 never sets a tracer end CP) and
    the fixed C_INIT_RandomLifeTime, gives the spawn a muzzle-local forward velocity, and
    derives lifespan from that velocity so the streak ends at the impact surface -- the
    same shape CS2's stock weapon_tracers and Povarehok's converted tracer already use.
    """
    text = _remove_init_blocks(text, "C_INIT_MoveBetweenPoints")
    text = _remove_init_blocks(text, "C_INIT_RandomLifeTime")
    m = re.search(r'_class = "C_INIT_CreateWithinSphere"', text)
    if m:
        start, end = common.block_span(text, m.start())
        block = text[start:end]
        block = re.sub(
            r"m_LocalCoordinateSystemSpeedMin = \[[^\]]*\]",
            f"m_LocalCoordinateSystemSpeedMin = [{_TRACER_LOCAL_FORWARD}, 0.0, 0.0]", block)
        block = re.sub(
            r"m_LocalCoordinateSystemSpeedMax = \[[^\]]*\]",
            f"m_LocalCoordinateSystemSpeedMax = [{_TRACER_LOCAL_FORWARD}, 0.0, 0.0]", block)
        block = re.sub(r"m_bLocalCoords = (?:true|false)", "m_bLocalCoords = true", block)
        text = text[:start] + block + text[end:]
        if "C_INIT_LifespanFromVelocity" not in text:
            m2 = re.search(r'_class = "C_INIT_CreateWithinSphere"', text)
            _, e2 = common.block_span(text, m2.start())
            j = e2
            if j < len(text) and text[j] == ",":
                j += 1
            if j < len(text) and text[j] == "\n":
                j += 1
            lifespan = (
                "\t\t{\n"
                '\t\t\t_class = "C_INIT_LifespanFromVelocity"\n'
                "\t\t\tm_flMaxTraceLength = 2048.0\n"
                "\t\t},\n"
            )
            text = text[:j] + lifespan + text[j:]
    # The GMod tracer spawned 150u ahead of the muzzle (a MoveBetweenPoints head-start);
    # with real forward motion that just opens a gap between the muzzle and the streak.
    text = text.replace("m_OffsetMin = [150.0, 0.0, 0.0]", "m_OffsetMin = [0.0, 0.0, 0.0]")
    text = text.replace("m_OffsetMax = [150.0, 0.0, 0.0]", "m_OffsetMax = [0.0, 0.0, 0.0]")
    return text


def apply_modern_gameplay_composites(root: Path) -> list[str]:
    """Returns the content-relative resources that were written/changed."""
    changed: list[str] = []

    for res, params in MVM_HEATWAVES.items():
        common.write_if_different(root, res, _distort_system_text(**params), changed)

    for res, params in MVM_GRENADE_TRAILS.items():
        common.write_if_different(root, res, _trail_system_text(**params), changed)

    muzzle_root = common.resource_path(root, MODERN_MUZZLE_DIR)
    if muzzle_root.is_dir():
        for path in muzzle_root.glob("*.vpcf"):
            if path.name.lower().startswith("testd"):
                continue
            text = path.read_text(encoding="utf-8")
            new_text = patch_modern_flash_burst(text)
            if new_text != text:
                path.write_text(new_text, encoding="utf-8")
                changed.append(path.relative_to(root).as_posix())

    for res in MODERN_BARREL_TRAIL_FILES:
        path = common.resource_path(root, res)
        if not path.is_file():
            continue
        text = new_text = path.read_text(encoding="utf-8")
        new_text = patch_cs2_modern_rope_trail_alignment(new_text)
        if new_text != text:
            path.write_text(new_text, encoding="utf-8")
            changed.append(res)

    for res in MODERN_LIVE_BARREL_SMOKE_FILES:
        path = common.resource_path(root, res)
        if not path.is_file():
            continue
        text = new_text = path.read_text(encoding="utf-8")
        new_text = patch_cs2_modern_barrel_smoke_alignment(new_text)
        if new_text != text:
            path.write_text(new_text, encoding="utf-8")
            changed.append(res)

    for res in MODERN_MUZZLEFLASH_FILES:
        path = common.resource_path(root, res)
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        world = patch_cs2_modern_muzzleflash_alignment(text)
        if world != text:
            path.write_text(world, encoding="utf-8")
            changed.append(res)
        # First-person viewmodel twin, written from the freshly-patched world flash so the
        # two only ever differ by the viewmodel-effect flag (see make_modern_muzzleflash_fp
        # and the _fps routing in kVariantWeaponFx). Must precede the _fp sniper
        # compositions below, which reference the _fp leaves as children.
        common.write_if_different(
            root, _fp_variant_res(res), make_modern_muzzleflash_fp(world), changed)

    for res in MODERN_TRACER_PARENTS:
        path = common.resource_path(root, res)
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        new_text = patch_modern_tracer_forward(text)
        if new_text != text:
            path.write_text(new_text, encoding="utf-8")
            changed.append(res)

    # Wire the "smoke follows the gun through the air" rope wisp back in (user
    # report 2026-07-03, see docs/mw2019-fx-mapping-reference.md §6b). ARC9's OWN
    # barrel_smoke.vpcf is already a thin wrapper whose ONLY content is a child
    # ref to barrel_smoke_trail_b.vpcf (confirmed post-conversion, 2026-07-03 --
    # barrel_smoke.vpcf has no renderers/operators of its own at all, so every
    # class in MODERN_SPRAY_SMOKE that resolves to it already gets the rope wisp
    # natively). barrel_smoke_plume.vpcf (the lmg/dmr/sniper-composition smoke)
    # has NO such child, so its classes never got a wisp at all. Give it the
    # OTHER trail file (barrel_smoke_trail.vpcf, previously unused by anything)
    # instead of duplicating barrel_smoke's own trail_b onto it.
    modern_trail_children = {
        f"{MODERN_MUZZLE_DIR}/barrel_smoke_plume.vpcf": f"{MODERN_MUZZLE_DIR}/barrel_smoke_trail.vpcf",
    }
    for parent_res, child_res in modern_trail_children.items():
        if not common.resource_path(root, child_res).is_file():
            continue
        if common._add_child_once(common.resource_path(root, parent_res), child_res):
            changed.append(parent_res)

    for res, children in {**MVM_COMPOSITIONS, **MVM_COMPOSITIONS_FP}.items():
        if not all(common.resource_path(root, c).is_file() for c in children):
            continue
        common.write_if_different(root, res, common._composition_text(children), changed)

    # Spray rework: strip the per-shot barrel smoke children the previous design
    # added directly to the class flashes, then write the spray wrappers.
    for name in MODERN_SPRAY_SMOKE:
        res = f"{MODERN_MUZZLE_DIR}/{name}"
        path = common.resource_path(root, res)
        if path.is_file() and common.remove_child_refs(
            path, {"barrel_smoke.vpcf", "barrel_smoke_plume.vpcf"}
        ):
            changed.append(res)

    for name, smoke in MODERN_SPRAY_SMOKE.items():
        flash_res = f"{MODERN_MUZZLE_DIR}/{name}"
        smoke_res = f"{MODERN_MUZZLE_DIR}/{smoke}"
        if not common.resource_path(root, flash_res).is_file() or not common.resource_path(root, smoke_res).is_file():
            continue
        common.write_if_different(
            root,
            common._spray_wrapper_res(flash_res),
            common._composition_text((flash_res, smoke_res)),
            changed,
        )

    for name in MODERN_HEATWAVE_CHILDREN_TO_REMOVE:
        res = f"{MODERN_MUZZLE_DIR}/{name}"
        path = common.resource_path(root, res)
        if path.is_file() and common.remove_child_refs(path, {"muzzle_heatwave.vpcf"}):
            changed.append(res)

    for res, distort in MODERN_DISTORT_CHILDREN.items():
        if common._add_child_once(common.resource_path(root, res), distort):
            changed.append(res)

    return changed
