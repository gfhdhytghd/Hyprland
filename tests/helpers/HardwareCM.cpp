#include <helpers/cm/HardwareCM.hpp>
#include <render/types.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <iostream>

using namespace NColorManagement;

// KMS LUTs interpolate linearly between their entries.
static double sampleLut(const std::vector<uint16_t>& lut, size_t channel, double x) {
    const size_t entries  = lut.size() / 3;
    const double position = std::clamp(x, 0.0, 1.0) * sc<double>(entries - 1);
    const size_t index    = std::min(sc<size_t>(position), entries - 2);
    const double fraction = position - sc<double>(index);
    return ((lut[(index * 3) + channel] * (1.0 - fraction)) + (lut[((index + 1) * 3) + channel] * fraction)) / UINT16_MAX;
}

static std::array<double, 3> runHardware(const SHardwareCM& hw, std::array<double, 3> signal) {
    std::array<double, 3> linear;
    for (size_t i = 0; i < 3; ++i)
        linear[i] = sampleLut(hw.degamma, i, signal[i]);
    std::array<double, 3> converted;
    for (size_t row = 0; row < 3; ++row)
        converted[row] = std::clamp((hw.ctm[row * 3] * linear[0]) + (hw.ctm[(row * 3) + 1] * linear[1]) + (hw.ctm[(row * 3) + 2] * linear[2]), 0.0, 1.0);
    std::array<double, 3> out;
    for (size_t i = 0; i < 3; ++i)
        out[i] = sampleLut(hw.gamma, i, converted[i]);
    return out;
}

static const SHardwareCMParams AMD_LIKE = {.degammaSize = 4096, .gammaSize = 4096, .supportsCTM = true, .sdrBlack = SDR_MIN_LUMINANCE};

static PImageDescription       hdrOutput() {
    return CImageDescription::from(SImageDescription{
        .transferFunction    = CM_TRANSFER_FUNCTION_ST2084_PQ,
        .primariesNameSet    = true,
        .primariesNamed      = CM_PRIMARIES_BT2020,
        .luminances          = {.min = 0, .max = 10000, .reference = 203},
        .masteringLuminances = {.min = 0, .max = 1000},
    });
}

static PImageDescription sdrSource(eTransferFunction tf = CM_TRANSFER_FUNCTION_GAMMA22) {
    return CImageDescription::from(SImageDescription{.transferFunction = tf, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_SRGB});
}

TEST(HardwareCM, SDROnHDRMatchesTheShaderConversion) {
    const auto source = sdrSource();
    const auto target = hdrOutput();
    const auto hw     = buildHardwareCM(source->value(), target->value(), AMD_LIKE);
    ASSERT_TRUE(hw.has_value());

    // The shader path: reference white anchored to 203 cd/m², SDR black at sdr_min_luminance, no tone mapping or gamut clipping needed.
    const float         scale = referenceLuminanceScale(source->value(), target->value());
    Render::SCMSettings settings{
        .sourceTF          = source->value().transferFunction,
        .targetTF          = target->value().transferFunction,
        .srcTFRange        = {.min = SDR_MIN_LUMINANCE, .max = source->value().encodingMaxLuminance() * scale},
        .dstTFRange        = {.min = target->value().encodingMinLuminance(), .max = target->value().encodingMaxLuminance()},
        .srcReferenceScale = scale,
        .convertMatrix     = Hyprgraphics::CMatrix3{source->getPrimaries()->convertMatrix(target->getPrimaries())}.mat(),
    };
    settings.dstPrimaries2XYZ = Hyprgraphics::CMatrix3{target->getPrimaries()->toXYZ()}.mat();

    // SDR white lands on the 203 cd/m² reference: PQ(203) = 0.58069.
    EXPECT_NEAR(runHardware(*hw, {1, 1, 1})[1], 0.58069, 0.5 / 1023);

    for (const auto& signal : std::array<std::array<double, 3>, 6>{{{0, 0, 0}, {1, 1, 1}, {0.5, 0.5, 0.5}, {1, 0, 0}, {0.02, 0.6, 0.9}, {0.003, 0.001, 0.002}}}) {
        const auto expected = convertColor(RGBAColor{{.r = signal[0], .g = signal[1], .b = signal[2], .a = 1}}, source, target, settings);
        const auto actual   = runHardware(*hw, signal);
        // Within half a 10 bit PQ code value.
        for (size_t i = 0; i < 3; ++i)
            EXPECT_NEAR(actual[i], expected.v[i], 0.5 / 1023) << "signal " << signal[0] << "," << signal[1] << "," << signal[2] << " channel " << i;
    }
}

