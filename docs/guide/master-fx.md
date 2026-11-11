# Master effects

Put an echo, chorus or tape saturation on the **whole mix**: press **Shift + Master**, pick
**Echo** with the arrows, press **Turn on** (F1). It plays straight away; **Edit** (F2)
opens its settings.

## The FX bank

**Shift + Master** opens the FX bank: every master effect, in the order the mix runs
through them. Each row says **On**, **Off**, or **Card** (on the SD card, not loaded yet).
The panel on the right shows where the selected effect lives, how much memory it takes
and its place in the chain.

| Key | Does |
|---|---|
| **Up / Down**, encoder | Pick an effect. |
| **Turn on / Turn off** (F1) | Switch the selected effect. |
| **Edit** (F2) | Its settings page (F1–F8 pick a setting, the encoder changes it). |
| **Shift + Master** | From a settings page: back to the bank. From the bank: back to Master. |
| **Master** | The Master, Track Mixer and Global Mixer pages, as on stock. |

## The effects

The mix runs through them in this order, whichever are on:

**Dirt** (drive) → **LoFi** → **Chorus** → **BBD chorus** → **Echo** → **Tape**

The order is fixed, so every project sounds the same on every Tracker.

## Effects on the card

The effects are **card modules**: copy them from `Modules.zip` on the release page into
**`Modules/`** on the SD card (as [DX7](dx7.md)). An effect only takes memory while it is
on:

- **Turn on** loads it from the card. While the song plays it loads at the next **Stop**
  (the bank says **At Stop**).
- **Turn off** silences it at once; it leaves memory at the next Stop.
- A project remembers its effects and their settings: opening it loads the ones that are on.
- **Echo** borrows **48 KB of sample memory** while it is loaded (about half a second of
  mono sample). Samples come first: when they fill the memory, Echo doesn't load and the
  bank says **Not enough sample memory**.
- Card effects and card synths share the card memory (32 KB). When it is full, an effect
  doesn't load and the bank says **Card memory full**; turn off another effect or pick a
  built-in synth.

Without the `Modules/` folder there are no master effects; projects still open and play.
