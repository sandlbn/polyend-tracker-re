// [I] Master effects: the FX bank's list and the card effects'
// loading. A card effect (modules/module_files.h) loads while it is on, with
// the card synths in one load set (App::syncCardSynths): Turn on loads it at
// once while stopped, at the next Stop while playing; Off lets it leave at
// the next Stop; Open loads the ones the project has data for, and the ones
// that are off leave again. Its project data waits in the sidecar's kept
// chunks while it is not loaded (stashModuleSection). Its SDRAM is held back
// from the top of the sample memory, after the samples.
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "modules/module.h"
#include "modules/module_files.h"
#include "modules/module_ui.h"
#include "sdram_bss.h"

namespace ot {

namespace {

struct FxState {
    uint32_t pending;   // card effects (cardFxAt bits) to load and turn on
    uint32_t open;      // card effects to load for their project data (Open)
    uint32_t tried;     // the set the last load tried (serviceCardSynths)
    uint32_t waiting;   // the set waiting to settle
    App::FxError error[mod::kMaxCardFx];
};
OT_SDRAM_BSS FxState g_fx;
static_assert(mod::kMaxCardFx <= 32, "FxState keeps the card effects as bits of a word");

uint32_t fxBit(int k) { return k >= 0 && k < 32 ? 1u << k : 0u; }

bool fxOn(const mod::IModule* m) { return m && mod::pageOn(const_cast<mod::IModule*>(m)->page()); }

// The ids in the list: compiled-in or loaded effects, then the card's not
// loaded, sorted by chain rank (stable).
struct FxIds {
    const char* id[mod::ModuleHost::kMaxModules + mod::kMaxCardFx];
    int n = 0;
};

void listIds(FxIds& l) {
    mod::ModuleHost& h = mod::ModuleHost::instance();
    mod::IModule* order[mod::ModuleHost::kMaxModules];
    const int n = h.chainOrder(order);
    for (int i = 0; i < n; ++i)
        if (order[i]->page()) l.id[l.n++] = order[i]->info().id;
    for (int k = 0; k < mod::cardFxCount(); ++k) {
        if (mod::cardFxModule(k)) continue;
        const char* id = mod::cardFxAt(k)->id;
        int at = l.n++;
        while (at > 0 && mod::ModuleHost::chainRank(l.id[at - 1]) > mod::ModuleHost::chainRank(id)) {
            l.id[at] = l.id[at - 1];
            --at;
        }
        l.id[at] = id;
    }
}

uint32_t holdBytes(const int* fx, int nFx) {
    uint32_t b = 0;
    for (int k = 0; k < nFx; ++k) b += (mod::cardFxAt(fx[k])->sdramBytes + 15u) & ~15u;
    return b;
}

}  // namespace

int App::masterFxCount() const {
    FxIds l;
    listIds(l);
    return l.n;
}

bool App::masterFx(int k, MasterFx& out) const {
    FxIds l;
    listIds(l);
    if (k < 0 || k >= l.n) return false;
    out = MasterFx{};
    std::memcpy(out.id, l.id[k], 4);
    out.id[4] = 0;
    out.card = mod::cardFxIndex(out.id);
    mod::ModuleHost& h = mod::ModuleHost::instance();
    out.module = const_cast<mod::IModule*>(h.find(out.id));
    if (out.module) {
        out.name = out.module->info().name;
        out.sdramBytes = out.module->info().sdramBytes;
        out.fromCard = out.card >= 0 || h.fromCard(*out.module);
        out.on = fxOn(out.module);
    } else {
        const mod::CardFx* f = mod::cardFxAt(out.card);
        out.name = f ? f->name : out.id;
        out.sdramBytes = f ? f->sdramBytes : 0;
        out.fromCard = true;
    }
    out.pending = (g_fx.pending & fxBit(out.card)) != 0;
    out.error = out.card >= 0 ? g_fx.error[out.card] : FxError::kNone;
    return true;
}

const char* App::fxErrorText(FxError e) {
    switch (e) {
        case FxError::kSampleMemory: return "Not enough sample memory";
        case FxError::kModuleMemory: return "Card memory full";   // as card synths say
        case FxError::kFailed: return "Didn't load";
        default: return nullptr;
    }
}

const char* App::turnMasterFx(int k, bool on) {
    MasterFx fx;
    if (!masterFx(k, fx)) return nullptr;
    if (fx.module) {
        mod::pageTurn(fx.module->page(), on);
        if (!on && fx.card >= 0) g_fx.pending &= ~fxBit(fx.card);   // leaves at the next Stop
        markProjectDirty();
        return nullptr;
    }
    if (fx.card < 0) return nullptr;
    if (!on) {   // not loaded yet: forget the request
        g_fx.pending &= ~fxBit(fx.card);
        return nullptr;
    }
    g_fx.pending |= fxBit(fx.card);
    g_fx.error[fx.card] = FxError::kNone;
    if (seq.isPlaying() || seq.isArmed() || jobBusy()) return nullptr;   // loads at the next Stop
    syncCardSynths(true);
    if (g_fx.error[fx.card] != FxError::kNone) {
        static char t[48];
        std::snprintf(t, sizeof t, "%s: %s", fx.name, fxErrorText(g_fx.error[fx.card]));
        return t;
    }
    return nullptr;
}

void App::bindMasterInserts() {
    audio::IMasterInsert* ins[audio::AudioEngine::kMaxMasterInserts];
    const int n = mod::ModuleHost::instance().masterInserts(ins, audio::AudioEngine::kMaxMasterInserts);
    engine.setMasterInserts(ins, n);
}

int App::wantedCardFx(int* fx) const {
    int n = 0;
    FxIds l;
    listIds(l);
    for (int i = 0; i < l.n; ++i) {   // chain order: the first ones get the room
        const int k = mod::cardFxIndex(l.id[i]);
        if (k < 0) continue;
        const mod::IModule* m = mod::cardFxModule(k);
        if ((m && fxOn(m)) || (g_fx.pending & fxBit(k)) || (g_fx.open & fxBit(k))) fx[n++] = k;
    }
    return n;
}

uint32_t App::cardFxMask() const {
    int fx[mod::kMaxCardFx];
    const int n = wantedCardFx(fx);
    uint32_t m = 0;
    for (int i = 0; i < n; ++i) m |= fxBit(fx[i]);
    return m;
}

void App::markCardFxTried(const int* fx, int nFx) {
    g_fx.tried = 0;
    for (int i = 0; i < nFx; ++i) g_fx.tried |= fxBit(fx[i]);
}

void App::settleCardFx() {
    // Nothing to load: requests for effects already in are done.
    for (int k = 0; k < mod::cardFxCount(); ++k)
        if (mod::IModule* m = mod::cardFxModule(k)) {
            if (g_fx.pending & fxBit(k)) mod::pageTurn(m->page(), true);
            g_fx.pending &= ~fxBit(k);
            g_fx.open &= ~fxBit(k);
        }
}

void App::beginCardFx(int* fx, int* nFx) {
    // Every loaded card effect leaves with this load: its data waits in the
    // sidecar, the chain lets go of it.
    for (int k = 0; k < mod::cardFxCount(); ++k)
        if (mod::IModule* m = mod::cardFxModule(k)) stashModuleSection(*m);
    engine.setMasterInserts(nullptr, 0);
    // Their SDRAM from the top of the sample memory, samples first: an
    // effect without room is left out (the later ones in chain order first).
    bank.setModuleHold(0);
    while (*nFx > 0 && !bank.setModuleHold((holdBytes(fx, *nFx) + 1u) / 2u)) {
        int last = -1;
        for (int i = 0; i < *nFx; ++i)
            if (mod::cardFxAt(fx[i])->sdramBytes) last = i;
        if (last < 0) break;   // nothing to drop (a reservation is held)
        g_fx.error[fx[last]] = FxError::kSampleMemory;
        for (int i = last; i + 1 < *nFx; ++i) fx[i] = fx[i + 1];
        --*nFx;
    }
    const uint32_t bytes = uint32_t(bank.moduleHold() * 2u);
    if (int16_t* area = bank.moduleArea()) {
        std::memset(area, 0, bytes);
        mod::ModuleHost::instance().setCardSdram(area, bytes);
    } else {
        mod::ModuleHost::instance().setCardSdram(nullptr, 0);
    }
}

bool App::endCardFx(const int* fx, int nFx) {
    mod::ModuleHost::instance().setCardSdram(nullptr, 0);
    bool again = false;
    for (int i = 0; i < nFx; ++i) {
        const int k = fx[i];
        mod::IModule* m = mod::cardFxModule(k);
        const mod::OtmLoadReport::Item& it = moduleLoad_.items[mod::cardFxAt(k)->file];
        if (!m) {
            if (g_fx.error[k] == FxError::kNone)
                g_fx.error[k] = it.error == mod::OtmError::TooBig || moduleLoad_.noRoom ? FxError::kModuleMemory
                                                                                        : FxError::kFailed;
            if ((g_fx.open & fxBit(k)) && !loadNotice_[0])
                std::snprintf(loadNotice_, sizeof loadNotice_, "%s: %s", mod::cardFxAt(k)->name,
                              fxErrorText(g_fx.error[k]));
            continue;
        }
        g_fx.error[k] = FxError::kNone;
        restoreModuleSection(*m);
        if (g_fx.pending & fxBit(k)) mod::pageTurn(m->page(), true);
        else if (!fxOn(m)) again = true;   // Open: off, it leaves again
    }
    // Errors of effects this load left out stay; the requests are done.
    g_fx.pending = 0;
    g_fx.open = 0;
    bindMasterInserts();
    return again;
}

void App::releaseCardFx() {
    g_fx.pending = g_fx.open = g_fx.tried = g_fx.waiting = 0;
    for (FxError& e : g_fx.error) e = FxError::kNone;
    bool any = false;
    for (int k = 0; k < mod::cardFxCount(); ++k) any |= mod::cardFxModule(k) != nullptr;
    if (!any || !mem_.scratch) {
        bank.setModuleHold(0);
        return;
    }
    int32_t engines[kCardWanted];
    cardWantedEngines(engines);
    if (previewSlot_ >= 0) endSynthPreview();
    holdSynthEngines();
    engine.setMasterInserts(nullptr, 0);
    mod::loadCardSynths(fs(), engines, kCardWanted, mem_.scratch, mem_.scratchBytes);
    engine.synth().holdEngines(false);
    bank.setModuleHold(0);
    bindMasterInserts();
}

void App::openCardFx() {
    // Card effects with project data load for it (stopped: the project load).
    for (int k = 0; k < mod::cardFxCount(); ++k)
        if (sidecarHasModule(mod::cardFxAt(k)->id)) g_fx.open |= fxBit(k);
}

bool App::cardFxWaiting(uint32_t now) {
    const uint32_t m = cardFxMask();
    if (m == g_fx.tried) return false;
    if (m != g_fx.waiting) {
        g_fx.waiting = m;
        cardSince_ = now ? now : 1;
    }
    return true;
}

}  // namespace ot
