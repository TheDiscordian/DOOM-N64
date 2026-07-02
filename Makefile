ifndef N64_INST
N64_INST := $(CURDIR)/libdragon
endif

REQUESTED_N64_INST := $(N64_INST)

ifeq ($(strip $(wildcard $(N64_INST)/mips64-elf/include/ktls.h) $(wildcard $(N64_INST)/include/ktls.h)),)
ifneq ($(wildcard $(CURDIR)/libdragon/mips64-elf/include/ktls.h),)
ifneq ($(origin N64_GCCPREFIX),command line)
N64_GCCPREFIX := $(REQUESTED_N64_INST)
endif
override N64_INST := $(CURDIR)/libdragon
endif
endif

ifeq ($(wildcard $(N64_INST)/bin/mips64-elf-gcc),)
ifneq ($(wildcard /opt/libdragon/bin/mips64-elf-gcc),)
N64_GCCPREFIX := /opt/libdragon
endif
endif
BUILD_DIR = build
SOURCE_DIR = .
DOOM_SRC = linuxdoom-1.10
N64_MKDFS_ROOT = filesystem

DEBUG ?= 0
BENCH ?= 0

ifneq ($(wildcard $(N64_INST)/n64.mk),)
include $(N64_INST)/n64.mk
else
include $(N64_INST)/include/n64.mk
endif

N64_ROM_TITLE = "DOOM N64 WIP"
N64_ROM_SAVETYPE = eeprom4k	# cart EEPROM for persisted settings (not the Controller Pak)

