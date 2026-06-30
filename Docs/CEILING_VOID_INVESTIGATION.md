# Ceiling void / flicker investigation (no-readback mesh leaves)

Living log so we STOP re-running ruled-out experiments. Append results; never repeat a
dead end below without a reason.

## Symptom (Ryan, repeated)
- "Usually works", but in **busy / complex-geometry scenes** floors and ceilings **flicker
  out**: the near ceiling that should draw is **missing**, and either **black** or the
  **surface behind it shows through** (e.g. a far ceiling / outdoor area).
- Reproduces statically on the off-128-grid frames **3200** (courtyard: upper area black)
  and **3712** (pillar room: near grey ceiling missing, far green outdoor shows through).
  Capture both with `BENCH_MARK_VOID=1` (frames 837/1274/3482 + 3200/3712 via that path).
- It is NOT a single bad frame and NOT a cells-vs-band thing.

## RULED OUT (do not re-test without cause)
1. **Cells vs band split** — identical output; both show the void. Not the tessellation.
2. **Texcoord-span overflow** (s10.5 edge derivative) — cells bound span ≤512u; measured
   `maxspan=320` at 3712, well under the ~938 limit. Not span.
3. **Projected-cy off-screen overflow** — measured `maxcy=256`, inside StageLeafVtx's
   `[-1024,1224]` cull. Not a cy/edge-field overflow.
4. **Dispatch drop / staging cap** — `CELLDROP=0` and `capstop=0` across the whole demo
   (DL_RSPLeafDispatch instrumentation). The dispatch stages every visible cell.
5. **Degenerate-area cull** (rsp_dlemit.S |cross|<32) — cross is height-independent in cx
   and mirrored in cy, so |cross| is identical for a cell's floor and ceiling. Can't drop
   ceilings only.
6. **Near-cull** (`cd1<=zlo`, nearz_eff=hf*cxf/EDGET) — the on-screen ceiling is always at
   depth > nearz_eff, so it is not near-culled (only the off-screen-above part is).
