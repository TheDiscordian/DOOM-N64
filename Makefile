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
endif
# BENCH_MARKS=1: frame-keyed visual-capture markers (BENCH_MARK frame=N via
# ISViewer every 256 retained frames) for exactly-paired cross-build
# screenshot series. Visual-capture builds only -- never timing builds, the
# debugf cost would skew the numbers.
ifeq ($(BENCH_MARKS),1)
CFLAGS += -DN64_BENCH_MARKS=1
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
