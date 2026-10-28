#include <Compositor.hpp>
#include <animation/AnimationManager.hpp>
#include <config/ConfigValue.hpp>
#include <config/lua/ConfigManager.hpp>
#include <config/shared/animation/AnimationTree.hpp>
#include <config/shared/inotify/ConfigWatcher.hpp>
#include <config/supplementary/jeremy/Jeremy.hpp>
#include <event/EventBus.hpp>
#include <managers/eventLoop/EventLoopManager.hpp>
#include <output/Monitor.hpp>
#include <render/Renderer.hpp>
#include <state/MonitorState.hpp>

#include <gtest/gtest.h>

using namespace NColorManagement;

class CHDRDescriptionTestOutput : public Aquamarine::IOutput {
  public:
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
        const auto& description = monitor->getHDRImageDescription(cmType)->value();

        EXPECT_EQ(description.transferFunction, CM_TRANSFER_FUNCTION_ST2084_PQ);
        // HDR10 signals BT.2020 colorimetry, the EDID gamut is only the mastering volume
        EXPECT_TRUE(description.primariesNameSet);
        EXPECT_EQ(description.primariesNamed, CM_PRIMARIES_BT2020);
        EXPECT_EQ(description.primaries, getPrimaries(CM_PRIMARIES_BT2020));
        EXPECT_EQ(description.masteringPrimaries, monitor->getMasteringPrimaries());

        EXPECT_FLOAT_EQ(description.luminances.min, 0.05F);
        EXPECT_EQ(description.luminances.max, sc<uint32_t>(HDR_MAX_LUMINANCE));
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

    const auto& description = monitor->getHDRImageDescription(NCMType::CM_HDR)->value();
    EXPECT_EQ(description.primaries, getPrimaries(CM_PRIMARIES_BT2020));
    EXPECT_FLOAT_EQ(description.luminances.min, HDR_MIN_LUMINANCE);
    EXPECT_EQ(description.luminances.reference, 250U);
    EXPECT_FLOAT_EQ(description.masteringLuminances.min, HDR_MIN_LUMINANCE);
    EXPECT_EQ(description.masteringLuminances.max, sc<uint32_t>(HDR_MAX_LUMINANCE));
    EXPECT_EQ(description.maxCLL, 0U);
    EXPECT_EQ(description.maxFALL, 0U);
}