TEST(HardwareCM, SDRBrightnessAndSaturationFollowTheShader) {
    auto params          = AMD_LIKE;
    params.sdrBrightness = 1.5F;
    params.sdrSaturation = 0.5F;
    const auto source    = sdrSource();
    const auto target    = hdrOutput();
    const auto hw        = buildHardwareCM(source->value(), target->value(), params);
    ASSERT_TRUE(hw.has_value());

    const float         scale = referenceLuminanceScale(source->value(), target->value());
    Render::SCMSettings settings{
        .sourceTF                = source->value().transferFunction,
        .targetTF                = target->value().transferFunction,
        .srcTFRange              = {.min = SDR_MIN_LUMINANCE, .max = source->value().encodingMaxLuminance() * scale},
        .dstTFRange              = {.min = target->value().encodingMinLuminance(), .max = target->value().encodingMaxLuminance()},
        .srcReferenceScale       = scale,
        .convertMatrix           = Hyprgraphics::CMatrix3{source->getPrimaries()->convertMatrix(target->getPrimaries())}.mat(),
        .needsSDRmod             = true,
        .sdrSaturation           = 0.5F,
        .sdrBrightnessMultiplier = 1.5F,
    };
    settings.dstPrimaries2XYZ = Hyprgraphics::CMatrix3{target->getPrimaries()->toXYZ()}.mat();

    for (const auto& signal : std::array<std::array<double, 3>, 3>{{{1, 1, 1}, {1, 0, 0}, {0.2, 0.7, 0.4}}}) {
        const auto expected = convertColor(RGBAColor{{.r = signal[0], .g = signal[1], .b = signal[2], .a = 1}}, source, target, settings);
        const auto actual   = runHardware(*hw, signal);
        for (size_t i = 0; i < 3; ++i)
            EXPECT_NEAR(actual[i], expected.v[i], 0.5 / 1023);
    }
}

TEST(HardwareCM, RefusesWhatTheHardwareCantReproduce) {
    const auto target = hdrOutput()->value();
    // Cross-channel transfer functions, HDR sources and extended ranges.
    EXPECT_FALSE(buildHardwareCM(sdrSource(CM_TRANSFER_FUNCTION_HLG)->value(), target, AMD_LIKE));
    EXPECT_FALSE(buildHardwareCM(sdrSource(CM_TRANSFER_FUNCTION_ST2084_PQ)->value(), target, AMD_LIKE));
    EXPECT_FALSE(buildHardwareCM(sdrSource(CM_TRANSFER_FUNCTION_EXT_LINEAR)->value(), target, AMD_LIKE));
    // Missing hardware.
    auto params        = AMD_LIKE;
    params.supportsCTM = false;
    EXPECT_FALSE(buildHardwareCM(sdrSource()->value(), target, params));
    params             = AMD_LIKE;
    params.degammaSize = 0;
    EXPECT_FALSE(buildHardwareCM(sdrSource()->value(), target, params));
    // A wider source gamut needs the shader's hue-preserving clip.
    const auto wide = CImageDescription::from(SImageDescription{.transferFunction = CM_TRANSFER_FUNCTION_GAMMA22, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_BT2020});
    EXPECT_FALSE(buildHardwareCM(wide->value(), sdrSource()->value(), AMD_LIKE));
    // Anything brighter than the target needs tone mapping.
    params               = AMD_LIKE;
    params.sdrBrightness = 10.F;
    EXPECT_FALSE(buildHardwareCM(sdrSource()->value(), target, params));
}

TEST(HardwareCM, GammaRampsComposeAfterTheEncoding) {
    const std::vector<uint16_t> base = {0, 0, 0, 32768, 32768, 32768, 65535, 65535, 65535};
    const std::vector<uint16_t> ramp = {65535, 0, 0, 0, 65535, 65535}; // red inverted, green and blue identity
    EXPECT_TRUE(composeGammaRamp({}, {}, 256).empty());
    EXPECT_EQ(composeGammaRamp(base, {}, 3), base);
    EXPECT_EQ(composeGammaRamp({}, ramp, 2), ramp);
    const auto composed = composeGammaRamp(base, ramp, 3);
    ASSERT_EQ(composed.size(), 9U);
    EXPECT_EQ(composed[0], 65535);      // red at 0 -> inverted
    EXPECT_NEAR(composed[3], 32767, 1); // red at 0.5 -> 0.5
    EXPECT_EQ(composed[6], 0);          // red at 1 -> inverted
    EXPECT_EQ(composed[7], 65535);      // green at 1 -> identity
}

