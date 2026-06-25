#ifdef BENCH_FORCE_MESH_RSP_EMIT
# ============================================================================
# rsp_dlemit.S -- RSP wall-EMIT overlay (overlay B of the two-overlay split).
#
# Why a second overlay: the wall TRANSFORM (rsp_dlwall.S) + libdragon's inlined
# rsp_rdpq_tri triangle engine (~1.4KB) + the per-corner S/T math overflow one
# 4KB IMEM overlay (measured: +180B over). Splitting relieves IMEM:
#   overlay A (rsp_dlwall): transform every visible wall -> batch_out (RDRAM),
#                           geometry + emit flag (+ S/T once that lands).
#   overlay B (this file):  DMA batch_out back in, emit each wall's 2 RDP
#                           triangles via RDPQ_Triangle_Send_Async.
# The CPU dispatches A then B as back-to-back rspq commands with NO rspq_wait and
# NO readback of batch_out -- rspq serializes them and B re-reads batch_out from
# RDRAM via DMA. The ~776us readback stall the port exists to kill stays gone.
#
# This overlay hosts ONLY the engine + emit loop, so it fits with room to spare,
# and frees A to host the S/T math. LOCATION must be dash-free (rsp/) -- see the
# rsp_dlwall.S header for the objcopy/--redefine-sym reason.
# ============================================================================

# Custom vertex layout the rsp_rdpq_tri engine reads. These offsets MUST match
# libdragon/include/rsp_rdpq_tri.inc EXACTLY -- the engine does `lsv vw_i, VTX_ATTR_Wi`
# etc. at fixed offsets. The engine's vertex is a COMPACT 24-byte record with NO clip-
# space position: Wi/Wf/INVWi/INVWf sit at 0x10/0x12/0x14/0x16, packed right after ST.
# (The earlier layout interleaved phantom CLIPPOSi/CLIPPOSf words and pushed Wi..INVWf to
# 0x16..0x22, so StageVtx wrote W/INV_W where the engine reads CLIPPOS and the engine read
# W/INV_W from uninitialised bytes -> degenerate perspective -> the triangles never
# rasterised, i.e. the BLACK WALLS. The engine ignores any bytes past 0x18.)
# DOOM walls are {X,Y,Z,S,T,INV_W} -- no per-vertex shade.
#define RDPQ_TRIANGLE_CUSTOM_VTX 1
#define VTX_ATTR_XY        0x00
#define VTX_ATTR_Z         0x04
#define VTX_ATTR_CLIPFLAGS 0x06
#define VTX_ATTR_REJFLAGS  0x07
#define VTX_ATTR_RGBA      0x08
#define VTX_ATTR_ST        0x0C
#define VTX_ATTR_Wi        0x10
#define VTX_ATTR_Wf        0x12
#define VTX_ATTR_INVWi     0x14
#define VTX_ATTR_INVWf     0x16

#include <rsp_queue.inc>
#include <rdpq_macros.h>
#include <rsp_rdpq_tri.inc>
    # The engine branches here only for clipped/culled tris. Walls write CLIPFLAGS=0
    # and pass v0=cull-disabled, so neither fires; a defined label is required to link.
    # ra still holds the per-tri caller's continuation, so just return to it.
    .text
RDPQ_Triangle_Clip:
RDPQ_Triangle_Cull:
    jr   ra
    nop

    .set noreorder
    .set at

    .data

RSPQ_BeginOverlayHeader
    RSPQ_DefineCommand DLEmitCmd_Batch, 16   # 0x00: (batch_out, count, band_t_lo, band_t_hi)
RSPQ_EndOverlayHeader

RSPQ_EmptySavedState

# ---- batch_out layout (MUST match rsp_bwall_out_t in rdp_view.c AND rsp_dlwall.S BWO_*).
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
#define BWO_emit   0x30
#define BWO_slopeA 0x34     /* dY/dT at corner A (16.16 px/texel) -- T-band Y clip */
#define BWO_slopeB 0x38     /* dY/dT at corner B */
#define BWO_sA    0x40      /* S at column A (16.16 texels, post near-clip) */
#define BWO_sB    0x44      /* S at column B */
#define BWO_ttop  0x48      /* T at top edge (period-relative texels, 16.16) */
#define BWO_tbot  0x4C      /* T at bottom edge */
#define BWALL_OUT_BYTES 80

