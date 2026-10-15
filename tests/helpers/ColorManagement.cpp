#include <helpers/cm/ColorManagement.hpp>
#include <render/types.hpp>
#include <gtest/gtest.h>
#include <drm_mode.h>

using namespace NColorManagement;

static Render::SCMSettings conversionSettings(PImageDescription source, PImageDescription target, Render::STFRange srcRange, Render::STFRange dstRange) {
    return {
        .sourceTF      = source->value().transferFunction,
        .targetTF      = target->value().transferFunction,
        .srcTFRange    = srcRange,
        .dstTFRange    = dstRange,
        .convertMatrix = Hyprgraphics::CMatrix3{source->getPrimaries()->convertMatrix(target->getPrimaries())}.mat(),
    };
}

TEST(ColorManagement, GenericFilmChannelOrder) {
    const auto& p = getPrimaries(CM_PRIMARIES_GENERIC_FILM);
    EXPECT_DOUBLE_EQ(p.red.x, 0.681);
    EXPECT_DOUBLE_EQ(p.green.x, 0.243);
    EXPECT_DOUBLE_EQ(p.blue.x, 0.145);
}

TEST(ColorManagement, XYZBasisIsFiniteAndInvertible) {
    const auto xyz    = CPrimaries::from(CM_PRIMARIES_CIE1931_XYZ);
    const auto rgb    = CPrimaries::from(CM_PRIMARIES_SRGB);
    const auto matrix = Hyprgraphics::CMatrix3{xyz->toXYZ()}.mat();
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
    const auto source  = CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_BT1886});
    const auto linear  = CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_LINEAR});
    const auto decoded = convertColor(RGBAColor{{.r = 0.05, .g = 0.0, .b = 1.0, .a = 1.0}}, source, linear, conversionSettings(source, linear, {.min = 0.01f, .max = 100.0f}, {}));
    const auto black   = std::pow(0.01, 1.0 / 2.4);
    const auto white   = std::pow(100.0, 1.0 / 2.4);
    EXPECT_NEAR(decoded.c.r, std::pow(0.05 * (white - black) + black, 2.4), 1e-6);
    EXPECT_NEAR(decoded.c.g, 0.01, 1e-6);
    EXPECT_NEAR(decoded.c.b, 100.0, 1e-6);
    const auto encoded = convertColor(decoded, linear, source, conversionSettings(linear, source, {}, {.min = 0.01f, .max = 100.0f}));
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
    for (const auto& description : {SCRGB_IMAGE_DESCRIPTION, BT2100_IMAGE_DESCRIPTION}) {
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
    // 75% HLG is BT.2100 reference white: 203 cd/m² on a 1000 cd/m² display with gamma 1.2.
    const auto result = convertColor(RGBAColor{{.r = 0.75, .g = 0.75, .b = 0.75, .a = 1}}, hlg, linear, conversionSettings(hlg, linear, {.min = 0, .max = 1000}, {}));
    EXPECT_NEAR(result.c.r, 203.152, 0.01);
    EXPECT_NEAR(result.c.g, result.c.r, 1e-6);
    const auto encoded = convertColor(result, linear, hlg, conversionSettings(linear, hlg, {}, {.min = 0, .max = 1000}));
    EXPECT_NEAR(encoded.c.r, 0.75, 1e-6);
}

