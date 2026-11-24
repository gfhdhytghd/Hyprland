#include <helpers/cm/OutputEncoding.hpp>
#include <render/CMSettings.hpp>
#include <drm_mode.h>
#include <gtest/gtest.h>

#include <format>
#include <vector>

using namespace NColorManagement;

static constexpr SOutputColorCapabilities HDR_OUTPUT = {
    .shaderCM           = true,
    .enabled10bit       = true,
    .bt2020Signal       = true,
    .wideColor          = true,
    .hdr                = true,
    .edidChromaticities = true,
};

static PImageDescription describe(eTransferFunction tf, SImageDescription::SPCLuminances luminances = {}, uint32_t masteringMax = 0, uint32_t maxCLL = 0) {
    return CImageDescription::from(SImageDescription{
        .transferFunction    = tf,
        .primariesNameSet    = true,
        .primariesNamed      = tf == CM_TRANSFER_FUNCTION_ST2084_PQ || tf == CM_TRANSFER_FUNCTION_HLG ? CM_PRIMARIES_BT2020 : CM_PRIMARIES_SRGB,
        .luminances          = luminances,
        .masteringLuminances = {.min = 0, .max = masteringMax},
        .maxCLL              = maxCLL,
    });
}

static PImageDescription pqDescription(uint32_t masteringMax, uint32_t maxCLL, ePrimaries primaries = CM_PRIMARIES_BT2020) {
    return CImageDescription::from(SImageDescription{
        .transferFunction    = CM_TRANSFER_FUNCTION_ST2084_PQ,
        .primariesNameSet    = true,
        .primariesNamed      = primaries,
        .luminances          = {.min = 0.005, .max = 10000, .reference = 203},
        .masteringLuminances = {.min = 0.005, .max = masteringMax},
        .maxCLL              = maxCLL,
    });
}

static SImageDescription pq(uint32_t masteringMax, uint32_t maxCLL, ePrimaries primaries = CM_PRIMARIES_BT2020) {
    return pqDescription(masteringMax, maxCLL, primaries)->value();
}

static SOutputEncodingInputs inputs(NCMType::eCMType rule, int64_t autoHDR, eFullscreenContent content) {
    return {
        .rule       = rule,
        .caps       = HDR_OUTPUT,
        .autoHDR    = autoHDR,
        .fullscreen = content != FS_CONTENT_NONE,
        .content    = content,
    };
}

static NCMType::eCMType configured(NCMType::eCMType rule, const SOutputColorCapabilities& caps) {
    return resolveConfiguredCMType(rule, caps).cmType;
}

// Fullscreen surface encodings and how auto HDR classifies them.
TEST(OutputEncoding, ClassifiesFullscreenContent) {
    EXPECT_EQ(classifyFullscreenContent(nullptr), FS_CONTENT_NONE);
    EXPECT_EQ(classifyFullscreenContent(describe(CM_TRANSFER_FUNCTION_SRGB)), FS_CONTENT_SDR);
    EXPECT_EQ(classifyFullscreenContent(describe(CM_TRANSFER_FUNCTION_GAMMA22)), FS_CONTENT_SDR);
    EXPECT_EQ(classifyFullscreenContent(describe(CM_TRANSFER_FUNCTION_ST2084_PQ)), FS_CONTENT_HDR);
    EXPECT_EQ(classifyFullscreenContent(describe(CM_TRANSFER_FUNCTION_HLG)), FS_CONTENT_HDR);
    EXPECT_EQ(classifyFullscreenContent(SCRGB_IMAGE_DESCRIPTION), FS_CONTENT_HDR);

    // Extended linear within its primary volume and without highlights is SDR.
    EXPECT_EQ(classifyFullscreenContent(describe(CM_TRANSFER_FUNCTION_EXT_LINEAR, {.min = 0, .max = 80, .reference = 80})), FS_CONTENT_SDR);
    // A primary maximum above reference white is HDR.
    EXPECT_EQ(classifyFullscreenContent(describe(CM_TRANSFER_FUNCTION_EXT_LINEAR, {.min = 0, .max = 1000, .reference = 80})), FS_CONTENT_HDR);
}