#define NEARZ (4 << 16)
#define EMIT_TRICMD 0x0B00          /* RDP tri + tex + zbuf, no shade */

    .bss

# How many walls to DMA per chunk (same budget reasoning as rsp_dlwall.S).
#define EBATCH_N 8
    .align 3
EOUT_BUF: .ds.b EBATCH_N * BWALL_OUT_BYTES   # batch_out chunk DMA'd back in

# Vertex scratch (4 wall corners) in the engine's VTX_ATTR layout. 8-byte aligned.
#define EMIT_VTX_SIZE 36
    .align 3
VBUF: .ds.b 4 * EMIT_VTX_SIZE
# DMEM staging the engine assembles each RDP TRI command into (s3 -> here); it DMAs
# it out to RDPQ_CURRENT itself and serializes on COP0_DMA_BUSY (one slot is safe).
    .align 3
EMIT_STAGING: .ds.b 0xB0

    .align 3
ES_outrd:  .word 0      # RDRAM read cursor into batch_out
ES_remain: .word 0      # walls left
ES_chunk:  .word 0      # walls in the current DMEM chunk
ES_idx:    .word 0      # wall index within the chunk
ES_outptr: .word 0      # DMEM ptr into EOUT_BUF for the current wall
ES_band_lo: .word 0     # this band's first source row (16.16 texels) -- T clip lo
ES_band_hi: .word 0     # one past this band's last source row (16.16 texels) -- T clip hi
EW_ra:     .word 0      # saved ra across StageVtx / the engine calls
# Per-wall T-band clip scratch (FixedMulVU clobbers t7..t9, so keep clipped Y/T in DMEM).
EW_Ttop:   .word 0      # clipped T at the band-top edge (period-relative texels)
EW_Tbot:   .word 0      # clipped T at the band-bottom edge
EW_dTtop:  .word 0      # ct_top - t_top (texels into the wall from its top)
EW_dTbot:  .word 0      # ct_bot - t_top
EW_ytA:    .word 0      # clipped screen Y, corner A top
EW_ybA:    .word 0      # corner A bottom
EW_ytB:    .word 0      # corner B top
EW_ybB:    .word 0      # corner B bottom

# Vector-multiply staging for FixedMulVU (the band-clip dY = dT * slope). 8-byte aligned.
    .align 4
VSCRATCH:  .ds.b 32

    .text

# ============================================================================
# DLEmitCmd_Batch -- a0 = batch_out RDRAM addr, a1 = wall count.
# DMA the array back in EBATCH_N walls at a time; emit each wall whose BWO_emit==1.
# ============================================================================
DLEmitCmd_Batch:
    sw   a0, %lo(ES_outrd)
    sw   a1, %lo(ES_remain)
    sw   a2, %lo(ES_band_lo)     # this band's T clip range [band_lo, band_hi)
    sw   a3, %lo(ES_band_hi)

EBatchChunkLoop:
    lw   t4, %lo(ES_remain)
    blez t4, EBatchDone
    nop
    li   t5, EBATCH_N
    sltu t3, t4, t5
    beqz t3, 1f
    nop
    move t5, t4                  # chunk = remain (remain < EBATCH_N)
1:
    sw   t5, %lo(ES_chunk)

    # ---- DMA this chunk of batch_out back in: chunk * 80 bytes ----
    sll  t6, t5, 6               # chunk * 64
    sll  t7, t5, 4               # chunk * 16   (80 = 64 + 16; no RSP scalar mul)
    addu t6, t6, t7              # bytes = chunk * 80 (BWALL_OUT_BYTES)
    addiu t6, t6, -1             # DMA width word = byte_count - 1
    andi t0, t6, 0xFFF
    li   s4, %lo(EOUT_BUF)
    lw   s0, %lo(ES_outrd)
    jal  DMAIn
    nop

    # ---- per-wall emit over the chunk ----
    sw   zero, %lo(ES_idx)
    li   t0, %lo(EOUT_BUF)
    sw   t0, %lo(ES_outptr)