TEST(ColorManagement, HLGSystemGammaAndBlackLiftFollowTheDisplay) {
    const auto hlg = CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_HLG, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    const auto linear =
        CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_LINEAR, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    // BT.2100: signal 0 is displayed at Lb, signal 1 at Lw.
    const Render::STFRange range = {.min = 0.05f, .max = 2000};
    const auto             black = convertColor(RGBAColor{{.r = 0, .g = 0, .b = 0, .a = 1}}, hlg, linear, conversionSettings(hlg, linear, range, {}));
    const auto             white = convertColor(RGBAColor{{.r = 1, .g = 1, .b = 1, .a = 1}}, hlg, linear, conversionSettings(hlg, linear, range, {}));
    EXPECT_NEAR(black.c.r, 0.05, 1e-4);
    EXPECT_NEAR(white.c.r, 2000, 1e-2);
    // gamma = 1.2 + 0.42 log10(2000 / 1000); 50% signal is scene light 1/12.
    const auto mid = convertColor(RGBAColor{{.r = 0.5, .g = 0.5, .b = 0.5, .a = 1}}, hlg, linear, conversionSettings(hlg, linear, {.min = 0, .max = 2000}, {}));
    EXPECT_NEAR(mid.c.r, 2000 * std::pow(1.0 / 12.0, 1.2 + 0.42 * std::log10(2.0)), 1e-3);
    const auto roundtrip = convertColor(mid, linear, hlg, conversionSettings(linear, hlg, {}, {.min = 0, .max = 2000}));
    EXPECT_NEAR(roundtrip.c.r, 0.5, 1e-6);
}

TEST(ColorManagement, EncodingRangeHonorsBlackAndRelativePeak) {
    SImageDescription d{.transferFunction = CM_TRANSFER_FUNCTION_GAMMA22, .luminances = {.min = 0.4f, .max = 120, .reference = 100}};
    EXPECT_FLOAT_EQ(d.encodingMinLuminance(), 0.4f);
    EXPECT_FLOAT_EQ(d.encodingMaxLuminance(), 120);
    d.transferFunction = CM_TRANSFER_FUNCTION_ST2084_PQ;
    EXPECT_FLOAT_EQ(d.encodingMaxLuminance(), 10000.4f);
    d.transferFunction = CM_TRANSFER_FUNCTION_EXT_LINEAR;
    EXPECT_FLOAT_EQ(d.encodingMinLuminance(), 0.4f);
    EXPECT_FLOAT_EQ(d.encodingMaxLuminance(), 120);
    EXPECT_FLOAT_EQ(d.getTFRefLuminance(), 80);
    d.isInternal = true;
    EXPECT_FLOAT_EQ(d.encodingMinLuminance(), 0);
    EXPECT_FLOAT_EQ(d.encodingMaxLuminance(), 80);
    d.isInternal = false;
    d.isWindows  = true;
    EXPECT_FLOAT_EQ(d.getTFRefLuminance(), 203);
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

TEST(ColorManagement, SDRBrightnessIsAppliedInLinearLight) {
    const auto source                = DEFAULT_SRGB_IMAGE_DESCRIPTION;
    auto       settings              = conversionSettings(source, source, {.min = 0, .max = 80}, {.min = 0, .max = 80});
    settings.needsSDRmod             = true;
    settings.sdrBrightnessMultiplier = 0.5f;
    const auto result                = convertColor(RGBAColor{{.r = 1, .g = 1, .b = 1, .a = 1}}, source, source, settings);
    EXPECT_NEAR(result.c.r, 0.735356983, 1e-6);
}

TEST(ColorManagement, MatchingPQEncodingsStillNeedPeakCompression) {
    SImageDescription source{
        .transferFunction = CM_TRANSFER_FUNCTION_ST2084_PQ, .luminances = {.min = 0, .max = 10000, .reference = 203}, .masteringLuminances = {.min = 0, .max = 1000}};
    auto target                    = source;
    target.masteringLuminances.max = 630;
    const auto destination         = CImageDescription::from(target);
    EXPECT_TRUE(CImageDescription::from(source)->needsCM(destination));
    source.maxCLL = 400;
    EXPECT_FALSE(CImageDescription::from(source)->needsCM(destination));
}

TEST(ColorManagement, BT2390MapsSourcePeakToTargetPeak) {
    const auto pq = CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_ST2084_PQ, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    const auto linear =
        CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_LINEAR, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    auto settings             = conversionSettings(pq, linear, {.min = 0, .max = 10000}, {.min = 0, .max = 10000});
    settings.needsTonemap     = true;
    settings.maxLuminance     = 1000;
    settings.dstMaxLuminance  = 600;
    settings.dstPrimaries2XYZ = Hyprgraphics::CMatrix3{linear->getPrimaries()->toXYZ()}.mat();
    const auto nits           = [&](double luminance) {
        const auto signal =
            std::pow((0.8359375 + 18.8515625 * std::pow(luminance / 10000.0, 0.1593017578125)) / (1.0 + 18.6875 * std::pow(luminance / 10000.0, 0.1593017578125)), 78.84375);
        return convertColor(RGBAColor{{.r = signal, .g = signal, .b = signal, .a = 1}}, pq, linear, settings).c.g;
    };
    // The knee leaves dark content alone, the source peak lands on the target peak and the curve is monotonic.
    EXPECT_NEAR(nits(10), 10, 0.05);
    EXPECT_NEAR(nits(1000), 600, 0.5);
    EXPECT_LT(nits(500), nits(800));
    EXPECT_LT(nits(800), nits(1000));
}

TEST(ColorManagement, OutOfGamutColorsAreDesaturatedTowardsWhite) {
    const auto bt2020 =
        CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_GAMMA22, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    const auto srgb   = CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_LINEAR, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_SRGB});
    const auto mapped = convertColor(RGBAColor{{.r = 0, .g = 1, .b = 0, .a = 1}}, bt2020, srgb, conversionSettings(bt2020, srgb, {.min = 0, .max = 80}, {.min = 0, .max = 80}));
    // A linear target keeps extended values for later stages.
    EXPECT_LT(std::min({mapped.c.r, mapped.c.b}), 0);

    const auto encoded =
        CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_GAMMA22, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_SRGB});
    auto       settings  = conversionSettings(bt2020, encoded, {.min = 0, .max = 80}, {.min = 0, .max = 80});
    const auto clipped   = convertColor(RGBAColor{{.r = 0, .g = 1, .b = 0, .a = 1}}, bt2020, encoded, settings);
    const auto linearRGB = [](double v) { return std::pow(v, 2.2) * 80; };
    const auto r = linearRGB(clipped.c.r), g = linearRGB(clipped.c.g), b = linearRGB(clipped.c.b);
    const auto Y = Hyprgraphics::CMatrix3{srgb->getPrimaries()->toXYZ()}.mat()[1];
    EXPECT_GE(std::min({r, g, b}), -1e-6);
    // Luminance is kept and the color moved straight towards the achromatic axis.
    EXPECT_NEAR(Y[0] * r + Y[1] * g + Y[2] * b, Y[0] * mapped.c.r + Y[1] * mapped.c.g + Y[2] * mapped.c.b, 1e-3);
    const auto lum = Y[0] * r + Y[1] * g + Y[2] * b;
    EXPECT_NEAR((g - lum) / (mapped.c.g - lum), (b - lum) / (mapped.c.b - lum), 1e-3);
}

