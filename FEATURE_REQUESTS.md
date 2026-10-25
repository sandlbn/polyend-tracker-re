# Polyend Backstage feature request'leri (Tracker) ve bu fork

Kaynak: [backstage.polyend.com, Feature Requests, *tracker* etiketi](https://backstage.polyend.com/tags/c/feature-requests/16/none/tracker/3), 25 Ekim 2026 itibarıyla 254 başlığın hepsi (yanıt sayıları başlık listesinden). Değerlendirme, OpenTracker 2.2.1-semi çalıştıran **orijinal Tracker** için.

Yapılabilecek 227 maddenin tek tek notlu listesi: **[YAPILABILIR.md](YAPILABILIR.md)**.

## Özet

| Durum | Adet | Anlamı |
|---|---|---|
| **Fork'ta yapıldı** | 3 | Bu fork (2.2.1-semi) ekledi. |
| **OpenTracker'da zaten var** | 1 | Upstream OpenTracker 2.x'te mevcut. |
| **Kısmen var** | 12 | Bir kısmı var (fork ya da upstream); eksik kısmı notta. |
| **Kolay** | 144 | Arayüz, sequencer, dosya ya da offline Sample Editor işi. Ses yoluna dokunmaz, Mini dosya formatını bozmaz, genelde birkaç yüz bayttan 2 KB'a kadar flash. |
| **Orta** | 71 | Daha çok kod, ses yolunda küçük bir değişiklik (önce CPU ölçümü) ya da yalnızca bu fork'un okuyacağı yeni bir proje alanı (stok firmware / Mini bu ayarı görmez). |
| **Zor / riskli** | 14 | Mini 2.2.1 dosya formatını bozar, yeni ses motoru ister ya da OG'nin CPU / flash bütçesini aşar. |
| **OG Tracker'da uygulanamaz** | 9 | Tracker Mini / Plus donanımı, OG'de olmayan USB ses / USB host, ya da Tracker dışı ürün fikri. |

## Bütçe

Flash: projenin 720 KB sınırının 698 KB'ı dolu (yaklaşık 22 KB kaldı; çipte 1 MB var, sınırı upstream koymuş ve nedeni repoda yazmıyor). Statik RAM: 90 KiB stack tabanının yaklaşık 8.7 KB üstünde. SDRAM tamponu: yaklaşık 62 KB boş.

Sadece kolay maddeler bile 22 KB'ı aşıyor (tahmin 40-60 KB). Hepsini yapmak için ya fork'un kendi kodundan flash geri kazanmak (birkaç KB) ya da upstream yazarına danışıp 720 KB sınırını yükseltmek gerekiyor. Sınır içinde kalırken sıra talebe (yanıt sayısına) göre.

## Plan (paketler)

1. **En çok istenen kolaylar:** OFF'suz live kayıt, Beat Slice'ı sample sonuna kadar çalma, mono'ya çevirme, pattern'e geçmeden kopyalama, track başına uzunluk (poliritim), Fill ile OFF/CUT/FAD, +/- için ölçek kilidi, MIDI Program Change ile pattern ve enstrüman seçimi, stereo'yu iki mono olarak yükleme, FX listesinin son kullanılan efektten açılması, pattern ekleme / klonlama / tekilleştirme, Song loop bölgesi.
2. **Sample Editor (offline):** mono, kesme, rezonanslı HPF/LPF, pitch up/down, crossfade loop, loop'a crop, sessizlik kırpma, sessizlik ekleme, faz ters çevirme, gürültü, frekans kaydırma, kromatik tuner + notaya otomatik akort, BPM ve tempoya göre ton hesaplayıcı, içe aktarmada smpl loop noktaları.
3. **MIDI:** Program Change girişi, MIDI thru, otomatik clock / transport, Song Position Pointer, MIDI bilgi çubuğu, delay için CC'ler, pad'lerle hızlı MIDI kanal seçimi.
4. **Pattern / Song arayüzü:** döndürme / kaydırma, track birleştirme ve takas, step'e atlama, bemol gösterimi, renk kodlama, Song konumu / saati, track gizleme, arm ederken track adı, stem dosya adlarında track adları, export adında BPM.
5. **Orta maddeler** tek tek; ses yoluna dokunanlarda önce QEMU CPU ölçümü.


## Fork'ta yapıldı (3)

| Request | Yanıt | Not |
|---|---|---|
| Possibility to have more and more accessible color schemes | 20 | 16 tema |
| Ability to scan the radio frequency while sampling | 3 | Shift + seek tarama |
| Beat Slice – Auto Slice Transient options | 2 | Breaks dedektörü (Config > Auto slice) |

## OpenTracker'da zaten var (1)

| Request | Yanıt | Not |
|---|---|---|
| Diagnostic mode | 9 | selftest imajı + Factory test ekranı |

## Kısmen var (12)

| Request | Yanıt | Not |
|---|---|---|
| Generate new patterns in Performance mode | 17 | B1..B10 play modları + Generate; Performance'tan tetikleme eksik |
| Offline synths for sample creation | 13 | live resampling ile synth sample'a çevrilebiliyor; tek tuş render yok |
| System-wide LoFi Sampling Mode (aka "SP-Mode") | 11 | Transcode / DCT Crush offline lo-fi; kayıt modu yok |
| Auto-place notes for an instruments slices | 8 | BBCut generator dilimleri yazıyor; düz sıralı yerleştirme eksik |
| Delete samples/files on the Tracker | 5 | proje ve .mod silme var; sample dosyası silme yok |
| Horizontal display mode zoomed out to see 16 steps without page scroll | 5 | Horizontal arrangement var; 16 step zoom yok |
| Dual Mono Option for Passthrough Line In Audio | 5 | Line in channel ayarı var |
| Real-time sample rate reduction parameter and step effect ("bitcrush" sound) | 4 | Bit Depth FX var; örnekleme hızı düşürme yok |
| Vibrato FX Command | 2 | Finetune LFO FX'i vibrato yapıyor |
| Sample Recording - Ability to choose Left or Right channel | 2 | Line in channel ayarı |
| Play Track While Recording Sample | 1 | live resampling pattern çalarken kaydediyor |
| Pitch bending support on midi | 0 | Micro-tune/Pitchbend FX var; MIDI in pitchbend eksik |

## Kolay (144)

| Request | Yanıt | Not |
|---|---|---|
| Live recording - Ability to disable OFF commands | 49 | Config seçeneği |
| Beat Slice mode allowing to play through to end of sample | 37 | slice sonu = sample sonu seçeneği |
| Convert samples to mono | 31 | Sample Editor efekti |
| Copy Pattern without needing to switch to a new Pattern | 31 |  |
| Polyrhythm ( Different Track length in a pattern ) | 25 | format zaten track başına uzunluk taşıyor; edit modunda track uzunluğu UI'ı |
| OFF, CUT, FAD via Fill Function | 22 |  |
| Musical key and scale lock toggle for +/- buttons | 14 | nota girişi +/- ölçek içinde |
| Listen to Program Change, to change patterns via Midi | 14 | MIDI PC in → pattern |
| Pattern mode - Insert new pattern, in between existing patterns | 14 |  |
| Import stereo samples as 2 mono samples | 13 | yükleyici seçeneği |
| Change Instrument via MIDI in Synth Mode | 13 | MIDI PC in |
| Swap content of neighboor tracks of same type | 13 |  |
| Start position in the effects list of an FX button should default to the last effect used in that column | 13 |  |
| Chromatic tuner / Spectral analysis for Sample Tuning | 12 | offline perde tespiti (nota + cent) |
| Sample Pitchshift-to-Tempo Calculator/Function | 12 | BPM'e göre ton hesabı |
| Looping multiple patterns in song-mode | 11 | Song loop bölgesi |
| Song Mode : Colour code song blocks using Note, Instrument, Fx1, Fx2 keys | 11 |  |
| Solo [Arm] a track | 11 |  |
| Merge step data of two (or more) tracks | 10 |  |
| Disable screen backlight while recording samples to remove device noise | 10 |  |
| Project Defaults | 10 | şablon proje |
| Option to disable automatic audio previews while editing patterns | 10 |  |
| Pattern / selection window rotation | 9 | step kaydırma |
| Sample Loader - Wavetable Preview Mode | 9 |  |
| Let song mode clock reflect the time into the song, even when starting the song from somewhere inside | 9 |  |
| Cut function in Sample Editor | 8 | seçimi sil (Crop'un tersi) |
| Replace notes while recording (overdubbing) | 8 | kayıt seçeneği |
| HPF and LPF with resonance in the sample editor | 8 | offline biquad efekti |
| MIDI thru should forward the channel data it receives | 8 |  |
| Mute Synths From Global Mixer Page | 8 | synth sütunlarına mute/solo |
| Improved Pattern Navigation for Pattern-View | 8 |  |
| Proper Incremental naming of song stem folders | 8 |  |
| Export looping songs | 8 |  |
| Audio "Status Bar" at the top of the UI | 8 |  |
| Mute/Solo for all 16 tracks (not just the new synths) in Master Mixer | 7 |  |
| Erase or initialise a pattern | 7 |  |
| Shortcut to jump quickly between Tracks | 7 |  |
| Noise color Effect for Sample Editor | 7 | offline |
| Add Slice to be respective of distance between prior Slices | 7 |  |
| Trim Silence in Sample Editor | 7 |  |
| Move/drag notes and selections up and down steps easily | 6 |  |
| Flag for long samples in Sample Loader - if too long to edit | 6 |  |
| Switch a synth/midi instrument slot or replace an instrument with another | 6 |  |
| Trigger Polyend Tracker patterns via MIDI from an External Source | 6 |  |
| Add BPM to export filenames | 6 |  |
| Redo option in song mode | 5 |  |
| Have a loop option for cropping in the Sample Editor | 5 | loop noktalarına crop |
| Simple way to preview all PERC elements while editing | 5 |  |
| Option: Automatically assign sample's pitch to its note to keep all samples in tune | 5 | tuner ile birlikte |
| Option to change note display in the Tracker from sharps (#) to flats (b) | 5 |  |
| Save / Load MIDI Instruments | 5 |  |
| We need the notes with beat slices highlighted | 5 |  |
| Quick-Crop from Sample Playback Loop points | 5 |  |
| Delete notes while in live record mode | 5 |  |
| Randomize slide up/down | 5 |  |
| Pitch Up/Down Effect for Sample Editor | 5 | offline |
| Better value scaling for more precise control over small values (eg. Envelope attack time) | 5 |  |
| Latch pads for live play | 5 |  |
| Display which Instrument is featured on which Patterns | 5 |  |
| Fast select midi channel using pads | 5 |  |
| Rename sample file in SD Card to mark favourites | 5 |  |
| Small improvement: show track name when arming | 5 |  |
| Display which Patterns differ from the Empty / Project Default State or contain Filtered Data | 5 |  |
| New Option - Change Keyboard Layout (QWERTZ) | 5 |  |
| Crossfade Loops / Loop-points | 4 | offline crossfade |
| Auto-Preview Option for Previewing Samples | 4 |  |
| Stem Export Filenames should use custom Track Names if available | 4 | şu an track1.wav ... |
| Pseudo real-time clock for easier file management | 4 | ayarlanan tarih + çalışma süresi |
| Use Fill mode only on steps with specific Instrument | 4 |  |
| Note quantization (pitch) on random note values | 4 | Random Note ölçeğe |
| MIDI Record Mode that maps keys to Instruments instead of Musical Notes | 4 |  |
| Master EQ bypass and the ability to save and load different master EQ settings | 4 |  |
| Keyboard UX - Use jogwheel to navigate between characters | 4 |  |
| Convert Note selection with Arp/Chord FX to actual notes in a pattern | 4 |  |
| Live record erase button | 4 |  |
| Beat Slice – Padgrid chopping | 4 |  |
| Sequencing Feature - Retain FX Value if FX Button is Pressed and Released and FX Type is Not Changed | 4 |  |
| Option to change Beat Slice Root Note | 3 |  |
| Let the user select which FX to use with Random FX Fill | 3 |  |
| Complex 'Where?' Fill conditions | 3 |  |
| Integrate Notepad Feature into Polyend Tracker for In-Device Project Notes | 3 | proje notu (txt) |
| Add MIDI CCs to control delay settings | 3 |  |
| Soloing one track should mute all tracks (including MIDI) | 3 |  |
| Fill Type - Interleaving values / value alternating | 3 |  |
| Enable dedicated pad mode for Perc engine | 3 |  |
| Track Mixer : Colour tracks in Edit mode by Note, Instrument, Fx1, Fx2 keys | 3 |  |
| More States for the Metronome | 3 |  |
| File button section - Use Insert key to enter folder | 3 |  |
| Set initial / default tempo in Config | 3 |  |
| More destructive processing options Eg. cut and paste, frequency shift, etc | 3 | offline |
| Screensaver or other idle screen options | 3 | Data ekranı ekran koruyucu olabilir |
| Duplicate patterns in song mode with unique IDs | 3 |  |
| Visibility/Invisibility of tracks | 3 |  |
| Sample Editing Seek via Pads | 3 |  |
| BPM calculator in sample editor | 3 |  |
| Naming / Renaming UX Improvements | 2 |  |
| Previewing single cycle waveforms should default to forward loop | 2 |  |
| Polyphony: Cycle through Armed Tracks Option | 2 |  |
| Jump to step in pattern edit mode | 2 |  |
| Make pattern unique | 2 |  |
| Light up root notes on Tracker | 2 |  |
| Ability to make all tracks switch patterns with a single shortcut in performance mode | 2 |  |
| Second pattern divider with a different colour | 2 |  |
| Meaningful auto-naming scheme for rendered selection | 2 |  |
| Shifting all events equally, aka sliding all events in a pattern around like on a Slide Projector | 2 |  |
| Midi inforbar for incoming midi information | 2 |  |
| Temporary Radio tuning presets | 2 |  |
| Sample Playback - Granular Mode - Set Position to Exact Middle | 2 |  |
| Offsets to Fills | 2 |  |
| Add option to transpose selection in selected scale with jog wheel | 2 |  |
| Bipolar step value editing when dealing with a range-selection | 2 |  |
| Show the number track on Arm/Disarm | 2 |  |
| Preroll with visual timer | 2 |  |
| Sample Editor – Add silence / dead-space | 2 |  |
| Fill Notes Window - Using incoming midi for data-input | 2 |  |
| Text Editing Shortcut - Select all in text edit with double-click Center / Enter button | 2 |  |
| Naming patterns and tracks in a flat file | 2 |  |
| Manually enter radio frequency instead of scanning | 2 |  |
| Song Mode UI - Playback Position | 1 |  |
| Auto option for MIDI sync/transport | 1 |  |
| Sample Loader - Colour code Instruments using Note, Instrument, Fx1, Fx2 keys | 1 |  |
| Basic utility effects | 1 | faz ters, kanal değiştir, DC |
| Show beatslices on Padgrid in non-recording mode too | 1 |  |
| Song Mode UI: mark tracks with only OFF, CUT or FAD events | 1 |  |
| See filled patterns on grid in song mode | 1 |  |
| Default to start (or choice of slot) when loading a project in song mode | 1 |  |
| Clone the active pattern via a single key | 1 |  |
| Program change in instrument mode | 1 |  |
| More gridpads being led to support the Letter keyboard | 1 |  |
| Add 'Zoom' functionality to Granular Playback settings page | 1 |  |
| Text Editing Shortcut - Select Text Letter by Letter in Text Edit with Shift + Left/Right | 1 |  |
| Sample Playback UI: Move "Zoom" to left side of screen for Slice & Beat Slice modes | 1 |  |
| Copying the name of multiple recorded samples | 0 |  |
| Default project track length | 0 |  |
| See slices in Sample Editor | 0 |  |
| Metronome in Sample recorder / Sample editor | 0 |  |
| import loop point already embedded in a .wav files | 0 | smpl chunk |
| move instrument in instrument list | 0 |  |
| Sample preview note setting | 0 |  |
| Manual Slicing As Sample Is Playing (chop) | 0 |  |
| Export each pattern in song | 0 |  |
| Crossfade loop option | 0 |  |
| Allow the grid pad to enter steps, like the Play | 0 |  |
| Invert phase FX? | 0 | offline |

## Orta (71)

| Request | Yanıt | Not |
|---|---|---|
| Additional MIDI CC In Controls / External controller as a full-featured control surface | 29 | CC haritası genişletme |
| Support for LFO Offsets | 23 |  |
| Glide without Sample retrigger | 19 | legato glide, ses motoru |
| Granular Length Step FX | 16 | yeni fork FX'i, granular motor var |
| A:B conditions for each sequencer pass | 15 | yeni bir fork FX'i (stok FW yok sayar) |
| Record perfect loops with external synth | 15 | saate bağlı N bar kayıt + gecikme telafisi |
| Custom MIDI chords | 14 | akor tablosu fork alanında |
| Configurable LFO retrigger in Instrument Parameters | 14 | enstrüman parametresi, fork alanı |
| Fluid MIDI pitchbend | 13 | pürüzsüz pitchbend çıkışı |
| Sample Editor Window Sample Length | 12 | editörde daha uzun sample (bellek) |
| Polyend Synth Engines: ACD - Distortion effect | 11 | ACD sesine distorsiyon; CPU ölçümü |
| Jump to pattern FX - creating Song Mode/ Pattern Mode loop regions | 11 | Jump FX fork'a özel; Song loop bölgesi kolay |
| Better Project Management/Search | 11 |  |
| Live-Perform Multi-Instrument Mode | 11 |  |
| Highpass Filter for Reverb | 9 | ses yolunda tek kutuplu HPF; CPU ölçümü |
| FX Steps: Additional micro move command (backward) | 9 | sequencer'da ileri bakma |
| Step FX for: Automation LFO Amount | 9 | yeni fork FX'i |
| Import a sample 'chain' feature | 9 | birden çok sample'ı tek dilimli enstrümana |
| Adjustable slew on LFO shapes | 9 |  |
| Change Envelope Automation Values via StepFX | 8 | yeni fork FX'leri |
| Swapping instruments between projects | 8 | başka projeden enstrüman içe aktarma |
| Route MIDI note input to each track, on an assignable MIDI channel, for triggering instruments and recording | 8 |  |
| Improve immediate pattern switching behaviour in Performance mode | 8 |  |
| Track Delays (Time Offsets) | 8 | fork alanı |
| To be able to address 32 Midi channels | 8 | DIN + USB ayrı port |
| Negative amounts for Modulation sources | 7 | format |
| Optional possibility to set master settings per pattern instead of per project | 7 | fork alanı |
| Secondary / Enhanced Chance Step FX | 6 | yeni fork FX'i |
| Expand Envelope / LFO shapes (akin to the wavetables in XAOC Zadar) | 6 | format |
| Render stems per instrument and not per track to help the mixing / mastering process | 6 |  |
| Placeholder sound for midi instrument | 6 |  |
| FX Parameter Glide | 5 |  |
| Undo sample load | 5 | bellek |
| Pattern Lock / Protection | 5 | fork alanı |
| Sampling directly to slices | 5 |  |
| Filter Keyboard/Note Tracking | 4 | ses parametresi |
| Add support for sending/receiving MIDI Song Position Pointer (SPP) and/or MIDI Timecodes (MTC) | 4 |  |
| LFO speeds to be independent of Tempo | 4 |  |
| Sample Editor - Chorus improvements | 4 |  |
| Master page for Midi | 4 |  |
| Introduce tagging of synth patches, to improve patch management and browsing | 3 |  |
| CUT/COPY/PASTE (Browser, Sample Editor... Everywhere) | 3 |  |
| Expanded midi control of performance mode on the Tracker Mini | 3 | OG için de geçerli |
| More than one Song in Song Mode | 3 | fork alanı |
| New Performance Mode Effect - Gate Length | 3 |  |
| Instrument Groups | 3 |  |
| Optionally set Envelope AD and R times with Step-based lengths | 3 |  |
| Chord FX enhancements | 3 |  |
| Apply scales to rolls | 2 | perdeli roll'lar ölçeğe |
| Extend MIDI Chord/ARP combo possibilities | 2 |  |
| Tracker OG - Song mode improvements | 2 | geniş kapsamlı |
| Tracker Perform Jog Wheel Macros | 2 | Performance'ta encoder makrosu |
| Step-effect: Strum | 2 | yeni fork FX'i |
| Beat Slice - "Loop options" | 2 |  |
| New Performance Mode Effect - Chance | 2 |  |
| Adjustable Delay stage for the Envelopes | 2 | format |
| Add a Master Effects page to Performance Mode | 2 |  |
| Engine - Midi Perc | 2 |  |
| Make menu mappable to function buttons | 2 |  |
| Displaying long samples as a waveform | 2 |  |
| Song Export improvements | 2 | geniş kapsamlı |
| Volume LFO Speed - Add up to 128steps | 2 |  |
| MIDI Parameters in performance mode | 1 |  |
| Wavetable Sampling and Creation Automation | 1 | offline wavetable çıkarma |
| Ability to store/link and recall single track sequences across patterns | 1 | SD'de track kütüphanesi |
| Link multiple instruments / parts | 1 |  |
| Reload Instrument Parameters/ Reload Sample Pool/ Reload Pattern | 1 |  |
| Sample Volume sent to effects not effected by Volume step FX | 1 | send yönlendirmesi |
| Using Midi Channel to control Either Performace mode OR putting Additional FX | 0 |  |
| Infinity value for sample volume release | 0 |  |
| Give the ability for the tracker to follow the user by 'listening' for midi note events | 0 |  |

## Zor / riskli (14)

| Request | Yanıt | Not |
|---|---|---|
| More than 2 step fx (Tracker Mini and Plus) | 46 | Mini 2.2.1 dosya formatı 2 FX taşıyor; daha fazlası formatı bozar |
| Free up FX slots by moving Velocity and Gate into the Note parameter | 18 | dosya formatını bozar |
| Single Track Patterns - updated | 15 | format |
| User-Assignable Instrument Automation Destinations | 13 | modülasyon matrisi, format |
| A sample based synth engine for Play+/Tracker+/Mini | 12 | yeni ses motoru, CPU + flash |
| More Filter Types | 7 | ses yolu, CPU |
| Expand Instrument Automations and Step FX | 5 | geniş kapsamlı, format |
| Individual patterns for each track | 4 | format |
| Expand Filter capabilities on the Delay effect | 3 | ses yolu |
| Draw Envolopes (Filter, Resonance, Pan, Amp, Etc) | 1 | otomasyon depolama + UI |
| Exchangable Master Effects | 1 | ses yolu |
| add new synth engine: SID engine | 0 | yeni motor |
| Increase maximum pattern length | 0 | 128 step format sınırı |
| Bell Filter in instrument parameters | 0 | ses yolu |

## OG Tracker'da uygulanamaz (9)

| Request | Yanıt | Not |
|---|---|---|
| »Finalize« - a new Product for Polyend - NOT just for Tracker | 12 | ürün fikri |
| Alternate Tracker-Mini Button Behaviour for better accessibility | 7 | Tracker Mini donanımı |
| Tracker Mini: Press and hold center button with + and - to change note in step jump | 7 | Tracker Mini donanımı |
| Vocal Effects Engine for Tracker+ & Mini | 4 | Tracker+/Mini; OG'de CPU yok |
| Tracker+ OG/mini - using a usb typing keyboard for naming files or other functions | 4 | OG'nin USB'si yalnızca cihaz modu |
| Tracker Mini - USB audio outputs numbering and order | 3 | OG'de USB ses yok |
| Legacy OS archive | 2 | Polyend sitesi |
| 'pedantic' ? streamlining of firmware | 0 | belirsiz |
| Jog-wheel not just for the tracker? | 0 | başka ürünler |
