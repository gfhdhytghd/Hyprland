#include <Compositor.hpp>
#include <animation/AnimationManager.hpp>
#include <config/ConfigValue.hpp>
#include <config/lua/ConfigManager.hpp>
#include <config/lua/types/LuaConfigValue.hpp>
#include <config/shared/animation/AnimationTree.hpp>
#include <config/shared/inotify/ConfigWatcher.hpp>
#include <config/supplementary/jeremy/Jeremy.hpp>
#include <event/EventBus.hpp>
#include <managers/eventLoop/EventLoopManager.hpp>
#include <output/Monitor.hpp>
#include <output/OutputCommitCoordinator.hpp>
#include <protocols/PresentationTime.hpp>
#include <helpers/cm/ICC.hpp>
#include <sys/inotify.h>
#include <fstream>
#include <render/Renderer.hpp>
#include <state/MonitorState.hpp>

#include <gtest/gtest.h>
#include <aquamarine/backend/Backend.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

extern "C" {
#include <lauxlib.h>
}

using namespace NColorManagement;

class CHDRDescriptionTestOutput : public Aquamarine::IOutput {
  public:
    int  m_framesRequested = 0;
    void scheduleFrame(scheduleFrameReason) override {
        ++m_framesRequested;
    }
    size_t getGammaSize() override {
        return 16;
    }
    uint32_t commitCapabilities() const override {
        return AQ_OUTPUT_COMMIT_CAPABILITY_QUEUED;
    }
    SCommitSubmission commitAsync(const SCommitOptions&) override {
        return {.id = 1};
    }
    bool commit() override {
        ADD_FAILURE() << "Unexpected output commit";
        return false;
    }
    bool test() override {
        ADD_FAILURE() << "Unexpected output test";
        return false;
    }
    SP<Aquamarine::IBackendImplementation> getBackend() override {
        return nullptr;
    }
    std::vector<SDRMFormat> getRenderFormats() override {
        return {};
    }
    bool pendingPageFlip() override {
        return false;
    }
    bool pendingIdleFrame() override {
        return false;
    }
};

class CHDRImageDescriptionTest : public ::testing::Test {
  protected:
    void       SetUp() override;
    void       TearDown() override;
    PHLMONITOR makeMonitor(bool withEDID);

  private:
    UP<CCompositor>                      m_previousCompositor;
    UP<CEventLoopManager>                m_previousEventLoop;
    UP<Render::IHyprRenderer>            m_previousRenderer;
    UP<State::CMonitorStateTracker>      m_previousMonitors;
    UP<Event::CEventBus>                 m_previousEventBus;
    UP<Config::IConfigManager>           m_previousConfig;
    UP<Config::CConfigWatcher>           m_previousWatcher;
    UP<Animation::CHyprAnimationManager> m_previousAnimations;
    UP<Config::CAnimationTreeController> m_previousTree;
};

void CHDRImageDescriptionTest::SetUp() {
    m_previousCompositor = std::move(g_pCompositor);
    m_previousEventLoop  = std::move(g_pEventLoopManager);
    m_previousRenderer   = std::move(g_pHyprRenderer);
    m_previousMonitors   = std::move(State::monitorState());
    m_previousEventBus   = std::move(Event::bus());
    m_previousConfig     = std::move(Config::mgr());
    m_previousWatcher    = std::move(Config::watcher());
    m_previousAnimations = std::move(Animation::mgr());
    m_previousTree       = std::move(Config::animationTree());

    Event::bus()          = makeUnique<Event::CEventBus>();
    State::monitorState() = makeUnique<State::CMonitorStateTracker>();
    g_pCompositor         = makeUnique<CCompositor>(true);
    // Construct only config defaults. This dummy path is never loaded or written.
    g_pCompositor->m_explicitConfigPath = "hdr-image-description-test.lua";
    Config::Supplementary::Jeremy::flushCachedCfgPath();
    Config::watcher() = makeUnique<Config::CConfigWatcher>();
    Config::mgr()     = makeUnique<Config::Lua::CConfigManager>();
    CConfigValueBase::flushCaches();
    Animation::mgr()        = makeUnique<Animation::CHyprAnimationManager>();
    Config::animationTree() = makeUnique<Config::CAnimationTreeController>();
}

