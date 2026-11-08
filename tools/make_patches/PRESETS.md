# OpenTracker preset pack

Our own synth patches for the ACD, FAT, VAP, WTFM and PERC engines of OpenTracker 2.x and the JU6,
D-Type, CS-Type, A-Type and T-Type module engines of OpenTracker 3.x, written by
`reimpl/tools/make_patches` (`make -C reimpl patches`) through the firmware's own Save As path.
Every patch starts from the engine's built-in Init patch; no factory patch is used.
The JU6, D-Type and T-Type sections come from `PROFILE=nofm`, the CS-Type and A-Type sections from `PROFILE=classic`, the 8-Type, Hand-Type, B-Type and Arc-Type sections from `PROFILE=retro` (not in profile `full`).

Files: `Patches/<MODEL>/<name>.patch`. Copy the `Patches` folder to the root of the SD card,
then use Load Patch on a synth page.

Macros: M1-M6 (FX CC A-F). A bipolar macro (+-) at 63 and a unipolar macro at 0 play the
patch as saved. M7 is Velocity (Volume FX), as in the Init patch. Levels are voiced for the
default slot gain (-9 dB); `tests/patches_test.cpp` checks that every patch is audible, stays
at least 1 dB below full scale (VAP, JU6, CS-Type, FAT and WTFM Poly 4: 4-voice chords; A-Type: also a
Duo pair from two tracks; PERC: five drums at once, toms alone; T-Type: every drum alone, then eight
at once, toms
alone) and does not clip with all macros at either extreme.

Poly: the suggested slot polyphony (1 = mono, 4 = chords). It is a slot setting on the synth
page, not part of the patch.

## ACD (12)

| Patch | Description | Poly | Macros M1-M6 |
|---|---|---|---|
| OT Rubber Acid | Round saw acid line: RD3 ladder, fast filter envelope, legato glide. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Glide, Sub |
| OT Squelch Box | Square-wave squelch: high ladder resonance, snappy envelope. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Wobble, Glide |
| OT Deep Sub | Clean sub bass: triangle sub with a little square on top. | 1 | Tone (+-), Body, Punch (+-), Release (+-), Glide, Grit |
| OT Rubber Bass | Pulse + sub bass with a short filter bounce. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Width (+-), Sub (+-) |
| OT Night Pluck | Saw + square pluck, dark and short; good with delay. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Release (+-), Vibrato |
| OT Glass Pluck | Narrow pulse pluck with a pulse-width sweep; bright and glassy. | 1 | Cutoff (+-), Resonance (+-), Width (+-), PW Sweep (+-), Decay (+-), Release (+-) |
| OT Acid Lead | Singing mono lead: saw + pulse, slow filter swell, vibrato, glide. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Vibrato (+-), Glide, Release (+-) |
| OT Wobble Acid | Ladder bass with a triangle LFO on the cutoff (wobble). | 1 | Cutoff (+-), Resonance (+-), Wobble (+-), Rate (+-), Env Amt (+-), Square |
| OT Hollow Bass | Square bass with a two-octave sub; hollow and woody. | 1 | Cutoff (+-), Resonance (+-), Width (+-), Sub (+-), Env Amt (+-), Release (+-) |
| OT Bent Acid | Dirty acid: pulse sweep, a touch of noise, glide on every note. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Noise, Glide (+-) |
| OT Sub Pluck | Short sub-heavy pluck bass for house and garage lines. | 1 | Cutoff (+-), Punch (+-), Decay (+-), Saw (+-), Resonance (+-), Release (+-) |
| OT Wah Lead | Saw lead with a slow resonant filter wah. | 1 | Cutoff (+-), Resonance (+-), Wah Depth (+-), Wah Rate (+-), Release (+-), Glide |

## FAT (8)

| Patch | Description | Poly | Macros M1-M6 |
|---|---|---|---|
| OT Super Saw | The classic seven-saw stack: wide detune, open OB filter, a little vibrato. | 4 | Cutoff (+-), Fatness (+-), Brightness (+-), Resonance (+-), Attack (+-), Release (+-) |
| OT Wide Pad | Slow, dark unison pad: soft attack, a filter swell and a slow free-running sweep. | 4 | Cutoff (+-), Fatness (+-), Sweep, Attack (+-), Release (+-), Brightness (+-) |
| OT Hoover Lead | Mono hoover: full stack part pulse, deep vibrato, legato glide. | 1 | Cutoff (+-), Timbre (+-), Vibrato (+-), Glide (+-), Resonance (+-), Fatness (+-) |
| OT Trance Pluck | Short unison pluck: resonant OB filter snapped open by its envelope. | 4 | Cutoff (+-), Env Amt (+-), Decay (+-), Resonance (+-), Fatness (+-), Release (+-) |
| OT Thick Bass | Mono unison bass: three saws, a touch of pulse, MG filter bounce, short glide. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Fatness (+-), Timbre, Glide |
| OT Dark Strings | String ensemble: a warm stack under a 12 dB filter, swell and vibrato. | 4 | Cutoff (+-), Brightness (+-), Vibrato (+-), Attack (+-), Release (+-), Fatness (+-) |
| OT Square Stack | Mono lead of detuned squares (Timbre 1), OB 24 filter, overlap glide. | 1 | Cutoff (+-), Timbre (+-), Resonance (+-), Env Amt (+-), Fatness (+-), Glide |
| OT Detuned Keys | Keys from a light stack: free-running phases, a decaying filter, a breath of noise. | 4 | Cutoff (+-), Env Amt (+-), Decay (+-), Fatness (+-), Timbre (+-), Release (+-) |

## VAP (16)

