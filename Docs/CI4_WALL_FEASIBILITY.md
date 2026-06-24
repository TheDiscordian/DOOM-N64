# CI4 + Tile-Wrap Wall Renderer — Go/No-Go Feasibility Brief

**Status**: READ-ONLY design + prediction pass. No code changed. This brief is the go/no-go input.
**Branch**: `perf/rdp-renderer` (HEAD `f223486`)
**Date**: 2026-06-13
**Inputs**: architecture+perf projection (modeled from `rdp_view.c` cost comments + measured `seg_rast=5211`), fidelity assessment (real WAD parse + CIELab ΔE on E1M1's actual most-used walls), `Docs/RDP_PRIOR_ART.md` (RSP floor).

---

## TL;DR — GO, conditional

CI4 + hardware tile-wrap is the **only design on the table whose primitive arithmetic can physically fit under the RSP triangle-setup floor**. It credibly takes the dominant wall class from 16 tris → 2, beats software (19793 µs) with high confidence, and the fidelity cost is small, concentrated, and predictable — not broad. The 16670 (60fps) line is *reachable for the first time* but not guaranteed on p95. The one real blocker that remains is engineering, not feasibility: the TLUT-coexistence ordering, which has a verified resolution but is fragile.

---

## 1. PREDICTED WIN

**Does CI4/one-quad get walls from 16 tris → ~2, and beat 19793 / approach 16670?**

**Tris: YES for the dominant class.** Today a full-height wall on a 128-wide CI8 texture band-splits into 8 TMEM bands × 2 = **16 tris per record**, each band its own `LOAD_TILE`. With CI4 + `tp.t.mask`/`tp.s.mask` hardware wrap:
- **64-wide ≤64-tall texture → exactly 2 tris** (one `LOAD_TILE` of the whole image, one tri-pair, hardware wraps S and T). This is DOOM 64's shipped figure and it lands on **81/125 (65%)** of DOOM1 textures.
- **128-wide → 4 tris** (S-split, keeps full horizontal detail) or 2 tris (downsample to 64).
- **Tall wall, short texture → still 2 tris** — this is exactly the case hardware T-wrap was meant to kill (a 64-tall texture on a 200-tall wall is one quad).

**The dominant win is the LOAD count, not the tri count.** Tile-loads collapse from ~150/frame → ~10 typical. Each eliminated `LOAD_TILE` removes an `AUTOSYNC_TILE` + `LOAD` + 10–14 cyc dispatch tax. The `rdp_view.c:1471` comment attributes ~7.5 ms of the ~9.6 ms `DL_BUILD` to per-band uploads; the measured `seg_rast=5211 µs` is RSP triangle-setup throughput for the inflated tri count.

**Perf projection (modeled, with error bars):**
- **Beats software 19793: YES, confidence HIGH.** Removing ~8× of wall tri-setup and ~14× of tile-loads collapses the cost currently pushing the frame past 22020. `seg_rast 5211 → projected ~700–1100 µs` (residual is plane spans + screen-space trapezoid splits, *not* texel bands), clearing the design's own `seg_rast<1000` gate for typical frames. **Projected avg ≈ 15000–17500 µs.**
- **Clears 16670 (60fps): PLAUSIBLE, confidence MEDIUM.** Projected avg lands *on or just over* the line. p95 — driven by tail frames (drawsegs=47, ~25 distinct textures → >16 CI4 palette slots → mid-frame TLUT re-upload) — is the real risk of staying over. The current path **cannot** reach 16670 by construction (16 tris/wall floors it); CI4 is the first design that physically can. Whether p95 actually clears it is what the bench must settle.

**Honest caveat**: these numbers are modeled from the `rdp_view.c` comments' own cost attributions (the ~7.5 ms upload figure, `seg_rast=5211`), not measured. The bench is the only thing that confirms 16670.

---

## 2. FIDELITY COST

**How much does 256→16 CI4 quantization cost on real DOOM walls?** Measured by parsing the actual WAD, assembling each composite, frequency-weighted median-cut to ≤16 colors snapped to PLAYPAL in CIELab, then CIE76 ΔE original-vs-CI4 over every pixel. Verdict: **small, concentrated, predictable — does NOT block on fidelity grounds.**

The three most-used E1M1 walls carry the verdict:

| Texture | E1M1 uses | Distinct colors | mean ΔE | p95 ΔE | Verdict |
|---|---|---|---|---|---|
| **STARTAN3** | 109 (most-used) | 66 | 2.16 | 7.48 | Perceptible but acceptable — gradient banding on metallic highlights |
| **BROWNGRN** | 104 | 25 | 0.07 | 0.00 | Essentially lossless |
| **BROWN1** | 92 | 32 | 0.65 | 4.69 | Imperceptible in practice |
| **COMPTILE** | 13 | 15 | 0.00 | 0.00 | Lossless (already ≤16 colors) |
| **COMPUTE2** | 8 | 63 | 0.88 | 7.68 | Good — blue/green/amber bank stays recognizable |
| **SLADWALL** | 12 | 20 | 0.05 | 0.00 | Essentially lossless |
| **TEKWALL1** | 16 | 108 | **2.37** | **13.92** | **Worst case** — sparse green circuit accents desaturate |

**The pattern**: flat/dark/low-saturation walls — most of DOOM's wall *area* — are free. Cost lands only on (a) smooth tonal gradients → banding (STARTAN3's metallic panel tops) and (b) rare-but-saturated accent colors → desaturation (TEKWALL1's green wire/indicator pop, which median-cut sacrifices because they're low-frequency).

