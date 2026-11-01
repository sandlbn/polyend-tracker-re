# Build profile (docs/PLAN_3X.md §7.3), included by the device, host-test, sim
# and tool Makefiles after they set ROOT (the reimpl/ directory).
#   make PROFILE=core                 -> image without modules
#   make MODULES_DIR=/path/to/repo    -> where the profile's module paths point
# tools/profile.py runs at parse time and writes build/<profile>/gen/
# (modules.mk, ot_profile.h, module_registry.cpp) when they change.
PROFILE     ?= full
MODULES_DIR ?= $(abspath $(ROOT)/../../opentracker-synths)
GEN         := $(abspath $(ROOT)/build/$(PROFILE)/gen)
export PROFILE MODULES_DIR
ifeq ($(wildcard $(ROOT)/profiles/$(PROFILE).toml),)
$(error no profile '$(PROFILE)' (reimpl/profiles/*.toml))
endif
PROFILE_GEN_RC := $(shell python3 -I $(ROOT)/tools/profile.py gen --profile $(ROOT)/profiles/$(PROFILE).toml \
                     --modules-dir $(MODULES_DIR) --out $(GEN) 1>&2; echo $$?)
ifneq ($(PROFILE_GEN_RC),0)
$(error tools/profile.py failed for profile '$(PROFILE)' (MODULES_DIR=$(MODULES_DIR)))
endif
include $(GEN)/modules.mk
# The synth pack: the test songs, preset tool and engine benches need all five.
SYNTH_PACK_IDS  := ACD1 FAT1 VAP1 PRC1 WTF1
HAVE_SYNTH_PACK := $(if $(filter-out $(MOD_IDS),$(SYNTH_PACK_IDS)),,1)
# The synth modules with presets (tools/make_patches/presets.cpp): the preset tool and
# tests/patches_test.cpp run when the profile has any of them.
PRESET_MODS     := ACD1 FAT1 VAP1 PRC1 WTF1 JU6S DTY1 CST1 ATY1 TTY1
HAVE_PRESET_MOD := $(if $(filter $(PRESET_MODS),$(MOD_IDS)),1,)
MOD_DEFS        := $(foreach i,$(MOD_IDS),-DOT_MOD_$(i)=1)
vpath %.cpp $(MOD_SRC_DIRS) $(GEN) $(MOD_TEST_DIRS)