EBatchWallLoop:
    lw   t0, %lo(ES_idx)
    lw   t5, %lo(ES_chunk)
    bge  t0, t5, EBatchChunkNext
    nop
    jal  EmitWall
    nop
    lw   t0, %lo(ES_idx)
    addiu t0, t0, 1
    sw   t0, %lo(ES_idx)
    lw   t1, %lo(ES_outptr)
    addiu t1, t1, BWALL_OUT_BYTES
    sw   t1, %lo(ES_outptr)
    j    EBatchWallLoop
    nop

EBatchChunkNext:
    # advance the RDRAM read cursor + remaining count for the next chunk
    lw   t5, %lo(ES_chunk)
    lw   t1, %lo(ES_outrd)
    sll  t6, t5, 6               # chunk * 80 (64 + 16)
    sll  t7, t5, 4
    addu t6, t6, t7
    addu t1, t1, t6
    sw   t1, %lo(ES_outrd)
    lw   t1, %lo(ES_remain)
    subu t1, t1, t5
    j    EBatchChunkLoop
    sw   t1, %lo(ES_remain)      # (delay)

EBatchDone:
    # Wait for the last triangle's async DMA to drain, then back to the queue engine.
    j    RDPQ_Triangle_Send_End
    li   ra, %lo(RSPQ_Loop)      # (delay)

# ---- EmitWall: emit ONE wall, clipped to the current TMEM T-band -----------------------
# A wall taller than the texture's TMEM cap is drawn across several bands; each band loads
# cap source rows and dispatches B with [band_lo,band_hi). This routine clips the wall's
# T range to the band, lerps the per-corner screen Y to the band edges via the precomputed
# dY/dT slopes (one FixedMul each -- no divide), and emits the band's quad. fits_hw walls
# are the degenerate single-band case (band covers the whole texture -> no clip).
# Corners: V0=(sxA,ytA') V1=(sxB,ytB') V2=(sxA,ybA') V3=(sxB,ybB'); T'=band-clipped texels.
EmitWall:
    sw   ra, %lo(EW_ra)
    lw   t9, %lo(ES_outptr)
    lw   t0, BWO_emit(t9)
    beqz t0, EmitWallRet
    nop

    # ---- clip the wall's [t_top,t_bot] to this band's [band_lo,band_hi) ----
    lw   t0, BWO_ttop(t9)        # t_top
    lw   t1, BWO_tbot(t9)        # t_bot
    lw   t2, %lo(ES_band_lo)
    lw   t3, %lo(ES_band_hi)
    slt  t4, t0, t2              # t_top < band_lo ?
    beqz t4, 1f
    nop
    move t0, t2                  # ct_top = max(t_top, band_lo)
1:  slt  t4, t3, t1              # band_hi < t_bot ?
    beqz t4, 2f
    nop
    move t1, t3                  # ct_bot = min(t_bot, band_hi)
