// OpenTracker 2.x -- USB mass storage: Bulk-Only Transport + SCSI command set
// (USB Mass Storage Class Bulk-Only Transport 1.0; SCSI SPC-2 / SBC-2 subset).
//
// USB Storage mode (Tracker Mini manual §13.10, docs/PLAN_2X.md): the SD card
// is offered to the computer as one removable logical unit. Outside the mode
// the unit reports "medium not present" (NOT READY, ASC 3Ah), so the computer
// sees an empty card reader and can never write while FatFs has the card.
//
// Threading (device): service() runs in the USB interrupt (on every MSC token
// and every 1 ms frame, usb_msc.h) and never touches the SD card. Block reads
// and writes are handed to the main loop as one IoRequest at a time; the main
// loop runs them with runIo(). setMedium() / busy() / host*() are called from
// the main loop with the USB interrupt masked (src/hal/usb_storage.cpp). The
// host tests drive everything from one thread with fakes.
#pragma once

#include <cstdint>

namespace ot {
namespace msc {

// Bulk endpoint pair (device: third_party/teensy3/usb_msc.h). A segment is a
// whole buffer; at most 4 per direction are outstanding (queued, or done and
// not yet polled). Transmit segments that are not a multiple of 64 bytes end
// with a short packet.
class IEndpoints {
public:
    virtual ~IEndpoints() = default;
    virtual bool configured() const = 0;
    virtual bool rxQueue(uint8_t* buf, uint32_t len) = 0;
    virtual bool rxPoll(uint32_t* got) = 0;   // oldest receive segment done -> true (popped)
    virtual bool txQueue(const uint8_t* buf, uint32_t len) = 0;
    virtual bool txPoll() = 0;                // oldest transmit segment sent -> true (popped)
    // Disarm and drop every receive / transmit segment (complete or not).
    // Only while the host moves nothing on that pipe: halted, or between a
    // short packet that ended the data stage and the CSW.
    virtual void rxFlush() = 0;
    virtual void txFlush() = 0;
    virtual void stall(bool in) = 0;
    virtual bool stalled(bool in) const = 0;
};

// The card behind the unit: 512-byte blocks.
class IBlockDevice {
public:
    virtual ~IBlockDevice() = default;
    virtual bool read(uint32_t lba, uint8_t* dst, uint32_t count) = 0;
    virtual bool write(uint32_t lba, const uint8_t* src, uint32_t count) = 0;
};

// SCSI sense (fixed format, SPC-2 4.5.3).
struct Sense {
    uint8_t key = 0, asc = 0, ascq = 0;
};
namespace sense {
constexpr uint8_t kNoSense = 0x0, kNotReady = 0x2, kMediumError = 0x3, kIllegalRequest = 0x5,
                  kUnitAttention = 0x6;
constexpr uint8_t kAscInvalidOpcode = 0x20, kAscLbaOutOfRange = 0x21, kAscInvalidField = 0x24,
                  kAscMediumChanged = 0x28, kAscNoMedium = 0x3A, kAscUnrecoveredRead = 0x11,
                  kAscWriteFault = 0x0C, kAscSavingNotSupported = 0x39;
}  // namespace sense

// SCSI operation codes handled.
namespace op {
constexpr uint8_t kTestUnitReady = 0x00, kRequestSense = 0x03, kInquiry = 0x12, kModeSense6 = 0x1A,
                  kStartStopUnit = 0x1B, kPreventAllow = 0x1E, kReadFormatCapacities = 0x23,
                  kReadCapacity10 = 0x25, kRead10 = 0x28, kWrite10 = 0x2A, kVerify10 = 0x2F,
                  kSyncCache10 = 0x35, kModeSense10 = 0x5A, kServiceActionIn16 = 0x9E, kReportLuns = 0xA0;
constexpr uint8_t kSaReadCapacity16 = 0x10;   // SERVICE ACTION IN(16) service action
}

// Session statistics (diagnostics): failed commands by cause, phase errors,
// and where the computer's reads went on a FAT volume.
struct ErrorKind {
    uint8_t op = 0, key = 0, asc = 0, ascq = 0;
    uint16_t count = 0;
};
// Read regions: [0, fatStart) boot sector + reserved (FSINFO), [fatStart,
// fat2Start) the first FAT, [fat2Start, dataStart) the other FAT copies (and
// FAT12/16 root directory), [dataStart, end) clusters.
enum ReadRegion : uint8_t { kRegionReserved = 0, kRegionFat1, kRegionFat2, kRegionData, kRegions };

class Bot {
public:
    static constexpr uint32_t kBlock = 512;
    static constexpr uint32_t kCbwLen = 31, kCswLen = 13;
    static constexpr uint32_t kCbwSig = 0x43425355u, kCswSig = 0x53425355u;
    static constexpr uint8_t kStatusPassed = 0, kStatusFailed = 1, kStatusPhase = 2;
    // Small DMA-reachable area of the unit's own: CBW (64), CSW (16), short
    // replies and discarded host data (64).
    static constexpr uint32_t kSmallBytes = 144;
    // READ(10) / WRITE(10) stream through a ring of 4 KB chunks: one card
    // request (8 blocks) per chunk, up to kMaxChunks of them ahead of the
    // slower side (the endpoint driver queues 4 segments per direction).
    static constexpr uint32_t kChunk = 4096, kMaxChunks = 4;

