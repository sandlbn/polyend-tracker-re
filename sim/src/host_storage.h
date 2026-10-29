// OpenTracker simulator -- IStorage backed by a host directory acting as the
// SD card root. Lookups are case-insensitive per path component, like FAT;
// new names keep the case they were created with.
#pragma once

#include <cstdio>
#include <string>

#include "hal_api.h"

namespace sim {

class HostStorage final : public ot::IStorage {
public:
    static constexpr int kMaxOpen = 32;
    ~HostStorage() override;
    HostStorage() = default;
    HostStorage(const HostStorage&) = delete;
    HostStorage& operator=(const HostStorage&) = delete;
    // Creates the root directory if needed. Returns false if it is unusable.
    bool init(const std::string& root);
    const std::string& root() const { return root_; }

    Handle open(const char* path, OpenMode mode) override;
    int32_t read(Handle h, void* dst, uint32_t len) override;
    int32_t write(Handle h, const void* src, uint32_t len) override;
    bool seek(Handle h, uint32_t pos) override;
    int32_t size(Handle h) override;
    void close(Handle h) override;
    bool exists(const char* path) override;
    bool mkdir(const char* path) override;
    bool remove(const char* path) override;
    bool rename(const char* from, const char* to) override;
    int32_t listDir(const char* path, DirFn fn, void* ctx) override;

    // USB Storage mode (SimUsbStorage): the "computer" owns the card; every
    // app access is refused and counted.
    void setLocked(bool on) { locked_ = on; }
    bool locked() const { return locked_; }
    int lockedAccess() const { return lockedAccess_; }

private:
    bool refuse() {
        if (locked_) ++lockedAccess_;
        return locked_;
    }
    bool locked_ = false;
    int lockedAccess_ = 0;
    // Returns an owned parent directory descriptor and a leaf name. Each
    // directory is opened without following symlinks, relative to the root.
    int resolve(const char* path, std::string& leaf, bool* existsOut = nullptr) const;
    int rootFd_ = -1;
    std::FILE* get(Handle h) const { return h >= 0 && h < kMaxOpen ? files_[h] : nullptr; }

    std::string root_;
    std::FILE* files_[kMaxOpen] = {};
};

}  // namespace sim
