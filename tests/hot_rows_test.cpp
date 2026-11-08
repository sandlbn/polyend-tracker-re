// [I] Built-in synths' hot rows (modules/hot_rows.h): a fake row
// moved from "flash" into a pool buffer at device addresses, its branches,
// literals, tables and slots checked after the move and after restoring.
#include "test.h"

#include <cstdint>
#include <vector>

#include "modules/hot_rows.h"
#include "modules/thumb_branch.h"

using namespace ot;
using namespace ot::mod;

namespace {

constexpr uint32_t kRow = 0x00010000u;     // the row in flash
constexpr uint32_t kTable = 0x00030000u;   // its table in flash
constexpr uint32_t kCold = 0x00020000u;    // a flash function outside the row
constexpr uint32_t kPool = 0x1FFF8000u;    // the pool in SRAM_L

uint32_t rd(const uint8_t* p) { return thumb::rd32(p); }

// A 48-byte row in three 16-byte pieces (cuts 16, 32, 48):
//   0: BL to piece 2 (row + 20)    8: BL to kCold    12: word row + 33 (piece 3, Thumb)
//  24: B.W to kCold               28: word kTable + 4
// and a table: [0] row + 1 (piece 1), [1] row + 41 (piece 3), [2] plain data.
struct Fake {
    uint8_t code[48] = {};
    uint8_t table[12] = {};
    uint32_t cuts[3] = {16, 32, 48};
    uint32_t fixups[5] = {0u | kHotFixBranch << 24, 8u | kHotFixBranch << 24, 12u | kHotFixWord << 24,
                          24u | kHotFixBranch << 24, 28u | kHotFixWord << 24};
    uint32_t entries[2] = {0, 4};
    uint32_t vptr = kTable + 4;   // the engine object's vtable pointer (RAM)
    HotTable tab{};
    HotSlot slot{};
    HotRow row{};

    static void branch(uint8_t* p, uint32_t from, uint32_t to, bool link) {
        thumb::wr16(p, 0xF000u);
        thumb::wr16(p + 2, link ? 0xF800u : 0xB800u);
        thumb::setBranchOffset(p, int64_t(to) - int64_t(from + 4));
    }
    Fake() {
        branch(code + 0, kRow + 0, kRow + 20, true);
        branch(code + 8, kRow + 8, kCold, true);
        thumb::wr32(code + 12, kRow + 33);
        branch(code + 24, kRow + 24, kCold, false);
        thumb::wr32(code + 28, kTable + 4);
        thumb::wr32(table + 0, kRow + 1);
        thumb::wr32(table + 4, kRow + 41);
        thumb::wr32(table + 8, 0x12345678u);
        tab = HotTable{table, kTable, 12, 2, entries};
        slot = HotSlot{&vptr, kTable + 4};
        row = HotRow{nullptr, code, kRow, 48, 3, cuts, 5, fixups, 1, &tab, 1, &slot};
    }
};

uint32_t target(const uint8_t* mem, uint32_t base, uint32_t off) {
    return base + off + 4u + uint32_t(thumb::branchOffset(mem + off));
}

}  // namespace

TEST(span_counts_stubs_and_tables) {
    Fake f;
    CHECK(hotRowCheck(f.row));
    CHECK_EQ(hotRowSpan(f.row, 0), 0u);
    // Cut 16: piece 2 and kCold stay in flash (2 stubs); cut 32: only kCold
    // (shared by two branches); cut 48: the same. The table always (12 B).
    CHECK_EQ(hotRowSpan(f.row, 16), 16u + 16u + 12u);
    CHECK_EQ(hotRowSpan(f.row, 32), 32u + 8u + 12u);
    CHECK_EQ(hotRowSpan(f.row, 48), 48u + 8u + 12u);
    CHECK_EQ(hotRowSpan(f.row, 24), UINT32_MAX);   // not a cut
}

