#include <helpers/cm/ICC.hpp>
#include <gtest/gtest.h>
#include <Compositor.hpp>
#include <config/ConfigValue.hpp>
#include <config/lua/ConfigManager.hpp>
#include <config/shared/inotify/ConfigWatcher.hpp>
#include <config/supplementary/jeremy/Jeremy.hpp>

using namespace NColorManagement;

class CICCProfileTest : public ::testing::Test {
  protected:
    void SetUp() override {
        m_profile = cmsCreate_sRGBProfile();
        ASSERT_NE(m_profile, nullptr);
    }

    void TearDown() override {
        if (m_profile)
            cmsCloseProfile(m_profile);
    }

    cmsHPROFILE m_profile = nullptr;
};

TEST_F(CICCProfileTest, RecoversNativeD65AndSRGBPrimaries) {
    const auto result = iccPrimaries(m_profile);
    ASSERT_TRUE(result.has_value()) << result.error();
    const auto& expected = NColorPrimaries::BT709;
    for (const auto& pair :
         {std::pair{result->red, expected.red}, std::pair{result->green, expected.green}, std::pair{result->blue, expected.blue}, std::pair{result->white, expected.white}}) {
        EXPECT_NEAR(pair.first.x, pair.second.x, 2e-4);
        EXPECT_NEAR(pair.first.y, pair.second.y, 2e-4);
    }
}

TEST_F(CICCProfileTest, SRGBProfileLUTIsIdentityInEncodedDomain) {
    SImageDescription image;
    const auto        result = iccBuildLUTData(m_profile, image);
    ASSERT_TRUE(result.has_value()) << result.error();

    const size_t N = image.icc.lutSize;
    ASSERT_EQ(image.icc.lutDataPacked.size(), N * N * N * 3);

    // An sRGB display must not alter sRGB content, including the shadows a linear-light grid used to crush.
    for (size_t b = 0; b < N; b += 4) {
        for (size_t g = 0; g < N; g += 4) {
            for (size_t r = 0; r < N; ++r) {
                const size_t o = ((b * N + g) * N + r) * 3;
                EXPECT_NEAR(image.icc.lutDataPacked[o + 0], r / float(N - 1), 2e-3);
                EXPECT_NEAR(image.icc.lutDataPacked[o + 1], g / float(N - 1), 2e-3);
                EXPECT_NEAR(image.icc.lutDataPacked[o + 2], b / float(N - 1), 2e-3);
            }
        }
    }
}

