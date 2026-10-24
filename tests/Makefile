# Host unit tests: builds every *_test.cpp against src/core and runs it.
CXX      ?= g++
SRC      := ../src
OUT      := ../build/host-tests
CXXFLAGS := -std=c++17 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined \
            -fno-sanitize-recover=undefined -ffp-contract=off -I$(SRC)/core -I$(SRC) -I. -MMD
LDFLAGS  := -fsanitize=address,undefined

CORE_SRCS := $(shell find $(SRC)/core -name '*.cpp')
CORE_OBJS := $(patsubst $(SRC)/%.cpp,$(OUT)/obj/%.o,$(CORE_SRCS))
TEST_SRCS := $(filter-out usb_desc_test.cpp,$(wildcard *_test.cpp))
TEST_BINS := $(patsubst %.cpp,$(OUT)/%,$(TEST_SRCS)) $(OUT)/usb_desc_test_msc $(OUT)/usb_desc_test_nomsc

all: run

$(OUT)/obj/%.o: $(SRC)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/libcore.a: $(CORE_OBJS)
	@rm -f $@
	ar rcs $@ $^

$(OUT)/%_test: %_test.cpp $(OUT)/libcore.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $< $(OUT)/libcore.a $(LDFLAGS) -o $@

# USB descriptor check: build each USB mode separately so this works without
# GNU objcopy on macOS.
USB_DESC_CFLAGS := -std=gnu11 -w -D__MK66FX1M0__ -DF_CPU=180000000 -DUSB_MIDI_SERIAL -DOT_USB_VID=0x1209 \
                   -DOT_USB_PID=0x0001 -DOT_USB_DESC_HOST_TEST -I../third_party/teensy3
