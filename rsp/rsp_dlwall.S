#include <rsp_queue.inc>

# TWO-OVERLAY SPLIT: this overlay (A) is now TRANSFORM-ONLY. The rsp_rdpq_tri engine
# + the per-wall emit moved to rsp_dlemit.S (overlay B), which reads batch_out and
# emits. A transforms every visible wall -> batch_out (RDRAM); the CPU then dispatches
# B (no rspq_wait, no readback). Reason: the engine (~1.4KB) + transform + S/T overflow
# one 4KB IMEM overlay. See rsp_dlemit.S header.

########################################################################
# rsp_dlwall.S -- RSP wall-transform overlay (GPU port; see Docs/RSP_PORT_PLAN.md)
#
# LOCATION: this file MUST live under rsp/ (a dash-free path), NOT linuxdoom-1.10/.
# n64.mk builds it as RSP ucode (filename starts with "rsp") and derives the symbol
# prefix without substituting '-', while objcopy mangles '-' to '_' in its _binary_*
# symbols -- a '-' in the build path (build/linuxdoom-1.10/) breaks --redefine-sym so
# rsp_dlwall_text_start never gets defined. See Makefile (BENCH_FORCE_MESH_RSP block).
#
# ======================================================================
# PHASE 0 -- DMA LOOPBACK PROBE (no transform math). DLWallCmd_Loopback DMAs a
# block of bytes RDRAM->DMEM->RDRAM byte-identically. De-risks overlay
# registration + 8-byte DMA alignment + CPU/RSP cache coherency. KEPT.
# ----------------------------------------------------------------------
# PHASE 1 -- ONE-WALL TRANSFORM, bit-exact compare gate. DLWallCmd_Xform DMAs a
# small fixed-point view block + one bake wall in, reproduces the per-corner
# world->screen transform of DL_MeshDrawWalls (rdp_view.c) ON THE RSP using only
# the VECTOR unit (the RSP scalar core has NO mul/div), and DMAs a fixed-point
# rsp_wall_out_t back out. The CPU recomputes the same fields in IEEE32 float and
# diffs them field-by-field -- the load-bearing precision gate for the whole port.
#
# The two precision risks the gate resolves (Docs/RSP_PORT_PLAN.md Sec 5):
#   1. 65536/depth divide  -> vrcph/vrcpl/vrcph + ONE Newton-Raphson step (VU).
#   2. FixedMul (a*b)>>16   -> vmudl/vmadm/vmadn/vmadh dual-precision 32x32->32.16.
########################################################################

    .set noreorder
    .set at

    .data

RSPQ_BeginOverlayHeader
    RSPQ_DefineCommand DLWallCmd_Loopback,  12   # 0x00: (size, src, dst) = 3 words
    RSPQ_DefineCommand DLWallCmd_Xform,     12   # 0x04: (viewblk, wall, out) = 3 words
    RSPQ_DefineCommand DLWallCmd_Batch,     16   # 0x08: (viewblk, in, out, count) = 4 words
#ifdef BENCH_FORCE_MESH_LEAF_RSP
    RSPQ_DefineCommand DLWallCmd_LeafBatch, 16   # 0x0C: (viewblk, in, out, vertcount) = 4 words
#endif
# RSP-EMIT reuses DLWallCmd_Batch (it transforms AND emits under the flag) -- no new command.
RSPQ_EndOverlayHeader

# The overlay needs no persisted state across commands.
RSPQ_EmptySavedState

    .bss

# DMEM scratch the loopback DMAs through (Phase 0). 8-byte aligned.
#define LOOP_SCRATCH 1024
    .align 3
LOOP_BUF: .ds.b LOOP_SCRATCH

# ---- Phase 1 transform scratch (DMEM) --------------------------------
# View block laid out by the CPU (DL_RSPXformProbe). All 32-bit words, signed.
# Kept compact + 8-byte aligned for the DMA. Field offsets MUST match the CPU
# struct rsp_view_blk_t in rdp_view.c.
    .align 3
VIEWBLK:
VB_viewx:   .word 0     # 0x00  fixed_t viewx
VB_viewy:   .word 0     # 0x04  fixed_t viewy
VB_viewz:   .word 0     # 0x08  fixed_t viewz
VB_vcos:    .word 0     # 0x0C  fixed_t finecosine[ang]
VB_vsin:    .word 0     # 0x10  fixed_t finesine[ang]
VB_centerx: .word 0     # 0x14  int centerx (plain integer)
VB_centery: .word 0     # 0x18  int centery (plain integer)
VB_pad0:    .word 0     # 0x1C  pad to 8-byte multiple (32 bytes total)
#define VIEWBLK_BYTES 32

# One baked wall's transform inputs (CPU packs only what the RSP needs, all 32-bit).
    .align 3
WALLIN:
WI_x1:      .word 0     # 0x00 fixed_t x1
WI_y1:      .word 0     # 0x04 fixed_t y1
WI_x2:      .word 0     # 0x08 fixed_t x2
WI_y2:      .word 0     # 0x0C fixed_t y2
WI_ztop:    .word 0     # 0x10 fixed_t ztop (CPU-snapshotted live sector height)
WI_zbot:    .word 0     # 0x14 fixed_t zbot
WI_pad0:    .word 0     # 0x18 pad
WI_pad1:    .word 0     # 0x1C pad to 32 bytes
#define WALLIN_BYTES 32

# Output record the RSP fills then DMAs back. All fixed-point (no FPU on the RSP);
# the CPU converts to float on read. Field offsets MUST match rsp_wall_out_t.
    .align 3
WALLOUT:
WO_dA:      .word 0     # 0x00 fixed_t depth at corner A (16.16)
WO_dB:      .word 0     # 0x04 fixed_t depth at corner B
WO_lA:      .word 0     # 0x08 fixed_t lateral at A (16.16)
WO_lB:      .word 0     # 0x0C fixed_t lateral at B
WO_invwA:   .word 0     # 0x10 invw at A = 65536/depth, stored as 16.16 fixed
WO_invwB:   .word 0     # 0x14 invw at B
WO_sxA:     .word 0     # 0x18 screen X at A, 16.16
WO_sxB:     .word 0     # 0x1C screen X at B
WO_ytA:     .word 0     # 0x20 screen Y top at A, 16.16
WO_ybA:     .word 0     # 0x24 screen Y bottom at A
WO_ytB:     .word 0     # 0x28 screen Y top at B
WO_ybB:     .word 0     # 0x2C screen Y bottom at B
#define WALLOUT_BYTES 48

# Vector scratch in DMEM for staging 16-bit lane data the VU loads via lqv/ldv.
    .align 4
VSCRATCH: .ds.b 64

# ---- Phase 2 batch scratch (DMEM) ------------------------------------
# The CPU packs the WHOLE visible wall set; the RSP can't hold it all in DMEM
# (491 E1M1 walls * 32B = ~15 KiB >> DMEM), so the batch command DMAs the wall
# inputs in chunks of BATCH_N walls, transforms each, and DMAs a matching chunk
# of outputs back. BATCH_N kept small (DMEM is shared with the whole rspq state).
#
# Per-wall input (BWALL_IN, 32B): same fields the CPU resolves each frame plus a
# precomputed visibility byte (bake_linevis[line] && ztop>zbot). Layout MUST match
# rsp_bwall_in_t in rdp_view.c.
#define BWI_x1    0x00
#define BWI_y1    0x04
#define BWI_x2    0x08
#define BWI_y2    0x0C
#define BWI_ztop  0x10
#define BWI_zbot  0x14
#define BWI_vis   0x18      /* 1 = passed bake_linevis && ztop>zbot, else skip */
#define BWI_texoff 0x1C     /* RSP-EMIT: S at corner A (16.16 texels, pre near-clip) */
#define BWI_slen   0x20     /* baked wall length (16.16 texels); S at B = texoff+slen */
#define BWI_ttop   0x24     /* T at top edge = midw - ztop (16.16 texels, CPU-precomp) */
#define BWI_tbot   0x28     /* T at bottom edge = midw - zbot */
#define BWI_light  0x2C     /* packed RGBA shade = dl_prim_lut[level] (per-wall light) */
#define BWALL_IN_BYTES 48

# Per-wall output (BWALL_OUT, 64B): the 48B WALLOUT fields + an emit flag word +
# padding to a 64B granule. Layout MUST match rsp_bwall_out_t in rdp_view.c.
#define BWO_dA    0x00
#define BWO_dB    0x04
#define BWO_lA    0x08
#define BWO_lB    0x0C
#define BWO_invwA 0x10
#define BWO_invwB 0x14
#define BWO_sxA   0x18
#define BWO_sxB   0x1C
#define BWO_ytA   0x20
#define BWO_ybA   0x24
#define BWO_ytB   0x28
#define BWO_ybB   0x2C
#define BWO_emit  0x30      /* 1 = wall survives all skips (emitted), 0 = culled */
#define BWO_slopeA 0x34     /* dY/dT at corner A = (ybA-ytA)/(t_bot-t_top), 16.16 px/texel */
#define BWO_slopeB 0x38     /* dY/dT at corner B (was pad1) -- B's per-band Y clip uses these */
#define BWO_rgba  0x3C      /* packed RGBA shade copied from BWI_light (per-wall light) */
#define BWO_sA    0x40      /* RSP-EMIT: S at column A (16.16 texels, post near-clip) */
#define BWO_sB    0x44      /* S at column B */
#define BWO_ttop  0x48      /* T at top edge (copied from input) */
#define BWO_tbot  0x4C      /* T at bottom edge */
#define BWALL_OUT_BYTES 80