void CHDRImageDescriptionTest::TearDown() {
    State::monitorState().reset();
    Animation::mgr().reset();
    Config::animationTree().reset();
    Config::mgr().reset();
    Config::watcher().reset();
    g_pCompositor.reset();
    Event::bus().reset();

    Animation::mgr()        = std::move(m_previousAnimations);
    Config::animationTree() = std::move(m_previousTree);
    Config::mgr()           = std::move(m_previousConfig);
    Config::watcher()       = std::move(m_previousWatcher);
    if (Config::mgr())
        CConfigValueBase::flushCaches();
    g_pCompositor         = std::move(m_previousCompositor);
    g_pEventLoopManager   = std::move(m_previousEventLoop);
    g_pHyprRenderer       = std::move(m_previousRenderer);
    Event::bus()          = std::move(m_previousEventBus);
    State::monitorState() = std::move(m_previousMonitors);
    Config::Supplementary::Jeremy::flushCachedCfgPath();
}

PHLMONITOR CHDRImageDescriptionTest::makeMonitor(bool withEDID) {
    auto output  = makeShared<CHDRDescriptionTestOutput>();
    output->name = "hdr-image-description-test";
    if (withEDID) {
        // a wide-gamut panel that isn't BT.2020
        output->parsedEDID.chromaticityCoords = Aquamarine::IOutput::SChromaticityCoords{
            .red   = {.x = 0.68, .y = 0.32},
            .green = {.x = 0.265, .y = 0.69},
            .blue  = {.x = 0.15, .y = 0.06},
            .white = {.x = 0.3127, .y = 0.329},
        };
        output->parsedEDID.hdrMetadata = Aquamarine::IOutput::SHDRMetadata{
            .desiredContentMaxLuminance      = 1000,
            .desiredMaxFrameAverageLuminance = 400,
            .desiredContentMinLuminance      = 0.05F,
            .supportsPQ                      = true,
        };
    }
    auto monitor    = makeShared<Monitor::CMonitor>(output);
    monitor->m_self = monitor;
    return monitor;
}

TEST_F(CHDRImageDescriptionTest, BothHDRPresetsEncodeBT2020WithEDIDMasteringVolume) {
    const auto monitor = makeMonitor(true);

    for (const auto cmType : {NCMType::CM_HDR, NCMType::CM_HDR_EDID}) {
        SCOPED_TRACE(NCMType::toString(cmType));
        const auto description = monitor->getHDRImageDescription(cmType)->value();

        EXPECT_EQ(description.transferFunction, CM_TRANSFER_FUNCTION_ST2084_PQ);
        // HDR10 signals BT.2020 colorimetry, the EDID gamut is only the mastering volume
        EXPECT_TRUE(description.primariesNameSet);
        EXPECT_EQ(description.primariesNamed, CM_PRIMARIES_BT2020);
        EXPECT_EQ(description.primaries, getPrimaries(CM_PRIMARIES_BT2020));
        EXPECT_EQ(description.masteringPrimaries, monitor->getMasteringPrimaries());

        EXPECT_FLOAT_EQ(description.luminances.min, 0.05F);
        EXPECT_EQ(description.luminances.max, description.luminances.min + HDR_MAX_LUMINANCE);
        EXPECT_EQ(description.luminances.reference, sc<uint32_t>(HDR_REF_LUMINANCE));
        EXPECT_FLOAT_EQ(description.masteringLuminances.min, 0.05F);
        EXPECT_EQ(description.masteringLuminances.max, 1000U);
        EXPECT_EQ(description.maxCLL, 1000U);
        EXPECT_EQ(description.maxFALL, 400U);
    }

    EXPECT_EQ(monitor->getHDRImageDescription(NCMType::CM_HDR)->value(), monitor->getHDRImageDescription(NCMType::CM_HDR_EDID)->value());
}

