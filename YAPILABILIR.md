# Yapılabilecek request'ler ve notlarım

Polyend Backstage'deki 254 Tracker request'inden orijinal Tracker'da (OpenTracker 2.2.1-semi) yapılabilecek **227** tanesi, alana göre gruplu. Her alanda en çok istenen üstte. Tam liste ve yapılamayanlar: [FEATURE_REQUESTS.md](FEATURE_REQUESTS.md).

**Zorluk:** *Kolay* = arayüz / sequencer / dosya / offline işlem, ses yoluna dokunmaz, Mini dosya formatını bozmaz. *Orta* = daha çok kod, ses yolunda küçük değişiklik (önce QEMU CPU ölçümü) ya da sadece bu fork'un okuduğu yeni bir proje alanı. *Kısmen var* = bir kısmı fork'ta ya da upstream'de var, notta eksik kısmı yazıyor.

**Flash tahmini** kabaca: < 0.5 KB, 0.5–2 KB. 720 KB'lık yumuşak sınırın altında ~22 KB, stok imaj alanının (877,664 B, `tools/check_image.py`) altında ~175 KB yer var.

**Fork FX'i / fork uzantısı:** Yeni step FX'leri ve yeni proje ayarları sadece bu fork'ta çalışır. Stok firmware ve Tracker Mini aynı projeyi açar ama bu FX'leri yok sayar / bu ayarları görmez.

## Pattern ve sequencer (31)

| # | Request | Yanıt | Zorluk | Flash | Notum |
|---|---|---|---|---|---|
| 1 | Copy Pattern without needing to switch to a new Pattern | 31 | Kolay | < 0.5 KB | Pattern sayfasında 'Copy to…' seçeneği: hedef pattern numarasını encoder'la seç, pattern'i oraya yaz. Pattern store'da kopyalama fonksiyonu zaten var. |
| 2 | Polyrhythm ( Different Track length in a pattern ) | 25 | Kolay | < 0.5 KB | Dosya formatı her track için ayrı uzunluk (lastStep) zaten tutuyor; Mini de okuyor. Pattern ekranındaki Length alanına Shift+Length = sadece cursor track'i seçeneği eklenir. Sequencer track uzunluklarını zaten ayrı sayıyor; sadece UI işi. |
| 3 | A:B conditions for each sequencer pass | 15 | Orta | 0.5–2 KB | Yeni bir fork FX'i: 'Cond A:B' (örn. 1:4 = dört geçişin ilkinde çal). Sequencer track başına geçiş sayacı tutar. Stok firmware / Mini bu FX'i yok sayar, pattern yine çalar. 15 yanıt. |
| 4 | Musical key and scale lock toggle for +/- buttons | 14 | Kolay | < 0.5 KB | Nota alanında encoder / +/- adımı proje ölçeğindeki bir sonraki dereceye atlar. Config'e 'Scale lock' aç/kapa. Generator'larda kullandığım scaleDegrees() hazır. |
| 5 | Pattern mode - Insert new pattern, in between existing patterns | 14 | Kolay | < 0.5 KB | Song'da cursor'daki yere boş ya da klon pattern ekler, sonrakileri kaydırır. Song zaten pattern dizisi; ekleme = dizi kaydırma. |
| 6 | Swap content of neighboor tracks of same type | 13 | Kolay | < 0.5 KB | Cursor track'i ile sağındakini yer değiştirir (aynı türde: sample/MIDI). Track adları ve mute'lar da taşınır. |
| 7 | Start position in the effects list of an FX button should default to the last effect used in that column | 13 | Kolay | < 0.5 KB | FX popup'ı her sütun için son kullanılan efektten açılır; iki byte RAM. 13 yanıtlı, çok ucuz. |
| 8 | Jump to pattern FX - creating Song Mode/ Pattern Mode loop regions | 11 | Orta | 0.5–2 KB | Loop bölgesi kısmı kolay (Song maddesine bak). 'Jump to pattern N' FX'i fork'a özel bir FX olur; stok firmware'de pattern normal devam eder. |
| 9 | Merge step data of two (or more) tracks | 10 | Kolay | < 0.5 KB | İki track'i tek track'e birleştirir: boş step'lere diğer track'in step'i gelir, çakışmada seçilen track kazanır. Copy/paste altyapısı üzerine. |
| 10 | Pattern / selection window rotation | 9 | Kolay | < 0.5 KB | Seçimi ya da track'i N step ileri/geri döndürür (sona taşan başa gelir). Generate popup'ına 'Rotate' olarak da eklenebilir; tek undo adımı. |
| 11 | FX Steps: Additional micro move command (backward) | 9 | Orta | 0.5–2 KB | Geriye micro-move için sequencer'ın bir step ileri bakması gerekir: notayı önceki step'in sonundan tetikler. Sequencer zamanlamasına dokunduğu için dikkatli test ister. |
| 12 | Improved Pattern Navigation for Pattern-View | 8 | Kolay | < 0.5 KB | Shift + Up/Down sayfa (16 step) atlar, Shift + Left/Right track grubu (4 track) atlar. Mevcut navigasyona ek kısayollar. |
| 13 | Track Delays (Time Offsets) | 8 | Orta | 0.5–2 KB | Track başına ms cinsinden gecikme (MIDI track'lerde gecikme telafisi için de iyi). Ayar fork'un proje uzantısında saklanır; stok firmware görmez. |
| 14 | Shortcut to jump quickly between Tracks | 7 | Kolay | < 0.5 KB | Shift + pad 1..16 ile doğrudan track'e atlama (Pattern ekranında pad'ler o an nota girişinde değilse). |
| 15 | Erase or initialise a pattern | 7 | Kolay | < 0.5 KB | Pattern sayfasında 'Clear pattern' (onaylı); Shift ile sadece cursor track. Undo destekli. |
| 16 | Move/drag notes and selections up and down steps easily | 6 | Kolay | < 0.5 KB | Rotation ile aynı altyapı ama taşmasız kaydırma: Shift + Up/Down seçimi bir step taşır. Undo'ya tek adım. |
| 17 | Secondary / Enhanced Chance Step FX | 6 | Orta | 0.5–2 KB | Chance FX'in yanına 'Chance+': önceki step çaldıysa/çalmadıysa koşulu. A:B ile aynı sayaç altyapısı. |
| 18 | Randomize slide up/down | 5 | Kolay | < 0.5 KB | Fill'in Random FX değerlerine Slide Up/Down eklenir; aslında 'Random FX Fill'de FX seçimi' maddesiyle birlikte çözülür. |
| 19 | Display which Patterns differ from the Empty / Project Default State or contain Filtered Data | 5 | Kolay | < 0.5 KB | Song / pattern seçicide dolu pattern'ler işaretlenir; pattern store'da 'boş mu' kontrolü. |
| 20 | Pattern Lock / Protection | 5 | Orta | 0.5–2 KB | Pattern başına kilit bayrağı (fork uzantısında); kilitli pattern'e yazma, Fill ve Generate reddedilir. |
| 21 | Convert Note selection with Arp/Chord FX to actual notes in a pattern | 4 | Kolay | 0.5–2 KB | Arp/Chord FX'li step'leri sequencer'ın aynı tablolarıyla açıp yanındaki track'lere ya da sonraki step'lere gerçek nota yazar. Sequencer'daki arp/chord tabloları hazır. |
| 22 | Note quantization (pitch) on random note values | 4 | Kolay | < 0.5 KB | Random Note FX'in ürettiği notayı proje ölçeğine yuvarlar (Config seçeneği). Sequencer'da tek fonksiyon çağrısı; stok firmware'de aynı pattern ölçeksiz çalar. |
| 23 | Sequencing Feature - Retain FX Value if FX Button is Pressed and Released and FX Type is Not Changed | 4 | Kolay | < 0.5 KB | FX tuşuna basıp bırakınca tür değişmediyse değeri sıfırlamamak; tek koşul. |
| 24 | Visibility/Invisibility of tracks | 3 | Kolay | < 0.5 KB | Pattern görünümünde track gizleme (sadece görünüm, çalmaya etkisi yok). 8-track görünümde boş track'leri atlamak da buna dahil. |
| 25 | Shifting all events equally, aka sliding all events in a pattern around like on a Slide Projector | 2 | Kolay | < 0.5 KB | Rotation'un pattern'in tüm track'leri için hali; aynı kod. |
| 26 | Jump to step in pattern edit mode | 2 | Kolay | < 0.5 KB | Shift + encoder push ile step numarası seçici açılır; ya da pad N = step N*… Pattern uzunluğu 128'e kadar olduğu için pad satırı x sütun eşlemesi. |
| 27 | Bipolar step value editing when dealing with a range-selection | 2 | Kolay | < 0.5 KB | Seçimde encoder çevirince değerler kendi değerlerinden göreli değişir (şu an hepsi aynı değere mi çekiliyor kontrol edilecek); Shift ile ortadan dışa (bipolar) ölçekleme. |
| 28 | Apply scales to rolls | 2 | Orta | 0.5–2 KB | Roll tipleri içinde notayı yukarı/aşağı yürüten bir tip ekleyip adımları ölçeğe oturtmak. Roll değeri formatta tip*9+hız; yeni tip stok firmware'de düz roll çalar. |
| 29 | Step-effect: Strum | 2 | Orta | 0.5–2 KB | Yeni fork FX'i: MIDI Chord / akor notalarını step içinde ms aralıklarla sırayla tetikler. Micro-move altyapısını kullanır. |
| 30 | Add option to transpose selection in selected scale with jog wheel | 2 | Kolay | < 0.5 KB | Seçimde Shift + encoder notaları ölçek derecesi kadar kaydırır (scale lock ile aynı fonksiyon). |
| 31 | Clone the active pattern via a single key | 1 | Kolay | < 0.5 KB | Shift + Copy: aktif pattern'i ilk boş pattern'e kopyalar ve oraya geçer. |