# How many walls to transform per DMA chunk. Kept small: the overlay's whole
# .data+.bss must fit DMEM (4 KiB) alongside the persistent rspq state + the
# 256B command buffer, AND the Phase-0 LOOP_BUF (1 KiB) is still resident. 8 in *
# 32B = 256B, 8 out * 64B = 512B = 768B of batch scratch. ~491 E1M1 walls => ~62
# chunks/frame; this is a correctness phase (CPU still renders), so chunk count is
# not a perf concern.
#define BATCH_N 8
    .align 3
BIN_BUF:  .ds.b BATCH_N * BWALL_IN_BYTES      # staged wall inputs (DMA'd in)
    .align 3
BOUT_BUF: .ds.b BATCH_N * BWALL_OUT_BYTES     # transformed outputs (DMA'd back to batch_out)

# Phase 4 leaf transform REUSES BIN_BUF / BOUT_BUF / BATCH_STATE (no new DMEM -- DMEM is
# at the cap). A leaf element is ONE vertex: in = (x,y) 8B; out = (cx, invw, emit) padded
# to 16B. 8 verts/chunk fits BIN_BUF (8*8=64<=256) and BOUT_BUF (8*16=128<=512).
# Layout MUST match rsp_bleaf_in_t / rsp_bleaf_out_t in rdp_view.c.
#define BLI_x      0x00
#define BLI_y      0x04
#define BLI_floorz 0x08    /* live surface height (fixed_t) -> screen-y */
#define BLEAF_IN_BYTES  16
#define BLO_cx   0x00      /* screen-x, 16.16 */
#define BLO_cy   0x04      /* screen-y, 16.16 (from floorz) */
#define BLO_invw 0x08      /* 65536/depth, 16.16 */
#define BLO_z    0x0C      /* zbuf depth */
#define BLO_u    0x10      /* world-coord flat texel x */
#define BLO_v    0x14      /* world-coord flat texel y */
#define BLO_emit 0x18      /* 1 = vertex in front of near plane, 0 = leaf must be culled */
#define BLO_pad  0x1C
#define BLEAF_OUT_BYTES 32

# Batch loop bookkeeping (MUST be in .bss/DMEM -- the RSP scalar lw/sw only reach
# DMEM 0x000-0xFFF; a .text/IMEM address here silently corrupts every load/store).
    .align 3
BATCH_STATE:
BS_inrd:   .word 0      # current RDRAM read cursor (input array)
BS_outrd:  .word 0      # current RDRAM write cursor (output array)
BS_remain: .word 0      # walls left to process
BS_chunk:  .word 0      # walls in the chunk currently in DMEM
BS_idx:    .word 0      # wall index within the current chunk
BS_inptr:  .word 0      # DMEM ptr into BIN_BUF for the current wall
BS_outptr: .word 0      # DMEM ptr into BOUT_BUF for the current wall
BS_pad:    .word 0

# Per-wall transform scratch (also DMEM): ra/in/out + near-clipped intermediates.
    .align 3
XW_ra:    .word 0    # saved return address (helpers clobber ra)
XW_in:    .word 0    # saved input  DMEM ptr
XW_out:   .word 0    # saved output DMEM ptr
XW_txa:   .word 0    # near-clipped tx/ty per corner
XW_tya:   .word 0
XW_txb:   .word 0
XW_tyb:   .word 0
XW_dA:    .word 0    # near-clipped depths
XW_dB:    .word 0
XW_top16: .word 0    # ztop - viewz
XW_bot16: .word 0
XW_cx:    .word 0    # centerx
XW_cy:    .word 0    # centery
XW_pad:   .word 0
XW_sA:    .word 0    # RSP-EMIT S/T scratch: S at column A (16.16 texels, post near-clip)
XW_sB:    .word 0    # S at column B
XW_ttop:  .word 0    # T at top edge (copied from BWI_ttop)
XW_tbot:  .word 0    # T at bottom edge (copied from BWI_tbot)

    .text

# ============================================================
# PHASE 0: DMA loopback probe (unchanged).
# ============================================================
DLWallCmd_Loopback:
#ifdef BENCH_FORCE_MESH_RSP_EMIT
    j    RSPQ_Loop               # Phase-0 probe stripped: free IMEM for the tri-emit engine
    nop
#else
    and  t0, a0, 0xFFFFFF        # DMA size word (byte_count-1), strip cmd id byte
    li   s4, %lo(LOOP_BUF)
    jal  DMAIn                   # RDRAM(a1)->DMEM, waits
    move s0, a1                  # (delay) s0 = src
    li   s4, %lo(LOOP_BUF)
    move s0, a2                  # RDRAM dst
    j    DMAOut                  # DMEM->RDRAM(a2)
    li   ra, %lo(RSPQ_Loop)      # (delay) return to queue engine
#endif


# ============================================================
# PHASE 1: one-wall transform.
#   a0 = RDRAM addr of the 32-byte view block
#   a1 = RDRAM addr of the 32-byte wall-input block
#   a2 = RDRAM addr of the 48-byte output block (CPU invalidates after)
#
# The transform mirrors DL_MeshDrawWalls EXACTLY for the unclipped path (the CPU
# picks a wall with both corners past the near plane and on-screen so no clip
# lerps run -- the gate isolates the two hardware precision risks):
#   tx = x - viewx ; ty = y - viewy
#   d  = FixedMul(tx,vcos) + FixedMul(ty,vsin)          (depth)
#   l  = FixedMul(ty,vcos) - FixedMul(tx,vsin)          (lateral)
#   invw = 65536 / d                                    (== 1/depth_mapunits)
#   sc   = centerx * invw                               (px per map-unit, float)
#   sx   = centerx - l*(1/65536)*sc
#   top  = ztop*(1/65536) - viewz*(1/65536)
#   bot  = zbot*(1/65536) - viewz*(1/65536)
#   ytop = centery - top*sc ; ybot = centery - bot*sc
#
# We carry everything in fixed-point. invw is computed as a 32-bit reciprocal of
# depth scaled so the result is 65536/depth in 16.16. sc = centerx*invw is then a
# 16.16 multiply; sx/sy follow with FixedMul-style 32x32->16.16 products.
# ============================================================
DLWallCmd_Xform:
#ifdef BENCH_FORCE_MESH_RSP_EMIT
    j    RSPQ_Loop               # Phase-1 probe stripped: free IMEM for the tri-emit engine
    nop