TEST_F(CHDRImageDescriptionTest, HDRPresetFallsBackWithoutEDIDAndHonoursReferenceLuminance) {
    const auto monitor            = makeMonitor(false);
    monitor->m_referenceLuminance = 250;

    const auto description = monitor->getHDRImageDescription(NCMType::CM_HDR)->value();
    EXPECT_EQ(description.primaries, getPrimaries(CM_PRIMARIES_BT2020));
    EXPECT_DOUBLE_EQ(description.luminances.min, HDR_MIN_LUMINANCE);
    EXPECT_EQ(description.luminances.reference, 250U);
    EXPECT_DOUBLE_EQ(description.masteringLuminances.min, HDR_MIN_LUMINANCE);
    EXPECT_EQ(description.masteringLuminances.max, sc<uint32_t>(HDR_MAX_LUMINANCE));
    EXPECT_EQ(description.maxCLL, 0U);
    EXPECT_EQ(description.maxFALL, 0U);
}

TEST_F(CHDRImageDescriptionTest, DetachedMonitorHasNoColorCapabilities) {
    const auto monitor = makeMonitor(true);
    monitor->m_output.reset();

    EXPECT_FALSE(monitor->canSignalBT2020());
    EXPECT_FALSE(monitor->supportsWideColor());
    EXPECT_FALSE(monitor->supportsHDR());
    EXPECT_FALSE(monitor->inHDR());
    EXPECT_FALSE(monitor->canNoShaderCM());
}

TEST_F(CHDRImageDescriptionTest, DetachedMonitorUsesLuminanceDefaults) {
    const auto monitor = makeMonitor(true);
    monitor->m_output.reset();

    EXPECT_FLOAT_EQ(monitor->minLuminance(0.2F), 0.2F);
    EXPECT_EQ(monitor->maxLuminance(500), 500);
    EXPECT_EQ(monitor->maxAvgLuminance(200), 200);
    EXPECT_FLOAT_EQ(monitor->maxCLL(), 0);
    EXPECT_FLOAT_EQ(monitor->maxFALL(), 0);
    EXPECT_EQ(monitor->getMasteringPrimaries(), SPCPRimaries{});
    EXPECT_FLOAT_EQ(monitor->getMasteringLuminances().min, 0);
    EXPECT_EQ(monitor->getMasteringLuminances().max, 0U);

    const auto description = monitor->getHDRImageDescription(NCMType::CM_HDR)->value();
    EXPECT_DOUBLE_EQ(description.luminances.min, HDR_MIN_LUMINANCE);
    EXPECT_EQ(description.masteringLuminances.max, sc<uint32_t>(HDR_MAX_LUMINANCE));
    EXPECT_EQ(description.maxCLL, 0U);
    EXPECT_EQ(description.maxFALL, 0U);
}

TEST_F(CHDRImageDescriptionTest, DegenerateEDIDPrimariesAreNotAdvertised) {
    const auto monitor        = makeMonitor(true);
    auto&      chromaticities = *monitor->m_output->parsedEDID.chromaticityCoords;
    chromaticities.red = chromaticities.green = chromaticities.blue = {};

    EXPECT_EQ(monitor->getMasteringPrimaries(), SPCPRimaries{});
    const auto description = monitor->getHDRImageDescription(NCMType::CM_HDR_EDID)->value();
    EXPECT_EQ(description.getPrimaries(), NColorPrimaries::BT2020);
    EXPECT_EQ(description.masteringPrimaries, SPCPRimaries{});
}

TEST_F(CHDRImageDescriptionTest, ResolvedEDIDLuminanceAndOverridesStayOrdered) {
    const auto monitor                       = makeMonitor(true);
    monitor->m_referenceLuminance            = 1;
    auto& metadata                           = *monitor->m_output->parsedEDID.hdrMetadata;
    metadata.desiredContentMinLuminance      = 2.F;
    metadata.desiredContentMaxLuminance      = 1;
    metadata.desiredMaxFrameAverageLuminance = 3;

    const auto description = monitor->getHDRImageDescription(NCMType::CM_HDR)->value();
    EXPECT_FLOAT_EQ(description.luminances.min, HDR_MIN_LUMINANCE);
    EXPECT_EQ(description.luminances.reference, 1U);
    EXPECT_FLOAT_EQ(description.masteringLuminances.min, HDR_MIN_LUMINANCE);
    EXPECT_EQ(description.masteringLuminances.max, 1U);
    EXPECT_EQ(description.maxCLL, 1U);
    EXPECT_EQ(description.maxFALL, 0U);
}