2:  slt  t4, t0, t1              # ct_top < ct_bot ?
    beqz t4, EmitWallRet         # band does not intersect this wall -> skip
    nop
    sw   t0, %lo(EW_Ttop)        # clipped T at band-top edge (period-relative)
    sw   t1, %lo(EW_Tbot)        # clipped T at band-bottom edge
    lw   t5, BWO_ttop(t9)        # t_top
    sub  t6, t0, t5             # dT_top = ct_top - t_top
    sub  t2, t1, t5            # dT_bot = ct_bot - t_top
    sw   t6, %lo(EW_dTtop)
    sw   t2, %lo(EW_dTbot)

    # ---- clipped screen Y per corner: yX' = ytX + dT*slopeX (FixedMulVU clobbers t7-t9) ----
    lw   a3, %lo(EW_dTtop)
    lw   s7, BWO_slopeA(t9)
    jal  FixedMulVU
    nop
    lw   t9, %lo(ES_outptr)
    lw   t0, BWO_ytA(t9)
    addu t0, t0, v1
    sw   t0, %lo(EW_ytA)
    lw   a3, %lo(EW_dTbot)
    lw   s7, BWO_slopeA(t9)
    jal  FixedMulVU
    nop
    lw   t9, %lo(ES_outptr)
    lw   t0, BWO_ytA(t9)
    addu t0, t0, v1
    sw   t0, %lo(EW_ybA)
    lw   a3, %lo(EW_dTtop)
    lw   s7, BWO_slopeB(t9)
    jal  FixedMulVU
    nop
    lw   t9, %lo(ES_outptr)
    lw   t0, BWO_ytB(t9)
    addu t0, t0, v1
    sw   t0, %lo(EW_ytB)
    lw   a3, %lo(EW_dTbot)
    lw   s7, BWO_slopeB(t9)
    jal  FixedMulVU
    nop
    lw   t9, %lo(ES_outptr)
    lw   t0, BWO_ytB(t9)
    addu t0, t0, v1
    sw   t0, %lo(EW_ybB)

    # Per corner: t1=sx t2=sy t3=invw t4=depth + t0=S s0=T (16.16). Y/T are band-clipped.
    # --- stage V0 (top-left): A column, band top ---
    li   t8, %lo(VBUF + 0*EMIT_VTX_SIZE)
    lw   t1, BWO_sxA(t9)
    lw   t2, %lo(EW_ytA)
    lw   t3, BWO_invwA(t9)
    lw   t4, BWO_dA(t9)
    lw   t0, BWO_sA(t9)
    lw   s0, %lo(EW_Ttop)
    jal  StageVtx
    nop
    # --- stage V1 (top-right): B column, band top ---
    li   t8, %lo(VBUF + 1*EMIT_VTX_SIZE)
    lw   t1, BWO_sxB(t9)
    lw   t2, %lo(EW_ytB)
    lw   t3, BWO_invwB(t9)
    lw   t4, BWO_dB(t9)
    lw   t0, BWO_sB(t9)
    lw   s0, %lo(EW_Ttop)
    jal  StageVtx
    nop
    # --- stage V2 (bottom-left): A column, band bottom ---
    li   t8, %lo(VBUF + 2*EMIT_VTX_SIZE)
    lw   t1, BWO_sxA(t9)
    lw   t2, %lo(EW_ybA)
    lw   t3, BWO_invwA(t9)
    lw   t4, BWO_dA(t9)
    lw   t0, BWO_sA(t9)
    lw   s0, %lo(EW_Tbot)
    jal  StageVtx
    nop
    # --- stage V3 (bottom-right): B column, band bottom ---
    li   t8, %lo(VBUF + 3*EMIT_VTX_SIZE)
    lw   t1, BWO_sxB(t9)
    lw   t2, %lo(EW_ybB)
    lw   t3, BWO_invwB(t9)
    lw   t4, BWO_dB(t9)
    lw   t0, BWO_sB(t9)
    lw   s0, %lo(EW_Tbot)
    jal  StageVtx
    nop

    # --- emit triangle 0: V0,V1,V2 ---
    li   a1, %lo(VBUF + 0*EMIT_VTX_SIZE)
    li   a2, %lo(VBUF + 1*EMIT_VTX_SIZE)
    li   a3, %lo(VBUF + 2*EMIT_VTX_SIZE)
    li   a0, EMIT_TRICMD
    li   v0, 2                   # culling disabled
    li   s3, %lo(EMIT_STAGING)
    j    RDPQ_Triangle_Send_Async
    li   ra, %lo(EmitTri1)       # (delay)

EmitTri1:
    # --- emit triangle 1: V1,V3,V2 ---
    li   a1, %lo(VBUF + 1*EMIT_VTX_SIZE)
    li   a2, %lo(VBUF + 3*EMIT_VTX_SIZE)
    li   a3, %lo(VBUF + 2*EMIT_VTX_SIZE)
    li   a0, EMIT_TRICMD
    li   v0, 2
    li   s3, %lo(EMIT_STAGING)
    j    RDPQ_Triangle_Send_Async
    li   ra, %lo(EmitTri2)       # (delay)

EmitTri2:
EmitWallRet:
    lw   ra, %lo(EW_ra)
    jr   ra
    nop