#else
    # ---- DMA the view block in ----
    # DMAIn reads the transfer size from t0 (DMA_SIZE width/height); the command
    # dispatch leaves t0 undefined, so it MUST be set per DMA or a short/garbage
    # transfer leaves tail fields (e.g. centery, the last VIEWBLK word) stale.
    li   t0, DMA_SIZE(VIEWBLK_BYTES, 1) & 0xFFF
    li   s4, %lo(VIEWBLK)
    jal  DMAIn
    move s0, a0                  # (delay) s0 = view block RDRAM addr (a0 preserved? no:
                                 #  DMAExec destroys only t2/at; a0/a1/a2 survive)
    # ---- DMA the wall input in ----
    li   t0, DMA_SIZE(WALLIN_BYTES, 1) & 0xFFF
    li   s4, %lo(WALLIN)
    jal  DMAIn
    move s0, a1                  # (delay) s0 = wall input RDRAM addr

    # ============================================================
    # SCALAR LOADS. Bring the 32-bit fixed-point inputs into GPRs.
    # ============================================================
    lw   t0, %lo(WI_x1)          # x1
    lw   t1, %lo(WI_y1)          # y1
    lw   t2, %lo(WI_x2)          # x2
    lw   t3, %lo(WI_y2)          # y2
    lw   s1, %lo(VB_viewx)       # viewx
    lw   s2, %lo(VB_viewy)       # viewy

    sub  t0, t0, s1              # txa = x1 - viewx
    sub  t1, t1, s2              # tya = y1 - viewy
    sub  t2, t2, s1              # txb = x2 - viewx
    sub  t3, t3, s2              # tyb = y2 - viewy

    # txa,tya,txb,tyb now live in t0,t1,t2,t3 (consumed below). Trig terms:
    vxor $v00, $v00, $v00        # vzero (used by Recip_invw)
    lw   s3, %lo(VB_vcos)
    lw   s5, %lo(VB_vsin)

    # ============================================================
    # All multiplies go through FixedMulVU (the dual-precision VU FixedMul). The RSP
    # scalar core has no multiply, so EVERY product below is a VU call. Each
    # FixedMulVU(a3,s7) returns (a3*s7)>>16 in v1 (signed 16.16), bit-exact to the
    # CPU's FixedMul. Kept one product per call for an auditable gate (correctness,
    # not speed -- this is Phase 1).
    # ============================================================

    # dA = FixedMul(txa,vcos) + FixedMul(tya,vsin)
    move a3, t0                  # a = txa
    jal  FixedMulVU              # -> v1 (result fixed_t in v1)
    move s7, s3                  # (delay) b = vcos
    move s0, v1                  # s0 = FixedMul(txa,vcos)
    move a3, t1                  # a = tya
    jal  FixedMulVU
    move s7, s5                  # (delay) b = vsin
    add  s0, s0, v1              # dA
    sw   s0, %lo(WO_dA)

    # dB = FixedMul(txb,vcos) + FixedMul(tyb,vsin)
    move a3, t2
    jal  FixedMulVU
    move s7, s3
    move s0, v1
    move a3, t3
    jal  FixedMulVU
    move s7, s5
    add  s0, s0, v1
    sw   s0, %lo(WO_dB)

    # lA = FixedMul(tya,vcos) - FixedMul(txa,vsin)
    move a3, t1
    jal  FixedMulVU
    move s7, s3
    move s0, v1
    move a3, t0
    jal  FixedMulVU
    move s7, s5
    sub  s0, s0, v1
    sw   s0, %lo(WO_lA)

    # lB = FixedMul(tyb,vcos) - FixedMul(txb,vsin)
    move a3, t3
    jal  FixedMulVU
    move s7, s3
    move s0, v1
    move a3, t2
    jal  FixedMulVU
    move s7, s5
    sub  s0, s0, v1
    sw   s0, %lo(WO_lB)

    # ============================================================
    # FIXED-POINT REFORMULATION (avoids the float-only sc/invw dynamic-range blowup).
    # The CPU computes (all real):
    #   invw = 65536/depth_real ; sc = centerx*invw ;
    #   sx   = centerx - (l/65536)*sc = centerx - centerx*(l_real/depth_real)
    #   ytop = centery - top_real*sc = centery - centerx*(top_real/depth_real)
    # Note l_real/depth_real = l_fixed/depth_fixed (the 2^16 scales cancel), and
    # likewise top_real/depth_real. Both ratios are BOUNDED (the wall is on-screen),
    # so they fit 16.16. We get each via FixedDivVU(a,b) = (a<<16)/b in 16.16
    # (vrcp-based), then:
    #   sx16   = (centerx<<16) - centerx * FixedDiv(l,   depth)
    #   ytop16 = (centery<<16) - centerx * FixedDiv(top, depth)
    # centerx * <16.16 ratio> stays in range (centerx<=160, ratio a few). invw for
    # the gate output = FixedDiv(65536, depth) = (65536<<16)/depth = 2^32/depth =
    # invw_real in 16.16 (depth_real>2 so invw_real<32768 -> fits). One divide per
    # corner reused for sx + both Y's.
    # ============================================================
    lw   s3, %lo(VB_centerx)     # centerx (int)
    lw   t4, %lo(WI_ztop)
    lw   t5, %lo(WI_zbot)
    lw   t6, %lo(VB_viewz)
    sub  t4, t4, t6              # top16 = ztop - viewz
    sub  t5, t5, t6             # bot16 = zbot - viewz
    lw   s6, %lo(VB_centery)     # centery (int)

    # ---- corner A (depth in WO_dA, lateral in WO_lA) ----
    lw   a3, %lo(WO_dA)
    jal  RecipFixVU              # s1:s2 = (ri:rf) 32-bit reciprocal of dA significand
    nop                          # results parked in DMEM RDIV_R; depth in RDIV_D
    # invwA = FixedDiv(65536, dA)
    li   a3, 0x10000             # 65536
    jal  FixedDivApply           # -> v1 = (a3<<16)/dA in 16.16  (uses parked recip)
    nop
    sw   v1, %lo(WO_invwA)
    # ratio_lA = FixedDiv(lA, dA) ; sxA = (centerx<<16) - centerx*ratio_lA
    lw   a3, %lo(WO_lA)
    jal  FixedDivApply
    nop
    move a3, v1                  # ratio (16.16)
    jal  MulIntVU                # v1 = centerx * ratio (16.16), s7=centerx
    move s7, s3
    sll  t7, s3, 16              # centerx<<16
    sub  s0, t7, v1
    sw   s0, %lo(WO_sxA)
    # ytA = (centery<<16) - centerx * FixedMul(top16, invwA)
    # invwA (16.16) is == invw_real; FixedMul(top16, invwA) = top16*invw_real =
    # topf*65536*invw_real = topf*scA/centerx (in 16.16). Then *centerx = topf*scA.
    # (Must NOT use FixedDiv(top16,dA): top16 is a fixed_t (already <<16), so
    #  (top16<<16) overflows AND the 65536 scale is wrong -- the height is in fixed,
    #  the lateral ratio path divides a fixed by a fixed so its scales cancel, the
    #  height path does not.)
    lw   s7, %lo(WO_invwA)        # invwA 16.16
    move a3, t4                   # top16
    jal  FixedMulVU              # v1 = top16*invwA>>16 (16.16 ratio = topf*scA/centerx)
    nop
    move a3, v1
    jal  MulIntVU
    move s7, s3                  # centerx
    sll  t8, s6, 16              # centery<<16
    sub  s0, t8, v1
    sw   s0, %lo(WO_ytA)
    # ybA = (centery<<16) - centerx * FixedMul(bot16, invwA)
    lw   s7, %lo(WO_invwA)
    move a3, t5                   # bot16
    jal  FixedMulVU
    nop
    move a3, v1
    jal  MulIntVU
    move s7, s3
    sll  t8, s6, 16
    sub  s0, t8, v1
    sw   s0, %lo(WO_ybA)

    # ---- corner B (depth in WO_dB, lateral in WO_lB) ----
    lw   a3, %lo(WO_dB)
    jal  RecipFixVU
    nop
    li   a3, 0x10000
    jal  FixedDivApply
    nop
    sw   v1, %lo(WO_invwB)
    lw   a3, %lo(WO_lB)
    jal  FixedDivApply
    nop
    move a3, v1
    jal  MulIntVU
    move s7, s3
    sll  t7, s3, 16
    sub  s0, t7, v1
    sw   s0, %lo(WO_sxB)
    # ytB = (centery<<16) - centerx * FixedMul(top16, invwB)
    lw   s7, %lo(WO_invwB)
    move a3, t4                   # top16
    jal  FixedMulVU
    nop
    move a3, v1
    jal  MulIntVU
    move s7, s3
    sll  t8, s6, 16
    sub  s0, t8, v1
    sw   s0, %lo(WO_ytB)
    # ybB = (centery<<16) - centerx * FixedMul(bot16, invwB)
    lw   s7, %lo(WO_invwB)
    move a3, t5                   # bot16
    jal  FixedMulVU
    nop
    move a3, v1
    jal  MulIntVU
    move s7, s3
    sll  t8, s6, 16
    sub  s0, t8, v1
    sw   s0, %lo(WO_ybB)

    # ---- DMA the output record back to RDRAM ----
    li   t0, DMA_SIZE(WALLOUT_BYTES, 1) & 0xFFF   # width word; height 1
    li   s4, %lo(WALLOUT)
    move s0, a2                  # RDRAM out addr
    j    DMAOut
    li   ra, %lo(RSPQ_Loop)      # (delay) return to the queue engine
#endif


# ============================================================
# PHASE 2: full-batch transform. The CPU still owns the render; this proves the
# RSP transform + emit decisions match the CPU at scale over a whole frame.
#   a0 = RDRAM addr of the 32-byte view block (same rsp_view_blk_t as Phase 1)
#   a1 = RDRAM addr of the wall-input array  (rsp_bwall_in_t[count], 32B each)
#   a2 = RDRAM addr of the wall-output array (rsp_bwall_out_t[count], 64B each)
#   a3 = wall count
#
# The RSP DMAs the view block once, then walks the input array in chunks of
# BATCH_N walls: DMA a chunk in, transform each into BOUT_BUF, DMA the chunk out.
# Each wall's transform mirrors DL_MeshDrawWalls EXACTLY for the EMIT decision and
# the per-corner (post-near-clip) screen geometry: bake_linevis && ztop>zbot (the
# CPU folds these into BWI_vis), both-corners-behind-near skip, near-plane corner
# slide, back-face (sxB<=sxA) skip, fully-off-screen skip. BWO_emit records the
# verdict; on a skip the geometry fields are left 0 (the CPU ignores them when
# emit=0, comparing only the emit flag for culled walls -- so the slot alignment is
# preserved 1:1 between CPU and RSP).
#
# Loop bookkeeping (BATCH_STATE / BS_*) lives in DMEM -- declared in .bss above
# (the RSP scalar lw/sw only reach DMEM; an IMEM/.text address would corrupt every
# load/store). Only the compile-time constants live here in .text.
#define NEARZ (4 << 16)             /* nearz = 4<<FRACBITS, same as the CPU */
#define SCRW_M1_16 ((320 - 1) << 16)  /* (SCREENWIDTH-1) in 16.16 for the offscreen test */

DLWallCmd_Batch:
    # ---- DMA the view block in (once for the whole batch) ----
    li   t0, DMA_SIZE(VIEWBLK_BYTES, 1) & 0xFFF
    li   s4, %lo(VIEWBLK)
    jal  DMAIn
    move s0, a0                  # (delay) s0 = view block RDRAM addr

    # Park the batch cursors in DMEM.
    sw   a1, %lo(BS_inrd)
    sw   a2, %lo(BS_outrd)
    sw   a3, %lo(BS_remain)

