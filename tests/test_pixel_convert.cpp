/* White-box unit tests of image/pixel_convert.hpp: YUV->BGRA color matrices, bob
 * deinterlacing of single fields, and frame-number burn-in (header-only, no device). */

#include "image/pixel_convert.hpp"
#include "doctest.h"

#include <array>
#include <vector>

using namespace deltacast;

namespace
{
// One UYVY pair (2 pixels) -> R, G, B of the first pixel.
std::array<int, 3> Rgb(int Y, int Cb, int Cr, const YuvMatrix& m)
{
    const uint8_t src[4] = { uint8_t(Cb), uint8_t(Y), uint8_t(Cr), uint8_t(Y) };
    uint8_t dst[8] = {};
    UYVY_to_BGRA(src, 4, dst, 8, 2, 1, false, m);
    return { dst[2], dst[1], dst[0] };
}

bool Near(std::array<int, 3> got, std::array<int, 3> want)
{
    for (int i = 0; i < 3; ++i)
        if (got[i] < want[i] - 2 || got[i] > want[i] + 2) return false;
    return true;
}

// Gray levels of the two field lines (from Y=60 and Y=200) and their average.
constexpr int A = 51, B = 214, AVG = (A + B) >> 1;

// Converts a 2x4 frame from one field of two lines (A then B) and returns the gray
// value (blue of pixel 0) of each output row.
std::array<int, 4> Bob(bool evenField, bool flipY, size_t srcPitch, size_t totalBytes)
{
    std::vector<uint8_t> src(totalBytes, 0x77); // 0x77 = padding garbage
    const uint8_t rows[2] = { 60, 200 };
    for (int r = 0; r < 2; ++r)
    {
        uint8_t* s = src.data() + r * srcPitch;
        s[0] = 128; s[1] = rows[r]; s[2] = 128; s[3] = rows[r];
    }
    std::vector<uint8_t> dst(2 * 4 * 4, 0);
    REQUIRE(UYVY_Field_to_BGRA_Bob(src.data(), totalBytes, dst.data(), 8, 2, 4, evenField, flipY, BT709));
    return { dst[0], dst[8], dst[16], dst[24] };
}
}

TEST_CASE("YUV to RGB uses the matrix of the video standard")
{
    // 100% color bars, 8-bit limited range
    CHECK(Near(Rgb(63, 102, 240, BT709), { 255, 0, 0 }));
    CHECK(Near(Rgb(173, 42, 26, BT709), { 0, 255, 0 }));
    CHECK(Near(Rgb(81, 90, 240, BT601), { 255, 0, 0 }));
    CHECK(Rgb(63, 102, 240, BT601)[0] < 240); // SD matrix visibly wrong on HD red
    CHECK(Near(Rgb(235, 128, 128, BT709), { 255, 255, 255 }));
    CHECK(Near(Rgb(16, 128, 128, BT709), { 0, 0, 0 }));

    CHECK(&MatrixForHeight(576) == &BT601);
    CHECK(&MatrixForHeight(720) == &BT709);
    CHECK(&MatrixForHeight(1080) == &BT709);
}

TEST_CASE("UYVY_to_BGRA flips rows")
{
    const uint8_t src[8] = { 128, 16, 128, 16,   128, 235, 128, 235 };
    uint8_t dst[16] = {};
    UYVY_to_BGRA(src, 4, dst, 8, 2, 2, true, BT709);
    CHECK(dst[0] == 255);
    CHECK(dst[8] == 0);
}

TEST_CASE("bob places each field on its parity and interpolates the other")
{
    using Rows = std::array<int, 4>;
    CHECK(Bob(false, false, 4, 8) == Rows{ A, AVG, B, B });
    CHECK(Bob(false, true, 4, 8) == Rows{ B, B, AVG, A });
    CHECK(Bob(true, false, 4, 8) == Rows{ A, A, AVG, B });
    CHECK(Bob(true, true, 4, 8) == Rows{ B, AVG, A, A });
    CHECK(Bob(false, false, 4, 16) == Rows{ A, AVG, B, B }); // full-frame-sized slot
    CHECK(Bob(false, false, 6, 12) == Rows{ A, AVG, B, B }); // padded field rows
}

TEST_CASE("bob rejects buffers it cannot read safely")
{
    std::vector<uint8_t> src(8), dst(32);
    CHECK_FALSE(UYVY_Field_to_BGRA_Bob(src.data(), 7, dst.data(), 8, 2, 4, false, false, BT709));
    CHECK_FALSE(UYVY_Field_to_BGRA_Bob(src.data(), 8, dst.data(), 12, 3, 4, false, false, BT709));

    uint8_t one[4] = { 1, 2, 3, 4 };
    BobFillMissingRows(one, 1, 1, 4, 1); // single row, nothing to interpolate from
    CHECK(one[0] == 1);
    CHECK(one[3] == 4);
}

TEST_CASE("burn-in stays inside the image")
{
    const int W = 8, H = 8, pitch = W * 4, guard = 16;
    std::vector<uint8_t> buffer(guard + H * pitch + guard, 0xAB);
    uint8_t* img = buffer.data() + guard;
    BurnFrameNumberBGRA(img, W, H, pitch, 1234567890ull, 0, W, true);
    for (int i = 0; i < guard; ++i)
    {
        CHECK(buffer[i] == 0xAB);
        CHECK(buffer[buffer.size() - 1 - i] == 0xAB);
    }
    bool drew = false;
    for (int i = 0; i < H * pitch; ++i) drew |= img[i] != 0xAB;
    CHECK(drew);
}
