# OpenTracker desktop simulator: src/core (unchanged) + host HAL in sim/src.
#   make -C sim          -> build/sim/opentracker-sim
#   make -C sim fonts    -> regenerate third_party/rom_fonts.inc (needs python3 + Pillow;
#                           `make fonts` in reimpl/ also regenerates the UI fonts)
CXX      ?= g++
ROOT     := ..
OUT      := $(ROOT)/build/sim
SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2)
HAVE_ALSA  := $(shell pkg-config --exists alsa && echo 1)
ifeq ($(HAVE_ALSA),1)
ALSA_CFLAGS := -DOT_SIM_ALSA $(shell pkg-config --cflags alsa)
ALSA_LIBS   := $(shell pkg-config --libs alsa)
endif
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -Wno-format-truncation -MMD -I$(ROOT)/src/core -I$(ROOT)/src -Isrc \
            $(SDL_CFLAGS) $(ALSA_CFLAGS)
LDLIBS   := $(SDL_LIBS) $(ALSA_LIBS) -lpthread

CORE_SRCS := $(shell find $(ROOT)/src/core -name '*.cpp')
CORE_OBJS := $(patsubst $(ROOT)/src/%.cpp,$(OUT)/obj/%.o,$(CORE_SRCS))
SIM_SRCS  := $(wildcard src/*.cpp)
SIM_OBJS  := $(patsubst src/%.cpp,$(OUT)/obj/sim/%.o,$(SIM_SRCS))
BIN       := $(OUT)/opentracker-sim
# Image-path check (sim only): SwDisplay + HostStorage, no SDL window.
JPEG_CHECK      := $(OUT)/jpeg-check
JPEG_CHECK_OBJS := $(OUT)/obj/sim/check/jpeg_check.o $(addprefix $(OUT)/obj/sim/,sw_display.o canvas.o jpeg_decode.o host_storage.o png.o) \
                   $(OUT)/obj/core/ui/fonts/fonts.o
# Visual-diff gate (docs/VISUAL_DIFF_1.9.4.md): replays visual/*.scene in the
# headless sim on a scratch copy of $(SD) and checks pixels against $(STOCK).
#   make visual-diff [DEVICE=dir] [ALLOW_FAIL=1] [SCENES=s01_pattern,s04_config]
#                    [PROJECTS=dir] [PROJECT="name"]
# A scene line 'project NAME' boots the stock project $(PROJECTS)/NAME (opened
# once per run on a scratch card; the fixture is not modified). PROJECT sets
# the project for scenes without that line (default: the SD fixture's own
# Workspace). STOCK or SD missing = skip; a missing project skips its scenes.
# Paths are relative to reimpl/sim.
VISUAL_DIFF      := $(OUT)/visual-diff
VISUAL_DIFF_OBJS := $(OUT)/obj/sim/check/visual_diff.o $(OUT)/obj/sim/png.o
STOCK      ?= ../../../ReversedPolytracker/fixtures/stock-snapshots-1.9.2
SD         ?= ../../../ReversedPolytracker/fixtures/sdcard-backup-2026-10-15
PROJECTS   ?= ../../../ReversedPolytracker/fixtures/stock-project
PROJECT    ?=
VISUAL_OUT ?= build/visual

all: $(BIN)

$(OUT)/obj/%.o: $(ROOT)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/obj/sim/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/obj/sim/check/%.o: check/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(JPEG_CHECK): $(JPEG_CHECK_OBJS)
	$(CXX) $^ -o $@

$(VISUAL_DIFF): $(VISUAL_DIFF_OBJS)
	$(CXX) $^ -o $@

$(BIN): $(CORE_OBJS) $(SIM_OBJS)
	$(CXX) $^ $(LDLIBS) -o $@

fonts:
	python3 -I $(ROOT)/tools/fonts/gen_fonts.py rom /usr/share/fonts/TTF/DejaVuSans.ttf \
	    /usr/share/fonts/TTF/DejaVuSansMono.ttf third_party/rom_fonts.inc

visual-diff: $(BIN) $(VISUAL_DIFF)
	$(VISUAL_DIFF) --scenes visual --stock "$(STOCK)" --sd "$(SD)" --sim $(BIN) --out "$(VISUAL_OUT)" \
	    --projects "$(PROJECTS)" $(if $(PROJECT),--project "$(PROJECT)") \
	    $(if $(DEVICE),--device "$(DEVICE)") $(if $(SCENES),--only $(SCENES)) \
	    $(if $(filter 1,$(ALLOW_FAIL)),--allow-fail)

# Key-flow scripts (2.0.1 user report): Performance entry from every screen,
# Master pages and sub-pages (scripts/keyflow.txt).
keyflow-check: $(BIN)
	rm -rf $(OUT)/check-keys $(OUT)/check-keys.eeprom
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy $(BIN) --sd $(OUT)/check-keys \
	    --script scripts/keyflow.txt
	# Sample Recorder meter redraw rate with a moving input.
	rm -rf $(OUT)/check-rec $(OUT)/check-rec.eeprom
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy $(BIN) --sd $(OUT)/check-rec --line-in-tone 440 \
	    --script scripts/recorder_meter.txt
	# Sample Playback playhead (USER_REPORTS_R2 1c).
	rm -rf $(OUT)/check-head $(OUT)/check-head.eeprom
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy $(BIN) --sd $(OUT)/check-head \
	    --script scripts/playhead.txt

# Headless smoke test: boot the app on a scratch SD card and save screenshots,
# then the visual-diff gate (report-only until the 1.9.5 screens land: remove
# the ALLOW_FAIL line below to make visual failures fail 'check').
check: ALLOW_FAIL ?= 1
check: $(BIN) $(JPEG_CHECK) visual-diff keyflow-check
	$(JPEG_CHECK) fixtures $(OUT)/jpeg-check.png
	rm -rf $(OUT)/check-sd $(OUT)/check-sd.eeprom
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy $(BIN) --sd $(OUT)/check-sd \
	    --script scripts/smoke.txt --screenshot $(OUT)/smoke.png
	# Display-list budget per screen + fragments-on/off pixel identity.
	rm -rf $(OUT)/check-dl $(OUT)/check-dl.eeprom
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy $(BIN) --sd $(OUT)/check-dl \
	    --script scripts/display_budget.txt
	# 2.x USB Storage mode: script commands and the Config row drive the mode page.
	rm -rf $(OUT)/check-usb $(OUT)/check-usb.eeprom
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy $(BIN) --sd $(OUT)/check-usb \
	    --script scripts/usb_storage.txt
	# 2.x test songs (TESTSONGS.md): each opens without errors and plays.
	$(MAKE) testsongs-check

# Generates the test songs this build supports and opens / plays each one in
# the headless sim on a card with only Projects/<name> (tools/make_testsongs).
# Song 19's stems are 20 s here (TESTSONGS_STEMS_SECONDS) to keep the check fast.
TESTSONGS_SD := $(abspath $(OUT)/testsongs)
TESTSONGS_STEMS_SECONDS ?= 20
testsongs-check: $(BIN)
	$(MAKE) -C $(ROOT)/tools/make_testsongs run SD=$(TESTSONGS_SD) STEMS_SECONDS=$(TESTSONGS_STEMS_SECONDS)
	sh $(ROOT)/tools/make_testsongs/sim_check.sh $(abspath $(BIN)) $(TESTSONGS_SD) $(abspath $(OUT)/testsongs-check)

clean:
	rm -rf $(OUT)

-include $(CORE_OBJS:.o=.d) $(SIM_OBJS:.o=.d) $(JPEG_CHECK_OBJS:.o=.d) $(VISUAL_DIFF_OBJS:.o=.d)
.PHONY: all clean fonts check visual-diff testsongs-check