TEST_F(CICCProfileTest, P3LUTMatchesDirectTransformForExtendedWorkBufferColors) {
    const cmsCIExyY       white{0.3127, 0.3290, 1};
    const cmsCIExyYTRIPLE p3{{0.68, 0.32, 1}, {0.265, 0.69, 1}, {0.15, 0.06, 1}};
    const cmsCIExyYTRIPLE bt709{{0.64, 0.33, 1}, {0.30, 0.60, 1}, {0.15, 0.06, 1}};
    auto                  makeProfile = [&](const cmsCIExyYTRIPLE& primaries, double gamma) {
        auto          curve     = cmsBuildGamma(nullptr, gamma);
        cmsToneCurve* curves[3] = {curve, curve, curve};
        auto          result    = cmsCreateRGBProfile(&white, &primaries, curves);
        cmsFreeToneCurve(curve);
        return result;
    };
    cmsCloseProfile(m_profile);
    m_profile = makeProfile(p3, 2.2);
    ASSERT_NE(m_profile, nullptr);
    const auto recovered = iccPrimaries(m_profile);
    ASSERT_TRUE(recovered);
    SImageDescription image{.primaries = *recovered};
    ASSERT_TRUE(iccBuildLUTData(m_profile, image));

    const auto work = makeProfile(bt709, 1);
    ASSERT_NE(work, nullptr);
    const auto direct = cmsCreateTransform(work, TYPE_RGB_FLT, m_profile, TYPE_RGB_FLT, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_HIGHRESPRECALC);
    cmsCloseProfile(work);
    ASSERT_NE(direct, nullptr);
    const auto n       = image.icc.lutSize;
    const auto convert = CPrimaries::from(CM_PRIMARIES_SRGB)->convertMatrix(CPrimaries::from(image.getPrimaries()));
    // Extended BT.709 for P3 red and green, plus an interior color and a near-black grid sample.
    for (const std::array<float, 3> pixel :
         {std::array{1.224940f, -0.042057f, -0.019638f}, {-0.224940f, 1.042057f, -0.078636f}, {0.61f, 0.35f, 0.42f}, {0.00242f, 0.00242f, 0.00242f}}) {
        std::array<float, 3> expected{};
        cmsDoTransform(direct, pixel.data(), expected.data(), 1);
        const auto rgb = convert * Hyprgraphics::CColor::XYZ{.x = pixel[0], .y = pixel[1], .z = pixel[2]};
        // Model the shader's shaper and hardware trilinear lookup in grid coordinates.
        const auto coordinate = [&](double linear) {
            linear = std::clamp(linear, 0.0, 1.0);
            return std::clamp(linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055, 0.0, 1.0) * (n - 1);
        };
        const std::array      coords{coordinate(rgb.x), coordinate(rgb.y), coordinate(rgb.z)};
        std::array<double, 3> actual{};
        for (size_t corner = 0; corner < 8; ++corner) {
            size_t index  = 0;
            double weight = 1;
            for (int axis = 2; axis >= 0; --axis) {
                const auto base = sc<size_t>(coords[axis]);
                const auto frac = coords[axis] - base;
                const bool high = corner & (1U << axis);
                index           = index * n + std::min(base + high, n - 1);
                weight *= high ? frac : 1 - frac;
            }
            for (size_t c = 0; c < 3; ++c)
                actual[c] += weight * image.icc.lutDataPacked[index * 3 + c];
        }
        for (size_t c = 0; c < 3; ++c)
            EXPECT_NEAR(actual[c], std::clamp(expected[c], 0.f, 1.f), 2e-3);
    }
    cmsDeleteTransform(direct);
}

class CVCGTTest : public CICCProfileTest {
  protected:
    void reopenWithTag(const std::vector<uint8_t>& tag) {
        ASSERT_TRUE(cmsWriteRawTag(m_profile, cmsSigVcgtTag, tag.data(), tag.size()));
        cmsUInt32Number size = 0;
        ASSERT_TRUE(cmsSaveProfileToMem(m_profile, nullptr, &size));
        std::vector<uint8_t> bytes(size);
        ASSERT_TRUE(cmsSaveProfileToMem(m_profile, bytes.data(), &size));
        cmsCloseProfile(m_profile);
        m_bytes   = std::move(bytes);
        m_profile = cmsOpenProfileFromMem(m_bytes.data(), size);
        ASSERT_NE(m_profile, nullptr);
    }

    std::vector<uint8_t> m_bytes;
};

TEST_F(CVCGTTest, DecodesTableWidthsAndReplicatesSingleChannel) {
    for (uint8_t channels : {1, 3}) {
        for (uint8_t width : {1, 2}) {
            std::vector<uint8_t> tag{'v', 'c', 'g', 't', 0, 0, 0, 0, 0, 0, 0, 0, 0, channels, 0, 4, 0, width};
            for (uint8_t c = 0; c < channels; ++c)
                for (uint8_t value : {0, 85, 170, 255}) {
                    tag.push_back(value - c * (value > 0));
                    if (width == 2)
                        tag.push_back(42);
                }
            ASSERT_NO_FATAL_FAILURE(reopenWithTag(tag));
            const auto result = iccReadVCGT(m_profile);
            ASSERT_TRUE(result && *result);
            for (size_t c = 0; c < 3; ++c) {
                const int   delta = channels == 1 ? 0 : sc<int>(c);
                const auto& ramp  = (**result).ch[c];
                ASSERT_EQ(ramp.size(), 4U);
                EXPECT_EQ(ramp[1], width == 1 ? (85 - delta) * 257 : (85 - delta) * 256 + 42);
                EXPECT_EQ(ramp.back(), width == 1 ? (255 - delta) * 257 : (255 - delta) * 256 + 42);
            }
        }
    }
}