// FP16 content with a 0-80 primary volume whose target volume (mastering luminance) or content light level reaches 1000 cd/m²:
// a signal of 12.5 is 1000 cd/m², color-management-v1 explicitly allows the target volume to exceed the primary volume.
TEST(OutputEncoding, ExtendedLinearHeadroomBeyondThePrimaryVolumeIsHDR) {
    const SImageDescription::SPCLuminances PRIMARY = {.min = 0, .max = 80, .reference = 80};

    const auto                             MASTERED = describe(CM_TRANSFER_FUNCTION_EXT_LINEAR, PRIMARY, 1000);
    EXPECT_TRUE(isHDR(MASTERED->value()));
    EXPECT_EQ(classifyFullscreenContent(MASTERED), FS_CONTENT_HDR);

    const auto CONTENT_LIGHT = describe(CM_TRANSFER_FUNCTION_EXT_LINEAR, PRIMARY, 0, 1000);
    EXPECT_TRUE(isHDR(CONTENT_LIGHT->value()));

    // A target volume ending at reference white has no highlights.
    EXPECT_FALSE(isHDR(describe(CM_TRANSFER_FUNCTION_EXT_LINEAR, PRIMARY, 80)->value()));

    // and auto HDR switches the output for it
    auto       in       = inputs(NCMType::CM_SRGB, 1, classifyFullscreenContent(MASTERED));
    const auto ENCODING = resolveOutputEncoding(in);
    EXPECT_TRUE(ENCODING.hdr());
    EXPECT_TRUE(ENCODING.autoHDR);
}

TEST(OutputEncoding, ConfiguredPresetCapabilityFallbacks) {
    EXPECT_EQ(resolveConfiguredCMType(NCMType::CM_HDR, HDR_OUTPUT), (SConfiguredCMType{.cmType = NCMType::CM_HDR}));
    EXPECT_EQ(configured(NCMType::CM_HDR_EDID, HDR_OUTPUT), NCMType::CM_HDR_EDID);
    EXPECT_EQ(configured(NCMType::CM_WIDE, HDR_OUTPUT), NCMType::CM_WIDE);
    EXPECT_EQ(configured(NCMType::CM_AUTO, HDR_OUTPUT), NCMType::CM_WIDE);
    EXPECT_EQ(configured(NCMType::CM_EDID, HDR_OUTPUT), NCMType::CM_EDID);
    EXPECT_EQ(configured(NCMType::CM_DCIP3, HDR_OUTPUT), NCMType::CM_DCIP3);

    auto caps = HDR_OUTPUT;
    caps.hdr  = false; // supports_hdr = -1, or no PQ in the EDID
    EXPECT_EQ(resolveConfiguredCMType(NCMType::CM_HDR, caps), (SConfiguredCMType{.cmType = NCMType::CM_SRGB, .fallback = CM_FALLBACK_CAPABILITY}));
    EXPECT_EQ(configured(NCMType::CM_HDR_EDID, caps), NCMType::CM_SRGB);
    EXPECT_EQ(configured(NCMType::CM_WIDE, caps), NCMType::CM_WIDE);

    caps.enabled10bit = false;
    EXPECT_EQ(configured(NCMType::CM_AUTO, caps), NCMType::CM_SRGB);

    caps.edidChromaticities = false;
    EXPECT_EQ(configured(NCMType::CM_EDID, caps), NCMType::CM_SRGB);
}

// cm = wide keeps its existing behaviour (owner decision 2026-11-24, OUT-04 not fixed): only a connector without BT2020_RGB falls back.
TEST(OutputEncoding, ExplicitWideKeepsItsExistingBehaviour) {
    auto caps      = HDR_OUTPUT;
    caps.hdr       = false;
    caps.wideColor = false; // no BT.2020 in the EDID, or supports_wide_color = -1
    EXPECT_EQ(resolveConfiguredCMType(NCMType::CM_WIDE, caps), (SConfiguredCMType{.cmType = NCMType::CM_WIDE}));
    caps.shaderCM = false;
    EXPECT_EQ(resolveConfiguredCMType(NCMType::CM_WIDE, caps), (SConfiguredCMType{.cmType = NCMType::CM_WIDE}));

    caps.bt2020Signal = false;
    EXPECT_EQ(resolveConfiguredCMType(NCMType::CM_WIDE, caps), (SConfiguredCMType{.cmType = NCMType::CM_SRGB, .fallback = CM_FALLBACK_CAPABILITY}));
}