7. **Z-fight / Z-precision / Z-bias** — RULED OUT by the cleanest test: **disabling the leaf
   z-test entirely (`rdpq_mode_zbuf(false,false)`) makes NO CHANGE** to the void (Ryan
   confirmed). So the ceiling is not losing a z-test — it is **not being rasterised at all**.
   The re-applied `LEAF_ZBIAS=32` (commit fa3cd11) did NOT fix it; the near-black number that
   "improved" was just the far surface showing through the gap, not the ceiling drawing.
   (Ryan: "z is fine unless it's a busy scene" — and even z-off doesn't bring it back.)

## CURRENT CONCLUSION
The near-ceiling cells are **staged by the dispatch but never reach the screen**, and it is
**not the z-buffer** (z-off = no change). So the loss is in the **emit → RSP transform →
LeafFan engine** path, and it is **busy-scene dependent** (Ryan's lead). Leading suspects,
untested: the emit's per-flat / frame-wide `DCAP=2048` descriptor cursor and its mid-flat
`break`/drain (floors are surf=0 and consume the cursor first; ceilings are surf=1 and could
be starved in dense frames), or the flat/cellvis caps, or the RSP transform output for those
cells.

## RULED OUT (cont.)
8. **Emit descriptor drop / DCAP / flat-cap** — EMITDIAG result:
   - quiet f=128:  floor[vis=40 desc=43] ceil[vis=43 desc=45] dcap_break=0
   - void  f=3200: floor[vis=21 desc=26] ceil[vis=20 desc=22] dcap_break=0
   - void  f=3712: floor[vis=40 desc=43] ceil[vis=43 desc=43] dcap_break=0
   Every visible ceiling cell emits a descriptor (vis≈desc), `dcap_break=0` always, and the
   void frames are NOT denser than the quiet one (3200 has FEWER cells). So the emit builds
   and queues all ceiling descriptors; the cap is never hit. The loss is **downstream of the
   emit** (RSP leaf transform or the LeafFan/rdpq_tri engine or the scissor), and the
   floor-vs-ceiling asymmetry is purely **screen-Y** (ceiling=top, floor=bottom). "Busy scene"
   is NOT about cell count.

## CURRENT CONCLUSION (updated)
Ceiling descriptors are emitted but the triangles don't rasterise, it's not z (z-off = no
change), and it's specific to top-of-screen (ceiling) vs bottom (floor). Candidates, untested:
(a) the RDP **scissor / view rect** excludes the top band where ceilings project; (b) the RSP
**leaf transform output** (leaf_out_buf cx/cy/invw/emit) is bad for those cells; (c) the engine
drops them on the per-edge S/T derivative (screen-space, NOT world span -- grazing top
ceilings have a tiny screen edge so dS/dx blows up even though the cell span is ≤512).

## NEXT CANDIDATES (if EMITDIAG is clean)
- Read back `leaf_out_buf` for the void ceiling cells at 3712 to see what the RSP transform
  actually produced (cx/cy/invw/emit) — is the transform itself dropping them in dense frames?
- Per-frame leaf-transform queue: does the wall batch's rspq_wait actually drain the leaf
  transform before the emit reads in busy frames?

## READBACK GROUND TRUTH (decisive) -- LEAFRB / LEAFRBSUM, frame 3712
Added a CPU readback in the cell emit pass-2 (leaf_out_buf is coherent there): per ceiling
band, project cy with the emit-overlay formula, count on-screen verts, and replicate the
rsp_dlemit `|cross|<32` degenerate cull in C.

    LEAFRBSUM f=3712 ceilbands=43 above=19 onscreen=24 onscr_tris=48 onscr_culled=1

- emit0 = 0 on every band -> NOTHING near-plane culled (emit flag clean).
- 24 ceiling bands land ON-SCREEN, 48 fan tris -> the visible ceiling IS staged + emitted.
- Only 1 of 48 on-screen tris hits the degenerate cull -> the `|cross|<32` cull is INNOCENT
  (the doc's earlier "symmetry" rule-out was right in effect; the cull barely fires).

Per-band, the on-screen ceiling is two surfaces:
- FAR sector h=231: cells 102..141, fully on-screen at cy~18..37, cx~150..252 -> DRAW (this is
  the green that shows through).
- NEAR sector h=191 (the grey ceiling that should occlude): cells 84..89. They step UP to the
  231 sector, so the near grey ceiling ends at ~depth 370 and almost all of it projects
  OVERHEAD (cy<0, correctly clipped). Its ONLY on-screen footprint is the farthest grey cell,
  cell 84 `cx[48..136] cy[-38..15]` -- a TOP-EDGE STRADDLER (one vert 38px above the screen,
  the rest on-screen). It emits, survives the cull, but does NOT rasterise.

## ROOT CAUSE (the only mechanism left after emit/cull/z/projection cleared)
TOP-EDGE-STRADDLING ceiling triangles -- a vert tens of px above the top edge, the rest
on-screen -- are dropped/mangled by the raw rsp_rdpq_tri LeafFan engine (verts well outside
the guard band; REJFLAGS=0xFF disables trivial reject so they are NOT clipped, they are sent
raw). The bands that DRAW (far green) are fully on-screen; the band that VANISHES (near grey,
cell 84) is the straddler. This is why z-off changes nothing (the tri never rasterises), why
emit/cull are clean, and why it is "busy-scene"-shaped: it needs a near ceiling that steps up
to a higher far sector so the near ceiling's on-screen footprint is a thin top-edge straddle.

Source of the straddle: the dispatch keeps a 256px OVERSCAN above the screen
(`EDGET = centery + 256`, rdp_view.c) so cy stays inside a now-DISABLED RSP cy-cull window.
That overscan is what lets the straddler reach the engine.

## FIX TESTED -- FAILED (EDGET = centery)
Clipped ceilings at the EXACT top edge: `EDGET = centery` (was `centery + 256`). Structurally
it did what was intended -- LEAFRBSUM f=3712 went `above=19 -> above=0`, cell 84 became
`cy[0..15]` (was `cy[-38..15]`), a clean fully-on-screen non-degenerate triangle that emits and
survives the cull. Ryan tested the build: **the grey ceiling is STILL missing / still flickers**.
So clean on-screen grey triangles STILL do not rasterise -> TOP-EDGE STRADDLING IS RULED OUT.
The drop is something these clean on-screen grey triangles share that the green ones beside them
do not. Commit `e014c77` (EDGET clip) is on the parked branch; it did not fix it.

## STILL OPEN (the real question)
Why do clean, fully-on-screen, non-degenerate GREY (near, h=191) ceiling fan triangles generate
ZERO fragments, while the GREEN (far, h=231) bands right next to them rasterise normally? Ruled
out so far: emit, the |cross|<32 degenerate cull, z (z-off = no change), projection/clip, and now
straddling. The grey band differs from the green by: NEARER (larger invw / smaller W), a different
flat (TMEM), and it is the case that most diverges from WALLS.

## WALL vs PLANE (Ryan's lead -- they are NOT "the same")
- Walls are vertical QUADS: V0/V1 top edge, V2/V3 bottom edge; transform clips them L/R to the
  frustum, emit clips them to TMEM *texture* bands and lerps corner-Y to the band edges. Never thin.
- Planes are horizontal polygons FANNED into n-2 tris from a shared v0, clipped to *depth* bands,
  per-vertex screen-Y via `cy = centery - centerx*FixedMul(hf16,invw)`. Near the horizon they go
  THIN + GRAZING (wide X, few px Y) -- a shape walls structurally cannot make.
- The leaf emit (rsp_dlemit.S ~387) flags that leaf coords are NOT clipped to screen the way wall
  coords are: "the projected coord (cx*4 in StageVtx) skews the RDP edge walker". The whole-leaf
  cx/near culls there are DISABLED. Next probe should look HERE: whether the grey (near) fan tris
  carry an off-screen-X vertex (the side-clip is +-160 world, not screen px) or a near/grazing
  W/INVW that skews the rsp_rdpq_tri edge setup so the RDP emits no spans. A per-vertex readback of
  cx (full range, not just on-screen count) + the staged INVW/W on the grey vs green bands is the
  untested measurement.

## STATUS: PARKED (2026-06-30)
Mesh floors/ceilings do not render correctly in busy scenes; cause unresolved. Checkpointed the
tree back to mesh-walls + poly-planes (`383cd05`); this whole experiment lives on branch
`perf/rdp-mesh-planes-wip`. Resume from "STILL OPEN" + "WALL vs PLANE" above. Build the parked
experiment with the full BENCH_FORCE_MESH_FLOORS + LEAF_* flag set; the working checkpoint builds
with just `BENCH=1 BENCH_FORCE_RDP=1 BENCH_FORCE_MESH=1`.