| Patch | Description | Poly | Macros M1-M6 |
|---|---|---|---|
| OT Velvet Pad | Warm detuned-saw pad, slow attack, a gentle tempo-synced filter drift. | 4 | Cutoff (+-), Resonance (+-), Attack (+-), Release (+-), Motion, Detune (+-) |
| OT Glass Pad | Airy triangle + pulse pad an octave apart, slow pulse-width shimmer. | 4 | Cutoff (+-), Shimmer (+-), Attack (+-), Release (+-), PWM Depth, Detune (+-) |
| OT Warm Strings | String ensemble: detuned saws, soft swell, gentle vibrato. | 4 | Cutoff (+-), Attack (+-), Release (+-), Vibrato (+-), Detune (+-), Env Amt (+-) |
| OT Brass Stab | Punchy synth brass: ladder filter with a quick envelope bite. | 4 | Cutoff (+-), Env Amt (+-), Attack (+-), Release (+-), Resonance (+-), Detune (+-) |
| OT Sync Lead | Hard-sync lead: an envelope sweeps the synced oscillator; glide and vibrato. | 1 | Cutoff (+-), Sync Sweep (+-), Sweep Time (+-), Vibrato, Glide, Release (+-) |
| OT PWM Lead | Two pulse waves with independent width LFOs; thick and moving. | 1 | Cutoff (+-), Resonance (+-), PWM Depth (+-), PWM Rate (+-), Glide, Release (+-) |
| OT Soft Pluck | Mellow triangle + saw pluck with an octave on top. | 4 | Cutoff (+-), Env Amt (+-), Decay (+-), Release (+-), Resonance (+-), Tone (+-) |
| OT Harp Pluck | Bright pulse + triangle pluck with a long ring; phase-reset attack. | 4 | Cutoff (+-), Env Amt (+-), Decay (+-), Width (+-), Release (+-), Resonance (+-) |
| OT Fat Bass | Saw + square an octave down through the ladder filter; mono, legato glide. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Sub (+-), Glide |
| OT Bell Keys | Struck bell: two triangles at an inharmonic interval, bright strike, long ring. | 4 | Tone (+-), Strike (+-), Decay (+-), Release (+-), Ratio (+-), Chorus |
| OT Mellow Keys | Soft electric-piano-like keys with a gentle tremolo. | 4 | Cutoff (+-), Env Amt (+-), Decay (+-), Release (+-), Tremolo, Tone (+-) |
| OT Wind Sweep | Resonant band-passed noise, slowly swept over four bars. | 4 | Cutoff (+-), Resonance (+-), Sweep Depth (+-), Air (+-), Attack (+-), Release (+-) |
| OT Arp Blip | Short square + saw blip with phase reset; made for arpeggios and fast lines. | 4 | Cutoff (+-), Resonance (+-), Env Amt (+-), Length (+-), Octave (+-), Release (+-) |
| OT Unison Lead | Wide detuned-saw mono lead with glide and vibrato. | 1 | Cutoff (+-), Detune (+-), Vibrato, Glide, Resonance (+-), Release (+-) |
| OT Dark Drone | Low resonant drone, very slow filter and pitch drift; hold a note. | 4 | Cutoff (+-), Resonance (+-), Motion, Attack (+-), Release (+-), Detune (+-) |
| OT Organ Keys | Square + triangle an octave up, instant on/off, light vibrato. | 4 | Tone (+-), Drawbar (+-), Vibrato, Shape (+-), Release (+-), Detune (+-) |

## WTFM (30)

| Patch | Description | Poly | Macros M1-M6 |
|---|---|---|---|
| OT FM E-Piano | Classic FM electric piano: 1:1 operators, bright attack that mellows. | 4 | Brightness (+-), Feedback, Decay (+-), Release (+-), Tone (+-), Ratio 2 (+-) |
| OT Tine Piano | Bell-like tine on top of a soft body: the modulator at 12x, short decay. | 4 | Brightness (+-), Feedback, Decay (+-), Release (+-), Tone (+-), Ratio 2 (+-) |
| OT Mellow Tines | Darker, rounder FM piano for chords. | 4 | Brightness (+-), Feedback, Decay (+-), Release (+-), Tone (+-), Ratio 2 (+-) |
| OT Bright Keys | Glassy 2:1 keys with a sharp attack. | 4 | Brightness (+-), Feedback, Decay (+-), Release (+-), Tone (+-), Ratio 2 (+-) |
| OT FM Clav | Funky clavinet: saw-like carrier, 3:1 bite and a quick filter. | 1 | Brightness (+-), Feedback, Decay (+-), Release (+-), Tone (+-), Ratio 2 (+-) |
| OT FM Harpsi | Plucked harpsichord: 4:1 modulator, no sustain. | 4 | Brightness (+-), Feedback, Decay (+-), Release (+-), Tone (+-), Ratio 2 (+-) |
| OT FM Organ | Drawbar-style organ: hollow odd row, 2:1 FM, gentle vibrato. | 4 | Brightness (+-), Feedback, Decay (+-), Release (+-), Tone (+-), Ratio 2 (+-) |
| OT Perc Organ | Organ with a percussive click from a fast FM envelope. | 4 | Brightness (+-), Feedback, Decay (+-), Release (+-), Tone (+-), Ratio 2 (+-) |
| OT Glass Bell | Clear bell: 3.5:1 inharmonic partials that ring out. | 4 | Brightness (+-), Feedback, Decay (+-), Release (+-), Tone (+-), Ratio 2 (+-) |
| OT Tubular Bell | Deep church bell: 2.75:1 with a long, darkening tail. | 4 | Brightness (+-), Feedback, Decay (+-), Release (+-), Tone (+-), Ratio 2 (+-) |
| OT FM Marimba | Wooden mallet: 4:1 modulator, very short FM envelope. | 4 | Brightness (+-), Feedback, Decay (+-), Release (+-), Tone (+-), Ratio 2 (+-) |
| OT FM Vibes | Vibraphone: soft 4:1 strike and a motor tremolo. | 4 | Brightness (+-), Feedback, Decay (+-), Release (+-), Tone (+-), Ratio 2 (+-) |
| OT Kalimba | Thumb piano: 5:1 ping on a sine. | 4 | Brightness (+-), Feedback, Decay (+-), Release (+-), Tone (+-), Ratio 2 (+-) |
| OT Music Box | Tiny music box: an octave up, 6:1 sparkle. | 4 | Brightness (+-), Feedback, Decay (+-), Release (+-), Tone (+-), Ratio 2 (+-) |
| OT FM Bass | Punchy 1:1 FM bass with a decaying growl. | 1 | Brightness (+-), Cutoff (+-), Feedback, Decay (+-), Glide, Grit |
| OT Solid Bass | Round, solid 80s FM bass: modulator feedback, quick bite. | 1 | Brightness (+-), Cutoff (+-), Feedback, Decay (+-), Glide, Grit |
| OT Slap Bass | Slapped bass: 3:1 snap on attack, then a plain body. | 1 | Brightness (+-), Cutoff (+-), Feedback, Decay (+-), Glide, Grit |
| OT Wood Bass | Upright-style bass: soft 1:1 pluck, short sustain. | 1 | Brightness (+-), Cutoff (+-), Feedback, Decay (+-), Glide, Grit |
| OT Sub Thump | Deep sine sub with a short FM thump. | 1 | Brightness (+-), Cutoff (+-), Feedback, Decay (+-), Glide, Grit |
| OT Growl Bass | Wild, moving bass: folded carrier, LFO on the FM depth. | 1 | Brightness (+-), Cutoff (+-), Feedback, Decay (+-), Glide, Grit |
| OT FM Brass | Brass section: the FM depth swells in with the attack. | 4 | Brightness (+-), Feedback (+-), Cutoff (+-), Vibrato, Glide, Release (+-) |
| OT Square Lead | Hollow square lead with feedback edge and vibrato. | 1 | Brightness (+-), Feedback (+-), Cutoff (+-), Vibrato, Glide, Release (+-) |
| OT Reed Lead | Clarinet-like reed: the odd-harmonic row, soft vibrato. | 1 | Brightness (+-), Feedback (+-), Cutoff (+-), Vibrato, Glide, Release (+-) |
| OT Metal Lead | Screaming lead: folded row, heavy feedback, oversampled. | 1 | Brightness (+-), Feedback (+-), Cutoff (+-), Vibrato, Glide, Release (+-) |
| OT FM Strings | Soft string ensemble: saw-like carrier, slow attack, slow shape motion. | 4 | Brightness (+-), Cutoff (+-), Shape (+-), Attack (+-), Release (+-), Motion |
| OT Crystal Pad | Shimmering 3:1 pad, the FM depth drifting slowly. | 4 | Brightness (+-), Cutoff (+-), Shape (+-), Attack (+-), Release (+-), Motion |
| OT Choir Pad | Airy choir: the Add 5 row through a low-pass, slow swell. | 4 | Brightness (+-), Cutoff (+-), Shape (+-), Attack (+-), Release (+-), Motion |
| OT Hollow Pad | Hollow odd-harmonic pad that slowly opens. | 4 | Brightness (+-), Cutoff (+-), Shape (+-), Attack (+-), Release (+-), Motion |
| OT Noise Hit | Metallic noise burst: Add 11 row, full feedback, short. | 1 | Brightness (+-), Feedback (+-), Cutoff (+-), Vibrato, Glide, Release (+-) |
| OT Laser Zap | Falling FM zap: the pitch and FM depth drop together. | 1 | Brightness (+-), Feedback (+-), Cutoff (+-), Vibrato, Glide, Release (+-) |