// ICC-03: with render:cm_enabled = false only presets that would signal PQ for unconverted pixels change. SDR presets keep their
// previous behaviour, wide and auto included (owner decision 2026-11-24).
TEST(OutputEncoding, DisabledShaderCMResolvesOnlyIncompatiblePresets) {
    auto caps     = HDR_OUTPUT;
    caps.shaderCM = false;

    for (const auto rule : {NCMType::CM_HDR, NCMType::CM_HDR_EDID}) {
        SCOPED_TRACE(NCMType::toString(rule));
        EXPECT_EQ(resolveConfiguredCMType(rule, caps), (SConfiguredCMType{.cmType = NCMType::CM_SRGB, .fallback = CM_FALLBACK_NO_SHADER_CM}));
    }

    for (const auto rule : {NCMType::CM_AUTO, NCMType::CM_WIDE, NCMType::CM_SRGB, NCMType::CM_EDID, NCMType::CM_DCIP3, NCMType::CM_DP3, NCMType::CM_ADOBE}) {
        SCOPED_TRACE(NCMType::toString(rule));
        EXPECT_EQ(resolveConfiguredCMType(rule, caps), resolveConfiguredCMType(rule, HDR_OUTPUT));
    }

    // auto HDR can't switch to PQ either
    for (const auto rule : {NCMType::CM_SRGB, NCMType::CM_DCIP3, NCMType::CM_WIDE, NCMType::CM_HDR}) {
        auto in = inputs(rule, 1, FS_CONTENT_HDR);
        in.caps = caps;
        EXPECT_FALSE(resolveOutputEncoding(in).hdr());
        EXPECT_FALSE(autoHDRCMType(in).has_value());
    }
}

// The decision table the renderer, the connector signalling and the preferred description all follow.
TEST(OutputEncoding, DecisionTable) {
    struct SCase {
        NCMType::eCMType   rule;
        int64_t            autoHDR;
        eFullscreenContent content;
        NCMType::eCMType   expected;
        bool               autoSelected;
    };

    const std::vector<SCase> CASES = {
        // SDR preset: auto HDR follows HDR content only
        {NCMType::CM_SRGB, 0, FS_CONTENT_NONE, NCMType::CM_SRGB, false},
        {NCMType::CM_SRGB, 0, FS_CONTENT_SDR, NCMType::CM_SRGB, false},
        {NCMType::CM_SRGB, 0, FS_CONTENT_HDR, NCMType::CM_SRGB, false},
        {NCMType::CM_SRGB, 1, FS_CONTENT_NONE, NCMType::CM_SRGB, false},
        {NCMType::CM_SRGB, 1, FS_CONTENT_SDR, NCMType::CM_SRGB, false},
        {NCMType::CM_SRGB, 1, FS_CONTENT_HDR, NCMType::CM_HDR, true},
        {NCMType::CM_SRGB, 2, FS_CONTENT_SDR, NCMType::CM_SRGB, false},
        {NCMType::CM_SRGB, 2, FS_CONTENT_HDR, NCMType::CM_HDR_EDID, true},
        {NCMType::CM_WIDE, 1, FS_CONTENT_HDR, NCMType::CM_HDR, true},
        {NCMType::CM_WIDE, 1, FS_CONTENT_SDR, NCMType::CM_WIDE, false},
        {NCMType::CM_DP3, 1, FS_CONTENT_HDR, NCMType::CM_HDR, true},
        {NCMType::CM_ADOBE, 2, FS_CONTENT_SDR, NCMType::CM_ADOBE, false},
        // fixed HDR preset: stays HDR for every fullscreen surface, including tagged SDR, whatever auto HDR is set to
        {NCMType::CM_HDR, 0, FS_CONTENT_NONE, NCMType::CM_HDR, false},
        {NCMType::CM_HDR, 0, FS_CONTENT_SDR, NCMType::CM_HDR, false},
        {NCMType::CM_HDR, 1, FS_CONTENT_SDR, NCMType::CM_HDR, false},
        {NCMType::CM_HDR, 1, FS_CONTENT_HDR, NCMType::CM_HDR, false},
        {NCMType::CM_HDR, 2, FS_CONTENT_SDR, NCMType::CM_HDR, false},
        {NCMType::CM_HDR, 2, FS_CONTENT_HDR, NCMType::CM_HDR, false},
        {NCMType::CM_HDR_EDID, 1, FS_CONTENT_SDR, NCMType::CM_HDR_EDID, false},
        {NCMType::CM_HDR_EDID, 1, FS_CONTENT_HDR, NCMType::CM_HDR_EDID, false},
    };

    for (const auto& c : CASES) {
        SCOPED_TRACE(std::format("rule {} auto {} content {}", NCMType::toString(c.rule), c.autoHDR, sc<int>(c.content)));
        const auto ENCODING = resolveOutputEncoding(inputs(c.rule, c.autoHDR, c.content));
        EXPECT_FALSE(ENCODING.icc);
        EXPECT_EQ(ENCODING.cmType, c.expected);
        EXPECT_EQ(ENCODING.autoHDR, c.autoSelected);
        EXPECT_EQ(ENCODING.hdr(), NCMType::isHDR(c.expected));
    }
}