## Fill (7)

| # | Request | Yanıt | Zorluk | Flash | Notum |
|---|---|---|---|---|---|
| 1 | OFF, CUT, FAD via Fill Function | 22 | Kolay | < 0.5 KB | Fill Notes listesine OFF, CUT, FAD eklenir; 'Each N step' ile birlikte her N step'e OFF gibi. 22 yanıt, birkaç satır. |
| 2 | Use Fill mode only on steps with specific Instrument | 4 | Kolay | < 0.5 KB | 'Where?' koşullarının bir parçası; birlikte yapılır. |
| 3 | Let the user select which FX to use with Random FX Fill | 3 | Kolay | < 0.5 KB | Random FX Fill'de FX listesinden çoklu seçim; seçili olanlar arasından rastgele. |
| 4 | Complex 'Where?' Fill conditions | 3 | Kolay | < 0.5 KB | Fill'in 'nereye' alanına: sadece boş step'ler, sadece notalı step'ler, sadece belirli enstrüman, her N'de bir + offset. |
| 5 | Fill Type - Interleaving values / value alternating | 3 | Kolay | < 0.5 KB | Fill tipi 'Alternate': iki değer (A/B) arasında sırayla. Constant/Random/Euclidean yanına. |
| 6 | Offsets to Fills | 2 | Kolay | < 0.5 KB | Fill 'Each N step'e başlangıç offset'i (Euclidean'daki rotation gibi). |
| 7 | Fill Notes Window - Using incoming midi for data-input | 2 | Kolay | < 0.5 KB | Fill popup'ı açıkken gelen MIDI nota, Fill'in nota değerini set eder. MIDI in altyapısı var. |

## Song (12)