TEST(ColorManagement, HDRMetadataReportsUnknownMasteringAsZero) {
    const SImageDescription pq{.transferFunction = CM_TRANSFER_FUNCTION_ST2084_PQ, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020};
    const auto              metadata = createHDRMetadata(pq).hdmi_metadata_type1;
    EXPECT_EQ(metadata.eotf, 2);
    EXPECT_EQ(metadata.display_primaries[0].x, 0);
    EXPECT_EQ(metadata.white_point.x, 0);
    EXPECT_EQ(metadata.max_display_mastering_luminance, 0);
    EXPECT_EQ(metadata.min_display_mastering_luminance, 0);
    EXPECT_EQ(createHDRMetadata(SCRGB_IMAGE_DESCRIPTION->value()).hdmi_metadata_type1.eotf, 0);
}

TEST(ColorManagement, HLGBlackStaysBlackBelowUnitSystemGamma) {
    const auto hlg = CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_HLG, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    const auto linear =
        CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_LINEAR, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    // A 150 cd/m² display has system gamma 1.2 * 1.111^log2(0.15) < 1, where pow(0, gamma - 1) diverges.
    const auto black = convertColor(RGBAColor{{.r = 0, .g = 0, .b = 0, .a = 1}}, hlg, linear, conversionSettings(hlg, linear, {.min = 0, .max = 150}, {}));
    EXPECT_EQ(black.c.r, 0);
    EXPECT_EQ(black.c.g, 0);
    EXPECT_EQ(black.c.b, 0);
}

