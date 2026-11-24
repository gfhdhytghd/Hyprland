#include <render/CMSettings.hpp>
#include <helpers/TransferFunction.hpp>
#include <gtest/gtest.h>

using namespace Render;
using namespace NColorManagement;

TEST(CMSettings, MonitorAdjustmentsCannotReuseStaleSettings) {
    CCMSettingsCache  cache;
    SCMSettingsInputs inputs{.needsSDRmod = true};
    const auto        source = DEFAULT_SRGB_IMAGE_DESCRIPTION;
    const auto        target = DEFAULT_HDR_IMAGE_DESCRIPTION;
    const auto        plain  = cache.get(source, target, inputs);
    EXPECT_FALSE(plain.needsSDRmod);
    inputs.sdrBrightness = 1.5F;
    const auto brighter  = cache.get(source, target, inputs);
    EXPECT_TRUE(brighter.needsSDRmod);
    EXPECT_FLOAT_EQ(brighter.sdrBrightnessMultiplier, 1.5F);
    EXPECT_FLOAT_EQ(brighter.maxLuminance, plain.maxLuminance * 1.5F);
    inputs.sdrSaturation = 0.5F;
    EXPECT_FLOAT_EQ(cache.get(source, target, inputs).sdrSaturation, 0.5F);
    inputs.sdrMinLuminance = 0.01F;
    EXPECT_FLOAT_EQ(cache.get(source, target, inputs).srcTFRange.min, 0.01F);
    inputs.needsSDRmod    = false; // including an EXT_LINEAR destination on a different physical output
    const auto unmodified = cache.get(source, target, inputs);
    EXPECT_FALSE(unmodified.needsSDRmod);
    EXPECT_FLOAT_EQ(unmodified.sdrBrightnessMultiplier, 1.F);
    EXPECT_FLOAT_EQ(unmodified.sdrSaturation, 1.F);
    EXPECT_FLOAT_EQ(unmodified.srcTFRange.min, plain.srcTFRange.min);
}

TEST(CMSettings, WindowToneMappingPolicyIsResolvedBeforeLookup) {
    CCMSettingsCache  cache;
    const auto        source = DEFAULT_HDR_IMAGE_DESCRIPTION;
    const auto        target = DEFAULT_SRGB_IMAGE_DESCRIPTION;
    SCMSettingsInputs inputs{.sourceTF = CM_TRANSFER_FUNCTION_ST2084_PQ};
    const auto        mapped = cache.get(source, target, inputs);
    ASSERT_TRUE(mapped.needsTonemap);
    inputs.tonemapMode = 0;
    EXPECT_FALSE(cache.get(source, target, inputs).needsTonemap);
    inputs.tonemapMode = 2;
    const auto limited = cache.get(source, target, inputs);
    EXPECT_TRUE(limited.needsTonemap);
    EXPECT_EQ(limited.tonemapMode, 2);
    EXPECT_FLOAT_EQ(limited.maxLuminance, limited.dstMaxLuminance);
    inputs.tonemapMode = 1;
    EXPECT_FLOAT_EQ(cache.get(source, target, inputs).maxLuminance, mapped.maxLuminance);
}

TEST(CMSettings, ResolvedSurfaceAndConfigTransferFunctionsCannotAlias) {
    CCMSettingsCache  cache;
    SCMSettingsInputs inputs{.sourceTF = CM_TRANSFER_FUNCTION_SRGB};
    const auto        source = DEFAULT_SRGB_IMAGE_DESCRIPTION;
    const auto        target = DEFAULT_HDR_IMAGE_DESCRIPTION;
    EXPECT_EQ(cache.get(source, target, inputs).sourceTF, CM_TRANSFER_FUNCTION_SRGB);
    // A new surface at a reused address, changed CM association, shouldUseSurface,
    // or cm_sdr_eotf reload may resolve a different TF with the same descriptions.
    inputs.sourceTF = CM_TRANSFER_FUNCTION_GAMMA22;
    EXPECT_EQ(cache.get(source, target, inputs).sourceTF, CM_TRANSFER_FUNCTION_GAMMA22);
    inputs.sourceTF = CM_TRANSFER_FUNCTION_SRGB;
    EXPECT_EQ(cache.get(source, target, inputs).sourceTF, CM_TRANSFER_FUNCTION_SRGB);
}

TEST(CMSettings, DescriptionChangesAndEvictionPreserveComputedResults) {
    CCMSettingsCache  cache;
    SCMSettingsInputs inputs{.sourceTF = CM_TRANSFER_FUNCTION_LINEAR};
    auto              description    = DEFAULT_SRGB_IMAGE_DESCRIPTION->value();
    description.transferFunction     = CM_TRANSFER_FUNCTION_LINEAR;
    description.luminances.reference = 47;
    auto       source                = CImageDescription::from(description);
    const auto id                    = source->id();
    const auto original              = cache.get(source, DEFAULT_HDR_IMAGE_DESCRIPTION, inputs);
    for (uint32_t i = 0; i < 128; ++i) {
        description.luminances.reference = 300 + i;
        const auto other                 = CImageDescription::from(description);
        const auto computed              = cache.get(other, DEFAULT_HDR_IMAGE_DESCRIPTION, inputs);
        EXPECT_FLOAT_EQ(computed.srcReferenceScale, sc<float>(DEFAULT_HDR_IMAGE_DESCRIPTION->value().luminances.reference) / (300 + i));
    }
    const auto rebuilt = cache.get(source, DEFAULT_HDR_IMAGE_DESCRIPTION, inputs);
    EXPECT_FLOAT_EQ(rebuilt.srcReferenceScale, original.srcReferenceScale);
    EXPECT_EQ(rebuilt.convertMatrix, original.convertMatrix);
    cache.clear();
    EXPECT_EQ(cache.get(source, DEFAULT_HDR_IMAGE_DESCRIPTION, inputs).convertMatrix, original.convertMatrix);
    source.reset();
    EXPECT_FALSE(CImageDescription::from(id)); // settings cache never retains descriptions
}

