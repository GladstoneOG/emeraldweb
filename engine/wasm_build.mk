# Standalone Emscripten WASM Build Driver for pokemonweb (pokeemerald-expansion)
# Designed to run inside WSL2 Ubuntu/Debian with ~/.emsdk activated.
# Leaves upstream GBA Makefile untouched.

SHELL := /bin/bash
.SHELLFLAGS := -o pipefail -c
MAKEFLAGS += --no-print-directory

GAME_VERSION ?= EMERALD
MAP_VERSION ?= emerald
BUILD_DIR := build/wasm
OBJ_DIR := $(BUILD_DIR)
ASSETS_DIR_NAME := build/assets
C_SUBDIR := src
ASM_SUBDIR := asm
DATA_SRC_SUBDIR := src/data
DATA_ASM_SUBDIR := data
MID_SUBDIR := sound/songs/midi
SONG_SUBDIR := sound/songs
C_BUILDDIR := $(OBJ_DIR)/$(C_SUBDIR)
DATA_ASM_BUILDDIR := $(OBJ_DIR)/arm_data
DATA_WASM_BUILDDIR := $(OBJ_DIR)/wasm_data
MID_BUILDDIR := $(OBJ_DIR)/arm_midi
MID_WASM_BUILDDIR := $(OBJ_DIR)/wasm_midi
SONG_BUILDDIR := $(OBJ_DIR)/$(SONG_SUBDIR)
INCLUDE_DIRS := include
WASM_PLAT_DIR := platform/wasm

TOOLS_DIR := tools
BIN2C := $(TOOLS_DIR)/bin2c/bin2c
GBAGFX := $(TOOLS_DIR)/gbagfx/gbagfx
GFX := $(GBAGFX)
JSONPROC := $(TOOLS_DIR)/jsonproc/jsonproc
MAPJSON := $(TOOLS_DIR)/mapjson/mapjson
MID2AGB := $(TOOLS_DIR)/mid2agb/mid2agb
MID := $(MID2AGB)
PREPROC := $(TOOLS_DIR)/preproc/preproc
RAMSCRGEN := $(TOOLS_DIR)/ramscrgen/ramscrgen
RSFONT := $(TOOLS_DIR)/rsfont/rsfont
SCANINC := $(TOOLS_DIR)/scaninc/scaninc
TRAINERPROC := $(TOOLS_DIR)/trainerproc/trainerproc
WAV2AGB := $(TOOLS_DIR)/wav2agb/wav2agb
SMOL := $(TOOLS_DIR)/compresSmol/compresSmol
SMOLTM := $(TOOLS_DIR)/compresSmol/compresSmolTilemap
COMPRESSMOL := $(SMOL)
ELF_TO_C := $(TOOLS_DIR)/wasm_elf_to_c.py

AS := arm-none-eabi-as
ASFLAGS := -mcpu=arm7tdmi -march=armv4t -meabi=5 --defsym MODERN=1 --defsym $(GAME_VERSION)=1
EMCC := emcc
# GNU cpp is used by map_data_rules.mk (.s) and trainer_rules.mk (-traditional-cpp);
# C translation units are preprocessed explicitly with $(EMCC) -E for wasm32 ILP32 <stdint.h>.
CPP := cpp
CPPFLAGS := -iquote include -I include -Wno-trigraphs -DMODERN=1 -DTESTING=0 -D$(GAME_VERSION) -DRELEASE
PYTHON := python3

WASM_USE_FIBER ?= 1

WASM_COMMON_FLAGS := -O2 -Wall -Wno-missing-field-initializers -std=gnu17 -DWASM=1 -DWASM_PLATFORM=1 -DMODERN=1 \
                     -DEMERALD -DTESTING=0 -DRELEASE \
                     -DWASM_USE_FIBER=$(WASM_USE_FIBER) \
                     -iquote include -iquote $(WASM_PLAT_DIR) -iquote $(WASM_PLAT_DIR)/include -iquote src/mp \
                     -Wno-trigraphs -Wno-strict-aliasing -Wno-pointer-to-int-cast -Wno-unused-parameter \
                     -Wno-ignored-qualifiers -Wno-gnu-alignof-expression -Wno-incompatible-function-pointer-types \
                     -Wno-unused-const-variable -Wno-unused-variable -Wno-unused-function