TEST(ColorManagement, HLGReferenceScalingKeepsTheSourceSystemGamma) {
    const auto hlg = CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_HLG, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    const auto linear =
        CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_LINEAR, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    // Doubling the reference white doubles the decoded light of a 1000 cd/m² HLG source instead of decoding it for a 2000 cd/m² display.
    auto settings              = conversionSettings(hlg, linear, {.min = 0, .max = 2000}, {});
    settings.srcReferenceScale = 2;
    const auto result          = convertColor(RGBAColor{{.r = 0.75, .g = 0.75, .b = 0.75, .a = 1}}, hlg, linear, settings);
    EXPECT_NEAR(result.c.r, 2 * 203.152146, 0.01);
}

static Render::SCMSettings pqTonemapSettings(PImageDescription pq, PImageDescription linear, float maxLuminance, float dstMaxLuminance) {
    auto settings             = conversionSettings(pq, linear, {.min = 0, .max = 10000}, {.min = 0, .max = 10000});
    settings.needsTonemap     = true;
    settings.maxLuminance     = maxLuminance;
    settings.dstMaxLuminance  = dstMaxLuminance;
    settings.dstPrimaries2XYZ = Hyprgraphics::CMatrix3{linear->getPrimaries()->toXYZ()}.mat();
    return settings;
}

static double pqSignal(double luminance) {
    const auto Y = std::pow(luminance / 10000.0, 0.1593017578125);
    return std::pow((0.8359375 + 18.8515625 * Y) / (1.0 + 18.6875 * Y), 78.84375);
}

static double pqDecodeForTest(double signal) {
    const auto E = std::pow(signal, 1.0 / 78.84375);
    return std::pow(std::max(E - 0.8359375, 0.0) / (18.8515625 - 18.6875 * E), 1.0 / 0.1593017578125) * 10000.0;
}

TEST(ColorManagement, BT2390NormalizesAgainstTheSourceBlack) {
    const auto pq = CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_ST2084_PQ, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    const auto linear =
        CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_LINEAR, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    auto settings            = pqTonemapSettings(pq, linear, 1000, 100);
    settings.srcMinLuminance = 0.1f;
    settings.dstTFRange.min  = 0.2f;
    const auto signal        = pqSignal(0.1);
    // BT.2390 §5.4: the mastering black LB lands on the target black LMIN.
    EXPECT_NEAR(convertColor(RGBAColor{{.r = signal, .g = signal, .b = signal, .a = 1}}, pq, linear, settings).c.g, 0.2, 1e-4);
}

TEST(ColorManagement, BT2390StaysFiniteWhenBothPeaksClampToPQWhite) {
    const auto pq = CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_ST2084_PQ, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    const auto linear =
        CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_LINEAR, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    // A reference-scaled source peak above 10000 cd/m² on a target without a known peak: both ends encode to PQ 1.0.
    auto settings = pqTonemapSettings(pq, linear, 20300, 10000.005F);
    for (const double signal : {0.0, 0.5, 1.0}) {
        const auto out = convertColor(RGBAColor{{.r = signal, .g = signal, .b = signal, .a = 1}}, pq, linear, settings);
        EXPECT_TRUE(std::isfinite(out.c.g)) << "signal " << signal;
        EXPECT_NEAR(out.c.g, pqDecodeForTest(signal), 0.05) << "signal " << signal;
    }
    // A source black at the PQ ceiling leaves no range to map.
    settings.srcMinLuminance = 10000;
    EXPECT_TRUE(std::isfinite(convertColor(RGBAColor{{.r = 1, .g = 1, .b = 1, .a = 1}}, pq, linear, settings).c.g));
}