TEST_F(CVCGTTest, DecodesFormulaTriples) {
    std::vector<uint8_t> tag{'v', 'c', 'g', 't', 0, 0, 0, 0, 0, 0, 0, 1};
    for (uint8_t gamma : {1, 2, 3})
        tag.insert(tag.end(), {0, gamma, 0, 0, 0, 0, 64, 0, 0, 0, 192, 0});
    ASSERT_NO_FATAL_FAILURE(reopenWithTag(tag));
    const auto result = iccReadVCGT(m_profile);
    ASSERT_TRUE(result && *result);
    for (size_t c = 0; c < 3; ++c) {
        const auto& ramp = (**result).ch[c];
        ASSERT_GT(ramp.size(), 1U);
        const auto i = ramp.size() / 2;
        EXPECT_NEAR(ramp[i], 65535 * (0.25 + 0.5 * std::pow(sc<double>(i) / (ramp.size() - 1), c + 1)), 1);
        EXPECT_EQ(ramp.front(), 16384);
        EXPECT_EQ(ramp.back(), 49151);
    }
}

TEST_F(CVCGTTest, RejectsSingletonAndTruncatedTags) {
    std::vector<uint8_t> tag{'v', 'c', 'g', 't', 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 42};
    ASSERT_NO_FATAL_FAILURE(reopenWithTag(tag));
    EXPECT_FALSE(iccReadVCGT(m_profile));
    tag[15] = 2;
    ASSERT_NO_FATAL_FAILURE(reopenWithTag(tag));
    EXPECT_FALSE(iccReadVCGT(m_profile));
    tag[11] = 1;
    tag.resize(47);
    ASSERT_NO_FATAL_FAILURE(reopenWithTag(tag));
    EXPECT_FALSE(iccReadVCGT(m_profile));
}

TEST_F(CVCGTTest, ReadsLegacyReservedWordAndLowEndpoints) {
    const std::vector<uint8_t> tag{'v',
                                   'c',
                                   'g',
                                   't',
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   3,
                                   0,
                                   2,
                                   0,
                                   2,
                                   0,
                                   0,
                                   0,
                                   1,
                                   10,
                                   42,
                                   0,
                                   2,
                                   11,
                                   43,
                                   0,
                                   3,
                                   12,
                                   44};
    ASSERT_NO_FATAL_FAILURE(reopenWithTag(tag));
    const auto result = iccReadVCGT(m_profile);
    ASSERT_TRUE(result && *result);
    for (size_t c = 0; c < 3; ++c) {
        EXPECT_EQ((**result).ch[c].front(), c + 1);
        EXPECT_EQ((**result).ch[c].back(), (10 + c) * 256 + 42 + c);
    }
}

TEST_F(CVCGTTest, ResolvesDeclaredAlignmentAndRejectsAmbiguousTables) {
    std::vector<uint8_t> tag{'v', 'c', 'g', 't', 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 3, 0, 2, 0, 0};
    for (int channel = 0; channel < 3; ++channel)
        tag.insert(tag.end(), {0, 0, 128, 0, 255, 255});
    tag.resize(40);
    ASSERT_NO_FATAL_FAILURE(reopenWithTag(tag));
    const auto result = iccReadVCGT(m_profile);
    ASSERT_TRUE(result && *result);
    for (const auto& ramp : (**result).ch)
        EXPECT_EQ(ramp, (std::vector<uint16_t>{0, 32768, 65535}));

    tag.back() = 1;
    ASSERT_NO_FATAL_FAILURE(reopenWithTag(tag));
    EXPECT_FALSE(iccReadVCGT(m_profile));
    tag.pop_back();
    ASSERT_NO_FATAL_FAILURE(reopenWithTag(tag));
    EXPECT_FALSE(iccReadVCGT(m_profile));

    // Two shared 16-bit samples: a 24-byte tag can be aligned standard or exact legacy.
    tag = {'v', 'c', 'g', 't', 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 0, 2, 0, 0, 128, 0, 0, 0};
    ASSERT_NO_FATAL_FAILURE(reopenWithTag(tag));
    EXPECT_FALSE(iccReadVCGT(m_profile));
    tag[19] = 1;
    ASSERT_NO_FATAL_FAILURE(reopenWithTag(tag));
    const auto standard = iccReadVCGT(m_profile);
    ASSERT_TRUE(standard && *standard);
    EXPECT_EQ((**standard).ch[0], (std::vector<uint16_t>{1, 32768}));
}