| # | Request | Yanıt | Zorluk | Flash | Notum |
|---|---|---|---|---|---|
| 1 | Looping multiple patterns in song-mode | 11 | Kolay | < 0.5 KB | Song'da iki işaret arasını döngüye alma (loop start/end). Sequencer'ın song pozisyonu ilerlerken sona gelince başa döner; ayar oturumluk (dosyaya yazmaya gerek yok). |
| 2 | Song Mode : Colour code song blocks using Note, Instrument, Fx1, Fx2 keys | 11 | Kolay | < 0.5 KB | Song bloklarını tuşa göre renklendirme: Note = pattern'deki baskın nota aralığı, Instr = baskın enstrüman, FX = FX yoğunluğu. Sadece çizim. |
| 3 | Let song mode clock reflect the time into the song, even when starting the song from somewhere inside | 9 | Kolay | < 0.5 KB | Song ortasından başlayınca saat, önceki pattern'lerin süresini (tempo FX'leri dahil) toplayarak başlar. |
| 4 | Redo option in song mode | 5 | Kolay | < 0.5 KB | Song ekranı undo'yu zaten kullanıyorsa redo yığını eklenir; pattern ekranındaki undo/redo altyapısı kullanılır. |
| 5 | Duplicate patterns in song mode with unique IDs | 3 | Kolay | < 0.5 KB | 'Make pattern unique' ile aynı işlev, kopyala + yeni numara. |
| 6 | More than one Song in Song Mode | 3 | Orta | 0.5–2 KB | Birden çok song dizisi; Mini formatında tek song var, ek song'lar fork uzantı dosyasında saklanır. |
| 7 | Make pattern unique | 2 | Kolay | < 0.5 KB | Song'daki bir slot'un pattern'ini ilk boş numaraya kopyalayıp slot'u ona bağlar (başka slot'lar etkilenmez). |
| 8 | Tracker OG - Song mode improvements | 2 | Orta | 0.5–2 KB | Çok geniş bir başlık; yukarıdaki Song maddeleri (loop, unique, renk, konum) bunun büyük kısmını karşılar. |
| 9 | Song Mode UI - Playback Position | 1 | Kolay | < 0.5 KB | Song ekranında çalan pattern'in içindeki konum ilerleme çubuğu olarak. |
| 10 | See filled patterns on grid in song mode | 1 | Kolay | < 0.5 KB | Song grid'inde dolu/boş pattern ayrımı; 'which patterns differ' ile aynı kontrol. |
| 11 | Song Mode UI: mark tracks with only OFF, CUT or FAD events | 1 | Kolay | < 0.5 KB | Song'da track hücrelerinde sadece OFF/CUT/FAD içerenleri farklı işaretleme. |
| 12 | Default to start (or choice of slot) when loading a project in song mode | 1 | Kolay | < 0.5 KB | Proje açılınca song cursor'u başa (Config seçeneği: başa / son konuma). |

## Live kayıt ve pad'ler (16)

| # | Request | Yanıt | Zorluk | Flash | Notum |
|---|---|---|---|---|---|
| 1 | Live recording - Ability to disable OFF commands | 49 | Kolay | < 0.5 KB | Config > Recording options'a 'OFF yazma' seçeneği. Live kayıtta pad bırakınca OFF yazılmaz. Listenin en çok istenen maddesi (49 yanıt), çok ucuz. |
| 2 | Solo [Arm] a track | 11 | Kolay | < 0.5 KB | Shift+Arm ile sadece o track'i arm'lamak (diğerlerini disarm). |
| 3 | Live-Perform Multi-Instrument Mode | 11 | Orta | 0.5–2 KB | Pad grid'ini bölgelere ayırıp her bölgeyi ayrı track/enstrümana bağlama; pad düzeni ve arm mantığında değişiklik. |
| 4 | Replace notes while recording (overdubbing) | 8 | Kolay | < 0.5 KB | Kayıt seçeneği: overdub (üstüne ekle) / replace (döngüde geçilen step'leri sil).  |
| 5 | Delete notes while in live record mode | 5 | Kolay | < 0.5 KB | Live kayıtta Shift+Delete basılıyken geçilen step'leri siler (MPC 'erase' gibi). |
| 6 | Latch pads for live play | 5 | Kolay | < 0.5 KB | Pad modu: basınca nota çalmaya devam eder, tekrar basınca durur (pad'ler drone/loop için). |
| 7 | Small improvement: show track name when arming | 5 | Kolay | < 0.5 KB | Bildirimde track adı; yukarıdakiyle birlikte. |
| 8 | Fast select midi channel using pads | 5 | Kolay | < 0.5 KB | MIDI track'te Instr basılıyken pad 1..16 = MIDI kanal 1..16. |
| 9 | Live record erase button | 4 | Kolay | < 0.5 KB | 'Delete notes while in live record' ile aynı; birlikte yapılır. |
| 10 | MIDI Record Mode that maps keys to Instruments instead of Musical Notes | 4 | Kolay | < 0.5 KB | MIDI in kaydında nota numarası = enstrüman numarası modu (davul kit kaydı için). |
| 11 | Enable dedicated pad mode for Perc engine | 3 | Kolay | < 0.5 KB | PERC synth seçiliyken pad'ler kromatik yerine PERC'in davul elemanlarını çalar. |
| 12 | Polyphony: Cycle through Armed Tracks Option | 2 | Kolay | < 0.5 KB | Birden çok track arm'lıyken her yeni pad notası sıradaki track'e gider (round-robin polifoni). |
| 13 | Show the number track on Arm/Disarm | 2 | Kolay | < 0.5 KB | Arm/disarm bildiriminde track numarası. Tek satır. |
| 14 | Light up root notes on Tracker | 2 | Kolay | < 0.5 KB | Nota pad düzeninde proje kök notasını parlak, ölçek notalarını yarım ışıkla göstermek. |
| 15 | Preroll with visual timer | 2 | Kolay | < 0.5 KB | Pre-roll sırasında ekranda geri sayım (4-3-2-1) büyük rakamla. |
| 16 | Allow the grid pad to enter steps, like the Play | 0 | Kolay | < 0.5 KB | Pad modu 'Step': 48 pad = step'ler, basınca cursor track'inde step aç/kapa (Play'deki gibi). Pattern ekranında ayrı mod. |

## Sample Editor (offline) (23)

| # | Request | Yanıt | Zorluk | Flash | Notum |
|---|---|---|---|---|---|
| 1 | Convert samples to mono | 31 | Kolay | < 0.5 KB | Sample Editor efekti 'To mono' (L+R/2, ya da sadece L / sadece R). Stereo sample'ın belleği yarıya iner. 31 yanıt. |
| 2 | Chromatic tuner / Spectral analysis for Sample Tuning | 12 | Kolay | 0.5–2 KB | Seçimin perdesini (YIN algoritması) bulup 'A3 +12 cent' gösterir. Offline, birkaç KB. |
| 3 | Sample Pitchshift-to-Tempo Calculator/Function | 12 | Kolay | < 0.5 KB | Sample uzunluğu + bar sayısından BPM'i, proje tempo'suna uyması için gereken yarım tonu hesaplar; tek tuşla tune'a yazar. |
| 4 | Sample Editor Window Sample Length | 12 | Orta | 0.5–2 KB | Editörün düzenleyebildiği maksimum sample uzunluğunu artırmak; düzenleme tampon belleğine bağlı (SDRAM), ölçüp sınırı genişletmek gerekiyor. |
| 5 | Cut function in Sample Editor | 8 | Kolay | < 0.5 KB | Seçimi siler, kalanları birleştirir (Crop'un tersi). Kesik yerine 5 ms crossfade ile tık önlenir. |
| 6 | HPF and LPF with resonance in the sample editor | 8 | Kolay | 0.5–2 KB | Offline biquad (HP/LP/BP, frekans + rezonans + mix). Ses yoluna değil sample'a uygulanır; CPU sorunu yok. |
| 7 | Trim Silence in Sample Editor | 7 | Kolay | < 0.5 KB | Baş ve sondaki eşik altı sessizliği kırpar (eşik ayarlı). |
| 8 | Noise color Effect for Sample Editor | 7 | Kolay | < 0.5 KB | Beyaz/pembe/kahverengi gürültü ekleme ya da seçimi gürültüyle doldurma (mix ayarı). |
| 9 | Pitch Up/Down Effect for Sample Editor | 5 | Kolay | 0.5–2 KB | Yarım ton + cent ile yeniden örnekleme (süre değişir) ya da mevcut Timestretch Note ile birlikte süre korunur. |
| 10 | Have a loop option for cropping in the Sample Editor | 5 | Kolay | < 0.5 KB | Crop'a 'loop noktalarına göre' seçeneği. |
| 11 | Quick-Crop from Sample Playback Loop points | 5 | Kolay | < 0.5 KB | Sample Playback'te kısayol: loop noktalarına crop. Yukarıdakiyle aynı fonksiyon. |
| 12 | Option: Automatically assign sample's pitch to its note to keep all samples in tune | 5 | Kolay | < 0.5 KB | Tuner'ın sonucunu enstrümanın tune/finetune'una yazan bir tuş. Tuner'la birlikte. |
| 13 | Crossfade Loops / Loop-points | 4 | Kolay | < 0.5 KB | Loop sonunu loop başının öncesiyle crossfade eder; tıksız loop. Sample Playback'teki loop noktalarını kullanır. |
| 14 | Sample Editor - Chorus improvements | 4 | Orta | 0.5–2 KB | Mevcut Chorus'a stereo genişlik / rate / feedback parametreleri; offline efekt, CPU sorunu yok. |
| 15 | More destructive processing options Eg. cut and paste, frequency shift, etc | 3 | Kolay | 0.5–2 KB | Cut kolay; paste için sample panosu (SDRAM tampon) gerekir. Frekans kaydırma: Hilbert dönüşümüyle offline, Transcode'daki FFT yardımcılarını kullanır. |
| 16 | BPM calculator in sample editor | 3 | Kolay | < 0.5 KB | Seçimin uzunluğundan (1/2/4/8 bar varsayımıyla) BPM gösterir. Pitch-to-tempo ile aynı ekran. |
| 17 | Sample Editing Seek via Pads | 3 | Kolay | < 0.5 KB | Sample Editor'da pad'ler sample'ı 48 parçaya böler; pad'e basmak o noktadan önizleme çalar ve marker'ı oraya koyar. |
| 18 | Sample Editor – Add silence / dead-space | 2 | Kolay | < 0.5 KB | Seçim yerine / sona N ms sessizlik ekler. |
| 19 | Basic utility effects | 1 | Kolay | < 0.5 KB | Faz ters, kanal takası, DC offset temizleme, L/R tek kanala kopyalama: hepsi tek bir 'Utility' efektinde. |
| 20 | Wavetable Sampling and Creation Automation | 1 | Orta | 0.5–2 KB | Bir sample'ı N eşit parçaya bölüp her parçadan tek döngü çıkararak wavetable üretir; WT Smoother ile birlikte. |
| 21 | Crossfade loop option | 0 | Kolay | < 0.5 KB | Yukarıdakiyle aynı. |
| 22 | Invert phase FX? | 0 | Kolay | < 0.5 KB | Faz ters çevirme (L, R ya da ikisi). |
| 23 | See slices in Sample Editor | 0 | Kolay | < 0.5 KB | Dilimli enstrümanda slice çizgileri dalga formunun üstünde. |

## Sample Playback / Loader / dilimler (27)

| # | Request | Yanıt | Zorluk | Flash | Notum |
|---|---|---|---|---|---|
| 1 | Beat Slice mode allowing to play through to end of sample | 37 | Kolay | < 0.5 KB | Beat Slice'ta 'slice sonuna kadar' yerine 'sample sonuna kadar' seçeneği (enstrüman ayarı; fork uzantısında ya da Config'te global). 37 yanıt. |
| 2 | Import stereo samples as 2 mono samples | 13 | Kolay | < 0.5 KB | Loader'da stereo dosyayı L ve R olarak iki enstrümana yükleme seçeneği. 13 yanıt. |
| 3 | Import a sample 'chain' feature | 9 | Orta | 0.5–2 KB | Bir klasördeki birden çok sample'ı uç uca ekleyip tek dilimli enstrüman yapar (her dosya bir slice). Bellek: toplam uzunluk sınırı kontrolü. |
| 4 | Sample Loader - Wavetable Preview Mode | 9 | Kolay | < 0.5 KB | Wavetable dosyalarını loader'da tek döngü loop + pencere taramasıyla önizleme. |
| 5 | Auto-place notes for an instruments slices | 8 | Kısmen var | < 0.5 KB | Kısmen var: BBCut generator dilimleri yazıyor. Eksik: dilim 1..N'yi sırayla step'lere yazan düz 'Slices in order' generator'ı. Generate popup'ına 7. generator. |
| 6 | Swapping instruments between projects | 8 | Orta | 0.5–2 KB | Başka bir projenin enstrüman listesinden içe aktarma: o projenin .pti dosyası + sample'ı kopyalanır. |
| 7 | Add Slice to be respective of distance between prior Slices | 7 | Kolay | < 0.5 KB | Yeni slice'ı, önceki iki slice arasındaki mesafe kadar ileriye otomatik koyar (eşit aralıklı devam). |
| 8 | Flag for long samples in Sample Loader - if too long to edit | 6 | Kolay | < 0.5 KB | Editörün sınırından uzun dosyaların yanında işaret. |
| 9 | Switch a synth/midi instrument slot or replace an instrument with another | 6 | Kolay | < 0.5 KB | 'Replace instrument': seçili pattern'lerde (ya da tüm projede) enstrüman X'i Y ile değiştirir. |
| 10 | We need the notes with beat slices highlighted | 5 | Kolay | < 0.5 KB | Beat Slice enstrümanında dilimi olan pad'ler yanar, boşlar sönük. |
| 11 | Rename sample file in SD Card to mark favourites | 5 | Kolay | < 0.5 KB | Loader'da dosyayı yeniden adlandırma (klavye zaten var); ad başına '*' koyan 'favori' kısayolu. |
| 12 | Delete samples/files on the Tracker | 5 | Kısmen var | < 0.5 KB | Kısmen var: projeler ve .mod silinebiliyor. Eksik: loader'da sample dosyası silme (onaylı). |
| 13 | Undo sample load | 5 | Orta | 0.5–2 KB | Yüklemeden önceki enstrümanı (sample dahil) SDRAM'de tutup geri almak; büyük sample'larda bellek sınırlı, sadece son yükleme. |
| 14 | Beat Slice – Padgrid chopping | 4 | Kolay | < 0.5 KB | Sample çalarken pad'e basınca o anki konuma slice koyar (manuel chop). |
| 15 | Auto-Preview Option for Previewing Samples | 4 | Kolay | < 0.5 KB | Loader'da cursor bir dosyaya gelince otomatik önizleme (Config seçeneği). |
| 16 | Option to change Beat Slice Root Note | 3 | Kolay | < 0.5 KB | Beat Slice'ın ilk dilimi hangi notaya denk gelecek (şu an 24 + n). Sadece pad/nota eşlemesi; dosyadaki notalar aynı kalır. |
| 17 | Previewing single cycle waveforms should default to forward loop | 2 | Kolay | < 0.5 KB | Loader önizlemesinde kısa (≤ 2048 örnek) sample'ları ileri loop'la çalmak. |
| 18 | Sample Playback - Granular Mode - Set Position to Exact Middle | 2 | Kolay | < 0.5 KB | Granular pozisyon alanında Shift+Delete = %50. |
| 19 | Displaying long samples as a waveform | 2 | Orta | 0.5–2 KB | Uzun sample'larda dalga formunu özetleyerek (min/max blokları) çizmek; ilk çizimde gecikme olabilir, arka planda hesaplanır. |
| 20 | Beat Slice - "Loop options" | 2 | Orta | 0.5–2 KB | Dilim başına loop (ileri/ping-pong) seçeneği; ses motorunda slice loop'u. Dosyada yeri yok, fork uzantısı. |
| 21 | Show beatslices on Padgrid in non-recording mode too | 1 | Kolay | < 0.5 KB | Yukarıdakiyle aynı; kayıt modunda olmasa da. |
| 22 | Add 'Zoom' functionality to Granular Playback settings page | 1 | Kolay | < 0.5 KB | Granular sayfasına Sample Playback'teki zoom'u eklemek. |
| 23 | Sample Playback UI: Move "Zoom" to left side of screen for Slice & Beat Slice modes | 1 | Kolay | < 0.5 KB | Sadece buton yerleşimi. |
| 24 | Manual Slicing As Sample Is Playing (chop) | 0 | Kolay | < 0.5 KB | Yukarıdakiyle aynı fonksiyon. |
| 25 | import loop point already embedded in a .wav files | 0 | Kolay | < 0.5 KB | WAV 'smpl' chunk'ındaki loop noktalarını okuyup Sample Playback loop'una yazar. |
| 26 | Sample preview note setting | 0 | Kolay | < 0.5 KB | Önizlemenin çalacağı nota (C3/C4/C5) Config'te. |
| 27 | move instrument in instrument list | 0 | Kolay | < 0.5 KB | Enstrümanı listede taşırken tüm pattern'lerdeki enstrüman numaraları yeniden eşlenir. |

## Sample Recorder ve radyo (12)

| # | Request | Yanıt | Zorluk | Flash | Notum |
|---|---|---|---|---|---|
| 1 | Record perfect loops with external synth | 15 | Orta | 0.5–2 KB | Saate bağlı kayıt: sequencer çalarken tam N bar kaydeder, gecikme (round-trip) ölçülüp telafi edilir. Live resampling altyapısı üzerine. |
| 2 | System-wide LoFi Sampling Mode (aka "SP-Mode") | 11 | Kısmen var | 0.5–2 KB | Kısmen var: Transcode / DCT Crush offline. Eksik: kaydı doğrudan 12 bit / 26 kHz gibi lo-fi alan bir Recorder modu (kayıt sonrası otomatik işlem). |
| 3 | Disable screen backlight while recording samples to remove device noise | 10 | Kolay | < 0.5 KB | Kayıt sırasında ekran parlaklığını kapatma seçeneği (Config). Herhangi bir tuşla geri gelir. |
| 4 | Sampling directly to slices | 5 | Orta | 0.5–2 KB | Her kayıt aynı enstrümanın sonuna yeni slice olarak eklenir (bellek sınırına kadar). |
| 5 | Dual Mono Option for Passthrough Line In Audio | 5 | Kısmen var | < 0.5 KB | Kısmen var: Line in channel ayarı. Eksik: passthrough için L'yi iki kanala çoğaltma seçeneği. |
| 6 | More States for the Metronome | 3 | Kolay | < 0.5 KB | Metronoma 'sadece kayıtta / sadece pre-roll / her zaman' ve vurgu seçenekleri. |
| 7 | Temporary Radio tuning presets | 2 | Kolay | < 0.5 KB | Recorder'da 8 radyo istasyonu kısayolu (pad'ler), oturum boyunca saklanır. |
| 8 | Manually enter radio frequency instead of scanning | 2 | Kolay | < 0.5 KB | Frekansı encoder'la 0.1 MHz adımla ya da sayı girişiyle ayarlamak. |
| 9 | Sample Recording - Ability to choose Left or Right channel | 2 | Kısmen var | < 0.5 KB | Kısmen var: Config > Line in channel. Eksik: Recorder ekranında doğrudan seçim. |
| 10 | Play Track While Recording Sample | 1 | Kısmen var | < 0.5 KB | Kısmen var: live resampling pattern çalarken kaydediyor. Eksik: Sample Recorder'da (Line In) kayıt sırasında pattern çalma. |
| 11 | Metronome in Sample recorder / Sample editor | 0 | Kolay | < 0.5 KB | Recorder'da kayıt sırasında metronom (kayda girmez, sadece kulaklık/çıkış). Metronom kodu var. |
| 12 | Copying the name of multiple recorded samples | 0 | Kolay | < 0.5 KB | Art arda kayıtlara artan isim (Kick 01, Kick 02 …) vermek. |

## MIDI (25)

| # | Request | Yanıt | Zorluk | Flash | Notum |
|---|---|---|---|---|---|
| 1 | Additional MIDI CC In Controls / External controller as a full-featured control surface | 29 | Orta | 0.5–2 KB | CC in haritasını genişletmek: transport, pattern seçimi, track mute, Performance FX'leri. Haritalama tablosu flash'ta yer tutar. |
| 2 | Listen to Program Change, to change patterns via Midi | 14 | Kolay | < 0.5 KB | MIDI PC n gelince sonraki pattern n olur (anında / bar sonunda seçeneği). 14 yanıt. |
| 3 | Custom MIDI chords | 14 | Orta | 0.5–2 KB | Kullanıcı akor tablosu (8 akor), fork uzantısında saklanır. |
| 4 | Change Instrument via MIDI in Synth Mode | 13 | Kolay | < 0.5 KB | MIDI PC ile aktif enstrümanı değiştirmek (Pattern dışında, çalma modunda). |
| 5 | Fluid MIDI pitchbend | 13 | Orta | 0.5–2 KB | Micro-tune/Pitchbend FX ve Glide'ı MIDI track'lerde adım adım pitchbend mesajlarına çevirmek (yumuşak geçiş). |
| 6 | MIDI thru should forward the channel data it receives | 8 | Kolay | < 0.5 KB | Config > MIDI thru: gelen kanal mesajlarını DIN out'a aynen geçirir. |
| 7 | Route MIDI note input to each track, on an assignable MIDI channel, for triggering instruments and recording | 8 | Orta | 0.5–2 KB | Her track'e bir MIDI giriş kanalı; o kanaldan gelen nota o track'in enstrümanını çalar ve kayıtta oraya yazılır. |
| 8 | To be able to address 32 Midi channels | 8 | Orta | 0.5–2 KB | DIN ve USB'yi ayrı port olarak kullanmak: MIDI enstrümanında port seçimi (DIN / USB / ikisi). |
| 9 | Trigger Polyend Tracker patterns via MIDI from an External Source | 6 | Kolay | < 0.5 KB | PC ile pattern değiştirme ile aynı; ayrıca nota ile pattern seçimi seçeneği. |
| 10 | Placeholder sound for midi instrument | 6 | Orta | 0.5–2 KB | MIDI track'te harici cihaz yokken dinlemek için seçilen bir sample'ı da çalmak. |
| 11 | Save / Load MIDI Instruments | 5 | Kolay | < 0.5 KB | MIDI enstrüman ayarlarını (kanal, ad, CC'ler) SD'ye kaydedip başka projede yükleme. |
| 12 | Add support for sending/receiving MIDI Song Position Pointer (SPP) and/or MIDI Timecodes (MTC) | 4 | Orta | 0.5–2 KB | SPP gönderme/alma (song içinde konuma atlama). MTC'ye gerek yok, SPP yeterli. |
| 13 | Master page for Midi | 4 | Orta | 0.5–2 KB | MIDI track'ler için ana sayfa: kanal başına velocity ölçeği, transpoze. |
| 14 | Add MIDI CCs to control delay settings | 3 | Kolay | < 0.5 KB | CC in haritasına delay zamanı / feedback / mix eklemek. |
| 15 | Chord FX enhancements | 3 | Orta | 0.5–2 KB | Akor ters çevirme (inversion) ve açık voicing seçenekleri. |
| 16 | Expanded midi control of performance mode on the Tracker Mini | 3 | Orta | 0.5–2 KB | OG için de geçerli: Performance FX'lerini CC ile kontrol etmek. |
| 17 | Midi inforbar for incoming midi information | 2 | Kolay | < 0.5 KB | Başlık çubuğunda son gelen MIDI mesajı (kanal, nota/CC, değer) birkaç saniye. |
| 18 | Extend MIDI Chord/ARP combo possibilities | 2 | Orta | 0.5–2 KB | Chord + Arp birleşimlerine yeni tablolar. |
| 19 | Engine - Midi Perc | 2 | Orta | 0.5–2 KB | MIDI track'te davul haritası: pad/nota → MIDI nota eşlemesi tablosu. |
| 20 | Program change in instrument mode | 1 | Kolay | < 0.5 KB | Yukarıdakiyle aynı. |
| 21 | Auto option for MIDI sync/transport | 1 | Kolay | < 0.5 KB | Clock in 'Auto': dış saat gelirse ona kilitlenir, gelmezse iç saat. |
| 22 | MIDI Parameters in performance mode | 1 | Orta | 0.5–2 KB | Performance FX'lerinde MIDI track'ler için CC gönderme hedefi. |
| 23 | Using Midi Channel to control Either Performace mode OR putting Additional FX | 0 | Orta | 0.5–2 KB | Ayrı bir MIDI kanalını Performance kontrolüne ayırma; yukarıdakiyle birlikte. |
| 24 | Give the ability for the tracker to follow the user by 'listening' for midi note events | 0 | Orta | 0.5–2 KB | Gelen MIDI notalarla sequencer'ı başlatma / cursor'u takip ettirme; ne istendiği tam net değil, okumak gerek. |
| 25 | Pitch bending support on midi | 0 | Kısmen var | < 0.5 KB | Kısmen var: Micro-tune/Pitchbend FX. Eksik: MIDI in pitchbend mesajını sample/synth çalarken uygulamak. |

## Performance (7)

| # | Request | Yanıt | Zorluk | Flash | Notum |
|---|---|---|---|---|---|
| 1 | Generate new patterns in Performance mode | 17 | Kısmen var | 0.5–2 KB | Kısmen var: B1..B10 play modları ve Generate popup. Eksik: Performance ekranından bir tuşla seçili track'e yeni varyasyon üretmek (Genotype/BBCut canlı). |
| 2 | Improve immediate pattern switching behaviour in Performance mode | 8 | Orta | 0.5–2 KB | Pattern değişimini 'anında / track sonu / bar sonu' seçenekleriyle quantize etmek. |
| 3 | New Performance Mode Effect - Gate Length | 3 | Orta | 0.5–2 KB | Yeni Performance FX: tüm notaların gate'ini yüzdeyle kısaltma. |
| 4 | Ability to make all tracks switch patterns with a single shortcut in performance mode | 2 | Kolay | < 0.5 KB | Performance'ta Shift + pattern seçimi = tüm track'ler o pattern'e geçer. |
| 5 | New Performance Mode Effect - Chance | 2 | Orta | 0.5–2 KB | Yeni Performance FX: step'leri olasılıkla atlama. Sequencer'da tek koşul ama yeni bir FX yuvası. |
| 6 | Add a Master Effects page to Performance Mode | 2 | Orta | 0.5–2 KB | Performance'ta reverb/delay/limiter ana ayarlarına hızlı sayfa. |
| 7 | Tracker Perform Jog Wheel Macros | 2 | Orta | 0.5–2 KB | Performance'ta encoder'ı birden çok FX'e aynı anda bağlayan makro. |

## Mixer ve master (7)

| # | Request | Yanıt | Zorluk | Flash | Notum |
|---|---|---|---|---|---|
| 1 | Highpass Filter for Reverb | 9 | Orta | 0.5–2 KB | Reverb girişine tek kutuplu HPF (frekans ayarı). Ses yolunda çok ucuz ama CPU ölçümü şart. |
| 2 | Mute Synths From Global Mixer Page | 8 | Kolay | < 0.5 KB | Mixer'da synth sütunlarına mute/solo (şu an sadece seviye). |
| 3 | Mute/Solo for all 16 tracks (not just the new synths) in Master Mixer | 7 | Kolay | < 0.5 KB | Mixer'da 13-16. (MIDI) track'lere mute/solo. |
| 4 | Optional possibility to set master settings per pattern instead of per project | 7 | Orta | 0.5–2 KB | Pattern başına master ayarı (fork uzantısı); pattern değişince yumuşak geçiş gerekir. |
| 5 | Master EQ bypass and the ability to save and load different master EQ settings | 4 | Kolay | < 0.5 KB | Master EQ'ya bypass ve SD'ye kaydet/yükle. |
| 6 | Soloing one track should mute all tracks (including MIDI) | 3 | Kolay | < 0.5 KB | Solo'nun MIDI track'leri de susturması (seçenek). |
| 7 | Sample Volume sent to effects not effected by Volume step FX | 1 | Orta | 0.5–2 KB | Send'leri pre/post-fader seçilebilir yapmak; ses yolunda küçük değişiklik. |

## Synth ve enstrüman (25)

| # | Request | Yanıt | Zorluk | Flash | Notum |
|---|---|---|---|---|---|
| 1 | Support for LFO Offsets | 23 | Orta | 0.5–2 KB | LFO faz/başlangıç offset'i; enstrüman ayarı. |
| 2 | Glide without Sample retrigger | 19 | Orta | 0.5–2 KB | Glide'da sample'ı baştan başlatmadan perdeyi kaydırmak (legato). Voice kodunda değişiklik, ses kesilmesi testi. |
| 3 | Granular Length Step FX | 16 | Orta | 0.5–2 KB | Yeni fork FX'i: granular grain uzunluğu step başına. |
| 4 | Configurable LFO retrigger in Instrument Parameters | 14 | Orta | 0.5–2 KB | LFO'nun notayla yeniden başlayıp başlamayacağı; enstrüman ayarı fork uzantısında. |
| 5 | Offline synths for sample creation | 13 | Kısmen var | 0.5–2 KB | Kısmen var: live resampling. Eksik: synth notasını tek tuşla sample'a render etmek (sequencer çalmadan). |
| 6 | Polyend Synth Engines: ACD - Distortion effect | 11 | Orta | 0.5–2 KB | ACD voice'a soft-clip distorsiyon (drive). Voice başına birkaç çarpma; QEMU CPU ölçümü. |
| 7 | Step FX for: Automation LFO Amount | 9 | Orta | 0.5–2 KB | Yeni fork FX'i: LFO miktarı step başına. |
| 8 | Adjustable slew on LFO shapes | 9 | Orta | 0.5–2 KB | LFO çıkışına slew (yumuşatma). |
| 9 | Change Envelope Automation Values via StepFX | 8 | Orta | 0.5–2 KB | Yeni fork FX'leri: attack/decay/release step başına. |
| 10 | Negative amounts for Modulation sources | 7 | Orta | 0.5–2 KB | Modülasyon miktarına negatif değer; enstrüman formatında işaretli alan gerekebilir, fork uzantısı. |
| 11 | Expand Envelope / LFO shapes (akin to the wavetables in XAOC Zadar) | 6 | Orta | 0.5–2 KB | Yeni LFO şekilleri (rampalar, adımlı, rastgele yumuşak). Ses yolunda tablo okuma, CPU önemsiz. |
| 12 | Simple way to preview all PERC elements while editing | 5 | Kolay | < 0.5 KB | PERC düzenlerken pad'ler tüm elemanları çalar (dedicated pad mode ile birlikte). |
| 13 | Better value scaling for more precise control over small values (eg. Envelope attack time) | 5 | Kolay | < 0.5 KB | Encoder ivmesi: küçük değerlerde ince adım, Shift ile kaba adım. |
| 14 | FX Parameter Glide | 5 | Orta | 0.5–2 KB | FX değerlerinin step'ler arasında yumuşak geçişi (filter cutoff glide gibi). |
| 15 | LFO speeds to be independent of Tempo | 4 | Orta | 0.5–2 KB | LFO'ya Hz modu (tempo'dan bağımsız). |
| 16 | Filter Keyboard/Note Tracking | 4 | Orta | 0.5–2 KB | Filtre frekansının notayla birlikte kayması (miktar ayarı). |
| 17 | Real-time sample rate reduction parameter and step effect ("bitcrush" sound) | 4 | Kısmen var | 0.5–2 KB | Kısmen var: Bit Depth FX. Eksik: örnekleme hızı düşürme (sample-and-hold) FX'i; ses yolunda ucuz ama yeni FX ve CPU ölçümü. |
| 18 | Optionally set Envelope AD and R times with Step-based lengths | 3 | Orta | 0.5–2 KB | Zarf zamanlarını tempo'ya bağlı step cinsinden girme seçeneği. |
| 19 | Instrument Groups | 3 | Orta | 0.5–2 KB | Enstrümanları gruplayıp grup seviyesi/mute (ör. davullar). |
| 20 | Volume LFO Speed - Add up to 128steps | 2 | Orta | 0.5–2 KB | Volume LFO FX değer aralığını 128 step'e genişletmek; stok firmware'de değer kırpılır. |
| 21 | Adjustable Delay stage for the Envelopes | 2 | Orta | 0.5–2 KB | Zarfın başına delay aşaması (DAHDSR). |
| 22 | Vibrato FX Command | 2 | Kısmen var | < 0.5 KB | Kısmen var: Finetune LFO FX vibrato yapıyor. Eksik: hız+derinlik tek FX'te (klasik tracker 4xy); gerekirse takma ad. |
| 23 | Link multiple instruments / parts | 1 | Orta | 0.5–2 KB | Birden çok enstrümanın aynı notayla birlikte çalması (katmanlama). |
| 24 | Reload Instrument Parameters/ Reload Sample Pool/ Reload Pattern | 1 | Orta | 0.5–2 KB | Kaydedilmiş halden enstrüman / sample / pattern'i geri yüklemek. |
| 25 | Infinity value for sample volume release | 0 | Orta | 0.5–2 KB | Release'e 'sonsuz' (OFF gelene kadar) değeri. |

## Ekran ve arayüz (10)

| # | Request | Yanıt | Zorluk | Flash | Notum |
|---|---|---|---|---|---|
| 1 | Option to disable automatic audio previews while editing patterns | 10 | Kolay | < 0.5 KB | Config: pattern düzenlerken önizleme çalma kapalı. |
| 2 | Audio "Status Bar" at the top of the UI | 8 | Kolay | < 0.5 KB | Başlık çubuğunda küçük master seviye göstergesi (display budget içinde). |
| 3 | Option to change note display in the Tracker from sharps (#) to flats (b) | 5 | Kolay | < 0.5 KB | Config'te '♯ / ♭' gösterim seçeneği; notes.cpp'deki isim tablosu. |
| 4 | Display which Instrument is featured on which Patterns | 5 | Kolay | < 0.5 KB | Enstrüman listesinde seçili enstrümanın kullanıldığı pattern numaraları. |
| 5 | Horizontal display mode zoomed out to see 16 steps without page scroll | 5 | Kısmen var | < 0.5 KB | Kısmen var: Horizontal arrangement. Eksik: 16 step'i tek ekrana sığdıran sıkı (sadece nota) görünüm. |
| 6 | Track Mixer : Colour tracks in Edit mode by Note, Instrument, Fx1, Fx2 keys | 3 | Kolay | < 0.5 KB | Pattern'de tuşa basılıyken track'leri o sütunun içeriğine göre renklendirme. |
| 7 | Screensaver or other idle screen options | 3 | Kolay | < 0.5 KB | Boşta N dakika sonra Data ekranı (barcode) ya da karartma; herhangi bir tuşla geri. |
| 8 | Second pattern divider with a different colour | 2 | Kolay | < 0.5 KB | Pattern divider'a ikinci bir aralık (ör. 4 ve 16) farklı renkle. |
| 9 | Sample Loader - Colour code Instruments using Note, Instrument, Fx1, Fx2 keys | 1 | Kolay | < 0.5 KB | Loader listesinde enstrümanları türüne (sample/synth/MIDI, slice'lı) göre renk. |
| 10 | More gridpads being led to support the Letter keyboard | 1 | Kolay | < 0.5 KB | Ekran klavyesi açıkken harflere karşılık gelen pad'leri yakmak. |

## Metin ve klavye (7)

| # | Request | Yanıt | Zorluk | Flash | Notum |
|---|---|---|---|---|---|
| 1 | New Option - Change Keyboard Layout (QWERTZ) | 5 | Kolay | < 0.5 KB | Klavye düzeni seçeneği (QWERTY/QWERTZ/AZERTY); sadece harf tablosu. |
| 2 | Keyboard UX - Use jogwheel to navigate between characters | 4 | Kolay | < 0.5 KB | Klavyede encoder imleci karakterler arasında taşır. |
| 3 | Integrate Notepad Feature into Polyend Tracker for In-Device Project Notes | 3 | Kolay | < 0.5 KB | Proje klasöründe notes.txt; ekranda gösterme ve klavyeyle düzenleme. |
| 4 | Naming / Renaming UX Improvements | 2 | Kolay | < 0.5 KB | İsim klavyesinde son kullanılan isimler, büyük/küçük harf kısayolu, sayı artırma. |
| 5 | Text Editing Shortcut - Select all in text edit with double-click Center / Enter button | 2 | Kolay | < 0.5 KB | Encoder push çift tık = tümünü seç. |
| 6 | Naming patterns and tracks in a flat file | 2 | Kolay | < 0.5 KB | Projede names.txt varsa pattern ve track adlarını oradan okur / oraya yazar. |
| 7 | Text Editing Shortcut - Select Text Letter by Letter in Text Edit with Shift + Left/Right | 1 | Kolay | < 0.5 KB | Shift+Left/Right ile harf harf seçim. |

## Dosya ve export (18)

| # | Request | Yanıt | Zorluk | Flash | Notum |
|---|---|---|---|---|---|
| 1 | Better Project Management/Search | 11 | Orta | 0.5–2 KB | Projeleri tarihe/ada göre sıralama ve harfle atlama; arama için klavye. |
| 2 | Project Defaults | 10 | Kolay | < 0.5 KB | 'Default' adlı proje varsa New Project onu şablon olarak kopyalar. |
| 3 | Proper Incremental naming of song stem folders | 8 | Kolay | < 0.5 KB | Stem klasörü varsa üzerine yazmak yerine '_2', '_3'. |
| 4 | Export looping songs | 8 | Kolay | < 0.5 KB | Render sonunda reverb/delay kuyruğunu başa ekleyerek kusursuz loop'lanan dosya. |
| 5 | Add BPM to export filenames | 6 | Kolay | < 0.5 KB | Export adına '_174bpm'. |
| 6 | Render stems per instrument and not per track to help the mixing / mastering process | 6 | Orta | 0.5–2 KB | Enstrüman başına render: her enstrüman için ayrı geçiş (render süresi uzar). |
| 7 | Stem Export Filenames should use custom Track Names if available | 4 | Kolay | < 0.5 KB | Stem dosyaları şu an track1.wav…; track adı varsa '01 Kick.wav'. render.cpp'deki sabit liste. |
| 8 | Pseudo real-time clock for easier file management | 4 | Kolay | < 0.5 KB | Config'te tarih/saat ayarı + çalışma süresi; dosyalar FAT zaman damgası alır (pil olmadığı için her açılışta son kayıtlı tarihten devam). |
| 9 | Set initial / default tempo in Config | 3 | Kolay | < 0.5 KB | Config'te yeni proje tempo'su. |
| 10 | File button section - Use Insert key to enter folder | 3 | Kolay | < 0.5 KB | File/Loader'da Insert tuşu klasöre girer. |
| 11 | CUT/COPY/PASTE (Browser, Sample Editor... Everywhere) | 3 | Orta | 0.5–2 KB | Dosya tarayıcıda kes/kopyala/yapıştır (dosya taşıma) ve Sample Editor panosu; ikisi ayrı işler. |
| 12 | Introduce tagging of synth patches, to improve patch management and browsing | 3 | Orta | 0.5–2 KB | Patch dosya adına etiket ön eki ya da yan dosya; tarayıcıda etikete göre filtre. |
| 13 | Meaningful auto-naming scheme for rendered selection | 2 | Kolay | < 0.5 KB | Seçim render'ına 'Proje_P03_T05_174bpm' gibi ad. |
| 14 | Song Export improvements | 2 | Orta | 0.5–2 KB | Başlık geniş; yukarıdaki export maddeleri (adlar, loop, pattern başına) çoğunu karşılar. |
| 15 | Make menu mappable to function buttons | 2 | Orta | 0.5–2 KB | Sık kullanılan menü öğelerini F tuşlarına atamak (Config'te tablo). |
| 16 | Ability to store/link and recall single track sequences across patterns | 1 | Orta | 0.5–2 KB | Track içeriğini SD'de bir 'track kütüphanesine' kaydedip başka pattern/projede çağırma. |
| 17 | Export each pattern in song | 0 | Kolay | < 0.5 KB | Song'daki her pattern'i ayrı WAV olarak (toplu render). |
| 18 | Default project track length | 0 | Kolay | < 0.5 KB | Project Defaults ile birlikte; ya da Config'te varsayılan pattern uzunluğu. |

## Toplam

- 144 kolay, 71 orta, 12 kısmen var.
- Flash: 147 madde < 0.5 KB, 80 madde 0.5–2 KB. Ortalama ~300 B ve ~1.2 KB ile toplam yaklaşık 140 KB: 720 KB'lık yumuşak sınıra sığmaz (orada ilk 50–60 kolay madde sığar), stok imaj alanının sert sınırına (~175 KB boş) sığar.
- Aynı işi gören maddeler (ör. rotation / slide projector, chop / padgrid chopping, make unique / duplicate) tek seferde çözülür; gerçek iş sayısı listedekinden az.