TEST(ColorManagement, BT2390UsesTheBT2100ICtCpMatrices) {
    const auto pq = CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_ST2084_PQ, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    const auto linear =
        CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_LINEAR, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    // Reference computed with the integer RGB -> LMS and ICtCp matrices of BT.2100 Table 7.
    const auto red = convertColor(RGBAColor{{.r = pqSignal(1000), .g = 0, .b = 0, .a = 1}}, pq, linear, pqTonemapSettings(pq, linear, 1000, 100));
    EXPECT_NEAR(red.c.r, 315.570419, 2e-3);
    EXPECT_NEAR(red.c.g, 12.945958, 2e-3);
    EXPECT_NEAR(red.c.b, 2.713443, 2e-3);
}

TEST(ColorManagement, AlternativeTonemapMatchesTheShader) {
    const auto pq = CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_ST2084_PQ, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    const auto linear =
        CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_LINEAR, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    auto settings        = pqTonemapSettings(pq, linear, 1000, 600);
    settings.tonemapMode = 3;
    const auto nits      = [&](double luminance) {
        const auto signal = pqSignal(luminance);
        return convertColor(RGBAColor{{.r = signal, .g = signal, .b = signal, .a = 1}}, pq, linear, settings).c.g;
    };
    // Knee at 0.6 * 600 = 360, rational roll-off reaching the target peak at the source peak.
    const double knee = 360, alpha = 400.0 / ((1000 - knee) * (600 - knee));
    EXPECT_NEAR(nits(300), 300, 0.05);
    EXPECT_NEAR(nits(700), knee + (700 - knee) / (1 + alpha * (700 - knee)), 0.05);
    EXPECT_NEAR(nits(1000), 600, 0.05);
}

TEST(ColorManagement, WhiteOnAPrimaryIsRejected) {
    auto primaries  = getPrimaries(CM_PRIMARIES_SRGB);
    primaries.white = primaries.red;
    EXPECT_FALSE(rgbToXYZFromPrimaries(primaries).has_value());
    EXPECT_TRUE(rgbToXYZFromPrimaries(getPrimaries(CM_PRIMARIES_SRGB)).has_value());
    EXPECT_TRUE(rgbToXYZFromPrimaries(getPrimaries(CM_PRIMARIES_CIE1931_XYZ)).has_value());
}

TEST(ColorManagement, HDRMetadataSendsNonPhysicalMasteringPrimariesAsUnknown) {
    SImageDescription d{.transferFunction = CM_TRANSFER_FUNCTION_ST2084_PQ, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020};
    EXPECT_TRUE(isPhysicalDisplayChromaticities(NColorPrimaries::DISPLAY_P3));
    EXPECT_TRUE(isPhysicalDisplayChromaticities(NColorPrimaries::BT2020));
    d.masteringPrimaries       = NColorPrimaries::DISPLAY_P3;
    d.masteringPrimaries.red.x = 2147.483647;
    EXPECT_FALSE(isPhysicalDisplayChromaticities(d.masteringPrimaries));
    auto metadata = createHDRMetadata(d).hdmi_metadata_type1;
    EXPECT_EQ(metadata.display_primaries[0].x, 0);
    EXPECT_EQ(metadata.white_point.y, 0);
    // Imaginary primaries are fine for an encoding, not for a mastering display.
    d.masteringPrimaries      = NColorPrimaries::DISPLAY_P3;
    d.masteringPrimaries.blue = {.x = 0.0001, .y = -0.077};
    EXPECT_FALSE(isPhysicalDisplayChromaticities(d.masteringPrimaries));
    EXPECT_EQ(createHDRMetadata(d).hdmi_metadata_type1.display_primaries[2].y, 0);
}

