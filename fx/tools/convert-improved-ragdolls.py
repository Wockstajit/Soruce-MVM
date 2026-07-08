#!/usr/bin/env python3
"""Produce CS2 "Improved Ragdolls" player models with correct, Valve-grade physics.

Background
----------
Earlier revisions of this tool tried to port jahpeg's Source 1 ragdoll metadata (body
masses, damping, joint limits) into ModelDoc as ``PhysicsBodyMarkup`` / ``PhysicsJointConical``
nodes. That approach is fundamentally broken: CS2's compiler drops Valve's authored
physics on any recompile (a decompiled agent recompiles to ``m_joints = []`` with zero
mass), and no source-level ModelDoc node can recreate a ragdoll joint's reference frames
(``PhysicsJointConical`` only ever fills the child frame, leaving the parent frame at
identity -> the swing/twist limits orient around the wrong axis -> the ragdoll goes
floppy, legs collapse into each other, the torso pinches thin).

Approach (two phases)
---------------------
Prepare phase (default): VRF decompiles every current ``ctm_*`` / ``tm_*`` player model
and relocates it verbatim under ``models/filmmaker/improved_ragdolls/agents/models/...``.
No physics editing -- the decompiled source keeps Valve's stock ``PhysicsShapeList`` so the
compiled model still emits a (mostly empty) PHYS block for the graft phase to replace.

Graft phase (``--graft-phys``, run AFTER resourcecompiler): raw-extracts each agent's stock
``vmdl_c`` and grafts its compiled PHYS block -- Valve's exact joint frames, masses (272),
inertia and damping -- into the recompiled improved ``vmdl_c``. Because every agent shares
the CS2 player skeleton, the PHYS block is a drop-in; the result is byte-for-byte Valve
ragdoll physics at the swappable improved path. The resource container is rebuilt cleanly
(block table re-laid-out with the substituted PHYS, offsets/sizes recomputed, header size
updated).

This tool no longer reads ``player-profile.phy``; the Source 1 payload is unused.
"""

from __future__ import annotations

import argparse
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
DEFAULT_VRF = Path(r"C:\Users\ayden\Documents\Github Projects\ValveResourceFormat\CLI\bin\Release\Source2Viewer-CLI.exe")
DEFAULT_VPK = Path(r"F:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\csgo\pak01_dir.vpk")

PLAYER_MODEL_RE = re.compile(r"^agents/models/(?:ctm|tm)_[^/]+/(?:ctm|tm)_[^/]+\.vmdl$", re.I)
OUTPUT_PREFIX = Path("models/filmmaker/improved_ragdolls")
ALIGN = 16


# --------------------------------------------------------------------------- VRF

def run_vrf_decompile(vrf: Path, vpk: Path, output: Path) -> None:
    subprocess.run([str(vrf), "-i", str(vpk), "-d", "-f", "agents/models/", "-e", "vmdl_c",
                    "-o", str(output)], check=True)


def run_vrf_extract_raw(vrf: Path, vpk: Path, output: Path) -> None:
    """Raw (non-decompiled) extraction of every agent vmdl_c -- keeps the compiled PHYS."""
    subprocess.run([str(vrf), "-i", str(vpk), "-f", "agents/models/", "-e", "vmdl_c",
                    "-o", str(output)], check=True)


# ------------------------------------------------------ Source 2 resource graft

def read_blocks(data: bytes):
    struct.unpack_from("<IHH", data, 0)  # fileSize, headerVersion, version
    block_offset, block_count = struct.unpack_from("<II", data, 8)
    table_pos = 8 + block_offset
    blocks = []
    for i in range(block_count):
        rel_field = table_pos + i * 12 + 4
        btype = data[table_pos + i * 12: table_pos + i * 12 + 4].decode("ascii", "replace")
        rel_off, size = struct.unpack_from("<II", data, rel_field)
        blocks.append({"type": btype, "rel_field": rel_field, "abs": rel_field + rel_off, "size": size})
    return blocks


def extract_phys(vmdlc: Path) -> bytes:
    data = vmdlc.read_bytes()
    for b in read_blocks(data):
        if b["type"] == "PHYS":
            return data[b["abs"]: b["abs"] + b["size"]]
    raise ValueError(f"{vmdlc.name}: no PHYS block")


