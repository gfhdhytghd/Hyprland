#include <gtest/gtest.h>

#include <pointer/PointerManager.hpp>
#include <helpers/cm/ColorManagement.hpp>

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