// Exhaustive invariants over every input combination.
TEST(OutputEncoding, InvariantsHoldForEveryInput) {
    for (const auto rule :
         {NCMType::CM_AUTO, NCMType::CM_SRGB, NCMType::CM_WIDE, NCMType::CM_EDID, NCMType::CM_HDR, NCMType::CM_HDR_EDID, NCMType::CM_DCIP3, NCMType::CM_DP3, NCMType::CM_ADOBE}) {
        for (const int64_t autoHDR : {0, 1, 2}) {
            for (const auto content : {FS_CONTENT_NONE, FS_CONTENT_SDR, FS_CONTENT_HDR}) {
                for (int flags = 0; flags < 512; ++flags) {
                    SOutputEncodingInputs in   = inputs(rule, autoHDR, content);
                    in.iccProfile              = (flags & 1) != 0;
                    in.caps.hdr                = (flags & 2) != 0;
                    in.caps.wideColor          = in.caps.hdr || (flags & 4) != 0;
                    in.caps.shaderCM           = (flags & 8) != 0;
                    in.noAutoHDR               = (flags & 16) != 0;
                    in.caps.bt2020Signal       = in.caps.wideColor || (flags & 64) != 0;
                    in.caps.enabled10bit       = (flags & 128) != 0;
                    in.caps.edidChromaticities = (flags & 256) != 0;
                    in.fullscreen              = content != FS_CONTENT_NONE || (flags & 32) != 0;

                    SCOPED_TRACE(std::format("rule {} auto {} content {} flags {}", NCMType::toString(rule), autoHDR, sc<int>(content), flags));
                    const auto ENCODING = resolveOutputEncoding(in);
                    const auto BASELINE = configured(rule, in.caps);

                    // An ICC profile is never replaced and never signalled as HDR.
                    if (in.iccProfile) {
                        EXPECT_TRUE(ENCODING.icc);
                        EXPECT_FALSE(ENCODING.hdr());
                        EXPECT_FALSE(autoHDRCMType(in).has_value());
                        continue;
                    }

                    // PQ only on outputs that can signal it, and only with shader CM converting into it.
                    EXPECT_TRUE(!ENCODING.hdr() || (in.caps.hdr && in.caps.shaderCM));
                    // BT.2020 only where it can be signalled.
                    EXPECT_TRUE(ENCODING.cmType != NCMType::CM_WIDE || in.caps.bt2020Signal);
                    // SDR presets don't depend on shader CM.
                    if (!NCMType::isHDR(rule)) {
                        auto withCM     = in.caps;
                        withCM.shaderCM = true;
                        EXPECT_EQ(BASELINE, configured(rule, withCM));
                    }

                    // A configured HDR preset keeps its encoding for every fullscreen surface.
                    EXPECT_TRUE(!NCMType::isHDR(BASELINE) || ENCODING.cmType == BASELINE);

                    // Without auto HDR, or with the window rule, the output follows its configuration, whatever happened before.
                    const bool AUTO_OFF = autoHDR == 0 || in.noAutoHDR || content != FS_CONTENT_HDR;
                    EXPECT_TRUE(!AUTO_OFF || ENCODING.cmType == BASELINE);

                    // The preferred description offered to the fullscreen surface is the description auto HDR switches to.
                    auto withHDR    = in;
                    withHDR.content = FS_CONTENT_HDR;
                    const auto HDR  = resolveOutputEncoding(withHDR);
                    EXPECT_EQ(autoHDRCMType(in).has_value(), HDR.autoHDR);
                    EXPECT_EQ(autoHDRCMType(in).value_or(BASELINE), HDR.cmType);

                    // CMonitor::outputEncodingInputs reads the window rule and searches the content only where auto HDR could act.
                    if (!autoHDRApplicable(in)) {
                        auto unresolved      = in;
                        unresolved.content   = FS_CONTENT_NONE;
                        unresolved.noAutoHDR = false;
                        EXPECT_EQ(resolveOutputEncoding(unresolved), ENCODING);
                    }

                    // auto HDR kept from switching by missing shader CM is reported, the baseline's own fallback first
                    const bool SUPPRESSED = content == FS_CONTENT_HDR && !in.noAutoHDR && !in.caps.shaderCM && autoHDRApplicable(in);
                    const auto FALLBACK   = resolveConfiguredCMType(rule, in.caps).fallback;
                    EXPECT_EQ(ENCODING.fallback, FALLBACK != CM_FALLBACK_NONE ? FALLBACK : (SUPPRESSED ? CM_FALLBACK_AUTO_HDR_NO_SHADER_CM : CM_FALLBACK_NONE));
                }
            }
        }
    }
}