TEST_F(CVCGTTest, RejectsInvalidTableHeaderFields) {
    for (const auto& [offset, value] : std::array<std::pair<size_t, uint8_t>, 5>{{{0, 'x'}, {11, 2}, {13, 2}, {15, 0}, {17, 3}}}) {
        std::vector<uint8_t> tag{'v', 'c', 'g', 't', 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 0, 1, 0, 255};
        tag[offset] = value;
        ASSERT_NO_FATAL_FAILURE(reopenWithTag(tag));
        EXPECT_FALSE(iccReadVCGT(m_profile));
    }
}

TEST_F(CICCProfileTest, RecoversCustomWhiteFromSerializedProfile) {
    const cmsCIExyY                                          white{0.314, 0.351, 1.0};
    const cmsCIExyYTRIPLE                                    primaries{{0.68, 0.32, 1.0}, {0.265, 0.69, 1.0}, {0.15, 0.06, 1.0}};
    const auto                                               freeCurve = [](cmsToneCurve* curve) { cmsFreeToneCurve(curve); };
    const std::unique_ptr<cmsToneCurve, decltype(freeCurve)> curve{cmsBuildGamma(nullptr, 2.2), freeCurve};
    ASSERT_NE(curve, nullptr);
    cmsToneCurve* curves[3] = {curve.get(), curve.get(), curve.get()};
    cmsCloseProfile(m_profile);
    m_profile = cmsCreateRGBProfile(&white, &primaries, curves);
    ASSERT_NE(m_profile, nullptr);
    cmsUInt32Number size = 0;
    ASSERT_TRUE(cmsSaveProfileToMem(m_profile, nullptr, &size));
    std::vector<uint8_t> bytes(size);
    ASSERT_TRUE(cmsSaveProfileToMem(m_profile, bytes.data(), &size));
    cmsCloseProfile(m_profile);
    m_profile = cmsOpenProfileFromMem(bytes.data(), size);
    ASSERT_NE(m_profile, nullptr);
    const auto result = iccPrimaries(m_profile);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_NEAR(result->white.x, white.x, 2e-4);
    EXPECT_NEAR(result->white.y, white.y, 2e-4);
    EXPECT_NEAR(result->red.x, primaries.Red.x, 2e-4);
    EXPECT_NEAR(result->green.y, primaries.Green.y, 2e-4);
    EXPECT_NEAR(result->blue.y, primaries.Blue.y, 2e-4);
}

TEST_F(CICCProfileTest, NativeWhiteWithoutAdaptationTag) {
    const cmsCIExyY white{0.3127, 0.3290, 1.0};
    cmsCIEXYZ       nativeWhite{};
    cmsxyY2XYZ(&nativeWhite, &white);
    ASSERT_TRUE(cmsWriteTag(m_profile, cmsSigChromaticAdaptationTag, nullptr));
    ASSERT_TRUE(cmsWriteTag(m_profile, cmsSigMediaWhitePointTag, &nativeWhite));
    const auto result = iccPrimaries(m_profile);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_NEAR(result->white.x, white.x, 1e-6);
    EXPECT_NEAR(result->white.y, white.y, 1e-6);
    EXPECT_NEAR(result->red.x, 0.64, 2e-4);
    EXPECT_NEAR(result->green.y, 0.60, 2e-4);
    EXPECT_NEAR(result->blue.y, 0.06, 2e-4);
}

TEST_F(CICCProfileTest, NativeWhiteWithAdaptationTagIsNotAdaptedTwice) {
    // v2-style profile: chad present, but wtpt already holds the native (D65) white.
    const cmsCIExyY white{0.3127, 0.3290, 1.0};
    cmsCIEXYZ       nativeWhite{};
    cmsxyY2XYZ(&nativeWhite, &white);
    ASSERT_TRUE(cmsIsTag(m_profile, cmsSigChromaticAdaptationTag));
    ASSERT_TRUE(cmsWriteTag(m_profile, cmsSigMediaWhitePointTag, &nativeWhite));
    const auto result = iccPrimaries(m_profile);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_NEAR(result->white.x, white.x, 2e-4);
    EXPECT_NEAR(result->white.y, white.y, 2e-4);
    EXPECT_NEAR(result->red.x, 0.64, 2e-4);
}