## PERC (16)

Notes (MIDI numbers): kick 60, snare 62, hi-hats 64-67 (closed to open), cymbal 69, perc 71; any other
note plays the tom at that pitch. The `OT Kit SW` kits are synthwave / retrowave kits
(gated snares, sweeping toms); their per-drum Delay / Reverb sends sound only when the
instrument's own Delay / Reverb send is up (drum send = patch send x instrument send).

| Patch | Description | Poly | Macros M1-M6 |
|---|---|---|---|
| OT Kit Boom | Analog-style boom kit: long sine kick with a pitch drop, soft snare, clap. | 1 | Tone (+-), Decay (+-), Pitch (+-), Snap (+-), Drive (+-), Space (+-) |
| OT Kit Punch | Punchy club kit: clicky kick, bright snare, noisy hats, saturated bus. | 1 | Tone (+-), Decay (+-), Pitch (+-), Snap (+-), Drive (+-), Space (+-) |
| OT Kit Dusty | Lo-fi kit: 6-bit crush, rolled-off highs, warm low end. | 1 | Tone (+-), Decay (+-), Pitch (+-), Snap (+-), Drive (+-), Space (+-) |
| OT Kit Foundry | Industrial kit: folded kick, resonant toms, hard overdrive, metal hats. | 1 | Tone (+-), Decay (+-), Pitch (+-), Snap (+-), Drive (+-), Space (+-) |
| OT Kit FM Metal | FM kit: phase-modulated kick, toms and snare, FM hats and cowbell. | 1 | Tone (+-), Decay (+-), Pitch (+-), Snap (+-), Drive (+-), Space (+-) |
| OT Kit Minimal | Minimal kit: tight kick, rimshot-like snare, short hats, shaker on delay. | 1 | Tone (+-), Decay (+-), Pitch (+-), Snap (+-), Drive (+-), Space (+-) |
| OT Kit Studio | Tight acoustic-like kit: beater kick, ringing toms, crisp snare, washy cymbal. | 1 | Tone (+-), Decay (+-), Pitch (+-), Snap (+-), Drive (+-), Space (+-) |
| OT Kit Glitch | Glitch kit: bit-twisted bodies, crushed bus, delays on hats and shaker. | 1 | Tone (+-), Decay (+-), Pitch (+-), Snap (+-), Drive (+-), Space (+-) |
| OT Kit SW Gated | 80s gated kit: big snap-heavy snare with a hard-cut room tail, sweeping toms, Clap+. | 1 | Kick Decay (+-), Snare Gate (+-), Snare Tone (+-), Hat Decay (+-), Drive (+-), Room (+-) |
| OT Kit SW Outrun | Driving outrun kit: tight clicky kick, bright gated snare, open hats on delay, cowbell. | 1 | Kick Decay (+-), Snare Tone (+-), Hat Decay (+-), Echo (+-), Drive (+-), Room (+-) |
| OT Kit SW Darksynth | Dark heavy kit: saturated low kick, huge snare, resonant toms, hard overdrive. | 1 | Kick Decay (+-), Snare Gate (+-), Tom Sweep (+-), Hat Decay (+-), Drive (+-), Room (+-) |
| OT Kit SW Vapor | Vaporwave lo-fi kit: soft round kick, washed-out snare, dull hats, 6-bit crush. | 1 | Kick Decay (+-), Snare Tone (+-), Hat Decay (+-), Crush (+-), Echo (+-), Room (+-) |
| OT Kit SW Hex Toms | Electronic tom kit: singing pitch-sweep toms with stick noise, ping snare, clap. | 1 | Tom Sweep (+-), Tom Decay (+-), Tom Stick (+-), Snare Tone (+-), Drive (+-), Room (+-) |
| OT Kit SW Electro | Electro kit in the classic analog drum machine style: long boomy kick, snappy snare, ticking hats, roomy clap. | 1 | Kick Decay (+-), Snare Tone (+-), Hat Decay (+-), Pitch (+-), Drive (+-), Room (+-) |
| OT Kit SW Drive | Midtempo driving kit: overdriven punchy kick and snare, shaker on delay, wide toms. | 1 | Kick Decay (+-), Snare Tone (+-), Snare Gate (+-), Echo (+-), Drive (+-), Room (+-) |
| OT Kit SW Studio 80 | 80s studio drum-machine kit: short beater kick, crisp snare, airy hats, clap, crash. | 1 | Kick Decay (+-), Snare Tone (+-), Hat Decay (+-), Pitch (+-), Drive (+-), Room (+-) |

## JU6 (12)

JU6 is a module engine (3.x, module JU6S): pick it after PERC in the Synth page model list.
Its stereo chorus is built in, as on the instrument it is modelled on: the patch parameter
Chorus (Off / I / II, Edit Patch section CHO) runs once per synth slot on the slot's voices.
Pads and strings use II, brass, keys, plucks and leads I, the bass is dry; the Chorus macro
(where there is one) steps it up a mode. A JU6 patch saved before the chorus loads with
Chorus Off. Every JU6 patch is checked with 4-voice chords, chorus on.

