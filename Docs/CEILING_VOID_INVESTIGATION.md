# Ceiling void / flicker investigation (no-readback mesh leaves)

Living log so we STOP re-running ruled-out experiments. Append results; never repeat a
dead end below without a reason.

## Symptom (Ryan, repeated)
- "Usually works", but in **busy / complex-geometry scenes** floors and ceilings **flicker
  out**: the near ceiling that should draw is **missing**, and either **black** or the
  **surface behind it shows through** (e.g. a far ceiling / outdoor area).
- Reproduces statically on the off-128-grid frames **3200** (courtyard: upper area black)
  and **3712** (pillar room: near grey ceiling missing / dark, far green outdoor shows through).
  Capture with `BENCH_MARK_VOID=1`.
- This is the **mesh floor/ceiling path** only. Current clean HEAD / working baseline remains
  **mesh walls + poly planes** (`BENCH=1 BENCH_FORCE_RDP=1 BENCH_FORCE_MESH=1`).

## Current branch state (retry)
- Clean baseline branch: `perf/rdp-renderer` at `383cd05` = mesh walls + poly planes.
- Retry branch: `perf/rdp-mesh-planes-retry` = parked WIP reapplied for investigation.
- Important: keep experiments on retry/WIP branches only. Do not dirty `perf/rdp-renderer`.

## RULED OUT (do not re-test without cause)
1. **Cells vs band split** — same class of output; neither resolves the mismatch vs poly planes.
2. **Texcoord-span overflow** — cells bound spans; no evidence this explains the 3712 loss.
3. **Dispatch drop / staging cap** — `EMITDIAG` shows visible ceiling bands emit descriptors;
   `dcap_break=0` on 3200/3712.
4. **Degenerate-area cull** — `LEAFRBSUM f=3712` shows only 1/50 on-screen ceiling fan tris
   would hit `|cross|<32`.
5. **Z-test only** — disabling leaf z-test did not bring the missing ceiling back in the old
   run. Note: this does **not** rule out malformed Z-bearing triangle setup; it only rules out
   ordinary z compare/write failure.
6. **Top-edge straddling alone** — clipping ceilings from `centery+256` to exact `centery`
   removed the off-screen straddler and made the grey triangles clean/on-screen, but Ryan still
   saw the same problem. A 0.25px inside-top experiment likewise did not materially change the
   same-geometry capture.
7. **Overlay-B async staging reuse as sole cause** — per-leaf / per-triangle `Send_End` proof
   builds changed pixels slightly but did not close the gap vs mesh+poly planes.
8. **Removing Z attribute** — leaf `TRI_SHADE_TEX` (no Z) changed pixels slightly but did not fix
   the 3712 mismatch.
9. **Leaf Z formula swap** — using depth-style leaf Z instead of `0x7FFF - 2*invw` changed pixels
   slightly but did not fix the mismatch.
10. **RSP leaf transform as sole cause** — a diagnostic that kept staged cells but reprojected
    them on the CPU before `rdpq_triangle()` still matched the bad mesh-leaf output much more
    than the working mesh+poly baseline.

## Confirmed facts from retry (2026-06-30)

### 1. The working visual baseline is mesh walls + poly planes
Use `/tmp/doom-mesh-polyplanes.z64` / `/tmp/doom-mesh-polyplanes-caps` for the same-geometry
capture baseline from this session. The relevant build flags were:

```bash
BENCH_FORCE_MESH=1 BENCH_MARKS=1 BENCH_MARK_VOID=1
```

This is the user-facing good configuration. It suppresses none of the poly plane path.

### 2. Mesh leaves are not just missing one tiny grey sliver
A same-geometry diff of frame 3712 against the working mesh+poly baseline found broad darker /
underdrawn regions in the mesh-leaf output. Biggest component (game coords approximate):

```text
box game=(14.7,6.6)..(132.1,91.9), base≈(56,57,49), mesh≈(17,17,15), 14240 px
```

So the old narrative "cell 84 grey ceiling only" is incomplete. Cell 84 is a useful repro probe,
but the actual mismatch vs poly planes is broader plane coverage/lighting/order.

### 3. The mesh geometry/cell pipeline emits descriptors, but does not match poly planes
At frame 3712 on the cells path:

```text
EMITDIAG f=3712 floor[vis=40 flat=2 desc=43] ceil[vis=24 flat=3 desc=24] dcap_break=0 DCAP=2048
LEAFRBSUM f=3712 ceilbands=24 above=0 onscreen=24 onscr_tris=50 onscr_culled=1
```

