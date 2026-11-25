// [I] Patch files of another format (modules/synth_descriptor.h PatchImport,
// e.g. DX7 .syx banks): a module engine declares the file extension and how
// to read it; Load Patch lists those files in Patches/<engine>/ next to the
// .patch files. Files of a bank's size come first and open like folders
// (their patches preview and load, Left goes back, Right opens again); a
// smaller file sits with the patches and previews / loads at once. A patch
// loads as the engine's Init patch (with its descriptor's initPatch) plus
// what the module reads from the file, named after it. Also the patch
// helpers modules use for data of their own (unknown top-level fields).
#include "test.h"

#include <cstring>
#include <string>
#include <vector>

#include "audio/synth_engine.h"
#include "formats/patch_pb.h"
#include "modules/module.h"
#include "modules/synth_descriptor.h"
#include "ui/screens.h"
#include "ui_fakes.h"

using namespace ot;

namespace {

constexpr int32_t kEng = 0xF3;          // a test-only id
constexpr uint32_t kVoiceField = 100;   // the engine's own patch data
constexpr int kNameLen = 12;

// The test format ".tbk": "TBK", a count byte, then that many 12-byte names.
int tbkCount(const uint8_t* f, uint32_t len) {
    if (len < 4 || std::memcmp(f, "TBK", 3) != 0) return 0;
    const int n = f[3];
    return n > 0 && len >= 4u + uint32_t(n) * kNameLen ? n : 0;
}
void tbkName(const uint8_t* f, uint32_t, int k, char* out, int cap) {
    int n = 0;
    for (; n < kNameLen && n < cap - 1; ++n) out[n] = char(f[4 + k * kNameLen + n]);
    while (n > 0 && out[n - 1] == ' ') --n;
    out[n] = 0;
}
bool tbkPatch(const uint8_t* f, uint32_t, int k, SynthPatch& p) {
    if (f[4 + k * kNameLen] == '!') return false;   // refused
    return patchSetOpaqueBytes(p, kVoiceField, f + 4 + k * kNameLen, kNameLen);
}
void initPatch(SynthPatch& p) { patchSetOpaqueBytes(p, kVoiceField, reinterpret_cast<const uint8_t*>("INIT"), 4); }

const mod::PatchImport kImport = {".tbk", 1024, 40, tbkCount, tbkName, tbkPatch};
const PatchParamRange kParams[1] = {{1, 0, 2}};
const synthui::ParamUi kUi[1] = {{"Volume", synthui::kFmtPct, nullptr, 0}};
const uint8_t kMain[] = {0};
const synthui::Section kSections[] = {OT_SYNTH_SECTION("Main", "Main", kMain), OT_SYNTH_MACRO_SECTIONS};
const mod::SynthDescriptor kDesc = {kEng, "TIMP", 0, 0, 1, kParams, kUi, kSections, 8, 0, nullptr,
                                    nullptr, -1, initPatch, &kImport};

class ImportModule final : public mod::IModule {
public:
    const mod::ModuleInfo& info() const override {
        static const mod::ModuleInfo i = {"TIMP", mod::kModuleApi, 100, "Test import",
                                          mod::kCapSynthEngine | mod::kCapSynthDescriptor, 0, 0, 0, 0, 8};
        return i;
    }
    bool attach(mod::ModuleHost&) override { return true; }
    audio::ISynthEngine* synthEngine(int32_t id) override { return id == kEng ? audio::synthTestEngine() : nullptr; }
    const mod::SynthDescriptor* synthDescriptor(int i) const override { return i == 0 ? &kDesc : nullptr; }
};
ImportModule g_mod;

void reg() {
    static bool done = false;
    if (!done) done = mod::ModuleHost::instance().addForTest(g_mod);
    CHECK(done);
}

std::vector<uint8_t> tbk(std::initializer_list<const char*> names) {
    std::vector<uint8_t> f = {'T', 'B', 'K', uint8_t(names.size())};
    for (const char* n : names) {
        char e[kNameLen];
        std::memset(e, ' ', sizeof e);
        std::memcpy(e, n, std::strlen(n));
        f.insert(f.end(), e, e + kNameLen);
    }
    return f;
}

std::vector<uint8_t> encode(const SynthPatch& p) {
    std::vector<uint8_t> out(kPatchMaxFileSize);
    size_t n = 0;
    CHECK(encodePatch(p, out.data(), out.size(), &n) == PatchStatus::kOk);
    out.resize(n);
    return out;
}

// The patch's own data (field 100), "" when absent.
std::string voice(const SynthPatch& p) {
    const uint8_t* d = nullptr;
    size_t n = 0;
    if (!patchOpaqueBytes(p, kVoiceField, &d, &n)) return "";
    return std::string(reinterpret_cast<const char*>(d), n);
}

SynthScreen& synth(App& a) { return *static_cast<SynthScreen*>(a.ui.screen(kModSynth)); }

}  // namespace

