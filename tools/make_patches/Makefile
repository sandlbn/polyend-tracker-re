# Host tool: writes the OpenTracker synth preset pack (PRESETS.md) with the
# App's own Save As path (src/core, unchanged).
#   make -C reimpl/tools/make_patches           -> build/make_patches/make_patches
#   make -C reimpl/tools/make_patches run       -> build/patches/Patches/<MODEL>/<name>.patch
#   make -C reimpl/tools/make_patches markdown  -> regenerates PRESETS.md (PROFILE=full plus the sections
#                                                  of PROFILE=nofm and classic that full lacks, splice_md.py)
#   make -C reimpl patches                      (same as run)
CXX      ?= g++
ROOT     := ../..
include $(ROOT)/profile.mk
# The pack holds the models of the profile's modules (presets.cpp); PRESETS.md
# comes from PROFILE=full (every model).
ifneq ($(HAVE_PRESET_MOD),1)
$(error make_patches needs a synth module with presets (profile.mk PRESET_MODS: $(PRESET_MODS)))
endif
OUT      := $(ROOT)/build/$(PROFILE)/make_patches
SD       ?= $(ROOT)/build/patches
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -Wno-format-truncation -MMD \
            -I$(ROOT)/src/core -I$(ROOT)/src -I$(ROOT)/sim/src $(MOD_INCS) $(MOD_DEFS)

CORE_SRCS := $(shell find $(ROOT)/src/core -name '*.cpp')
CORE_OBJS := $(patsubst $(ROOT)/src/%.cpp,$(OUT)/obj/%.o,$(CORE_SRCS)) \
             $(patsubst %.cpp,$(OUT)/obj/mod/%.o,$(MOD_SRCS))
TOOL_OBJS := $(OUT)/obj/tool/main.o $(OUT)/obj/tool/presets.o $(OUT)/obj/host_storage.o
BIN       := $(OUT)/make_patches

all: $(BIN)

$(OUT)/obj/%.o: $(ROOT)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/obj/mod/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/obj/tool/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/obj/host_storage.o: $(ROOT)/sim/src/host_storage.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BIN): $(CORE_OBJS) $(TOOL_OBJS)
	$(CXX) $^ -lpthread -o $@

# Fresh card folder each run; only Patches/ is meant for the card (the
# generator's own Workspace, config and project folders are removed).
run: $(BIN)
	rm -rf $(SD)
	mkdir -p $(SD)
	$(BIN) $(SD)
	cd $(SD) && find . -mindepth 1 -maxdepth 1 ! -name Patches -exec rm -rf {} +

# PRESETS.md covers every model: PROFILE=full plus the sections of the other profiles
# that full does not have (nofm: JU6, D-Type; classic: CS-Type, A-Type; techno: T-Type;
# retro: 8-Type, Hand-Type, B-Type, Arc-Type;
# splice_md.py).
MD_PROFILES := nofm classic idm mini techno retro
markdown: $(BIN)
	$(BIN) --markdown > $(OUT)/presets_full.md
	set -e; for p in $(MD_PROFILES); do \
	    $(MAKE) PROFILE=$$p all; \
	    $(ROOT)/build/$$p/make_patches/make_patches --markdown > $(OUT)/presets_$$p.md; \
	done
	python3 -I splice_md.py $(OUT)/presets_full.md $(foreach p,$(MD_PROFILES),$(p)=$(OUT)/presets_$(p).md) > PRESETS.md

clean:
	rm -rf $(OUT)

-include $(CORE_OBJS:.o=.d) $(TOOL_OBJS:.o=.d)
.PHONY: all run markdown clean