BatchChunkLoop:
    lw   t4, %lo(BS_remain)
    blez t4, BatchDone           # nothing left
    nop
    # chunk = min(remain, BATCH_N)
    li   t5, BATCH_N
    sltu t3, t4, t5              # remain < BATCH_N ?
    beqz t3, 1f
    nop
    move t5, t4                  # chunk = remain (remain < BATCH_N)
1:
    sw   t5, %lo(BS_chunk)

    # ---- DMA this chunk of inputs in: chunk * BWALL_IN_BYTES bytes ----
    sll  t6, t5, 5               # chunk * 32
    sll  t7, t5, 4               # chunk * 16   (48 = 32 + 16; no RSP scalar mul)
    addu t6, t6, t7              # bytes = chunk * 48 (BWALL_IN_BYTES)
    addiu t6, t6, -1            # DMA width word = byte_count - 1
    andi t0, t6, 0xFFF
    li   s4, %lo(BIN_BUF)
    lw   s0, %lo(BS_inrd)
    jal  DMAIn
    nop

    # ---- per-wall loop over the chunk ----
    sw   zero, %lo(BS_idx)
    li   t0, %lo(BIN_BUF)
    sw   t0, %lo(BS_inptr)
    li   t0, %lo(BOUT_BUF)
    sw   t0, %lo(BS_outptr)

BatchWallLoop:
    lw   t0, %lo(BS_idx)
    lw   t5, %lo(BS_chunk)
    bge  t0, t5, BatchChunkEmit
    nop
    jal  XformWall               # transforms BS_inptr -> BS_outptr (reads VIEWBLK)
    nop
    # (emit moved to overlay B, rsp_dlemit.S -- this overlay is transform-only)
    # advance per-wall cursors
    lw   t0, %lo(BS_idx)
    addiu t0, t0, 1
    sw   t0, %lo(BS_idx)
    lw   t1, %lo(BS_inptr)
    addiu t1, t1, BWALL_IN_BYTES
    sw   t1, %lo(BS_inptr)
    lw   t1, %lo(BS_outptr)
    addiu t1, t1, BWALL_OUT_BYTES
    sw   t1, %lo(BS_outptr)
    j    BatchWallLoop
    nop

BatchChunkEmit:
    # ---- DMA this chunk of outputs back: chunk * BWALL_OUT_BYTES bytes ----
    lw   t5, %lo(BS_chunk)
    sll  t6, t5, 6               # chunk * 64
    sll  t7, t5, 4               # chunk * 16   (80 = 64 + 16; no RSP scalar mul)
    addu t6, t6, t7              # bytes = chunk * 80 (BWALL_OUT_BYTES)
    addiu t6, t6, -1
    andi t0, t6, 0xFFF
    li   s4, %lo(BOUT_BUF)
    lw   s0, %lo(BS_outrd)
    jal  DMAOut
    nop

    # advance the RDRAM cursors + remaining count for the next chunk
    lw   t5, %lo(BS_chunk)
    lw   t1, %lo(BS_inrd)
    sll  t6, t5, 5               # chunk * 48 (32 + 16)
    sll  t7, t5, 4
    addu t6, t6, t7
    addu t1, t1, t6
    sw   t1, %lo(BS_inrd)
    lw   t1, %lo(BS_outrd)
    sll  t6, t5, 6               # chunk * 80 (64 + 16)
    sll  t7, t5, 4
    addu t6, t6, t7
    addu t1, t1, t6
    sw   t1, %lo(BS_outrd)
    lw   t1, %lo(BS_remain)
    subu t1, t1, t5
    j    BatchChunkLoop
    sw   t1, %lo(BS_remain)      # (delay)

BatchDone:
    j    RSPQ_Loop               # transform-only: batch_out is written; overlay B emits
    nop


#ifdef BENCH_FORCE_MESH_LEAF_RSP
# ============================================================
# DLWallCmd_LeafBatch (Phase 4): transform a flat array of LEAF VERTICES.
#   a0 = view block RDRAM, a1 = in array (rsp_bleaf_in_t[]), a2 = out array
#   (rsp_bleaf_out_t[]), a3 = vertex count. Mirrors DLWallCmd_Batch's chunked DMA
#   loop but with leaf strides and XformLeaf. Reuses BIN_BUF/BOUT_BUF/BATCH_STATE.
#   Runs under the default .set at (like DLWallCmd_Batch) -- bge/etc. need $at.
# ============================================================
DLWallCmd_LeafBatch:
    # ---- DMA the view block in (once) ----
    li   t0, DMA_SIZE(VIEWBLK_BYTES, 1) & 0xFFF
    li   s4, %lo(VIEWBLK)
    jal  DMAIn
    move s0, a0                  # (delay) s0 = view block RDRAM addr
    sw   a1, %lo(BS_inrd)
    sw   a2, %lo(BS_outrd)
    sw   a3, %lo(BS_remain)
LeafChunkLoop:
    lw   t4, %lo(BS_remain)
    blez t4, LeafDone
    nop
    li   t5, BATCH_N
    sltu t3, t4, t5
    beqz t3, 1f
    nop
    move t5, t4
1:  sw   t5, %lo(BS_chunk)
    # ---- DMA this chunk of inputs in: chunk * BLEAF_IN_BYTES (16) ----
    sll  t6, t5, 4
    addiu t6, t6, -1
    andi t0, t6, 0xFFF
    li   s4, %lo(BIN_BUF)
    lw   s0, %lo(BS_inrd)
    jal  DMAIn
    nop
    sw   zero, %lo(BS_idx)
    li   t0, %lo(BIN_BUF)
    sw   t0, %lo(BS_inptr)
    li   t0, %lo(BOUT_BUF)
    sw   t0, %lo(BS_outptr)
LeafVertLoop:
    lw   t0, %lo(BS_idx)
    lw   t5, %lo(BS_chunk)
    bge  t0, t5, LeafChunkEmit
    nop
    jal  XformLeaf               # BS_inptr -> BS_outptr (reads VIEWBLK)
    nop
    lw   t0, %lo(BS_idx)
    addiu t0, t0, 1
    sw   t0, %lo(BS_idx)
    lw   t1, %lo(BS_inptr)
    addiu t1, t1, BLEAF_IN_BYTES
    sw   t1, %lo(BS_inptr)
    lw   t1, %lo(BS_outptr)
    addiu t1, t1, BLEAF_OUT_BYTES
    sw   t1, %lo(BS_outptr)
    j    LeafVertLoop
    nop
LeafChunkEmit:
    lw   t5, %lo(BS_chunk)
    sll  t6, t5, 5               # bytes = chunk * 32 (BLEAF_OUT_BYTES)
    addiu t6, t6, -1
    andi t0, t6, 0xFFF
    li   s4, %lo(BOUT_BUF)
    lw   s0, %lo(BS_outrd)
    jal  DMAOut
    nop
    lw   t5, %lo(BS_chunk)
    lw   t1, %lo(BS_inrd)
    sll  t6, t5, 4
    addu t1, t1, t6
    sw   t1, %lo(BS_inrd)
    lw   t1, %lo(BS_outrd)
    sll  t6, t5, 5
    addu t1, t1, t6
    sw   t1, %lo(BS_outrd)
    lw   t1, %lo(BS_remain)
    subu t1, t1, t5
    j    LeafChunkLoop
    sw   t1, %lo(BS_remain)      # (delay)
LeafDone:
    j    RSPQ_Loop
    nop
    .set at


# ============================================================
# XformLeaf : project ONE leaf vertex. A reduced XformWall corner-A path -- no
#   2-corner, no near-clip slide, no heights/screen-Y, no back-face/off-screen.
#   reads:  VIEWBLK, the vert at BS_inptr (BLI_x/BLI_y)
#   writes: BS_outptr -> cx (== wall sx), invw, emit. The CPU derives sc=centerx*invw,
#           z=DL_WallZ(invw), u/v, and cy per surface (DL_DrawMeshLeaves). A vert behind
#           the near plane sets emit=0 -> the CPU culls the whole leaf (matches its
#           "if depth < nearz: bad" cull). Reuses XW_* DMEM scratch + the helpers.
# ============================================================
    .set noat