TEST(OutputEncoding, AutoHDRNeedsAFullscreenWindow) {
    auto in       = inputs(NCMType::CM_SRGB, 1, FS_CONTENT_HDR);
    in.fullscreen = false;
    EXPECT_FALSE(autoHDRCMType(in).has_value());
    EXPECT_FALSE(resolveOutputEncoding(in).hdr());
}

// The description has to follow every change of the decision, including auto HDR being disabled while it holds the output in HDR.
TEST(OutputEncoding, RuntimeTransitionsRebuildTheDescription) {
    const auto AUTO_HDR = resolveOutputEncoding(inputs(NCMType::CM_SRGB, 1, FS_CONTENT_HDR));
    ASSERT_EQ(AUTO_HDR.cmType, NCMType::CM_HDR);
    EXPECT_TRUE(outputEncodingNeedsApply(AUTO_HDR, NCMType::CM_SRGB, false));
    EXPECT_FALSE(outputEncodingNeedsApply(AUTO_HDR, NCMType::CM_HDR, false));

    const auto DISABLED = resolveOutputEncoding(inputs(NCMType::CM_SRGB, 0, FS_CONTENT_HDR));
    EXPECT_EQ(DISABLED.cmType, NCMType::CM_SRGB);
    EXPECT_TRUE(outputEncodingNeedsApply(DISABLED, NCMType::CM_HDR, false));

    auto icc       = inputs(NCMType::CM_SRGB, 1, FS_CONTENT_HDR);
    icc.iccProfile = true;
    EXPECT_FALSE(outputEncodingNeedsApply(resolveOutputEncoding(icc), NCMType::CM_SRGB, true));
    EXPECT_FALSE(outputEncodingNeedsApply(resolveOutputEncoding(icc), std::nullopt, true));
    EXPECT_TRUE(outputEncodingNeedsApply(resolveOutputEncoding(inputs(NCMType::CM_SRGB, 1, FS_CONTENT_NONE)), NCMType::CM_SRGB, true));
    EXPECT_TRUE(outputEncodingNeedsApply(resolveOutputEncoding(inputs(NCMType::CM_SRGB, 1, FS_CONTENT_NONE)), std::nullopt, false));
}

// One boundary for shader tone mapping, scanout admission, the CTM and CImageDescription::needsCM, at, below and above 1.01 × peak.
TEST(OutputEncoding, PeakBoundaryIsSharedByShaderScanoutAndCTM) {
    const auto OUTPUT = pqDescription(600, 0);
    EXPECT_FLOAT_EQ(displayPeakLuminance(OUTPUT->value()), 600);

    Render::CCMSettingsCache        cache;
    const Render::SCMSettingsInputs SHADER  = {.sourceTF = CM_TRANSFER_FUNCTION_ST2084_PQ};
    const SScanoutCMState           SCANOUT = {.policy = CM_NS_ONDEMAND, .outputSignalsHDR = true, .compatibleTF = true, .needsCM = true};

    struct SCase {
        uint32_t peak;
        bool     toneMapped;
    };
    for (const auto& c : {SCase{.peak = 605, .toneMapped = false}, SCase{.peak = 606, .toneMapped = true}, SCase{.peak = 607, .toneMapped = true}}) {
        SCOPED_TRACE(c.peak);
        const auto SOURCE = pqDescription(c.peak, 0);

        EXPECT_EQ(peakNeedsToneMapping(c.peak, 600), c.toneMapped);
        EXPECT_EQ(contentFitsDisplayPeak(SOURCE->value(), OUTPUT->value()), !c.toneMapped);
        EXPECT_EQ(cache.get(SOURCE, OUTPUT, SHADER).needsTonemap, c.toneMapped);
        EXPECT_EQ(SOURCE->needsCM(OUTPUT), c.toneMapped);
        EXPECT_EQ(scanoutBlockedByCM(SOURCE->value(), OUTPUT->value(), SCANOUT), c.toneMapped);
        EXPECT_EQ(ctmConvertsExactly(SOURCE->value(), OUTPUT->value(), true, true), !c.toneMapped);
        EXPECT_EQ(hdrPassthroughCompatible(SOURCE->value(), OUTPUT->value()), !c.toneMapped);
    }

    // MaxCLL wins over the mastering peak
    EXPECT_TRUE(contentFitsDisplayPeak(pq(4000, 500), OUTPUT->value()));
    EXPECT_FALSE(contentFitsDisplayPeak(pq(4000, 0), OUTPUT->value()));

    // non_shader_cm = ignore keeps its explicit exception
    auto ignore   = SCANOUT;
    ignore.policy = CM_NS_IGNORE;
    EXPECT_FALSE(scanoutBlockedByCM(pq(4000, 4000), OUTPUT->value(), ignore));
}

