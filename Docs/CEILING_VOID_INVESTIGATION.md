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