WASM_CPP_FLAGS := -DWASM=1 -DWASM_PLATFORM=1 -DMODERN=1 -DEMERALD -DTESTING=0 -DRELEASE \
                  -DWASM_USE_FIBER=$(WASM_USE_FIBER) \
                  -iquote include -iquote $(WASM_PLAT_DIR) -iquote $(WASM_PLAT_DIR)/include -iquote src/mp \
                  -I tools/agbcc/include -Wno-trigraphs

# emscripten_fiber_* requires -sASYNCIFY under Emscripten. For minimal binary
# experiments, set WASM_USE_FIBER=0 (WasmRunFrame frame-stepper).
ifeq ($(WASM_USE_FIBER),1)
WASM_FIBER_FLAGS := -sASYNCIFY -sASYNCIFY_STACK_SIZE=16384
else
WASM_FIBER_FLAGS :=
endif

WASM_LINK_FLAGS := -sUSE_SDL=0 \
                   $(WASM_FIBER_FLAGS) \
                   -sSUPPORT_LONGJMP=emscripten \
                   -sALLOW_MEMORY_GROWTH=1 \
                   -sINITIAL_MEMORY=67108864 \
                   -sMAXIMUM_MEMORY=268435456 \
                   -sSTACK_SIZE=2097152 \
                   -sEXPORTED_FUNCTIONS="['_main','_malloc','_free','_mp_set_speed_multiplier','_mp_ingest_packet','_mp_trigger_warp','_mp_start_synced_battle','_mp_update_peer_meta','_mp_kill_linked_pokemon','_mp_import_soullink_json','_mp_kill_mon_by_uid','_mp_on_trade_commit','_mp_on_snapshot_loaded','_mp_save_game_now','_mp_test_query','_mp_test_action','_wasm_step_frames','_wasm_get_framebuffer_ptr','_wasm_set_input_mask','_wasm_set_gamepad_mask','_wasm_get_flash_ptr']" \
                   -sEXPORTED_RUNTIME_METHODS="['HEAPU8','HEAP16','UTF8ToString','stringToUTF8','lengthBytesUTF8']"

SMOKE_SRCS := $(WASM_PLAT_DIR)/wasm_main.c \
              $(WASM_PLAT_DIR)/wasm_mmio.c \
              $(WASM_PLAT_DIR)/wasm_ppu.c \
              $(WASM_PLAT_DIR)/wasm_audio.c \
              $(WASM_PLAT_DIR)/wasm_bios_dma.c \
              $(WASM_PLAT_DIR)/wasm_input_flash.c \
              $(WASM_PLAT_DIR)/wasm_mp_bridge.c

AUTO_GEN_TARGETS :=
WILD_ENCOUNTERS_TOOL_DIR := tools/wild_encounters
MISC_TOOL_DIR := tools/misc
LEARNSET_HELPERS_DIR := tools/learnset_helpers
LEARNSET_HELPERS_DATA_DIR := $(LEARNSET_HELPERS_DIR)/porymoves_files
LEARNSET_HELPERS_BUILD_DIR := $(LEARNSET_HELPERS_DIR)/build
ALL_LEARNABLES_JSON := $(DATA_SRC_SUBDIR)/pokemon/all_learnables.json
ALL_TUTORS_JSON := $(LEARNSET_HELPERS_BUILD_DIR)/all_tutors.json
ALL_TEACHING_TYPES_JSON := $(LEARNSET_HELPERS_BUILD_DIR)/all_teaching_types.json

AUTO_GEN_TARGETS += $(DATA_SRC_SUBDIR)/wild_encounters.h
AUTO_GEN_TARGETS += $(INCLUDE_DIRS)/constants/script_commands.h

include map_data_rules.mk
include json_data_rules.mk
include trainer_rules.mk
include spritesheet_rules.mk
include audio_rules.mk
include graphics_file_rules.mk

# Terminal asset extensions and conversion pattern rules matching upstream Makefile
%.s:   ;
%.png: ;
%.pal: ;
%.wav: ;