TEST_F(CHDRImageDescriptionTest, UnencodableReferenceAndMasteringValuesUseDefaults) {
    const auto monitor                  = makeMonitor(true);
    monitor->m_referenceLuminance       = 10001;
    auto& metadata                      = *monitor->m_output->parsedEDID.hdrMetadata;
    metadata.desiredContentMinLuminance = 7.F;
    metadata.desiredContentMaxLuminance = 50000;

    const auto description = monitor->getHDRImageDescription(NCMType::CM_HDR)->value();
    EXPECT_FLOAT_EQ(description.luminances.min, HDR_MIN_LUMINANCE);
    EXPECT_EQ(description.luminances.reference, sc<uint32_t>(HDR_REF_LUMINANCE));
    EXPECT_EQ(description.masteringLuminances.max, sc<uint32_t>(HDR_MAX_LUMINANCE));
    EXPECT_EQ(description.maxCLL, 0U);
    EXPECT_EQ(description.maxFALL, 400U);

    metadata.desiredContentMaxLuminance      = std::numeric_limits<float>::infinity();
    metadata.desiredMaxFrameAverageLuminance = std::numeric_limits<float>::quiet_NaN();
    const auto nonfinite                     = monitor->getHDRImageDescription(NCMType::CM_HDR)->value();
    EXPECT_EQ(nonfinite.masteringLuminances.max, sc<uint32_t>(HDR_MAX_LUMINANCE));
    EXPECT_EQ(nonfinite.maxCLL, 0U);
    EXPECT_EQ(nonfinite.maxFALL, 0U);
}

TEST_F(CHDRImageDescriptionTest, GlobalSDRTransferReadsNewConfigBeforeRefresh) {
    EXPECT_EQ(NTransferFunction::fromConfig(), NTransferFunction::TF_DEFAULT);
    const auto  mgr   = sc<Config::Lua::CConfigManager*>(Config::mgr().get());
    const auto& value = mgr->m_configValues.at("render.cm_sdr_eotf");

    const auto  lua = luaL_newstate();
    ASSERT_NE(lua, nullptr);
    lua_pushliteral(lua, "gamma22");
    EXPECT_EQ(value->parse(lua).errorCode, Config::Lua::PARSE_ERROR_OK);
    EXPECT_EQ(NTransferFunction::fromConfig(), NTransferFunction::TF_GAMMA22);
    EXPECT_EQ(NTransferFunction::fromConfig(true), NTransferFunction::TF_SRGB);

    lua_pushliteral(lua, "gama22");
    EXPECT_EQ(value->parse(lua).errorCode, Config::Lua::PARSE_ERROR_OK);
    EXPECT_EQ(NTransferFunction::fromConfig(), NTransferFunction::TF_DEFAULT);
    lua_pushliteral(lua, "srgb");
    ASSERT_EQ(value->parse(lua).errorCode, Config::Lua::PARSE_ERROR_OK);
    EXPECT_EQ(NTransferFunction::fromConfig(), NTransferFunction::TF_SRGB);
    value->reset();
    EXPECT_EQ(NTransferFunction::fromConfig(), NTransferFunction::TF_DEFAULT);
    lua_close(lua);
}

TEST_F(CHDRImageDescriptionTest, DynamicColorOptionsRequestTheRequiredRefresh) {
    const auto mgr = sc<Config::Lua::CConfigManager*>(Config::mgr().get());
    using namespace Config::Supplementary;

    const auto sdrBits         = mgr->m_configValues.at("render.cm_sdr_eotf")->refreshBits();
    const auto vcgtBits        = mgr->m_configValues.at("render.icc_vcgt_enabled")->refreshBits();
    const auto contentTypeBits = mgr->m_configValues.at("render.send_content_type")->refreshBits();

    EXPECT_EQ(sdrBits, REFRESH_SDR_EOTF);
    EXPECT_EQ(vcgtBits, REFRESH_ICC_PROFILES);
    EXPECT_EQ(contentTypeBits & REFRESH_LAYOUTS, REFRESH_LAYOUTS);
}

