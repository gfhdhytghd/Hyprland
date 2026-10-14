#include <helpers/cm/ColorManagement.hpp>
#include <render/types.hpp>
#include <gtest/gtest.h>
#include <drm_mode.h>

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

TEST(ColorManagement, HLGReferenceDisplayOOTF) {
    const auto hlg = CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_HLG, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    const auto linear =
        CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_LINEAR, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    Render::SCMSettings settings{.srcTFRange = {.min = 0, .max = 1000}};
    const auto          result = convertColor(RGBAColor{{.r = 0.75, .g = 0.75, .b = 0.75, .a = 1}}, hlg, linear, settings);
    EXPECT_NEAR(result.c.r, 203.152, 0.01);
    EXPECT_NEAR(result.c.g, result.c.r, 1e-6);
    settings.dstTFRange = settings.srcTFRange;
    const auto encoded  = convertColor(result, linear, hlg, settings);
    EXPECT_NEAR(encoded.c.r, 0.75, 1e-6);
}

TEST(ColorManagement, EncodingRangeHonorsBlackAndRelativePeak) {
    SImageDescription d{.transferFunction = CM_TRANSFER_FUNCTION_GAMMA22, .luminances = {.min = 0.4f, .max = 120, .reference = 100}};
    EXPECT_FLOAT_EQ(d.encodingMinLuminance(), 0.4f);
    EXPECT_FLOAT_EQ(d.encodingMaxLuminance(), 120);
    d.transferFunction = CM_TRANSFER_FUNCTION_ST2084_PQ;
    EXPECT_FLOAT_EQ(d.encodingMaxLuminance(), 10000.4f);
    d.transferFunction = CM_TRANSFER_FUNCTION_EXT_LINEAR;
    EXPECT_FLOAT_EQ(d.encodingMinLuminance(), 0);
    EXPECT_FLOAT_EQ(d.encodingMaxLuminance(), 80);
}

TEST(ColorManagement, ReferenceWhiteScaleAndWindowsException) {
    SImageDescription source{.luminances = {.min = 0, .max = 10000, .reference = 446}};
    SImageDescription target{.luminances = {.min = 0, .max = 10000, .reference = 203}};
    EXPECT_NEAR(referenceLuminanceScale(source, target) * 446, 203, 1e-5);
    source.isWindows = true;
    EXPECT_FLOAT_EQ(referenceLuminanceScale(source, target), 1);
    source.isWindows            = false;
    source.luminances.reference = 0;
    EXPECT_FLOAT_EQ(referenceLuminanceScale(source, target), 1);
}

TEST(ColorManagement, HDRMetadataUsesMasteringGamutAndPreservesUnknownLightLevels) {
    SImageDescription d{.transferFunction    = CM_TRANSFER_FUNCTION_ST2084_PQ,
                        .primariesNameSet    = true,
                        .primariesNamed      = CM_PRIMARIES_BT2020,
                        .masteringPrimaries  = NColorPrimaries::DISPLAY_P3,
                        .masteringLuminances = {.min = 0.005f, .max = 1000}};
    const auto        metadata = createHDRMetadata(d).hdmi_metadata_type1;
    EXPECT_EQ(metadata.eotf, 2);
    EXPECT_EQ(metadata.display_primaries[0].x, 34000);
    EXPECT_EQ(metadata.max_display_mastering_luminance, 1000);
    EXPECT_EQ(metadata.min_display_mastering_luminance, 50);
    EXPECT_EQ(metadata.max_cll, 0);
    EXPECT_EQ(metadata.max_fall, 0);
    d.maxCLL = 100000;
    EXPECT_EQ(createHDRMetadata(d).hdmi_metadata_type1.max_cll, 65535);
}