**The single genuine loser is TEKWALL1** (only 16 uses, low traffic) and it is **recoverable**: a hand-tuned per-texture palette that force-reserves 1–2 green slots restores most of the pop. STARTAN3 is the only *high-traffic* wall with a visible hit, and it still clearly reads as the same texture at play distance.

**Samples to inspect** (4× nearest upscaled, original | CI4 side-by-side):
- `/tmp/ci4-preview/_contact_sheet.png` — all seven at once
- `/tmp/ci4-preview/STARTAN3.png` — the high-traffic visible-hit case
- `/tmp/ci4-preview/TEKWALL1.png` — the worst case (green desaturation)
- `/tmp/ci4-preview/BROWN1.png` — representative near-lossless high-traffic wall

---

## 3. EFFORT / RISK

**Scope of rework** (all in `rdp_view.c` + the convert path, the geometry emit is untouched):
1. `DL_RowMajorBlock` (:225–303) — CI4 pack (2 indices/byte) + a per-texture 256→16 quantizer LUT built once at first-touch, inverse map cached. Allocation halves.
2. `DL_DrawRecord` (:1404–1548) — **delete the entire band-walk for-loop** (:1428–1548), replace with `set_tile(mask_s, mask_t, palette)` + one `load_tile` + one tri-pair. The adaptive-S-window (:1315–1356) and upload-dedup (:1483–1493) become dead code.
3. `DL_Flush` (:1782+) — per-texture upload loads the full CI4 image once.
4. Format constant CI8→CI4, pitch `lw`→`lw/2`.
5. `DL_EmitRunPiece` (:898–1084) — **UNCHANGED.** Screen-space geometry (clip steps, glancing-wall S deviation) is orthogonal to the texel band split.

This is a **net deletion** of the most complex code in the renderer (the band walk), not just addition. That lowers risk.

**Master-TLUT coexistence — the load-bearing problem, with a verified resolution:**

Verified fact (`rdpq_tex.h:210–212`): the TMEM upper 2 KB holds 256 RGBA16 entries = one palette for CI8, OR up to 16 palettes for CI4. The present blit's master TLUT writes all 256 entries — sprites (CI8), HUD/overlay blit, and damage/pickup flashes all depend on it. A per-wall CI4 16-color sub-palette and the 256-entry master **genuinely cannot coexist** in the same region. DOOM 64 had no conflict (all-CI4); we have a mixed world.