TEST_F(CICCProfileTest, InvalidAdaptationFallsBackToPCSSampling) {
    const std::array<uint8_t, 44> singular = {'s', 'f', '3', '2'};
    ASSERT_TRUE(cmsWriteRawTag(m_profile, cmsSigChromaticAdaptationTag, singular.data(), singular.size()));
    const auto result = iccPrimaries(m_profile);
    ASSERT_TRUE(result.has_value()) << result.error();
    cmsCIExyY d50{};
    cmsXYZ2xyY(&d50, cmsD50_XYZ());
    EXPECT_NEAR(result->white.x, d50.x, 2e-4);
    EXPECT_NEAR(result->white.y, d50.y, 2e-4);
}

TEST_F(CICCProfileTest, MissingColorantFallsBackToLUTProfile) {
    const auto                                                 freePipeline = [](cmsPipeline* pipeline) { cmsPipelineFree(pipeline); };
    const std::unique_ptr<cmsPipeline, decltype(freePipeline)> pipeline{cmsPipelineAlloc(nullptr, 3, 3), freePipeline};
    ASSERT_NE(pipeline, nullptr);
    // A matrix-only AToB1 supplies the relative-colorimetric RGB-to-PCS path.
    const std::array<double, 9> matrix = {0.25, 0.15, 0.0821, 0.1, 0.35, 0.05, 0.01, 0.05, 0.35245};
    ASSERT_TRUE(cmsPipelineInsertStage(pipeline.get(), cmsAT_END, cmsStageAllocMatrix(nullptr, 3, 3, matrix.data(), nullptr)));
    ASSERT_TRUE(cmsWriteTag(m_profile, cmsSigAToB1Tag, pipeline.get()));
    ASSERT_TRUE(cmsWriteTag(m_profile, cmsSigRedColorantTag, nullptr));
    const auto result = iccPrimaries(m_profile);
    ASSERT_TRUE(result.has_value()) << result.error();
    cmsCIExyY d50{};
    cmsXYZ2xyY(&d50, cmsD50_XYZ());
    EXPECT_NEAR(result->white.x, d50.x, 2e-4);
    EXPECT_NEAR(result->white.y, d50.y, 2e-4);
}

static SImageDescription iccDescription() {
    return SImageDescription{
        .rawICC    = {1, 2, 3, 4},
        .primaries = NColorPrimaries::BT709,
        .icc       = {.present = true},
    };
}

TEST(ICCDescription, IdenticalContentReusesInternedIdentityAndLUT) {
    auto first              = iccDescription();
    first.icc.lutDataPacked = {0.1f, 0.2f, 0.3f};
    auto second             = iccDescription();
    EXPECT_EQ(first, first);
    EXPECT_EQ(first, second);
    const auto original = CImageDescription::from(first);
    const auto reloaded = CImageDescription::from(second);
    EXPECT_EQ(original, reloaded);
    EXPECT_EQ(original->id(), reloaded->id());
    EXPECT_EQ(reloaded->value().icc.lutDataPacked, first.icc.lutDataPacked);
    EXPECT_EQ(CImageDescription::from(original->id()), original);
}

TEST(ICCDescription, ContentAndEffectiveSettingsDistinguishProfiles) {
    const auto first    = iccDescription();
    const auto original = CImageDescription::from(first);
    auto       changed  = first;
    changed.rawICC.back()++;
    EXPECT_NE(first, changed);
    EXPECT_NE(original, CImageDescription::from(changed));
    changed             = first;
    changed.icc.present = false;
    EXPECT_NE(first, changed);
    EXPECT_NE(original, CImageDescription::from(changed));
    changed             = first;
    changed.icc.lutSize = 17;
    EXPECT_NE(first, changed);
    EXPECT_NE(original, CImageDescription::from(changed));
    changed          = first;
    changed.icc.vcgt = SVCGTTable16{.channels = 3, .entries = 2, .entrySize = 2, .ch = {{{0, 65535}, {0, 65535}, {0, 65535}}}};
    EXPECT_NE(first, changed);
    const auto withVCGT = CImageDescription::from(changed);
    EXPECT_NE(original, withVCGT);
    EXPECT_EQ(withVCGT, CImageDescription::from(changed));
    changed.icc.vcgt->ch[0][0] = 1;
    EXPECT_NE(withVCGT, CImageDescription::from(changed));
    changed                      = first;
    changed.luminances.reference = 100;
    EXPECT_NE(first, changed);
    EXPECT_NE(original, CImageDescription::from(changed));
}