TEST(CMSettings, NarrowBT1886SurvivesReferenceScaling) {
    auto description             = DEFAULT_SRGB_IMAGE_DESCRIPTION->value();
    description.transferFunction = CM_TRANSFER_FUNCTION_BT1886;
    description.luminances       = {.min = 4095.9999, .max = 4096, .reference = 4096};
    CCMSettingsCache cache;
    const auto       settings = cache.get(CImageDescription::from(description), DEFAULT_SRGB_IMAGE_DESCRIPTION, {.sourceTF = CM_TRANSFER_FUNCTION_BT1886});
    const auto       range    = settings.srcTFRange;
    EXPECT_DOUBLE_EQ(range.min, 79.999998046875);
    EXPECT_DOUBLE_EQ(range.max, 80);
    for (double signal : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        const double light = decodeChannel(signal, settings.sourceTF, range);
        EXPECT_NEAR(light, std::lerp(range.min, range.max, signal), 1e-13);
        EXPECT_NEAR(encodeChannel(light, settings.sourceTF, range), signal, 1e-8);
    }
}

TEST(CMSettings, BlackOverrideCannotCollapseSourceRange) {
    CCMSettingsCache cache;
    const auto       source   = DEFAULT_SRGB_IMAGE_DESCRIPTION;
    const auto       settings = cache.get(source, source, {.needsSDRmod = true, .sdrMinLuminance = 80});
    EXPECT_DOUBLE_EQ(settings.srcTFRange.min, source->value().encodingMinLuminance());
}

// The renderer compares this against the described TF before skipping CM for equal descriptions (SHD-05).
TEST(CMSettings, ForcedGammaOverridesTheDescribedSRGBOfCMAwareSurfaces) {
    using namespace NTransferFunction;
    // gamma22force decodes a CM-aware sRGB surface as gamma 2.2, so an equal sRGB target still needs conversion.
    EXPECT_EQ(resolveSourceTF(CM_TRANSFER_FUNCTION_SRGB, true, true, TF_FORCED_GAMMA22), CM_TRANSFER_FUNCTION_GAMMA22);
    EXPECT_EQ(resolveSourceTF(CM_TRANSFER_FUNCTION_SRGB, true, true, TF_GAMMA22), CM_TRANSFER_FUNCTION_SRGB);
    EXPECT_EQ(resolveSourceTF(CM_TRANSFER_FUNCTION_GAMMA22, true, true, TF_SRGB), CM_TRANSFER_FUNCTION_GAMMA22);
    // Surfaces without a CM extension follow the SDR policy.
    EXPECT_EQ(resolveSourceTF(CM_TRANSFER_FUNCTION_GAMMA22, true, false, TF_SRGB), CM_TRANSFER_FUNCTION_SRGB);
    EXPECT_EQ(resolveSourceTF(CM_TRANSFER_FUNCTION_SRGB, true, false, TF_GAMMA22), CM_TRANSFER_FUNCTION_GAMMA22);
    EXPECT_EQ(resolveSourceTF(CM_TRANSFER_FUNCTION_SRGB, true, false, TF_DEFAULT), CM_TRANSFER_FUNCTION_SRGB);
    // Non-surface sources and other transfer functions are never overridden.
    EXPECT_EQ(resolveSourceTF(CM_TRANSFER_FUNCTION_SRGB, false, false, TF_FORCED_GAMMA22), CM_TRANSFER_FUNCTION_SRGB);
    EXPECT_EQ(resolveSourceTF(CM_TRANSFER_FUNCTION_ST2084_PQ, true, true, TF_FORCED_GAMMA22), CM_TRANSFER_FUNCTION_ST2084_PQ);
}

TEST(CMSettings, TaggedBlurBackdropAndClientShareSurfaceContext) {
    const auto srgb     = DEFAULT_SRGB_IMAGE_DESCRIPTION;
    const auto client   = textureColorSource(nullptr, srgb, nullptr);
    const auto backdrop = textureColorSource(srgb, srgb, nullptr);
    EXPECT_EQ(client.description, srgb);
    EXPECT_EQ(backdrop.description, srgb);
    EXPECT_EQ(resolveSourceTF(client.description->value().transferFunction, client.useSurfacePolicy, true, NTransferFunction::TF_FORCED_GAMMA22), CM_TRANSFER_FUNCTION_GAMMA22);
    EXPECT_EQ(resolveSourceTF(backdrop.description->value().transferFunction, backdrop.useSurfacePolicy, true, NTransferFunction::TF_FORCED_GAMMA22), CM_TRANSFER_FUNCTION_SRGB);
}

TEST(CMSettings, ToneMapDrawsReuseTargetPrimaryMatrices) {
    const auto  target   = CPrimaries::from(CM_PRIMARIES_DCI_P3);
    const auto& cached   = target->d65Matrices();
    auto        expected = toD65XYZ(Hyprgraphics::CMatrix3{target->toXYZ()}.mat());
    EXPECT_EQ(Hyprgraphics::CMatrix3{cached.forward}.mat(), expected.mat());
    EXPECT_EQ(Hyprgraphics::CMatrix3{cached.inverse}.mat(), expected.invert().mat());
    EXPECT_NE(&CPrimaries::from(CM_PRIMARIES_SRGB)->d65Matrices(), &cached);
    EXPECT_EQ(&CPrimaries::from(CM_PRIMARIES_DCI_P3)->d65Matrices(), &cached);
}
