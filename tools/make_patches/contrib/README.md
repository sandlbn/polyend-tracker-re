# Contributed patches

Patches made on the Tracker by OpenTracker users, shipped in the preset pack as they were saved
(`make patches` copies `contrib/<MODEL>/` into `Patches/<MODEL>/` for the models of the profile).
`tests/patches_test.cpp` loads each one through Load Patch and plays it like the generated
presets of its engine: audible, at least 1 dB below full scale, no clipping as saved or with
all macros at either extreme.

## A-Type (35), by Itay D

Play at Poly 1 (Keyboard Duo splits two held notes). "12dB": the same patch with the 2-pole
(12 dB) low-pass instead of the 4-pole.

| Group | Patches |
|---|---|
| Bass | Slow Wobble Bass (+ 12dB), Square Pluck Bass, Resonant Drone Bass, Juicy Sustain Bass, Fat PWM Bass (+ 12dB), Power Bass (+ 12dB), Breathy Solo Bass, Soft Acid Bass |
| Brass | Classic Brass, 80s Brass Stab, Bright Brass |
| Lead | Resonant Saw Lead, Dance Lead (+ 12dB), Easy Lead (+ 12dB), Ring Funk Lead, PWM Lead (+ 12dB), Stab Lead (+ 12dB) |
| Pad | Essential Pad, Molten Air, Orbit Pad, Pad-O-Matic, Random Filter Pad |
| Pluck, strings, synth | Bubble Pluck, Disco Strings, Symphonic Strings, Basic Arp, Fifth Saw Chord, Dream Stab |
