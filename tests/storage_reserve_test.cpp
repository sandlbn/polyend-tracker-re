// SD streaming C14b: IStorage::reserve() (a file about to be written to its
// full size: contiguous on the card, refused when it cannot fit),
// IStorage::truncate() and IStorage::freeBytes(), on the test fake with a
// card capacity.
#include "test.h"
#include "ui_fakes.h"
#include "mem_storage.h"

#include <cstdint>
#include <vector>

using ot::IStorage;

TEST(fake_storage_full_card_fails_write_and_reports_free) {
    FakeStorage fs;
    CHECK(fs.freeBytes() == UINT64_MAX);   // no capacity set: unknown
    fs.capacity = 1000;
    fs.addFile("a.bin", std::vector<uint8_t>(300));
    CHECK_EQ(fs.freeBytes(), uint64_t(700));
    IStorage::Handle h = fs.open("b.bin", IStorage::kWrite);
    CHECK(h >= 0);
    CHECK(!fs.reserve(h, 800));            // does not fit
    CHECK(fs.reserve(h, 600));
    CHECK_EQ(int(fs.reserves.size()), 2);
    // Allocated at once, like the device (f_expand opt 1).
    CHECK_EQ(fs.files["b.bin"].size(), size_t(600));
    CHECK_EQ(fs.freeBytes(), uint64_t(100));
    std::vector<uint8_t> buf(500, 7);
    CHECK_EQ(fs.write(h, buf.data(), 500), 500);   // inside the reserved run
    CHECK_EQ(fs.freeBytes(), uint64_t(100));
    CHECK(fs.truncate(h));                          // the unwritten rest goes
    CHECK_EQ(fs.files["b.bin"].size(), size_t(500));
    CHECK_EQ(fs.freeBytes(), uint64_t(200));
    CHECK(fs.write(h, buf.data(), 500) < 500);   // card full: short write
    fs.close(h);
}

TEST(storage_defaults_without_a_card_model) {
    // A storage that does not override them: reserve accepts, free is unknown.
    MemStorage m;
    CHECK(m.reserve(0, 123456789u));
    CHECK(m.truncate(0));
    CHECK(m.freeBytes() == UINT64_MAX);
}
