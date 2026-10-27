# Host tool: writes the OpenTracker 2.x hardware-test projects (TESTSONGS.md)
# with the App's own project / instrument save paths (src/core, unchanged).
#   make -C reimpl/tools/make_testsongs        -> build/make_testsongs/make_testsongs
#   make -C reimpl/tools/make_testsongs run    -> build/testsongs/Projects/<name>/
#   make -C reimpl testsongs                   (same as run)
#   STEMS_SECONDS=180                          -> song 19's stems 180 s long (default 30)
CXX      ?= g++
ROOT     := ../..
OUT      := $(ROOT)/build/make_testsongs
SD       ?= $(ROOT)/build/testsongs
SONGS    ?=
STEMS_SECONDS ?=
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -Wno-format-truncation -MMD \
            -I$(ROOT)/src/core -I$(ROOT)/src -I$(ROOT)/sim/src

CORE_SRCS := $(shell find $(ROOT)/src/core -name '*.cpp')
CORE_OBJS := $(patsubst $(ROOT)/src/%.cpp,$(OUT)/obj/%.o,$(CORE_SRCS))
TOOL_OBJS := $(OUT)/obj/tool/main.o $(OUT)/obj/tool/testsongs.o $(OUT)/obj/tool/stock_check.o \
             $(OUT)/obj/host_storage.o
BIN       := $(OUT)/make_testsongs

all: $(BIN)

$(OUT)/obj/%.o: $(ROOT)/src/%.cpp
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

# Fresh card folder each run (stale songs from an older build must not
# linger). Only Projects/<name>/ and the source WAVs under Samples/ are meant
# for the card: the generator's own Workspace, config and recents are removed
# so copying the folder cannot overwrite the device's.
run: $(BIN)
	rm -rf $(SD)
	mkdir -p $(SD)
	$(BIN) $(SD) $(foreach s,$(SONGS),--song $(s)) $(if $(STEMS_SECONDS),--stems-seconds $(STEMS_SECONDS))
	rm -rf $(SD)/Workspace $(SD)/OpenTracker $(SD)/Projects/recents

clean:
	rm -rf $(OUT)

-include $(CORE_OBJS:.o=.d) $(TOOL_OBJS:.o=.d)
.PHONY: all run clean
