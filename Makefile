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

DOOM_PLATFORM_SRCS = \
	$(DOOM_SRC)/i_main_n64.c \
	$(DOOM_SRC)/i_wad_browser_n64.c \
	$(DOOM_SRC)/i_system_n64.c \
	$(DOOM_SRC)/i_video_n64.c \
	$(DOOM_SRC)/i_sound_n64.c \
	$(DOOM_SRC)/i_net_n64.c

DOOM_SRCS = $(DOOM_COMMON_SRCS) $(DOOM_PLATFORM_SRCS)
OBJS = $(DOOM_SRCS:%.c=$(BUILD_DIR)/%.o)

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

WAD_ASSETS_LOWER = $(wildcard WADs/*.wad)
WAD_ASSETS_UPPER = $(wildcard WADs/*.WAD)

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
	for src in WADs/*.wad WADs/*.WAD; do \
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
