#include "vcam/VcamFrame.hpp"

#include <algorithm>
#include <cstring>

namespace usc::vcam {

size_t frameBytes(PixelFormat fmt, int width, int height) {
    if (width <= 0 || height <= 0) return 0;
    const size_t w = size_t(width), h = size_t(height);
    switch (fmt) {
    case PixelFormat::YUY2: return w * h * 2;
    case PixelFormat::NV12:
    case PixelFormat::I420: return w * h + 2 * ((w + 1) / 2) * ((h + 1) / 2);
    }
    return 0;
}

void packNV12(const uint8_t* src, int pitch, size_t uvOffset, int width, int height, uint8_t* dst) {
    const size_t w = size_t(width), h = size_t(height), cw = (w + 1) / 2 * 2, ch = (h + 1) / 2;
    if (size_t(pitch) == w && uvOffset == w * h) {
        std::memcpy(dst, src, w * h + cw * ch);
        return;
    }
    for (size_t y = 0; y < h; ++y) std::memcpy(dst + y * w, src + y * size_t(pitch), w);
    uint8_t* uv = dst + w * h;
    for (size_t y = 0; y < ch; ++y) std::memcpy(uv + y * cw, src + uvOffset + y * size_t(pitch), cw);
}

void yuy2ToNV12(const uint8_t* src, int pitch, int width, int height, uint8_t* dst) {
    const size_t w = size_t(width), h = size_t(height);
    uint8_t* yPlane = dst;
    uint8_t* uv = dst + w * h;
    for (size_t y = 0; y < h; ++y) {
        const uint8_t* row = src + y * size_t(pitch);
        uint8_t* yRow = yPlane + y * w;
        for (size_t x = 0; x < w; x += 2) { // Y0 U Y1 V
            yRow[x] = row[x * 2];
            if (x + 1 < w) yRow[x + 1] = row[x * 2 + 2];
        }
        if (y % 2 == 0) { // chroma from the even row (and the odd one below, averaged)
            const uint8_t* next = y + 1 < h ? row + pitch : row;
            uint8_t* uvRow = uv + (y / 2) * ((w + 1) / 2 * 2);
            for (size_t x = 0; x < w; x += 2) {
                uvRow[x] = uint8_t((row[x * 2 + 1] + next[x * 2 + 1] + 1) / 2);
                uvRow[x + 1] = uint8_t((row[x * 2 + 3] + next[x * 2 + 3] + 1) / 2);
            }
        }
    }
}

namespace {

// Bilinear resampling of one plane with `channels` interleaved components.
void bilinear(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh, int channels) {
    const int64_t xStep = (int64_t(sw) << 16) / dw, yStep = (int64_t(sh) << 16) / dh;
    for (int y = 0; y < dh; ++y) {
        int64_t fy = std::max<int64_t>(0, (y * yStep) + yStep / 2 - 32768);
        int y0 = int(fy >> 16), wy = int(fy & 0xFFFF) >> 8;
        int y1 = std::min(y0 + 1, sh - 1);
        y0 = std::min(y0, sh - 1);
        const uint8_t* r0 = src + size_t(y0) * size_t(sw) * channels;
        const uint8_t* r1 = src + size_t(y1) * size_t(sw) * channels;
        uint8_t* out = dst + size_t(y) * size_t(dw) * channels;
        for (int x = 0; x < dw; ++x) {
            int64_t fx = std::max<int64_t>(0, (x * xStep) + xStep / 2 - 32768);
            int x0 = int(fx >> 16), wx = int(fx & 0xFFFF) >> 8;
            int x1 = std::min(x0 + 1, sw - 1);
            x0 = std::min(x0, sw - 1);
            for (int c = 0; c < channels; ++c) {
                int a = r0[x0 * channels + c], b = r0[x1 * channels + c];
                int d = r1[x0 * channels + c], e = r1[x1 * channels + c];
                int top = a * (256 - wx) + b * wx, bottom = d * (256 - wx) + e * wx;
                out[x * channels + c] = uint8_t((top * (256 - wy) + bottom * wy + 32768) >> 16);
            }
        }
    }
}

// Exact 2:1 downscale (2x2 box) of a plane with `channels` interleaved components.
void half(const uint8_t* src, int sw, uint8_t* dst, int dw, int dh, int channels) {
    for (int y = 0; y < dh; ++y) {
        const uint8_t* r0 = src + size_t(2 * y) * size_t(sw) * channels;
        const uint8_t* r1 = r0 + size_t(sw) * channels;
        uint8_t* out = dst + size_t(y) * size_t(dw) * channels;
        for (int x = 0; x < dw; ++x)
            for (int c = 0; c < channels; ++c) {
                int i = 2 * x * channels + c;
                out[x * channels + c] = uint8_t((r0[i] + r0[i + channels] + r1[i] + r1[i + channels] + 2) / 4);
            }
    }
}

} // namespace

void scaleNV12(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
    if (sw == dw && sh == dh) {
        std::memcpy(dst, src, frameBytes(PixelFormat::NV12, sw, sh));
        return;
    }
    const uint8_t* srcUV = src + size_t(sw) * size_t(sh);
    uint8_t* dstUV = dst + size_t(dw) * size_t(dh);
    const int scw = (sw + 1) / 2, sch = (sh + 1) / 2, dcw = (dw + 1) / 2, dch = (dh + 1) / 2;
    if (sw == 2 * dw && sh == 2 * dh && scw == 2 * dcw && sch == 2 * dch) {
        half(src, sw, dst, dw, dh, 1);
        half(srcUV, scw, dstUV, dcw, dch, 2);
        return;
    }
    bilinear(src, sw, sh, dst, dw, dh, 1);
    bilinear(srcUV, scw, sch, dstUV, dcw, dch, 2);
}

void convertFromNV12(const uint8_t* nv12, int width, int height, PixelFormat fmt, uint8_t* dst) {
    const size_t w = size_t(width), h = size_t(height), cw = (w + 1) / 2, ch = (h + 1) / 2;
    const uint8_t* yPlane = nv12;
    const uint8_t* uv = nv12 + w * h;
    switch (fmt) {
    case PixelFormat::NV12:
        std::memcpy(dst, nv12, frameBytes(fmt, width, height));
        return;
    case PixelFormat::I420: {
        std::memcpy(dst, yPlane, w * h);
        uint8_t* u = dst + w * h;
        uint8_t* v = u + cw * ch;
        for (size_t i = 0; i < cw * ch; ++i) {
            u[i] = uv[2 * i];
            v[i] = uv[2 * i + 1];
        }
        return;
    }
    case PixelFormat::YUY2:
        for (size_t y = 0; y < h; ++y) {
            const uint8_t* yRow = yPlane + y * w;
            const uint8_t* uvRow = uv + (y / 2) * cw * 2;
            uint8_t* out = dst + y * w * 2;
            for (size_t x = 0; x < w; x += 2) {
                out[x * 2] = yRow[x];
                out[x * 2 + 1] = uvRow[x];
                out[x * 2 + 2] = x + 1 < w ? yRow[x + 1] : yRow[x];
                out[x * 2 + 3] = uvRow[x + 1];
            }
        }
        return;
    }
}

void nv12ToBgr24(const uint8_t* nv12, int width, int height, uint8_t* bgr) {
    const size_t w = size_t(width), h = size_t(height), cw = (w + 1) / 2 * 2;
    const uint8_t* uv = nv12 + w * h;
    auto clamp = [](int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); };
    for (size_t y = 0; y < h; ++y) {
        const uint8_t* yRow = nv12 + y * w;
        const uint8_t* uvRow = uv + (y / 2) * cw;
        uint8_t* out = bgr + y * w * 3;
        for (size_t x = 0; x < w; ++x) {
            // BT.709, limited -> full range, 8.8 fixed point.
            int c = (int(yRow[x]) - 16) * 298;
            int d = int(uvRow[x & ~size_t(1)]) - 128, e = int(uvRow[(x & ~size_t(1)) + 1]) - 128;
            out[x * 3 + 0] = clamp((c + 541 * d + 128) >> 8);           // B
            out[x * 3 + 1] = clamp((c - 55 * d - 136 * e + 128) >> 8);  // G
            out[x * 3 + 2] = clamp((c + 459 * e + 128) >> 8);           // R
        }
    }
}

void fillPlaceholder(PixelFormat fmt, int width, int height, uint8_t* dst) {
    // Dark slate (BT.709 limited range): Y 40, Cb 136, Cr 124.
    const uint8_t Y = 40, U = 136, V = 124;
    const size_t w = size_t(width), h = size_t(height), cw = (w + 1) / 2, ch = (h + 1) / 2;
    switch (fmt) {
    case PixelFormat::NV12:
        std::memset(dst, Y, w * h);
        for (size_t i = 0; i < cw * ch; ++i) {
            dst[w * h + 2 * i] = U;
            dst[w * h + 2 * i + 1] = V;
        }
        return;
    case PixelFormat::I420:
        std::memset(dst, Y, w * h);
        std::memset(dst + w * h, U, cw * ch);
        std::memset(dst + w * h + cw * ch, V, cw * ch);
        return;
    case PixelFormat::YUY2:
        for (size_t i = 0; i < w * h / 2; ++i) {
            dst[4 * i] = Y;
            dst[4 * i + 1] = U;
            dst[4 * i + 2] = Y;
            dst[4 * i + 3] = V;
        }
        return;
    }
}

} // namespace usc::vcam
