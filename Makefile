# OpenTracker firmware for the original Polyend Tracker hardware (MK66FX1M0).
# make            -> build/<profile>/opentracker.hex and PolyendTrackerOT<variant><ver digits>.ptf (24 chars)
# PROFILE=full    -> the modules of profiles/<name>.toml (full = synth engines, core = none);
#                    MODULES_DIR = the module repository (default ../../opentracker-synths), profile.mk
# make SELFTEST=1 -> build/selftest/PolyendTracker_OTst<DIAG_N>.ptf (24 chars)
# make host-test  -> build and run host-side unit tests
# make perf-gate  -> bench-m4 instruction counts vs tests/perf_baseline.txt (+5 % max), engine
#                    limits, flash <= 800,000 B, host bench, telemetry thresholds on LOG=<device log>
#                    (docs/HARDWARE_TESTING.md)
# OT_SDRAM_CACHE=0 -> image without the LMEM system cache for the SDRAM (build dir build/nocache)
# OT_FASTRUN=0    -> image with all audio code in flash (no .fastrun in SRAM_L; build dir build/nofastrun)
# OT_USB_MSC=0    -> image without the USB mass-storage interface (old MIDI + Serial descriptors;
#                    build dir build/nomsc)
# make testsongs  -> build/testsongs/Projects/<name>/ (2.x hardware-test projects, TESTSONGS.md)
# make patches    -> build/patches/Patches/<MODEL>/ (synth preset pack, tools/make_patches/PRESETS.md)
# make budget     -> the profile's declared module budgets (flash, SRAM) and their limits
# make card          -> [I] the profile's card modules as build/<profile>/otm/<name>.otm (fork #21)
# make hot-rows      -> [I] tools/hot_rows/*.hot, the synths' hot code for the fast block (fork #21)
# make otm OTM_MODULE=<path> -> [I] build/<profile>/otm/<name>.otm, a module loadable from the card
#                    (<path> as in a profile, relative to MODULES_DIR; tools/make_otm.py)
# make otm-test   -> [I] tools/make_otm.py's images against the linker's (ARM toolchain, pyelftools)
# make fonts      -> regenerate the committed UI font image (src/core/ui/fonts/font_data.*,
#                    the firmware's packed font_data_z.inc / font_adv.inc,
#                    sim/third_party/rom_fonts.inc); needs python3 + Pillow
VERSION   ?= 3.0.0
ROOT      := .
include profile.mk
BUILD     := build/$(PROFILE)
# Image file names must be exactly 24 characters (like the stock
# "PolyendTracker_1.9.2.ptf"): the device's update bootloader rejected a
# 45-character name and accepted a 24-character one (docs/HARDWARE_TESTING.md).
# DIAG_N numbers successive diagnostic selftest images (one digit).
DIAG_N    ?= 3
VERNUM    := $(subst .,,$(VERSION))
# make SELFTEST=1 builds a hardware bring-up image that always runs the selftest.
ifeq ($(SELFTEST),1)
BUILD     := build/selftest
DEFS_EXTRA := -DOT_FORCE_SELFTEST=1
PTF_NAME  := PolyendTracker_OTst$(DIAG_N).ptf
else
PTF_NAME  := PolyendTrackerOT$(OT_PROFILE_VARIANT)$(VERNUM).ptf
endif
# LMEM system cache for the SDRAM (src/hal/sdram_cache.h): 1 = on (boot self-test
# can still turn it off), 0 = never enabled. Own build dir so objects never mix.
OT_SDRAM_CACHE ?= 1
ifeq ($(OT_SDRAM_CACHE),0)
BUILD     := $(BUILD)/nocache
endif
# Hot audio code in SRAM_L (src/core/fastrun.h, docs/OPTIMIZATION_2X.md item 6):
# 1 = on (2.x default), 0 = everything runs from flash. Own build dir.
OT_FASTRUN ?= 1
ifeq ($(OT_FASTRUN),0)
BUILD     := $(BUILD)/nofastrun
endif
# [I] #21 shared-pool measurement images (profiles/pooltest.toml): 1 = a 12 KB
# SRAM_L block for card modules' fast parts, card modules replace built-ins
# with the same id, stack floor 15 KB, pool cap 48 KB, module log every 10 s.
# Never for releases.
OT_POOL_TEST ?= 0
OTM_FAST_BYTES := $(if $(filter 1,$(OT_POOL_TEST)),12288,0)
ifeq ($(OT_POOL_TEST),1)
BUILD     := $(BUILD)/pooltest
endif
# USB Storage mode (docs/PLAN_2X.md, Tracker Mini manual §13.10): 1 = the USB
# device adds a mass-storage interface (third_party/teensy3/usb_msc.h) next to
# MIDI + Serial; 0 = the old MIDI + Serial descriptors exactly (fallback image).
OT_USB_MSC ?= 1
ifeq ($(OT_USB_MSC),0)
BUILD     := $(BUILD)/nomsc
endif
PTF       := $(BUILD)/$(PTF_NAME)
ifneq ($(shell printf '%s' '$(PTF_NAME)' | wc -c | tr -d ' '),24)
$(error image name '$(PTF_NAME)' must be exactly 24 characters (update bootloader limit, docs/HARDWARE_TESTING.md))
endif
# USB identity (third_party/teensy3/PATCHES.txt). Not PJRC's 16C0:0489 (licensed for
# genuine Teensy boards only) and not Polyend's 16D0:0E9F.
# TODO: 1209:0001 is the pid.codes *test* PID -- register a real pid.codes PID
# before any public release.
USB_ID    := -DOT_USB_VID=0x1209 -DOT_USB_PID=0x0001
CORE      := third_party/teensy3
ARM_GCC   := $(lastword $(sort $(wildcard /Applications/ArmGNUToolchain/*/arm-none-eabi/bin/arm-none-eabi-gcc)))
CROSS     ?= $(if $(ARM_GCC),$(patsubst %gcc,%,$(ARM_GCC)),arm-none-eabi-)
CC        := $(CROSS)gcc
CXX       := $(CROSS)g++
OBJCOPY   := $(CROSS)objcopy
SIZE      := $(CROSS)size

CPUFLAGS  := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
DEFS      := -D__MK66FX1M0__ -DF_CPU=180000000 -DUSB_MIDI_SERIAL -DLAYOUT_US_ENGLISH \
             -DF_BUS=90000000 -DSERIAL5_TX_BUFFER_SIZE=512 -DSERIAL5_RX_BUFFER_SIZE=512 \
             -DTEENSYDUINO=159 -DARDUINO=10819 -DOT_VERSION=\"$(VERSION)\" -DOT_TARGET_DEVICE=1 \
             -DOT_SDRAM_CACHE=$(OT_SDRAM_CACHE) -DOT_FASTRUN=$(OT_FASTRUN) -DOT_USB_MSC=$(OT_USB_MSC) -DOT_POOL_TEST=$(OT_POOL_TEST) -DOT_FONTS_COMPRESSED=1 $(USB_ID) $(DEFS_EXTRA) $(MOD_DEFS)
INCS      := -I$(CORE) -Isrc -Isrc/core -Isrc/hal -Ithird_party/fatfs $(MOD_INCS) -I$(BUILD)/otm-gen
COMMON    := $(CPUFLAGS) $(DEFS) $(INCS) -O2 -g -ffunction-sections -fdata-sections -Wall -MMD
CFLAGS    := $(COMMON) -std=gnu11
CXXFLAGS  := $(COMMON) -std=gnu++17 -fno-exceptions -fno-rtti -felide-constructors -Wno-psabi
# Own linker script: the Teensy one + the uncached `.dma` section (src/hal/opentracker.ld).
LDSCRIPT  := src/hal/opentracker.ld
LDFLAGS   := $(CPUFLAGS) -O2 -Wl,--gc-sections,--defsym=__rtc_localtime=0,--defsym=__otm_fast_bytes=$(OTM_FAST_BYTES),--no-warn-rwx-segments -T$(LDSCRIPT) \
             --specs=nano.specs -u _printf_float
LIBS      := -lm -lstdc++

CORE_C    := $(wildcard $(CORE)/*.c)
CORE_CXX  := $(filter-out $(CORE)/main.cpp,$(wildcard $(CORE)/*.cpp))
APP_C     := $(shell find src -name '*.c') third_party/fatfs/ff.c third_party/fatfs/ffunicode.c
APP_CXX   := $(shell find src -name '*.cpp')
OBJS      := $(patsubst %,$(BUILD)/%.o,$(CORE_C) $(CORE_CXX) $(APP_C) $(APP_CXX))
# Profile modules (profile.mk): sources from MODULES_DIR and the generated
# registry, found through vpath; -O2 (audio path). The .fastrun groups in
# src/hal/opentracker.ld match these objects by file name.
MOD_OBJS  := $(patsubst %.cpp,$(BUILD)/mod/%.cpp.o,$(MOD_SRCS))
OBJS      += $(MOD_OBJS)

# [I] Modules from the card (tools/make_otm.py): the core's export table and
# the versions a loadable module must match, generated for src/hal/module_pool.cpp.
.DEFAULT_GOAL := all
OTM_GEN   := $(BUILD)/otm-gen
OTM_EXPORTS := src/core/modules/module_exports.txt
$(OTM_GEN)/otm_core.h: tools/make_otm.py $(OTM_EXPORTS) $(wildcard src/core/modules/*.h src/core/audio/*.h) src/core/fastrun.h
	python3 -I tools/make_otm.py abi-header --cxx $(CXX) --cxxflags '$(CXXFLAGS)' --out $@
$(OTM_GEN)/otm_exports.cpp: tools/make_otm.py $(OTM_EXPORTS)
	python3 -I tools/make_otm.py exports-cpp --out $@
$(BUILD)/src/hal/module_pool.cpp.o: $(OTM_GEN)/otm_core.h
OBJS      += $(BUILD)/$(OTM_GEN)/otm_exports.cpp.o

# Size optimization per directory (docs/OPTIMIZATION_2X.md OP-4): UI, app,
# HAL, formats and FatFs build with -Os; everything on the audio /
# sequencer path or in an interrupt handler keeps -O2:
#  - src/core/audio, src/core/sequencer, src/core/model (pattern assembly runs in
#    the audio block on a pattern switch), the Teensy core (USB / DMA ISRs);
#  - app.cpp (App::preBlock, ISequencerHost callbacks), capture.cpp (recorder
#    tap in the audio block), render.cpp (offline engine render loop);
#  - hal: ft81x.cpp (display SPI/DMA), sdhc.cpp (SD transfers), sdram_cache.cpp,
#    audio_sgtl5000.cpp (I2S DMA ISRs), diag.cpp (USB drain / fault handler ISR),
#    board.cpp (PORTC ISR), usb_storage.cpp (USB interrupt hook, SD block I/O for the computer).
OS_DIRS   := src/core/ui/% src/core/app/% src/hal/% src/core/formats/% \
             third_party/fatfs/%
O2_KEEP   := src/core/app/app.cpp src/core/app/capture.cpp src/core/app/render.cpp \
             src/hal/ft81x.cpp src/hal/sdhc.cpp src/hal/sdram_cache.cpp src/hal/audio_sgtl5000.cpp \
             src/hal/diag.cpp src/hal/board.cpp src/hal/usb_storage.cpp
OS_SRCS   := $(filter-out $(O2_KEEP),$(filter $(OS_DIRS),$(APP_C) $(APP_CXX)))
$(patsubst %,$(BUILD)/%.o,$(OS_SRCS)): OPT := -Os
# Float libm only (OP-4): the files that used the double libm warn on any
# implicit float -> double promotion; the double libm must not come back
# (tools/check_image.py checks the linked image).
DPROMO    := src/core/app/sample_fx.cpp src/core/app/perf_presets.cpp src/core/ui/screen_recorder.cpp \
             src/core/ui/screen_resample.cpp \
             src/core/ui/screen_instrument.cpp src/core/ui/screen_mixer.cpp src/core/ui/screen_loader.cpp \
             src/core/ui/widgets.cpp src/core/fmath.cpp
$(patsubst %,$(BUILD)/%.o,$(DPROMO)): WARN := -Wdouble-promotion

# The stock updater only lists Firmware/ files named "PolyendTrac*.ptf" (spec 06), so the
# image name keeps that prefix for compatibility (and the 24-character length, above).

all: check-toolchain $(BUILD)/opentracker.hex $(PTF)

# Fail early if a host-only ARM GCC package is installed without the target
# C/C++ headers needed to build firmware.
check-toolchain:
	@set -eu; \
	for tool in "$(CC)" "$(CXX)" "$(OBJCOPY)" "$(SIZE)"; do \
	    if ! command -v "$$tool" >/dev/null 2>&1; then \
	        echo "error: required ARM tool '$$tool' was not found." >&2; \
	        echo "Install the complete Arm GNU Toolchain, or set CROSS=/path/to/arm-none-eabi-." >&2; \
	        exit 1; \
	    fi; \
	done; \
	if ! printf '#include <stdint.h>\n' | $(CC) $(CPUFLAGS) -x c -fsyntax-only - >/dev/null 2>&1; then \
	    echo "error: $(CC) cannot find the ARM C standard headers (for example stdint.h)." >&2; \
	    echo "The installed compiler may not include the target runtime; install the complete Arm GNU Toolchain." >&2; \
	    exit 1; \
	fi; \
	if ! printf '#include <cstdint>\n' | $(CXX) $(CPUFLAGS) -std=gnu++17 -x c++ -fsyntax-only - >/dev/null 2>&1; then \
	    echo "error: $(CXX) cannot find the ARM C++ standard headers." >&2; \
	    echo "Install the complete Arm GNU Toolchain, or set CROSS=/path/to/arm-none-eabi-." >&2; \
	    exit 1; \
	fi

$(OBJS): | check-toolchain

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OPT) $(WARN) -c $< -o $@

$(BUILD)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(OPT) $(WARN) -c $< -o $@

$(BUILD)/mod/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fstack-usage -c $< -o $@

$(BUILD)/opentracker.elf: $(OBJS) $(LDSCRIPT)
	$(CXX) $(LDFLAGS) $(OBJS) -L$(CORE) $(LIBS) -o $@ -Wl,-Map=$(BUILD)/opentracker.map
	$(SIZE) $@
	@# No static constructor may touch SDRAM (it is not running before main()).
	python3 -I tools/check_ctors.py $(CROSS)objdump $@ || { rm -f $@; exit 1; }

$(BUILD)/opentracker.hex: $(BUILD)/opentracker.elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

# The stock updater loads Intel HEX files named *.ptf from /Firmware on the SD card.
$(PTF): $(BUILD)/opentracker.hex
	python3 tools/check_image.py $<
	cp $< $@

HOSTCXX   ?= g++
host-test:
	python3 -I tools/fonts/pack_fonts.py --check src/core/ui/fonts
	$(MAKE) -C tests

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
.PHONY: all check-toolchain clean host-test

# Performance gate (docs/PLAN_2X.md "Gate"; docs/HARDWARE_TESTING.md "Performance gate").
#  1. bench-m4: QEMU -icount instruction counts per audio block vs tests/perf_baseline.txt
#     (fail above +5 %); skipped with a note when qemu-system-arm / $(CROSS)g++ are missing.
#  2. host bench (timing on this machine, informational).
#     The same run gates the per-engine limit lines (acd.* / vap* / fat* / perc*: "limit N").
#  3. tools/telemetry_check.py: its built-in sample check, plus LOG=<captured device log>
#     against the thresholds (render avg < 45 %, max < 70 %, loop max < 50 ms outside loading).
#  4. Flash size of the firmware image (.text + .data): fail above 800,000 B, warn above
#     600,000 B (docs/VERIFY_M1.md R4); builds build/opentracker.elf when it is stale.
PERF_OUT := $(BUILD)/perf
# Per-profile baseline when the profile has its own (tests/perf_baseline_<profile>.txt).
PERF_BASELINE := $(firstword $(wildcard tests/perf_baseline_$(PROFILE).txt) tests/perf_baseline.txt)
perf-gate:
	@mkdir -p $(PERF_OUT)
	@if command -v qemu-system-arm >/dev/null 2>&1 && command -v $(CXX) >/dev/null 2>&1; then \
	    $(MAKE) -s -C tests bench-m4 CROSS=$(CROSS) > $(PERF_OUT)/bench_m4.txt && \
	    python3 tools/telemetry_check.py bench --baseline $(PERF_BASELINE) $(PERF_OUT)/bench_m4.txt; \
	else echo "perf-gate: qemu-system-arm or $(CXX) missing -- bench-m4 SKIPPED (host-only gate)"; fi
	@if command -v $(CXX) >/dev/null 2>&1; then \
	    { $(MAKE) -s $(BUILD)/opentracker.elf > $(PERF_OUT)/firmware_build.txt 2>&1 || \
	      { tail -20 $(PERF_OUT)/firmware_build.txt; exit 1; }; } && \
	    python3 tools/telemetry_check.py size --size-tool $(SIZE) $(BUILD)/opentracker.elf && \
	    $(SIZE) $(MOD_OBJS) > $(PERF_OUT)/module_sizes.txt && \
	    python3 -I tools/profile.py sizes --profile profiles/$(PROFILE).toml --modules-dir $(MODULES_DIR) \
	        --size-output $(PERF_OUT)/module_sizes.txt; \
	else echo "perf-gate: $(CXX) missing -- flash size check SKIPPED"; fi
	@$(MAKE) -s -C tests bench > $(PERF_OUT)/bench_host.txt && sed 's/^/  host: /' $(PERF_OUT)/bench_host.txt | head -8
	@python3 tools/telemetry_check.py selftest
	@if [ -n "$(LOG)" ]; then python3 tools/telemetry_check.py log "$(LOG)" \
	    $$( [ -f $(PERF_OUT)/bench_m4.txt ] && echo --bench $(PERF_OUT)/bench_m4.txt ); \
	else echo "perf-gate: no LOG=<device log> given -- telemetry thresholds not checked on device data"; fi
	@echo "perf-gate: PASS"
.PHONY: perf-gate

# Fonts (tools/fonts/gen_fonts.py): the generated files are committed, so
# ordinary builds never run Python.
fonts:
	python3 -I tools/fonts/gen_fonts.py ui src/core/ui/fonts
	python3 -I tools/fonts/pack_fonts.py src/core/ui/fonts
	$(MAKE) -C sim fonts
.PHONY: fonts

# 2.x hardware-test projects (TESTSONGS.md): build/testsongs/Projects/<name>/.
# SONGS="1 3" limits the set; songs whose milestone is not built are skipped.
# STEMS_SECONDS=180: song 19's stems 180 s long (default 30 s; the C13 hardware run).
testsongs:
	$(MAKE) -C tools/make_testsongs run SONGS="$(SONGS)" STEMS_SECONDS="$(STEMS_SECONDS)"
.PHONY: testsongs

# Declared module budgets of the profile (tools/profile.py budget).
budget:
	python3 -I tools/profile.py budget --profile profiles/$(PROFILE).toml --modules-dir $(MODULES_DIR)
.PHONY: budget

# [I] A module as a file loadable from the card (tools/make_otm.py), compiled
# with the firmware's flags. OTM_REF=<base> also writes <name>.ref.bin, the image
# otmLoad must produce at that base (tests/module_loader_test.cpp). OTM_HOT=<file>:
# the row's functions, hottest first (tools/synth_hot.py --emit-hot); OTM_REF_FAST=<base>
# and OTM_REF_CUT=<bytes> the reference's fast base and cut; OTM_SUFFIX names a variant
# (<name><suffix>.otm).
OTM_NAME = $(notdir $(OTM_MODULE))
otm:
	@test -n "$(OTM_MODULE)" || { echo "make otm OTM_MODULE=<module path, e.g. ../opentracker-fx/modules/tape>"; exit 1; }
	@mkdir -p $(BUILD)/otm
	python3 -I tools/make_otm.py build --module $(MODULES_DIR)/$(OTM_MODULE) --obj-dir $(BUILD)/otm/obj/$(OTM_NAME) \
	    --out $(BUILD)/otm/$(OTM_NAME)$(OTM_SUFFIX).otm --cxx $(CXX) --cxxflags '$(CXXFLAGS)' \
	    $(if $(OTM_HOT),--hot $(OTM_HOT)) \
	    $(if $(OTM_REF),--reference $(OTM_REF) --ref-out $(BUILD)/otm/$(OTM_NAME)$(OTM_SUFFIX).ref.bin) \
	    $(if $(OTM_REF_FAST),--ref-fast $(OTM_REF_FAST) --ref-fast-out $(BUILD)/otm/$(OTM_NAME)$(OTM_SUFFIX).ref-fast.bin) \
	    $(if $(OTM_REF_CUT),--ref-cut $(OTM_REF_CUT))
.PHONY: otm

# [I] tools/make_otm.py against the linker (tools/make_otm_test.py): needs
# the ARM toolchain and pyelftools.
otm-test:
	python3 -I tools/make_otm_test.py --cxx $(CXX) --cxxflags '$(CXXFLAGS)'
.PHONY: otm-test

# [I] The profile's card modules (profiles/*.toml card = true, fork #21), each as
# build/<profile>/otm/<name>.otm for the card's Modules/ folder, with its hot row
# from tools/hot_rows/<name>.hot when there is one (make hot-rows).
HOT_ROWS := tools/hot_rows
card:
	@mkdir -p $(BUILD)/otm
	@$(if $(MOD_CARD_DIRS),,echo "profile $(PROFILE) has no card modules")
	$(foreach d,$(MOD_CARD_DIRS),python3 -I tools/make_otm.py build --module $(d) --obj-dir $(BUILD)/otm/obj/$(notdir $(d)) \
	    --out $(BUILD)/otm/$(notdir $(d)).otm --cxx $(CXX) --cxxflags '$(CXXFLAGS)' \
	    $(if $(wildcard $(HOT_ROWS)/$(notdir $(d)).hot),--hot $(HOT_ROWS)/$(notdir $(d)).hot) &&) true
.PHONY: card

# [I] The synths' hot rows (fork #21): every mode of every synth under the QEMU bench
# (profiles/allsynths.toml), folded into tools/hot_rows/<module>.hot by tools/synth_hot.py.
# Rerun after a synth's DSP code changes; the rows are checked in.
hot-rows:
	$(MAKE) -C tests bench-m4-synth-modes PROFILE=allsynths
	python3 -I tools/synth_hot.py build/allsynths/bench/audio_bench_m4_modes.elf build/allsynths/bench/synth_modes_hot.txt \
	    --modes build/allsynths/bench/synth_modes.txt --emit-hot $(HOT_ROWS)
.PHONY: hot-rows

# Synth preset pack (tools/make_patches/PRESETS.md): build/patches/Patches/<MODEL>/.
patches:
	$(MAKE) -C tools/make_patches run
.PHONY: patches

# Desktop simulator (sim/README.md).
sim:
	$(MAKE) -C sim
.PHONY: sim

# OpenTracker Desktop (desktop/README.md, docs/PLAN_DESKTOP_APP.md).
desktop:
	$(MAKE) -C desktop
.PHONY: desktop