| Patch | Description | Poly | Macros M1-M6 |
|---|---|---|---|
| OT Ensemble Strings | Saw + slow PWM ensemble pad, gentle attack and long release; Chorus II. | 4 | Cutoff (+-), Attack (+-), Release (+-), PWM (+-), Env Amt (+-), Vibrato |
| OT Poly Brass | Saw brass: the filter envelope opens on every chord, velocity to the VCF; Chorus I. | 4 | Cutoff (+-), Env Amt (+-), Attack (+-), Decay (+-), Resonance (+-), Chorus |
| OT PWM Pad | Pulse only, deep slow PWM, sub underneath: the classic moving pad; Chorus II. | 4 | Cutoff (+-), PWM (+-), LFO Rate, Attack (+-), Release (+-), Resonance (+-) |
| OT Sub Bass | Square + full sub through the bass-boost HPF, short filter thump; dry (Chorus Off). | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Release (+-), Chorus |
| OT Square Pluck | Saw + square pluck: fast filter decay, no sustain; Chorus I. | 4 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Release (+-), PWM (+-) |
| OT Arp Seq | Short resonant saw blip for arpeggios and sequences; HPF 2 keeps it out of the bass; Chorus I. | 4 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Release (+-), Sub |
| OT Gate Organ | Square + sub on the Gate VCA, open filter, delayed vibrato: organ-ish keys; Chorus I. | 4 | Cutoff (+-), Sub, PW, Vibrato, LFO Rate (+-), Resonance (+-) |
| OT Hoover Whoop | Saw + deep fast PWM + sub with a slow resonant filter whoop; Chorus II. | 4 | Cutoff (+-), Env Amt (+-), Resonance (+-), Attack (+-), LFO Rate (+-), Vibrato |
| OT Poly Lead | Saw + narrow pulse lead with delayed vibrato and key-tracked filter; Chorus I. | 4 | Cutoff (+-), Resonance (+-), Env Amt (+-), Vibrato, Release (+-), PWM (+-) |
| OT Glass Ring | Narrow pulse through a ringing, fully key-tracked filter: bell and glass; Chorus II. | 4 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Release (+-), PWM (+-) |
| OT Noise Sweep | Noise through a slow resonant filter sweep with LFO wobble: wind and risers; Chorus II. | 4 | Cutoff (+-), Resonance (+-), Env Amt (+-), Attack (+-), LFO > VCF, LFO Rate |
| OT Warm Keys | Saw + sub e-piano-like keys: velocity to the VCF, decaying tone, bass boost; Chorus I. | 4 | Cutoff (+-), Env Amt (+-), Decay (+-), Release (+-), Sub, Chorus |

## D-Type (12)

D-Type is a module engine (3.x, module DTY1, folder `Patches/DTYPE/`): pick it after JU6 in
the Synth page model list. It is a mono synth (three oscillators, noise, a 4-pole ladder):
set the slot's Poly to 1 for last-note priority, legato and glide. M1-M4 are the same on
every patch (Cutoff, Emphasis, Contour, Decay of both contours); Wheel (M5 where it is the
Wheel or Vibrato macro) brings in the modulation of VCO 3 or the noise. Leads and brass
(Fat Lead, Brass Section, Sync Bite) also scale the Contour with the velocity (M7).

| Patch | Description | Poly | Macros M1-M6 |
|---|---|---|---|
| OT Model Bass | Two saws at 16', half-open ladder with a short contour, legato glide 50 ms; M6 adds a 32' square. | 1 | Cutoff (+-), Emphasis (+-), Contour (+-), Decay (+-), Glide, Sub |
| OT Fat Lead | Three saws at 8', detuned, cutoff 2.5 kHz, legato (tied notes glide 100 ms, no re-attack); velocity to the contour. | 1 | Cutoff (+-), Emphasis (+-), Contour (+-), Decay (+-), Glide, Detune (+-) |
| OT Brass Section | Saw at 8' + square at 16', a slow filter attack (80 ms), vibrato from VCO 3 on the wheel (M5). | 1 | Cutoff (+-), Emphasis (+-), Contour (+-), Decay (+-), Vibrato, Attack (+-) |
| OT Sync Bite | Saw lead with VCO 3 on the keyboard as audio-rate FM and high emphasis: a hard, bright edge; M5 more FM. | 1 | Cutoff (+-), Emphasis (+-), Contour (+-), Decay (+-), Wheel (+-), FM Tune (+-) |
| OT Pluck Perc | Square + saw pluck: Decay switch Off (short release), no sustain, a fast contour. | 1 | Cutoff (+-), Emphasis (+-), Contour (+-), Decay (+-), Glide, Noise |
| OT Tom Thump | Triangle at 32' + noise into a ladder near self-oscillation, Key Track Full: a pitched drum. | 1 | Cutoff (+-), Emphasis (+-), Contour (+-), Decay (+-), Glide, Noise |
| OT Whistle Sine | No VCO: Emphasis 100 % makes the ladder the oscillator, in tune from C4 (Key Track Full). | 1 | Cutoff (+-), Emphasis (+-), Contour (+-), Decay (+-), Vibrato, Breath |
| OT Hollow Square | Squares at 8' and 16', Key Track 2/3, a gentle contour: a hollow, woody voice. | 1 | Cutoff (+-), Emphasis (+-), Contour (+-), Decay (+-), Glide, Detune (+-) |
| OT Feedback Growl | Saws at 32' and 16' with the output fed back into the mixer (Feedback 80 %), cutoff 300 Hz. | 1 | Cutoff (+-), Emphasis (+-), Contour (+-), Decay (+-), Glide, Feedback (+-) |
| OT Ladder Noise | Noise alone through a resonant ladder with a full, fast contour: snares, claps, zaps. | 1 | Cutoff (+-), Emphasis (+-), Contour (+-), Decay (+-), Glide, Pink |
| OT Wind Drone | Pink noise + triangle at 32', slow contours, a slow VCO 3 moving the filter on the wheel. | 1 | Cutoff (+-), Emphasis (+-), Contour (+-), Decay (+-), Wheel (+-), Rate (+-) |
| OT Lab Sweep | Saw + wide pulse, VCO 3 at 16' as audio-rate FM, Emphasis 90 %, a 2 s filter contour sweep. | 1 | Cutoff (+-), Emphasis (+-), Contour (+-), Decay (+-), Wheel (+-), Mod Mix |

## T-Type (12)

