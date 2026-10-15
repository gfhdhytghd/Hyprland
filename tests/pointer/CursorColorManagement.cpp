#include <gtest/gtest.h>

#include <pointer/PointerManager.hpp>
#include <pointer/CursorBuffer.hpp>
#include <helpers/cm/ColorManagement.hpp>
#include <limits>

using namespace NColorManagement;

static PImageDescription pqOutput() {
    return CImageDescription::from(SImageDescription{
        .transferFunction = CM_TRANSFER_FUNCTION_ST2084_PQ,
        .primariesNameSet = true,
        .primariesNamed   = CM_PRIMARIES_BT2020,
        .luminances       = {.min = 0, .max = 10000, .reference = 203},
    });
}

TEST(CursorColorManagement, SRGBCursorCopiedOntoPQPlaneIsRejected) {
    // raw sRGB white copied unchanged would be PQ code 1.0, i.e. 10000 nits
    EXPECT_FALSE(Pointer::cpuCursorCopyIsExact(DEFAULT_SRGB_IMAGE_DESCRIPTION, pqOutput(), true, false));
}

TEST(CursorColorManagement, MatchingDescriptionsCopyExactly) {
    EXPECT_TRUE(Pointer::cpuCursorCopyIsExact(DEFAULT_SRGB_IMAGE_DESCRIPTION, DEFAULT_SRGB_IMAGE_DESCRIPTION, true, false));
    EXPECT_TRUE(Pointer::cpuCursorCopyIsExact(pqOutput(), pqOutput(), true, false));
}

TEST(CursorColorManagement, CMCursorSurfaceIsComparedToTheOutput) {
    // a cursor surface already in the output's encoding needs no conversion, but in sRGB it would
    EXPECT_TRUE(Pointer::cpuCursorCopyIsExact(pqOutput(), pqOutput(), true, false));
    EXPECT_FALSE(Pointer::cpuCursorCopyIsExact(pqOutput(), DEFAULT_SRGB_IMAGE_DESCRIPTION, true, false));
}

TEST(CursorColorManagement, FollowsTheRendererWhenItSkipsCM) {
    // the GPU cursor path would not convert either, so the copy matches it
    EXPECT_TRUE(Pointer::cpuCursorCopyIsExact(DEFAULT_SRGB_IMAGE_DESCRIPTION, pqOutput(), false, false));
    EXPECT_TRUE(Pointer::cpuCursorCopyIsExact(DEFAULT_SRGB_IMAGE_DESCRIPTION, pqOutput(), true, true));
}

TEST(CursorColorManagement, ReadbackPreservesRowsAlphaAndPaddedStride) {
    const std::array<uint8_t, 16> rgba        = {0x20, 0x10, 0x08, 0x40, 0, 0, 0, 0, 0x12, 0x34, 0x56, 0xFF, 0x80, 0x40, 0x20, 0x80};
    std::array<uint8_t, 24>       destination = {};
    destination.fill(0xCD);
    ASSERT_TRUE(Pointer::packCursorARGB8888(rgba, destination, 2, 2, 12));
    const std::array<uint8_t, 16> expected = {0x08, 0x10, 0x20, 0x40, 0, 0, 0, 0, 0x56, 0x34, 0x12, 0xFF, 0x20, 0x40, 0x80, 0x80};
    for (size_t y = 0; y < 2; ++y) {
        for (size_t x = 0; x < 8; ++x)
            EXPECT_EQ(destination[y * 12 + x], expected[y * 8 + x]);
        for (size_t padding = 8; padding < 12; ++padding)
            EXPECT_EQ(destination[y * 12 + padding], 0xCD);
    }
}

TEST(CursorColorManagement, EncodedPQWhiteReadbackIsNotPremultipliedTwice) {
    // Expected shader-encoded values: 203-nit reference white is PQ 0.580689,
    // rounded to 148/255 (opaque) or 74/255 (half alpha).
    const std::array<uint8_t, 12> rgba        = {148, 148, 148, 255, 74, 74, 74, 128, 0, 0, 0, 0};
    std::array<uint8_t, 12>       destination = {};
    ASSERT_TRUE(Pointer::packCursorARGB8888(rgba, destination, 3, 1, 12));
    EXPECT_EQ(destination, rgba);
}

TEST(CursorColorManagement, InvalidReadbackLayoutDoesNotWrite) {
    const std::array<uint8_t, 16> rgba        = {};
    std::array<uint8_t, 16>       destination = {};
    destination.fill(0xCD);
    EXPECT_FALSE(Pointer::packCursorARGB8888(rgba, destination, 2, 2, 4));
    EXPECT_FALSE(Pointer::packCursorARGB8888(rgba, destination, 2, 2, 12));
    EXPECT_FALSE(Pointer::packCursorARGB8888(std::span{rgba}.first(15), destination, 2, 2, 8));
    EXPECT_FALSE(Pointer::packCursorARGB8888(rgba, destination, 0, 2, 8));
    EXPECT_FALSE(Pointer::packCursorARGB8888(rgba, destination, 2, 0, 8));
    EXPECT_FALSE(Pointer::packCursorARGB8888(rgba, destination, std::numeric_limits<size_t>::max(), 2, 8));
    EXPECT_FALSE(Pointer::packCursorARGB8888(rgba, destination, 2, std::numeric_limits<size_t>::max(), 8));
    for (const auto byte : destination)
        EXPECT_EQ(byte, 0xCD);
}