**Resolution (strategy A + B), confirmed viable:**
- Walls and present blit are **sequential in the same rspq stream**. Wall pass draws first with packed CI4 sub-palettes (up to 16 wall textures' 16-entry palettes in the 256-region); each wall record carries `tp.palette = slot`. Then before the present blit (which already re-uploads the master TLUT), upload the master 256-TLUT over the same region.
- Cost: **+1 mandatory TLUT upload/frame** (~0.3–0.5 µs RDP + 512B writeback) plus forcing `n64_palette_dirty` every frame. Negligible vs the 16670 budget.
- The bench scenario routes ~10 textures, combat ~12 — **under 16, no mid-frame swap**. Tail frames (ds=47, ~25 textures) exceed 16 → sort the bucket walk by sub-palette and re-upload per overflow group (~0.5 µs each).

**Ordering is fragile**: `walls(CI4 palettes) → re-upload master → flats/sprites/present(master)`. Get it wrong and sprites sample wall sub-palettes (garbage) or walls sample the master 256 (16-color subset). The forced per-frame master re-upload must be verified to not break the damage/pickup flash path.

**Residual risks:**
- **T-wrap correctness**: switching `tp.t.clamp=true` → `tp.t.mask=log2(height)` changes vertical sampling to hardware mod-pow2. Non-pow2-height textures wrap at the wrong period — must verify all DOOM textureheights are pow2 or clamp+band the exceptions.
- **Wide/tall textures don't one-quad as CI4** (128-wide = 4096B, 128-tall composites > 2048B TMEM) → fall back to 2–4 bands. The ~2-tri ideal fully lands only on the 65% that are 64×≤64. A level heavy in wide/tall textures regresses toward today's floor.
- **Quantizer quality/cost**: poor quant → banding; first-touch cost on the cold frame; inverse-map LUT must be cached or every pack re-quantizes.
- **Tail >16 textures**: adds p95 cost exactly where the 16670 gate is already at risk.

---

## 4. RECOMMENDATION

**GO — the predicted perf win is worth the fidelity cost and the rework.**

Three independent reasons the balance favors proceeding:

1. **It is the only design that can physically fit under the RSP floor.** PRIOR_ART confirms no API/tiny3d tuning lowers per-tri setup cost — only *volume* does, and this is the volume cut (16→2 tris, 150→10 loads). The current path is regression-by-construction; this is the first change whose arithmetic can clear 16670 at all.

2. **The fidelity cost is small, concentrated, and recoverable.** The two heaviest-traffic walls (BROWNGRN, BROWN1) are effectively lossless; the only high-traffic visible hit (STARTAN3, ΔE 2.16) still reads correctly at play distance; the worst case (TEKWALL1) is a low-traffic wall fixable with a hand-tuned palette. This is not a broad quality collapse — it's a handful of textures with a known, bounded, mitigable cost.

3. **The rework is a net deletion** of the most complex code (the band walk), with a *verified* resolution to the one hard blocker (TLUT coexistence via sequential pass + 1 extra upload/frame).

**Honest qualifications on the GO:**
- The win over software (19793) is HIGH confidence. **Clearing 16670 on p95 is MEDIUM and the bench must settle it** — the projection no longer says "impossible," but it does not say "guaranteed" either.
- Ship the fidelity safety valve from the design: per-texture histogram keeps >16-color walls on CI8+S-split (no quad collapse but no quantization) and applies CI4 only to ≤16-color-safe textures. STARTAN3/TEKWALL1 are the textures to A/B before final commit.
- The decision to **downsample the 8 256-wide and S-split the 36 128-wide** (rather than downsample 128-wide) is the right fidelity/tri tradeoff — caps the worst wall at 4 tris while preserving horizontal detail on the most-detailed art.

**Bottom line for the next stage**: proceed to implementation behind the histogram safety valve, treat the TLUT ordering as the highest-risk item to verify first, and let the bench p95 (tail frames) be the gate on whether 16670 is actually claimed — beating software is not in doubt.