%.1bpp:      %.png  ; $(GFX) $< $@
%.4bpp:      %.png  ; $(GFX) $< $@
%.8bpp:      %.png  ; $(GFX) $< $@
%.gbapal:    %.pal  ; $(GFX) $< $@
%.gbapal:    %.png  ; $(GFX) $< $@
%.lz:        %      ; $(GFX) $< $@
%.smolTM:    %      ; $(SMOLTM) $< $@
%.fastSmol:  %      ; $(SMOL) -w $< $@ false false false
%.smol:      %      ; $(SMOL) -w $< $@
%.rl:        %      ; $(GFX) $< $@
%.latfont:   %.png  ; $(GFX) $< $@
%.hwjpnfont: %.png  ; $(GFX) $< $@
%.fwjpnfont: %.png  ; $(GFX) $< $@

$(DATA_SRC_SUBDIR)/wild_encounters.h: $(DATA_SRC_SUBDIR)/wild_encounters.json $(WILD_ENCOUNTERS_TOOL_DIR)/wild_encounters_to_header.py $(INCLUDE_DIRS)/config/overworld.h $(INCLUDE_DIRS)/config/dexnav.h
	$(PYTHON) $(WILD_ENCOUNTERS_TOOL_DIR)/wild_encounters_to_header.py

$(INCLUDE_DIRS)/constants/script_commands.h: $(MISC_TOOL_DIR)/make_scr_cmd_constants.py $(DATA_ASM_SUBDIR)/script_cmd_table.inc
	$(PYTHON) $(MISC_TOOL_DIR)/make_scr_cmd_constants.py

$(LEARNSET_HELPERS_BUILD_DIR):
	@mkdir -p $@

$(ALL_LEARNABLES_JSON):
	$(PYTHON) $(LEARNSET_HELPERS_DIR)/make_learnables.py $(LEARNSET_HELPERS_DATA_DIR) $@

$(ALL_TUTORS_JSON): $(shell find data/ -type f -name '*.inc') $(LEARNSET_HELPERS_DIR)/make_tutors.py | $(LEARNSET_HELPERS_BUILD_DIR)
	$(PYTHON) $(LEARNSET_HELPERS_DIR)/make_tutors.py $@

$(ALL_TEACHING_TYPES_JSON): $(wildcard $(DATA_SRC_SUBDIR)/pokemon/species_info/*_families.h) $(LEARNSET_HELPERS_DIR)/make_teaching_types.py | $(LEARNSET_HELPERS_BUILD_DIR)
	$(PYTHON) $(LEARNSET_HELPERS_DIR)/make_teaching_types.py $@

TEACHABLE_DEPS := $(ALL_LEARNABLES_JSON) $(INCLUDE_DIRS)/constants/tms_hms.h $(INCLUDE_DIRS)/config/pokemon.h $(DATA_SRC_SUBDIR)/pokemon/special_movesets.json $(INCLUDE_DIRS)/config/pokedex_plus_hgss.h $(LEARNSET_HELPERS_DIR)/make_teachables.py

$(DATA_SRC_SUBDIR)/pokemon/teachable_learnsets.h: $(TEACHABLE_DEPS) | $(ALL_TUTORS_JSON) $(ALL_TEACHING_TYPES_JSON)
	$(PYTHON) $(LEARNSET_HELPERS_DIR)/make_teachables.py $(LEARNSET_HELPERS_BUILD_DIR)

$(DATA_SRC_SUBDIR)/tutor_moves.h: $(DATA_SRC_SUBDIR)/pokemon/special_movesets.json | $(ALL_TUTORS_JSON)
	$(PYTHON) $(LEARNSET_HELPERS_DIR)/make_teachables.py --tutors $(LEARNSET_HELPERS_BUILD_DIR)

C_SRCS_IN := $(wildcard $(C_SUBDIR)/*.c $(C_SUBDIR)/*/*.c $(C_SUBDIR)/*/*/*.c)
C_SRCS := $(filter-out $(C_SUBDIR)/test/%,$(foreach src,$(C_SRCS_IN),$(if $(findstring .inc.c,$(src)),,$(src))))
C_OBJS := $(patsubst $(C_SUBDIR)/%.c,$(C_BUILDDIR)/%.o,$(C_SRCS))
C_DEPS := $(patsubst $(C_SUBDIR)/%.c,$(C_BUILDDIR)/%.d,$(C_SRCS))