// The CTM acts on the output signal, so a primaries conversion is only exact between linear encodings.
TEST(OutputEncoding, CTMConvertsPrimariesOfLinearSignalsOnly) {
    const auto SRGB_SOURCE    = describe(CM_TRANSFER_FUNCTION_SRGB)->value();
    auto       wideOutput     = SRGB_SOURCE;
    wideOutput.primariesNamed = CM_PRIMARIES_BT2020;

    // sRGB pixels on a BT.2020 sRGB-encoded output: even with an sRGB work buffer the CTM can't convert them
    EXPECT_FALSE(ctmConvertsExactly(SRGB_SOURCE, wideOutput, true, true));
    EXPECT_TRUE(ctmConvertsExactly(SRGB_SOURCE, SRGB_SOURCE, true, false));

    auto linearSource             = SRGB_SOURCE;
    linearSource.transferFunction = CM_TRANSFER_FUNCTION_LINEAR;
    auto linearOutput             = wideOutput;
    linearOutput.transferFunction = CM_TRANSFER_FUNCTION_LINEAR;
    EXPECT_TRUE(ctmConvertsExactly(linearSource, linearOutput, true, true));
    EXPECT_FALSE(ctmConvertsExactly(linearSource, linearOutput, true, false));
    EXPECT_FALSE(ctmConvertsExactly(linearSource, linearOutput, false, true));

    EXPECT_FALSE(ctmConvertsExactly(pq(600, 0, CM_PRIMARIES_DCI_P3), pq(600, 0), true, true));
}

static constexpr SFullscreenCTMInputs ELIGIBLE = {
    .policy           = CM_NS_ALWAYS,
    .fullscreen       = true,
    .source           = 1,
    .solitarySource   = true,
    .target           = 2,
    .needsCM          = true,
    .convertsToOutput = true,
    .convertsToWork   = true,
    .coversFrame      = true,
};

// The bypass skips shader CM for every draw of the frame, so it is only granted when the CTM converts all of them.
TEST(OutputEncoding, FullscreenCTMCoversTheWholeFrame) {
    EXPECT_EQ(decideFullscreenCTM(ELIGIBLE), FS_CTM_CONVERT);

    // a software cursor, popups, layers or a DPMS fade are drawn too (not solitary)
    auto cursor        = ELIGIBLE;
    cursor.coversFrame = false;
    EXPECT_EQ(decideFullscreenCTM(cursor), FS_CTM_RESTORE);
    cursor.userCTM = true;
    EXPECT_EQ(decideFullscreenCTM(cursor), FS_CTM_IDENTITY);
    cursor.interop = true;
    EXPECT_EQ(decideFullscreenCTM(cursor), FS_CTM_RESTORE);
    // ... which a direct scanout never has
    cursor.scanout = true;
    EXPECT_EQ(decideFullscreenCTM(cursor), FS_CTM_CONVERT);

    // FP16 work buffer: composited pixels would pass through a buffer in another encoding (RND-01 keeps HEAD's refusal)
    auto fp16           = ELIGIBLE;
    fp16.convertsToWork = false;
    EXPECT_EQ(decideFullscreenCTM(fp16), FS_CTM_RESTORE);
    fp16.scanout = true;
    EXPECT_EQ(decideFullscreenCTM(fp16), FS_CTM_CONVERT);

    // The CTM converts the solitary resource only: the buffer scanout submits and all a solitary frame shows. Without it, e.g. a window
    // whose content description isn't the description of its pixels, nothing is converted.
    auto unknown           = ELIGIBLE;
    unknown.solitarySource = false;
    EXPECT_EQ(decideFullscreenCTM(unknown), FS_CTM_RESTORE);
    unknown.scanout = true;
    EXPECT_EQ(decideFullscreenCTM(unknown), FS_CTM_RESTORE);

    // only the output, not the work buffer: RND-01
    auto outputOnly             = ELIGIBLE;
    outputOnly.convertsToOutput = false;
    EXPECT_EQ(decideFullscreenCTM(outputOnly), FS_CTM_RESTORE);

    // copies for capture, mirrors or the unmodified copy: CAP-01
    auto copied        = ELIGIBLE;
    copied.frameCopied = true;
    EXPECT_EQ(decideFullscreenCTM(copied), FS_CTM_RESTORE);

    // policies
    auto onDemand   = ELIGIBLE;
    onDemand.policy = CM_NS_ONDEMAND;
    EXPECT_EQ(decideFullscreenCTM(onDemand), FS_CTM_RESTORE);
    onDemand.scanout = true;
    EXPECT_EQ(decideFullscreenCTM(onDemand), FS_CTM_CONVERT);

    auto ignore    = ELIGIBLE;
    ignore.policy  = CM_NS_IGNORE;
    ignore.userCTM = true;
    EXPECT_EQ(decideFullscreenCTM(ignore), FS_CTM_RESTORE);

    auto disable             = ELIGIBLE;
    disable.policy           = CM_NS_DISABLE;
    disable.convertsToOutput = false;
    EXPECT_EQ(decideFullscreenCTM(disable), FS_CTM_RESTORE);
    disable.userCTM = true;
    EXPECT_EQ(decideFullscreenCTM(disable), FS_CTM_IDENTITY);

    auto none       = ELIGIBLE;
    none.fullscreen = false;
    EXPECT_EQ(decideFullscreenCTM(none), FS_CTM_RESTORE);

    // A conversion the CTM can't stage keeps shader CM.
    EXPECT_EQ(fullscreenCTMWithoutConversion(ELIGIBLE), FS_CTM_RESTORE);
    auto suppressed    = ELIGIBLE;
    suppressed.userCTM = true;
    EXPECT_EQ(fullscreenCTMWithoutConversion(suppressed), FS_CTM_IDENTITY);
}