T-Type is a module engine (3.x, module TTY1, folder `Patches/TTYPE/`): a drum synth in the
classic analog drum machine style, pick it after D-Type in the Synth page model list. One
patch is one kit; play it at Poly 1 (the drums overlap in the one voice). Notes: kick 60, rim
61, snare 62, clap 63 and 71, hats 64-67 (closed to open; a closed hat cuts an open one),
cowbell 68, crash 69, ride 70 (one cymbal), low / mid / high tom 48-51 / 52-55 / 56-59 (a
semitone up per note); notes from 72 up repeat 60-71, notes below 48 play the low tom. Every
drum has a Character (punchy hybrid / deep analog / small tight). The Volume FX is the hit's
velocity through Accent. Every kit has the same macros: M1 Kick Decay, M2 Hat Decay, M3 Tune
(kick, snare, toms), M4 Drive, M5 Snappy (snare noise, clap tail), M6 Accent. The per-drum
sends sound when the instrument's own Delay / Reverb send is up.

| Patch | Description | Poly | Macros M1-M6 |
|---|---|---|---|
| OT Kit Hybrid Punch | Punchy hybrid kick with a click, a cracking snare, noisy hats, a wide clap: the all-round techno kit. | 1 | Kick Decay (+-), Hat Decay (+-), Tune (+-), Drive, Snappy (+-), Accent (+-) |
| OT Kit Deep Boom | A long, deep, pure kick (Boom), soft snare, round toms, metal hats and a crash: deep house and dub techno. | 1 | Kick Decay (+-), Hat Decay (+-), Tune (+-), Drive, Snappy (+-), Accent (+-) |
| OT Kit Tight | Small and tight: a knocking kick, a thin snare, short toms and hats, a two-burst clap, a low cowbell. | 1 | Kick Decay (+-), Hat Decay (+-), Tune (+-), Drive, Snappy (+-), Accent (+-) |
| OT Kit Industrial | Overdriven: the kick's drive and the kit Drive up, a bright cracking snare, metal hats, a thin cymbal, popping rim. | 1 | Kick Decay (+-), Hat Decay (+-), Tune (+-), Drive, Snappy (+-), Accent (+-) |
| OT Kit Minimal Tech | Minimal: a short punchy kick, a loud stick rim, thin closed hats with an echo, a clanging cowbell, a dry clap. | 1 | Kick Decay (+-), Hat Decay (+-), Tune (+-), Drive, Snappy (+-), Accent (+-) |
| OT Kit Acid House | A booming kick, a wide clap with a long tail, a soft snare, noisy open hats, a bright cowbell, a ride. | 1 | Kick Decay (+-), Hat Decay (+-), Tune (+-), Drive, Snappy (+-), Accent (+-) |
| OT Kit Dub | Dub techno: deep kick, a soft snare and a popping rim into the delay and the reverb, metal hats with an echo. | 1 | Kick Decay (+-), Hat Decay (+-), Tune (+-), Drive, Snappy (+-), Accent (+-) |
| OT Kit Electro | Electro: a knocking kick with a long sweep, thin snare, a tight clap, sweeping toms, a bright cowbell. | 1 | Kick Decay (+-), Hat Decay (+-), Tune (+-), Drive, Snappy (+-), Accent (+-) |
| OT Kit Hard | Hard techno: a driven punchy kick with a loud click, kit Drive, a snappy snare, a dry clap, bright noisy hats. | 1 | Kick Decay (+-), Hat Decay (+-), Tune (+-), Drive, Snappy (+-), Accent (+-) |
| OT Kit Dusty Tape | Lo-fi: a dull booming kick, a dark soft snare, dark metal hats, a ringing cymbal, narrow, light accents. | 1 | Kick Decay (+-), Hat Decay (+-), Tune (+-), Drive, Snappy (+-), Accent (+-) |
| OT Kit Broken | Broken beat: a knocking kick, a cracking snare, short toms, a ticking rim, a low cowbell, a tight clap. | 1 | Kick Decay (+-), Hat Decay (+-), Tune (+-), Drive, Snappy (+-), Accent (+-) |
| OT Kit Rave | Rave: a long punchy kick with drive, a wide clap into the reverb, a cracking snare, a ringing ride, open hats. | 1 | Kick Decay (+-), Hat Decay (+-), Tune (+-), Drive, Snappy (+-), Accent (+-) |

## CS-Type (12)

CS-Type is a module engine (3.x, module CST1, folder `Patches/CSTYPE/`): pick it after D-Type
in the Synth page model list. Every note plays two layers (I and II), each a VCO into a
resonant high-pass and low-pass and its own VCA; Poly is the slot setting (4 for chords).
M1 Brilliance, M2 Resonance, M3 Touch (after-touch: FX CC c brings it in), M4 Release,
M5 Ribbon (pitch, +-), M6 the patch's own. Velocity (M7) also scales each layer's filter
attack level (Init -> Bril).

| Patch | Description | Poly | Macros M1-M6 |
|---|---|---|---|
| OT Two Layer Brass | Layer I a saw, layer II saw + pulse at 16', both low-passes opened by their contours; velocity to the attack levels, Touch opens them. | 4 | Brilliance (+-), Resonance (+-), Touch, Release (+-), Ribbon (+-), Layer II (+-) |
| OT Glass Strings | Two pulse layers (8' and 4') swept by their own slow PWM, a high-pass taking the body out, slow attack and long release. | 4 | Brilliance (+-), Resonance (+-), Touch, Release (+-), Ribbon (+-), PWM (+-) |
| OT Ring Mod Bells | Two sines an octave apart through the ring modulator, its rate swept down at the start, a decaying bell envelope. | 4 | Brilliance (+-), Resonance (+-), Touch, Release (+-), Ribbon (+-), RM Depth (+-) |
| OT Pressure Lead | Poly 1 lead: saw + pulse and a detuned saw; Touch (M3, FX CC c) brings in vibrato, opens the filter and pushes the level. | 1 | Brilliance (+-), Resonance (+-), Touch, Release (+-), Ribbon (+-), Glide |
| OT Noise Wash | Noise in both layers through resonant filters, layer II high-passed, the cutoffs moved by a slow sub-osc sine: surf and wind. | 4 | Brilliance (+-), Resonance (+-), Touch, Release (+-), Ribbon (+-), Wave Speed (+-) |
| OT Soft Keys | A dark saw + sine layer with a piano-like decay and a 4' sine layer for the attack; velocity to brightness and level. | 4 | Brilliance (+-), Resonance (+-), Touch, Release (+-), Ribbon (+-), Tine (+-) |
| OT Film Pad | A saw at 8' and a PWM pulse at 16', slow attacks, a long release, a gentle sub-osc drift on the pitch. | 4 | Brilliance (+-), Resonance (+-), Touch, Release (+-), Ribbon (+-), Drift |
| OT Reso Sweep | One saw layer into a low-pass at 85 % resonance swept up by a slow contour from below (IL negative). | 4 | Brilliance (+-), Resonance (+-), Touch, Release (+-), Ribbon (+-), Sweep (+-) |
| OT Hard Touch | A pulse and a 16' saw: the velocity sets the brightness and the level strongly; play it with Volume FX. | 4 | Brilliance (+-), Resonance (+-), Touch, Release (+-), Ribbon (+-), Width |
| OT Whistle Duet | No waves: both low-passes sing (resonance at the top), layer II a fifth above, following the keys by half. | 4 | Brilliance (+-), Resonance (+-), Touch, Release (+-), Ribbon (+-), Interval (+-) |
| OT Dual Filter Growl | A 16' saw between a resonant high-pass and low-pass, a 16' pulse layer, the low-passes wobbled by the sub-osc. | 4 | Brilliance (+-), Resonance (+-), Touch, Release (+-), Ribbon (+-), Growl |
| OT Choir Vox | A breathy pulse and a saw each between resonant band-limiting filters: vowel-like formants; a slow vibrato from the sub-osc. | 4 | Brilliance (+-), Resonance (+-), Touch, Release (+-), Ribbon (+-), Vowel (+-) |

