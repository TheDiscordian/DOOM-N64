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
# PHASE 0 -- DMA LOOPBACK PROBE (no transform math). This overlay exposes a
# single command that de-risks the three things the real transform depends on
# BEFORE any VU math is written:
#   1. overlay registration (DEFINE_RSP_UCODE + rspq_overlay_register),
#   2. 8-byte-aligned RDRAM<->DMEM DMA of bake_wall_t records,
#   3. CPU/RSP cache writeback/invalidate coherency.
#
# DLWallCmd_Loopback DMAs a block of bytes from RDRAM src into a DMEM scratch
# buffer, then DMAs that scratch straight back out to a separate RDRAM dst.
# The CPU then memcmp's dst vs src and must see a byte-identical round-trip.
#
# Command words (rspq_write(ovl_id, 0, a0, a1, a2)):
#   a0: low 24 bits = DMA size word for height==1 (== byte_count - 1). The CPU
#       pre-encodes it so the RSP needs no arithmetic. Capped to <= LOOP_SCRATCH
#       bytes (Phase 0 keeps the batch small + 8-byte aligned).
#   a1: RDRAM source address (cache-flushed by the CPU before the command).
#   a2: RDRAM destination address (invalidated by the CPU after rspq_wait).
########################################################################

    .set noreorder
    .set at

    .data

RSPQ_BeginOverlayHeader
    RSPQ_DefineCommand DLWallCmd_Loopback, 12   # 0x00: (size, src, dst) = 3 words
RSPQ_EndOverlayHeader

# The overlay needs no persisted state across commands, but rsp_queue.inc
# requires a non-empty saved-state region OR RSPQ_EmptySavedState. Phase 0 has
# nothing to persist, so declare the saved state empty.
RSPQ_EmptySavedState

    .bss

# DMEM scratch the loopback DMAs through. Must be 8-byte aligned (DMA hardware
# treats DMEM/RDRAM addresses as 8-byte aligned). DMEM (ram_data) is only 4 KiB
# total and is SHARED with the rspq engine's own data + this overlay's header /
# saved state, so the scratch must stay small. 1 KiB holds 36 bake_wall_t (28B)
# records -- ample for the Phase 0 probe -- and is well inside the 12-bit DMA width.
#define LOOP_SCRATCH 1024
    .align 3
LOOP_BUF: .ds.b LOOP_SCRATCH

    .text

DLWallCmd_Loopback:
    # a0: DMA size word (byte_count-1) in the low 24 bits. Mask off the command
    #     id byte that rspq packs into the MSB so t0 is a clean width/height word
    #     (height==1, so the high 12 bits are 0 and pitch is ignored). t0 (size)
    #     survives DMAIn -- DMAExec's only DESTROY regs are t2 + at.
    and  t0, a0, 0xFFFFFF

    # ---- DMA in: RDRAM src (a1) -> DMEM LOOP_BUF ----
    # jal DMAIn returns here (ra = the next insn) and blocks until the RDRAM->DMEM
    # transfer is done. This CLOBBERS ra, but that is fine: only the FINAL DMA below
    # restores ra to RSPQ_Loop and tail-jumps there (the rsp_yuv.S multi-DMA pattern:
    # intermediate `jal DMAIn`, final `jal_and_j DMAOut, RSPQ_Loop`). a1/a2 are not
    # touched by DMAExec, so a2 (the dst) is still live after the call.
    li   s4, %lo(LOOP_BUF)       # DMEM scratch address
    jal  DMAIn                   # RDRAM(a1)->DMEM, waits for completion
    move s0, a1                  # (delay slot) s0 = RDRAM source address

    # ---- DMA out: DMEM LOOP_BUF -> RDRAM dst (a2) ----
    # t0 (size) is untouched by DMAIn; s4 was advanced by the src unalignment offset,
    # but the CPU guarantees 8-byte-aligned buffers (offset 0) so it is still LOOP_BUF.
    # Reset it explicitly to be safe. The final DMA tail-jumps DMAOut with ra pinned to
    # RSPQ_Loop, so DMAOut's `jr ra` returns control to the queue engine.
    li   s4, %lo(LOOP_BUF)       # DMEM scratch address
    move s0, a2                  # RDRAM destination address
    j    DMAOut                  # DMEM->RDRAM(a2); ends in `jr ra`
    li   ra, %lo(RSPQ_Loop)      # (delay slot) return to the queue engine, not into us
