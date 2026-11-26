#pragma once

// Pixel work shared by the app (writer) and the virtual camera DLL (reader).
// Portable and allocation-free, so it is unit-tested on every platform.
//
// "Packed" images have no row padding: NV12/I420 stride == width,
// YUY2 stride == 2 * width. Widths and heights are even.

#include <cstddef>
#include <cstdint>

namespace usc::vcam {

enum class PixelFormat : uint32_t { NV12 = 1, YUY2 = 2, I420 = 3 };

// Bytes of a packed image.
size_t frameBytes(PixelFormat fmt, int width, int height);

// NV12 with any row pitch / UV-plane offset (as a capture driver delivers
// it) -> packed NV12.
void packNV12(const uint8_t* src, int pitch, size_t uvOffset, int width, int height, uint8_t* dst);
// YUY2 with any row pitch -> packed NV12.
void yuy2ToNV12(const uint8_t* src, int pitch, int width, int height, uint8_t* dst);

// Packed NV12 -> packed NV12 of another size: plain copy, exact 2:1 box
// filter (e.g. 4K -> 1080p), or bilinear otherwise.
void scaleNV12(const uint8_t* src, int srcW, int srcH, uint8_t* dst, int dstW, int dstH);

// Packed NV12 -> packed `fmt` of the same size.
void convertFromNV12(const uint8_t* nv12, int width, int height, PixelFormat fmt, uint8_t* dst);

// Packed NV12 (BT.709 limited range) -> 24-bit BGR rows (for previews).
void nv12ToBgr24(const uint8_t* nv12, int width, int height, uint8_t* bgr);

// "No signal" picture (dark slate) in `fmt`.
void fillPlaceholder(PixelFormat fmt, int width, int height, uint8_t* dst);

} // namespace usc::vcam