CFLAGS += -I$(DOOM_SRC)
CFLAGS += -IDOOM_N64_Port_Example/src
CFLAGS += -DDEBUG=$(DEBUG)
ifeq ($(BENCH),1)
CFLAGS += -DN64_BENCH=1
# PLANETESS_COUNT=1: count-only go/no-go instrumentation for the future
# "visplanes as RDP polygons" feature. Every bench frame, R_CountPlanePolyTris
# (r_plane.c) tessellates the live visplanes into trapezoid strips using the
# wall split predicate (DL_SPLIT_DEVY) and reports the triangle count -- mean +
# p95 on the BENCH_PLANETESS line. NO render / NO UV / NO RDP emit, so the
# geometry fingerprint is unperturbed. Always on for bench builds (the visplanes
# exist regardless of which wall/plane renderer is selected).
CFLAGS += -DPLANETESS_COUNT=1
# PVS_PROBE=1: one-off count-only go/no-go instrumentation for a PVS/occlusion
# bake. Every bench frame, R_Subsector (r_bsp.c) indexes the existing REJECT
# lump (view sector vs each VISITED subsector's sector) and counts how many the
# REJECT matrix would have culled -- a free, conservative LOWER bound on what a
# true subsector PVS could cull. Reports mean + p95-tail cull_pct on the
# BENCH_PVS line. NO render / NO geometry change (the fingerprint is unperturbed),
# so it is OFF by default -- pass PVS_PROBE=1 on the make line for the one probe run.
ifeq ($(PVS_PROBE),1)
CFLAGS += -DPVS_PROBE=1
endif
# BAKEFAN_PROBE=1: one-off count-only go/no-go for the future native offline-baked
# RDP renderer (DOOM 64's model: per-subsector floor/ceiling LEAF FANS instead of
# runtime visplane trapezoid tessellation). Every bench frame, R_Subsector
# (r_bsp.c) counts the leaf-fan triangles a bake WOULD emit -- (numsegs - 2),
# clamp >=1 -- for each subsector whose floor and/or ceiling is actually drawn
# (floorplane/ceilingplane != NULL, the same visibility the runtime planes use),
# floor + ceiling separately. Reports mean + EXACT p95 on the BENCH_BAKEFAN line,
# directly A/B-able against BENCH_PLANETESS (the runtime trapezoid-run tris the
# bake would replace). NO render / NO geometry change (fingerprint unperturbed),
# so it is OFF by default -- pass BAKEFAN_PROBE=1 on the make line for the probe run.
ifeq ($(BAKEFAN_PROBE),1)
CFLAGS += -DBAKEFAN_PROBE=1
endif
# RDPWAIT_PROBE=1: one-off time/count-only instrumentation pinning WHERE the CPU
# blocks on the RDP/RDRAM. Attributes the async RDP-completion interrupt
# (I_N64BufferDone, fired via rdpq_detach_cb on DP SYNC_FULL) to whichever BPH_*
# bracket is open at fire time (BENCH_ASYNC lines: which phase's "phantom" time is
# really a CP0-completion interrupt, not CPU work), and splits the two present-seam
# waits -- display_get() free-framebuffer acquire and the buffer-flip RDP-busy spin
# -- out of PRESENT/RDP_BUSY (BENCH_ASYNC_HDR). NO render / NO bracket-boundary
# change (fingerprint unperturbed), so it is OFF by default -- pass RDPWAIT_PROBE=1
# on the make line for the probe run.
ifeq ($(RDPWAIT_PROBE),1)
CFLAGS += -DRDPWAIT_PROBE=1
endif
# DPLANES_PROBE=1: one-off time-only sub-bracket of the `planes` BPH bracket. Every
# bench frame, R_DrawPlanes (r_plane.c) accumulates RAW CP0 ticks into three
# counters -- the per-visplane W_CacheLumpNum/Z_ChangeTag flat-lump cache, the
# recursive R_EmitIslandRuns run-fitter (MINUS the un-projection it calls), and the
# R_PlaneCornerAttr float un-projections -- and N64Bench_SetDPlanes latches them.
# Reports mean + EXACT p95 us per sub-part on the BENCH_DPLANES line, to split the
# ~1646us `planes` cost across its constituents so the optimizer attacks the real
# one. The emit body is timed in place but NOT changed (the geometry fingerprint is
# unperturbed), so it is OFF by default -- pass DPLANES_PROBE=1 on the make line.
ifeq ($(DPLANES_PROBE),1)
CFLAGS += -DDPLANES_PROBE=1
endif
# BSPWALK_PROBE=1: one-off time-only sub-bracket of the `bsp_walk` BPH bracket (the
# biggest TAIL contributor, ~2925us mean / ~9294us p95). Every bench frame, call-site CP0
# brackets in r_bsp.c / r_segs.c / r_main.c accumulate RAW ticks into DISJOINT counters --
# R_AddLine whole (incl its nested clip+R_StoreWallRange+R_RenderSegLoop), R_RenderSegLoop
# alone (the SEG_RASTER loop nested in R_AddLine -> SUBTRACTED, so addline_net = addline -
# segloop is the per-seg BSP-walk-as-visibility + seg setup), R_CheckBBox node cull,
# R_AddSprites collection, and DL_MeshDrawWalls (the GPU wall emit charged to bsp_walk) --
# plus an R_AddLine call counter. N64Bench_SetBspWalk latches them; reports MEAN us per
# sub-part on the BENCH_BSPWALK line (mean-only -- per-frame storage for a p95 would bloat
# bench_frames[] BSS ~98KB and OOM the init heap), to split the bsp_walk cost across its
# constituents so the optimizer attacks the real one. The walk body is timed in place but
# NOT changed (the geometry fingerprint is unperturbed -- verify drawsegs/vissprites/
# visplanes stay identical), so it is OFF by default -- pass BSPWALK_PROBE=1 on the make line.
ifeq ($(BSPWALK_PROBE),1)
CFLAGS += -DBSPWALK_PROBE=1
endif
# DLBUILD_TRACE=1: log every CI4 wall-block build (DL_RowMajorBlock) that happens
# DURING rendering (N64Bench_FrameNo > 0; builds at frame 0 are the R_PrecacheLevel
# prequant). A render-time build = a texture the precache MISSED -> a first-touch
# dlbuild spike. Each hit prints `DLBUILD-LATE frame=N tex=I name=NAME raw=R w=.. h=..`
# to the ISViewer log, so a dlbuild-spike frame can be traced to the exact textures.
# Diagnostic only (debugf, no geometry change); OFF by default.
ifeq ($(DLBUILD_TRACE),1)
CFLAGS += -DDLBUILD_TRACE=1
endif
# BENCH_MP=<2|3|4>: scripted local split-screen bench with that many players.
ifneq ($(BENCH_MP),)
CFLAGS += -DN64_BENCH_MP=$(BENCH_MP)
endif
# BENCH_FORCE_RDP=1: the flag-ON A/B run. Pins n64_use_rdp_renderer=1 at startup
# (d_main.c) so the RDP-renderer run is reproducible from committed source
# instead of a throwaway harness patch. Default (unset) is the flag-OFF run.
ifeq ($(BENCH_FORCE_RDP),1)
CFLAGS += -DBENCH_FORCE_RDP=1
# Stage-4 sub-path selectors (only meaningful with BENCH_FORCE_RDP=1). Pin the
# wall/plane A/B toggles at startup so the isolation A/B runs are reproducible
# from committed source.
#   BENCH_FORCE_PLANES_ONLY=1 -> SW walls + RDP planes (the isolation experiment)
#   BENCH_FORCE_WALLS_ONLY=1  -> RDP walls + SW planes (Stage-3 regression guard)
ifeq ($(BENCH_FORCE_PLANES_ONLY),1)
CFLAGS += -DBENCH_FORCE_PLANES_ONLY=1
endif
ifeq ($(BENCH_FORCE_WALLS_ONLY),1)
CFLAGS += -DBENCH_FORCE_WALLS_ONLY=1
endif
#   BENCH_FORCE_MESH=1        -> GPU port: bake the static world mesh at level load
#   (p_setup.c). Phase 1: nothing RENDERS from it yet, so frames stay byte-identical
#   -- this only proves the bake compiles + produces sane counts. See
#   Docs/GPU_PORT_PLAN.md.
ifeq ($(BENCH_FORCE_MESH),1)
CFLAGS += -DBENCH_FORCE_MESH=1
#   BENCH_FORCE_MESH_FLOORS=1 -> GPU port Phase 3: baked floor/ceiling leaf fans on the
#   RDP (replacing the per-frame visplane tessellation). Needs the Z-buffer (occlude vs
#   walls). Nested under BENCH_FORCE_MESH.
ifeq ($(BENCH_FORCE_MESH_FLOORS),1)
CFLAGS += -DBENCH_FORCE_MESH_FLOORS=1
endif
#   BENCH_FORCE_MESH_WORLDZ=1 -> Option 3 Phase A candidate: draw baked floor/ceiling
#   leaves as opaque Z-tested world geometry alongside mesh walls, suppressing non-sky
#   poly planes. Distinct from the old visplane-replacement BENCH_FORCE_MESH_FLOORS path:
#   this is a full-scene-Z stepping stone, not a drop-in mask-equivalence attempt.
ifeq ($(BENCH_FORCE_MESH_WORLDZ),1)
CFLAGS += -DBENCH_FORCE_MESH_WORLDZ=1
#   BENCH_FORCE_MESH_WORLDZ_RSP_EMIT=1 -> Option 3 Phase A perf lever: no-readback RSP
#   emit for the world-Z planes. The CPU keeps the clipping + per-vertex distance light;
#   the RSP transforms and emits the banded plane fans (overlay B leaf pipeline with
#   per-vertex gouraud shade). Implies the wall RSP emit so BOTH classes share overlay
#   B's screen-affine Z convention (mixing it with DL_WallZ breaks depth compares).
#   Reaches CFLAGS and RSPASFLAGS (StageLeafVtx reads per-vertex shade under this flag).
ifeq ($(BENCH_FORCE_MESH_WORLDZ_RSP_EMIT),1)
CFLAGS += -DBENCH_FORCE_MESH_WORLDZ_RSP_EMIT=1
RSPASFLAGS += -DBENCH_FORCE_MESH_WORLDZ_RSP_EMIT=1
BENCH_FORCE_MESH_LEAF_RSP := 1
BENCH_FORCE_MESH_RSP_EMIT := 1
BENCH_FORCE_MESH_LEAF_EMIT := 1
endif
endif
#   BENCH_FORCE_MESH_CULL=1 -> the mesh's OWN visibility: mark walls by frustum (every
#   wall in a frustum-visible subsector) instead of the BSP solidsegs occlusion. Wall-Z
#   handles overdraw. First step toward replacing the per-seg BSP occlusion walk.
ifeq ($(BENCH_FORCE_MESH_CULL),1)
CFLAGS += -DBENCH_FORCE_MESH_CULL=1
endif
#   BENCH_FORCE_MESH_LEAF_RSP=1 -> Phase 4: the floor-leaf vertex transform on the RSP
#   (extends the wall offload to leaves). The ucode leaf command is #ifdef'd so the
#   DEFAULT overlay stays byte-identical (a bigger overlay costs per-frame reload DMA).
#   The flag must reach BOTH the CPU (CFLAGS) and the RSP assembly (RSPASFLAGS).
ifeq ($(BENCH_FORCE_MESH_LEAF_RSP),1)
CFLAGS += -DBENCH_FORCE_MESH_LEAF_RSP=1
RSPASFLAGS += -DBENCH_FORCE_MESH_LEAF_RSP=1
#   BENCH_FORCE_MESH_LEAF_RSP_VERIFY=1 -> A/B the RSP-computed leaf vertex (cx,cy,z,u,v)
#   against the CPU reference each frame (debugf LEAF-AB). CPU-only diagnostic; off by
#   default so the production fold pays no per-vertex compare.
ifeq ($(BENCH_FORCE_MESH_LEAF_RSP_VERIFY),1)
CFLAGS += -DBENCH_FORCE_MESH_LEAF_RSP_VERIFY=1
endif
endif
#   RSP port (Docs/RSP_PORT_PLAN.md): the wall-transform offload is now DEFAULT-ON in
#   the mesh build -- compaction made it beat the CPU transform -6% (render-equivalent,
#   emit_disagree=0). Opt OUT for a CPU-mesh A/B with BENCH_FORCE_MESH_RSP=0.
ifneq ($(BENCH_FORCE_MESH_RSP),0)
MESH_RSP := 1
CFLAGS += -DBENCH_FORCE_MESH_RSP=1
#   BENCH_FORCE_MESH_RSP_EARLY=1 -> OVERLAP experiment (GPU_PORT_PLAN.md): dispatch the wall
#   RSP transform for ALL baked walls BEFORE the BSP walk (which is ~3ms of pure CPU, no
#   RSP/RDP), so the transform overlaps it and the consume-time rspq_wait drops toward 0.
#   Trades the BSP-vis compaction (transform ~475 not ~30 walls) for the overlap -- net is
#   unknown, A/B it. batch_in/out indexed by wall id, not dense vis-slot. Nested under MESH_RSP.
ifeq ($(BENCH_FORCE_MESH_RSP_EARLY),1)
CFLAGS += -DBENCH_FORCE_MESH_RSP_EARLY=1
endif
#   BENCH_FORCE_MESH_RSP_EMIT=1 -> KEYSTONE (RSP_PORT_PLAN.md §9): the RSP TRANSFORMS AND EMITS
#   the wall RDP triangles (via libdragon's rsp_rdpq_tri.inc compiled into rsp_dlwall.S), so the
#   CPU never reads batch_out back (kills the 776us readback stall) nor emits rdpq_triangle.
#   Reaches BOTH CFLAGS and the RSP assembly (RSPASFLAGS). Nested under MESH_RSP.
ifeq ($(BENCH_FORCE_MESH_RSP_EMIT),1)
CFLAGS += -DBENCH_FORCE_MESH_RSP_EMIT=1
RSPASFLAGS += -DBENCH_FORCE_MESH_RSP_EMIT=1
#   BENCH_FORCE_MESH_LEAF_EMIT=1 -> the no-readback FLOOR emit (RSP_PORT_PLAN §8.5): overlay B
#   gains a leaf-fan command (DLEmitCmd_LeafFan); the CPU dispatches it per leaf-surface instead
#   of folding leaf_out_buf back + emitting rdpq_triangle. Requires LEAF_RSP (the transform) +
#   RSP_EMIT (the rsp_rdpq_tri engine). Reaches BOTH CFLAGS and the RSP assembly.
ifeq ($(BENCH_FORCE_MESH_LEAF_EMIT),1)
CFLAGS += -DBENCH_FORCE_MESH_LEAF_EMIT=1
RSPASFLAGS += -DBENCH_FORCE_MESH_LEAF_EMIT=1
endif
endif
endif
endif
endif
# BENCH_FORCE_SHOW_FPS=1: pin the on-screen SHOW-FPS counter ON at startup
# (d_main.c) so a BENCH_MARKS capture can grab the overlay for an A/B vs the
# default-off build -- reproducible from committed source. Renderer-independent
# (not nested under BENCH_FORCE_RDP). Visual-capture builds only -- never timing
# builds; the overlay draw + sprintf would skew the numbers.
ifeq ($(BENCH_FORCE_SHOW_FPS),1)
CFLAGS += -DBENCH_FORCE_SHOW_FPS=1
endif
# BENCH_FORCE_FIXEDCOLORMAP=<n>: pin the player's fixedcolormap to row <n> every
# frame in R_SetupFrame, forcing the whole view into a powerup colormap state the
# E1M1 demo never reaches: 1 = light-amp visor (near-fullbright), 32 = invuln (the
# inverted grey-scale map). Lets a BENCH_MARKS capture verify the CI4 mesh walls'
# fixedcolormap path (rdp_view.c DL_RetintSlot) against a software-rendered
# reference under the SAME forced state. Renderer-independent (applies to SW and
# RDP/mesh alike). Visual-capture builds only -- never timing builds.
ifneq ($(BENCH_FORCE_FIXEDCOLORMAP),)
CFLAGS += -DBENCH_FORCE_FIXEDCOLORMAP=$(BENCH_FORCE_FIXEDCOLORMAP)
endif
# BENCH_VOID_SCAN=1: demo-wide black-void detector. After each present, drain the RDP
# and count pure-black pixels in the view region of the composited 16bpp fb; logs any
# frame >= 40% black ("BENCH_VOID frame=N black=P%"). Catches the off-grid near-total-
# black voids across ALL ~4117 demo frames (the 128-frame marker grid samples only 32).
# Serialises on the RDP every frame -> TIMING IS MEANINGLESS; correctness-diagnostic only.
ifeq ($(BENCH_VOID_SCAN),1)
CFLAGS += -DBENCH_VOID_SCAN=1
endif
# BENCH_WIPE_FREEZE=1: capture hook for the death->respawn MELT WIPE (a transient in
# D_Display's melt loop that bypasses the frame markers). On a few melt steps it emits
# BENCH_MARK sentinels (9208/9216/9224 = early/mid/late melt) + holds ~2s so a
# BENCH_MARKS capture freezes on the mid-melt frame. Pair with BENCH_MARKS=1. Used to
# A/B the mesh-build black wipe vs the software melt. Bench-only; no renderer effect.
ifeq ($(BENCH_WIPE_FREEZE),1)
CFLAGS += -DBENCH_WIPE_FREEZE=1
endif
# BENCH_MARKS=1: frame-keyed visual-capture markers (BENCH_MARK frame=N via
# ISViewer every 256 retained frames) for exactly-paired cross-build
# screenshot series. Visual-capture builds only -- never timing builds, the
# debugf cost would skew the numbers.
ifeq ($(BENCH_MARKS),1)
CFLAGS += -DN64_BENCH_MARKS=1
# BENCH_MARK_FLASH=1: extra off-grid markers on the death-flash detail frames
# (3150/3160) so the red damage-flash band-fix A/B can pair them. Superset of the
# canonical 128-grid (those still fire). Bench-only; no renderer effect.
ifeq ($(BENCH_MARK_FLASH),1)
CFLAGS += -DBENCH_MARK_FLASH=1
endif
# BENCH_MARK_VOID=1: extra off-grid markers on known near-total-BLACK void frames
# (837/1274/3482) so the off-grid-void diagnosis can confirm the void on the current
# build and pair RDP-walls/mesh/plane control builds at the SAME state. Superset of
# the 128-grid (those still fire). Bench-only; no renderer effect.
ifeq ($(BENCH_MARK_VOID),1)
CFLAGS += -DBENCH_MARK_VOID=1
endif
endif
endif
# DL_TRACE=1: one-off diagnostic builds only -- per-present RDP flush/emit
# trace lines (DL_TRACE ...) on the ISViewer log (rdp_view.c DL_DEBUG_TRACE).
# Never for timing runs, never the default for capture runs (extra log
# traffic skews both).
ifeq ($(DL_TRACE),1)
CFLAGS += -DDL_DEBUG_TRACE=1
endif
# PLANE_UV_TRACE=1: one-off diagnostic builds only -- floor-poly texel self-trace
# (PUVT_* lines on the ISViewer log; r_plane.c PLANE_UV_TRACE). Dumps, for the
# first few floor trapezoid polys of an early frame, the per-corner EMITTED u/v/
# invw vs R_MapPlane's EXPECTED s/t AND the poly-center RDP-reconstructed texel
# vs expected, to localise the garbage-floor bug. Never for timing runs (the
# debugf cost + log traffic skew the numbers); default off (compiled out).
ifeq ($(PLANE_UV_TRACE),1)
CFLAGS += -DPLANE_UV_TRACE=1
# Optional frame selection for the FINAL-S/T (rdp_view.c DL_DrawPlanePoly) half of
# the trace. These name a BENCH_MARK frame (128/256/384...): the rdp_view.c dump
# pairs to BENCH_MARK frame=N. Set on the make line, e.g.
#   PLANE_UV_TRACE=1 PLANE_UV_TRACE_FRAME=128 PLANE_UV_TRACE_FRAME2=384
# Default 128 (garbage) + 384 (clean) in the source; FRAME2=0 dumps one frame.
ifneq ($(PLANE_UV_TRACE_FRAME),)
CFLAGS += -DPLANE_UV_TRACE_FRAME=$(PLANE_UV_TRACE_FRAME)
endif
ifneq ($(PLANE_UV_TRACE_FRAME2),)
CFLAGS += -DPLANE_UV_TRACE_FRAME2=$(PLANE_UV_TRACE_FRAME2)
endif
endif
# PLANE_GEOM_TRACE=1: one-off diagnostic builds only -- floor-poly COVERAGE/geometry
# trace (PGT_* lines on the ISViewer log; r_plane.c PLANE_GEOM_TRACE). Dumps, for the
# first few floor trapezoid RUNS of an early frame, the emitted run's screen coverage
# (x1/x2 + four corner ytop/ybot), the visplane's TRUE per-column extent (top[x]/
# bottom[x] at x1/mid/x2 with the poly-vs-visplane row delta), and the adjacent-run
# boundary (prev run right edge vs this run left edge -> overlap/gap/clean). Localises
# the floor-bleed/clip-wrong COVERAGE bug (NOT the texel bug -- that is PLANE_UV_TRACE).
# Never for timing runs (the debugf cost + log traffic skew the numbers); default off.
ifeq ($(PLANE_GEOM_TRACE),1)
CFLAGS += -DPLANE_GEOM_TRACE=1
endif
# Optional marker-frame overrides for the geom trace (default 3200/3328 in source):
#   PLANE_GEOM_TRACE=1 PLANE_GEOM_TRACE_FRAME=3200 PLANE_GEOM_TRACE_FRAME2=3328
ifneq ($(PLANE_GEOM_TRACE_FRAME),)
CFLAGS += -DPLANE_GEOM_TRACE_FRAME=$(PLANE_GEOM_TRACE_FRAME)
endif
ifneq ($(PLANE_GEOM_TRACE_FRAME2),)
CFLAGS += -DPLANE_GEOM_TRACE_FRAME2=$(PLANE_GEOM_TRACE_FRAME2)
endif
ifeq ($(strip $(wildcard $(REQUESTED_N64_INST)/mips64-elf/include/ktls.h) $(wildcard $(REQUESTED_N64_INST)/include/ktls.h)),)
ifneq ($(wildcard $(CURDIR)/libdragon/include/ktls.h),)
CFLAGS += -I$(CURDIR)/libdragon/include
endif
endif
CFLAGS += -Wno-error
CFLAGS += -Wno-old-style-definition
CFLAGS += -Wno-error=enum-compare
CFLAGS += -Wno-error=sizeof-pointer-memaccess
CFLAGS += -Wno-error=pointer-sign
CFLAGS += -Wno-error=misleading-indentation
CFLAGS += -Wno-error=implicit-int
CFLAGS += -Wno-error=implicit-function-declaration
CFLAGS += -Wno-error=maybe-uninitialized
CFLAGS += -Wno-error=uninitialized