USB_DESC_DEPS := usb_desc_host.c ../third_party/teensy3/usb_desc.c ../third_party/teensy3/usb_desc.h
$(OUT)/obj/usb_desc_msc.o: $(USB_DESC_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(USB_DESC_CFLAGS) -DOT_USB_MSC=1 -c usb_desc_host.c -o $@
$(OUT)/obj/usb_desc_nomsc.o: $(USB_DESC_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(USB_DESC_CFLAGS) -DOT_USB_MSC=0 -c usb_desc_host.c -o $@
$(OUT)/usb_desc_test_msc: usb_desc_test.cpp $(OUT)/obj/usb_desc_msc.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -DOT_USB_MSC=1 $< $(OUT)/obj/usb_desc_msc.o $(LDFLAGS) -o $@
$(OUT)/usb_desc_test_nomsc: usb_desc_test.cpp $(OUT)/obj/usb_desc_nomsc.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -DOT_USB_MSC=0 $< $(OUT)/obj/usb_desc_nomsc.o $(LDFLAGS) -o $@

build: $(TEST_BINS)

# Private vendor fixtures (never committed): the repo-root fixtures symlink.
OT_VENDOR_FIXTURES ?= $(abspath $(CURDIR)/../../fixtures/vendor)

run: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do OT_FIXTURES=$(CURDIR)/fixtures OT_VENDOR_FIXTURES=$(OT_VENDOR_FIXTURES) $$t; done

# Audio render benchmark (tests/audio_bench.cpp): -O2, no sanitizers.
BENCH_OUT := ../build/bench
# Headers of the benched code (the bench binaries are one compile each).
M4_HDRS := $(wildcard $(SRC)/core/audio/*.h) $(wildcard $(SRC)/core/audio/engines/*.h) $(wildcard $(SRC)/core/model/*.h) \
           $(wildcard $(SRC)/core/sequencer/*.h)
BENCH_FLAGS := -std=c++17 -O2 -g -DOT_AUDIO_SDRAM_STATS -I$(SRC)/core -I$(SRC) -I.
BENCH_SRCS := $(CORE_SRCS)
$(BENCH_OUT)/audio_bench: audio_bench.cpp peak_trace.h $(CORE_SRCS) $(M4_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(BENCH_FLAGS) audio_bench.cpp $(BENCH_SRCS) -o $@

# Song 5 peak trace (peak_trace.cpp): the whole App plays TESTSONGS song 5 on
# the host and records the engine's input; the "peak" bench scenario replays it.
PEAK_TRACE := $(BENCH_OUT)/song5.trace
$(OUT)/peak_trace: peak_trace.cpp peak_trace.h ui_fakes.h $(OUT)/libcore.a $(wildcard ../tools/make_testsongs/*.cpp) \
                   $(wildcard ../tools/make_testsongs/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $< $(OUT)/libcore.a $(LDFLAGS) -o $@
$(PEAK_TRACE): $(OUT)/peak_trace
	@mkdir -p $(dir $@)
	$(OUT)/peak_trace $@
peak-trace: $(PEAK_TRACE)

bench: $(BENCH_OUT)/audio_bench $(PEAK_TRACE)
	$(BENCH_OUT)/audio_bench $(PEAK_TRACE)

# Same benchmark compiled with the device's compiler flags for Cortex-M4 and
# run under QEMU (mps2-an386) with -icount: reports executed M4 instructions.
# Toolchain prefix as in the top-level Makefile (Arm GNU Toolchain package
# under /Applications on macOS, else arm-none-eabi- from PATH).
ARM_GCC := $(lastword $(sort $(wildcard /Applications/ArmGNUToolchain/*/arm-none-eabi/bin/arm-none-eabi-gcc)))
CROSS ?= $(if $(ARM_GCC),$(patsubst %gcc,%,$(ARM_GCC)),arm-none-eabi-)
M4_CXX := $(CROSS)g++
M4_CC := $(CROSS)gcc
M4_FLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -O2 -ffunction-sections -fdata-sections
M4_SRCS := audio_bench.cpp $(wildcard $(SRC)/core/audio/*.cpp) $(wildcard $(SRC)/core/audio/engines/*.cpp) \
           $(SRC)/core/formats/wav.cpp $(SRC)/core/formats/patch_pb.cpp \
           $(SRC)/core/formats/format_common.cpp $(SRC)/core/model/project.cpp $(SRC)/core/model/fx.cpp \
           $(wildcard $(SRC)/core/sequencer/*.cpp)
# The device's memcpy / memmove / memset (src/hal/fast_mem.c) replace newlib-nano's here too.
$(BENCH_OUT)/audio_bench_m4.elf: $(M4_SRCS) $(M4_HDRS) peak_trace.h bench_m4/startup.c bench_m4/mps2.ld $(SRC)/hal/fast_mem.c
	@mkdir -p $(dir $@)
	$(M4_CC) $(M4_FLAGS) -c bench_m4/startup.c -o $(BENCH_OUT)/startup_m4.o
	$(M4_CC) $(M4_FLAGS) -std=gnu11 -c $(SRC)/hal/fast_mem.c -o $(BENCH_OUT)/fast_mem_m4.o
	$(M4_CXX) $(M4_FLAGS) -std=gnu++17 -fno-exceptions -fno-rtti -DOT_BENCH_M4 -DOT_AUDIO_SDRAM_STATS -I$(SRC)/core -I$(SRC) -I. \
	    $(M4_SRCS) $(BENCH_OUT)/startup_m4.o $(BENCH_OUT)/fast_mem_m4.o -T bench_m4/mps2.ld -nostartfiles --specs=nano.specs --specs=rdimon.specs -u _printf_float \
	    -Wl,--gc-sections -lm -o $@

# Regenerate the ARM const audio tables (src/core/audio/audio_tables_arm.inc,
# see audio/audio_tables.h) with the device toolchain's libm under QEMU.
audio-tables:
	@mkdir -p $(BENCH_OUT)
	$(M4_CC) $(M4_FLAGS) -c bench_m4/startup.c -o $(BENCH_OUT)/startup_m4.o
	$(M4_CXX) $(M4_FLAGS) -std=gnu++17 -fno-exceptions -fno-rtti -DOT_TABLES_RUNTIME -I$(SRC)/core -I$(SRC) \
	    bench_m4/gen_tables.cpp $(SRC)/core/audio/audio_tables.cpp $(BENCH_OUT)/startup_m4.o -T bench_m4/mps2.ld \
	    -nostartfiles --specs=nano.specs --specs=rdimon.specs -Wl,--gc-sections -lm -o $(BENCH_OUT)/gen_tables.elf
	qemu-system-arm -M mps2-an386 -nographic -icount shift=0 -semihosting-config enable=on,target=native \
	    -kernel $(BENCH_OUT)/gen_tables.elf | python3 -I bench_m4/tables_inc.py > $(BENCH_OUT)/audio_tables_arm.inc
	mv $(BENCH_OUT)/audio_tables_arm.inc $(SRC)/core/audio/audio_tables_arm.inc

bench-m4: $(BENCH_OUT)/audio_bench_m4.elf $(PEAK_TRACE)
	qemu-system-arm -M mps2-an386 -nographic -icount shift=0 -semihosting-config enable=on,target=native \
	    -kernel $<

# Executed instructions per function in the song 5 replay (QEMU TCG plugin,
# tests/bench_m4/hot_plugin.c + hot_funcs.py): the .fastrun candidates
# (docs/OPTIMIZATION_2X.md item 6). Needs /usr/include/qemu-plugin.h.
$(BENCH_OUT)/hot_plugin.so: bench_m4/hot_plugin.c
	@mkdir -p $(dir $@)
	cc -O2 -shared -fPIC -Wall $$(pkg-config --cflags glib-2.0) $< -o $@
$(BENCH_OUT)/audio_bench_m4_peak.elf: $(M4_SRCS) $(M4_HDRS) peak_trace.h bench_m4/startup.c bench_m4/mps2.ld $(SRC)/hal/fast_mem.c
	@mkdir -p $(dir $@)
	$(M4_CC) $(M4_FLAGS) -c bench_m4/startup.c -o $(BENCH_OUT)/startup_m4.o
	$(M4_CC) $(M4_FLAGS) -std=gnu11 -c $(SRC)/hal/fast_mem.c -o $(BENCH_OUT)/fast_mem_m4.o
	$(M4_CXX) $(M4_FLAGS) -std=gnu++17 -fno-exceptions -fno-rtti -DOT_BENCH_M4 -DOT_BENCH_PEAK_ONLY -DOT_AUDIO_SDRAM_STATS \
	    -I$(SRC)/core -I$(SRC) -I. $(M4_SRCS) $(BENCH_OUT)/startup_m4.o $(BENCH_OUT)/fast_mem_m4.o -T bench_m4/mps2.ld \
	    -nostartfiles --specs=nano.specs --specs=rdimon.specs -u _printf_float -Wl,--gc-sections -lm -o $@
bench-m4-hot: $(BENCH_OUT)/audio_bench_m4_peak.elf $(BENCH_OUT)/hot_plugin.so $(PEAK_TRACE)
	qemu-system-arm -M mps2-an386 -nographic -icount shift=0 -semihosting-config enable=on,target=native \
	    -plugin $(BENCH_OUT)/hot_plugin.so,out=$(BENCH_OUT)/hot.txt -kernel $<
	python3 -I bench_m4/hot_funcs.py $< $(BENCH_OUT)/hot.txt $(HOT_ARGS)

clean:
	rm -rf $(OUT) $(BENCH_OUT)

-include $(CORE_OBJS:.o=.d) $(TEST_BINS:=.d)
.PHONY: all build run clean bench bench-m4 audio-tables peak-trace bench-m4-hot

# USB mass-storage endpoint driver (usb_msc_driver_test.cpp): the device's
# third_party/teensy3/usb_msc_impl.h against a simulated USB controller.
USB_MSC_FLAGS := -D__MK66FX1M0__ -DF_CPU=180000000 -DUSB_MIDI_SERIAL -DOT_USB_MSC=1 -DOT_USB_VID=0x1209 \
                 -DOT_USB_PID=0x0001 -I../third_party/teensy3
$(OUT)/usb_msc_driver_test: usb_msc_driver_test.cpp ../third_party/teensy3/usb_msc_impl.h ../third_party/teensy3/usb_msc.h $(OUT)/libcore.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(USB_MSC_FLAGS) $< $(OUT)/libcore.a $(LDFLAGS) -o $@