TEST(patch_opaque_bytes_replace_and_keep_order) {
    SynthPatch p;
    CHECK(patchInit(kSynthEngineAcd, &p));
    const uint8_t* d = nullptr;
    size_t n = 0;
    CHECK(!patchOpaqueBytes(p, 100, &d, &n));
    CHECK(patchSetOpaqueBytes(p, 100, reinterpret_cast<const uint8_t*>("abc"), 3));
    CHECK(patchSetOpaqueBytes(p, 101, reinterpret_cast<const uint8_t*>("xy"), 2));
    CHECK(patchSetOpaqueBytes(p, 100, reinterpret_cast<const uint8_t*>("longer"), 6));   // in place
    CHECK(patchOpaqueBytes(p, 100, &d, &n));
    CHECK_EQ(int(n), 6);
    CHECK(std::memcmp(d, "longer", 6) == 0);
    CHECK(patchOpaqueBytes(p, 101, &d, &n));
    CHECK_EQ(int(n), 2);
    // Through a file: the fields stay.
    const std::vector<uint8_t> f = encode(p);
    SynthPatch back;
    CHECK(decodePatch(f.data(), f.size(), &back) == PatchStatus::kOk);
    CHECK(patchOpaqueBytes(back, 100, &d, &n));
    CHECK(std::memcmp(d, "longer", 6) == 0);
    // Too much: refused, unchanged.
    std::vector<uint8_t> big(kPatchOpaqueMax, 7);
    CHECK(!patchSetOpaqueBytes(p, 102, big.data(), big.size()));
    CHECK(patchOpaqueBytes(p, 100, &d, &n));
    CHECK_EQ(int(n), 6);
}

// Only an unknown field number may be stored: field 0 and numbers past the
// protobuf range make no valid tag, and the fields the encoder writes itself
// (engine, parameters, names, macros, extra, extension parameters) would be
// written twice.
TEST(patch_opaque_bytes_refuse_reserved_fields) {
    SynthPatch p;
    CHECK(patchInit(kSynthEngineAcd, &p));
    const std::vector<uint8_t> before = encode(p);
    const uint8_t* x = reinterpret_cast<const uint8_t*>("x");
    for (uint32_t f : {0u, 1u, 6u, 7u, 8u, 9u, kPatchExtField, 0x20000000u, 0xFFFFFFFFu}) {
        CHECK(!patchSetOpaqueBytes(p, f, x, 1));
        CHECK_EQ(int(p.opaqueLen), 0);
    }
    CHECK(encode(p) == before);
    for (uint32_t f : {2u, 5u, 10u, 15u, 17u, 100u, 0x1FFFFFFFu}) CHECK(patchSetOpaqueBytes(p, f, x, 1));
    const std::vector<uint8_t> f = encode(p);
    SynthPatch back;
    CHECK(decodePatch(f.data(), f.size(), &back) == PatchStatus::kOk);
    const uint8_t* d = nullptr;
    size_t n = 0;
    CHECK(patchOpaqueBytes(back, 0x1FFFFFFFu, &d, &n));
    CHECK_EQ(int(n), 1);
}

