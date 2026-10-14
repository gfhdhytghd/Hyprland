#include <protocols/types/SurfaceState.hpp>
#include <gtest/gtest.h>

using namespace NColorManagement;

TEST(ColorManagementState, CommitsOnlyWhenUpdated) {
    SSurfaceState current, pending;
    current.imageDescription = DEFAULT_SRGB_IMAGE_DESCRIPTION;
    pending.imageDescription = DEFAULT_HDR_IMAGE_DESCRIPTION;
    current.updateFrom(pending);
    EXPECT_EQ(current.imageDescription, DEFAULT_SRGB_IMAGE_DESCRIPTION);
    pending.updated.bits.colorManagement = true;
    current.updateFrom(pending);
    EXPECT_EQ(current.imageDescription, DEFAULT_HDR_IMAGE_DESCRIPTION);
    pending.reset();
    EXPECT_FALSE(pending.updated.bits.colorManagement);
}

TEST(ColorManagementState, CachedSubsurfaceStatePreservesDescriptionAndUnset) {
    SSurfaceState cached, pending;
    pending.imageDescription             = DEFAULT_HDR_IMAGE_DESCRIPTION;
    pending.updated.bits.colorManagement = true;
    cached.mergeFrom(pending);
    EXPECT_EQ(cached.imageDescription, DEFAULT_HDR_IMAGE_DESCRIPTION);
    EXPECT_TRUE(cached.updated.bits.colorManagement);
    pending.imageDescription = {};
    cached.mergeFrom(pending);
    EXPECT_FALSE(cached.imageDescription);
}