## A-Type (12)

A-Type is a module engine (3.x, module ATY1, folder `Patches/ATYPE/`): pick it after CS-Type
in the Synth page model list. Two VCOs with sync, ring modulator, sample & hold. Play it at
Poly 1: Keyboard Duo splits two held notes (from two tracks on the slot, or two MIDI keys),
VCO 1 the lowest, VCO 2 the highest; Priority Last / Low / High picks the note in Mono, and
releasing a key returns to a note still held. M1-M4 are the same on every patch (Cutoff,
Resonance, Env Amt, Decay + Release); leads (Sync Tear, Lead 2-Osc, Sync Scream) also scale
the envelope amount with the velocity (M7).

| Patch | Description | Poly | Macros M1-M6 |
|---|---|---|---|
| OT Duo Fifths | Duo: two notes held (two tracks or two keys) split, VCO 1 the low one, VCO 2 the high one; two saws, a bright 4-pole. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Vibrato, Detune (+-) |
| OT Sync Tear | VCO 2 synced to VCO 1, swept by the ADSR: the classic tearing lead; M6 moves the sync tune. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Vibrato, Sync Tune (+-) |
| OT Sample Hold Zap | A pulse through a resonant 4-pole stepped by the S&H on noise at 10 Hz; M5 the rate, M6 the lag. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), S&H Rate (+-), Lag |
| OT Odd Bass | A 32' saw and a 16' pulse a fifth up through a dark 4-pole with a short envelope; M6 the pulse width. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Vibrato, PW (+-) |
| OT Ring Bell | The ring modulator of two VCOs a tritone apart, an AR with a long release: a metallic bell; M6 the ring's tune. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Vibrato, Ring Tune (+-) |
| OT PWM Strings | Duo: two pulse waves swept by a slow LFO into the 2-pole, slow attack and release; play two tracks for two voices. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Vibrato, PWM (+-) |
| OT Random Arp | The S&H on noise steps VCO 2 at 8 Hz over a held note: random melodies; M6 the step range. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), S&H Rate (+-), Steps (+-) |
| OT Noise Drum | Noise through the resonant 2-pole with a fast decay; Priority Low: on a slot two tracks share, the lower held note wins; M6 the snap. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Vibrato, Snap (+-) |
| OT Lead 2-Osc | Saw and pulse slightly apart, a singing 4-pole, legato glide 60 ms; velocity to the envelope; M5 vibrato. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Vibrato, Detune (+-) |
| OT Noise Wind | Noise into the 2-pole near self-oscillation, the cutoff swept by a slow LFO, slow attack: wind; M6 the LFO rate. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Vibrato, LFO Rate (+-) |
| OT Sync Scream | Synced VCO 2 an octave up, its pitch wobbled by the LFO, a resonant 4-pole above a high-pass: a screaming lead. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Vibrato, Scream (+-) |
| OT Duo Organ | Duo: two square waves (16' and 8') on an AR, the 2-pole open: a two-voice organ from two tracks; M6 VCO 2's level. | 1 | Cutoff (+-), Resonance (+-), Env Amt (+-), Decay (+-), Vibrato, VCO2 Level (+-) |

## 8-Type (12)

8-Type is a module engine (3.x, module 8TY1, folder `Patches/8TYPE/`): a 3-voice 8-bit home computer
style voice. Three oscillators (Tri, Saw, Pulse with PWM, Noise, the combined waves where the pulse
gates the other wave); Ring and Sync take the oscillator before (oscillator 1's is oscillator 3);
Filt sends an oscillator through the one 12 dB multimode filter of the voice (any mix of low,
band and high-pass; Curve Bright / Dark; 16 resonance steps that never self-oscillate), Clean
keeps it out of Grit (the DAC character of the mix). Leads at Poly 1, chords at Poly 3. The CHIP
section plays an arp at a fixed tick or a tempo division without retriggering the note, a stepped
vibrato and a slide. M1 Cutoff, M2 Resonance, M3 PW (or Grit / Dirt / Key Track where no pulse
plays), M4 Decay + Release on every patch.

| Patch | Description | Poly | Macros M1-M6 |
|---|---|---|---|
| OT Chip Lead | A 25 % pulse with slow PWM through a bright low-pass, a delayed stepped vibrato; M5 the vibrato. | 1 | Cutoff (+-), Resonance (+-), PW (+-), Decay (+-), Vib Depth, Grit |
| OT PWM Bass | Two pulses an octave apart, both swept by the PWM LFO, a short filter envelope: the classic computer bass. | 1 | Cutoff (+-), Resonance (+-), PW (+-), Decay (+-), Env Amt (+-), Sub (+-) |
| OT Arp Major | One note plays a major chord: the arp at 60 Hz (0 4 7) on a 50 % pulse; M5 the rate. | 1 | Cutoff (+-), Resonance (+-), PW (+-), Decay (+-), Arp Rate (+-), Arp Mode (+-) |
| OT Arp Minor | A minor chord from one note (0 3 7) at 1/64 of the tempo on a thin pulse with a little Grit. | 1 | Cutoff (+-), Resonance (+-), PW (+-), Decay (+-), Arp Rate (+-), Grit |
| OT Ring Chime | Oscillator 1's triangle ringed by oscillator 3 an octave and a fifth up (silent): metallic sum and difference tones; M6 the tune. | 1 | Cutoff (+-), Resonance (+-), Grit, Decay (+-), Env Amt (+-), Ring Tune (+-) |
| OT Sync Buzz | Oscillator 2's saw synced to oscillator 1 (silent) a ninth up, the filter envelope sweeping it: a tearing lead. | 1 | Cutoff (+-), Resonance (+-), Dirt, Decay (+-), Env Amt (+-), Sync Tune (+-) |
| OT Filter Bass | A saw into the resonant low-pass, the filter envelope plucking it (Dark curve); M6 Dirt. | 1 | Cutoff (+-), Resonance (+-), Key Track (+-), Decay (+-), Env Amt (+-), Dirt |
| OT Tri Flute | A triangle with a slow attack and the stepped vibrato, the filter open: a soft chip flute. | 1 | Cutoff (+-), Resonance (+-), Grit, Decay (+-), Vib Depth, Breath |
| OT Saw Buzz | Saw+Pulse (the pulse gating the saw) and a detuned saw: a thin, buzzy lead; M6 the pulse width. | 1 | Cutoff (+-), Resonance (+-), PW (+-), Decay (+-), Env Amt (+-), Width (+-) |
| OT Octave Arp | The octave arp at 120 Hz on a pulse with a band-pass: the chirping chip lead; M5 the rate. | 1 | Cutoff (+-), Resonance (+-), PW (+-), Decay (+-), Arp Rate (+-), Grit |
| OT Chord Voice | Three oscillators at 0 / +4 / +7: a major chord from one voice (Poly 1), a slow filter swell. | 1 | Cutoff (+-), Resonance (+-), PW (+-), Decay (+-), Env Amt (+-), Third (+-) |
| OT Noise Drums | Noise with a fast slide down from +12 and a short decay: kicks low, snares and hats high (Poly 3). | 3 | Cutoff (+-), Resonance (+-), Grit, Decay (+-), Slide (+-), Tone (+-) |

## Hand-Type (12)

Hand-Type is a module engine (3.x, module HND1, folder `Patches/HANDTYPE/`): a 4-channel handheld
console style voice. One patch is one channel: Pulse (duty, a frequency sweep that can end the
note, a second pulse), Wave (32 steps of 4 bits) or Noise (a 15- or 7-bit register), at Poly 1;
or Drums, a kit by note at Poly 6 (60 kick, 61 blip, 62 snare, 63 / 71 clap, 64-67 hats closed
to open, a hat cuts the one before, 68 bell, 69 crash, 70 ride, 48-59 toms). The CHIP section
plays an arp (Arp Mode / Rate: a chord from one note at a fixed tick or a tempo division, the
note is not retriggered), a stepped vibrato and a slide at the note start. Channel patches:
M1 Env Step, M2 Duty / Wave / Noise Shift, M3 the channel's own (Pulse 2, the 4-level volume,
7-bit noise), M4 Bits (to a clean DAC).