PLAT_SRCS := $(wildcard $(WASM_PLAT_DIR)/*.c)
PLAT_OBJS := $(patsubst $(WASM_PLAT_DIR)/%.c,$(OBJ_DIR)/$(WASM_PLAT_DIR)/%.o,$(PLAT_SRCS))

DATA_ASM_SRCS := $(wildcard $(DATA_ASM_SUBDIR)/*.s)
DATA_ASM_DEPS := $(patsubst $(DATA_ASM_SUBDIR)/%.s,$(DATA_ASM_BUILDDIR)/%.d,$(DATA_ASM_SRCS))
DATA_WASM_OBJS := $(patsubst $(DATA_ASM_SUBDIR)/%.s,$(DATA_WASM_BUILDDIR)/%.o,$(DATA_ASM_SRCS))
ABS_PROVIDER_ELFS := $(DATA_ASM_BUILDDIR)/event_scripts.arm.o $(DATA_ASM_BUILDDIR)/battle_scripts_1.arm.o

MID_SRCS := $(wildcard $(MID_SUBDIR)/*.mid)
MID_WASM_OBJS := $(patsubst $(MID_SUBDIR)/%.mid,$(MID_WASM_BUILDDIR)/%.o,$(MID_SRCS))

ALL_WASM_OBJS := $(C_OBJS) $(PLAT_OBJS) $(DATA_WASM_OBJS) $(MID_WASM_OBJS)

.PHONY: all wasm link-wasm wasm-smoke host-tools generated clean check-env

all: wasm

check-env:
	@command -v $(EMCC) >/dev/null || { echo "[wasm_build] ERROR: emcc not found (source ~/.emsdk/emsdk_env.sh)"; exit 1; }
	@command -v $(AS) >/dev/null || { echo "[wasm_build] ERROR: arm-none-eabi-as not found"; exit 1; }
	@command -v $(PYTHON) >/dev/null || { echo "[wasm_build] ERROR: python3 not found"; exit 1; }

host-tools:
	@$(MAKE) -f make_tools.mk tools

generated: host-tools $(AUTO_GEN_TARGETS) $(DATA_SRC_SUBDIR)/pokemon/teachable_learnsets.h $(DATA_SRC_SUBDIR)/tutor_moves.h
	@echo "[wasm_build] Generated headers/data ready."

# Standalone platform shim smoke binary (Canvas + Fiber + PPU + BIOS + _mp_* bridge)
wasm-smoke: check-env
	@mkdir -p $(BUILD_DIR)
	$(EMCC) $(WASM_COMMON_FLAGS) -DWASM_SMOKE_TEST=1 $(SMOKE_SRCS) \
		$(WASM_LINK_FLAGS) \
		-o $(BUILD_DIR)/smoke.js
	@echo "[wasm_build] Smoke binary ready: $(BUILD_DIR)/smoke.js + $(BUILD_DIR)/smoke.wasm"

# Explicit generated-header dependencies matching upstream Makefile
$(C_BUILDDIR)/move_relearner.o: $(C_SUBDIR)/move_relearner.c $(DATA_SRC_SUBDIR)/tutor_moves.h
$(C_BUILDDIR)/pokemon.o: $(C_SUBDIR)/pokemon.c $(DATA_SRC_SUBDIR)/pokemon/teachable_learnsets.h
$(DATA_ASM_BUILDDIR)/maps.arm.o: $(DATA_ASM_SUBDIR)/maps.s $(LAYOUTS_DIR)/layouts.inc $(LAYOUTS_DIR)/layouts_table.inc $(MAPS_DIR)/headers.inc $(MAPS_DIR)/groups.inc $(MAPS_DIR)/connections.inc $(MAP_CONNECTIONS) $(MAP_HEADERS)
$(DATA_ASM_BUILDDIR)/map_events.arm.o: $(DATA_ASM_SUBDIR)/map_events.s $(MAPS_DIR)/events.inc $(MAP_EVENTS)

$(C_BUILDDIR)/%.d: $(C_SUBDIR)/%.c
	@mkdir -p $(dir $@)
	$(SCANINC) -M $@ -g $(ASSETS_DIR_NAME) -I include -I $(WASM_PLAT_DIR) -I $(WASM_PLAT_DIR)/include -I src/mp -I tools/agbcc/include $<

$(C_BUILDDIR)/%.o: $(C_SUBDIR)/%.c
	@mkdir -p $(dir $@)
	@$(EMCC) -E $(WASM_CPP_FLAGS) $< | $(PREPROC) -i -g $(ASSETS_DIR_NAME) $< charmap.txt | $(EMCC) $(WASM_COMMON_FLAGS) -x c - -c -o $@

$(OBJ_DIR)/$(WASM_PLAT_DIR)/%.o: $(WASM_PLAT_DIR)/%.c
	@mkdir -p $(dir $@)
	$(EMCC) $(WASM_COMMON_FLAGS) -c $< -o $@

$(DATA_ASM_BUILDDIR)/%.d: $(DATA_ASM_SUBDIR)/%.s
	@mkdir -p $(dir $@)
	$(SCANINC) -M $(patsubst %.d,%.arm.d,$@) -g $(ASSETS_DIR_NAME) -I include -I "" $<
	@mv -f $(patsubst %.d,%.arm.d,$@) $@

$(DATA_ASM_BUILDDIR)/%.arm.o: $(DATA_ASM_SUBDIR)/%.s
	@mkdir -p $(dir $@)
	@$(PREPROC) -s $< charmap.txt | $(CPP) $(CPPFLAGS) - | $(PREPROC) -ie $< charmap.txt | $(AS) $(ASFLAGS) -o $@

$(BUILD_DIR)/func_syms.txt: $(C_OBJS) $(PLAT_OBJS)
	@mkdir -p $(dir $@)
	@printf '%s\n' $^ > $(BUILD_DIR)/c_objs.rsp
	@LLVM_NM="$$(command -v llvm-nm 2>/dev/null || echo "$$(dirname "$$(command -v $(EMCC))")/../bin/llvm-nm")"; \
	"$$LLVM_NM" --defined-only -g @$(BUILD_DIR)/c_objs.rsp \
		| awk 'NF>=3 && ($$2=="T" || $$2=="t" || $$2=="W" || $$2=="w") {print $$3}' \
		| sort -u > $@

$(DATA_WASM_BUILDDIR)/%.wasm.c: $(DATA_ASM_BUILDDIR)/%.arm.o $(ABS_PROVIDER_ELFS) $(BUILD_DIR)/func_syms.txt $(ELF_TO_C)
	@mkdir -p $(dir $@)
	$(PYTHON) $(ELF_TO_C) $< $@ --unit data_$* --abs-from $(ABS_PROVIDER_ELFS) --funcs-file $(BUILD_DIR)/func_syms.txt

$(DATA_WASM_BUILDDIR)/%.o: $(DATA_WASM_BUILDDIR)/%.wasm.c
	@mkdir -p $(dir $@)
	$(EMCC) -O2 -Wall -Wno-unused-const-variable -c $< -o $@

$(MID_WASM_BUILDDIR)/%.arm.o: $(MID_ASM_DIR)/%.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -I sound -o $@ $<

$(MID_WASM_BUILDDIR)/%.wasm.c: $(MID_WASM_BUILDDIR)/%.arm.o $(BUILD_DIR)/func_syms.txt $(ELF_TO_C)
	@mkdir -p $(dir $@)
	$(PYTHON) $(ELF_TO_C) $< $@ --unit mid_$* --funcs-file $(BUILD_DIR)/func_syms.txt

$(MID_WASM_BUILDDIR)/%.o: $(MID_WASM_BUILDDIR)/%.wasm.c
	@mkdir -p $(dir $@)
	$(EMCC) -O2 -Wall -Wno-unused-const-variable -c $< -o $@

$(BUILD_DIR)/index.js: $(ALL_WASM_OBJS)
	@mkdir -p $(dir $@)
	@printf '%s\n' $(ALL_WASM_OBJS) > $(BUILD_DIR)/all_objs.rsp
	$(EMCC) -O2 @$(BUILD_DIR)/all_objs.rsp $(WASM_LINK_FLAGS) -o $@
	@echo "[wasm_build] Full engine WASM ready: $@ + $(BUILD_DIR)/index.wasm"

ifeq ($(filter link-wasm,$(MAKECMDGOALS)),link-wasm)
-include $(C_DEPS) $(DATA_ASM_DEPS)
endif

link-wasm: $(BUILD_DIR)/index.js

# Stage 1: build native host tools; Stage 2: generate headers/map data; Stage 3: scan deps & link full WASM
wasm: check-env
	@$(MAKE) -f wasm_build.mk host-tools
	@$(MAKE) -f wasm_build.mk generated
	@$(MAKE) -f wasm_build.mk link-wasm

clean:
	rm -rf $(BUILD_DIR)