TEST(HardwareCM, RefusesLUTsTooCoarseForTheEncoding) {
    // Linear-light LUT spacing can't follow PQ near black: 256 entries miss dark SDR by tens of 10 bit codes.
    auto params        = AMD_LIKE;
    params.degammaSize = 256;
    params.gammaSize   = 256;
    EXPECT_FALSE(buildHardwareCM(sdrSource()->value(), hdrOutput()->value(), params));
    // So does a 4096 entry pipeline once SDR black goes to zero.
    params          = AMD_LIKE;
    params.sdrBlack = 0.F;
    EXPECT_FALSE(buildHardwareCM(sdrSource()->value(), hdrOutput()->value(), params));
}

TEST(HardwareCM, RefusesColorsTheShaderGamutMapsAtTheBlackFloor) {
    // (0.05, 0, 0) sRGB decodes below the 0.2 cd/m² BT.1886 black in luminance; the shader maps it to black, per-channel clipping keeps red.
    const auto source = CImageDescription::from(SImageDescription{
        .transferFunction = CM_TRANSFER_FUNCTION_SRGB, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_SRGB, .luminances = {.min = 0, .max = 80, .reference = 80}});
    const auto target = CImageDescription::from(SImageDescription{
        .transferFunction = CM_TRANSFER_FUNCTION_BT1886, .primariesNameSet = true, .primariesNamed = CM_PRIMARIES_SRGB, .luminances = {.min = 0.2F, .max = 80, .reference = 80}});
    EXPECT_FALSE(buildHardwareCM(source->value(), target->value(), AMD_LIKE));
}

TEST(HardwareCM, DiagonalUserCTMFoldsIntoTheEncoding) {
    auto       params = AMD_LIKE;
    const auto plain  = buildHardwareCM(sdrSource()->value(), hdrOutput()->value(), params);
    params.outputGain = {0.5F, 1.F, 0.25F};
    const auto gained = buildHardwareCM(sdrSource()->value(), hdrOutput()->value(), params);
    ASSERT_TRUE(plain && gained);
    // Composited frames get the user CTM on the encoded signal: half the PQ value, not PQ of half the light.
    const auto white = runHardware(*plain, {1, 1, 1});
    const auto dim   = runHardware(*gained, {1, 1, 1});
    EXPECT_NEAR(dim[0], 0.5 * white[0], 1.0 / 65535);
    EXPECT_NEAR(dim[1], white[1], 1.0 / 65535);
    EXPECT_NEAR(dim[2], 0.25 * white[2], 1.0 / 65535);
}

TEST(HardwareCM, BuildTiming) {
    const auto          source = sdrSource();
    const auto          target = hdrOutput();
    std::vector<double> timings;
    for (size_t i = 0; i < 21; ++i) {
        const auto   start   = std::chrono::steady_clock::now();
        const auto   result  = buildHardwareCM(source->value(), target->value(), AMD_LIKE);
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        ASSERT_TRUE(result);
        if (i > 0)
            timings.push_back(elapsed);
    }
    std::ranges::sort(timings);
    std::cout << "buildHardwareCM Debug 4096/4096 SDR->PQ, 20 warm calls: min=" << timings.front() << " ms median=" << timings[timings.size() / 2] << " ms max=" << timings.back()
              << " ms\n";
}

TEST(HardwareCM, CacheRetainsAlternatingDescriptionsAcrossBudgetDenials) {
    CHardwareCMCache cache;
    const auto       first  = sdrSource();
    const auto       second = sdrSource(CM_TRANSFER_FUNCTION_SRGB);
    const auto       target = hdrOutput();
    const auto       start  = std::chrono::steady_clock::time_point{};
    const auto       a      = cache.get(first, target, AMD_LIKE, start);
    ASSERT_TRUE(a);
    // Misses fall back to composition; hits remain available in the same frame.
    EXPECT_FALSE(cache.get(second, target, AMD_LIKE, start));
    EXPECT_EQ(cache.get(first, target, AMD_LIKE, start), a);
    const auto later = start + std::chrono::hours(1);
    const auto b     = cache.get(second, target, AMD_LIKE, later);
    ASSERT_TRUE(b);
    for (size_t i = 0; i < 100; ++i) {
        EXPECT_EQ(cache.get(first, target, AMD_LIKE, later), a);
        EXPECT_EQ(cache.get(second, target, AMD_LIKE, later), b);
    }
}