DOOM_COMMON_SRCS = \
	$(DOOM_SRC)/doomdef.c \
	$(DOOM_SRC)/doomstat.c \
	$(DOOM_SRC)/dstrings.c \
	$(DOOM_SRC)/tables.c \
	$(DOOM_SRC)/f_finale.c \
	$(DOOM_SRC)/f_wipe.c \
	$(DOOM_SRC)/d_main.c \
	$(DOOM_SRC)/d_net.c \
	$(DOOM_SRC)/d_items.c \
	$(DOOM_SRC)/g_game.c \
	$(DOOM_SRC)/m_menu.c \
	$(DOOM_SRC)/m_misc.c \
	$(DOOM_SRC)/m_argv.c \
	$(DOOM_SRC)/m_bbox.c \
	$(DOOM_SRC)/m_fixed.c \
	$(DOOM_SRC)/m_swap.c \
	$(DOOM_SRC)/m_cheat.c \
	$(DOOM_SRC)/m_random.c \
	$(DOOM_SRC)/am_map.c \
	$(DOOM_SRC)/p_ceilng.c \
	$(DOOM_SRC)/p_doors.c \
	$(DOOM_SRC)/p_enemy.c \
	$(DOOM_SRC)/p_floor.c \
	$(DOOM_SRC)/p_inter.c \
	$(DOOM_SRC)/p_lights.c \
	$(DOOM_SRC)/p_map.c \
	$(DOOM_SRC)/p_maputl.c \
	$(DOOM_SRC)/p_plats.c \
	$(DOOM_SRC)/p_pspr.c \
	$(DOOM_SRC)/p_setup.c \
	$(DOOM_SRC)/p_sight.c \
	$(DOOM_SRC)/p_spec.c \
	$(DOOM_SRC)/p_switch.c \
	$(DOOM_SRC)/p_mobj.c \
	$(DOOM_SRC)/p_telept.c \
	$(DOOM_SRC)/p_tick.c \
	$(DOOM_SRC)/p_saveg.c \
	$(DOOM_SRC)/p_user.c \
	$(DOOM_SRC)/r_bake.c \
	$(DOOM_SRC)/r_bsp.c \
	$(DOOM_SRC)/r_data.c \
	$(DOOM_SRC)/r_draw.c \
	$(DOOM_SRC)/r_main.c \
	$(DOOM_SRC)/r_plane.c \
	$(DOOM_SRC)/r_segs.c \
	$(DOOM_SRC)/r_sky.c \
	$(DOOM_SRC)/r_things.c \
	$(DOOM_SRC)/w_wad.c \
	$(DOOM_SRC)/wi_stuff.c \
	$(DOOM_SRC)/v_video.c \
	$(DOOM_SRC)/st_lib.c \
	$(DOOM_SRC)/st_stuff.c \
	$(DOOM_SRC)/hu_stuff.c \
	$(DOOM_SRC)/hu_lib.c \
	$(DOOM_SRC)/s_sound.c \
	$(DOOM_SRC)/z_zone.c \
	$(DOOM_SRC)/info.c \
	$(DOOM_SRC)/sounds.c \
	$(DOOM_SRC)/lzfx.c