TEST(ICCDescription, DestructionRemovesInternedIdentity) {
    uint64_t id = 0;
    {
        const auto description = CImageDescription::from(iccDescription());
        id                     = description->id();
        EXPECT_EQ(CImageDescription::from(id), description);
    }
    EXPECT_EQ(CImageDescription::from(id), nullptr);
    EXPECT_NE(CImageDescription::from(iccDescription())->id(), id);
}

class CICCReloadTest : public CICCProfileTest {
  protected:
    void SetUp() override {
        CICCProfileTest::SetUp();
        m_directory = std::filesystem::current_path() / "verification" / "icc-reload-test";
        std::filesystem::create_directories(m_directory);
        m_previousCompositor                = std::move(g_pCompositor);
        g_pCompositor                       = makeUnique<CCompositor>(true);
        g_pCompositor->m_explicitConfigPath = (m_directory / "hyprland.lua").string();
        Config::Supplementary::Jeremy::flushCachedCfgPath();
        m_previousWatcher = std::move(Config::watcher());
        Config::watcher() = makeUnique<Config::CConfigWatcher>();
        m_previousConfig  = std::move(Config::mgr());
        Config::mgr()     = makeUnique<Config::Lua::CConfigManager>();
        CConfigValueBase::flushCaches();
    }

    void TearDown() override {
        Config::mgr()     = std::move(m_previousConfig);
        Config::watcher() = std::move(m_previousWatcher);
        if (Config::mgr())
            CConfigValueBase::flushCaches();
        g_pCompositor = std::move(m_previousCompositor);
        Config::Supplementary::Jeremy::flushCachedCfgPath();
        std::filesystem::remove_all(m_directory);
        CICCProfileTest::TearDown();
    }

    std::filesystem::path      m_directory;
    UP<CCompositor>            m_previousCompositor;
    UP<Config::IConfigManager> m_previousConfig;
    UP<Config::CConfigWatcher> m_previousWatcher;
};

TEST_F(CICCReloadTest, UnchangedFileReusesLUTWithoutRenderer) {
    const auto      path = m_directory / "display.icc";
    cmsUInt32Number size = 0;
    ASSERT_TRUE(cmsSaveProfileToMem(m_profile, nullptr, &size));
    std::vector<uint8_t> bytes(size);
    ASSERT_TRUE(cmsSaveProfileToMem(m_profile, bytes.data(), &size));
    ASSERT_TRUE(cmsSaveProfileToFile(m_profile, path.c_str()));
    cmsCloseProfile(m_profile);
    m_profile = cmsOpenProfileFromMem(bytes.data(), size);
    ASSERT_NE(m_profile, nullptr);
    const auto primaries = iccPrimaries(m_profile);
    ASSERT_TRUE(primaries.has_value());
    auto previous              = iccDescription();
    previous.rawICC            = bytes;
    previous.primaries         = *primaries;
    previous.icc.lutDataPacked = {0.1f, 0.2f, 0.3f};
    const auto original        = CImageDescription::from(previous);
    // No renderer/GL context is initialized: a LUT rebuild would fail this test.
    const auto loaded = SImageDescription::fromICC(path, original);
    ASSERT_TRUE(loaded.has_value()) << loaded.error();
    EXPECT_EQ(loaded->icc.lutDataPacked, previous.icc.lutDataPacked);
    EXPECT_EQ(CImageDescription::from(*loaded), original);
    const auto renamed = m_directory / "renamed.icc";
    std::filesystem::rename(path, renamed);
    const auto reloaded = SImageDescription::fromICC(renamed, original);
    ASSERT_TRUE(reloaded.has_value()) << reloaded.error();
    EXPECT_EQ(CImageDescription::from(*reloaded), original);
}
