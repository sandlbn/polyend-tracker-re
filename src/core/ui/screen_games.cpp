// Games module (spec 14 §1-§4): browser, launch, suspend/resume, exit, and the
// in-game key mapping, display and audio routing.
#include "ui/screen_games.h"

#if OT_GAMES

#include <cstdio>

#include "app/app.h"
#include "games/session.h"

namespace ot {

namespace {
constexpr int kListX = 250, kListY = 29, kListW = 297, kListH = 394;   // spec 14 §1.5 layout
constexpr uint32_t kHintGrey = 0x444444;                               // spec 14 §3.2 item 2

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// spec 14 §2.1: tracker control -> NES pad 1 bit.
uint8_t padBit(uint8_t id) {
    switch (id) {
        case btn::Up: return games::Nes::kUp;
        case btn::Down: return games::Nes::kDown;
        case btn::Left: return games::Nes::kLeft;
        case btn::Right: return games::Nes::kRight;
        case btn::F2: return games::Nes::kA;
        case btn::F1: return games::Nes::kB;
        case btn::Play: return games::Nes::kStart;
        case btn::Rec: return games::Nes::kSelect;
        default: return 0;
    }
}
}  // namespace

void openGames(App& app) { app.ui.switchTo(kModGames, 0); }

uint32_t GamesScreen::micros() const {
    const IClock* c = app_.hal().clock;
    return c ? c->micros() : app_.millis() * 1000u;
}

// ---------------------------------------------------------------------------
// Entry, browser

void GamesScreen::start(int opt) {
    (void)opt;
    ready_ = false;
    if (game_ == Game::Suspended) {
        // spec 14 §1.7: re-entering resumes the suspended game directly. It is
        // only resumed when its memory (the sample pool's free tail) is intact.
        const bool same = !lent_ && app_.bank.usedBytes() == tailUsed_ && games::session().fingerprint() == print_;
        if (same) {
            game_ = Game::Running;
            enterGame();
            return;
        }
        endGame();
        ui_.notify("Game ended: its memory was reused");
    }
    showBrowser();
}

void GamesScreen::showBrowser() {
    // spec 14 §1.3/§1.4: list Games/ (synchronous here; the folder is small).
    const int n = roms_.scan(app_.fs());
    ready_ = n >= 0;
    if (cursor >= roms_.count()) cursor = 0;   // §1.2: reset only when >= count
    refreshBar();
    ui_.dirty();
}

void GamesScreen::refreshBar() {
    // spec 14 §1.5 bottom labels.
    ui_.bar.clear();
    ui_.bar.set(0, "B");
    ui_.bar.set(1, "A");
    ui_.bar.setName(6, "Start", "Game", ready_ && roms_.count() > 0);
    ui_.bar.set(7, "Back");
    // Stock strip 050505 (VISUAL_DIFF C5); "Start / Game" is the two-line
    // grey set, drawn by draw() [S snap 028].
    ui_.bar.stripBg = ui_.th().panelFade() ? ui_.th().darkStrip() : wg::kThemeColour;
    ui_.bar.label[6].hidden = true;
}

void GamesScreen::stop() {
    if (!inGame_) return;
    // spec 14 §1.7: leaving by a screen key suspends the game.
    leaveGame();
    if (lent_) {
        // OpenTracker: the sample memory the game borrowed is needed back by
        // the music screens, so the game ends and the samples are reloaded.
        endGame();
        ui_.notify("Game ended: samples reloaded");
        return;
    }
    game_ = Game::Suspended;
    print_ = games::session().fingerprint();
}

// ---------------------------------------------------------------------------
// Launch / enter / leave

void GamesScreen::stopTransport() {
    // spec 14 §1.6 step 2: a playing sequencer is stopped (full stop).
    if (app_.seq.isPlaying() || app_.seq.isArmed()) app_.togglePlay(false);
    app_.allVoicesOff();
    app_.previewFile(nullptr, 0, false);
}

void GamesScreen::launch() {
    // spec 14 §1.6 preconditions.
    if (!ready_ || roms_.count() <= 0 || cursor < 0 || cursor >= roms_.count()) return;
    const char* file = roms_.fileName(cursor);
    if (!file[0]) return;
    char path[300];
    if (std::snprintf(path, sizeof(path), "%s/%s", games::kGamesDir, file) >= int(sizeof(path))) return;
    IStorage& fs = app_.fs();
    app_.finishJob();   // no project job may touch the sample pool meanwhile
    stopTransport();

    // Size first: it decides where the game lives (spec 14 §1.6: checked
    // before reading).
    const IStorage::Handle h = fs.open(path, IStorage::kRead);
    if (h < 0) {
        ui_.notify("Cannot open the ROM file");
        return;
    }
    const int32_t size = fs.size(h);
    fs.close(h);
    if (size <= 0 || uint32_t(size) > games::kMaxRomBytes) {
        ui_.notify(size <= 0 ? "Cannot read the ROM file" : "ROM file too large");
        return;
    }
    const uint32_t need = games::Session::arenaBytes(uint32_t(size));
    // Memory (OpenTracker; stock reserves SDRAM for the ROM, spec 14 §1.6):
    // the free tail of the sample pool when it is large enough, else the
    // whole pool is lent and the project's samples are reloaded afterwards.
    uint8_t* arena = nullptr;
    uint32_t cap = 0;
    lent_ = false;
    if (int16_t* tail = app_.bank.tail()) {
        const uintptr_t t = reinterpret_cast<uintptr_t>(tail);
        const uintptr_t a = (t + 15u) & ~uintptr_t(15u);
        const uint64_t freeBytes = uint64_t(app_.bank.freeSamples()) * 2u;
        if (freeBytes >= (a - t) + need) {
            arena = reinterpret_cast<uint8_t*>(a);
            cap = uint32_t(freeBytes - (a - t));
            tailUsed_ = app_.bank.usedBytes();
        }
    }
    if (!arena) {
        const AppMemory& m = app_.mem();
        const uintptr_t t = reinterpret_cast<uintptr_t>(m.sampleBank);
        const uintptr_t a = (t + 15u) & ~uintptr_t(15u);
        const uint64_t total = uint64_t(m.sampleBankSamples) * 2u;
        if (!m.sampleBank || total < (a - t) + need) {
            ui_.notify("Not enough memory for this game");
            return;
        }
        // Everything dirty goes to the Workspace first; the autosave is held
        // while the samples are gone (App::sampleBankLent).
        app_.autosave();
        app_.allVoicesOff();
        app_.sampleBankLent = true;
        lent_ = true;
        arena = reinterpret_cast<uint8_t*>(a);
        cap = uint32_t(total - (a - t));
    }
    const games::LoadStatus st = games::session().load(fs, path, arena, cap);
    if (st != games::LoadStatus::Ok) {
        // spec 14 §5.1 recommendation (stock shows nothing and enters a black
        // game screen): report the error and stay in the browser.
        if (lent_) returnBank();
        ui_.notify(games::session().errorText());
        return;
    }
    game_ = Game::Running;
    enterGame();
}

void GamesScreen::enterGame() {
    // spec 14 §1.6 steps 1-3 / §4 "enter game mode": transport stopped, the
    // emulator becomes the only audio source (music muted), the normal UI is
    // replaced by the game frame.
    stopTransport();
    if (IAudioOut* out = app_.hal().audio) {
        games::session().setAudioActive(true);
        // Same ctx as the engine's callback, so a half-swapped fn/ctx pair
        // never calls the engine with a foreign ctx (fillAudio ignores ctx).
        out->setCallback(&games::Session::fillAudio, &app_.engine);
    }
    inGame_ = true;
    keys_ = 0;
    games::session().setPad(0);
    games::session().invalidateDisplay();
    games::session().resync(micros());
    ui_.bar.clear();
    ui_.dirty();
}

void GamesScreen::leaveGame() {
    // spec 14 §4 "leave game mode": the exact inverse.
    if (IAudioOut* out = app_.hal().audio) app_.engine.attachOutput(*out);
    games::session().setAudioActive(false);
    inGame_ = false;
    keys_ = 0;
    games::session().setPad(0);
    ui_.dirty();
}

void GamesScreen::endGame() {
    games::session().unload();
    game_ = Game::None;
    if (lent_) returnBank();
}

void GamesScreen::returnBank() {
    // The lent sample memory was overwritten: reload the project's samples
    // with the normal background Workspace load (progress popup).
    lent_ = false;
    app_.sampleBankLent = false;
    if (!app_.startLoadWorkspace()) ui_.notify("Samples could not be reloaded");
}

// ---------------------------------------------------------------------------
// Main loop

void GamesScreen::update(uint32_t now) {
    (void)now;
    if (!inGame_) return;
    games::Session& s = games::session();
    if (s.jammed()) {
        // The ROM crashed the CPU (JAM opcode): back to the browser.
        leaveGame();
        endGame();
        ui_.notify("The game stopped (CPU halted)");
        showBrowser();
        return;
    }
    if (s.service(micros())) {
        if (IDisplay* d = app_.hal().display) s.upload(*d);
        ui_.dirty();
    }
}

// ---------------------------------------------------------------------------
// Input

void GamesScreen::onButton(uint8_t id, uint8_t state) {
    if (inGame_) {
        if (id == btn::F8) {
            // spec 14 §1.8: F8 (press) discards the game and shows the browser.
            if (state != kPress) return;
            leaveGame();
            endGame();
            showBrowser();
            return;
        }
        // spec 14 §2.1: press sets, release clears, hold is ignored.
        const uint8_t bit = padBit(id);
        if (!bit || state == kHold) return;
        if (state == kPress) keys_ |= bit;
        else keys_ &= uint8_t(~bit);
        games::session().setPad(keys_);
        return;
    }
    if (state != kPress) {
        if (id == btn::Play) Screen::onButton(id, state);
        return;
    }
    switch (id) {
        case btn::Up:
        case btn::F4:
            if (ready_ && roms_.count()) cursor = clampi(cursor - 1, 0, roms_.count() - 1);
            break;
        case btn::Down:
        case btn::F5:
            if (ready_ && roms_.count()) cursor = clampi(cursor + 1, 0, roms_.count() - 1);
            break;
        case btn::F7:
            launch();
            break;
        case btn::F8:
            ui_.switchTo(kModFile, 0);   // spec 14 §1.5: back to the File screen
            return;
        case btn::Play:
            Screen::onButton(id, state);   // transport as elsewhere (§1.5)
            break;
        default:
            break;   // F1, F2, F3, F6, Left, Right, Rec, Shift: no-ops
    }
    ui_.dirty();
}

void GamesScreen::onEncoder(int d) {
    // spec 14 §1.5: cursor += clamp(delta, -5, +5), clamped to the list.
    if (inGame_ || !ready_ || roms_.count() <= 0) return;
    cursor = clampi(cursor + clampi(d, -5, 5), 0, roms_.count() - 1);
    ui_.dirty();
}

// ---------------------------------------------------------------------------
// Drawing

void GamesScreen::draw(Gfx& g) {
    if (inGame_) {
        // spec 14 §3.2: black screen, the frame at x 144 magnified x2, and
        // dark grey key hints: "B" / "A" under the left border (F1 / F2) and
        // "..." on the right (F8 exits).
        g.fill(0, 0, kScreenW, kScreenH, 0x000000);
        g.text(80, 450, kFontSmall, kHintGrey, "B");
        g.text(110, 450, kFontSmall, kHintGrey, "A");
        g.text(729, 450, kFontSmall, kHintGrey, "...");
        g.color(0xFFFFFF);   // the bitmap is modulated by the current colour
        g.display().drawRgb332(games::kFrameX, games::kFrameY, IDisplay::kRamGGameFrame, games::Nes::kWidth,
                               games::Nes::kHeight, games::kFrameScale);
        return;
    }
    const Theme& th = ui_.th();
    ui_.drawTitle(g, "Games");
    // spec 20 §4.13 / spec 14 §1.5: panels x 0..247 and 550..799 around the
    // list (250, 29, 297, 394): rows from y 33, text x 263, white selection
    // outline; names without a number (spec 14 §8 item 7 resolved by the
    // shot) [S snap 028].
    wg::panel(g, th, 0, 30, 247, wg::kPanelH - 1);
    wg::panel(g, th, 550, 30, 249, wg::kPanelH - 1);
    const int n = roms_.count();
    wg::ListView v;
    v.x = kListX;
    v.y = kListY;
    v.w = kListW;
    v.h = kListH;
    v.count = ready_ ? n : 0;
    v.sel = cursor;
    v.selStyle = wg::ListSel::kWhite;
    if (cursor < top_) top_ = cursor;
    if (cursor >= top_ + kRows) top_ = cursor - kRows + 1;
    top_ = clampi(top_, 0, n > kRows ? n - kRows : 0);
    v.top = top_;
    v.ctx = this;
    v.item = [](void* c, int i, char* out, int cap) {
        char dn[56];
        static_cast<GamesScreen*>(c)->roms().displayName(i, dn, sizeof(dn));
        std::snprintf(out, size_t(cap), "%s", dn);
    };
    wg::list(g, th, v);
    const uint32_t textC = th.slot(slot::kList + 4), dimC = th.slot(slot::kBottomLabel);
    if (!ready_) {
        g.text(kListX + kListW / 2, kListY + kListH / 2, kFont1, dimC, "Cannot read Games/", IDisplay::kOptCenter);
    } else if (n == 0) {
        g.text(kListX + kListW / 2, kListY + kListH / 2 - 12, kFont1, dimC, "No games", IDisplay::kOptCenter);
        g.text(kListX + kListW / 2, kListY + kListH / 2 + 12, kFont1, dimC, "Put .nes files in Games/",
               IDisplay::kOptCenter);
    }
    // spec 14 §1.5 hint panel (OpenTracker wording): font 1 lines from
    // x 655, first cap y 69, pitch 22 [S snap 028].
    static const char* const kHints[] = {"In game:", "Play = Start", "Rec = Select"};
    for (int k = 0; k < int(sizeof(kHints) / sizeof(kHints[0])); ++k)
        g.text(655, 74 + k * 22, kFont1, textC, kHints[k], IDisplay::kOptCenterY);
    // F4 / F5 "▲" / "▼" (cursor keys, spec 14 §1.5) and F7 "Start / Game"
    // in the two-line grey style [S snap 028].
    const uint32_t lc = th.slot(slot::kBottomLabel);
    for (int k = 0; k < 2; ++k) {
        g.rectsBegin(lc);
        for (int r = 0; r < 6; ++r) {
            const int w = 4 + 2 * r;
            g.rectsAdd(350 + 100 * k - w / 2, k == 0 ? 451 + r : 456 - r, w, 1);
        }
        g.rectsEnd();
    }
    const BarLabel& st = ui_.bar.label[6];
    const uint32_t sc = th.slot(st.enabled ? slot::kBottomLabel : slot::kBottomLabelOff);
    g.text(wg::labelCentre(6), wg::kLabelY1, kFont3, sc, st.top, IDisplay::kOptCenter);
    g.text(wg::labelCentre(6), wg::kLabelY2, kFont3, sc, st.bottom, IDisplay::kOptCenter);
}

}  // namespace ot
#endif  // OT_GAMES