XformLeaf:
    sw   ra, %lo(XW_ra)
    lw   t8, %lo(BS_inptr)
    lw   t9, %lo(BS_outptr)
    sw   t8, %lo(XW_in)
    sw   t9, %lo(XW_out)
    # zero the 16B output slot
    sw   zero, BLO_cx(t9)
    sw   zero, BLO_invw(t9)
    sw   zero, BLO_emit(t9)
    sw   zero, BLO_pad(t9)
    # ---- view-space tx/ty ----
    lw   t0, BLI_x(t8)
    lw   t1, BLI_y(t8)
    lw   s1, %lo(VB_viewx)
    lw   s2, %lo(VB_viewy)
    sub  t0, t0, s1              # tx
    sub  t1, t1, s2              # ty
    sw   t0, %lo(XW_txa)
    sw   t1, %lo(XW_tya)
    # ---- depth = tx*vcos + ty*vsin ----
    lw   s3, %lo(VB_vcos)
    lw   s5, %lo(VB_vsin)
    move a3, t0
    jal  FixedMulVU
    move s7, s3                  # (delay) tx*vcos
    move s0, v1
    move a3, t1
    jal  FixedMulVU
    move s7, s5                  # ty*vsin
    add  s0, s0, v1             # depth
    sw   s0, %lo(XW_dA)
    # ---- cull if depth < nearz (leaf vert behind near plane) ----
    li   t8, NEARZ
    slt  t6, s0, t8
    bnez t6, XL_skip
    nop
    # ---- lat = ty*vcos - tx*vsin ----
    lw   t0, %lo(XW_txa)
    lw   t1, %lo(XW_tya)
    move a3, t1
    jal  FixedMulVU
    move s7, s3                  # ty*vcos
    move s0, v1
    move a3, t0
    jal  FixedMulVU
    move s7, s5                  # tx*vsin
    sub  s0, s0, v1             # lat
    sw   s0, %lo(XW_pad)         # park lat (reuse the wall scratch)
    # ---- invw = FixedDiv(65536, depth) ; park recip(depth), reuse for the lat ratio ----
    lw   a3, %lo(XW_dA)
    jal  RecipFixVU
    nop
    li   a3, 0x10000
    jal  FixedDivApply           # invw
    nop
    lw   t9, %lo(XW_out)
    sw   v1, BLO_invw(t9)
    # ---- cx = (centerx<<16) - centerx * FixedDiv(lat, depth) ----
    lw   a3, %lo(XW_pad)         # lat
    jal  FixedDivApply           # ratio = lat/depth (recip(depth) still parked)
    nop
    move a3, v1
    lw   s7, %lo(VB_centerx)
    jal  MulIntVU                # centerx * ratio
    nop
    lw   s3, %lo(VB_centerx)
    sll  t7, s3, 16
    sub  s0, t7, v1             # cx
    lw   t9, %lo(XW_out)
    sw   s0, BLO_cx(t9)
    li   t0, 1
    sw   t0, BLO_emit(t9)
    lw   ra, %lo(XW_ra)
    jr   ra
    nop
XL_skip:
    lw   t9, %lo(XW_out)
    sw   zero, BLO_emit(t9)
    lw   ra, %lo(XW_ra)
    jr   ra
    nop
    .set at
#endif /* BENCH_FORCE_MESH_LEAF_RSP */


# ============================================================
# XformWall : transform one batched wall.
#   reads:  VIEWBLK (DMEM), the wall at DMEM ptr BS_inptr (BWALL_IN layout)
#   writes: the output slot at DMEM ptr BS_outptr (BWALL_OUT layout)
#   ra preserved across all inner helper calls by saving it in DMEM (XW_ra).
#
# Mirrors DL_MeshDrawWalls's emit decision + per-corner transform. On any skip,
# BWO_emit is set 0 and the geometry fields are zeroed. Otherwise BWO_emit=1 and
# all 12 geometry fields are filled (depth/lateral/invw/sx/ytop/ybot per corner).
# (XW_* scratch is declared in .bss above -- it MUST be in DMEM.)
# ============================================================
    .set noat
XformWall:
    sw   ra, %lo(XW_ra)
    lw   t8, %lo(BS_inptr)
    lw   t9, %lo(BS_outptr)
    sw   t8, %lo(XW_in)
    sw   t9, %lo(XW_out)

    # Zero the whole 64-byte output slot first (so a skipped wall reads clean).
    sw   zero, BWO_dA(t9)
    sw   zero, BWO_dB(t9)
    sw   zero, BWO_lA(t9)
    sw   zero, BWO_lB(t9)
    sw   zero, BWO_invwA(t9)
    sw   zero, BWO_invwB(t9)
    sw   zero, BWO_sxA(t9)
    sw   zero, BWO_sxB(t9)
    sw   zero, BWO_ytA(t9)
    sw   zero, BWO_ybA(t9)
    sw   zero, BWO_ytB(t9)
    sw   zero, BWO_ybB(t9)
    sw   zero, BWO_emit(t9)
    sw   zero, BWO_slopeA(t9)
    sw   zero, BWO_slopeB(t9)
    sw   zero, BWO_rgba(t9)
    sw   zero, BWO_sA(t9)
    sw   zero, BWO_sB(t9)
    sw   zero, BWO_ttop(t9)
    sw   zero, BWO_tbot(t9)

    # ---- vis gate (bake_linevis && ztop>zbot, folded by the CPU) ----
    lw   t0, BWI_vis(t8)
    beqz t0, XW_skip             # vis==0 -> culled before transform
    nop

    # ---- RSP-EMIT default S/T: sA=textureoffset, sB=texoff+slen (unclipped); copy T.
    # S is overridden in the near-clip branches below; T is near-clip-invariant.
    lw   t4, BWI_texoff(t8)
    sw   t4, %lo(XW_sA)
    lw   t5, BWI_slen(t8)
    add  t4, t4, t5
    sw   t4, %lo(XW_sB)
    lw   t4, BWI_ttop(t8)
    sw   t4, %lo(XW_ttop)
    lw   t5, BWI_tbot(t8)
    sw   t5, %lo(XW_tbot)

    # ---- load corners + view, compute view-space tx/ty ----
    lw   t0, BWI_x1(t8)
    lw   t1, BWI_y1(t8)
    lw   t2, BWI_x2(t8)
    lw   t3, BWI_y2(t8)
    lw   s1, %lo(VB_viewx)
    lw   s2, %lo(VB_viewy)
    sub  t0, t0, s1              # txa
    sub  t1, t1, s2              # tya
    sub  t2, t2, s1              # txb
    sub  t3, t3, s2              # tyb
    sw   t0, %lo(XW_txa)
    sw   t1, %lo(XW_tya)
    sw   t2, %lo(XW_txb)
    sw   t3, %lo(XW_tyb)

    # ---- depth at each (raw) corner: dA = txa*vcos + tya*vsin ----
    lw   s3, %lo(VB_vcos)
    lw   s5, %lo(VB_vsin)
    move a3, t0
    jal  FixedMulVU
    move s7, s3                  # (delay) txa*vcos
    move s0, v1
    move a3, t1
    jal  FixedMulVU
    move s7, s5                  # tya*vsin
    add  s0, s0, v1              # dA
    sw   s0, %lo(XW_dA)
    move a3, t2
    jal  FixedMulVU
    move s7, s3                  # txb*vcos
    move s0, v1
    move a3, t3
    jal  FixedMulVU
    move s7, s5                  # tyb*vsin
    add  s0, s0, v1              # dB
    sw   s0, %lo(XW_dB)

    # ---- both-behind-near skip: if dA<NEARZ && dB<NEARZ -> cull ----
    # NEARZ kept in t8 (not $at: $at collides with gas's slt/li macro expansion).
    lw   t4, %lo(XW_dA)
    lw   t5, %lo(XW_dB)
    li   t8, NEARZ
    slt  t6, t4, t8              # dA < nearz ?
    slt  t7, t5, t8             # dB < nearz ?
    and  t6, t6, t7
    bnez t6, XW_skip
    nop

    # ---- near-plane corner slide (matches DL_MeshDrawWalls) ----
    # when dA<nearz: u=(nearz-dA)/(dB-dA); txa+=u*(txb-txa); tya+=u*(tyb-tya); dA=nearz
    # otherwise dB<nearz: u=(nearz-dB)/(dA-dB); txb+=u*(txa-txb); tyb+=u*(tya-tyb); dB=nearz
    lw   t4, %lo(XW_dA)
    li   t8, NEARZ
    slt  t6, t4, t8
    beqz t6, XW_clipB_chk
    nop
    # --- clip corner A ---
    lw   t5, %lo(XW_dB)
    sub  a3, t8, t4              # nearz - dA   (t8 == NEARZ)
    sub  s7, t5, t4              # dB - dA
    sw   a3, %lo(XW_pad)         # stash numerator (a3) -- RecipFixVU takes b in a3
    move a3, s7                  # b = (dB - dA)
    jal  RecipFixVU              # park recip(dB-dA)
    nop
    lw   a3, %lo(XW_pad)         # numerator = nearz - dA
    jal  FixedDivApply           # v1 = u = (nearz-dA)/(dB-dA) in 16.16
    nop
    sw   v1, %lo(XW_pad)         # stash u in DMEM (FixedMulVU clobbers t-regs)
    # txa += FixedMul(u, txb - txa)
    lw   t0, %lo(XW_txa)
    lw   t2, %lo(XW_txb)
    sub  s7, t2, t0              # txb - txa
    lw   a3, %lo(XW_pad)         # u
    jal  FixedMulVU
    nop
    lw   t0, %lo(XW_txa)
    add  t0, t0, v1
    sw   t0, %lo(XW_txa)
    # tya += FixedMul(u, tyb - tya)
    lw   t1, %lo(XW_tya)
    lw   t3, %lo(XW_tyb)
    sub  s7, t3, t1
    lw   a3, %lo(XW_pad)         # u
    jal  FixedMulVU
    nop
    lw   t1, %lo(XW_tya)
    add  t1, t1, v1
    sw   t1, %lo(XW_tya)
    li   t8, NEARZ
    sw   t8, %lo(XW_dA)          # dA = nearz
    # RSP-EMIT: clipped S at A = FixedMul(u, slen)  (matches DL_MeshDrawWalls sA=u*sLen)
    lw   t8, %lo(XW_in)
    lw   s7, BWI_slen(t8)        # slen
    lw   a3, %lo(XW_pad)         # u (16.16)
    jal  FixedMulVU              # v1 = u*slen >> 16
    nop
    sw   v1, %lo(XW_sA)
    j    XW_after_clip
    nop
