// Song (spec 06 §2; stock layout spec 20 §3.18 / §4.3, VISUAL_DIFF §3.9).
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "app/project_fields.h"
#include "ui/screens.h"
#include "ui/widgets.h"

namespace ot {

namespace {
constexpr float kTempoMin = 40.0f, kTempoMax = 800.0f;   // spec 06 §2.1
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Stock layout (spec 20 §3.18 / §4.3, VISUAL_DIFF §3.9 [S snap 010]).
constexpr int kRows = 12;            // visible slots (grid rows = list rows)
constexpr int kPitchX = 40;          // grid column pitch
constexpr int kPitchY = 27;          // grid / list row pitch
constexpr int kGridX = 5;            // dark tile box (5 + 40 c, 92 + 27 r, 39 x 26)
constexpr int kGridY = 92;
constexpr int kListNumX = 520;       // CMD_NUMBER at rightX - 178 (rightX 698)
constexpr int kListNameX = 550;      // text at rightX - 148
constexpr int kListX0 = 491, kListX1 = 688;   // selection outline centre lines
constexpr int kListY0 = 91;          // first row outline top (rows 91 + 27 r .. 118 + 27 r)
constexpr int kScrollX = 692, kScrollY = 94, kScrollH = 320;
constexpr int kHeaderX = 20;         // rotated header ink column (x 20 + 40 c)
constexpr int kHeaderBottom = 87;    // headers read upward from here
constexpr int kHeaderSpace = 5;      // vertical-text space advance
int gListTop = 0;                    // first visible slot (the only Song screen)

// Song bottom labels 0..5: two-line set +0x44 (both lines grey, font 3),
// disabled +0x54 (both 333333) [spec 20 §4.3, S snap 010]. wg::barLabel has
// no both-grey variant, so these are drawn here (local helper).
void songLabelText(Gfx& g, const Theme& th, int cx, const BarLabel& l) {
    const uint32_t c = th.slot(l.enabled ? 17 : 21);   // set +0x44 / +0x54 text
    const uint32_t c2 = th.slot(l.enabled ? 20 : 24);  // their line-2 colour
    if (l.bottom[0]) {
        g.text(cx, wg::kLabelY1, kFont3, c, l.top, IDisplay::kOptCenter);
        g.text(cx, wg::kLabelY2, kFont3, c2, l.bottom, IDisplay::kOptCenter);
    } else {
        g.text(cx, wg::kLabelY, kFont3, c, l.top, IDisplay::kOptCenter);
    }
}

// Double triangle (icons 3 / 4, our own drawing): two 6 x 13 triangles,
// tip-to-base 10 px apart, pointing left (dir -1) or right (+1) from x0.
void doubleTriangle(Gfx& g, int x0, int cy, int dir, uint32_t c) {
    g.rectsBegin(c);
    for (int t = 0; t < 2; ++t)
        for (int k = 0; k < 6; ++k) {
            const int x = x0 + dir * (10 * t + k);
            g.rectsAdd(x, cy - k, 1, 2 * k + 1);
        }
    g.rectsEnd();
}

void songWideLabel(Gfx& g, const Theme& th, int i, const BarLabel& l) {
    const int cx = wg::labelCentre(i, true);
    const uint32_t c = th.slot(l.enabled ? 17 : 21);
    g.text(cx, wg::kLabelY, kFont3, c, l.top, IDisplay::kOptCenter);
    doubleTriangle(g, cx - 68, wg::kLabelY + 1, 1, c);    // ◀◀ x 532..547 (tips at 532 / 542)
    doubleTriangle(g, cx + 67, wg::kLabelY + 1, -1, c);   // ▶▶ x 652..667
    if (l.underline)
        wg::hairH(g, cx - 90, cx + 90, wg::kUnderlineY,
                  th.slot(l.underline == wg::kUnderlineGreen ? slot::kGreen : slot::kSolidRect));
}

bool trackUsed(const Track& tr) {
    for (int s = 0; s < tr.length(); ++s)
        if (tr.steps[s].note >= 0) return true;
    return false;
}

// Track srcT of pattern src -> track dstT of pattern dst (whole record, as
// stock). Ext tracks (12..15) go through the pool; an empty source copied
// onto an ext track that has no block leaves it without one.
void copyStoredTrack(App& a, int src, int srcT, int dst, int dstT) {
    Track tmp;
    a.readStoredTrack(src, srcT, tmp);
    if (dstT >= kStockTrackCount && !a.findStoredTrack(dst, dstT) &&
        extTrackIsDefault(tmp, a.storedStock(dst).tracks[0].lastStep))
        return;
    a.storedTrack(dst, dstT) = tmp;
}
}  // namespace

void SongScreen::followGridCursor(int edge) {
    if (edge >= firstTrack + kGridCols) firstTrack = kTrackCount - kGridCols;
    if (edge < firstTrack) firstTrack = 0;
}

int SongScreen::length() const {
    Project& p = app_.project;
    int n = 0;
    while (n < kSongSlots && p.playlist[n]) ++n;
    if (n == 0) {
        p.playlist[0] = 1;   // empty song -> one slot holding pattern 1
        n = 1;
    }
    return n;
}

void SongScreen::start(int opt) {
    (void)opt;
    cursor = clampi(cursor, 0, length() - 1);
    followPos_ = -1;   // re-join the playing slot on entry
    if (!edit && focus == 0) focus = 1;
    refreshBar();
}

int SongScreen::transportState() const {
    // spec 06 §2.2 F1/F2 label table (SO+0xb2).
    const Sequencer& s = app_.seq;
    if (!s.isPlaying()) return ui_.shift() && focus == 0 && selTracks == 1 ? 1 : 0;
    if (s.isSongMode()) return s.performanceMode() ? 4 : 2;
    return loopInSong_ ? 3 : 5;
}

bool SongScreen::loopingInSong() const {
    return loopInSong_ && app_.seq.isPlaying() && app_.playStartMs() == loopRun_;
}

void SongScreen::applyLoopToggle() {
    // spec 06 §2.3 F1 deferred action (0x5ae00), run at the sync point: the
    // song keeps playing; only the next pattern end changes (loop / advance).
    togglePending_ = false;
    Sequencer& s = app_.seq;
    if (!s.isPlaying()) {
        loopInSong_ = false;
    } else if (s.isSongMode()) {
        s.setSongMode(false);   // "Loop Pattern": the current slot repeats
        loopInSong_ = true;
        loopRun_ = app_.playStartMs();
    } else if (loopInSong_) {
        s.setSongMode(true);    // "Continue Song": advance at this pattern's end
        loopInSong_ = false;
    }
    refreshBar();
    ui_.dirty();
}

void SongScreen::reloadPlayingSlot() {
    // spec 06 §2.3 F6/F7: "if the slot is playing, the pattern is reloaded".
    App& a = app_;
    Sequencer& s = a.seq;
    if (!s.isPlaying() || !(s.isSongMode() || loopInSong_) || s.songPosition() != cursor) return;
    const int num = a.project.playlist[cursor];
    if (num == a.patternNumber()) return;
    a.commitCurrentPattern();
    a.loadPattern(uint8_t(num), a.pattern(), true);
    a.project.setCurrentPattern(uint8_t(num));
    s.attach(&a.engine, &a, &a.project, s.currentPattern(), s.preloadPattern());
}

void SongScreen::refreshBar() {
    BottomBar& b = ui_.bar;
    b.clear();
    b.separators = 0xBF;   // spec 20 §1.2: Song drops the separator at x 600
    static const char* kF1[6] = {"Play", "Preview", "Loop", "Continue", "Loop", "Stop"};
    static const char* kF1b[6] = {"Pattern", "Track", "Pattern", "Song", "Perf.", ""};
    const int st = transportState();
    b.set(0, kF1[st], kF1b[st]);
    b.set(1, st >= 2 ? "Stop" : "Play", st >= 2 ? "" : "Song");
    b.set(2, "Undo", nullptr, edit);
    b.set(3, "Add", ui_.shift() ? "Empty Slot" : "Slot", edit);
    b.setName(4, "Delete", "Slot", edit);
    b.setWide(5, "Pattern", nullptr, edit);   // "◀◀ Pattern ▶▶" (icons 3 / 4)
    // Edit-only keys: red underline while disabled, green when available.
    for (int i = 2; i <= 5; ++i) b.underline(i, edit ? wg::kUnderlineGreen : wg::kUnderlineRed);
    char t[24];
    if (app_.cfg.clockIn) {
        if (app_.seq.externalClockLost()) std::snprintf(t, sizeof(t), "NO CLK");
        else std::snprintf(t, sizeof(t), "%.0f BPM", double(app_.seq.externalTempo()));
    } else {
        std::snprintf(t, sizeof(t), "%.1f BPM", double(app_.project.tempo()));
    }
    char top[24];
    if (tapShown && tapCount >= 2) std::snprintf(top, sizeof(top), "TAP %d/4", tapCount);
    else std::snprintf(top, sizeof(top), "%s", app_.cfg.clockIn ? "Ext Tempo" : "Tempo");
    b.set(7, top, t);
    b.highlight(7, focus == 2);
    // Labels 0..5 keep their text in the bar model but are drawn by
    // SongScreen::draw (stock both-grey two-line set, icon label).
    for (int i = 0; i <= 5; ++i) b.label[i].hidden = true;
}

void SongScreen::setPattern(int slot, int num) {
    app_.project.playlist[slot] = uint8_t(clampi(num, 1, kPatternCount));
    app_.markProjectDirty();
    focus = 1;
}

void SongScreen::tempoDelta(float d, bool integer) {
    if (app_.cfg.clockIn) return;   // disabled with an external clock (spec 06 §2.3)
    float t = app_.project.tempo();
    if (integer) t = float(int(t)) + d;
    else t += d;
    if (t < kTempoMin) t = kTempoMin;
    if (t > kTempoMax) t = kTempoMax;
    app_.project.setTempo(t);
    app_.markProjectDirty();
}

void SongScreen::addSlot(bool empty) {
    const int n = length();
    if (n >= kSongSlots) return;
    Project& p = app_.project;
    const uint8_t num = empty ? uint8_t(app_.firstEmptyPattern()) : p.playlist[cursor];
    for (int i = n; i > cursor + 1; --i) p.playlist[i] = p.playlist[i - 1];
    p.playlist[cursor + 1] = num;
    ++cursor;
    app_.markProjectDirty();
}

void SongScreen::deleteSlot() {
    const int n = length();
    if (n <= 1) return;
    Project& p = app_.project;
    for (int i = cursor; i < n - 1; ++i) p.playlist[i] = p.playlist[i + 1];
    p.playlist[n - 1] = 0;
    if (cursor >= n - 1) cursor = n - 2;
    app_.markProjectDirty();
}

void SongScreen::copyJob(bool paste) {
    App& a = app_;
    if (!paste) {
        if (focus == 0) {
            clipRow0_ = selRow0;
            clipRows_ = selRows;
            clipTrack0_ = selTrack0;
            clipTracks_ = selTracks;
        } else {
            clipRow0_ = cursor;
            clipRows_ = 1;
            clipTrack0_ = 0;
            clipTracks_ = kTrackCount;
        }
        clipValid_ = true;
        ui_.notify("Copied");
        return;
    }
    if (!clipValid_) return;
    // Destination = the top-left of the paste: the grid selection when the
    // grid has focus (1.9.2 manual, Song page paste; spec 06 §2.3 names only
    // "the cursor", which in our model is the slot list's cursor), else the
    // list cursor with the source tracks. A paste onto its own source (same
    // pattern, same tracks) is a no-op.
    const int n = length();
    const int row0 = focus == 0 ? selRow0 : cursor;
    const int dstT0 = focus == 0 ? selTrack0 : clipTrack0_;
    a.commitCurrentPattern();
    for (int i = 0; i < clipRows_; ++i) {
        const int dstSlot = row0 + i;
        if (dstSlot >= n || clipRow0_ + i >= n) break;
        const int src = a.project.playlist[clipRow0_ + i], dst = a.project.playlist[dstSlot];
        if (src == dst && dstT0 == clipTrack0_) continue;
        a.pushFileUndo(dst);
        const int cnt = clipTracks_ < kTrackCount - dstT0 ? clipTracks_ : kTrackCount - dstT0;
        // Same pattern, overlapping tracks: copy away from the overlap.
        const bool back = src == dst && dstT0 > clipTrack0_;
        for (int k = 0; k < cnt; ++k) {
            const int j = back ? cnt - 1 - k : k;
            if (clipTrack0_ + j >= kTrackCount) continue;
            copyStoredTrack(a, src, clipTrack0_ + j, dst, dstT0 + j);
        }
        a.markPatternDirty(dst);
        ui_.progress("Copying", (i + 1) * 100 / clipRows_);
    }
    ui_.progressDone();
    a.reloadCurrentFromStore();
}

void SongScreen::deleteJob() {
    App& a = app_;
    a.commitCurrentPattern();
    for (int i = 0; i < selRows; ++i) {
        const int slot = selRow0 + i;
        if (slot >= length()) break;
        const int num = a.project.playlist[slot];
        a.pushFileUndo(num);
        for (int t = selTrack0; t < selTrack0 + selTracks && t < kTrackCount; ++t) {
            // An ext track without a pool block is empty already.
            if (!a.findStoredTrack(num, t)) continue;
            for (auto& s : a.storedTrack(num, t).steps) s = Step{};
        }
        a.markPatternDirty(num);
        // spec 06 §2.3 Delete: background "Deleting" job with a percentage
        // popup, like "Copying" (stock title 0xa9d28).
        ui_.progress("Deleting", (i + 1) * 100 / selRows);
    }
    ui_.progressDone();
    a.reloadCurrentFromStore();
}

void SongScreen::onButton(uint8_t id, uint8_t state) {
    App& a = app_;
    Sequencer& s = a.seq;
    if (id == btn::Song) {
        // spec 06 §2.5.
        if (state == kPress) {
            savedFocus_ = focus;
            focus = 2;
            songKeyHeld_ = true;
            playWhileSongKey_ = false;
        } else if (state == kRelease) {
            focus = savedFocus_;
            songKeyHeld_ = false;
            if (playWhileSongKey_) {
                playWhileSongKey_ = false;
                ui_.switchTo(ui_.previous());   // interface event 5
                return;
            }
        }
        refreshBar();
        return;
    }
    if (id == btn::Shift) {
        refreshBar();
        return;
    }
    const bool press = state == kPress;
    const bool pressOrHold = state != kRelease;
    switch (id) {
        case btn::Play:
            if (!press) break;
            if (songKeyHeld_) playWhileSongKey_ = true;
            onButton(ui_.shift() ? btn::F1 : btn::F2, kPress);
            return;
        case btn::F1:
            if (!press) break;
            if (!s.isPlaying()) {
                loopInSong_ = false;
                togglePending_ = false;
                if (ui_.shift() && focus == 0 && selTracks == 1) {
                    a.selectPattern(a.project.playlist[selRow0]);
                    // Preview Track plays the clip once (manual p.232, MANUAL_AUDIT #11).
                    s.playSelection(0, a.pattern().tracks[selTrack0].lastStep, selTrack0, selTrack0, false);
                } else {
                    a.selectPattern(a.project.playlist[cursor]);
                    s.play(0);
                }
            } else if (s.isSongMode() || loopInSong_) {
                // Deferred to the next sync point (master step change).
                if (!togglePending_) {
                    togglePending_ = true;
                    toggleStep_ = s.masterStep();
                }
            } else {
                s.stop();
                a.allVoicesOff();
            }
            break;
        case btn::F2:
            if (!press) break;
            loopInSong_ = false;
            togglePending_ = false;
            if (!s.isPlaying()) s.playSong(cursor);
            else {
                s.stop();
                a.allVoicesOff();
            }
            break;
        case btn::F3:
            if (!press || !edit) break;
            {
                const int top = a.fileUndo.topNumber();
                uint8_t num = 0;
                if (top < 1 || !a.popFileUndo(&num)) {
                    ui_.notify("No undo data :(");
                    break;
                }
                s.stop();
                a.markPatternDirty(num);
                if (num == a.patternNumber()) a.reloadCurrentFromStore();
            }
            break;
        case btn::F4:
            if (press && edit) addSlot(ui_.shift());
            break;
        case btn::F5:
            if (press && edit) deleteSlot();
            break;
        case btn::F6:
        case btn::F7:
            if (pressOrHold && edit) {
                const int d = (id == btn::F6 ? -1 : 1) * (ui_.shift() ? 10 : 1);
                setPattern(cursor, a.project.playlist[cursor] + d);
                reloadPlayingSlot();
            }
            break;
        case btn::F8:
            if (!press) break;
            if (focus != 2) {
                focus = 2;
            } else {
                // Tap tempo (spec 06 §2.4).
                const uint32_t now = ui_.now();
                tapShown = true;
                if (tapCount == 0) {
                    tapCount = 1;
                    tapT0 = now;
                } else {
                    ++tapCount;
                    if (now - tapT0 >= uint32_t(tapCount) * 1000 + 1000) {
                        tapCount = 0;
                    } else if (tapCount > 3) {
                        const float bpm = 180000.0f / float(now - tapT0);
                        if (bpm >= kTempoMin && bpm <= kTempoMax && !a.cfg.clockIn) {
                            a.project.setTempo(bpm);
                            a.markProjectDirty();
                        }
                        tapT0 = now;
                        tapCount = 1;
                    }
                }
            }
            break;
        case btn::Rec:
            if (!press) break;
            edit = !edit;
            if (!edit && focus == 0) focus = 1;
            break;
        case btn::Copy:
            if (press && edit) copyJob(ui_.shift());
            break;
        case btn::Delete:
            if (!press) break;
            if (focus == 0 && edit) deleteJob();
            else if (focus == 1 && edit) deleteSlot();
            else if (focus == 2 && !a.cfg.clockIn) {
                a.project.setTempo(130.0f);
                a.markProjectDirty();
            }
            break;
        case btn::Insert:
            if (!press || !ui_.shift()) break;
            if (focus == 0) selRow0 = 0;
            else cursor = clampi(cursor - 5, 0, length() - 1);
            break;
        case btn::Up:
        case btn::Down: {
            if (!press) break;
            const int d = id == btn::Up ? -1 : 1;
            if (focus == 0) {
                if (ui_.shift()) selRows = clampi(selRows + d, 1, length() - selRow0);
                else selRow0 = clampi(selRow0 + d, 0, length() - 1);
            } else if (focus == 1) {
                cursor = clampi(cursor + d, 0, length() - 1);
            } else {
                tempoDelta(ui_.shift() ? -0.1f * float(d) : -1.0f * float(d), !ui_.shift());
            }
            break;
        }
        case btn::Left:
        case btn::Right: {
            if (!press) break;
            const int d = id == btn::Right ? 1 : -1;
            if (focus == 0) {
                const int nt = selTrack0 + d;
                if (ui_.shift()) {
                    selTracks = clampi(selTracks + d, 1, kTrackCount - selTrack0);
                    followGridCursor(selTrack0 + selTracks - 1);
                } else if (nt >= kTrackCount) {
                    focus = 1;
                } else {
                    selTrack0 = clampi(nt, 0, kTrackCount - 1);
                    followGridCursor(selTrack0);
                }
            } else {
                int nf = focus + d;
                if (nf < (edit ? 0 : 1)) nf = edit ? 0 : 1;
                if (nf > 2) nf = 2;
                if (nf == 0) {
                    // Enters on the rightmost shown column (stock: track 12).
                    selRow0 = cursor;
                    selTrack0 = firstTrack + kGridCols - 1;
                }
                focus = nf;
            }
            break;
        }
        default:
            Screen::onButton(id, state);
            break;
    }
    refreshBar();
}

#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
bool SongScreen::encoderOnList() const {
    return focus == 0 || (focus == 1 && !(edit && ui_.shift()));   // grid rows, song list
}
#endif

void SongScreen::onEncoder(int d) {
    App& a = app_;
    if (focus == 1) {
        if (edit && ui_.shift()) {
            setPattern(cursor, a.project.playlist[cursor] + d);
        } else if (edit || !(a.seq.isPlaying() && a.seq.isSongMode())) {
            cursor = clampi(cursor + clampi(d, -5, 5), 0, length() - 1);
        }
    } else if (focus == 2) {
        tempoDelta(ui_.shift() ? 0.1f * float(d) : float(d), !ui_.shift());
    } else {
        selRow0 = clampi(selRow0 + d, 0, length() - 1);
    }
    refreshBar();
}

void SongScreen::stop() {
    // The stock action lives on the sequencer and survives a screen switch;
    // run a still-pending one now rather than lose it.
    if (togglePending_) applyLoopToggle();
}

void SongScreen::update(uint32_t now) {
    App& a = app_;
    if (togglePending_ && (!a.seq.isPlaying() || a.seq.masterStep() != toggleStep_)) applyLoopToggle();
    if (!a.seq.isPlaying() && loopInSong_) {
        loopInSong_ = false;
        ui_.dirty();
    }
    if (now - lastUpdate_ < 100) return;   // spec 06 §2.6
    lastUpdate_ = now;
    // The cursor follows the playing slot when that slot CHANGES (and on play
    // start / screen entry / leaving edit mode); in between Up/Down browse
    // the list (USER_REPORTS_R2 §4; spec 06 §2.6 does not say whether stock
    // re-copies on every tick).
    if (a.seq.isPlaying() && (a.seq.isSongMode() || loopInSong_) && !edit) {
        const int pos = a.seq.songPosition();
        if (pos != followPos_) {
            followPos_ = pos;
            if (pos != cursor && pos < length()) {
                cursor = pos;
                ui_.dirty();
            }
        }
    } else {
        followPos_ = -1;
    }
    // spec 06 §2.4: the timeout clears only the display flag; the count is
    // kept, so the next tap abandons the series (count := 0).
    if (tapShown && tapCount && now - tapT0 >= uint32_t(tapCount) * 1000 + 1000) {
        tapShown = false;
        ui_.dirty();
    }
    refreshBar();
}

void SongScreen::draw(Gfx& g) {
    App& a = app_;
    const Theme& th = ui_.th();
    ui_.drawTitle(g, "Song", edit ? "EDIT" : nullptr);
    const int n = length();
    gListTop = wg::listFollow(cursor, n, kRows, gListTop);
    const int first = gListTop;
    const int playing =
        a.seq.isPlaying() && (a.seq.isSongMode() || loopInSong_) ? a.seq.songPosition() : -1;

    // Song control (class 0x12) at (1, 29, 697, 394) with the fade.
    wg::panel(g, th, 1, wg::kPanelY, 697, wg::kPanelH, th.slot(slot::kSongList + 3));

    // Song list set (slot kSongList, spec 20 §1.4 +0x31c): [0] text / white
    // cells, [2] scroll thumb, [3] bg, [5] edit selection, [6] scroll track,
    // [8] empty tile 121212, [9] selection 30cb3f.
    // Rotated column headers (style bit7, font 1).
    const uint32_t white = th.slot(slot::kSongList);
    const int ft = firstTrack;   // first shown track: 0 or 4 (12-column window)
    for (int c = 0; c < kGridCols; ++c) {
        // Word and number as two runs: the stock vertical text advances
        // only 5 px for the space (font 1 is 8) [S snap 010].
        const int t = ft + c;
        const int x = kHeaderX - 4 + kPitchX * c;
        // A renamed track shows its name (stock photo; user report: the
        // Song page kept "Track X" after a rename).
        char nm[24], def[16];
        std::snprintf(def, sizeof(def), t < 8 ? "Track %d" : "Midi %d", t + 1);
        if (a.project.trackName(t, nm, sizeof(nm)) && nm[0] && std::strcmp(nm, def) != 0) {
            g.textRotated(x, kHeaderBottom - g.textWidth(kFont1, nm), kFont1, white, nm, 1);
            continue;
        }
        const char* word = t < 8 ? "Track" : "Midi";
        char num[12];
        std::snprintf(num, sizeof(num), "%d", t + 1);
        const int ww = g.textWidth(kFont1, word);
        g.textRotated(x, kHeaderBottom - ww, kFont1, white, word, 1);
        g.textRotated(x, kHeaderBottom - ww - kHeaderSpace - g.textWidth(kFont1, num), kFont1, white, num, 1);
    }

    // Slot grid: dark tile (121212, soft edge) per empty track, white
    // rounded cell where the track of that slot's pattern has notes.
    bool used[kRows][kGridCols] = {};
    const int rows = n - first < kRows ? n - first : kRows;
    for (int r = 0; r < rows; ++r) {
        const int num = a.project.playlist[first + r];
        const bool cur = num == a.patternNumber();
        if (!cur && !a.patternPresent(num)) continue;
        for (int c = 0; c < kGridCols; ++c) {
            const Track* tr = cur ? &a.pattern().tracks[ft + c] : a.findStoredTrack(num, ft + c);
            used[r][c] = tr && trackUsed(*tr);   // ext track without a block: empty
        }
    }
    g.lineWidth(32);   // RECTS corners r 2: boxes 1 px beyond the vertices
    g.rectsBegin(th.slot(slot::kSongList + 8));
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < kGridCols; ++c)
            if (!used[r][c]) g.rectsAdd(kGridX + 1 + kPitchX * c, kGridY + 1 + kPitchY * r, 37, 24);
    g.rectsEnd();
    // Used cells: white, or dim on a muted track (manual p.225 "Dimmed
    // Tracks", MANUAL_AUDIT_2026-10-23 #10).
    for (int pass = 0; pass < 2; ++pass) {
        bool any = false;
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < kGridCols; ++c)
                if (used[r][c] && a.trackMuted(ft + c) == (pass == 1)) {
                    if (!any) g.rectsBegin(pass ? th.slot(slot::kSongList + 2) : white);
                    any = true;
                    g.rectsAdd(kGridX + 41 - kPitchX + kPitchX * c, kGridY + 2 + kPitchY * r, 36, 22);
                }
        if (any) g.rectsEnd();
    }
    g.lineWidth(16);
    // Playback progress (stock 1.9.2 photo): a 2-px green line across the
    // grid, moving from the top to the bottom of the playing slot's row as
    // its pattern plays.
    if (playing >= first && playing < first + rows) {
        const int len = a.pattern().tracks[0].lastStep + 1;
        const int st = clampi(a.seq.step(0), 0, len > 0 ? len - 1 : 0);
        const int y = kGridY + kPitchY * (playing - first) + (len > 0 ? (kPitchY - 2) * st / len : 0);
        g.fill(kGridX, y, kPitchX * kGridCols - 1, 2, th.slot(slot::kSongList + 9));   // 30cb3f
    }
    if (focus == 0) {
        // Grid selection (edit mode): white 1.5-px outline on the gaps,
        // clipped to the shown columns.
        const int r0 = selRow0 - first, r1 = selRow0 + selRows - first;
        const int c0 = clampi(selTrack0 - ft, 0, kGridCols), c1 = clampi(selTrack0 + selTracks - ft, 0, kGridCols);
        if (r1 > 0 && r0 < kRows && c0 < c1) {
            const int y0 = kGridY - 1 + kPitchY * clampi(r0, 0, kRows);
            const int y1 = kGridY - 1 + kPitchY * clampi(r1, 0, kRows);
            wg::stroke(g, kGridX - 1 + kPitchX * c0, y0, kGridX - 1 + kPitchX * c1, y1, white);
        }
    }

    // Pattern list: caption, numbers (names after them), selection outline.
    g.text(600, 59, kFont1, white, "Pattern", IDisplay::kOptCenter);
    for (int r = 0; r < rows; ++r) {
        const int slotI = first + r;
        const int num = a.project.playlist[slotI];
        const int cy = kListY0 + 14 + kPitchY * r;
        const uint32_t c = white;   // stock photo: the playing slot keeps white text, marked by the icon
        if (slotI == playing)       // ▶ while the song plays, the loop arrow while Shift+Play loops it
            g.icon(kListNumX - 26, cy - 6, loopInSong_ ? kIconLoop : kIconPlay, white);
        g.textf(kListNumX, cy, kFont1, c, IDisplay::kOptCenterY, "%d", num);
        const char* nm = a.names.get(num);
        if (nm[0]) g.text(kListNameX, cy, kFont1, c, nm, IDisplay::kOptCenterY);
    }
    if (cursor >= first && cursor < first + kRows) {
        const int y0 = kListY0 + kPitchY * (cursor - first);
        wg::stroke(g, kListX0, y0, kListX1, y0 + kPitchY, th.slot(slot::kSongList + (edit ? 5 : 9)));
    }
    if (n > kRows) {
        // Scrollbar: black track, 575757 thumb with soft sides.
        g.fill(kScrollX, kScrollY, 4, kScrollH, th.slot(slot::kSongList + 6));
        const int len = kScrollH * kRows / n;
        const int pos = (kScrollH - len) * first / (n - kRows);
        const int ty = kScrollY + 2 + pos, tl = len - 3;
        g.fill(kScrollX, ty, 4, tl, 0x4f4f4f);
        g.fill(kScrollX + 1, ty, 2, tl, th.slot(slot::kSongList + 2));
    }

    // Tempo value bar (701, 29, 97, 394), white fill = tempo / 800.
    const float tempo = a.cfg.clockIn ? a.seq.externalTempo() : a.project.tempo();
    wg::ValueBarStyle vs;
    vs.fill = white;
    wg::valueBar(g, th, 701, wg::kPanelY, 97, wg::kPanelH, tempo / kTempoMax, vs);
    if (focus == 2) wg::panelFocus(g, th, 701, 97);

    // Bottom labels 0..5 (hidden in the bar model, see refreshBar).
    if (!ui_.askOpen() && !ui_.kb.isOpen()) {
        const BottomBar& b = ui_.bar;
        for (int i = 0; i < 5; ++i) {
            songLabelText(g, th, wg::labelCentre(i), b.label[i]);
            wg::barUnderline(g, th, wg::labelCentre(i), b.label[i].underline);
        }
        songWideLabel(g, th, 5, b.label[5]);
    }
}

}  // namespace ot
