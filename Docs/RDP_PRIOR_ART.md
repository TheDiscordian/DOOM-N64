# RDP Renderer — Prior-Art Verdict 🦴📚

Synthesis of five prior-art investigations against our measured finding: a fully-RDP
DOOM renderer (libdragon, ares paraLLEl-RDP) where the CPU keeps BSP/clip and emits
screen-space primitives is **+11% slower** for walls (22020µs vs 19793µs software) and
**+14% slower** for planes (22518µs), with RDP fill nearly free (`rdp_busy ~97µs`),
perfect CPU/RDP overlap (`spin_hits=0`), and the bottleneck living in **CPU/RSP
command-generation + RSP triangle-setup throughput**.

---

## Headline Verdict (read this first)

Our command-generation floor is a **known, documented, by-design N64 phenomenon** — not a
bug, not an artifact of bypassing tiny3d. Every shipped DOOM-class renderer on real N64
hardware either kept the **software span/column renderer** (64Doom) or, in the one
hardware-accelerated case (DOOM 64), fed the RDP per-primitive **but kept the primitive
count minimal** (one quad per wall via CI4 + hardware tile-wrap, never band-splitting) and
routed setup **through the RSP T&L engine**. Bypassing tiny3d was **not the architectural
mistake the suspicion suggests**: our `rdpq_triangle` is *already* on the RSP setup path
(`RDPQ_TRIANGLE_REFERENCE 0`), so the heavy coefficient math we'd "offload" to tiny3d is
**already off the CPU** — tiny3d would only cut rspq-FIFO command *count*, not the
per-triangle setup compute or the TMEM-forced band count that actually floors us. **The
deficit is dominated by triangle/command VOLUME, and that volume is manufactured by two of
our own choices** (CI8/128-wide textures forcing 8-band splits, and per-triangle un-batched
emission) — both fixable, but neither is what we currently blame.

---

## (1) Is the command-gen floor a KNOWN, expected N64 phenomenon? YES.

**Confidence: HIGH (primary-sourced, official + decompiled).**

The N64 *Introductory Manual* §2.4.2 "Tuning Performance" describes our exact profile
verbatim:

- A textured triangle command "must set up the command with close to **180 bytes**".
- The RSP DMEM command FIFO is **1 KB**, which "becomes full with the drawing command of
  **six triangles**".
- "If the triangles are big, this command process is slow and the **RSP has to wait for the
  output**." — i.e. for **small** triangles (DOOM walls/planes) the RDP drains instantly and
  the **RSP/command throughput is the limiter**, which is precisely `rdp_busy~97µs` +
  `spin_hits=0`.
- "This method is effective only when you have ... a problem with the **RSP processing
  time, not the drawing time**." — the manual *names* our observation that cutting CPU emit
  doesn't help wall-clock: when RDP draw time isn't the constraint, only RSP
  command-processing time matters.
- Textured triangles are the expensive ones: dropping the texture makes the RDP command "64
  bytes shorter". DOOM walls/planes are **all textured**, so they pay the full cost.

**Quantified per-triangle / per-command cost** (HackerN64 F3DEX3 `Performance.md`, hand-counted
incl. pipeline stalls + dual-issue, "textured, shade, and Z"):

| Item | F3DEX2 | F3DEX3 / NOC |
|---|---|---|
| Draw 1 triangle (tex+shade+Z) | **173 cyc** (1st) / 172 | 152/151 (FEX3), 150/149 (NOC) |
| Culled / offscreen triangle | ~20–34 cyc | ~20–34 cyc |
| Per-command dispatch tax | **12 cyc** | 10 cyc |
| Small RDP command | 14 cyc | 4 cyc |
| Vertex pair, no lighting | 54 cyc | 70 cyc (FEX3), 54 (NOC) |

So one drawn textured triangle ≈ **~150–173 RSP cycles**, and **every** rspq command carries a
fixed **~10–14 cycle dispatch tax before doing any work** — the per-command floor that
band-splitting multiplies.

**Realistic primitive budget at 60fps.** RSP runs at `RCP_FREQUENCY = 62 500 000` Hz NTSC
(verified `n64sys.h:48`). A 60fps frame = ~1.04M RSP cycles. At ~150–173 cyc/drawn triangle
the **hard ceiling is ~6 000–7 000 fully-featured (tex+shade+Z) triangles/frame IF the RSP did
nothing else** — no vertex transform, no audio, no dispatch slack. Realistically far fewer.
The only concrete *shipped* 60fps figure surfaced is an N64-manual benchmark of **410
triangles @ 60fps NTSC / 507 @ 50fps PAL**, and that was a small-triangle-heavy effect test —
a data point, not a general ceiling. **Implication:** our 8 bands × 2 tris = **16 tris/wall**
plus hundreds of segs/planes pushes thousands of fully-featured triangles/frame, hitting the
RSP-setup wall **independent of RDP fill** — exactly as measured.

