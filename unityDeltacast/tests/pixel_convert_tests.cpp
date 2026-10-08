#include "../pixel_convert.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace deltacast;

static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

// One UYVY pair (2 pixels) -> BGRA of the first pixel.
static std::array<int, 3> rgb(int Y, int Cb, int Cr, const YuvMatrix& m) {
    const uint8_t src[4] = { uint8_t(Cb), uint8_t(Y), uint8_t(Cr), uint8_t(Y) };
    uint8_t dst[8] = {};
    UYVY_to_BGRA(src, 4, dst, 8, 2, 1, false, m);
    return { dst[2], dst[1], dst[0] };
}

static void color_tests() {
    auto near = [](std::array<int, 3> got, std::array<int, 3> want) {
        for (int i = 0; i < 3; ++i) if (got[i] < want[i] - 2 || got[i] > want[i] + 2) return false;
        return true;
    };
    // 100% bars, 8-bit limited range
    check(near(rgb(63, 102, 240, BT709), { 255, 0, 0 }), "BT.709 red");
    check(near(rgb(173, 42, 26, BT709), { 0, 255, 0 }), "BT.709 green");
    check(near(rgb(81, 90, 240, BT601), { 255, 0, 0 }), "BT.601 red");
    check(rgb(63, 102, 240, BT601)[0] < 240, "BT.601 must differ on HD red");
    check(near(rgb(235, 128, 128, BT709), { 255, 255, 255 }) && near(rgb(16, 128, 128, BT709), { 0, 0, 0 }),
          "white and black");
    check(&MatrixForHeight(576) == &BT601 && &MatrixForHeight(720) == &BT709 &&
          &MatrixForHeight(1080) == &BT709, "matrix by height");

    // vertical flip of a full frame
    const uint8_t src[8] = { 128, 16, 128, 16,   128, 235, 128, 235 };
    uint8_t dst[16] = {};
    UYVY_to_BGRA(src, 4, dst, 8, 2, 2, true, BT709);
    check(dst[0] == 255 && dst[8] == 0, "flipY row order");
}

// Field rows hold gray levels A (first field line) and B (second); returns the gray value
// (blue channel of pixel 0) of each of the 4 output rows.
static const int A = 51, B = 214, AVG = (A + B) >> 1; // from Y=60 and Y=200

static std::array<int, 4> bob(bool evenField, bool flipY, size_t srcPitch, size_t totalBytes) {
    std::vector<uint8_t> src(totalBytes, 0x77); // 0x77 = padding garbage
    const uint8_t rows[2] = { 60, 200 };
    for (int r = 0; r < 2; ++r) {
        uint8_t* s = src.data() + r * srcPitch;
        s[0] = 128; s[1] = rows[r]; s[2] = 128; s[3] = rows[r];
    }
    std::vector<uint8_t> dst(2 * 4 * 4, 0);
    check(UYVY_Field_to_BGRA_Bob(src.data(), totalBytes, dst.data(), 8, 2, 4, evenField, flipY, BT709),
          "bob conversion rejected");
    return { dst[0], dst[8], dst[16], dst[24] };
}

static void bob_tests() {
    using R = std::array<int, 4>;
    check(bob(false, false, 4, 8) == R{ A, AVG, B, B }, "odd field");
    check(bob(false, true, 4, 8) == R{ B, B, AVG, A }, "odd field flipped");
    check(bob(true, false, 4, 8) == R{ A, A, AVG, B }, "even field");
    check(bob(true, true, 4, 8) == R{ B, AVG, A, A }, "even field flipped");
    check(bob(false, false, 4, 16) == R{ A, AVG, B, B }, "full-frame-sized slot");
    check(bob(false, false, 6, 12) == R{ A, AVG, B, B }, "padded field rows");

    std::vector<uint8_t> src(8), dst(32);
    check(!UYVY_Field_to_BGRA_Bob(src.data(), 7, dst.data(), 8, 2, 4, false, false, BT709), "short field");
    check(!UYVY_Field_to_BGRA_Bob(src.data(), 8, dst.data(), 12, 3, 4, false, false, BT709), "odd width");

    uint8_t one[4] = { 1, 2, 3, 4 };
    BobFillMissingRows(one, 1, 1, 4, 1); // single row, nothing to interpolate from
    check(one[0] == 1 && one[3] == 4, "single-row bob");
}

static void burn_in_tests() {
    const int W = 8, H = 8, pitch = W * 4, guard = 16;
    std::vector<uint8_t> buffer(guard + H * pitch + guard, 0xAB);
    uint8_t* img = buffer.data() + guard;
    BurnFrameNumberBGRA(img, W, H, pitch, 1234567890ull, 0, W, true);
    for (int i = 0; i < guard; ++i)
        check(buffer[i] == 0xAB && buffer[buffer.size() - 1 - i] == 0xAB, "burn-in wrote outside the image");
    bool drew = false;
    for (int i = 0; i < H * pitch; ++i) drew |= img[i] != 0xAB;
    check(drew, "burn-in drew nothing");
}

int main() {
    try {
        color_tests();
        bob_tests();
        burn_in_tests();
        std::cout << "pixel convert tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
