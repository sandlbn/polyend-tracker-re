# Host tool: converts .sy1 patch files to 1-Type patches (Patches/1TYPE/) with
# the App's own Save As path (src/core, unchanged); the mapping is the 1-Type
# module's tools/sy1/sy1_map.h.
#   make -C reimpl/tools/sy1_to_patch                       -> build/full/sy1_to_patch/sy1_to_patch
#   build/full/sy1_to_patch/sy1_to_patch <file.sy1 | folder>... -o <sd-root>
CXX      ?= g++
ROOT     := ../..
OT_HOST_BUILD := 1   # [I] card modules compiled in (profile.mk)
include $(ROOT)/profile.mk
# The pack holds the models of the profile's modules (presets.cpp); PRESETS.md
# comes from PROFILE=full (every model).
ifneq ($(HAVE_PRESET_MOD),1)
$(error sy1_to_patch needs a synth module with presets (profile.mk PRESET_MODS: $(PRESET_MODS)))
endif
OUT      := $(ROOT)/build/$(PROFILE)/sy1_to_patch
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -Wno-format-truncation -MMD \
            -I$(ROOT)/src/core -I$(ROOT)/src -I$(ROOT)/sim/src -I../make_patches -I$(MODULES_DIR)/modules/onetype $(MOD_INCS) $(MOD_DEFS)

CORE_SRCS := $(shell find $(ROOT)/src/core -name '*.cpp')
CORE_OBJS := $(patsubst $(ROOT)/src/%.cpp,$(OUT)/obj/%.o,$(CORE_SRCS)) \
             $(patsubst %.cpp,$(OUT)/obj/mod/%.o,$(MOD_SRCS))
TOOL_OBJS := $(OUT)/obj/tool/main.o $(OUT)/obj/tool/presets.o $(OUT)/obj/host_storage.o
BIN       := $(OUT)/sy1_to_patch

all: $(BIN)

$(OUT)/obj/%.o: $(ROOT)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/obj/mod/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/obj/tool/main.o: main.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/obj/tool/presets.o: ../make_patches/presets.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/obj/host_storage.o: $(ROOT)/sim/src/host_storage.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BIN): $(CORE_OBJS) $(TOOL_OBJS)
	$(CXX) $^ -lpthread -o $@


ifeq ($(filter 1TY1,$(MOD_IDS)),)
$(error sy1_to_patch needs the 1-Type module (1TY1) in the profile)
endif

clean:
	rm -rf $(OUT)

-include $(CORE_OBJS:.o=.d) $(TOOL_OBJS:.o=.d)
.PHONY: all clean