---

## (2) What did people who actually shipped/built DOOM-class N64 renderers do?

**Confidence: HIGH on the source facts (decompiled binary + DeepWiki + DoomWiki).**

There are exactly two relevant lineages, and a third (ours) that is genuinely novel:

**A. 64Doom (jnmartin84) — SOFTWARE.** The canonical libdragon source-port of classic
(linuxdoom) DOOM — *our exact lineage* — **kept DOOM's original software renderer**:
`R_DrawColumn` (vertical strips for walls/sprites) and `R_DrawSpan` (horizontal runs for
floors/ceilings) into a fixed 320×200 palette framebuffer, with the RDP used **only for the
final present**. DoomWiki: it "renders directly to the N64 frame buffer, as opposed to Doom
64's polygon-based hardware rendering approach." This is **identical to our software
baseline**.

**B. DOOM 64 (Midway/Williams, 1997, id-supervised) — HARDWARE, BUT DISCIPLINED.** The only
shipped id-sanctioned hardware-rendered N64 DOOM. From the Erick194 `DOOM64-RE` decompilation
of the actual shipped binary:

- It **kept BSP/visibility on the CPU** (`r_phase1.c`: `R_RenderBSPNode`/`R_AddLine` emit
  **zero** GBI), then emitted primitives in `r_phase3.c` — **same high-level shape as ours**.
- It **DID feed the RDP per-primitive** — walls via `gSPVertex(4)+gSP1Quadrangle`, leaves via
  `gSPVertex+gSP2Triangles` fan — but **through the RSP geometry pipeline, not around it**.
  `r_main.c` sets up `guFrustum` (near 8, far 3808) + `gSPMatrix` MODELVIEW and feeds
  **WORLD-space vertices**, letting the RSP microcode do transform/clip/project. **It did NOT
  pre-project on the CPU.** Our design doc §9 (verified, line ~936) does the **opposite**: it
  bypasses T&L because "DOOM already produces clipped screen-space geometry."
- **It never band-split.** Walls are drawn as **ONE quad (2 tris) per segment with ONE
  texture load**, using **CI4** (16-colour, half the bytes/texel of our CI8) at **64×64** and
  **hardware S/T tile WRAP/MIRROR** so a single `LoadBlock`+quad spans an arbitrarily tall
  wall. Texel math: CI4 64×64 = 4096 texels × 0.5 B = **2 KB** (fits below the persistent
  TLUT); our CI8 128-wide = only 32 rows in 4 KB, **forcing the 8-band/16-tri split**.
- Tile loads were **conditional/cached**: `R_RenderWall` guards
  `if ((texture != globallump) || (globalcm != (cms|cmt)))` before reloading TMEM,
  amortizing loads across consecutive same-texture walls.
- Carmack design constraint (Wikipedia making-of): the team had triangles available but was
  "adamant" about **not turning DOOM into a generic triangle engine** — they kept counts
  Doom-like (one quad/wall, fan/leaf). **Our band-splitting violates exactly this discipline.**

**Implication for us.** Nobody shipped a *classic*-DOOM full-RDP wall/plane rasterizer; the
software path won for classic DOOM on every shipped libdragon port, and the one hardware DOOM
(DOOM 64) avoided our floor by (i) **minimal primitive count** (no tessellation, no
band-splits) and (ii) **CI4 + 64-wide + hardware tile-wrap**. We reproduced the *shape* of
DOOM 64's architecture but inverted both of its cost-avoiding decisions.

---

## (3) THE KEY DECISION: was bypassing tiny3d the architectural mistake?

**Verdict: NO — the bypass was sound on the T&L axis, and tiny3d would NOT have averted the
floor. But the survey threw out one real tiny3d win (command-count batching) along with the
genuinely-useless T&L. Confidence: HIGH on the mechanism, MODERATE on the magnitude.**

The decisive facts, verified against our own vendored source:

1. **Our "direct rdpq_triangle" is ALREADY the RSP setup path, not the CPU path.**
   `rdpq_constants.h:42` → `RDPQ_TRIANGLE_REFERENCE 0`, so `rdpq_tri.c` dispatches to
   `rdpq_triangle_rsp` (the `#if RDPQ_TRIANGLE_REFERENCE` CPU branch is **disabled**). The CPU
   emits only small stubs (3× `RDPQ_CMD_TRIANGLE_DATA` vertex words + 1 trigger); the **RSP
   computes all edge/shade/tex/z gradients**. → **tiny3d's single biggest theoretical win —
   "move coefficient math off the CPU" — is a win we already have.**

2. **tiny3d uses the SAME triangle-setup ucode we do.** `rsp_tiny3d.rspl` `#include`s
   `rspq_triangle.inc` (a fork of libdragon's `RDPQ_Triangle`). Every triangle, raw or
   tiny3d-batched, runs the **identical ~150-cycle gradient computation**. → If our floor is
   RSP triangle-setup *throughput*, **tiny3d cannot lower it** — same instructions, same count.

3. **The TMEM band-split floor is orthogonal to tiny3d.** tiny3d operates above the RDP
   tile/TMEM layer ("Tiny3D does not know about textures... the RDPQ API & sprite_load must be
   used"). It changes **neither** the band count **nor** the tile-load commands. The 8-band/
   16-tri volume is set by **TMEM + our CI8/128-wide choice**, not by the draw API.

**Where tiny3d (or a tiny3d-*style* rspq path) WOULD genuinely beat our current direct path —
in the FIFO, not in setup:**

- `rdpq_triangle_rsp` emits **4 rspq commands per triangle** (3 vertex-data + 1 trigger) → ~64
  rspq commands per 16-tri wall, each paying the ~10–14 cyc dispatch tax. A tiny3d
  `T3DCmd_VertLoad` + `TriDraw_Seq`/`quad_draw_unindexed` flow loads the **4 wall corners ONCE**
  and drives N per-triangle setups in a **tight RSP loop with zero per-triangle rspq dispatch**,
  emitting **ONE 8-byte command** for the batch. That directly attacks the **"rspq-FIFO wait"
  the saving reappears in** when we cut CPU emit — the exact relocation we measured.
- **But the magnitude almost certainly does not transfer.** tiny3d's headline numbers —
  command count 2326→127 (**94.5%**), bandwidth −45.5%, **−1650µs of an 8807µs RSP budget** —
  come from **triangle-strip index compression on a connected mesh with reusable vertices**.
  DOOM walls are **disjoint screen-space quads with no shared vertices across walls**, so the
  strip-compression half of the win is **unavailable**; only the per-quad-vs-per-triangle
  batching + vertex-load-once amortization transfers. And F3DEX3's own warning bites here: once
  you are RSP-bound, memory-traffic tricks can **lose** ("snakes save ~70µs RDP but cost ~400µs
  RSP") — mirroring our "saving reappears as FIFO wait."

**Net:** the bypass-rationale ("T&L offload re-does solved work") was **correct for T&L**.
Where it erred was **conflating** tiny3d's dead-weight T&L with its still-relevant
**command-batching + vertex-caching**, which are independent of T&L and *do* target the FIFO
floor. The honest read of the original suspicion — "the bypass is why we're command-gen-bound"
— is **misplaced**: **any** direct RDP path is command-gen-bound at this triangle volume,
because setup is RSP ucode either way. The leverage is the **volume**, not the API.

---

## (4) Concrete recommendation — is there a credible path to beat software / approach 60fps?

**Verdict: There is a credible path, but it is NOT "adopt tiny3d." It is "attack the triangle/
command VOLUME at its two sources." Confidence: MEDIUM on the remedy clearing software for a
busy scene; the remedy is well-grounded extrapolation, not a proven recipe — no prior art
demonstrates a full-RDP classic-DOOM beating software at 60fps.**

In priority order, each lever tied to the evidence:

1. **Kill the band-split at its source (the prime target).** Switch wall textures to **CI4
   and/or ≤64-wide** so a tall wall draws as **one quad (2 tris) via hardware S/T tile-wrap**
   instead of 8 bands/16 tris — exactly DOOM 64's shipped approach. This cuts the now-binding
   per-triangle count **~8×** *and* eliminates ~8 per-band tile-load commands/wall (each its own
   autosync PIPE/TILE/TMEM + dispatch tax). This single change attacks the multiplier that
   inflates every other cost. **(Source: DOOM64-RE r_phase3.c/r_main.c texel math; HIGH
   confidence it cuts volume, MEDIUM that volume reduction nets the wall-clock win.)**

2. **Batch by texture (`rspq_block` zero-CPU replay + material sort).** libdragon docs:
   rdpq commands recorded into `rspq_block`s replay "with zero CPU time," have special
   bandwidth optimizations, and are "advised ... for sequences of 3 or more RDPQ calls." For
   DOOM's per-frame-static geometry (view border, sky, split dividers, per-sector band
   templates) this removes CPU emit **and** cuts the FIFO traffic the saving reappears in.
   Amortize tile loads per-texture (à la DOOM 64's conditional `globallump` guard), not per-seg.
   **This is the one axis where tiny3d's strip-batching wisdom genuinely contributes — and it
   applies to raw rdpq directly, no tiny3d dependency.** (Already flagged in our own NOTES §1.)

3. **Only then, if still FIFO-bound: a narrow tiny3d-style batched-RSP experiment.** A
   project-owned fork (vendored tree is read-only) emitting `VertLoad`-once + indexed
   `TriDraw_Seq` to collapse ~64 rspq commands/wall → ~1. Gate it: **one wall via
   `T3DCmd_VertLoad` + `t3d_quad_draw_unindexed`, `RDPQ_TRIANGLE_PROFILE` on, before
   committing.** Do **not** expect modelOpt.md's 94.5% to transfer to disjoint quads.

4. **Revise the NOTES premise.** `RDP_RENDERER_NOTES.md` asserts "The N64 is bandwidth-bound,
   not setup-bound" / "cost is per-pixel fill, not per-primitive setup." That was **correct for
   the software path** (CPU byte-fill into uncached RDRAM) but **inverts on the RDP path** —
   `rdp_busy~97µs` proves fill is no longer the cost. The architecture survey optimized for the
   *old* bottleneck and walked into the *new* one. This premise should be explicitly marked
   path-dependent.

**The honest counter-case (must be stated).** No prior art demonstrates a full-RDP classic
DOOM beating software at 60fps; every shipped libdragon DOOM kept software. The strongest
negative evidence is tiny3d's own `22_bigtex` example reaching 60fps "mostly" **only with
skybox-only visible** — direct large-textured RDP fill is RDRAM-bandwidth-bound even before the
setup floor. **If, after levers 1–2, a busy DOOM scene still doesn't clear software, the
software span/column renderer was the right call for classic DOOM on N64** — and that
conclusion would be consistent with the entire shipped record. The credible path is real but
**unproven**; treat levers 1–2 as the experiment that decides it, not as a guaranteed win.

---

## Best sources

- **DOOM 64 shipped behaviour:** `Erick194/DOOM64-RE` — `r_phase1.c` (CPU BSP, zero GBI),
  `r_phase3.c` (quad/wall, fan/leaf, conditional tile load), `r_main.c` (guFrustum + gSPMatrix
  world-space T&L). The actual decompiled 1997 binary.
- **The floor, named officially:** N64 *Introductory Manual* §2.4.2 "Tuning Performance"
  (180 B/tri, 1 KB DMEM = 6 tris, "RSP processing time not drawing time").
- **Quantified RSP cycle costs:** HackerN64 **F3DEX3 `Performance.md`** (150–173 cyc/drawn tri,
  10–14 cyc dispatch tax) — single best quantified source.
- **64Doom = software:** DeepWiki `jnmartin84/64doom/3-rendering-system`; DoomWiki `64Doom`.
- **Mechanism in OUR tree (decisive):** `libdragon/include/rdpq_constants.h:42`
  (`RDPQ_TRIANGLE_REFERENCE 0`), `libdragon/src/rdpq/rdpq_tri.c` (RSP dispatch),
  `tiny3d/src/t3d/rsp/rsp_tiny3d.rspl` + `rspq_triangle.inc` (shared setup ucode),
  `n64sys.h:48` (`RCP_FREQUENCY 62500000`).
- **Batching wisdom (transfers partially):** `tiny3d/docs/modelOpt.md` (94.5% command cut,
  −1650µs RSP — strip/connected-mesh numbers).
- **`rspq_block` zero-CPU replay:** libdragon `group__rdpq`/`rspq_8h` docs; libdragon issue #489.
- **Our own decisions under review:** `Docs/RDP_RENDERER_DESIGN.md` §9 (T&L bypass),
  `Docs/RDP_RENDERER_NOTES.md` §4 (bypass rationale) + §5 (TMEM) + the "bandwidth-bound not
  setup-bound" premise to revise.
