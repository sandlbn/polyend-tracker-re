# OpenTracker preset pack

Our own synth patches for the ACD, FAT, VAP, WTFM and PERC engines of OpenTracker 2.x, written by
`reimpl/tools/make_patches` (`make -C reimpl patches`) through the firmware's own Save As path.
Every patch starts from the engine's built-in Init patch; no factory patch is used.

Files: `Patches/<MODEL>/<name>.patch`. Copy the `Patches` folder to the root of the SD card,
then use Load Patch on a synth page.

Macros: M1-M6 (FX CC A-F). A bipolar macro (+-) at 63 and a unipolar macro at 0 play the
patch as saved. M7 is Velocity (Volume FX), as in the Init patch. Levels are voiced for the
default slot gain (-9 dB); `tests/patches_test.cpp` checks that every patch is audible, stays
at least 1 dB below full scale (VAP, FAT and WTFM Poly 4: 4-voice chords; PERC: five drums at once, toms
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

## PERC (8)

Notes (MIDI numbers): kick 60, snare 62, hi-hats 64-67 (closed to open), cymbal 69, perc 71; any other
note plays the tom at that pitch.

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

Total: 74 patches.
