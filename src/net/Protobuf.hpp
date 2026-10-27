#pragma once

// Minimal protobuf wire-format reader / writer. Enough for TikTok's webcast
// protocol, where we only need a handful of known fields; no generated code
// and no protobuf runtime dependency.

#include <cstdint>
#include <string>
#include <string_view>

namespace usc::pb {

enum Wire : uint32_t { Varint = 0, Fixed64 = 1, Bytes = 2, Fixed32 = 5 };

struct Field {
    uint32_t number = 0;
    uint32_t wire = 0;
    uint64_t varint = 0;     // Varint / Fixed32 / Fixed64 value
    std::string_view bytes;  // Bytes (string, sub-message, packed)
};

class Reader {
public:
    explicit Reader(std::string_view data) : d_(data) {}

    // Reads the next field; false at the end or on malformed input.
    bool next(Field& f) {
        if (pos_ >= d_.size()) return false;
        uint64_t key;
        if (!readVarint(key)) return fail();
        f.number = static_cast<uint32_t>(key >> 3);
        f.wire = static_cast<uint32_t>(key & 7);
        f.bytes = {};
        f.varint = 0;
        switch (f.wire) {
        case Varint:
            return readVarint(f.varint) || fail();
        case Fixed64:
            if (d_.size() - pos_ < 8) return fail();
            for (int i = 7; i >= 0; --i) f.varint = (f.varint << 8) | static_cast<uint8_t>(d_[pos_ + i]);
            pos_ += 8;
            return true;
        case Fixed32:
            if (d_.size() - pos_ < 4) return fail();
            for (int i = 3; i >= 0; --i) f.varint = (f.varint << 8) | static_cast<uint8_t>(d_[pos_ + i]);
            pos_ += 4;
            return true;
        case Bytes: {
            uint64_t len;
            if (!readVarint(len) || len > d_.size() - pos_) return fail();
            f.bytes = d_.substr(pos_, static_cast<size_t>(len));
            pos_ += static_cast<size_t>(len);
            return true;
        }
        default:
            return fail(); // groups (3/4) are not used
        }
    }

    bool ok() const { return !failed_; }

private:
    bool readVarint(uint64_t& v) {
        v = 0;
        for (int shift = 0; shift < 64 && pos_ < d_.size(); shift += 7) {
            uint8_t b = static_cast<uint8_t>(d_[pos_++]);
            v |= static_cast<uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return true;
        }
        return false;
    }
    bool fail() {
        failed_ = true;
        pos_ = d_.size();
        return false;
    }

    std::string_view d_;
    size_t pos_ = 0;
    bool failed_ = false;
};

class Writer {
public:
    Writer& varint(uint32_t field, uint64_t v) {
        key(field, Varint);
        raw(v);
        return *this;
    }
    Writer& bytes(uint32_t field, std::string_view s) {
        key(field, Bytes);
        raw(s.size());
        out_.append(s.data(), s.size());
        return *this;
    }
    const std::string& str() const { return out_; }

private:
    void key(uint32_t field, uint32_t wire) { raw((static_cast<uint64_t>(field) << 3) | wire); }
    void raw(uint64_t v) {
        while (v >= 0x80) {
            out_.push_back(static_cast<char>((v & 0x7F) | 0x80));
            v >>= 7;
        }
        out_.push_back(static_cast<char>(v));
    }
    std::string out_;
};

} // namespace usc::pb