// Every evaluated input is part of the cached decision: a content type change flips the interop result without changing the window or
// the descriptions, and needs a new decision.
TEST(OutputEncoding, FullscreenCTMKeyContainsEveryEligibilityInput) {
    auto video    = ELIGIBLE;
    video.userCTM = true;
    auto none     = video;
    none.interop  = true; // non_shader_cm_interop = 2 and content type none
    EXPECT_NE(video, none);

    video.coversFrame = false;
    none.coversFrame  = false;
    EXPECT_EQ(decideFullscreenCTM(video), FS_CTM_IDENTITY);
    EXPECT_EQ(decideFullscreenCTM(none), FS_CTM_RESTORE);

    auto cursor        = ELIGIBLE;
    cursor.coversFrame = false;
    EXPECT_NE(cursor, ELIGIBLE);
    auto fp16           = ELIGIBLE;
    fp16.convertsToWork = false;
    EXPECT_NE(fp16, ELIGIBLE);
    auto child           = ELIGIBLE;
    child.solitarySource = false;
    EXPECT_NE(child, ELIGIBLE);
}

// Only the output frame goes through the CTM. Captures (render modes other than normal) and other scenes keep their shader conversion.
TEST(OutputEncoding, ShaderBypassOnlyForTheOutputFrame) {
    EXPECT_TRUE(shaderCMBypassed(true, true, false));
    EXPECT_FALSE(shaderCMBypassed(true, false, false));
    EXPECT_FALSE(shaderCMBypassed(true, true, true));
    EXPECT_FALSE(shaderCMBypassed(false, true, false));
}

// Finding 5 of the second review: auto HDR that only shader CM keeps from switching to PQ reports it, while the HDR content is shown.
TEST(OutputEncoding, AutoHDRWithoutShaderCMReportsTheSuppressedSwitch) {
    auto in          = inputs(NCMType::CM_SRGB, 1, FS_CONTENT_HDR);
    in.caps.shaderCM = false;
    EXPECT_EQ(resolveOutputEncoding(in), (SOutputEncoding{.fallback = CM_FALLBACK_AUTO_HDR_NO_SHADER_CM}));
    EXPECT_TRUE(autoHDRApplicable(in));
    EXPECT_FALSE(autoHDRCMType(in).has_value());

    // the condition ends with the HDR content, the window rule, auto HDR off, or shader CM back
    for (const auto content : {FS_CONTENT_NONE, FS_CONTENT_SDR}) {
        auto sdr    = in;
        sdr.content = content;
        EXPECT_EQ(resolveOutputEncoding(sdr).fallback, CM_FALLBACK_NONE);
    }
    auto rule      = in;
    rule.noAutoHDR = true;
    EXPECT_EQ(resolveOutputEncoding(rule).fallback, CM_FALLBACK_NONE);
    auto off    = in;
    off.autoHDR = 0;
    EXPECT_EQ(resolveOutputEncoding(off).fallback, CM_FALLBACK_NONE);
    EXPECT_EQ(resolveOutputEncoding(inputs(NCMType::CM_SRGB, 1, FS_CONTENT_HDR)).fallback, CM_FALLBACK_NONE);

    // where auto HDR could never act, nothing is reported: no HDR support, an ICC profile
    auto sdrOutput     = in;
    sdrOutput.caps.hdr = false;
    EXPECT_FALSE(autoHDRApplicable(sdrOutput));
    EXPECT_EQ(resolveOutputEncoding(sdrOutput).fallback, CM_FALLBACK_NONE);
    auto icc       = in;
    icc.iccProfile = true;
    EXPECT_FALSE(autoHDRApplicable(icc));
    EXPECT_EQ(resolveOutputEncoding(icc).fallback, CM_FALLBACK_NONE);

    // a configured HDR preset reports its own fallback, once
    auto hdr = in;
    hdr.rule = NCMType::CM_HDR;
    EXPECT_EQ(resolveOutputEncoding(hdr).fallback, CM_FALLBACK_NO_SHADER_CM);

    // wide keeps its behaviour (owner decision 2026-11-24): BT.2020 is still signalled for the SDR desktop
    auto wide = in;
    wide.rule = NCMType::CM_WIDE;
    EXPECT_EQ(resolveOutputEncoding(wide), (SOutputEncoding{.baseline = NCMType::CM_WIDE, .cmType = NCMType::CM_WIDE, .fallback = CM_FALLBACK_AUTO_HDR_NO_SHADER_CM}));
}

