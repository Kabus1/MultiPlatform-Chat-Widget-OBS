#pragma once

// Frame exchange between the app (one writer) and "USC Virtual Camera"
// (one reader inside every program that opens the camera: OBS, TikTok
// Studio, a browser...). On Windows the region is a named, pagefile-backed
// file mapping in the session namespace; the protocol itself only needs a
// block of memory, so it is unit-tested everywhere.
//
// Layout: header (4 KiB) + kSlots frame slots of packed NV12. The writer
// fills the slot after the newest one and publishes it; each slot carries a
// sequence number (odd while being written) so a reader never uses a frame
// that was overwritten while it copied it (seqlock). No locks, no waiting:
// a slow camera user can never slow the writer down.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace usc::vcam {

constexpr wchar_t kMappingName[] = L"Local\\USCVirtualCamera_v1";
constexpr char kFriendlyName[] = "USC Virtual Camera";
constexpr uint32_t kMagic = 0x4D414356; // "VCAM"
constexpr uint32_t kVersion = 1;
constexpr int kMaxWidth = 3840;
constexpr int kMaxHeight = 2160;
constexpr int kSlots = 3;
constexpr size_t kSlotBytes = size_t(kMaxWidth) * kMaxHeight * 3 / 2;
constexpr size_t kHeaderBytes = 4096;
constexpr size_t kRegionBytes = kHeaderBytes + kSlots * kSlotBytes;
// A writer that has not delivered a frame for this long counts as gone:
// readers then show the "no signal" picture.
constexpr int64_t kWriterTimeoutMs = 1500;

static_assert(std::atomic<uint64_t>::is_always_lock_free, "needs lock-free 64-bit atomics (address-free)");
static_assert(std::atomic<int64_t>::is_always_lock_free, "needs lock-free 64-bit atomics (address-free)");

struct alignas(64) SlotInfo {
    std::atomic<uint64_t> seq;   // odd while the writer fills the slot
    std::atomic<uint32_t> width;
    std::atomic<uint32_t> height;
    std::atomic<uint64_t> frameNumber;
    std::atomic<int64_t> timestamp100ns;
};

struct alignas(64) Header {
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> version;
    std::atomic<uint32_t> writerPid;      // 0 = no writer
    std::atomic<int64_t> lastWriteMs;     // writer's monotonic clock (shared by all processes)
    std::atomic<int32_t> latest;          // newest complete slot, -1 none
    std::atomic<uint32_t> width, height;  // current output format (what the camera advertises)
    std::atomic<uint32_t> fpsNum, fpsDen;
    std::atomic<uint64_t> frameCounter;
    SlotInfo slots[kSlots];
};
static_assert(sizeof(Header) <= kHeaderBytes, "header must fit its page");

inline Header* header(void* base) { return static_cast<Header*>(base); }
inline const Header* header(const void* base) { return static_cast<const Header*>(base); }
inline uint8_t* slotData(void* base, int i) { return static_cast<uint8_t*>(base) + kHeaderBytes + size_t(i) * kSlotBytes; }
inline const uint8_t* slotData(const void* base, int i) {
    return static_cast<const uint8_t*>(base) + kHeaderBytes + size_t(i) * kSlotBytes;
}

// ------------------------------------------------------------------ writer

class FrameWriter {
public:
    explicit FrameWriter(void* base) : base_(base) {}

    // Prepares the region. Safe on a region that readers already map (a
    // restarted app re-uses the existing mapping).
    void init(uint32_t pid) {
        Header* h = header(base_);
        if (h->magic.load() != kMagic || h->version.load() != kVersion) {
            std::memset(base_, 0, kHeaderBytes);
            h->latest.store(-1);
        }
        h->version.store(kVersion);
        h->magic.store(kMagic);
        h->writerPid.store(pid);
    }

    void setFormat(uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen) {
        Header* h = header(base_);
        h->width.store(width);
        h->height.store(height);
        h->fpsNum.store(fpsNum);
        h->fpsDen.store(fpsDen ? fpsDen : 1);
    }