TEST_F(CHDRImageDescriptionTest, EncodedBlackStaysBelowPeakAndContentLightLevels) {
    const auto monitor            = makeMonitor(true);
    monitor->m_referenceLuminance = monitor->m_output->parsedEDID.hdrMetadata->desiredContentMaxLuminance = 1;
    monitor->m_output->parsedEDID.hdrMetadata->desiredContentMinLuminance                                 = 0.99995F;
    auto settings                                                                                         = monitor->getHDRImageDescription(NCMType::CM_HDR)->value();
    auto metadata                                                                                         = createHDRMetadata(settings).hdmi_metadata_type1;
    EXPECT_EQ(metadata.min_display_mastering_luminance, 9999);
    EXPECT_EQ(metadata.max_display_mastering_luminance, 1);

    monitor->m_referenceLuminance                                              = 203;
    monitor->m_output->parsedEDID.hdrMetadata->desiredContentMaxLuminance      = 1000;
    monitor->m_output->parsedEDID.hdrMetadata->desiredContentMinLuminance      = 0.99996F;
    monitor->m_output->parsedEDID.hdrMetadata->desiredMaxFrameAverageLuminance = 1;
    settings                                                                   = monitor->getHDRImageDescription(NCMType::CM_HDR)->value();
    metadata                                                                   = createHDRMetadata(settings).hdmi_metadata_type1;
    EXPECT_EQ(metadata.min_display_mastering_luminance, 10000);
    EXPECT_EQ(metadata.max_fall, 0);
}

class CColorRefreshTestMonitors : public State::CMonitorStateTracker {
  public:
    std::vector<PHLMONITOR>        m_outputs;
    const std::vector<PHLMONITOR>& allMonitors() const override {
        return m_outputs;
    }
};

// Seed the cached profile so rereading the same file exercises LCMS without a GPU upload.
static void writeCachedProfile(const std::filesystem::path& path, NColorManagement::PImageDescription& description) {
    const auto profile = cmsCreate_sRGBProfile();
    ASSERT_NE(profile, nullptr);
    Hyprutils::Utils::CScopeGuard closeProfile([&] { cmsCloseProfile(profile); });
    // clang-format off
    const std::array<uint8_t, 38> vcgt = {
        'v', 'c', 'g', 't', 0, 0, 0, 0, 0, 0, 0, 0,
        0, 3, 0, 3, 0, 2, 0, 0, // three channels, three 16-bit entries per channel
        0, 0, 0x40, 0, 0xff, 0xff,
        0, 0, 0x40, 0, 0xff, 0xff,
        0, 0, 0x40, 0, 0xff, 0xff,
    };
    // clang-format on
    ASSERT_TRUE(cmsWriteRawTag(profile, cmsSigVcgtTag, vcgt.data(), vcgt.size()));

    SImageDescription settings;
    cmsUInt32Number   size = 0;
    ASSERT_TRUE(cmsSaveProfileToMem(profile, nullptr, &size));
    settings.rawICC.resize(size);
    ASSERT_TRUE(cmsSaveProfileToMem(profile, settings.rawICC.data(), &size));
    std::ofstream file(path, std::ios::binary);
    ASSERT_TRUE(file.write(rc<const char*>(settings.rawICC.data()), size));
    const auto serialized = cmsOpenProfileFromMem(settings.rawICC.data(), size);
    ASSERT_NE(serialized, nullptr);
    auto primaries = iccPrimaries(serialized);
    cmsCloseProfile(serialized);
    ASSERT_TRUE(primaries);
    settings.primaries   = *primaries;
    settings.icc.present = true;
    settings.icc.vcgt    = SVCGTTable16{.channels = 3, .entries = 3, .entrySize = 2, .ch = {{{0, 16384, 65535}, {0, 16384, 65535}, {0, 16384, 65535}}}};
    description          = CImageDescription::from(settings);
}

static size_t countProfileOpens(int fd) {
    std::array<inotify_event, 16> events{};
    const auto                    bytes = read(fd, events.data(), sizeof(events));
    if (bytes < 0 && errno == EAGAIN)
        return 0;
    EXPECT_GT(bytes, 0);
    size_t count = 0;
    for (ssize_t i = 0; i < bytes / sc<ssize_t>(sizeof(inotify_event)); ++i) {
        EXPECT_EQ(events[i].len, 0U);
        if (events[i].mask & IN_OPEN)
            ++count;
    }
    return count;
}