// Finding 4 of the second review: a composited frame carries the surface's own mastering metadata only while shader CM passes its pixels
// unchanged. The peak (tone mapping) and the primaries (gamut conversion) both change them.
TEST(OutputEncoding, CompositeReproducesOnlyUnconvertedPQ) {
    const auto OUTPUT = pq(600, 0);
    EXPECT_TRUE(compositeReproducesSurface(pq(600, 400), OUTPUT));
    EXPECT_FALSE(compositeReproducesSurface(pq(4000, 0), OUTPUT));
    EXPECT_FALSE(compositeReproducesSurface(pq(600, 400, CM_PRIMARIES_DCI_P3), OUTPUT));
    EXPECT_FALSE(compositeReproducesSurface(describe(CM_TRANSFER_FUNCTION_SRGB)->value(), describe(CM_TRANSFER_FUNCTION_SRGB)->value()));

    // scanout admission keeps its own rule: matching primaries or a CTM that converts them
    EXPECT_TRUE(hdrPassthroughCompatible(pq(600, 400, CM_PRIMARIES_DCI_P3), OUTPUT));
    const SScanoutCMState SCANOUT = {.policy = CM_NS_ONDEMAND, .outputSignalsHDR = true, .compatibleTF = true, .needsCM = true};
    EXPECT_TRUE(scanoutBlockedByCM(pq(600, 400, CM_PRIMARIES_DCI_P3), OUTPUT, SCANOUT));
    EXPECT_FALSE(scanoutBlockedByCM(pq(600, 400), OUTPUT, SCANOUT));
}

// Finding 3 and 6 of the second review: metadata decisions compare with the output's own metadata only when the outcome depends on it.
TEST(OutputEncoding, HDRMetadataDecisionBuildsTheOutputMetadataOnDemand) {
    int        built  = 0;
    const auto STAGED = [&] {
        ++built;
        return true;
    };

    // sustained HDR scanout of an admitted surface
    const SHDRMetadataInputs SCANOUT = {
        .hdrSupported     = true,
        .wantHDR          = true,
        .stagedHDR        = true,
        .initialized      = true,
        .scanoutCandidate = true,
        .fromSurface      = true,
        .surfaceWindow    = true,
    };
    EXPECT_EQ(decideHDRMetadata(SCANOUT, STAGED), (SHDRMetadataDecision{.keepSurface = true, .initialized = false}));
    EXPECT_EQ(built, 0);

    // a composited frame that still shows the surface alone and unchanged
    auto composited              = SCANOUT;
    composited.scanoutCandidate  = false;
    composited.surfaceComposites = true;
    EXPECT_EQ(decideHDRMetadata(composited, STAGED), (SHDRMetadataDecision{.source = HDR_METADATA_SURFACE, .keepSurface = true}));
    EXPECT_EQ(built, 0);

    // ... anything else uses the output's metadata, however long the window stays
    composited.surfaceComposites = false;
    EXPECT_EQ(decideHDRMetadata(composited, [] { return false; }), (SHDRMetadataDecision{.source = HDR_METADATA_OUTPUT}));

    // stable SDR
    const SHDRMetadataInputs SDR = {.hdrSupported = true, .initialized = true};
    EXPECT_EQ(decideHDRMetadata(SDR, STAGED), SHDRMetadataDecision{});
    EXPECT_EQ(built, 0);
}
