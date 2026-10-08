#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace deltacast {

// Limited-range Y'CbCr -> RGB coefficients, scaled by 256 (R+=rv*V, G-=gu*U+gv*V, B+=bu*U).
struct YuvMatrix { int rv, gu, gv, bu; };
inline constexpr YuvMatrix BT601{409, 100, 208, 516};
inline constexpr YuvMatrix BT709{459, 55, 136, 541};

// SD video is BT.601, HD (720 lines and up) is BT.709. The recorder tags its files by the same rule.
inline bool IsHdHeight(int height) { return height >= 720; }
inline const YuvMatrix& MatrixForHeight(int height) { return IsHdHeight(height) ? BT709 : BT601; }

inline uint8_t clamp8(int x) {
    return (uint8_t)(x < 0 ? 0 : x>255 ? 255 : x);
}

// UYVY (U Y0 V Y1) -> BGRA, optional vertical flip
inline void UYVY_to_BGRA(const uint8_t* src, int srcPitch, uint8_t* dst, int dstPitch, int W, int H,
                         bool flipY, const YuvMatrix& m)
{
    for (int y = 0; y < H; ++y) {
        const uint8_t* s = src + size_t(y) * srcPitch;
        // write to bottom-up row if flipping
        uint8_t* d = dst + size_t(flipY ? (H - 1 - y) : y) * dstPitch;

        for (int x = 0; x < W; x += 2) {
            int U = int(s[0]) - 128;
            int Y0 = int(s[1]);
            int V = int(s[2]) - 128;
            int Y1 = int(s[3]);
            s += 4;

            auto emit = [&](int Y) {
                int C = Y - 16; if (C < 0) C = 0;
                *d++ = clamp8((298 * C + m.bu * U + 128) >> 8);
                *d++ = clamp8((298 * C - m.gu * U - m.gv * V + 128) >> 8);
                *d++ = clamp8((298 * C + m.rv * V + 128) >> 8);
                *d++ = 255; // A
                };

            emit(Y0);
            emit(Y1);
        }
    }
}

// Bob deinterlacing: every row whose parity is not keptParity becomes the average of its two
// neighbours; an edge row copies its single neighbour. Rows of keptParity are not touched.
inline void BobFillMissingRows(uint8_t* img, int W, int H, int pitch, int keptParity)
{
    const size_t rowBytes = size_t(W) * 4;
    for (int y = 0; y < H; ++y) {
        if ((y & 1) == keptParity) continue;

        int upY = y - 1;
        int downY = y + 1;
        if (upY < 0) upY = downY;
        if (downY >= H) downY = upY;
        if (upY < 0 || upY >= H) continue; // single-row image: no neighbour

        uint8_t* out = img + size_t(y) * pitch;
        const uint8_t* up = img + size_t(upY) * pitch;
        const uint8_t* down = img + size_t(downY) * pitch;

        if (upY == downY) {
            std::memcpy(out, up, rowBytes);
        }
        else {
            for (size_t i = 0; i < rowBytes; ++i) {
                out[i] = uint8_t((int(up[i]) + int(down[i])) >> 1);
            }
        }
    }
}

// Convert one packed UYVY field to a full-height BGRA frame using bob
// deinterlacing. Field lines are copied to their natural parity and the missing
// lines are interpolated from the nearest captured lines. The SDK returns field
// lines contiguously; totalBytes may describe either the field payload or a
// full-frame-sized slot allocation, so derive padding only for a half-frame
// payload and otherwise use the natural UYVY row size.
inline bool UYVY_Field_to_BGRA_Bob(const uint8_t* src,
                                   size_t totalBytes,
                                   uint8_t* dst,
                                   int dstPitch,
                                   int W,
                                   int H,
                                   bool evenField,
                                   bool flipY,
                                   const YuvMatrix& m)
{
    if (!src || !dst || W <= 0 || H <= 0 || (W & 1) != 0) return false;

    // Field 1/odd carries display rows 0,2,4...; field 2/even carries
    // display rows 1,3,5... (zero-based row coordinates).
    const int sourceParity = evenField ? 1 : 0;
    const int fieldRows = (H - sourceParity + 1) / 2;
    if (fieldRows <= 0) return false;

    const size_t rowBytes = size_t(W) * 2; // UYVY 4:2:2 8-bit
    const size_t minimumFieldBytes = rowBytes * size_t(fieldRows);
    if (totalBytes < minimumFieldBytes) return false;

    size_t srcPitch = rowBytes;
    const size_t minimumFrameBytes = rowBytes * size_t(H);
    if (totalBytes < minimumFrameBytes && totalBytes % size_t(fieldRows) == 0) {
        const size_t candidatePitch = totalBytes / size_t(fieldRows);
        if (candidatePitch >= rowBytes) srcPitch = candidatePitch;
    }

    if ((size_t(fieldRows - 1) * srcPitch) + rowBytes > totalBytes) return false;

    // Convert every real field line into its full-frame row.
    for (int fieldY = 0; fieldY < fieldRows; ++fieldY) {
        const int sourceFrameY = fieldY * 2 + sourceParity;
        const int outputY = flipY ? (H - 1 - sourceFrameY) : sourceFrameY;

        UYVY_to_BGRA(src + size_t(fieldY) * srcPitch, int(srcPitch),
                     dst + size_t(outputY) * dstPitch, dstPitch, W, 1, false, m);
    }

    // Vertical flipping reverses line parity when the output height is even.
    const int outputFieldParity = flipY ? ((H - 1 - sourceParity) & 1) : sourceParity;
    BobFillMissingRows(dst, W, H, dstPitch, outputFieldParity);
    return true;
}