TEST(patch_import_init_patch_hook) {
    reg();
    SynthPatch p;
    CHECK(patchInit(kEng, &p));
    CHECK(voice(p) == "INIT");
    CHECK(patchInit(kSynthEngineAcd, &p));   // engines without the hook: none
    CHECK(voice(p) == "");
}

TEST(patch_import_browser) {
    reg();
    Harness h(false);
    h.fs.dirs.insert("Patches");
    h.fs.dirs.insert("Patches/TIMP");
    h.fs.files["Patches/TIMP/kit.tbk"] = tbk({"ONE", "TWO", "THREE"});   // 40 bytes: a bank
    const char* const kLong = "zz a long bank name, over the 32 characters.tbk";
    h.fs.files[std::string("Patches/TIMP/") + kLong] = tbk({"L1", "L2", "L3"});
    h.fs.files["Patches/TIMP/Lone.TBK"] = tbk({"SOLO"});   // 16 bytes: a single patch
    h.fs.files["Patches/TIMP/bad.tbk"] = std::vector<uint8_t>{1, 2, 3};
    SynthPatch saved;
    CHECK(patchInit(kEng, &saved));
    h.fs.files["Patches/TIMP/saved.patch"] = encode(saved);
    h.fs.files["Patches/ACD/other.tbk"] = tbk({"ONE", "TWO", "THREE"});   // other models list no .tbk
    h.app->boot();
    App& a = *h.app;
    a.setSynthEngine(1, kEng);
    a.setCurrentInstrument(kFirstSynthInstrument + 1);
    h.tap(btn::Pattern);
    h.tap(btn::InstrParams);
    SynthScreen& s = synth(a);
    CHECK(s.available());
    h.tap(btn::F7);
    CHECK(s.view == SynthScreen::View::Browser);
    CHECK_EQ(s.browserCount, 5);
    CHECK_STR(s.browser[0], "kit.tbk");
    CHECK_STR(s.browser[1], kLong);
    CHECK_STR(s.browser[2], "bad.tbk");
    CHECK_STR(s.browser[3], "Lone.TBK");
    CHECK_STR(s.browser[4], "saved");
    CHECK(s.browserKind[0] == SynthScreen::Entry::Bank);
    CHECK(s.browserKind[1] == SynthScreen::Entry::Bank);
    CHECK(s.browserKind[2] == SynthScreen::Entry::Single);
    CHECK(s.browserKind[3] == SynthScreen::Entry::Single);
    CHECK(s.browserKind[4] == SynthScreen::Entry::Patch);
    // On a bank: Open, no Preview.
    CHECK_EQ(s.browserSel, 0);
    CHECK_STR(a.ui.bar.label[7].top, "Open");
    CHECK(!a.ui.bar.label[4].enabled);
    // A single-patch file: Preview plays it, Confirm loads it.
    s.browserSel = 2;
    h.tap(btn::Down);   // Lone.TBK
    CHECK_STR(a.ui.bar.label[7].top, "Confirm");
    CHECK(a.ui.bar.label[4].enabled);
    h.down(btn::F5);
    h.audio.pump(4);
    CHECK_EQ(a.synthPreviewSlot(), 1);
    CHECK(voice(a.synthPatch(1)) == "SOLO        ");
    CHECK(std::strcmp(a.synthSlot(1).patchName, "Init") == 0);
    h.up(btn::F5);
    // ... a damaged one does not load: the slot's own patch is back.
    h.tap(btn::Up);   // bad.tbk
    h.down(btn::F5);
    h.audio.pump(4);
    h.up(btn::F5);
    CHECK_EQ(s.previewSel(), -1);
    CHECK_EQ(a.synthPreviewSlot(), -1);
    CHECK(voice(a.synthPatch(1)) == "INIT");
    h.tap(btn::Down);
    h.tap(btn::F8);
    CHECK(s.view == SynthScreen::View::Params);
    CHECK(std::strcmp(a.synthSlot(1).patchName, "SOLO") == 0);
    CHECK(voice(a.synthPatch(1)) == "SOLO        ");
    // Open the bank: its patches.
    h.tap(btn::F7);
    CHECK_EQ(s.browserSel, 0);
    h.tap(btn::F8);
    CHECK_STR(s.bank, "kit.tbk");
    CHECK_EQ(s.browserCount, 3);
    CHECK_STR(s.browser[0], "ONE");
    CHECK_STR(s.browser[2], "THREE");
    CHECK(s.browserKind[0] == SynthScreen::Entry::Voice);
    CHECK_STR(a.ui.bar.label[7].top, "Confirm");
    // Preview patch 2: it plays in the slot, the record stays.
    h.tap(btn::Down);
    h.down(btn::F5);
    h.audio.pump(4);
    CHECK_EQ(a.synthPreviewSlot(), 1);
    CHECK(voice(a.synthPatch(1)) == "TWO         ");
    CHECK(std::strcmp(a.synthSlot(1).patchName, "SOLO") == 0);
    h.up(btn::F5);
    // Left: back to the folder, the bank highlighted; Right opens it again.
    h.tap(btn::Left);
    CHECK_STR(s.bank, "");
    CHECK_EQ(s.browserCount, 5);
    CHECK_EQ(s.browserSel, 0);
    h.tap(btn::Right);
    CHECK_STR(s.bank, "kit.tbk");
    // Confirm patch 3: loaded, named after it, dirty.
    h.tap(btn::Down);
    h.tap(btn::Down);
    h.tap(btn::F8);
    CHECK(s.view == SynthScreen::View::Params);
    CHECK_EQ(a.synthPreviewSlot(), -1);
    CHECK(std::strcmp(a.synthSlot(1).patchName, "THREE") == 0);
    CHECK(voice(a.synthPatch(1)) == "THREE       ");
    CHECK(a.synthPatchDirty(1));
    // Save writes Patches/TIMP/THREE.patch with the module's data in it.
    s.savePatch();
    CHECK(h.fs.files.count("Patches/TIMP/THREE.patch") == 1);
    SynthPatch back;
    const std::vector<uint8_t>& f = h.fs.files["Patches/TIMP/THREE.patch"];
    CHECK(decodePatch(f.data(), f.size(), &back) == PatchStatus::kOk);
    CHECK(voice(back) == "THREE       ");
    // The long-named bank opens too.
    h.tap(btn::F7);
    CHECK_EQ(s.browserCount, 6);   // + THREE
    s.browserSel = 1;
    h.tap(btn::F8);
    CHECK_STR(s.bank, kLong);
    CHECK_EQ(s.browserCount, 3);
    // Cancel inside a bank closes the popup; the next Load starts in the folder.
    h.tap(btn::F7);
    CHECK(s.view == SynthScreen::View::Params);
    h.tap(btn::F7);
    CHECK_STR(s.bank, "");
    CHECK_EQ(s.browserCount, 6);
    h.tap(btn::F7);
    // Another model lists no .tbk files.
    a.setSynthModel(2, int(SynthModel::ACD));
    a.setCurrentInstrument(kFirstSynthInstrument + 2);
    h.tap(btn::Pattern);
    h.tap(btn::InstrParams);
    h.tap(btn::F7);
    for (int i = 0; i < s.browserCount; ++i) CHECK(s.browserKind[i] == SynthScreen::Entry::Patch);
}