TEST(move_fixes_branches_words_tables_slots) {
    Fake f;
    std::vector<uint8_t> pool(256, 0xEE);
    CHECK(hotRowMove(f.row, 32, OtmBlock{pool.data(), uint32_t(pool.size()), kPool}));
    const uint8_t* p = pool.data();
    // Inside the moved part: the displacement holds, the target moved along.
    CHECK_EQ(target(p, kPool, 0), kPool + 20);
    // To flash: both branches through the one stub after the code.
    CHECK_EQ(target(p, kPool, 8), kPool + 32);
    CHECK_EQ(target(p, kPool, 24), kPool + 32);
    CHECK_EQ(thumb::rd16(p + 32), 0xF8DFu);
    CHECK_EQ(thumb::rd16(p + 34), 0xF000u);
    CHECK_EQ(rd(p + 36), kCold | 1u);
    // The word into piece 3 (not moved) stays; the one into the table
    // points at its copy after the stub.
    CHECK_EQ(rd(p + 12), kRow + 33);
    const uint32_t tcopy = kPool + 40;
    CHECK_EQ(rd(p + 28), tcopy + 4);
    // The table's copy: piece 1 moved, piece 3 not, data untouched.
    CHECK_EQ(rd(p + 40), kPool + 1);
    CHECK_EQ(rd(p + 44), kRow + 41);
    CHECK_EQ(rd(p + 48), 0x12345678u);
    CHECK_EQ(pool[52], 0xEE);   // nothing past the span
    CHECK_EQ(f.vptr, tcopy + 4);
    // Flash is never written.
    Fake g;
    CHECK(std::memcmp(f.code, g.code, sizeof(f.code)) == 0);
    CHECK(std::memcmp(f.table, g.table, sizeof(f.table)) == 0);
    hotRowRestore(f.row);
    CHECK_EQ(f.vptr, kTable + 4);
}

TEST(move_whole_row) {
    Fake f;
    std::vector<uint8_t> pool(256, 0);
    CHECK(hotRowMove(f.row, 48, OtmBlock{pool.data(), uint32_t(pool.size()), kPool}));
    CHECK_EQ(rd(pool.data() + 12), kPool + 33);   // piece 3 moved now
    CHECK_EQ(rd(pool.data() + 48 + 8 + 4), kPool + 41);
    CHECK(hotRowMove(f.row, 0, OtmBlock{nullptr, 0, 0}));   // cut 0 = restore
    CHECK_EQ(f.vptr, kTable + 4);
}

TEST(move_refusals_write_nothing) {
    Fake f;
    std::vector<uint8_t> pool(256, 0xEE);
    CHECK(!hotRowMove(f.row, 24, OtmBlock{pool.data(), 256, kPool}));      // not a cut
    CHECK(!hotRowMove(f.row, 32, OtmBlock{pool.data(), 51, kPool}));       // one byte short
    CHECK(!hotRowMove(f.row, 32, OtmBlock{pool.data(), 256, kPool + 4}));  // base not 8-aligned
    for (uint8_t b : pool) CHECK_EQ(b, 0xEE);
    CHECK_EQ(f.vptr, kTable + 4);
    CHECK(hotRowMove(f.row, 32, OtmBlock{pool.data(), 52, kPool}));        // exactly the span
}

TEST(check_refuses_bad_rows) {
    {
        Fake f;
        f.fixups[2] = 14u | kHotFixWord << 24;   // straddles cut 16
        CHECK(!hotRowCheck(f.row));
    }
    {
        Fake f;
        f.fixups[0] = 4u | kHotFixBranch << 24;   // not a branch there
        CHECK(!hotRowCheck(f.row));
    }
    {
        Fake f;
        f.cuts[2] = 40;   // the last cut isn't the row's end
        CHECK(!hotRowCheck(f.row));
    }
    {
        Fake f;
        f.slot.value = kRow;   // a slot outside every table
        CHECK(!hotRowCheck(f.row));
    }
    {
        Fake f;
        f.fixups[1] = 8u | 7u << 24;   // unknown kind
        CHECK(!hotRowCheck(f.row));
    }
}