| Patch | Description | Poly | Macros M1-M6 |
|---|---|---|---|
| OT Pulse Lead | A 25 % pulse held at full level (Env Up), Release Fade, a short vibrato after 0.3 s; M6 a slide into the note. | 1 | Env Step (+-), Duty (+-), Pulse 2, Bits, Vib Depth, Slide (+-) |
| OT Twin Pulse | Two pulses 12 cents apart (50 % and 25 %): the thick handheld lead; M6 the detune. | 1 | Env Step (+-), Duty (+-), Pulse 2, Bits, Vib Depth, Detune (+-) |
| OT Wave Bass | The Bass wave with a slow 4-bit decay: a round, stepped low end; M2 steps the wave, M6 cuts the release. | 1 | Env Step (+-), Wave (+-), Levels 4, Bits, Vib Depth, Release Cut |
| OT Wave Organ | The Organ wave held (Env Up), Release Fade: a reedy chip organ; M3 the 4-level volume, M6 cuts the release. | 1 | Env Step (+-), Wave (+-), Levels 4, Bits, Vib Depth, Release Cut |
| OT Drop Kick | A pulse swept down fast (Sweep Shift 2), cut at the bottom: the handheld kick on any note; M3 the drop. | 1 | Env Step (+-), Duty (+-), Sweep Shift (+-), Bits, Vib Depth, Slide (+-) |
| OT Noise Snare | 15-bit noise at a high clock, a fast 4-bit decay: the noise channel snare; M2 the noise pitch, M6 a length. | 1 | Env Step (+-), Noise Shift (+-), 7-bit, Bits, Divider (+-), Length |
| OT Noise Hat | 7-bit (metallic) noise at the highest clock, the shortest decay: a hat on every note; M6 the length. | 1 | Env Step (+-), Noise Shift (+-), 15-bit, Bits, Divider (+-), Length (+-) |
| OT Arp Chord Pulse | One note plays a major chord: the arp at 60 Hz (0 4 7) on a 12.5 % pulse; M5 the arp rate. | 1 | Env Step (+-), Duty (+-), Pulse 2, Bits, Arp Rate (+-), Arp Mode (+-) |
| OT Vib Lead | A 50 % pulse with the stepped vibrato (0.5 st at 6 Hz after 0.15 s) and a slide in from below; M5 the depth. | 1 | Env Step (+-), Duty (+-), Pulse 2, Bits, Vib Depth, Slide (+-) |
| OT Wave Bell | The Hollow wave with a quick start and a slow 4-bit fall, an octave arp: a chime; M6 the arp's chord. | 1 | Env Step (+-), Wave (+-), Levels 4, Bits, Arp Rate (+-), Arp Mode (+-) |
| OT Blip Arp | Short 12.5 % blips (Length 1) through a Min7 arp at 1/64 of the tempo; M5 the rate. | 1 | Env Step (+-), Duty (+-), Pulse 2, Bits, Arp Rate (+-), Length (+-) |
| OT Kit Handheld | Drums by note (Poly 6): 60 kick, 61 blip, 62 snare, 63 clap, 64-67 hats (a hat cuts the last), 68 bell, 69 crash, 70 ride, 48-59 toms. | 6 | Kick Decay (+-), Hat Decay (+-), Kick Sweep (+-), Hat Shift (+-), Snare Decay (+-), Kick Tune (+-) |

## B-Type (12)

B-Type is a module engine (3.x, module BTY1, folder `Patches/BTYPE/`): a 4-channel 8-bit home computer
style voice with polynomial counters. Clock 15.6 kHz / 64 kHz / 1.79 MHz; Tuning Free (in tune),
Coarse (the nearest 8-bit divider: out of tune up high, on purpose) or Fine (a 16-bit divider);
the Distortions (Pure, Poly4 .. Poly5x9) take the counters' bits at each divider event: the buzz.
Hi-Pass takes the output less itself 1 / HP Freq earlier (thin, whistling tones); channel 2 sits a
few divider steps away. Play at Poly 1. M1 Distortion, M2 Hi-Pass, M3 Decay, M4 Channel 2.

