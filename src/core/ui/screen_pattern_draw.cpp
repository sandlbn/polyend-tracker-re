// Pattern editor drawing (spec 20 §4.1; docs/VISUAL_DIFF_1.9.4.md Part 1
// §3.1, Part 2 §7.5, §8.1..§8.8): the vertical grid, the frame (strip,
// title, bar) and the column-key popups. Geometry and colours are the
// measured stock pixels; nothing is copied from stock graphics.
//
// Display-list economy: the empty-cell dashes (one 6 x 1 dash per character
// cell) are drawn as one tall rect per character column, then the rows
// between the dash lines are restored by a few full-width "mask" rects and
// non-empty cells erase their dash line; every glyph goes through glyphRun,
// grouped by colour.
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "ui/fonts/fonts.h"
#include "ui/fx_text.h"
#include "ui/notes.h"
#include "ui/screens.h"

namespace ot {

namespace {

// --- grid geometry (spec 20 §4.1, VISUAL_DIFF §2.3 / §7.5) ------------------
constexpr int kRows = 15;
constexpr int kCursorRow = 7;
constexpr int kGutterW = 27;          // left step-number gutter x 0..26, separator x 27
constexpr int kRightSep = 772;        // right gutter separator, numbers from x 776
constexpr int kTrackW4 = 186;         // 4-track view
constexpr int kTrackW8 = 93;          // 8-track view (column-view mask)
constexpr int kPitch = 11;            // grid cell pitch (font 2 at 11 px, spec 20 §2.3)
constexpr int kDashW = 6;             // empty-cell dash, cell + 1 .. cell + 6
constexpr int kStepX = 3;             // left step numbers
constexpr int kStepXR = 776;          // right step numbers
constexpr int kStepPitch = 7;         // step-number digit pitch (Part 1 §2.2: 7 px)
constexpr int kPopupX = 600;          // right-hand popups x 600..799
constexpr int kNoteKeyX = 13, kNoteKeyY = 130, kNoteKeyPitch = 65, kNoteKeySize = 61;

// Grid colour block (theme slot kGrid + i, spec 20 §4.1).
enum GridColour {
    kGcTabText = 0, kGcBg = 1, kGcNote = 2, kGcNoteDark = 6, kGcRec = 10, kGcBeat = 11, kGcMuted = 12,
    kGcTrough = 13, kGcWhite = 14, kGcSeparator = 15, kGcStep = 16, kGcCursor = 17, kGcSelect = 18,
    kGcTab = 19, kGcWrap = 20,
};
uint32_t gc(const Theme& th, int i) { return th.slot(slot::kGrid + i); }

// One shown cell column inside a track: text x offset, character cells, and
// the cursor box (x offset, width) (spec 20 §4.1 cell columns; Part 2 §7.5).
struct ColGeo {
    int text = 0, chars = 0, box = 0, boxW = 0;
    bool shown = false;
};
struct Layout {
    int tracks = 4;
    int trackW = kTrackW4;
    ColGeo col[4];
};

Layout layoutFor(uint8_t mask) {
    Layout L;
    static const ColGeo k4[4] = {{8, 3, 2, 41, true}, {52, 2, 46, 30, true}, {86, 4, 80, 50, true}, {140, 4, 134, 49, true}};
    if (!mask) {
        for (int c = 0; c < 4; ++c) L.col[c] = k4[c];
        return L;
    }
    L.tracks = 8;
    L.trackW = kTrackW8;
    int shown[2], n = 0;
    for (int c = 0; c < 4 && n < 2; ++c)
        if (mask & (1u << c)) shown[n++] = c;
    // 1-column view: text Note +32, Instr +37, Fx +27 (spec 20 §4.1 other
    // column modes). SPEC GAP: the 2-column tables 0xaabd4.. are only
    // summarised ("Note +7, Instr +61 or +10, Fx1 +5 or +50, Fx2 +50 or +5");
    // the first shown column takes the small offset, the second the large one.
    static const int k1[4] = {32, 37, 27, 27};
    static const int kFirst[4] = {7, 10, 5, 5};
    static const int kSecond[4] = {61, 61, 50, 50};
    for (int i = 0; i < n; ++i) {
        const int c = shown[i];
        ColGeo& g = L.col[c];
        g = k4[c];
        g.text = n == 1 ? k1[c] : (i == 0 ? kFirst[c] : kSecond[c]);
        g.box = g.text - 6;
    }
    return L;
}

// Cap-top offset of a font role inside its cell (custom fonts; the ROM
// fallback is close enough for host tests).
int capTop(uint8_t role) {
    const fontdata::FontInfo& f = fontdata::info(fontSet(), role - 1);
    return f.baseline - f.cap;
}

// Glyph text with its cap top at capY: glyphRun at `pitch` px (custom
// fonts, or the ROM font glyphs when they are not bound: OP-6); one CMD_TEXT
// in romTextMode() kRomWholeText (host tests read whole strings).
void run(Gfx& g, int x, int capY, uint8_t font, const char* s, uint32_t c, int pitch = 0) {
    if (!s || !*s) return;
    const int y = capY - capTop(font);
    if (customFontsBound() || romTextMode() != kRomWholeText) g.glyphRun(x, y, font, s, c, pitch);
    else g.text(x, y, font, c, s);
}

// Filled box with the stock 1.5-px soft edge: core, one column/row at
// alpha 227 and one at 28 on every side (cursor cells, Part 2 §7.5).
void softBox(Gfx& g, int x0, int y0, int x1, int y1, uint32_t c, bool edgesY, bool leftEdge = true) {
    g.fill(x0, y0, x1 - x0 + 1, y1 - y0 + 1, c);
    g.alpha(wg::kStrokeA1);
    g.rectsBegin(c);
    if (leftEdge) g.rectsAdd(x0 - 1, y0, 1, y1 - y0 + 1);
    g.rectsAdd(x1 + 1, y0, 1, y1 - y0 + 1);
    if (edgesY) {
        g.rectsAdd(x0, y0 - 1, x1 - x0 + 1, 1);
        g.rectsAdd(x0, y1 + 1, x1 - x0 + 1, 1);
    }
    g.rectsEnd();
    g.alpha(wg::kStrokeA2);
    g.rectsBegin(c);
    if (leftEdge) g.rectsAdd(x0 - 2, y0, 1, y1 - y0 + 1);
    g.rectsAdd(x1 + 2, y0, 1, y1 - y0 + 1);
    g.rectsEnd();
    g.alpha(255);
}

// 1.5-px outline on centre lines x0..x1 / y0..y1 like the stock grid
// cursor (Part 2 §7.5): the AA bands do not reach round the corners, so the
// separator next to a corner stays black.
void gridStroke(Gfx& g, int x0, int y0, int x1, int y1, uint32_t c) {
    const int w = x1 - x0 + 3, h = y1 - y0 + 3;
    g.alpha(wg::kStrokeA2);
    g.rectsBegin(c);
    g.rectsAdd(x0 - 1, y0 - 2, w, 1);
    g.rectsAdd(x0 - 1, y1 + 2, w, 1);
    g.rectsAdd(x0 - 2, y0 - 1, 1, h);
    g.rectsAdd(x1 + 2, y0 - 1, 1, h);
    g.rectsEnd();
    g.alpha(231);   // measured: B61515 / 2CB739 beside a CD1818 / 30CB3F core (Part 2 §8.1)
    g.rectsBegin(c);
    g.rectsAdd(x0 - 1, y0 - 1, w, 1);
    g.rectsAdd(x0 - 1, y0 + 1, w, 1);
    g.rectsAdd(x0 - 1, y1 - 1, w, 1);
    g.rectsAdd(x0 - 1, y1 + 1, w, 1);
    g.rectsAdd(x0 - 1, y0 + 2, 1, h - 6);
    g.rectsAdd(x0 + 1, y0 + 2, 1, h - 6);
    g.rectsAdd(x1 - 1, y0 + 2, 1, h - 6);
    g.rectsAdd(x1 + 1, y0 + 2, 1, h - 6);
    g.rectsEnd();
    g.alpha(255);
    g.rectsBegin(c);
    g.rectsAdd(x0 - 1, y0, w, 1);
    g.rectsAdd(x0 - 1, y1, w, 1);
    g.rectsAdd(x0, y0 + 1, 1, h - 4);
    g.rectsAdd(x1, y0 + 1, 1, h - 4);
    g.rectsEnd();
}

void trimLeft(char* s) {
    int i = 0;
    while (s[i] == ' ') ++i;
    if (i) std::memmove(s, s + i, std::strlen(s + i) + 1);
}

// Two left-pointing triangles 6 x 13 (the "◀◀" of the Fx popup footer,
// Part 2 §8.7), drawn as rows.
void doubleLeft(Gfx& g, int x, int y, uint32_t c) {
    g.rectsBegin(c);
    for (int k = 0; k < 2; ++k) {
        const int tx = x + 10 * k;
        for (int r = 0; r < 13; ++r) {
            const int d = r < 6 ? 6 - r : r - 6;
            const int w = 6 - d;
            if (w > 0) g.rectsAdd(tx + 6 - w, y + r, w, 1);
        }
    }
    g.rectsEnd();
}

}  // namespace

// ---------------------------------------------------------------------------
// Grid

void PatternScreen::drawGrid(Gfx& g) {
    if (app_.cfg.arrangement == 2) {
        drawGridHorizontal(g);
        return;
    }
    drawGridVertical(g, pop_.isOpen() && pop_.kind() != 0);
}

void PatternScreen::drawGridVertical(Gfx& g, bool threeTracks) {
    App& a = app_;
    const Theme& th = ui_.th();
    const int l = len();
    const bool wrap = a.cfg.arrangement == 0;
    const Layout L = layoutFor(colMask);
    // The Fx description panel (x 360..599) covers the third track: it is
    // not drawn under it (RAM_DL, docs/VERIFY_1.9.5.md R4).
    const bool descOpen = pop_.isOpen() && pop_.kind() == 3 && pop_.descriptionOpen();
    const int nT = threeTracks && !colMask ? (descOpen ? 2 : 3) : L.tracks;
    const int gy = gridTop();                    // control box (0, gy, 800, h)
    const int gh = topInfoOn() ? 395 : 423;
    const int pitch = rowHeight();               // 26 (28 without the top info)
    const int band0 = gy + 1;
    const int dashDy = pitch / 2 - 1;            // 12: the band's vertical centre
    const int capDy = (pitch - 12) / 2 - 1;      // 6: cap top of 12-px caps
    const int divider = a.project.u8(0x4F8);
    const uint32_t bg = gc(th, kGcBg), beatC = gc(th, kGcBeat);
    const bool playing = a.seq.isPlaying();
    const int top = step - kCursorRow;
    const int right = threeTracks ? kPopupX + 2 : kScreenW;   // bare grid bg up to x 600
    const int trackX0 = kGutterW;
    auto trackX = [&](int v) { return trackX0 + v * L.trackW; };
    const int dashX1 = trackX(nT) - 1;           // last pixel of the track area

    // Visible rows: step shown (or -1 = blank row) and beat flag.
    int rowStep[kRows];
    bool beat[kRows];
    for (int r = 0; r < kRows; ++r) {
        int s = top + r;
        if (s < 0 || s >= l) s = wrap && l > 0 ? ((s % l) + l) % l : -1;
        rowStep[r] = s;
        beat[r] = s >= 0 && divider > 0 && s % (divider + 1) == 0;
    }
    auto bandTop = [&](int r) { return band0 + pitch * r; };

    // 1. Background (rect [1] from x 1 to 798, soft 1-px side edges).
    g.fill(1, band0, right - 2, gh - 1, bg);
    g.alpha(128);
    g.rectsBegin(bg);
    g.rectsAdd(0, band0, 1, gh - 1);
    g.rectsAdd(right - 1, band0, 1, gh - 1);
    g.rectsEnd();
    g.alpha(255);
    // 2. Beat bands [11] (the dash row of a beat band keeps its colour).
    int nBeat = 0;
    for (int r = 0; r < kRows; ++r) nBeat += beat[r];
    if (nBeat) {
        g.rectsBegin(beatC);
        for (int r = 0; r < kRows; ++r)
            if (beat[r]) g.rectsAdd(1, bandTop(r), right - 2, pitch);
        g.rectsEnd();
    }

    // 3. Dash columns: one tall rect per character cell of every shown
    //    column, from the first to the last visible dash line, per colour.
    const int dashY0 = bandTop(0) + dashDy, dashY1 = bandTop(kRows - 1) + dashDy;
    for (int c = 0; c < 4; ++c) {
        if (!L.col[c].shown) continue;
        for (int pass = 0; pass < 2; ++pass) {   // 0 normal tracks, 1 muted
            bool any = false;
            for (int v = 0; v < nT && !any; ++v) {
                const int t = firstTrack + v;
                any = t < kTrackCount && a.trackMuted(t) == (pass == 1);
            }
            if (!any) continue;
            const uint32_t dc = wg::mix(bg, pass ? gc(th, kGcMuted) : gc(th, kGcNoteDark + c), 222);
            g.rectsBegin(dc);
            for (int v = 0; v < nT; ++v) {
                const int t = firstTrack + v;
                if (t >= kTrackCount || a.trackMuted(t) != (pass == 1)) continue;
                const int x = trackX(v) + L.col[c].text;
                for (int i = 0; i < L.col[c].chars; ++i)
                    g.rectsAdd(x + i * kPitch + 1, dashY0, kDashW, dashY1 - dashY0 + 1);
            }
            g.rectsEnd();
        }
    }
    // 4. Masks: restore the rows between two dash lines (split where the
    //    band colour changes), then the 1-px AA rows of the beat bands.
    for (int pass = 0; pass < 2; ++pass) {
        const uint32_t mc = pass ? beatC : bg;
        bool begun = false;
        for (int r = 0; r + 1 < kRows; ++r) {
            const int y0 = bandTop(r) + dashDy + 1, y1 = bandTop(r + 1) + dashDy - 1;
            const int split = bandTop(r + 1);
            auto add = [&](int ya, int yb) {
                if (yb < ya) return;
                if (!begun) {
                    g.rectsBegin(mc);
                    begun = true;
                }
                g.rectsAdd(trackX0 + 1, ya, dashX1 - trackX0, yb - ya + 1);
            };
            if (beat[r] == beat[r + 1]) {
                if (beat[r] == (pass == 1)) add(y0, y1);
            } else {
                if (beat[r] == (pass == 1)) add(y0, split - 1);
                if (beat[r + 1] == (pass == 1)) add(split, y1);
            }
        }
        if (begun) g.rectsEnd();
    }
    if (nBeat && divider > 0) {
        g.alpha(85);
        g.rectsBegin(beatC);
        for (int r = 0; r < kRows; ++r) {
            if (!beat[r]) continue;
            if (r > 0 && !beat[r - 1]) g.rectsAdd(1, bandTop(r) - 1, right - 2, 1);
            if (r + 1 < kRows && !beat[r + 1]) g.rectsAdd(1, bandTop(r) + pitch, right - 2, 1);
        }
        g.rectsEnd();
        g.alpha(255);
    }
    // 5. Erase the dash line of non-empty cells and blank rows. Adjacent
    //    non-empty columns of a row (across tracks too) are erased with one
    //    rect: on the dash row the pixels between two columns are the band
    //    colour already (dashes sit only under the characters, the
    //    separators are drawn later), so the result is the same and a
    //    dense page costs one rect per row instead of one per cell column
    //    (1.9.5: ~960 vertices on a dense page, over the RAM_DL limit).
    const Pattern& pat = a.pattern();
    auto colEmpty = [&](const Step& st, int c) {
        switch (c) {
            case 0: return st.note == kNoteEmpty;
            // OFF / CUT / FADE show no instrument (stock photos of
            // live-recorded OFF steps, USER_REPORTS_R3 R3-2).
            case 1:
                return (st.note == kNoteEmpty || st.note == kNoteOff || st.note == kNoteCut || st.note == kNoteFade) &&
                       !isMidiInstrument(st.instrument);
            default: return st.fx[c - 2].type == 0;
        }
    };
    for (int pass = 0; pass < 2; ++pass) {
        const uint32_t ec = pass ? beatC : bg;
        bool begun = false;
        for (int r = 0; r < kRows; ++r) {
            if (beat[r] != (pass == 1)) continue;
            const int y = bandTop(r) + dashDy;
            auto add = [&](int x, int w) {
                if (!begun) {
                    g.rectsBegin(ec);
                    begun = true;
                }
                g.rectsAdd(x, y, w, 1);
            };
            if (rowStep[r] < 0) {
                add(trackX0 + 1, dashX1 - trackX0);
                continue;
            }
            int runX0 = -1, runX1 = 0;   // current run of adjacent non-empty columns
            auto flush = [&] {
                if (runX0 >= 0) add(runX0, runX1 - runX0);
                runX0 = -1;
            };
            for (int v = 0; v < nT; ++v) {
                const int t = firstTrack + v;
                if (t >= kTrackCount) {
                    flush();
                    continue;
                }
                const Step& st = pat.tracks[t].steps[rowStep[r]];
                for (int c = 0; c < 4; ++c) {
                    if (!L.col[c].shown) continue;
                    if (colEmpty(st, c)) {
                        flush();
                        continue;
                    }
                    const int x = trackX(v) + L.col[c].text, w = L.col[c].chars * kPitch;
                    if (runX0 < 0) runX0 = x;
                    runX1 = x + w;
                }
            }
            flush();
        }
        if (begun) g.rectsEnd();
    }
    // Tracks past 12 (8-track view scrolled right) / outside the 3-track view.
    if (!threeTracks && firstTrack + nT > kTrackCount) {
        const int x = trackX(kTrackCount - firstTrack);
        g.fill(x, band0, dashX1 - x + 1, gh - 1, bg);
    }

    // 6. Cursor-row shading (spec 20 §1.5, Part 2 C4): the playhead row while
    //    playing, the step-1 (wrap) row otherwise; then the wrap line.
    int wrapRow = -1, troughRow = -1;
    for (int r = 0; r < kRows; ++r)
        if (rowStep[r] == 0 && wrapRow < 0) wrapRow = r;
    if (playing) {
        const int ps = a.seq.step(track);
        for (int r = 0; r < kRows; ++r)
            if (rowStep[r] == ps) troughRow = r;
    } else {
        troughRow = wrapRow;
    }
    // 7. Bottom fade of the grid control (style bit21).
    // h - 1: the fade ends on row 423 and leaves the strip's edge row 424.
    wg::bottomFade(g, th, 0, gy, right - 1, gh - 1, bg);
    if (troughRow >= 0) {
        const int T = bandTop(troughRow);
        const uint32_t tc = gc(th, kGcTrough);
        if (th.flag(slot::kFlagCursorRowGrad)) {
            g.gradientRect(1, T - 11, right - 2, 10, wg::mix(bg, tc, 23), wg::mix(bg, tc, 223));
            g.gradientRect(1, T + pitch, right - 2, 10, wg::mix(bg, tc, 223), wg::mix(bg, tc, 23));
        } else {
            g.fill(1, T - 11, right - 2, 10, tc);
            g.fill(1, T + pitch, right - 2, 10, tc);
        }
        g.fill(1, T + pitch - 1, right - 2, 1, tc);
    }
    // 8. Vertical separators [15] with their AA columns: a cached RAM_G
    //    fragment (2.x M0, Gfx::cached), static while the view does not change.
    g.cached(kFragPatSeparators, [&] {
        int xs[10], n = 0;
        for (int v = 0; v <= nT && v <= L.tracks; ++v) xs[n++] = (v == 4 && !colMask) || v == 8 ? kRightSep : trackX(v);
        if (threeTracks) n = colMask ? 3 : nT;   // the 4th track area stays bare [S snap 015]
        const uint32_t sc = gc(th, kGcSeparator);
        const int sy = band0, sh = gh - 1;
        g.alpha(120);
        g.rectsBegin(sc);
        for (int i = 0; i < n; ++i) {
            g.rectsAdd(xs[i] - 1, sy, 1, sh);
            g.rectsAdd(xs[i] + 1, sy, 1, sh);
        }
        g.rectsEnd();
        g.alpha(255);
        g.rectsBegin(sc);
        for (int i = 0; i < n; ++i) g.rectsAdd(xs[i], sy, 1, sh);
        g.rectsEnd();
    });

    // Wrap line [20] (spec 20 §4.1 step 8), over the separators.
    if (wrapRow > 0 && wrap) {
        const int y = bandTop(wrapRow) - 1;
        const uint32_t yc = gc(th, kGcWrap);
        g.alpha(14);
        g.rectsBegin(yc);
        g.rectsAdd(0, y - 1, right, 1);
        g.rectsAdd(0, y + 1, right, 1);
        g.rectsEnd();
        g.alpha(134);
        g.fill(0, y, right, 1, yc);
        g.alpha(255);
    }

    // 9. Selection (block) outline and cursor.
    const SelRect sel = target();
    const bool hasSel = selState == 2 || wholeCol || allTracks;
    const uint32_t curC = gc(th, recordCursor() ? kGcRec : kGcCursor);
    if (hasSel) {
        int r0 = -1, r1 = -1;
        for (int r = 0; r < kRows; ++r)
            if (rowStep[r] >= sel.s0 && rowStep[r] <= sel.s1 && (r == 0 || r0 < 0 || r1 == r - 1)) {
                if (r0 < 0) r0 = r;
                r1 = r;
            }
        const int v0 = sel.t0 - firstTrack < 0 ? 0 : sel.t0 - firstTrack;
        const int v1 = sel.t1 - firstTrack >= nT ? nT - 1 : sel.t1 - firstTrack;
        if (r0 >= 0 && v0 <= v1)
            wg::stroke(g, trackX(v0) + 2, bandTop(r0) - 1, trackX(v1) + L.trackW - 3, bandTop(r1) + pitch - 2,
                       gc(th, kGcSelect));
    }
    const int cv = track - firstTrack;
    const bool cursorShown = cv >= 0 && cv < nT && L.col[col].shown;
    const int cT = bandTop(kCursorRow);
    if (cursorShown) {
        const int x = trackX(cv);
        const ColGeo& cg = L.col[col];
        // A box starting on the frame line (Note column) has no AA of its own on the left.
        softBox(g, x + cg.box, cT - 1, x + cg.box + cg.boxW - 1, cT + pitch - 2, curC, false, cg.box > 2);
        gridStroke(g, x + 2, cT - 1, x + L.trackW - 3, cT + pitch - 2, curC);
        const int s = rowStep[kCursorRow];
        if (s >= 0 && colEmpty(pat.tracks[track].steps[s], col)) {
            g.rectsBegin(wg::mix(curC, gc(th, kGcWhite), 220));
            for (int i = 0; i < cg.chars; ++i) g.rectsAdd(x + cg.text + i * kPitch + 1, cT + dashDy, kDashW, 1);
            g.rectsEnd();
        }
    }

    // 10. Step numbers [16] in font 1, both gutters (left only in the
    //     reduced modes).
    {
        const uint32_t nc = gc(th, kGcStep);
        char lb[8];
        for (int side = 0; side < (threeTracks ? 1 : 2); ++side)
            for (int r = 0; r < kRows; ++r) {
                if (rowStep[r] < 0) continue;
                stepLabel(rowStep[r], lb);
                trimLeft(lb);
                run(g, side ? kStepXR : kStepX, bandTop(r) + capDy, kFont1, lb, nc, kStepPitch);
            }
    }

    // 11. Cell texts, one colour at a time: note, instr, fx1, fx2, then the
    //     muted tracks, then white (cursor cell / block selection). Track by
    //     track within a pass (cells never overlap, so the order inside a
    //     pass is free): the VERTEX_TRANSLATE_X for x >= 512 then changes
    //     once per pass instead of twice per row.
    auto luma = [](uint32_t c) { return 299 * ((c >> 16) & 255) + 587 * ((c >> 8) & 255) + 114 * (c & 255); };
    const bool selWhite = luma(gc(th, kGcWhite)) > luma(gc(th, kGcBg));
    for (int pass = 0; pass < 6; ++pass) {
        const int c = pass < 4 ? pass : -1;
        uint32_t colour = c >= 0 ? gc(th, kGcNote + c) : gc(th, pass == 4 ? kGcMuted : kGcWhite);
        for (int v = 0; v < nT; ++v) {
            const int t = firstTrack + v;
            if (t >= kTrackCount) continue;
            for (int r = 0; r < kRows; ++r) {
                const int s = rowStep[r];
                if (s < 0) continue;
                const Step& st = pat.tracks[t].steps[s];
                const bool muted = a.trackMuted(t);
                const bool inSel = hasSel && t >= sel.t0 && t <= sel.t1 && s >= sel.s0 && s <= sel.s1;
                for (int k = 0; k < 4; ++k) {
                    if (!L.col[k].shown || colEmpty(st, k)) continue;
                    // Cursor cell: white on its fill. Block selection (an
                    // outline only): white where the theme's [14] is lighter
                    // than the grid background (Original); on the dark-text
                    // themes the column colour, so the text stays readable [I].
                    const bool cursorCell = r == kCursorRow && t == track && k == col && cursorShown;
                    const bool white = cursorCell || (inSel && selWhite);
                    const int want = white ? 5 : (muted ? 4 : k);
                    if (want != pass) continue;
                    char txt[8];
                    int x = trackX(v) + L.col[k].text;
                    uint8_t font = kFont2;
                    int adv = kPitch;
                    if (k == 0) {
                        noteName(st.note, txt);
                    } else if (k == 1) {
                        if (isMidiInstrument(st.instrument)) {
                            // "M" + 2 digits in font 1, shifted 6 - width("M").
                            std::snprintf(txt, sizeof(txt), "M%02d", st.instrument - kFirstMidiInstrument + 1);
                            font = kFont1;
                            adv = 0;
                            x += 6 - textWidth(kFont1, "M");
                        } else if (st.instrument >= kInstrumentCount) {
                            std::snprintf(txt, sizeof(txt), "S%c", char('1' + (st.instrument - kInstrumentCount) % 9));
                        } else {
                            std::snprintf(txt, sizeof(txt), "%02d", st.instrument + 1);
                        }
                    } else {
                        fxCellText(st.fx[k - 2].type, st.fx[k - 2].value, txt);
                    }
                    run(g, x, bandTop(r) + capDy, font, txt, colour, adv);
                }
            }
        }
    }

    // 12. Track header tabs: grey [19] (muted [12]), black bold names; a
    //     cached RAM_G fragment (2.x M0), static while names / mutes / the
    //     view do not change.
    g.cached(kFragPatHeaders, [&] {
        int tx[8], tw[8], n = 0;
        char names[8][24];
        bool muted[8];
        for (int v = 0; v < nT && n < 8; ++v) {
            const int t = firstTrack + v;
            if (t >= kTrackCount) break;
            char nm[24];
            if (!a.project.trackName(t, nm, sizeof(nm)) || !nm[0])
                std::snprintf(nm, sizeof(nm), t < 8 ? "Track %d" : "Midi %d", t + 1);
            std::snprintf(names[n], sizeof(names[n]), "%s", nm);
            int w = textWidth(kFont4, names[n]);
            const int maxW = L.trackW - 8;
            if (w > maxW) w = maxW;
            // Narrow (8-track) view: the tab spans the track, or the top row's
            // note shows half-covered beside the name ("Hats5", R3-10).
            if (L.trackW < 150) w = maxW;
            tx[n] = trackX(v);
            tw[n] = w;
            muted[n] = a.trackMuted(t);
            ++n;
        }
        const int ty = band0;
        for (int pass = 0; pass < 2; ++pass) {
            bool any = false;
            for (int i = 0; i < n; ++i) any |= muted[i] == (pass == 1);
            if (!any) continue;
            const uint32_t c = gc(th, pass ? kGcMuted : kGcTab);
            g.rectsBegin(c);
            for (int i = 0; i < n; ++i)
                if (muted[i] == (pass == 1)) g.rectsAdd(tx[i] + 1, ty + 1, tw[i] + 3, 26);
            g.rectsEnd();
            g.alpha(225);
            g.rectsBegin(c);
            for (int i = 0; i < n; ++i) {
                if (muted[i] != (pass == 1)) continue;
                g.rectsAdd(tx[i], ty + 1, 1, 26);
                g.rectsAdd(tx[i] + tw[i] + 4, ty + 1, 1, 26);
                g.rectsAdd(tx[i] + 1, ty, tw[i] + 3, 1);
                g.rectsAdd(tx[i] + 1, ty + 27, tw[i] + 3, 1);
            }
            g.rectsEnd();
            g.alpha(28);
            g.rectsBegin(c);
            for (int i = 0; i < n; ++i) {
                if (muted[i] != (pass == 1)) continue;
                g.rectsAdd(tx[i] + tw[i] + 5, ty + 1, 1, 26);
                g.rectsAdd(tx[i] + 1, ty + 28, tw[i] + 3, 1);
            }
            g.rectsEnd();
            g.alpha(255);
        }
        g.clip(0, ty, right, 28);
        for (int i = 0; i < n; ++i) run(g, tx[i] + 4, ty + 6, kFont4, names[i], gc(th, kGcTabText));
        g.noClip();
    });
}

// ---------------------------------------------------------------------------
// Bottom bar

void PatternScreen::drawBar(Gfx& g) {
    const Theme& th = ui_.th();
    if (ui_.askOpen() || ui_.kb.isOpen()) return;   // Ui draws its own labels over the strip
    if (ui_.shift() && focus < 0 && !pop_.isOpen() && !fillOpen && !recArmPage) {
        drawMuteBar(g);
        return;
    }
    ui_.bar.drawLabels(g, th);
}

// Shift mute page (Part 2 §8.6): "Track n" in font 3 grey over "Mute" in
// the 8-px font 1, white; no underlines, no page dots.
void PatternScreen::drawMuteBar(Gfx& g) {
    const Theme& th = ui_.th();
    const BottomBar& b = ui_.bar;
    for (int i = 0; i < 8; ++i) {
        const BarLabel& l = b.label[i];
        if (l.hidden || !l.top[0]) continue;
        const int cx = wg::labelCentre(i);
        g.text(cx, wg::kLabelY1, kFont3, th.slot(slot::kBottomLabel), l.top, IDisplay::kOptCenter);
        if (l.bottom[0]) g.text(cx, wg::kLabelY2, kFont1, th.slot(slot::kBottomLabel + 3), l.bottom, IDisplay::kOptCenter);
    }
    uint8_t hl = 0;
    for (int i = 0; i < 8; ++i)
        if (b.label[i].highlight && !b.label[i].hidden) hl |= uint8_t(1u << i);
    wg::barFocusMask(g, th, hl);
}

// ---------------------------------------------------------------------------
// Popups

namespace {
struct PopupCtx {
    App* app;
    int kind;
};
void popupItem(void* ctx, int i, char* out, int cap) {
    const PopupCtx* p = static_cast<const PopupCtx*>(ctx);
    App& a = *p->app;
    if (p->kind == 3) {
        const FxInfo& f = fxInfo(kFxMenuOrder[i]);
        std::snprintf(out, size_t(cap), "%c - %s", f.symbol, f.name);
    } else if (p->kind == 5) {
        // MIDI tracks: Synth 1..3, then the MIDI channels.
        const int ins = synthui::popupInstrumentMidi(i);
        if (isSynthInstrument(uint8_t(ins)))
            synthui::slotLabel(a, synthSlotOf(uint8_t(ins)), out, cap);
        else
            std::snprintf(out, size_t(cap), "MIDI Channel %d", ins - kFirstMidiInstrument + 1);
    } else {
        // Instruments: i is the list position, Synth 1..3 first (Tracker+
        // manual p.100 figure: "Synth 1 (WTFM)" before "1. ...").
        const int ins = synthui::popupInstrument(i);
        if (ins < kSampleInstrumentCount)
            std::snprintf(out, size_t(cap), "%d. %s", ins + 1, a.instrPresent[ins] ? a.instruments[ins].name : "");
        else if (isSynthInstrument(uint8_t(ins)))
            synthui::slotLabel(a, synthSlotOf(uint8_t(ins)), out, cap);
        else
            std::snprintf(out, size_t(cap), "%d. MIDI Channel %d", ins + 1, ins - kFirstMidiInstrument + 1);
    }
}
}  // namespace

// Right-hand list popup (Instrument / MIDI channel / Fx; Part 2 §8.4, §8.7):
// panel x 600..799 over the title bar and the bar, white header band with
// a black bold caption, the list from y 30, Fx: "◀◀ Description" footer.
void PatternScreen::drawListPopup(Gfx& g) {
    App& a = app_;
    const Theme& th = ui_.th();
    const int kind = pop_.kind();
    const bool desc = kind == 3 && pop_.descriptionOpen();
    const int x = desc ? 360 : kPopupX;
    // Panel x 600..799 from y 30 down over the bar (W2b widget); the
    // header band is drawn by Ui (openPopupHeader in openPicker). With the
    // description panel open the popup widens to x 360 (spec 16 §B.4).
    wg::rightPopupPanel(g, th);
    if (desc) {
        const uint32_t white = th.slot(slot::kList), panelC = th.slot(slot::kEmptyPanel + 1);
        g.fill(x + 1, 0, kPopupX - x, 27, white);
        g.fill(x + 1, 27, kPopupX - x, 1, 0x868686);
        g.fill(x + 2, 28, kPopupX - x - 1, 1, 0x525252);
        g.fill(x + 2, 29, kPopupX - x, kScreenH - 29, panelC);
    }
    // List.
    PopupCtx ctx{&a, kind};
    wg::ListView v;
    v.x = kPopupX;
    v.y = 30;
    v.w = 200;
    v.h = kind == 3 ? 409 : 394;
    v.count = kind == 3 ? kFxCount : (kind == 5 ? synthui::kMidiPopupCount : kInstrumentCount + kSynthSlotCount);
    v.sel = pop_.cursor();
    popTop_ = wg::listFollow(v.sel, v.count, wg::listVisibleRows(v.h), popTop_);
    v.top = popTop_;
    v.background = false;
    v.selStyle = wg::ListSel::kWhite;
    v.item = popupItem;
    v.ctx = &ctx;
    if (kind == 3) g.clip(kPopupX, 30, 200, 401);
    wg::list(g, th, v);
    g.noClip();
    if (kind == 3) {
        const uint32_t grey = th.slot(slot::kBottomLabel);
        doubleLeft(g, kPopupX + 7, 452, grey);
        run(g, kPopupX + 40, 455, kFont1, "Description", grey);
    }
    if (desc) {
        char buf[48];
        popupItem(&ctx, pop_.cursor(), buf, sizeof(buf));
        g.text(x + 13, 34 + 13, kFont1, th.slot(slot::kList + 4), buf, IDisplay::kOptCenterY);
        wg::textBox(g, th, x + 8, 62, 232, 360, kFont5, fxHelpText(kFxMenuOrder[pop_.cursor() < kFxCount ? pop_.cursor() : 0]), 3);
    }
}

// Notes popup (Part 2 §8.5): full screen, white header "Notes", 12 x 4 keys
// with the octaves high to low from the top, red frame on the selection.
void PatternScreen::drawNotesPopup(Gfx& g) {
    const Theme& th = ui_.th();
    // Header band "Notes": Ui (PopupHeader::kFull, opened in openPicker).
    const uint32_t keyC = th.slot(slot::kKeyboardKeys);
    g.rectsBegin(wg::mix(0x000000, keyC, 128));
    for (int k = 0; k < 48; ++k)
        g.rectsAdd(kNoteKeyX + kNoteKeyPitch * (k % 12) - 1, kNoteKeyY + kNoteKeyPitch * (k / 12) - 1, kNoteKeySize + 2,
                   kNoteKeySize + 2);
    g.rectsEnd();
    g.rectsBegin(keyC);
    for (int k = 0; k < 48; ++k)
        g.rectsAdd(kNoteKeyX + kNoteKeyPitch * (k % 12), kNoteKeyY + kNoteKeyPitch * (k / 12), kNoteKeySize, kNoteKeySize);
    g.rectsEnd();
    const uint32_t tc = th.slot(slot::kKeyboardKeys + 1);
    for (int k = 0; k < 48; ++k) {
        const int n = noteWin_ + 12 * (3 - k / 12) + k % 12;
        if (n > 127) continue;
        char nm[4];
        noteName(int8_t(n), nm);
        const int kx = kNoteKeyX + kNoteKeyPitch * (k % 12), ky = kNoteKeyY + kNoteKeyPitch * (k / 12);
        run(g, kx + (kNoteKeySize - textWidth(kFont1, nm)) / 2, ky + 25, kFont1, nm, tc);
    }
    const int c = pop_.cursor() - noteWin_;
    if (c >= 0 && c < 48) {
        const int row = 3 - c / 12, colk = c % 12;
        const int kx = kNoteKeyX + kNoteKeyPitch * colk, ky = kNoteKeyY + kNoteKeyPitch * row;
        wg::stroke(g, kx - 1, ky - 1, kx + kNoteKeySize, ky + kNoteKeySize, th.slot(slot::kKeyboardKeys + 2));
    }
}

void PatternScreen::followNoteWindow() {
    const int n = pop_.cursor();
    if (n < noteWin_) noteWin_ = (n / 12) * 12;
    if (n >= noteWin_ + 48) noteWin_ = (n / 12 - 3) * 12;
    if (noteWin_ < 0) noteWin_ = 0;
    if (noteWin_ > 84) noteWin_ = 84;
}

bool PatternScreen::popupMove(uint8_t id) {
    int d = 0;
    const bool keys = pop_.kind() == 0;
    switch (id) {
        case btn::Up: d = keys ? 12 : -1; break;
        case btn::Down: d = keys ? -12 : 1; break;
        // Lists page with Left / Right (manual p.101 / 103, MANUAL_AUDIT #19).
        case btn::Left: d = keys ? -1 : -wg::listVisibleRows(pop_.kind() == 3 ? 409 : 394); break;
        case btn::Right: d = keys ? 1 : wg::listVisibleRows(pop_.kind() == 3 ? 409 : 394); break;
        default: return false;
    }
    pop_.move(d);
    if (keys) followNoteWindow();
    refreshBar();
    return true;
}

void PatternScreen::popupPad(uint8_t pad) {
    if (pad >= 48) return;
    if (pop_.kind() == 0) {
        const int n = noteWin_ + 12 * (3 - pad / 12) + pad % 12;
        if (n <= 127) pop_.select(n);
    } else if (pop_.kind() == 2) {
        // Pads keep the stock instrument map (Tracker+ manual p.100: pads
        // 1-48 = sample instruments, synths only with the encoder): page 0
        // = samples (also from a synth entry), page 1 = MIDI 1..16.
        const int ins = synthui::popupInstrument(pop_.cursor());
        const int n = (ins >= kSampleInstrumentCount && ins < kInstrumentCount ? kSampleInstrumentCount : 0) + pad;
        if (n < kInstrumentCount) pop_.select(synthui::popupPosition(n));
    } else if (pop_.kind() == 5) {
        // MIDI tracks: the Instr-column pad map (pads 1-16 MIDI, 17-19 synths).
        if (pad < synthui::kMidiPopupCount) pop_.select(synthui::popupPositionMidi(synthui::midiTrackPadInstrument(pad)));
    } else {
        pop_.pick(pad);
    }
    refreshBar();
    ui_.dirty();
}

void PatternScreen::popupLeds(uint8_t* lv) const {
    const uint8_t dim = app_.cfg.padDim(), full = app_.cfg.padFull();
    if (pop_.kind() == 2) {   // the stock pad map of popupPad (synth entries light no pad)
        const int ins = synthui::popupInstrument(pop_.cursor());
        const int base = ins >= kSampleInstrumentCount && ins < kInstrumentCount ? kSampleInstrumentCount : 0;
        for (int p = 0; p < 48; ++p) lv[p] = base + p == ins ? full : (base + p < kInstrumentCount ? dim : 0);
        return;
    }
    if (pop_.kind() == 5) {   // MIDI tracks: the pad map of popupPad
        const int ins = synthui::popupInstrumentMidi(pop_.cursor());
        for (int p = 0; p < synthui::kMidiPopupCount; ++p) lv[p] = synthui::midiTrackPadInstrument(p) == ins ? full : dim;
        return;
    }
    if (pop_.kind() != 0) {
        pop_.leds(lv, dim, full);
        return;
    }
    for (int p = 0; p < 48; ++p) {
        const int n = noteWin_ + 12 * (3 - p / 12) + p % 12;
        lv[p] = n == pop_.cursor() ? full : (n <= 127 ? dim : 0);
    }
}

// ---------------------------------------------------------------------------
// Frame

void PatternScreen::draw(Gfx& g) {
    const Theme& th = ui_.th();
    const bool notes = pop_.isOpen() && pop_.kind() == 0;
    if (notes) {
        drawNotesPopup(g);
        drawCountIn(g);
        return;
    }
    const bool rightPop = pop_.isOpen();
    BottomBar& b = ui_.bar;
    b.setStripStyle(fillOpen ? wg::StripStyle::kDark : wg::StripStyle::kNormal);   // 050505 / 434343 under Fill [S snap 019]
    // The Fill popup's overlay covers the whole grid (spec 20 §4.1 Fill
    // popup: full overlay label (0, 32, 800, 389)); the grid is not drawn.
    if (fillOpen) drawFill(g);
    else drawGrid(g);
    if (!ui_.kb.isOpen()) b.drawStrip(g, th);
    // fullScreen(): this screen draws strip, title, hairline and labels
    // itself so the popup panel can cover the bar (and the Fx footer sits on
    // it); the popup header bands come from Ui (openPopupHeader).
    if (topInfoOn()) {
        drawTitleBar(g, rightPop);
        wg::titleHairline(g, th, 0, rightPop ? kPopupX - 1 : kScreenW - 1);
    }
    drawBar(g);
    if (rightPop) drawListPopup(g);   // covers F7 / F8 and the bar from x 600
    drawCountIn(g);
}

}  // namespace ot
