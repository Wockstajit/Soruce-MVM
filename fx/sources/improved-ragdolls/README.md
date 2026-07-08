# Improved Ragdolls source profile

> **Note:** `player-profile.phy` (jahpeg's Source 1 "Improved Ragdolls" payload) is **no
> longer used** by the build. Porting Source 1 PHY metadata into ModelDoc produced broken,
> floppy ragdolls: CS2's compiler drops authored physics on recompile, and no source node
> can recreate a joint's parent reference frame. The pipeline now grafts Valve's own
> compiled `PHYS` block into the recompiled models instead — see
> [`fx/README.md` § Improved Ragdolls](../../README.md) and
> [`fx/tools/convert-improved-ragdolls.py`](../../tools/convert-improved-ragdolls.py).
> The `.phy` is retained only for reference.

`player-profile.phy` is the canonical player physics payload from jahpeg's Source 1
"Improved Ragdolls" addon (21 bodies, 20 constraints, Source 1 total mass 800). It is kept
here for historical reference only.

Canonical SHA-256:
`3efdd600926fbde8e6f6c5ea4b6460d1f6dee3732d1bd109a55ccdd5da84b27c`
