# Improved Ragdolls source profile

`player-profile.phy` is the canonical player physics payload from jahpeg's Source 1
"Improved Ragdolls" addon. The original package contained 239 byte-identical copies of
this payload under legacy CT/T player-model paths; only one is retained here.

The CS2 conversion uses the trailing VPHY KeyValues metadata (21 bodies, 20 constraints,
and Source 1 total mass 800). Body-mass ratios are preserved but normalized to Valve
CS2's stock total of 272. Current collision geometry and skeletons come from Valve's CS2
models through VRF. Hostage profiles, custom body-impact sounds, and duplicate sidecars
are intentionally excluded.

Canonical SHA-256:
`3efdd600926fbde8e6f6c5ea4b6460d1f6dee3732d1bd109a55ccdd5da84b27c`