TEST(ColorManagement, InternedDescriptionsReleaseTheirPrimariesAndNeverReuseIds) {
    auto value = SImageDescription{.primaries = NColorPrimaries::BT709, .luminances = {.min = 0.12345F, .max = 913, .reference = 137}};
    value.primaries.red.x += 0.000123;
    auto                        description     = CImageDescription::from(value);
    const auto                  id              = description->id();
    const auto                  primariesId     = description->getPrimaries()->id();
    WP<const CImageDescription> weakDescription = description;
    WP<const CPrimaries>        weakPrimaries   = description->getPrimaries();
    EXPECT_EQ(CImageDescription::from(value), description);
    EXPECT_EQ(CImageDescription::from(id), description);
    EXPECT_EQ(CPrimaries::from(primariesId), description->getPrimaries());
    description.reset();
    EXPECT_TRUE(weakDescription.expired());
    EXPECT_TRUE(weakPrimaries.expired());
    EXPECT_FALSE(CImageDescription::from(id));
    EXPECT_FALSE(CPrimaries::from(primariesId));
    const auto recreated = CImageDescription::from(value);
    EXPECT_GT(recreated->id(), id);
    EXPECT_GT(recreated->getPrimaries()->id(), primariesId);
}

TEST(ColorManagement, DescriptionOwnersKeepSharedValuesAlive) {
    auto                        description = CImageDescription::from(SImageDescription{.luminances = {.min = 0.125F, .max = 914, .reference = 139}});
    auto                        owner       = description;
    const auto                  id          = description->id();
    WP<const CImageDescription> weak        = description;
    description.reset();
    EXPECT_FALSE(weak.expired());
    EXPECT_EQ(CImageDescription::from(id), owner);
    owner.reset();
    EXPECT_TRUE(weak.expired());
}

TEST(ColorManagement, UnknownIdentitiesAreNull) {
    EXPECT_FALSE(CImageDescription::from(uint64_t{0}));
    EXPECT_FALSE(CImageDescription::from(UINT64_MAX));
    EXPECT_FALSE(CPrimaries::from(uint64_t{0}));
    EXPECT_FALSE(CPrimaries::from(UINT64_MAX));
}

TEST(ColorManagement, ChurnDoesNotKeepHistoricalValuesAlive) {
    for (uint32_t i = 0; i < 4096; ++i) {
        auto value = SImageDescription{.primaries = NColorPrimaries::BT709};
        value.primaries.red.x += (i + 1) * 0.0000001;
        value.luminances.reference = 1000 + i;
        auto       description     = CImageDescription::from(value);
        const auto id              = description->id();
        const auto primariesId     = description->getPrimaries()->id();
        // Exercise matrix conversion as well: no historical all-pairs cache.
        auto matrix = description->getPrimaries()->convertMatrix(DEFAULT_SRGB_IMAGE_DESCRIPTION->getPrimaries());
        EXPECT_TRUE(std::isfinite(matrix.mat()[0][0]));
        description.reset();
        EXPECT_FALSE(CImageDescription::from(id));
        EXPECT_FALSE(CPrimaries::from(primariesId));
    }
}

TEST(ColorManagement, ICCDescriptionsRemainDistinctAndReleaseNormally) {
    SImageDescription value;
    value.icc.present = true;
    auto first        = CImageDescription::from(value);
    auto second       = CImageDescription::from(value);
    EXPECT_NE(first->id(), second->id());
    const auto id = first->id();
    first.reset();
    EXPECT_FALSE(CImageDescription::from(id));
    EXPECT_EQ(CImageDescription::from(second->id()), second);
}

TEST(ColorManagement, InterningUsesTheSameEqualityForNamedPrimariesAndSignedZero) {
    SImageDescription first{.primariesNameSet = true};
    auto              second       = first;
    second.primaries.red.x         = 0.9; // ignored by equality for named primaries
    first.masteringLuminances.min  = 0.F;
    second.masteringLuminances.min = -0.F;
    const auto owner               = CImageDescription::from(first);
    EXPECT_EQ(CImageDescription::from(second), owner);
}
