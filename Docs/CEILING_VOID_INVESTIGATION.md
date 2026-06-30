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

## OPEN TEST (in progress)
`EMITDIAG` log (rdp_view.c, this commit): per-frame floor-vs-ceiling **descriptors actually
emitted**, visible-cell count, distinct flats, and `dcap_break` count, on busy frames
(3200/3712) vs a quiet frame (128). If ceilings emit far fewer descriptors than are visible
on busy frames, or `dcap_break>0`, that localises the drop. RESULT: _pending build_.

## NEXT CANDIDATES (if EMITDIAG is clean)
- Read back `leaf_out_buf` for the void ceiling cells at 3712 to see what the RSP transform
  actually produced (cx/cy/invw/emit) — is the transform itself dropping them in dense frames?
- Per-frame leaf-transform queue: does the wall batch's rspq_wait actually drain the leaf
  transform before the emit reads in busy frames?