XW_clipB_chk:
    lw   t5, %lo(XW_dB)
    li   t8, NEARZ
    slt  t6, t5, t8
    beqz t6, XW_after_clip
    nop
    # --- clip corner B ---
    lw   t4, %lo(XW_dA)
    sub  a3, t8, t5              # nearz - dB
    sub  s7, t4, t5              # dA - dB
    sw   a3, %lo(XW_pad)
    move a3, s7
    jal  RecipFixVU
    nop
    lw   a3, %lo(XW_pad)
    jal  FixedDivApply           # u
    nop
    sw   v1, %lo(XW_pad)         # stash u in DMEM (FixedMulVU clobbers t-regs)
    lw   t0, %lo(XW_txa)
    lw   t2, %lo(XW_txb)
    sub  s7, t0, t2              # txa - txb
    lw   a3, %lo(XW_pad)         # u
    jal  FixedMulVU
    nop
    lw   t2, %lo(XW_txb)
    add  t2, t2, v1
    sw   t2, %lo(XW_txb)
    lw   t1, %lo(XW_tya)
    lw   t3, %lo(XW_tyb)
    sub  s7, t1, t3              # tya - tyb
    lw   a3, %lo(XW_pad)         # u
    jal  FixedMulVU
    nop
    lw   t3, %lo(XW_tyb)
    add  t3, t3, v1
    sw   t3, %lo(XW_tyb)
    li   t8, NEARZ
    sw   t8, %lo(XW_dB)          # dB = nearz
    # RSP-EMIT: clipped S at B = slen - FixedMul(u, slen)  (matches sB = sLen - u*sLen)
    lw   t8, %lo(XW_in)
    lw   s7, BWI_slen(t8)        # slen
    lw   a3, %lo(XW_pad)         # u (16.16)
    jal  FixedMulVU              # v1 = u*slen >> 16
    nop
    lw   t8, %lo(XW_in)
    lw   t0, BWI_slen(t8)        # slen
    sub  t0, t0, v1              # slen - u*slen
    sw   t0, %lo(XW_sB)

XW_after_clip:
    # Store final depths to the output (the CPU also clamps to nearz here).
    lw   t9, %lo(XW_out)
    lw   t4, %lo(XW_dA)
    lw   t5, %lo(XW_dB)
    sw   t4, BWO_dA(t9)
    sw   t5, BWO_dB(t9)

    # ---- lateral: lA = tya*vcos - txa*vsin ; lB = tyb*vcos - txb*vsin ----
    lw   s3, %lo(VB_vcos)
    lw   s5, %lo(VB_vsin)
    lw   t0, %lo(XW_txa)
    lw   t1, %lo(XW_tya)
    lw   t2, %lo(XW_txb)
    lw   t3, %lo(XW_tyb)
    move a3, t1
    jal  FixedMulVU
    move s7, s3                  # tya*vcos
    move s0, v1
    move a3, t0
    jal  FixedMulVU
    move s7, s5                  # txa*vsin
    sub  s0, s0, v1              # lA
    lw   t9, %lo(XW_out)
    sw   s0, BWO_lA(t9)
    move a3, t3
    jal  FixedMulVU
    move s7, s3                  # tyb*vcos
    move s0, v1
    move a3, t2
    jal  FixedMulVU
    move s7, s5                  # txb*vsin
    sub  s0, s0, v1              # lB
    lw   t9, %lo(XW_out)
    sw   s0, BWO_lB(t9)

    # ---- centerx / centery / heights into DMEM ----
    lw   s3, %lo(VB_centerx)
    lw   s6, %lo(VB_centery)
    sw   s3, %lo(XW_cx)
    sw   s6, %lo(XW_cy)
    lw   t8, %lo(XW_in)
    lw   t4, BWI_ztop(t8)
    lw   t5, BWI_zbot(t8)
    lw   t6, %lo(VB_viewz)
    sub  t4, t4, t6
    sub  t5, t5, t6
    sw   t4, %lo(XW_top16)
    sw   t5, %lo(XW_bot16)

    # ============================================================
    # Per-corner perspective: identical reformulation to Phase 1.
    #   invw = FixedDiv(65536, depth)                    (16.16 == invw_real)
    #   sx   = (centerx<<16) - centerx * FixedDiv(l, depth)
    #   yt   = (centery<<16) - centerx * FixedMul(top16, invw)
    #   yb   = (centery<<16) - centerx * FixedMul(bot16, invw)
    # ============================================================
    # ---- corner A ----
    lw   t9, %lo(XW_out)
    lw   a3, BWO_dA(t9)
    jal  RecipFixVU
    nop
    li   a3, 0x10000
    jal  FixedDivApply           # invwA
    nop
    lw   t9, %lo(XW_out)
    sw   v1, BWO_invwA(t9)
    lw   a3, BWO_lA(t9)
    jal  FixedDivApply           # ratio_lA = FixedDiv(lA, dA)
    nop
    move a3, v1
    lw   s7, %lo(XW_cx)
    jal  MulIntVU                # centerx * ratio_lA
    nop
    lw   s3, %lo(XW_cx)
    sll  t7, s3, 16
    sub  s0, t7, v1              # sxA
    lw   t9, %lo(XW_out)
    sw   s0, BWO_sxA(t9)
    # ytA
    lw   s7, BWO_invwA(t9)
    lw   a3, %lo(XW_top16)
    jal  FixedMulVU              # top16 * invwA
    nop
    move a3, v1
    lw   s7, %lo(XW_cx)
    jal  MulIntVU
    nop
    lw   s6, %lo(XW_cy)
    sll  t8, s6, 16
    sub  s0, t8, v1              # ytA
    lw   t9, %lo(XW_out)
    sw   s0, BWO_ytA(t9)
    # ybA
    lw   s7, BWO_invwA(t9)
    lw   a3, %lo(XW_bot16)
    jal  FixedMulVU
    nop
    move a3, v1
    lw   s7, %lo(XW_cx)
    jal  MulIntVU
    nop
    lw   s6, %lo(XW_cy)
    sll  t8, s6, 16
    sub  s0, t8, v1              # ybA
    lw   t9, %lo(XW_out)
    sw   s0, BWO_ybA(t9)

    # ---- corner B ----
    lw   a3, BWO_dB(t9)
    jal  RecipFixVU
    nop
    li   a3, 0x10000
    jal  FixedDivApply           # invwB
    nop
    lw   t9, %lo(XW_out)
    sw   v1, BWO_invwB(t9)
    lw   a3, BWO_lB(t9)
    jal  FixedDivApply           # ratio_lB
    nop
    move a3, v1
    lw   s7, %lo(XW_cx)
    jal  MulIntVU
    nop
    lw   s3, %lo(XW_cx)
    sll  t7, s3, 16
    sub  s0, t7, v1              # sxB
    lw   t9, %lo(XW_out)
    sw   s0, BWO_sxB(t9)
    # ytB
    lw   s7, BWO_invwB(t9)
    lw   a3, %lo(XW_top16)
    jal  FixedMulVU
    nop
    move a3, v1
    lw   s7, %lo(XW_cx)
    jal  MulIntVU
    nop
    lw   s6, %lo(XW_cy)
    sll  t8, s6, 16
    sub  s0, t8, v1              # ytB
    lw   t9, %lo(XW_out)
    sw   s0, BWO_ytB(t9)
    # ybB
    lw   s7, BWO_invwB(t9)
    lw   a3, %lo(XW_bot16)
    jal  FixedMulVU
    nop
    move a3, v1
    lw   s7, %lo(XW_cx)
    jal  MulIntVU
    nop
    lw   s6, %lo(XW_cy)
    sll  t8, s6, 16
    sub  s0, t8, v1              # ybB
    lw   t9, %lo(XW_out)
    sw   s0, BWO_ybB(t9)

    # ---- emit decision: back-face (sxB<=sxA) + off-screen ----
    #   when sxB <= sxA          -> cull (degenerate/back-facing)
    #   when sxB <= 0            -> cull (fully off left)
    #   when sxA >= (SCRW-1)<<16 -> cull (fully off right)
    lw   t9, %lo(XW_out)
    lw   t0, BWO_sxA(t9)          # sxA 16.16 (signed)
    lw   t1, BWO_sxB(t9)          # sxB 16.16
    slt  t6, t0, t1              # sxA < sxB ?  (need sxB > sxA, i.e. NOT <=)
    beqz t6, XW_skip             # sxB <= sxA -> cull
    nop
    blez t1, XW_skip             # sxB <= 0 -> cull (signed)
    nop
    li   t8, SCRW_M1_16
    slt  t6, t0, t8              # sxA < (SCRW-1)<<16 ?
    beqz t6, XW_skip             # sxA >= edge -> cull
    nop

    # survived all skips -> emit
    li   t0, 1
    sw   t0, BWO_emit(t9)
    # RSP-EMIT: publish per-column S + per-edge T (16.16 texels) into batch_out for B.
    lw   t0, %lo(XW_sA)
    sw   t0, BWO_sA(t9)
    lw   t0, %lo(XW_sB)
    sw   t0, BWO_sB(t9)
    lw   t0, %lo(XW_ttop)
    sw   t0, BWO_ttop(t9)
    lw   t0, %lo(XW_tbot)
    sw   t0, BWO_tbot(t9)
    # per-wall SHADE: copy the packed RGBA light straight through (near-clip invariant)
    lw   t8, %lo(XW_in)
    lw   t0, BWI_light(t8)
    sw   t0, BWO_rgba(t9)

    # ---- T-band clip slopes: slopeX = (ybX - ytX) / (t_bot - t_top), 16.16 px/texel.
    # Overlay B clips each wall to a TMEM T-band via yX' = ytX + (band_edge - t_top)*slopeX
    # (one FixedMul, no per-band divide). denom = t_bot - t_top > 0 (ztop>zbot guaranteed
    # by the CPU vis gate). recip(denom) computed once, applied to each corner's dY.
    lw   t0, %lo(XW_tbot)
    lw   t1, %lo(XW_ttop)
    sub  a3, t0, t1              # denom = t_bot - t_top  (>0, texels 16.16)
    jal  RecipFixVU             # park recip(denom)
    nop
    lw   t9, %lo(XW_out)
    lw   t0, BWO_ybA(t9)
    lw   t1, BWO_ytA(t9)
    sub  a3, t0, t1             # ybA - ytA  (screen px 16.16)
    jal  FixedDivApply          # slopeA = (ybA-ytA)/denom
    nop
    lw   t9, %lo(XW_out)
    sw   v1, BWO_slopeA(t9)
    lw   t0, BWO_ybB(t9)
    lw   t1, BWO_ytB(t9)
    sub  a3, t0, t1             # ybB - ytB
    jal  FixedDivApply          # slopeB = (ybB-ytB)/denom (recip(denom) still parked)
    nop
    lw   t9, %lo(XW_out)
    sw   v1, BWO_slopeB(t9)

    lw   ra, %lo(XW_ra)
    jr   ra
    nop