| Patch | Description | Poly | Macros M1-M6 |
|---|---|---|---|
| OT Pure Square | A pure square in tune (Free), a 4-bit sustain: the plain channel; M1 steps through the distortions. | 1 | Distortion (+-), Hi-Pass, Decay (+-), Channel 2, Vib Depth, Bits |
| OT Buzz Bass | Poly5 at the 64 kHz clock: the low buzzing bass of the counters (it repeats every 31 events). | 1 | Distortion (+-), Hi-Pass, Decay (+-), Channel 2, Vib Depth, Clock (+-) |
| OT Poly5 Lead | Poly5 with a second Poly5 channel a divider step away, the stepped vibrato: a nasal lead. | 1 | Distortion (+-), Hi-Pass, Decay (+-), Detune (+-), Vib Depth, Ch 2 Level (+-) |
| OT Grainy Lead | Poly9: a grainy, noisy tone that still has a pitch; a slow attack in frames. | 1 | Distortion (+-), Hi-Pass, Decay (+-), Channel 2, Vib Depth, Attack (+-) |
| OT Hiss Wash | Poly17 (the long counter): noise that the note colours, a long release; M2 brings in the Hi-Pass. | 1 | Distortion (+-), Hi-Pass, Decay (+-), Channel 2, Vib Depth, Release (+-) |
| OT HiPass Whistle | A pure square less itself at HP Freq near the note: thin, whistling pulses; M2 moves the comb. | 1 | Distortion (+-), HP Freq (+-), Decay (+-), Channel 2, Vib Depth, Glide |
| OT Joined Bass | Tuning Fine (the 16-bit divider at 1.79 MHz: in tune everywhere), a short pluck in frames. | 1 | Distortion (+-), Hi-Pass, Decay (+-), Channel 2, Vib Depth, Smooth Env |
| OT Detuned Pair | Two pure squares two divider steps apart (Coarse): the beating two-channel lead. | 1 | Distortion (+-), Hi-Pass, Decay (+-), Detune (+-), Vib Depth, Ch 2 Level (+-) |
| OT Engine Rumble | Poly5 at the 15.6 kHz clock low down: the rumbling motor of the counters; M6 the clock. | 1 | Distortion (+-), Hi-Pass, Decay (+-), Channel 2, Vib Depth, Clock (+-) |
| OT Arp Buzz | Poly4 through a Min7 arp at 60 Hz: a buzzing chord from one note; M5 the rate. | 1 | Distortion (+-), Hi-Pass, Decay (+-), Channel 2, Arp Rate (+-), Arp Mode (+-) |
| OT Coarse Lead | Tuning Coarse at 64 kHz: the 8-bit divider's detuned top octave is the sound; a slide in from below. | 1 | Distortion (+-), Hi-Pass, Decay (+-), Channel 2, Vib Depth, Clock (+-) |
| OT Thin Blip | A short pure blip, Sustain 0, decaying in 1/60 s frames, through Hi-Pass: a thin, clicky blip. | 1 | Distortion (+-), HP Freq (+-), Decay (+-), Channel 2, Vib Depth, Tune (+-) |

## Arc-Type (12)

Arc-Type is a module engine (3.x, module ARC1, folder `Patches/ARCTYPE/`): a home-console 4-operator FM
style voice. Eight algorithms, feedback, per operator MUL / DT / Level / RS and a 5-rate envelope
(AR, D1R to D1L, D2R, RR) whose Loop mode repeats, mirrors or inverts it at -48 dB (the buzzing
envelopes), an LFO (PMS vibrato, AMS per operator), a 9-bit DAC (Ladder: a gap around zero and
uneven steps), and the Aux square or noise voice. Chords at Poly 4, basses and leads at Poly 1.
M1 Brightness (the modulators' Level), M2 Feedback, M3 Release (on the plucks and hits: decays),
M4 Vibrato.

| Patch | Description | Poly | Macros M1-M6 |
|---|---|---|---|
| OT FM Bass Slap | 1 > 2 > 3 > 4 with a fast-decaying modulator and feedback: the slap of a console FM bass; velocity to brightness. | 1 | Brightness (+-), Feedback (+-), Release (+-), Vibrato, DAC (+-), Decay (+-) |
| OT Arc E-Piano | Two pairs (1 > 2, 3 > 4): a bright tine on one, a soft body on the other; a slow decay. | 4 | Brightness (+-), Feedback (+-), Release (+-), Vibrato, DAC (+-), Tine (+-) |
| OT Arc Brass | (1 + 2) > 3 > 4 with a slow attack on the modulators: the brass swell; M4 vibrato. | 4 | Brightness (+-), Feedback (+-), Release (+-), Vibrato, DAC (+-), Swell (+-) |
| OT Square Lead FM | (1 + 2 > 3) > 4: operator 1 with feedback 6 and operator 3 at MUL 2 into the carrier: a hollow, square-ish lead (Poly 1). | 1 | Brightness (+-), Feedback (+-), Release (+-), Vibrato, DAC (+-), Glide |
| OT Tine Bell | 1 > (2, 3, 4) at MUL 7 / 1 / 2 / 3: a ringing bell with a long decay. | 4 | Brightness (+-), Feedback (+-), Release (+-), Vibrato, DAC (+-), Ring (+-) |
| OT Arc Organ | Four carriers (1, 2, 3, 4) at MUL 0.5 / 1 / 2 / 4 held: drawbar-style registers; M6 the top one. | 4 | Brightness (+-), Feedback (+-), Release (+-), Vibrato, DAC (+-), Top (+-) |
| OT Arc Strings | 1 > 2, 3, 4 with slow attacks and the LFO's vibrato: a console string pad. | 4 | Brightness (+-), Feedback (+-), Release (+-), Vibrato, DAC (+-), Attack (+-) |
| OT Loop Buzz Pad | The modulator's envelope looping (Repeat) at -48 dB: the chip-tracker buzz under a pad; M6 the loop's speed. | 4 | Brightness (+-), Feedback (+-), Release (+-), Vibrato, DAC (+-), Loop Speed (+-) |
| OT Wood Pluck | A modulator at MUL 3 decaying fast into a short carrier: a dry, woody pluck. | 4 | Brightness (+-), Mod Decay (+-), Body (+-), Vibrato, DAC (+-), Knock (+-) |
| OT FM Kick | A sine sliding down two octaves in 60 ms (Slide Start +24) with a short modulator click (Poly 1). | 1 | Brightness (+-), Click (+-), Length (+-), Vibrato, DAC (+-), Slide (+-) |
| OT Twin Square | A soft FM tone with the Aux square an octave down under it: the console's two chips together (Poly 1). | 1 | Brightness (+-), Aux Decay, Release (+-), Vibrato, Aux Level (+-), Octave (+-) |
| OT Noise Burst | A short FM hit with the Aux noise decaying in frames: snares and zaps (Poly 2). | 2 | Brightness (+-), Feedback (+-), Length (+-), Vibrato, Aux Level (+-), Decay (+-) |

Total: 190 patches.