inline constexpr uint8_t DIGITS_3x5[10][5] = {
    {0b111, 0b101, 0b101, 0b101, 0b111}, // 0
    {0b010, 0b110, 0b010, 0b010, 0b111}, // 1
    {0b111, 0b001, 0b111, 0b100, 0b111}, // 2
    {0b111, 0b001, 0b111, 0b001, 0b111}, // 3
    {0b101, 0b101, 0b111, 0b001, 0b001}, // 4
    {0b111, 0b100, 0b111, 0b001, 0b111}, // 5
    {0b111, 0b100, 0b111, 0b101, 0b111}, // 6
    {0b111, 0b001, 0b001, 0b001, 0b001}, // 7
    {0b111, 0b101, 0b111, 0b101, 0b111}, // 8
    {0b111, 0b101, 0b111, 0b001, 0b111}  // 9
};

inline void SetPixelBGRA(uint8_t* img, int W, int H, int pitch, int x, int y,
    uint8_t b, uint8_t g, uint8_t r, uint8_t a = 255)
{
    if (x < 0 || y < 0 || x >= W || y >= H) return;

    uint8_t* p = img + size_t(y) * pitch + size_t(x) * 4;
    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = a;
}

inline void FillRectBGRA(uint8_t* img, int W, int H, int pitch,
    int x0, int y0, int rw, int rh,
    uint8_t b, uint8_t g, uint8_t r, uint8_t a = 255)
{
    for (int y = y0; y < y0 + rh; ++y) {
        for (int x = x0; x < x0 + rw; ++x) {
            SetPixelBGRA(img, W, H, pitch, x, y, b, g, r, a);
        }
    }
}

inline void DrawDigit3x5BGRA(uint8_t* img, int W, int H, int pitch,
    int digit, int x0, int y0, int scale,
    uint8_t b, uint8_t g, uint8_t r,
    bool flipTextY)
{
    if (digit < 0 || digit > 9) return;

    for (int row = 0; row < 5; ++row) {
        int srcRow = flipTextY ? (4 - row) : row;
        uint8_t bits = DIGITS_3x5[digit][srcRow];

        for (int col = 0; col < 3; ++col) {
            bool on = (bits & (1 << (2 - col))) != 0;
            if (!on) continue;

            for (int sy = 0; sy < scale; ++sy) {
                for (int sx = 0; sx < scale; ++sx) {
                    SetPixelBGRA(
                        img, W, H, pitch,
                        x0 + col * scale + sx,
                        y0 + row * scale + sy,
                        b, g, r, 255
                    );
                }
            }
        }
    }
}

inline void BurnFrameNumberBGRA(uint8_t* img,
    int imageW,
    int imageH,
    int pitch,
    unsigned long long frameNo,
    int regionX,
    int regionW,
    bool bufferIsVerticallyFlipped)
{
    if (!img || imageW <= 0 || imageH <= 0 || pitch <= 0 || regionW <= 0) {
        return;
    }

    const int scale = 2;
    const int gap = scale;
    const int margin = 4;

    std::string text = std::to_string(frameNo);

    int digitW = 3 * scale;
    int digitH = 5 * scale;
    int textW = int(text.size()) * digitW + int(text.size() - 1) * gap;
    int textH = digitH;

    int x = regionX + regionW - textW - margin;

    // If the buffer is vertically flipped before Unity displays it,
    // native bottom becomes visible top.
    int y = bufferIsVerticallyFlipped
        ? imageH - margin - textH
        : margin;

    FillRectBGRA(img, imageW, imageH, pitch,
        x - 2, y - 2,
        textW + 4, textH + 4,
        0, 0, 0, 255);

    int cx = x;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            DrawDigit3x5BGRA(
                img, imageW, imageH, pitch,
                c - '0',
                cx, y,
                scale,
                255, 255, 255,
                bufferIsVerticallyFlipped
            );
        }

        cx += digitW + gap;
    }
}

}