The visible cell/band descriptors exist; this is not a simple cap/drop.

### 4. Same staged cells through CPU `rdpq_triangle()` still look like the bad mesh-leaf output
Diagnostic mode added in the retry branch:

```bash
BENCH_FORCE_MESH_LEAF_CPU_EMIT=1
```

It keeps the RSP leaf/cell staging, but emits final fans with `rdpq_triangle()` on the CPU. Two
variants were tried:
- read `leaf_out_buf` (RSP transform) then CPU emit;
- recompute the projection from `leaf_in_buf` on the CPU, then CPU emit.

Both stayed much closer to the bad mesh-leaf output than to mesh+poly planes. This strongly shifts
suspicion away from overlay-B alone and toward the **leaf/cell/band representation vs the poly
visplane representation** (coverage, order, lighting, or plane clipping).

### 5. CPU leaf-mesh path is not the same reference as poly planes
The old CPU leaf mesh build (`BENCH_FORCE_MESH_FLOORS=1` without leaf emit) differs substantially
from mesh+poly planes too; it is not a golden reference. The actual golden visual baseline for this
project is mesh walls + **poly planes**, not CPU leaf mesh.

## Current best interpretation
The limitation is not "meshing planes is impossible", but the current leaf/cell mesh representation
is not equivalent to the poly-plane path. The poly plane path is built from live visplanes after the
BSP/opening clip arrays, while mesh leaves are visible subsector/cell fans clipped by frustum/depth
bands and then rely on Z/order to compose. That loses or darkens significant plane regions in busy
stepped-ceiling scenes.

## DECISION (2026-06-30): pivot to Option 3 (full-scene Z-buffered renderer)
This finding forced a strategy change. Rather than keep debugging leaf-fan planes to imitate
visplane opening masks, the plan is now to make the shared **Z-buffer the authority for ALL opaque
world visibility** and retire the CPU visibility products (visplane `top[]/bottom[]` + drawseg
sprite-clip arrays) in phases (walls+floors -> masked midtex -> sprites -> strip the BSP walk).
The full roadmap, gotchas, and flagged risks live in **`Docs/GPU_PORT_PLAN.md` -> DIRECTION CHANGE
(2026-06-30): Option 3**. The mesh-leaf/cell code here is parked reference, NOT the path forward.

If anyone DOES want to resume the leaf-vs-visplane coverage debugging (NOT the chosen path), the
unfinished measurement was to compare the **actual coverage masks** generated by poly planes vs
mesh leaves for frame 3712:

- For each mesh cell/band triangle, compute its approximate screen coverage bbox / area and sector
  flat/light.
- For each poly-plane trapezoid in `DL_FlushPlanePolys`, dump screen bbox / flat / light at 3712.
- Match by flat/sector/height region. Find what poly-plane regions have no equivalent mesh cell or
  are drawn in different order/light.

## Build / capture notes
- Experimental no-readback mesh leaves:

```bash
BENCH_FORCE_MESH=1 \
BENCH_FORCE_MESH_FLOORS=1 \
BENCH_FORCE_MESH_LEAF_RSP=1 \
BENCH_FORCE_MESH_RSP_EMIT=1 \
BENCH_FORCE_MESH_LEAF_EMIT=1 \
BENCH_MARKS=1 BENCH_MARK_VOID=1
```

- Cells are default unless `BENCH_FORCE_MESH_LEAF_BANDS=1` is set.
- CPU-emits-staged-cells diagnostic:

```bash
BENCH_FORCE_MESH_LEAF_CPU_EMIT=1
```

- Same-geometry captures from this session:
  - Poly baseline: `/tmp/doom-mesh-polyplanes-caps/frame-3712.png`
  - Old no-readback cells: `/tmp/doom-oldwip-recap/frame-3712.png`
  - Whole-leaf bands: `/tmp/doom-leaf-bands-caps/frame-3712.png`
  - CPU emit staged cells: `/tmp/doom-leaf-cpuemit-staged-caps/frame-3712.png`
  - CPU xform + CPU emit staged cells: `/tmp/doom-leaf-cpuemit-cpuxform-caps/frame-3712.png`

**Capture caveat:** compare only captures with the same ares geometry. Earlier captures had different
window sizes and caused misleading crop averages. For the current same-geometry set, the active game
rect in the PNG is approximately `(37,182)..(558,584)`.