TEST(HardwareCM, NegativeCacheHitsDoNotSpendTheRebuildBudget) {
    CHardwareCMCache cache;
    const auto       source = sdrSource(CM_TRANSFER_FUNCTION_HLG);
    const auto       target = hdrOutput();
    const auto       start  = std::chrono::steady_clock::time_point{};
    EXPECT_FALSE(cache.get(source, target, AMD_LIKE, start));
    EXPECT_FALSE(cache.get(sdrSource(), target, AMD_LIKE, start)); // failures also spend the budget
    const auto later = start + std::chrono::hours(1);
    EXPECT_FALSE(cache.get(source, target, AMD_LIKE, later));
    // A cached failure must not build again and block this supported source.
    EXPECT_TRUE(cache.get(sdrSource(), target, AMD_LIKE, later));
}

TEST(HardwareCM, CacheKeysCoverEveryHardwareParameterAndTarget) {
    CHardwareCMCache cache;
    const auto       source   = sdrSource();
    const auto       target   = hdrOutput();
    const auto       now      = std::chrono::steady_clock::time_point{};
    const auto       original = cache.get(source, target, AMD_LIKE, now);
    ASSERT_TRUE(original);
    std::vector<SHardwareCMParams> variants(8, AMD_LIKE);
    variants[0].degammaSize++;
    variants[1].gammaSize++;
    variants[2].supportsCTM   = false;
    variants[3].sdrBrightness = 1.5F;
    variants[4].sdrSaturation = 0.5F;
    variants[5].sdrBlack      = 0.F;
    variants[6].outputGain[0] = 0.5F;
    variants[7].sdrBlack.reset();
    for (const auto& params : variants)
        EXPECT_FALSE(cache.get(source, target, params, now)); // each is a miss, denied by the budget
    EXPECT_FALSE(cache.get(source, DEFAULT_SRGB_IMAGE_DESCRIPTION, AMD_LIKE, now));
    EXPECT_EQ(cache.get(source, target, AMD_LIKE, now), original);
}

TEST(HardwareCM, LRUIsBoundedAndReleasesDescriptionOwners) {
    CHardwareCMCache                 cache;
    std::array<PImageDescription, 9> descriptions;
    auto                             now = std::chrono::steady_clock::time_point{};
    for (size_t i = 0; i < descriptions.size(); ++i) {
        auto value                 = sdrSource()->value();
        value.luminances.reference = 80 + i;
        descriptions[i]            = CImageDescription::from(value);
        cache.get(descriptions[i], hdrOutput(), AMD_LIKE, now);
        now += std::chrono::hours(1);
        if (i == 7)
            cache.get(descriptions[0], hdrOutput(), AMD_LIKE, now); // first becomes MRU
    }
    const auto oldestId   = descriptions[1]->id();
    const auto retainedId = descriptions[0]->id();
    descriptions[1].reset();
    descriptions[0].reset();
    EXPECT_FALSE(CImageDescription::from(oldestId));
    EXPECT_TRUE(CImageDescription::from(retainedId));
}

TEST(HardwareCM, BudgetDeniesUniqueDescriptionChurnWithoutRetainingIt) {
    CHardwareCMCache cache;
    const auto       source  = sdrSource();
    const auto       target  = hdrOutput();
    const auto       now     = std::chrono::steady_clock::time_point{};
    const auto       initial = cache.get(source, target, AMD_LIKE, now);
    ASSERT_TRUE(initial);
    for (uint32_t i = 0; i < 64; ++i) {
        auto value                 = source->value();
        value.luminances.reference = 1000 + i;
        auto       description     = CImageDescription::from(value);
        const auto id              = description->id();
        EXPECT_FALSE(cache.get(description, target, AMD_LIKE, now + std::chrono::milliseconds(99)));
        description.reset();
        EXPECT_FALSE(CImageDescription::from(id));
    }
    EXPECT_EQ(cache.get(source, target, AMD_LIKE, now), initial);
}