ifeq ($(BENCH),1)
DOOM_COMMON_SRCS += $(DOOM_SRC)/n64_bench.c
endif

DOOM_PLATFORM_SRCS = \
	$(DOOM_SRC)/i_main_n64.c \
	$(DOOM_SRC)/i_wad_browser_n64.c \
	$(DOOM_SRC)/i_system_n64.c \
	$(DOOM_SRC)/i_video_n64.c \
	$(DOOM_SRC)/i_sound_n64.c \
	$(DOOM_SRC)/i_net_n64.c \
	$(DOOM_SRC)/rdp_view.c

DOOM_SRCS = $(DOOM_COMMON_SRCS) $(DOOM_PLATFORM_SRCS)
OBJS = $(DOOM_SRCS:%.c=$(BUILD_DIR)/%.o)

# RSP port (Docs/RSP_PORT_PLAN.md): the rsp_dlwall overlay is linked whenever the RSP
# offload is on -- now the default in the mesh build (MESH_RSP, set above unless
# BENCH_FORCE_MESH_RSP=0). Outside the mesh build the .o is never built or linked, so the
# default ROM stays byte-identical. The n64.mk %.o:%.S rule auto-detects the "rsp"
# prefix and builds it as RSP ucode (the DEFINE_RSP_UCODE symbols resolve from here).
#
# IMPORTANT: the source MUST live under rsp/ (a DASH-FREE path), NOT linuxdoom-1.10/.
# n64.mk derives the ucode symbol prefix with $(subst .,_,$(subst /,_,...)) but does
# NOT substitute '-', while objcopy's _binary_* symbols mangle BOTH '.' and '-' to '_'.
# A build path containing '-' (e.g. build/linuxdoom-1.10/) makes the two disagree, so
# --redefine-sym silently no-ops and rsp_dlwall_text_start stays undefined at link.
# build/rsp/rsp_dlwall.o => prefix build_rsp_rsp_dlwall, which matches. (Cannot fix in
# n64.mk -- libdragon is read-only.)
ifeq ($(MESH_RSP),1)
OBJS += $(BUILD_DIR)/rsp/rsp_dlwall.o
endif