def graft_phys(target: Path, donor_phys: bytes) -> None:
    """Replace target's PHYS block with donor_phys, rebuilding the resource container."""
    data = bytearray(target.read_bytes())
    blocks = read_blocks(data)
    if not any(b["type"] == "PHYS" for b in blocks):
        raise ValueError(f"{target.name}: no PHYS block to replace")
    order = sorted(blocks, key=lambda b: b["abs"])
    layout_start = order[0]["abs"]
    payload = {b["rel_field"]: (donor_phys if b["type"] == "PHYS"
                                else bytes(data[b["abs"]: b["abs"] + b["size"]])) for b in order}
    new = bytearray(data[:layout_start])
    for b in order:
        while len(new) % ALIGN:
            new.append(0)
        pl = payload[b["rel_field"]]
        struct.pack_into("<II", new, b["rel_field"], len(new) - b["rel_field"], len(pl))
        new += pl
    struct.pack_into("<I", new, 0, len(new))  # header fileSize
    target.write_bytes(new)


# ------------------------------------------------------------------- phases

def prepare(content_root: Path, vrf: Path, vpk: Path) -> list[str]:
    content_root.mkdir(parents=True, exist_ok=True)
    run_vrf_decompile(vrf, vpk, content_root)
    shared = content_root / "agents" / "models" / "shared"
    if shared.exists():
        shutil.rmtree(shared)

    models: list[str] = []
    for path in content_root.glob("agents/models/**/*.vmdl"):
        relative = path.relative_to(content_root).as_posix()
        if not PLAYER_MODEL_RE.match(relative):
            continue
        out_rel = OUTPUT_PREFIX / Path(relative)
        out_path = content_root / out_rel
        out_path.parent.mkdir(parents=True, exist_ok=True)
        if out_path.exists():
            out_path.unlink()
        shutil.move(str(path), str(out_path))  # relocate verbatim; physics grafted post-compile
        models.append(out_rel.as_posix())
    if not models:
        raise RuntimeError("VRF produced no CT/T player ModelDoc files")

    report = content_root / "improved-ragdolls-report.txt"
    report.write_text(f"model_count={len(models)}\n" + "\n".join(models) + "\n", encoding="utf-8")
    return models


def graft_phase(game_dir: Path, vrf: Path, vpk: Path) -> tuple[int, int]:
    improved_root = game_dir / OUTPUT_PREFIX
    compiled = sorted(improved_root.rglob("*.vmdl_c"))
    if not compiled:
        raise RuntimeError(f"No compiled improved models under {improved_root}")
    with tempfile.TemporaryDirectory() as tmp:
        stock_root = Path(tmp)
        run_vrf_extract_raw(vrf, vpk, stock_root)
        ok = fail = 0
        for imp in compiled:
            rel = imp.relative_to(improved_root)          # agents/models/<agent>/<variant>.vmdl_c
            stock = stock_root / rel
            if not stock.is_file():
                print(f"  MISSING stock PHYS for {rel}")
                fail += 1
                continue
            try:
                graft_phys(imp, extract_phys(stock))
                ok += 1
            except ValueError as e:
                print(f"  GRAFT FAIL {rel}: {e}")
                fail += 1
    return ok, fail


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--content-root", type=Path)
    parser.add_argument("--vrf-cli", type=Path, default=DEFAULT_VRF)
    parser.add_argument("--cs2-vpk", type=Path, default=DEFAULT_VPK)
    parser.add_argument("--graft-phys", action="store_true",
                        help="Post-compile: graft Valve PHYS into compiled improved models under --game-dir.")
    parser.add_argument("--game-dir", type=Path, help="game/source_mvm_fx dir holding compiled improved vmdl_c.")
    args = parser.parse_args()

    if args.graft_phys:
        if not args.game_dir:
            parser.error("--graft-phys requires --game-dir")
        ok, fail = graft_phase(args.game_dir, args.vrf_cli, args.cs2_vpk)
        print(f"Grafted Valve ragdoll physics into {ok} improved models ({fail} failed).")
        return 1 if fail else 0

    if not args.content_root:
        parser.error("prepare phase requires --content-root")
    models = prepare(args.content_root, args.vrf_cli, args.cs2_vpk)
    print(f"Prepared {len(models)} CS2 player models; physics grafted after compile.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
