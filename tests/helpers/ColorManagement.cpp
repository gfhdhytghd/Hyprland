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

TEST(ColorManagement, BT1886DisplayCharacteristic) {
    const auto          source = CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_BT1886});
    const auto          linear = CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_LINEAR});
    Render::SCMSettings settings{.srcTFRange = {.min = 0.01f, .max = 100.0f}};
    const auto          decoded = convertColor(RGBAColor{{.r = 0.05, .g = 0.0, .b = 1.0, .a = 1.0}}, source, linear, settings);
    const auto          black   = std::pow(0.01, 1.0 / 2.4);
    const auto          white   = std::pow(100.0, 1.0 / 2.4);
    EXPECT_NEAR(decoded.c.r, std::pow(0.05 * (white - black) + black, 2.4), 1e-6);
    EXPECT_NEAR(decoded.c.g, 0.01, 1e-6);
    EXPECT_NEAR(decoded.c.b, 100.0, 1e-6);
    settings.dstTFRange = settings.srcTFRange;
    const auto encoded  = convertColor(decoded, linear, source, settings);
    EXPECT_NEAR(encoded.c.r, 0.05, 1e-6);
    EXPECT_NEAR(encoded.c.g, 0.0, 1e-6);
    EXPECT_NEAR(encoded.c.b, 1.0, 1e-6);
}

TEST(ColorManagement, PQPrimaryRangeDoesNotAdvertiseDisplayPeak) {
    SImageDescription description{
        .transferFunction = CM_TRANSFER_FUNCTION_ST2084_PQ, .luminances = {.min = 0, .max = 630, .reference = 203}, .masteringLuminances = {.min = 0, .max = 630}};
    const auto pq = CImageDescription::from(description);
    EXPECT_EQ(pq->value().luminances.max, 10000U);
    EXPECT_EQ(pq->value().masteringLuminances.max, 630U);
    description.luminances.max = 4000;
    EXPECT_EQ(CImageDescription::from(description), pq);
}

TEST(ColorManagement, WindowsPresetsCarryHDRHeadroom) {
    for (const auto description : {SCRGB_IMAGE_DESCRIPTION, BT2100_IMAGE_DESCRIPTION}) {
        EXPECT_EQ(description->value().luminances.min, 0);
        EXPECT_EQ(description->value().luminances.max, 10000U);
        EXPECT_EQ(description->value().luminances.reference, 203U);
    }
    EXPECT_EQ(SCRGB_IMAGE_DESCRIPTION->value().getTFMaxLuminance(), 80);
}
