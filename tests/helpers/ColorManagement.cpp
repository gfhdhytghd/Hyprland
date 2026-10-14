#include <helpers/cm/ColorManagement.hpp>
#include <render/types.hpp>
#include <gtest/gtest.h>

using namespace NColorManagement;

TEST(ColorManagement, GenericFilmChannelOrder) {
    const auto& p = getPrimaries(CM_PRIMARIES_GENERIC_FILM);
    EXPECT_DOUBLE_EQ(p.red.x, 0.681);
    EXPECT_DOUBLE_EQ(p.green.x, 0.243);
    EXPECT_DOUBLE_EQ(p.blue.x, 0.145);
}

TEST(ColorManagement, XYZBasisIsFiniteAndInvertible) {
    const auto xyz    = CPrimaries::from(CM_PRIMARIES_CIE1931_XYZ);
    const auto rgb    = CPrimaries::from(CM_PRIMARIES_SRGB);
    const auto matrix = xyz->toXYZ().mat();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            EXPECT_NEAR(matrix[i][j], i == j ? 1.0 : 0.0, 1e-6);
    const auto roundtrip = rgb->convertMatrix(xyz) * xyz->convertMatrix(rgb);
    const auto result    = roundtrip * Hyprgraphics::CColor::XYZ{.x = 0.2, .y = 0.4, .z = 0.6};
    EXPECT_NEAR(result.x, 0.2, 1e-6);
    EXPECT_NEAR(result.y, 0.4, 1e-6);
    EXPECT_NEAR(result.z, 0.6, 1e-6);
}
