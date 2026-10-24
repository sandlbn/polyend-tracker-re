# OpenTracker, semi fork (2.2.1-semi)

A fork of [sandlbn/polyend-tracker-re](https://github.com/sandlbn/polyend-tracker-re)
(OpenTracker 2.2.1) for the original Polyend Tracker, with generators and tools
from the semi plug-in family (Acid, Call & Response, Breakbeat / BBCut,
TURNAROUND, GENOTYPE / TILT, TRANSCODE / FRAMELOSS, CIPHER, DATASCREEN).

Image: `PolyendTrackerOTs221.ptf` (24 characters, installs like the upstream
image: copy to `/Firmware`, Config > Firmware update). Keep the stock
`PolyendTracker_1.9.2.ptf` (and the upstream `PolyendTrackerOTv221.ptf`) in
`/Firmware` to go back.

Everything a generator writes is ordinary steps and step FX, so a pattern made
with this fork plays the same on stock 1.9.x and on the Tracker Mini. The new
play modes (B1..B10), effects and themes are fork-only.

## Generate (Pattern, page 3, F3)

Edit mode on, Pattern page 3 (More, More), **F3 Generate**. Laid out like Fill:
**F1** the generator, **F2..F7** its parameters (lists or bars; pads pick a list
item or spread over the range), **F8** Generate, **Pattern** or **Rec** closes.
The target is the selection, or the cursor track over the whole pattern with a
single-cell cursor. One undo step per Generate. **Seed** 0 is random; any other
seed rebuilds the same result.

| Generator | What it writes | Parameters |
|---|---|---|
| **Acid** | a 303 line in the project scale (TB-3PO idea): Glide on slid notes, Volume 100 on accents, Gate on short notes | Density, Accent, Slide, Range (1-3 oct), **Mutate**, Seed |
| **Call+Resp** | a call phrase and its answer (port of Nordl3's Call / Response Generator: 46 styles, 38 answer strategies) | Style, Answer, Density, Layout (Call+Answer in halves / Answer mine: the first half you wrote is the call / Two tracks), Seed |
| **BBCut** | Nick Collins' BBCut procedures (via BBenCut, the Ableton extension) as hits of a **sliced** instrument: cut offsets pick slices, cuts shorter than a step become Roll FX, Micro-move for off-grid starts | Proc (Classic, ChooseCut, ChooseBlk, WarpCut, SQPusher1, SQPusher2, CageCut, Timeline, Permute, ThrashCut), A / B (per procedure), Source (Auto from the sample length at the song tempo, or 1 beat .. 4 bars), Seed |
| **Turnaround** | phrase-end edits on a budget: the end of every phrase, half and quarter phrase gets a tail of edits | Phrase (1-8 bars), Budget (steps), Style (Rolls, Stutter, Drop, Reverse, Build, Mix), Intensity, Seed |
| **Genotype** | learns the selected steps and plays a variation made only of them | Mode (Markov walk / Ball: a bouncing TILT ball / both), Identity (chance to keep a step), Order (1-2), Variety, Seed |
| **Station** | a numbers-station line from the radio: RDS name, PI code, frequency read as digits (letters by alphabet position), in groups of five | Source, Mode (Notes: scale degrees / Slices: digit d plays slice d), Rate, Groups, Repeat, Seed |

BBCut wants a sliced sample: Auto Slice (Breaks detector, below) or equal
slices in Sample Playback. With Beat Slice the hits are notes 24 + slice; in
Slice mode a unity note and the Slice FX. For **Station / Slices**, record or
load a voice counting 0..9 and give it 10 slices: the Tracker reads the station.

### Live

- **Acid Mutate** (0 = Lock): the generated line re-rolls part of its steps at
  every loop end while playing, like TB-3PO's mutate. Two tracks at a time (A / B);
  generate again with Mutate 0 to lock. "LIVE" shows in the popup.
- **B1..B10 play modes**: Performance FX "Pattern Play Mode" has ten more modes
  after C20, one per BBCut procedure (same order as the Proc list). They reorder
  any track's own steps with BBCut cuts and rolls, in Performance mode. A new cut
  set is built every time the sequencer stops.

## Sample Editor effects

| Effect | Parameters |
|---|---|
| **Transcode** | Codec (u-law, A-law, G.726 40k / 32k / 24k / 16k), Rate (8 / 11 / 16 / 22 kHz), Generations (1-16 tandem passes with a D/A-A/D hop between), Bit errors (per 10k bits), Loss (10 ms packets), Conceal (Silence / Repeat / Glitch), Mix |
| **DCT Crush** | Block (64-512), Keep (% strongest bins), Bits (magnitude quantiser), Freeze (% frames that repeat the last spectrum), Mix |

G.726 follows the ITU algorithm in the classic Sun g72x structure (SNR on a busy
signal: 40k 34 dB, 32k 29 dB, 24k 23 dB, 16k 17 dB; u/A-law 37 dB).

## Config

- **General > Auto slice**: Stock (the original detector, 232 ms apart at least)
  or **Breaks** (spectral-flux onsets, 3 ms steps, 46 ms spacing, up to 48 slices,
  the strongest kept): every hit of a 170 BPM break.
- **General > Display theme**: 16 themes. Original, Monochrome, Anniversary,
  then Early Cyber, Corporate, Amber CRT, Green Phosphor, Thermal, Workstation,
  Datamatic, Dubplate, Rave Flyer, Jungle, Ice, Sunset, Teletext. The new ones
  recolour Original (Workstation: Monochrome) in RAM; the stock config byte keeps
  the base theme, so a stock firmware still shows something valid.
- Both live in the OpenTracker config extension record (now 4 bytes).

## Radio (Sample Recorder, source Radio)

- RDS is decoded (programme service name and PI): the station name shows on the
  right of the title.
- **Shift + seek** (F3 / F4) scans the band: seek, stay 4 s, seek on. "SCAN"
  in the title; a seek key without Shift stops it. Record while it scans for a
  radio collage.

## Data screen (File, F7 "Data")

In images without Games (the release image), File F7 opens an Ikeda /
datamatics style view of what plays: **F1 Barcode** (one column per step, one
row per track), **F2 Matrix** (16 tracks x 32 steps around the playhead),
**F3 Numbers** (raw hex step data and counters). F8 or Play starts / stops. The
pads show the next 12 steps of the four track groups.

## Not done (and why)

- **New fonts** (Departure Mono, Ark Pixel, DSEG): the image is at 698 KB of the
  720 KB flash gate; a second font set does not fit next to the rest.
- **Open303 engine**: the ACD engine plays the acid lines; a second 303 engine
  needs its own QEMU benchmark round first.

## Building and checks

The fork builds **without Games by default** (`OT_GAMES ?= 0`): with Games the
image is 745 KB, over the 720 KB flash gate. The CI workflow checks
`build/nogames/` accordingly. `make OT_GAMES=1` still builds the Games image
for experiments.

```
make VERSION=2.2.1                      # build/nogames/PolyendTrackerOTs221.ptf
make host-test                          # all host tests (gen_test, sample_fx_ot_test, ot_fork_misc_test, ...)
make perf-gate                          # QEMU instruction counts + flash size
make -C sim                             # desktop simulator
```

The fork's audio path is unchanged: its QEMU instruction counts equal upstream
built with the same compiler (within 0.02 %). With Ubuntu's arm-none-eabi GCC
13.2 both upstream and the fork exceed three PERC limit lines by about 2 %
(compiler code generation, not this fork); build the release image with the
same toolchain as the upstream releases where possible.

Sources: TB-3PO idea (Phazerville Hemisphere / charlesvestal schwung-tb3po),
Nordl3 Call / Response Generator (MIT), bencodec/BBenCut and Nick Collins'
BBCut, ITU-T G.711 / G.726, Silicon Labs AN230 (RDS registers).
