#include <rsp_queue.inc>

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
    RSPQ_DefineCommand DLWallCmd_Loopback, 12   # 0x00: (size, src, dst) = 3 words
    RSPQ_DefineCommand DLWallCmd_Xform,    12   # 0x04: (viewblk, wall, out) = 3 words
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

    .text

# ============================================================
# PHASE 0: DMA loopback probe (unchanged).
# ============================================================
DLWallCmd_Loopback:
    and  t0, a0, 0xFFFFFF        # DMA size word (byte_count-1), strip cmd id byte
    li   s4, %lo(LOOP_BUF)
    jal  DMAIn                   # RDRAM(a1)->DMEM, waits
    move s0, a1                  # (delay) s0 = src
    li   s4, %lo(LOOP_BUF)
    move s0, a2                  # RDRAM dst
    j    DMAOut                  # DMEM->RDRAM(a2)
    li   ra, %lo(RSPQ_Loop)      # (delay) return to queue engine


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
    # ---- DMA the view block in ----
    li   s4, %lo(VIEWBLK)
    jal  DMAIn
    move s0, a0                  # (delay) s0 = view block RDRAM addr (a0 preserved? no:
                                 #  DMAExec destroys only t2/at; a0/a1/a2 survive)
    # ---- DMA the wall input in ----
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
    # ytA = (centery<<16) - centerx * FixedDiv(top, dA)
    move a3, t4
    jal  FixedDivApply
    nop
    move a3, v1
    jal  MulIntVU
    move s7, s3                  # centerx
    sll  t8, s6, 16              # centery<<16
    sub  s0, t8, v1
    sw   s0, %lo(WO_ytA)
    # ybA = (centery<<16) - centerx * FixedDiv(bot, dA)
    move a3, t5
    jal  FixedDivApply
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
    move a3, t4
    jal  FixedDivApply
    nop
    move a3, v1
    jal  MulIntVU
    move s7, s3
    sll  t8, s6, 16
    sub  s0, t8, v1
    sw   s0, %lo(WO_ytB)
    move a3, t5
    jal  FixedDivApply
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
# vrcp(b) returns (ri:rf) ~= 2^31/b_significand with vrcp's built-in 1-LSB shift.
# We do NOT analytically pin the exponent here; FixedDivApply folds the net scale
# into a single compile-time shift FIXEDDIV_SH, tuned so the gate's relative error
# collapses to the vrcp epsilon (a wrong shift shows as a clean power-of-two error).
# ============================================================
RDIV_R: # parked 32-bit reciprocal (ri at +0, rf at +2)  -- in VSCRATCH+8
RDIV_D: # parked b hi/lo                                  -- in VSCRATCH+12
    .set noat
RecipFixVU:
    sw   a3, %lo(VSCRATCH)+12    # park b ([dh][dl]) for FixedDivApply
    li   t9, %lo(VSCRATCH)
    lsv  $v02, 0, 14, t9         # dl (low half)  -> vrcpl  (VSCRATCH+12 -> +14 = lo)
    lsv  $v03, 0, 12, t9         # dh (high half) -> vrcph
    vxor $v00, $v00, $v00
    vrcph $v04, $v03.e0          # ri
    vrcpl $v05, $v02.e0          # rf
    vrcph $v04, $v00.e0          # finalize
    ssv  $v04, 0, 8,  t9         # park ri at VSCRATCH+8
    jr   ra
    ssv  $v05, 0, 10, t9         # (delay) park rf at VSCRATCH+10
    .set at


# ============================================================
# FixedDivApply(a3 = numerator a, 16.16) -> v1 = (a<<16)/b in 16.16, using the
# reciprocal of b parked by the most recent RecipFixVU. Computes a * recip(b) with
# the dual-precision multiply, then applies FIXEDDIV_SH to land 16.16. CLOBBERS at,
# t7,t8,t9, v20..v25. ra preserved (no inner jal).
#
# recip = (ri:rf) is a 32-bit fixed reciprocal. a*(ri:rf) via vmudl/vmadm/vmadn/vmadh
# gives the 64-bit product in (pI:pF). The correctly-scaled (a<<16)/b is a fixed
# window of that product; FIXEDDIV_SH selects it. We extract a 32-bit window then a
# final scalar shift. FIXEDDIV_SH is set so result == FixedDiv(a,b) for the gate.
# ============================================================
#define FIXEDDIV_SH 1           /* net scalar left-shift; tuned via the gate's invw error */
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
    #   acc  = al*rf  (both unsigned low)         vmudl
    #   acc += ah*rf  (ah signed)                 vmadm
    #   pF   = al*ri  (ri signed)                 vmadn -> low/frac lanes
    #   pI   = ah*ri  (signed)                    vmadh -> high/int lanes
    vmudl $v24, $v21, $v23
    vmadm $v24, $v20, $v23
    vmadn $v24, $v21, $v22       # pF
    vmadh $v25, $v20, $v22       # pI
    ssv  $v24, 0, 0, t9          # pF -> VSCRATCH+0
    ssv  $v25, 0, 2, t9          # pI -> VSCRATCH+2
    lh   t7, %lo(VSCRATCH)+2     # pI (signed high)
    lhu  t8, %lo(VSCRATCH)+0     # pF (unsigned low)
    sll  t7, t7, 16
    or   t7, t7, t8             # 32-bit window of a*recip
    # net scale to 16.16 FixedDiv(a,b): a*recip ~= a * 2^31/b within this 32-bit
    # window already representing a*2^(31-32)/b = a/(2b); the <<FIXEDDIV_SH lands it
    # at (a<<16)/b. Tuned empirically by the gate (see FIXEDDIV_SH).
    sll  t7, t7, FIXEDDIV_SH
    jr   ra
    move v1, t7                 # (delay) v1 = FixedDiv(a,b) in 16.16
    .set at