    // buf: data buffer for READ(10) / WRITE(10) (>= kChunk bytes; split into
    // up to kMaxChunks chunks of kChunk, device: 16 KB = 4). It is touched only
    // while a medium is present (storage mode), so it may be a buffer the app
    // uses otherwise (device: the SD bounce buffer). small: kSmallBytes, used
    // all the time. Both must stay valid and DMA-reachable (device: SRAM_U).
    void init(IEndpoints* ep, uint8_t* buf, uint32_t bufBytes, uint8_t* small);
    // Main loop, no medium present and not busy(): lend / take back the data
    // buffer (device: the SD bounce buffer, the computer's only while it owns
    // the card). Null: none (no medium may be presented then).
    void setDataBuffer(uint8_t* buf, uint32_t bufBytes);

    // ---- USB side (device: USB interrupt) --------------------------------
    void service();
    void onReset();   // bus reset / SET_CONFIGURATION / Bulk-Only Mass Storage Reset
    // Give up on the command in flight (the computer stopped mid-transfer and
    // storage mode must end): both pipes halt until the host's reset recovery
    // and every queued segment is dropped (nothing points into the data
    // buffer any more); a block request in progress finishes, its result is
    // dropped.
    void abort();

    // ---- main loop -------------------------------------------------------
    // Present the card (blocks of 512 B) with UNIT ATTENTION "medium changed",
    // or remove it (NOT READY "medium not present" from the next command on).
    void setMedium(bool present, uint32_t blocks);
    bool mediumPresent() const { return present_; }
    // A command is between its CBW and its CSW, block I/O is pending, or a
    // receive into the data buffer is still queued in the endpoint driver.
    bool busy() const;
    // Runs the pending block request on dev (true when one ran).
    bool runIo(IBlockDevice& dev);
    bool ioPending() const { return io_.state == kIoPending; }

    // Host view since the last setMedium(true): the computer used the drive
    // (it read the capacity or any block), and it ejected it (START STOP UNIT
    // with LoEj; it then reads as "medium not present" until loaded again).
    bool hostAccessed() const { return accessed_; }
    bool hostEjected() const { return ejected_; }
    bool hostPrevented() const { return prevent_; }

    // Diagnostics / tests. The counters cover one session: setMedium(true)
    // starts them from zero (the NOT READY polls of a computer that sees an
    // empty reader outside storage mode are not counted into it).
    const Sense& sense() const { return sense_; }
    uint32_t commands() const { return commands_; }
    uint32_t bytesRead() const { return bytesRead_; }
    uint32_t bytesWritten() const { return bytesWritten_; }
    uint32_t errors() const { return errors_; }
    uint32_t phaseErrors() const { return phaseErrors_; }
    uint32_t readCommands() const { return readCmds_; }
    uint32_t smallReads() const { return smallReads_; }   // READ(10) of <= 8 blocks
    void resetStats();
    // Failed commands by opcode + sense, most frequent first (up to
    // kErrorKinds kinds; failures of further kinds count in otherErrors()).
    static constexpr int kErrorKinds = 8;
    int errorKinds(ErrorKind* out) const;
    uint32_t otherErrors() const { return otherErrors_; }
    // "00 2/3A/00 x12, 12 5/24/00 x3" (empty string when none); returns length.
    uint32_t formatErrors(char* out, uint32_t n) const;
    // Region bounds in blocks for readBlocks() (main loop, before presenting).
    void setReadRegions(uint32_t fatStart, uint32_t fat2Start, uint32_t dataStart);
    uint32_t readBlocks(ReadRegion r) const { return regionBlocks_[r]; }

    enum State : uint8_t {
        kIdle = 0,     // next CBW not yet queued
        kWaitCbw,      // CBW receive queued
        kDataIn,       // small reply going out
        kReadStream,   // READ(10): SD -> ring chunks -> USB
        kWriteStream,  // WRITE(10) / discard: USB -> ring chunks -> SD
        kWaitUnstall,  // data stage ended with a halted pipe; CSW once the host clears it
        kSendCsw,      // CSW queued
        kHalted,       // invalid CBW: both pipes halted until reset recovery (BOT 6.6.1)
    };
    State state() const { return state_; }

private:
    enum IoState : uint8_t { kIoIdle = 0, kIoPending, kIoOk, kIoFail };
    struct IoRequest {
        volatile uint8_t state = kIoIdle;
        bool write = false;
        uint32_t lba = 0, count = 0;
        uint8_t* buf = nullptr;
    };

