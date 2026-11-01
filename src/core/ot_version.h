// The version text the user sees (docs/PROFILES.md "The flavours"): OT_VERSION
// from the Makefile (VERSION) plus the profile's flavour name, display_name in
// profiles/<name>.toml (OT_PROFILE_DISPLAY_NAME, tools/profile.py). String
// literals only: the text is in flash (.rodata), no RAM.
#pragma once
#include "ot_profile.h"

#ifndef OT_VERSION
#define OT_VERSION "dev"
#endif

// "3.0.0 Techno": diag / selftest log lines.
#define OT_VERSION_TEXT OT_VERSION " " OT_PROFILE_DISPLAY_NAME
// "OpenTracker 3.0.0 Techno": Config > Firmware > Current version, Licenses page.
#define OT_VERSION_LINE "OpenTracker " OT_VERSION_TEXT
// "v3.0.0 Techno": the boot splash (bottom left).
#define OT_SPLASH_VERSION "v" OT_VERSION_TEXT