XW_skip:
    # culled: emit stays 0 (slot already zeroed). Keep the slot aligned with the CPU.
    lw   t9, %lo(XW_out)
    sw   zero, BWO_emit(t9)
    lw   ra, %lo(XW_ra)
    jr   ra
    nop
    .set at


# ============================================================
# FixedMulVU(a3, s7) -> v1 = (a3 * s7) >> 16, signed 16.16 fixed multiply.
# Reproduces FixedMul((long long)a*(long long)b >> 16) EXACTLY using the VU
# dual-precision partial-product pattern (the RSP scalar core has no multiply).
#
# Method: split a=(ah:al), b=(bh:bl) into signed-high / unsigned-low 16-bit halves
# staged into DMEM, lqv into vector lanes, then:
#   vmudl acc = al*bl        (unsigned low)
#   vmadm acc += ah*bl       (ah signed)
#   vmadn rF  = al*bh        (bh signed)  -> fractional 16 bits in lane
#   vmadh rI  = ah*bh        (signed)     -> integer 16 bits in lane
# The 64-bit product is (rI:rF). FixedMul = product>>16 = (rI_lo<<16)|rF_hi.
# We read rI and rF lane0 back to GPRs and assemble.
#
# CLOBBERS: at, t7,t8,t9, v20..v25. Preserves a3,s7 inputs are consumed; result v1.
# ============================================================
    .set noat
FixedMulVU:
    # Stage a and b hi/lo shorts into VSCRATCH (a at +0, b at +8). Big-endian
    # halfwords: storing a 32-bit word puts hi short at +0, lo short at +2.
    sw   a3, %lo(VSCRATCH)+0     # a : [ah][al]
    sw   s7, %lo(VSCRATCH)+4     # b : [bh][bl]
    li   t9, %lo(VSCRATCH)
    # Load a-hi, a-lo, b-hi, b-lo into separate lane-0 of vector regs via lsv.
    lsv  $v20, 0, 0, t9          # v20.e0 = ah (signed high half of a)
    lsv  $v21, 0, 2, t9          # v21.e0 = al (low half of a)
    lsv  $v22, 0, 4, t9          # v22.e0 = bh
    lsv  $v23, 0, 6, t9          # v23.e0 = bl
    # acc = al*bl (unsigned*unsigned)
    vmudl $v24, $v21, $v23       # low partial
    # acc += ah*bl (signed*unsigned -> mid)
    vmadm $v24, $v20, $v23
    # rF = al*bh (unsigned*signed -> mid, fractional)
    vmadn $v24, $v21, $v22       # v24 now fractional lanes (acc N)
    # rI = ah*bh (signed*signed -> high integer)
    vmadh $v25, $v20, $v22       # v25 = integer lanes (acc H)
    # Store fractional + integer lane0 back; assemble (rI_lo<<16)|rF_hi.
    ssv  $v24, 0, 0, t9          # VSCRATCH+0 = rF (low 16 acc)  -> we need rF HIGH 16?
    ssv  $v25, 0, 2, t9          # VSCRATCH+2 = rI (high 16 acc)
    # The accumulator after the 4 ops holds the 48-bit product in (rI:rF) where
    # rI is acc bits [31:16] worth (the "H" accumulator lane) and rF is acc [15:0]
    # ("N" lane). The full product P = (rI << 16) | rF, and FixedMul = P >> 16 = rI
    # WITH its own fractional carry. Because vmadn/vmadh already split at the 16-bit
    # boundary of the 32.16 product, the 16.16 FixedMul result is exactly the 32-bit
    # value (rI:rF) read as integer:fraction = rI<<0 ... assembled below by halfword.
    lh   t7, %lo(VSCRATCH)+2     # rI (high partial, signed)
    lhu  t8, %lo(VSCRATCH)+0     # rF (low partial, unsigned)
    sll  t7, t7, 16
    jr   ra
    or   v1, t7, t8             # (delay) v1 = (rI<<16)|rF = FixedMul result 16.16
    .set at


# ============================================================
# MulIntVU(a3 = 16.16 value, s7 = small signed integer) -> v1 = a3 * s7 (16.16).
# Used for centerx * ratio. Reuses FixedMulVU by promoting the integer to 16.16
# (s7<<16) so (a*(s7<<16))>>16 = a*s7. CLOBBERS as FixedMulVU + at.
# ============================================================
    .set noat
MulIntVU:
    sll  s7, s7, 16             # integer -> 16.16
    j    FixedMulVU             # tail-call: returns to OUR caller via ra
    nop
    .set at