    void startCommand(uint32_t got);
    void execute();
    // Reply of `len` bytes already in buf_ (command data direction In).
    void replyIn(uint32_t len);
    // No data stage expected by the command.
    void noData();
    // Data-out of `bytes` expected by the command; write = to the card at
    // lba_ (else received and discarded).
    void dataOut(uint32_t bytes, bool write);
    void dataIn(uint32_t bytes);   // READ(10) stream of `bytes` from lba_
    // Receive `total` bytes; the first `writeBytes` go to the card at lba_.
    void startWriteStream(uint32_t total, uint32_t writeBytes);
    void fail(uint8_t key, uint8_t asc, uint8_t ascq = 0);
    void finish();                 // data stage over: stall if needed, then CSW
    void sendCsw();
    bool unitReady();              // reports NOT READY / UNIT ATTENTION and fails when not
    bool mediaOk() const { return present_ && !ejected_; }
    void serviceRead();
    void serviceWrite();
    uint32_t chunkBytes(uint32_t k, uint32_t total) const;
    uint8_t* chunkBuf(uint32_t k) const { return chunk_ == kChunk ? buf_ + (k % ring_) * kChunk : reply_; }

    IEndpoints* ep_ = nullptr;
    uint8_t* buf_ = nullptr;
    uint32_t ring_ = 0;            // chunks of kChunk in buf_ (0: no data buffer)
    uint8_t* cbw_ = nullptr;       // 64 B
    uint8_t* csw_ = nullptr;       // 16 B
    uint8_t* reply_ = nullptr;     // 64 B: short replies, discarded data
    uint32_t chunk_ = 0;           // stream chunk: kChunk (card data) or 64 (discard)
    State state_ = kIdle;

    // Current command.
    uint32_t tag_ = 0, hostLen_ = 0;
    bool hostIn_ = false;
    uint8_t cb_[16] = {};
    uint8_t status_ = kStatusPassed;
    uint32_t residue_ = 0;
    uint32_t processed_ = 0;         // data-out bytes the command used (residue)
    bool stallIn_ = false, stallOut_ = false;
    bool cswQueued_ = false, outDirection_ = false;
    // Streams.
    uint32_t lba_ = 0, total_ = 0, writeBytes_ = 0;   // stream bytes
    uint32_t chunks_ = 0, issued_ = 0, ready_ = 0, sent_ = 0;   // chunk counters
    uint32_t xferred_ = 0;           // bytes moved over USB in this command
    bool ioError_ = false;
    IoRequest io_;
    volatile uint8_t rxData_ = 0;    // receive segments of a data stage queued in the driver
    // Reset / abort while the main loop ran a request: service() starts
    // nothing until that request is done (its buffer is not free before).
    volatile bool staleIo_ = false;

    // Unit.
    volatile bool present_ = false;
    volatile uint32_t blocks_ = 0;
    volatile bool unitAttention_ = false;
    volatile bool accessed_ = false, ejected_ = false, prevent_ = false;
    Sense sense_;
    uint32_t commands_ = 0, bytesRead_ = 0, bytesWritten_ = 0, errors_ = 0;
    uint32_t phaseErrors_ = 0, readCmds_ = 0, smallReads_ = 0, otherErrors_ = 0;
    ErrorKind kinds_[kErrorKinds];
    uint32_t regionStart_[kRegions] = {};   // first block of each region (0: region unused)
    uint32_t regionBlocks_[kRegions] = {};
    void countRead(uint32_t lba, uint32_t n);
};

// Main loop: gives up on a command in flight when the computer is gone (USB
// cable pulled, host crashed or asleep mid-transfer). Without it the unit
// would stay busy() forever: no re-presented card, a 3 s wait on exit.
class HostWatch {
public:
    static constexpr uint32_t kGoneMs = 500;
    // hostSeen: the computer is there (frames / tokens within the last
    // 100 ms, IUsbStorage::host() != kNoUsb). True (once per absence) when
    // the command in flight must be aborted now (Bot::abort()).
    bool update(bool hostSeen, bool busy, uint32_t nowMs) {
        if (hostSeen) {
            gone_ = fired_ = false;
            return false;
        }
        if (!gone_) {
            gone_ = true;
            since_ = nowMs;
        }
        if (!busy || fired_ || nowMs - since_ < kGoneMs) return false;
        fired_ = true;
        return true;
    }

private:
    uint32_t since_ = 0;
    bool gone_ = false, fired_ = false;
};

}  // namespace msc
}  // namespace ot
