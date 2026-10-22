# Per-track 4-band EQ (OpenTracker extension)

An EQ Eight-style parametric insert on each of the eight sample tracks. Four bands per track; each band has a
type, frequency, gain and Q, and an on/off switch.

## Using it

Two ways in:

* **Pattern screen:** press **More** (F8) until the third page, then press **F3 Track EQ**. It opens the EQ of the track under the cursor (tracks 1-8; F3 is greyed on tracks 9-16). **Pattern** returns to the pattern.
* **Track Mixer:** press **Master** to reach the Track Mixer (page 2 of 3), **hold a track key F1..F8**, and press **Insert**. The key does not mute. **Master** returns to the Track Mixer.

The Master cycle itself is the stock three pages.

| Control | Action |
| --- | --- |
| Left / Right | previous / next track (1..8) |
| F1..F4 | select band 1..4; pressing the selected band again switches it on / off |
| F5..F8 | choose what the jog wheel edits: Type, Freq, Gain, Q |
| Jog wheel / Up / Down | edit the chosen value (editing a band switches it on) |
| Shift + jog | coarse steps |
| Delete | reset the selected band to its default |

Defaults: band 1 is a Low Cut (80 Hz), bands 2 and 3 are Bells (500 Hz, 2.5 kHz), band 4 is a High Cut (12 kHz); all start off. Any band can be changed to any type.

Band types: Bell, Low Shelf, High Shelf, Low Cut, High Cut, Notch. Cut and Notch have no gain (as in EQ Eight).

Slope: on bands 1 and 4 only, when the type is Low Cut or High Cut, the F7 slot reads **Slope** instead of Gain and the
jog wheel toggles 12 dB/oct (one biquad) and 48 dB/oct (8th-order Butterworth, four biquads). At 48 dB, Q shapes only the
last (resonant) stage, by the square root of its ratio to Butterworth, so the peak stays close to a 12 dB cut's. Changing
the type away from a cut clears the slope. A track uses at most 10 biquad stages (4 + 1 + 1 + 4); with both cuts at
48 dB that is roughly 2 to 2.5 % of the block budget per mono track (estimate, not measured on hardware). Steps: frequency 1/24 octave (coarse 1/6), gain 0.5 dB (coarse 3 dB), Q x1.06 (coarse x1.5).
Ranges: 20 Hz..20 kHz, +-15 dB, Q 0.10..18.00.

The graph shows the combined response of the four bands (log frequency, +-15 dB) and a marker per band.

## Signal path

voice -> envelope -> bit crusher -> overdrive -> filter -> **track EQ** -> delay / reverb sends -> volume -> pan.
The sends therefore carry the EQ'd sound. A stereo sample runs the same settings on both channels (separate filter
state). A band at 0 dB (bell / shelves) or switched off costs nothing; with no active band the voice takes exactly the
same code path as before, so output is bit-identical to a build without the EQ.

## Saving

project.mt is a fixed-size stock file and is not touched. The EQ is saved in `track_eq.ote` (268 bytes, versioned,
CRC-32) next to it, in the project folder and in the Workspace (autosave). No file = EQ off. Projects that never
used the EQ get no file. Stock firmware ignores the extra file. Format: `src/core/formats/track_eq_file.h`.

## Sample Editor: EQ4 effect

The same four bands are in the Sample Editor's effect list as **EQ4**, right after Equalizer. Like the other
effects it is offline and destructive: Preview, then Apply on the Start..End selection (stereo samples run both
channels), with the usual one-level Undo. It runs in single precision like the real-time EQ (the M4 has no hardware double; a double loop was slow on long samples). Against a double reference the difference is 1-2 LSB at normal settings and up to about 18 LSB (-65 dBFS) at extreme 20-50 Hz settings. It uses the track EQ's own filters, so the curve you see is the sound you get.
A long run shows a progress bar (it moves every few percent). When a preview plays to the end, F7 goes back from Stop to Preview (this applies to every effect). Nothing is saved: the settings reset each time the editor is entered, as for every other effect.

* Page 1: pick EQ4 in the list, F6 opens the bands page.
* Bands page (page 2): F1..F4 = Band 1..4. A press selects the band and switches it on / off; the column shows its
  type, frequency, gain (or cut slope) and Q. Left / Right select a band without switching it. Delete returns the
  band to its factory layout, off. F5 = Params, F6 = Back, F7 = Preview, F8 = Apply.
* Band parameters (page 3): the response graph with F1..F4 = Type, Freq, Slope (cuts of bands 1 and 4) or Gain, Q;
  the jog wheel edits (Shift = coarse), Left / Right pick the parameter, Delete resets it. F5 = next band,
  F6 = Back (to the bands page), F7 = Preview, F8 = Apply. Editing a band switches it on.
* With no band on (or only neutral ones), Apply leaves the audio untouched.

`sample_fx.h` gets a 24-value parameter block for EQ4 (`kMaxParams` 9 -> 24, `eq4Pack` / `eq4Unpack`); the graph
and value texts are shared with the Track EQ page in `ui/eq_view.{h,cpp}`. Tests: `sample_eq4_test`.

## Code

* `src/core/audio/track_eq.{h,cpp}` - parameters, RBJ cookbook biquads (transposed direct form II, float), response.
* `src/core/audio/voice*.{h,cpp}`, `engine.{h,cpp}` - insert point and per-block parameter hand-over.
* `src/core/ui/screen_mixer.cpp` - the Track EQ page (MixerScreen page 3).
* `src/core/app/project_io.cpp`, `app.{h,cpp}` - persistence hooks (new / open / save / autosave / workspace load).
* Tests: `track_eq_test`, `track_eq_ui_test`, `track_eq_file_test`; benchmark lines in `audio_bench`.