# ============================================================
# RecipFixVU(a3 = b, fixed_t) : compute the VU 32-bit reciprocal of b's significand
# and PARK it (ri:rf vector lanes -> DMEM RDIV_R) plus b's hi/lo (RDIV_D) for the
# following FixedDivApply call(s). Computing the reciprocal ONCE then applying it to
# several numerators (invw, lateral-ratio, top-ratio, bot-ratio share the same depth)
# saves three vrcp sequences per corner. CLOBBERS at, t9, v00,v02..v05. ra preserved.
#
# vrcp(b) returns (ri:rf) ~= 2^31/b_significand whose IMPLICIT EXPONENT depends on
# where b's MSB falls. A single compile-time FIXEDDIV_SH is therefore correct only for
# ONE divisor magnitude (Phase 1's single wall) -- the full batch spans depths 4..1300+
# map units, so the shift must track the divisor exponent or invw drifts by powers of
# two (observed: ~4% on deep walls, catastrophic on the near-clip ratio). Fix:
# NORMALIZE b to a fixed MSB position (NORMBIT) before vrcp, park the normalization
# shift, and let FixedDivApply undo it. b is always positive here (depths >= nearz>0;
# the clip divisor dB-dA>0), so we normalize |b| with a scalar binary-search msb.
# ============================================================
#define NORMBIT 23   /* normalize b's MSB to bit 23 (Phase-1 depth regime; keeps the
                        normalized divisor clear of the sign bit so vrcp treats it as
                        positive, and matches the FIXEDDIV_SH=1 window). Residual: the
                        single-pass vrcp gives ~1-2% on depths far from NORMBIT (a
                        Newton refinement -- Docs/RSP_PORT_PLAN.md Sec 5.1 -- would tighten
                        it; libdragon's GL ships the same single-pass divide w/ a TODO:NR). */
RDIV_R: # parked 32-bit reciprocal (ri at +0, rf at +2)  -- in VSCRATCH+8
RDIV_SH: # parked normalization shift (signed) -- in VSCRATCH+16 (NOT +4: FixedMulVU
         # stages its 'b' operand at VSCRATCH+4 and would clobber the shift between a
         # RecipFixVU and its FixedDivApply when an intervening FixedMulVU runs)
    .set noat
RecipFixVU:
    # --- find msb(b): binary search for the highest set bit of b (b>0) ---
    # CLOBBERS ONLY t7,t8,t9 (+ v00..v05). Phase 1's DLWallCmd_Xform keeps top16/bot16
    # live in t4/t5 across this call, so this routine MUST NOT touch t0..t6, s0..s7.
    # t8 = working value, t9 = msb-bit accumulator, t7 = scratch test ($at can't be an
    # explicit operand on the RSP gas backend).
    move t8, a3
    li   t9, 0
    # bits 31..16 set -> msb>=16
    srl  t7, t8, 16
    beqz t7, 1f
    nop
    addiu t9, t9, 16
    srl  t8, t8, 16
1:  srl  t7, t8, 8
    beqz t7, 2f
    nop
    addiu t9, t9, 8
    srl  t8, t8, 8
2:  srl  t7, t8, 4
    beqz t7, 3f
    nop
    addiu t9, t9, 4
    srl  t8, t8, 4
3:  srl  t7, t8, 2
    beqz t7, 4f
    nop
    addiu t9, t9, 2
    srl  t8, t8, 2
4:  srl  t7, t8, 1
    beqz t7, 5f
    nop
    addiu t9, t9, 1
5:  # t9 = msb index of b (0..30 for our positive divisors)
    # sh = NORMBIT - msb ; bn = (sh>=0) ? b<<sh : b>>(-sh)
    li   t7, NORMBIT
    subu t9, t7, t9            # sh = NORMBIT - msb (signed)
    sw   t9, %lo(VSCRATCH)+16   # park sh (NOT +4 -- FixedMulVU stages 'b' there)
    bgez t9, 6f
    move t8, a3                 # (delay) t8 = b
    # sh < 0 : bn = b >> (-sh)
    subu t7, zero, t9           # -sh
    srlv t8, a3, t7
    j    7f
    nop
6:  # sh >= 0 : bn = b << sh
    sllv t8, a3, t9
7:  sw   t8, %lo(VSCRATCH)+12   # park bn ([bnh][bnl]) for vrcp
    li   t9, %lo(VSCRATCH)
    lsv  $v02, 0, 14, t9        # bnl (low half)  -> vrcpl  (v02.e0)
    lsv  $v03, 0, 12, t9        # bnh (high half) -> vrcph  (v03.e0)
    vxor $v00, $v00, $v00
    vrcph $v04.e0, $v03.e0      # ri  (r0 high 16, signed)
    vrcpl $v05.e0, $v02.e0      # rf  (r0 low  16, unsigned)
    vrcph $v04.e0, $v00.e0      # finalize

    # ----------------------------------------------------------------
    # ONE Newton-Raphson step (Docs/RSP_PORT_PLAN.md Sec 5.1). vrcp gives a single-
    # pass r0 = (ri:rf) ~= 2^31/bn (the FIXEDDIV_SH=1 scaling proves r0's numerator is
    # 2^31: FixedDivApply does (a*r0)>>15 == (a<<16)/bn  =>  r0 == 2^31/bn). Its
    # relative error grows with |NORMBIT-msb(bn)| -- ~1-2% on depths far from NORMBIT,
    # large on near-clip divisors. Refine in the SAME normalized domain:
    #   real Newton  e1 = e0*(2 - bn*e0),  e0 = r0/2^31
    #   scale by 2^31: r1 = r0*(2 - bn*r0/2^31)
    # Compute m = (bn*r0)>>15 in 16.16 (~= 1.0 since bn*r0 ~= 2^31), corr = (2<<16)-m
    # in 16.16, then r1 = (r0*corr)>>16 ~= r0. Both products are the dual-precision
    # 32x32->(>>16) pattern (vmudl/vmadm/vmadn/vmadh); all operands positive here (bn
    # normalized >0, r0>0, corr~1.0>0) so the signed pattern is exact. Uses ONLY
    # t7,t8,t9 (+ v06,v07,v20..v25) so top16/bot16 in t4/t5 stay live for Phase 1; no
    # jal so ra is untouched.
    #
    # m_hi = (bn*r0)>>16 : a = bn = (bnh:bnl) in (v03:v02), b = r0 = (ri:rf) in (v04:v05)
#ifndef RSP_NR_OFF
    # Newton scratch lives at VSCRATCH+24..+31 ONLY: +0/+2/+4 are FixedMul/Div a/b
    # staging, +8/+10 the parked recip, +12/+14 bn, and +16..+19 the RDIV_SH WORD
    # (do NOT touch +18 -- it is sh's high half; clobbering it corrupts the
    # un-normalization on large-|sh| batch divisors). +24.. is clear of all of these.
    vmudl $v24, $v02, $v05      # bnl * rf      (al*bl, unsigned)
    vmadm $v24, $v03, $v05      # + bnh * rf    (ah*bl)
    vmadn $v24, $v02, $v04      # + bnl * ri    (al*bh) -> pF lane
    vmadh $v25, $v03, $v04      # + bnh * ri    (ah*bh) -> pI lane
    ssv  $v25, 0, 24, t9        # pI (signed high 16 of bn*r0) -> VSCRATCH+24
    ssv  $v24, 0, 26, t9        # pF (unsigned low 16)         -> VSCRATCH+26
    lh   t7, %lo(VSCRATCH)+24   # pI
    lhu  t8, %lo(VSCRATCH)+26   # pF
    sll  t7, t7, 16
    or   t7, t7, t8             # t7 = (bn*r0)>>16  (~= 2^15)
    sll  t7, t7, 1              # m = (bn*r0)>>15 in 16.16 (~= 2^16 == 1.0)
    li   t8, 0x20000            # 2.0 in 16.16
    subu t7, t8, t7             # corr = (2<<16) - m   (~= 1.0 in 16.16)
    # r1 = (r0 * corr)>>16 : a = r0 = (ri:rf) in (v04:v05), b = corr (split hi/lo)
    sw   t7, %lo(VSCRATCH)+28   # corr [corrh][corrl]
    lsv  $v06, 0, 28, t9        # corrh (high half, signed)   -> v06.e0
    lsv  $v07, 0, 30, t9        # corrl (low  half, unsigned) -> v07.e0
    vmudl $v24, $v05, $v07      # rf  * corrl   (al*bl)
    vmadm $v24, $v04, $v07      # + ri  * corrl (ah*bl)
    vmadn $v24, $v05, $v06      # + rf  * corrh (al*bh) -> pF
    vmadh $v25, $v04, $v06      # + ri  * corrh (ah*bh) -> pI
    ssv  $v25, 0, 24, t9        # r1 high 16 -> VSCRATCH+24
    ssv  $v24, 0, 26, t9        # r1 low  16 -> VSCRATCH+26
    # repack r1 into ri:rf lanes (v04:v05) so the park below stores the refined recip
    lsv  $v04, 0, 24, t9        # ri' (high 16)
    lsv  $v05, 0, 26, t9        # rf' (low  16)
#endif /* RSP_NR_OFF -- A/B: -DRSP_NR_OFF skips the Newton step (single-pass vrcp) */
    # ----------------------------------------------------------------

    ssv  $v04, 0, 8,  t9        # park ri at VSCRATCH+8
    jr   ra
    ssv  $v05, 0, 10, t9        # (delay) park rf at VSCRATCH+10
    .set at


# ============================================================
# FixedDivApply(a3 = numerator a, 16.16) -> v1 = (a<<16)/b in 16.16, using the
# normalized reciprocal of b parked by the most recent RecipFixVU. Computes
# a*recip(bn) with the dual-precision multiply (giving (a<<16)/bn at FIXEDDIV_SH), then
# undoes the normalization: (a<<16)/b = (a<<16)/(bn>>sh) = ((a<<16)/bn) << sh.
# CLOBBERS at, t6,t7,t8,t9, v20..v25. ra preserved (no inner jal).
# ============================================================
#define FIXEDDIV_SH 1           /* lands (a<<16)/bn for the NORMBIT-normalized divisor */
    .set noat
FixedDivApply:
    # a -> hi/lo at VSCRATCH+0 ; recip already parked at VSCRATCH+8 (ri:rf).
    sw   a3, %lo(VSCRATCH)+0     # a [ah][al]
    li   t9, %lo(VSCRATCH)
    lsv  $v20, 0, 0, t9          # ah
    lsv  $v21, 0, 2, t9          # al
    lsv  $v22, 0, 8, t9          # ri (recip high)
    lsv  $v23, 0, 10, t9         # rf (recip low)
    # product a * recip : (ah:al) * (ri:rf), dual precision.
    vmudl $v24, $v21, $v23
    vmadm $v24, $v20, $v23
    vmadn $v24, $v21, $v22       # pF
    vmadh $v25, $v20, $v22       # pI
    ssv  $v24, 0, 0, t9          # pF -> VSCRATCH+0
    ssv  $v25, 0, 2, t9          # pI -> VSCRATCH+2
    lh   t7, %lo(VSCRATCH)+2     # pI (signed high)
    lhu  t8, %lo(VSCRATCH)+0     # pF (unsigned low)
    sll  t7, t7, 16
    or   t7, t7, t8             # 32-bit window of a*recip = (a<<16)/bn at FIXEDDIV_SH
#if FIXEDDIV_SH >= 0
    sll  t7, t7, FIXEDDIV_SH     # -> (a<<16)/bn in 16.16 (left)
#else
    sra  t7, t7, (-(FIXEDDIV_SH)) # -> (a<<16)/bn in 16.16 (right)
#endif
    # undo normalization: result = ((a<<16)/bn) << sh
    lw   t6, %lo(VSCRATCH)+16    # sh (signed)
    bgez t6, 8f
    nop
    subu t9, zero, t6           # -sh  (t9 free here -- ptr no longer needed)
    srav t7, t7, t9             # sh<0 : >> (-sh)  (arithmetic; numerator can be signed)
    j    9f
    nop
8:  sllv t7, t7, t6             # sh>=0 : << sh
9:  jr   ra
    move v1, t7                 # (delay) v1 = FixedDiv(a,b) in 16.16
    .set at
