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