# ---- StageVtx: write one vertex --------------------------------------------------------
# IN: t8=VBUF addr; t1=sx t2=sy t3=invw t4=depth (geometry, 16.16); t0=S, s0=T (16.16
#     texels). Packs X/Y=coord>>14 (s11.2), Z from depth (clamped 32767), S/T as s10.5
#     (16.16>>11 == texel*32, the scale rdpq_triangle applies). Clobbers t5,t6,t7; reads t0,s0.
StageVtx:
    sra  t5, t1, 14
    sh   t5, VTX_ATTR_XY(t8)         # X
    sra  t6, t2, 14
    sh   t6, VTX_ATTR_XY+2(t8)       # Y
    srl  t5, t4, 16                  # depth_mapunits
    li   t6, 32767
    sltu t7, t6, t5                  # 32767 < depth_mapunits ?
    beqz t7, 1f
    nop
    move t5, t6                      # clamp
1:
    sh   t5, VTX_ATTR_Z(t8)          # Z
    # CLIPFLAGS (byte 0x06) = 0: no near/far/guard clip needed (walls are pre-clipped).
    # REJFLAGS (byte 0x07) = 0xFF: the engine CULLS unless (rej1|rej2|rej3)&0x3F == 0x3F
    # (rsp_rdpq_tri.inc:277-279 -- negated trivial-reject = "inside all 6 planes"). A
    # 16-bit 0x00FF store sets clipflags=0, rejflags=0xFF. Writing 0 here (the old code)
    # made EVERY wall triangle trivially-rejected -> the BLACK WALLS.
    li   t6, 0x00FF
    sh   t6, VTX_ATTR_CLIPFLAGS(t8)
    sw   zero, VTX_ATTR_RGBA(t8)
    sra  t5, t0, 11                  # S: 16.16 texels -> s10.5 (texel*32)
    sh   t5, VTX_ATTR_ST(t8)
    sra  t6, s0, 11                  # T: 16.16 texels -> s10.5
    sh   t6, VTX_ATTR_ST+2(t8)
    srl  t5, t4, 16
    sh   t5, VTX_ATTR_Wi(t8)         # Wi
    sh   t4, VTX_ATTR_Wf(t8)         # Wf
    srl  t5, t3, 16
    sh   t5, VTX_ATTR_INVWi(t8)      # INVWi
    jr   ra
    sh   t3, VTX_ATTR_INVWf(t8)      # (delay) INVWf

# ============================================================
# FixedMulVU(a3, s7) -> v1 = (a3 * s7) >> 16, signed 16.16 multiply (the RSP scalar
# core has no multiply). Dual-precision VU partial-product pattern, bit-exact to the
# CPU FixedMul. Copied from rsp_dlwall.S (overlay A) -- B needs it for the per-band
# Y lerp dY = dT * slope. CLOBBERS at, t7,t8,t9, v20..v25; preserves t0-t6,s0-s6,ra.
# ============================================================
    .set noat
FixedMulVU:
    sw   a3, %lo(VSCRATCH)+0     # a : [ah][al]
    sw   s7, %lo(VSCRATCH)+4     # b : [bh][bl]
    li   t9, %lo(VSCRATCH)
    lsv  $v20, 0, 0, t9          # ah (signed high half of a)
    lsv  $v21, 0, 2, t9          # al (low half)
    lsv  $v22, 0, 4, t9          # bh
    lsv  $v23, 0, 6, t9          # bl
    vmudl $v24, $v21, $v23       # al*bl (unsigned low)
    vmadm $v24, $v20, $v23       # + ah*bl
    vmadn $v24, $v21, $v22       # + al*bh -> fractional lane
    vmadh $v25, $v20, $v22       # + ah*bh -> integer lane
    ssv  $v24, 0, 0, t9          # rF (low 16)
    ssv  $v25, 0, 2, t9          # rI (high 16)
    lh   t7, %lo(VSCRATCH)+2     # rI (signed)
    lhu  t8, %lo(VSCRATCH)+0     # rF (unsigned)
    sll  t7, t7, 16
    jr   ra
    or   v1, t7, t8             # (delay) v1 = (rI<<16)|rF = FixedMul result 16.16
    .set at

#endif /* BENCH_FORCE_MESH_RSP_EMIT */
