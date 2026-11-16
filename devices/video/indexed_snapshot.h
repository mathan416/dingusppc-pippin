// SPDX-License-Identifier: GPL-3.0-or-later
// Owned Taos pixels. No SDL, guest callbacks, or guest pointers survive capture.
#ifndef PIPPIN_INDEXED_SNAPSHOT_H
#define PIPPIN_INDEXED_SNAPSHOT_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

namespace pippin_indexed {

enum class Format { indexed8, taos15_be };

class Snapshot {
public:
    static std::unique_ptr<const Snapshot> capture(
        Format format, int width, int height, const uint8_t* allocation,
        size_t allocation_bytes, size_t source_offset, int source_pitch,
        const uint32_t* palette) {
        if (!allocation || !palette || width <= 0 || height <= 0 ||
            width > std::numeric_limits<int>::max() / 4 || source_pitch <= 0 ||
            (format != Format::indexed8 && format != Format::taos15_be))
            return nullptr;
        const size_t bpp = format == Format::indexed8 ? 1 : 2;
        const size_t row = size_t(width) * bpp;
        if (size_t(source_pitch) < row || source_offset > allocation_bytes ||
            row > allocation_bytes - source_offset)
            return nullptr;
        // Division proves the final visible row fits without overflowing.
        if (size_t(height - 1) > (allocation_bytes - source_offset - row) / size_t(source_pitch) ||
            size_t(height) > std::numeric_limits<size_t>::max() / row)
            return nullptr;
        std::unique_ptr<Snapshot> frame(new Snapshot(format, width, height, row));
        // This allocation is deliberately uninitialized. Every packed byte is
        // filled before the immutable object can leave the guest owner.
        for (int y = 0; y < height; ++y)
            std::memcpy(frame->raw_.get() + size_t(y) * row,
                        allocation + source_offset + size_t(y) * size_t(source_pitch), row);
        std::memcpy(frame->palette_.data(), palette, sizeof(frame->palette_));
        return std::unique_ptr<const Snapshot>(frame.release());
    }

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    size_t raw_bytes() const noexcept { return row_bytes_ * size_t(height_); }

    // Output is native-endian ARGB8888, exactly matching FB_WRITE. Reject an
    // invalid destination before writing any byte. Row padding is untouched.
    bool write_argb(uint8_t* dst, size_t dst_bytes, int dst_pitch) const noexcept {
        const size_t row = size_t(width_) * 4;
        if (!dst || dst_pitch <= 0 || size_t(dst_pitch) < row || dst_bytes < row ||
            size_t(height_ - 1) > (dst_bytes - row) / size_t(dst_pitch))
            return false;
        for (int y = 0; y < height_; ++y) {
            const uint8_t* src = raw_.get() + size_t(y) * row_bytes_;
            uint8_t* out = dst + size_t(y) * size_t(dst_pitch);
            for (int x = 0; x < width_; ++x) {
                uint32_t pixel;
                if (format_ == Format::indexed8) {
                    pixel = palette_[src[x]];
                } else {
                    const uint16_t c = (uint16_t(src[size_t(x) * 2]) << 8) | src[size_t(x) * 2 + 1];
                    pixel = (palette_[(c >> 10) & 31] & 0x00ff0000u) |
                            (palette_[(c >> 5) & 31] & 0x0000ff00u) |
                            (palette_[c & 31] & 0xff0000ffu);
                }
                std::memcpy(out + size_t(x) * 4, &pixel, sizeof(pixel));
            }
        }
        return true;
    }

private:
    Snapshot(Format format, int width, int height, size_t row)
        : format_(format), width_(width), height_(height), row_bytes_(row),
          raw_(new uint8_t[row * size_t(height)]) {}
    Format format_;
    int width_, height_;
    size_t row_bytes_;
    std::unique_ptr<uint8_t[]> raw_;
    std::array<uint32_t, 256> palette_;
};

} // namespace pippin_indexed
#endif