# RSP-EMIT keystone two-overlay split: overlay B (rsp_dlemit) hosts the rsp_rdpq_tri
# triangle engine + the per-wall emit, reading batch_out that overlay A (rsp_dlwall)
# transforms. Only built when the emit flag is on (which implies MESH_RSP).
ifeq ($(BENCH_FORCE_MESH_RSP_EMIT),1)
OBJS += $(BUILD_DIR)/rsp/rsp_dlemit.o
endif

# Hot TUs at -O3 (appended after n64.mk's -O2; last -O wins).
# Renderer (round 1), plus game logic, sound mixer, and MUS synth (round 2).
$(BUILD_DIR)/$(DOOM_SRC)/r_%.o: CFLAGS += -O3
$(BUILD_DIR)/$(DOOM_SRC)/p_%.o: CFLAGS += -O3
$(BUILD_DIR)/$(DOOM_SRC)/i_sound_n64.o: CFLAGS += -O3
$(BUILD_DIR)/$(DOOM_SRC)/s_sound.o: CFLAGS += -O3

MUSIC_ASSETS_XM_LOWER = $(wildcard assets/music/*.xm)
MUSIC_ASSETS_XM_UPPER = $(wildcard assets/music/*.XM)
MUSIC_ASSETS_YM_LOWER = $(wildcard assets/music/*.ym)
MUSIC_ASSETS_YM_UPPER = $(wildcard assets/music/*.YM)

MUSIC_ASSETS_CONV = \
	$(addprefix $(N64_MKDFS_ROOT)/music/,$(notdir $(MUSIC_ASSETS_XM_LOWER:%.xm=%.xm64))) \
	$(addprefix $(N64_MKDFS_ROOT)/music/,$(notdir $(MUSIC_ASSETS_XM_UPPER:%.XM=%.xm64))) \
	$(addprefix $(N64_MKDFS_ROOT)/music/,$(notdir $(MUSIC_ASSETS_YM_LOWER:%.ym=%.ym64))) \
	$(addprefix $(N64_MKDFS_ROOT)/music/,$(notdir $(MUSIC_ASSETS_YM_UPPER:%.YM=%.ym64)))

MUS_INSTRUMENT_BANK_SRC ?= MUS/MIDI_Instruments
MUS_BANK_PROFILE ?= legacy
MUS_BANK_TOOL_SRC := tools/mus_bank_tier1.c
MUS_BANK_TOOL_BIN := $(BUILD_DIR)/host/mus_bank_tier1
MUS_INSTRUMENT_BANK_TIER1 := $(BUILD_DIR)/music/MIDI_Instruments.tier1.bin
MUS_INSTRUMENT_BANK_STAGE_SRC := $(MUS_INSTRUMENT_BANK_SRC)
MUS_BANK_PROFILE_EFFECTIVE := $(MUS_BANK_PROFILE)

ifeq ($(MUS_BANK_PROFILE),tier1)
ifneq ($(wildcard $(MUS_BANK_TOOL_SRC)),)
MUS_INSTRUMENT_BANK_STAGE_SRC := $(MUS_INSTRUMENT_BANK_TIER1)
else
MUS_BANK_PROFILE_EFFECTIVE := legacy
endif
endif

ifneq ($(wildcard $(MUS_INSTRUMENT_BANK_SRC)),)
MUS_INSTRUMENT_BANK_DST := $(N64_MKDFS_ROOT)/MUS/MIDI_Instruments
MUS_BANK_ASSET := $(MUS_INSTRUMENT_BANK_DST)
endif

# WAD filenames to leave out of the ROM, e.g. make EXCLUDE_WADS=DOOM1.WAD
EXCLUDE_WADS ?=
WAD_ASSETS_LOWER = $(filter-out $(addprefix WADs/,$(EXCLUDE_WADS)),$(wildcard WADs/*.wad))
WAD_ASSETS_UPPER = $(filter-out $(addprefix WADs/,$(EXCLUDE_WADS)),$(wildcard WADs/*.WAD))

WAD_ASSETS_COPY = \
	$(addprefix $(N64_MKDFS_ROOT)/,$(notdir $(WAD_ASSETS_LOWER))) \
	$(addprefix $(N64_MKDFS_ROOT)/,$(notdir $(WAD_ASSETS_UPPER)))

ROM_NAME = Doom-N64
FS_PREP_STAMP = $(BUILD_DIR)/.filesystem-prepared.stamp

all: $(ROM_NAME).z64
.PHONY: all clean prepare-filesystem check-wads stage-wads check-music-assets FORCE

FORCE:

$(BUILD_DIR)/$(ROM_NAME).elf: $(OBJS)

$(ROM_NAME).z64: $(BUILD_DIR)/doom.dfs

prepare-filesystem: $(FS_PREP_STAMP)

$(FS_PREP_STAMP): FORCE
	@echo "    [FS] Resetting filesystem"
	@rm -rf $(N64_MKDFS_ROOT)
	@mkdir -p $(N64_MKDFS_ROOT)/MUS
	@mkdir -p $(BUILD_DIR)
	@touch $@

check-wads: $(FS_PREP_STAMP)
	@if [ -z "$(strip $(WAD_ASSETS_LOWER) $(WAD_ASSETS_UPPER))" ]; then \
		echo "Missing WADs: place one or more .wad/.WAD files in WADs/"; \
		exit 1; \
	fi

stage-wads: check-wads | $(FS_PREP_STAMP)
	@set -e; \
	for src in $(WAD_ASSETS_LOWER) $(WAD_ASSETS_UPPER); do \
		if [ -f "$$src" ]; then \
			dst="$(N64_MKDFS_ROOT)/$$(basename "$$src")"; \
			echo "    [WAD] $$src -> $$dst"; \
			cp "$$src" "$$dst"; \
		fi; \
	done

check-music-assets:
	@if [ -z "$(strip $(MUSIC_ASSETS_XM_LOWER) $(MUSIC_ASSETS_XM_UPPER) $(MUSIC_ASSETS_YM_LOWER) $(MUSIC_ASSETS_YM_UPPER))" ]; then \
		echo "    [AUDIO] No assets/music .xm/.ym sources found; MUS fallback path will be used."; \
	fi
	@if [ ! -f "$(MUS_INSTRUMENT_BANK_SRC)" ]; then \
		echo "    [AUDIO] Missing MUS instrument bank ($(MUS_INSTRUMENT_BANK_SRC)); fallback uses synthetic waveforms when samples are unavailable."; \
	fi
	@if [ "$(MUS_BANK_PROFILE)" = "tier1" ] && [ ! -f "$(MUS_BANK_TOOL_SRC)" ]; then \
		echo "    [AUDIO] Missing Tier1 converter source ($(MUS_BANK_TOOL_SRC)); auto-falling back to legacy MUS bank staging."; \
	fi

$(N64_MKDFS_ROOT)/%.wad: WADs/%.wad | $(FS_PREP_STAMP)
	@mkdir -p $(dir $@)
	@echo "    [WAD] $< -> $@"
	@cp "$<" "$@"

$(N64_MKDFS_ROOT)/%.WAD: WADs/%.WAD | $(FS_PREP_STAMP)
	@mkdir -p $(dir $@)
	@echo "    [WAD] $< -> $@"
	@cp "$<" "$@"

$(N64_MKDFS_ROOT)/music/%.xm64: assets/music/%.xm | $(FS_PREP_STAMP)
	@mkdir -p $(dir $@)
	@echo "    [AUDIO] $@"
	@$(N64_AUDIOCONV) -o $(N64_MKDFS_ROOT)/music "$<"

$(N64_MKDFS_ROOT)/music/%.xm64: assets/music/%.XM | $(FS_PREP_STAMP)
	@mkdir -p $(dir $@)
	@echo "    [AUDIO] $@"
	@$(N64_AUDIOCONV) -o $(N64_MKDFS_ROOT)/music "$<"

$(N64_MKDFS_ROOT)/music/%.ym64: assets/music/%.ym | $(FS_PREP_STAMP)
	@mkdir -p $(dir $@)
	@echo "    [AUDIO] $@"
	@$(N64_AUDIOCONV) -o $(N64_MKDFS_ROOT)/music "$<"

$(N64_MKDFS_ROOT)/music/%.ym64: assets/music/%.YM | $(FS_PREP_STAMP)
	@mkdir -p $(dir $@)
	@echo "    [AUDIO] $@"
	@$(N64_AUDIOCONV) -o $(N64_MKDFS_ROOT)/music "$<"

$(MUS_BANK_TOOL_BIN): $(MUS_BANK_TOOL_SRC)
	@mkdir -p $(dir $@)
	@echo "    [HOST] $@"
	@cc -O2 -std=c11 -Wall -Wextra -o "$@" "$<"

$(MUS_INSTRUMENT_BANK_TIER1): $(MUS_INSTRUMENT_BANK_SRC) $(MUS_BANK_TOOL_BIN)
	@mkdir -p $(dir $@)
	@echo "    [AUDIO] Tier1 MUS bank $@"
	@"$(MUS_BANK_TOOL_BIN)" "$<" "$@"

$(N64_MKDFS_ROOT)/MUS/MIDI_Instruments: $(MUS_INSTRUMENT_BANK_STAGE_SRC) | $(FS_PREP_STAMP)
	@mkdir -p $(dir $@)
	@echo "    [AUDIO] MUS bank ($(MUS_BANK_PROFILE_EFFECTIVE)) $< -> $@"
	@cp "$<" "$@"

$(BUILD_DIR)/doom.dfs: $(FS_PREP_STAMP)
$(BUILD_DIR)/doom.dfs: check-wads
$(BUILD_DIR)/doom.dfs: stage-wads
$(BUILD_DIR)/doom.dfs: check-music-assets
$(BUILD_DIR)/doom.dfs: $(MUSIC_ASSETS_CONV) $(MUS_BANK_ASSET)

clean:
	rm -rf $(BUILD_DIR) *.z64 *.v64

-include $(wildcard $(BUILD_DIR)/**/*.d)
-include $(wildcard $(BUILD_DIR)/*.d)