TEST_F(CHDRImageDescriptionTest, IdleColorRefreshDamagesEachOutputOnceWithoutProbingModes) {
    using namespace Config::Supplementary;
    auto       outputs    = makeUnique<CColorRefreshTestMonitors>();
    const auto first      = makeMonitor(false);
    const auto second     = makeMonitor(false);
    const auto idle       = makeMonitor(false);
    outputs->m_outputs    = {first, second, idle};
    State::monitorState() = std::move(outputs);

    auto                           profilePath = (std::filesystem::temp_directory_path() / "hypr-color-refresh-XXXXXX").string();
    Hyprutils::OS::CFileDescriptor profileFile{mkstemp(profilePath.data())};
    ASSERT_TRUE(profileFile.isValid());
    Hyprutils::Utils::CScopeGuard       removeProfile([&] { std::filesystem::remove(profilePath); });
    NColorManagement::PImageDescription cached;
    ASSERT_NO_FATAL_FAILURE(writeCachedProfile(profilePath, cached));
    Hyprutils::OS::CFileDescriptor watch{inotify_init1(IN_NONBLOCK | IN_CLOEXEC)};
    ASSERT_TRUE(watch.isValid());
    ASSERT_GE(inotify_add_watch(watch.get(), profilePath.c_str(), IN_OPEN | IN_CLOSE_NOWRITE), 0);
    for (const auto& m : State::monitorState()->allMonitors()) {
        m->m_enabled                     = true;
        m->m_sdrEotf                     = NTransferFunction::TF_SRGB;
        m->m_activeMonitorRule.m_iccFile = profilePath;
        m->m_imageDescription            = cached;
        m->m_damage.setSize({640, 480});
        m->m_output->state->setGammaLut({1, 2, 3});
    }
    idle->m_activeMonitorRule.m_iccFile.clear();
    idle->m_imageDescription = getDefaultImageDescription();
    idle->m_output->state->setGammaLut({});
    idle->m_damage.beginTransaction().commit();
    ASSERT_FALSE(idle->m_damage.hasChanged());

    Aquamarine::SBackendImplementationOptions backend;
    backend.backendType        = Aquamarine::AQ_BACKEND_NULL;
    g_pCompositor->m_aqBackend = Aquamarine::CBackend::create({backend}, Aquamarine::SBackendOptions{});
    ASSERT_TRUE(g_pCompositor->m_aqBackend);

    const std::unique_ptr<wl_display, decltype(&wl_display_destroy)> display{wl_display_create(), wl_display_destroy};
    ASSERT_TRUE(display);
    auto loop           = wl_display_get_event_loop(display.get());
    g_pEventLoopManager = makeUnique<CEventLoopManager>(display.get(), loop);
    auto previous       = std::move(refresher());
    refresher()         = makeUnique<CPropRefresher>();
    Hyprutils::Utils::CScopeGuard                          cleanup([&] {
        refresher() = std::move(previous);
        g_pEventLoopManager.reset();
    });

    const auto                                             mgr         = sc<Config::Lua::CConfigManager*>(Config::mgr().get());
    const auto&                                            curve       = mgr->m_configValues.at("render.cm_sdr_eotf");
    const auto&                                            calibration = mgr->m_configValues.at("render.icc_vcgt_enabled");
    const std::unique_ptr<lua_State, decltype(&lua_close)> lua{luaL_newstate(), lua_close};
    ASSERT_TRUE(lua);
    lua_pushliteral(lua.get(), "gamma22");
    ASSERT_EQ(curve->parse(lua.get()).errorCode, Config::Lua::PARSE_ERROR_OK);
    lua_pushboolean(lua.get(), false);
    ASSERT_EQ(calibration->parse(lua.get()).errorCode, Config::Lua::PARSE_ERROR_OK);

    using Coordinator = Monitor::COutputCommitCoordinator;
    const auto& async = mgr->m_configValues.at("render.async_commit");
    lua_pushboolean(lua.get(), true);
    ASSERT_EQ(async->parse(lua.get()).errorCode, Config::Lua::PARSE_ERROR_OK);
    g_pCompositor->m_wlDisplay = display.get();
    auto previousPresentation  = std::move(PROTO::presentation);
    PROTO::presentation        = makeUnique<CPresentationProtocol>(&wp_presentation_interface, 2, "color-refresh-test");
    Hyprutils::Utils::CScopeGuard cleanupPresentation([&] {
        for (const auto& m : State::monitorState()->allMonitors())
            m->m_commitCoordinator->cancelPending();
        PROTO::presentation        = std::move(previousPresentation);
        g_pCompositor->m_wlDisplay = nullptr;
    });
    for (const auto& m : {first, second}) {
        m->m_output->state->setContentType(DRM_MODE_CONTENT_TYPE_GAME);
        ASSERT_EQ(m->m_commitCoordinator->submit({.damage = m->m_damage.beginTransaction(), .copyFBPrepared = true}), Coordinator::SUBMIT_ASYNC);
        m->stageContentType(DRM_MODE_CONTENT_TYPE_NO_DATA);
        m->m_commitCoordinator->onCommitResult({.id = 1, .status = Aquamarine::IOutput::AQ_OUTPUT_COMMIT_SUBMITTED});
        EXPECT_EQ(m->m_committedContentType, DRM_MODE_CONTENT_TYPE_GAME);
        EXPECT_TRUE(m->stageContentType(DRM_MODE_CONTENT_TYPE_NO_DATA));
        ASSERT_TRUE(m->m_commitCoordinator->hasPendingCommit());
        ASSERT_FALSE(m->m_damage.hasChanged());
    }
    const auto bits = curve->refreshBits() | calibration->refreshBits();
    refresher()->scheduleRefresh(bits);
    wl_event_loop_dispatch_idle(loop);
    for (const auto& m : State::monitorState()->allMonitors()) {
        EXPECT_TRUE(m->m_damage.hasChanged());
        EXPECT_EQ(m->m_appliedGlobalSdrEotf, NTransferFunction::TF_GAMMA22);
        EXPECT_TRUE(m->m_output->state->state().gammaLut.empty());
        EXPECT_EQ(sc<CHDRDescriptionTestOutput*>(m->m_output.get())->m_framesRequested, 1);
    }
    EXPECT_EQ(countProfileOpens(watch.get()), 0U);

    second->m_commitCoordinator->onPresented(1, true);
    lua_pushboolean(lua.get(), true);
    ASSERT_EQ(calibration->parse(lua.get()).errorCode, Config::Lua::PARSE_ERROR_OK);
    refresher()->scheduleRefresh(calibration->refreshBits());
    wl_event_loop_dispatch_idle(loop);
    EXPECT_EQ(countProfileOpens(watch.get()), 2U);
    for (const auto& m : {first, second}) {
        EXPECT_TRUE(m->gammaRampsInUse());
        EXPECT_EQ(m->m_output->state->state().gammaLut.size(), 48U);
    }

    first->m_commitCoordinator->onPresented(1, true);
    refresher()->scheduleRefresh(bits);
    wl_event_loop_dispatch_idle(loop);
    EXPECT_EQ(countProfileOpens(watch.get()), 0U);
    for (const auto& m : State::monitorState()->allMonitors())
        EXPECT_EQ(sc<CHDRDescriptionTestOutput*>(m->m_output.get())->m_framesRequested, m == idle ? 1 : 2);
}

TEST_F(CHDRImageDescriptionTest, ContentHintClearRetriesUntilCommitted) {
    const auto monitor              = makeMonitor(false);
    monitor->m_committedContentType = DRM_MODE_CONTENT_TYPE_GAME;
    monitor->m_output->state->setContentType(DRM_MODE_CONTENT_TYPE_GAME);
    EXPECT_TRUE(monitor->stageContentType(DRM_MODE_CONTENT_TYPE_NO_DATA));
    EXPECT_EQ(monitor->m_output->state->state().contentType, DRM_MODE_CONTENT_TYPE_NO_DATA);
    EXPECT_TRUE(monitor->stageContentType(DRM_MODE_CONTENT_TYPE_NO_DATA));
    monitor->m_committedContentType = DRM_MODE_CONTENT_TYPE_NO_DATA;
    EXPECT_FALSE(monitor->stageContentType(DRM_MODE_CONTENT_TYPE_NO_DATA));
}