// An imported patch's name makes a valid file name: the characters a FAT
// file name cannot hold become '_'. The first Save of an imported patch
// asks before it replaces a library patch of the same name; once saved,
// Save writes over it as for any library patch.
TEST(patch_import_save_name_and_overwrite) {
    reg();
    Harness h(false);
    h.fs.dirs.insert("Patches");
    h.fs.dirs.insert("Patches/TIMP");
    h.fs.files["Patches/TIMP/odd.tbk"] = tbk({"a:b*c?d\"e<f", "g>h|i\x01j\x7fk", "TWO"});
    SynthPatch lib;
    CHECK(patchInit(kEng, &lib));
    const std::vector<uint8_t> kept = encode(lib);
    h.fs.files["Patches/TIMP/TWO.patch"] = kept;
    h.app->boot();
    App& a = *h.app;
    a.setSynthEngine(1, kEng);
    a.setCurrentInstrument(kFirstSynthInstrument + 1);
    h.tap(btn::Pattern);
    h.tap(btn::InstrParams);
    SynthScreen& s = synth(a);
    CHECK(s.available());
    h.tap(btn::F7);
    CHECK(s.view == SynthScreen::View::Browser);
    CHECK_STR(s.browser[0], "odd.tbk");
    h.tap(btn::F8);   // open the bank
    CHECK_STR(s.bank, "odd.tbk");
    CHECK_EQ(s.browserCount, 3);
    CHECK_STR(s.browser[0], "a_b_c_d_e_f");
    CHECK_STR(s.browser[1], "g_h_i_j_k");
    // Load and Save: a file of the mapped name.
    h.tap(btn::F8);
    CHECK(s.view == SynthScreen::View::Params);
    CHECK(std::strcmp(a.synthSlot(1).patchName, "a_b_c_d_e_f") == 0);
    s.savePatch();
    CHECK(!a.ui.askOpen());
    CHECK(h.fs.files.count("Patches/TIMP/a_b_c_d_e_f.patch") == 1);
    h.tap(btn::F7);
    s.browserSel = 0;
    h.tap(btn::F8);   // the bank again
    CHECK_STR(s.bank, "odd.tbk");
    s.browserSel = 1;
    h.tap(btn::F8);
    CHECK(std::strcmp(a.synthSlot(1).patchName, "g_h_i_j_k") == 0);
    s.savePatch();
    CHECK(!a.ui.askOpen());
    CHECK(h.fs.files.count("Patches/TIMP/g_h_i_j_k.patch") == 1);
    // A patch of that name exists: Save asks; Cancel writes nothing.
    h.tap(btn::F7);
    s.browserSel = 0;
    h.tap(btn::F8);   // the bank again
    CHECK_STR(s.bank, "odd.tbk");
    s.browserSel = 2;
    h.tap(btn::F8);
    CHECK(s.view == SynthScreen::View::Params);
    CHECK(std::strcmp(a.synthSlot(1).patchName, "TWO") == 0);
    s.savePatch();
    CHECK(a.ui.askOpen());
    h.tap(btn::F7);   // Cancel
    CHECK(!a.ui.askOpen());
    CHECK(h.fs.files["Patches/TIMP/TWO.patch"] == kept);
    // Overwrite writes the imported patch.
    s.savePatch();
    CHECK(a.ui.askOpen());
    h.tap(btn::F8);   // Overwrite
    CHECK(!a.ui.askOpen());
    SynthPatch back;
    const std::vector<uint8_t>& f = h.fs.files["Patches/TIMP/TWO.patch"];
    CHECK(decodePatch(f.data(), f.size(), &back) == PatchStatus::kOk);
    CHECK(voice(back) == "TWO         ");
    // Saved once: the next Save writes at once.
    patchSetOpaqueBytes(a.synthPatch(1), kVoiceField, reinterpret_cast<const uint8_t*>("NEW"), 3);
    s.savePatch();
    CHECK(!a.ui.askOpen());
    const std::vector<uint8_t>& g = h.fs.files["Patches/TIMP/TWO.patch"];
    CHECK(decodePatch(g.data(), g.size(), &back) == PatchStatus::kOk);
    CHECK(voice(back) == "NEW");
}