    // Slot to fill next (never the newest one, which readers are using).
    uint8_t* begin(int width, int height) {
        if (width <= 0 || height <= 0 || width > kMaxWidth || height > kMaxHeight) return nullptr;
        Header* h = header(base_);
        int32_t latest = h->latest.load(std::memory_order_acquire);
        slot_ = (latest + 1 + kSlots) % kSlots;
        SlotInfo& s = h->slots[slot_];
        s.seq.fetch_add(1, std::memory_order_acq_rel); // odd: being written
        std::atomic_thread_fence(std::memory_order_release);
        s.width.store(uint32_t(width), std::memory_order_relaxed);
        s.height.store(uint32_t(height), std::memory_order_relaxed);
        return slotData(base_, slot_);
    }

    void commit(int64_t timestamp100ns, int64_t nowMs) {
        Header* h = header(base_);
        SlotInfo& s = h->slots[slot_];
        uint64_t n = h->frameCounter.load(std::memory_order_relaxed) + 1;
        s.frameNumber.store(n, std::memory_order_relaxed);
        s.timestamp100ns.store(timestamp100ns, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        s.seq.fetch_add(1, std::memory_order_release); // even: complete
        h->latest.store(slot_, std::memory_order_release);
        h->frameCounter.store(n, std::memory_order_release);
        h->lastWriteMs.store(nowMs, std::memory_order_release);
    }

    void detach() {
        Header* h = header(base_);
        h->writerPid.store(0);
        h->lastWriteMs.store(0);
    }

private:
    void* base_;
    int slot_ = 0;
};

// ------------------------------------------------------------------ reader

class FrameReader {
public:
    explicit FrameReader(const void* base) : base_(base) {}

    bool valid() const {
        const Header* h = header(base_);
        return h->magic.load() == kMagic && h->version.load() == kVersion;
    }

    bool writerAlive(int64_t nowMs) const {
        const Header* h = header(base_);
        if (!valid() || h->writerPid.load() == 0) return false;
        int64_t last = h->lastWriteMs.load(std::memory_order_acquire);
        return last > 0 && nowMs - last <= kWriterTimeoutMs && nowMs >= last - 1000;
    }

    // Advertised format; false if there is none yet.
    bool format(uint32_t& width, uint32_t& height, uint32_t& fpsNum, uint32_t& fpsDen) const {
        const Header* h = header(base_);
        if (!valid()) return false;
        width = h->width.load();
        height = h->height.load();
        fpsNum = h->fpsNum.load();
        fpsDen = h->fpsDen.load();
        return width > 0 && height > 0 && width <= uint32_t(kMaxWidth) && height <= uint32_t(kMaxHeight) && fpsNum > 0;
    }

    uint64_t frameCounter() const { return header(base_)->frameCounter.load(std::memory_order_acquire); }

    // Copies the newest frame (packed NV12) into dst. False if there is no
    // frame, it does not fit, or it kept changing while being copied.
    bool readLatest(uint8_t* dst, size_t capacity, int& width, int& height, uint64_t& frameNumber) const {
        const Header* h = header(base_);
        if (!valid()) return false;
        for (int attempt = 0; attempt < 3; ++attempt) {
            int32_t i = h->latest.load(std::memory_order_acquire);
            if (i < 0 || i >= kSlots) return false;
            const SlotInfo& s = h->slots[i];
            uint64_t s1 = s.seq.load(std::memory_order_acquire);
            if (s1 & 1) continue;
            uint32_t w = s.width.load(std::memory_order_relaxed), ht = s.height.load(std::memory_order_relaxed);
            uint64_t n = s.frameNumber.load(std::memory_order_relaxed);
            if (w == 0 || ht == 0 || w > uint32_t(kMaxWidth) || ht > uint32_t(kMaxHeight)) return false;
            size_t bytes = size_t(w) * ht + 2 * size_t((w + 1) / 2) * ((ht + 1) / 2);
            if (bytes > capacity) return false;
            // May overlap a writer that wraps around to this slot (a slow
            // reader): the sequence re-check below then discards the copy.
            // Race detectors flag this overlap; it is the seqlock's design.
            std::memcpy(dst, slotData(base_, i), bytes);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s.seq.load(std::memory_order_relaxed) != s1) continue; // overwritten meanwhile: retry
            width = int(w);
            height = int(ht);
            frameNumber = n;
            return true;
        }
        return false;
    }

private:
    const void* base_;
};

} // namespace usc::vcam
