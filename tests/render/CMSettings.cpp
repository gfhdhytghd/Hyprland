#include <render/CMSettings.hpp>
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
        EXPECT_FLOAT_EQ(computed.srcRefLuminance, 300 + i);
    }
    const auto rebuilt = cache.get(source, DEFAULT_HDR_IMAGE_DESCRIPTION, inputs);
    EXPECT_FLOAT_EQ(rebuilt.srcReferenceScale, original.srcReferenceScale);
    EXPECT_EQ(rebuilt.convertMatrix, original.convertMatrix);
    cache.clear();
    EXPECT_EQ(cache.get(source, DEFAULT_HDR_IMAGE_DESCRIPTION, inputs).convertMatrix, original.convertMatrix);
    source.reset();
    EXPECT_FALSE(CImageDescription::from(id)); // settings cache never retains descriptions
}
