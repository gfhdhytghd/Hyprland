#include <Compositor.hpp>
#include <event/EventBus.hpp>
#include <config/ConfigValue.hpp>
#include <config/lua/ConfigManager.hpp>
#include <config/shared/inotify/ConfigWatcher.hpp>
#include <protocols/ColorManagement.hpp>
#include <protocols/PresentationTime.hpp>
#include <protocols/core/Compositor.hpp>
#include <protocols/core/Subcompositor.hpp>
#include <gtest/gtest.h>
#include <hyprutils/os/FileDescriptor.hpp>
#include <array>
#include <cstring>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>

using namespace NColorManagement;

// Dispatch real Wayland requests without a GPU, output backend, or desktop session.
class CColorManagementProtocolTest : public ::testing::Test {
  protected:
    void                                                       SetUp() override;
    void                                                       TearDown() override;
    void                                                       request(uint32_t object, uint16_t opcode, std::initializer_list<uint32_t> arguments);
    void                                                       expectError(uint32_t object, uint32_t code);
    void                                                       expectEvent(uint32_t object, uint16_t opcode);
    void                                                       createSurface();
    void                                                       createParams(uint32_t version = 1);

    std::unique_ptr<wl_display, decltype(&wl_display_destroy)> m_display{nullptr, wl_display_destroy};
    Hyprutils::OS::CFileDescriptor                             m_socket;
    wl_client*                                                 m_client           = nullptr; // Owned by m_display.
    bool                                                       m_installedGlobals = false;
    UP<Event::CEventBus>                                       m_previousBus;
    UP<CCompositor>                                            m_previousCompositor;
    UP<Config::IConfigManager>                                 m_previousConfig;
    UP<Config::CConfigWatcher>                                 m_previousWatcher;
    UP<CColorManagementProtocol>                               m_previousColor;
    UP<CWLCompositorProtocol>                                  m_previousCore;
    UP<CWLSubcompositorProtocol>                               m_previousSubcompositor;
    UP<CPresentationProtocol>                                  m_previousPresentation;
};

void CColorManagementProtocolTest::SetUp() {
    m_display.reset(wl_display_create());
    ASSERT_TRUE(m_display);
    std::array<int, 2> sockets{-1, -1};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()), 0);
    Hyprutils::OS::CFileDescriptor server{sockets[0]};
    m_socket = Hyprutils::OS::CFileDescriptor{sockets[1]};
    m_client = wl_client_create(m_display.get(), server.get());
    ASSERT_NE(m_client, nullptr);
    server.take();
    m_installedGlobals           = true;
    m_previousCompositor         = std::move(g_pCompositor);
    g_pCompositor                = makeUnique<CCompositor>(true);
    g_pCompositor->m_wlDisplay   = m_display.get();
    g_pCompositor->m_wlEventLoop = wl_display_get_event_loop(m_display.get());
    m_previousWatcher            = std::move(Config::watcher());
    Config::watcher()            = makeUnique<Config::CConfigWatcher>();
    m_previousConfig             = std::move(Config::mgr());
    Config::mgr()                = makeUnique<Config::Lua::CConfigManager>();
    CConfigValueBase::flushCaches();
    m_previousBus           = std::move(Event::bus());
    Event::bus()            = makeUnique<Event::CEventBus>();
    m_previousColor         = std::move(PROTO::colorManagement);
    m_previousCore          = std::move(PROTO::compositor);
    m_previousSubcompositor = std::move(PROTO::subcompositor);
    m_previousPresentation  = std::move(PROTO::presentation);
    PROTO::colorManagement  = makeUnique<CColorManagementProtocol>(&wp_color_manager_v1_interface, 3, "cm-test");
    PROTO::compositor       = makeUnique<CWLCompositorProtocol>(&wl_compositor_interface, 6, "cm-core-test");
    PROTO::subcompositor    = makeUnique<CWLSubcompositorProtocol>(&wl_subcompositor_interface, 1, "cm-subcompositor-test");
    PROTO::presentation     = makeUnique<CPresentationProtocol>(&wp_presentation_interface, 2, "cm-presentation-test");
}

void CColorManagementProtocolTest::TearDown() {
    if (m_display)
        wl_display_destroy_clients(m_display.get());
    if (!m_installedGlobals)
        return;
    PROTO::colorManagement = std::move(m_previousColor);
    PROTO::compositor      = std::move(m_previousCore);
    PROTO::subcompositor   = std::move(m_previousSubcompositor);
    PROTO::presentation    = std::move(m_previousPresentation);
    Config::mgr()          = std::move(m_previousConfig);
    Config::watcher()      = std::move(m_previousWatcher);
    if (Config::mgr())
        CConfigValueBase::flushCaches();
    Event::bus()  = std::move(m_previousBus);
    g_pCompositor = std::move(m_previousCompositor);
}

void CColorManagementProtocolTest::request(uint32_t object, uint16_t opcode, std::initializer_list<uint32_t> arguments) {
    std::vector<uint32_t> message{object, sc<uint32_t>(((arguments.size() + 2) * sizeof(uint32_t) << 16) | opcode)};
    message.insert(message.end(), arguments);
    ASSERT_EQ(send(m_socket.get(), message.data(), message.size() * sizeof(uint32_t), MSG_NOSIGNAL), sc<ssize_t>(message.size() * sizeof(uint32_t)));
    ASSERT_EQ(wl_event_loop_dispatch(wl_display_get_event_loop(m_display.get()), 0), 0);
    wl_display_flush_clients(m_display.get());
}

void CColorManagementProtocolTest::expectError(uint32_t object, uint32_t code) {
    std::array<uint32_t, 4096> events{};
    const auto                 bytes = recv(m_socket.get(), events.data(), sizeof(events), MSG_DONTWAIT);
    ASSERT_GT(bytes, 0);
    size_t offset = 0;
    while ((offset + 2) * sizeof(uint32_t) <= sc<size_t>(bytes)) {
        const auto size = events[offset + 1] >> 16;
        ASSERT_GE(size, 8U);
        ASSERT_LE(offset * sizeof(uint32_t) + size, sc<size_t>(bytes));
        if (events[offset] == 1 && (events[offset + 1] & 0xFFFF) == 0) {
            ASSERT_GE(size, 20U);
            EXPECT_EQ(events[offset + 2], object);
            EXPECT_EQ(events[offset + 3], code);
            return;
        }
        offset += size / sizeof(uint32_t);
    }
    FAIL() << "No wl_display.error event received";
}

void CColorManagementProtocolTest::expectEvent(uint32_t object, uint16_t opcode) {
    std::array<uint32_t, 4096> events{};
    const auto                 bytes = recv(m_socket.get(), events.data(), sizeof(events), MSG_DONTWAIT);
    ASSERT_GT(bytes, 0);
    size_t offset = 0;
    while ((offset + 2) * sizeof(uint32_t) <= sc<size_t>(bytes)) {
        const auto size = events[offset + 1] >> 16;
        ASSERT_GE(size, 8U);
        EXPECT_FALSE(events[offset] == 1 && (events[offset + 1] & 0xFFFF) == 0) << "Unexpected wl_display.error";
        if (events[offset] == object && (events[offset + 1] & 0xFFFF) == opcode)
            return;
        offset += size / sizeof(uint32_t);
    }
    FAIL() << "Event " << opcode << " was not sent to object " << object;
}

void CColorManagementProtocolTest::createParams(uint32_t version) {
    PROTO::colorManagement->bindManager(m_client, nullptr, version, 2);
    request(2, 5, {3});
}

void CColorManagementProtocolTest::createSurface() {
    PROTO::colorManagement->bindManager(m_client, nullptr, 1, 2);
    PROTO::compositor->bindManager(m_client, nullptr, 6, 3);
    request(3, 0, {4});
    ASSERT_NE(wl_client_get_object(m_client, 4), nullptr);
}

TEST_F(CColorManagementProtocolTest, InertFeedbackSurvivesDescriptionChange) {
    createSurface();
    request(2, 3, {5, 4});
    request(4, 0, {});
    PROTO::colorManagement->onMonitorImageDescriptionChanged({});
    PROTO::colorManagement->onImagePreferredChanged();
    request(5, 1, {6});
    expectError(5, WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_ERROR_INERT);
}

TEST_F(CColorManagementProtocolTest, InertSurfaceRejectsUnset) {
    createSurface();
    request(2, 2, {5, 4});
    request(4, 0, {});
    request(5, 2, {});
    expectError(5, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT);
}

TEST_F(CColorManagementProtocolTest, DescriptionAndDestructionAreDoubleBuffered) {
    createSurface();
    request(2, 2, {5, 4});
    request(2, 6, {6});
    const auto surface = CWLSurfaceResource::fromResource(wl_client_get_object(m_client, 4));
    ASSERT_TRUE(surface);
    ASSERT_TRUE(surface->m_colorManagement);
    request(5, 1, {6, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL});
    EXPECT_FALSE(surface->m_current.imageDescription);
    request(6, 0, {});
    EXPECT_TRUE(surface->m_pending.imageDescription);
    request(4, 6, {});
    ASSERT_TRUE(surface->m_current.imageDescription);
    EXPECT_TRUE(isWindowsScRGB(surface->m_current.imageDescription->value()));
    // Destroying the extension object unsets the description only on the next commit.
    request(5, 0, {});
    EXPECT_FALSE(surface->m_colorManagement);
    EXPECT_TRUE(surface->m_current.imageDescription);
    request(4, 6, {});
    EXPECT_FALSE(surface->m_current.imageDescription);
}

TEST_F(CColorManagementProtocolTest, FailedDescriptionRejectsInformation) {
    PROTO::colorManagement->bindManager(m_client, nullptr, 1, 2);
    auto output = makeShared<CColorManagementOutput>(makeShared<CWpColorManagementOutputV1>(m_client, 1, 3), WP<CWLOutputResource>{});
    request(3, 1, {4});
    request(4, 1, {5});
    expectError(4, WP_IMAGE_DESCRIPTION_V1_ERROR_NOT_READY);
}

TEST_F(CColorManagementProtocolTest, FailedDescriptionRejectsSurfaceUse) {
    createSurface();
    request(2, 2, {5, 4});
    auto output = makeShared<CColorManagementOutput>(makeShared<CWpColorManagementOutputV1>(m_client, 1, 6), WP<CWLOutputResource>{});
    request(6, 1, {7});
    request(5, 1, {7, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL});
    expectError(5, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_IMAGE_DESCRIPTION);
}

TEST_F(CColorManagementProtocolTest, IncompleteCreatorIsRejected) {
    createParams();
    request(3, 1, {WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ});
    request(3, 0, {4});
    expectError(3, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INCOMPLETE_SET);
}

TEST_F(CColorManagementProtocolTest, ZeroMasteringRangeIsRejected) {
    createParams();
    request(3, 7, {0, 0});
    expectError(3, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE);
}

TEST_F(CColorManagementProtocolTest, ContentLightLevelsAreOrdered) {
    createParams();
    request(3, 1, {WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ});
    request(3, 3, {WP_COLOR_MANAGER_V1_PRIMARIES_BT2020});
    request(3, 8, {400});
    request(3, 9, {500});
    request(3, 0, {4});
    expectError(3, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE);
}

TEST_F(CColorManagementProtocolTest, TFPowerUsesCreatorErrorCode) {
    createParams();
    request(3, 2, {22000});
    expectError(3, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE);
}

TEST_F(CColorManagementProtocolTest, VersionOneRejectsCompoundPower) {
    createParams();
    request(3, 1, {WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4});
    expectError(3, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF);
}

TEST_F(CColorManagementProtocolTest, VersionTwoRejectsDeprecatedSRGB) {
    createParams(2);
    request(3, 1, {WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB});
    expectError(3, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF);
}

TEST_F(CColorManagementProtocolTest, ExplicitZeroPrimariesFail) {
    createParams();
    request(3, 1, {WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22});
    request(3, 4, {0, 0, 0, 0, 0, 0, 0, 0});
    request(3, 0, {4});
    expectEvent(4, 0); // wp_image_description_v1.failed
}

TEST_F(CColorManagementProtocolTest, NonPhysicalMasteringPrimariesFail) {
    createParams();
    request(3, 1, {WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ});
    request(3, 3, {WP_COLOR_MANAGER_V1_PRIMARIES_BT2020});
    // Mastering display chromaticities outside the xy unit triangle can't describe a display, nor be sent as HDR metadata.
    request(3, 6, {INT32_MAX, INT32_MAX, 265000, 690000, 150000, 60000, 312700, 329000});
    request(3, 0, {4});
    expectEvent(4, 0); // wp_image_description_v1.failed
}

TEST_F(CColorManagementProtocolTest, PreferredDescriptionOfAnOrphanedSubsurface) {
    createSurface();
    request(3, 0, {5});
    PROTO::subcompositor->bindManager(m_client, nullptr, 1, 6);
    request(6, 1, {7, 5, 4}); // child 5 below parent 4
    const auto child = CWLSurfaceResource::fromResource(wl_client_get_object(m_client, 5));
    ASSERT_TRUE(child);
    // The parent goes away while the wl_subsurface and its role stay: the child falls back to its own outputs.
    request(4, 0, {});
    EXPECT_TRUE(child->getPreferredImageDescription());
}

TEST_F(CColorManagementProtocolTest, ParametricDescriptionIsFreedOnResourceDestroy) {
    createParams();
    request(3, 1, {WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22});
    request(3, 4, {640123, 330000, 300000, 600000, 150000, 60000, 312700, 329000});
    request(3, 5, {123, 997, 149});
    request(3, 0, {4});
    auto resource = CColorManagementImageDescription::fromReference(wl_client_get_object(m_client, 4));
    ASSERT_TRUE(resource);
    ASSERT_TRUE(resource->ready());
    const auto                  id          = resource->m_settings->id();
    const auto                  primariesId = resource->m_settings->getPrimaries()->id();
    WP<const CImageDescription> weak        = resource->m_settings;
    resource.reset();
    request(4, 0, {});
    EXPECT_TRUE(weak.expired());
    EXPECT_FALSE(CImageDescription::from(id));
    EXPECT_FALSE(CPrimaries::from(primariesId));
}

TEST_F(CColorManagementProtocolTest, ClientDisconnectReleasesParametricDescription) {
    createParams();
    request(3, 1, {WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22});
    request(3, 4, {640321, 330000, 300000, 600000, 150000, 60000, 312700, 329000});
    request(3, 0, {4});
    auto resource = CColorManagementImageDescription::fromReference(wl_client_get_object(m_client, 4));
    ASSERT_TRUE(resource);
    ASSERT_TRUE(resource->ready());
    const auto                  id          = resource->m_settings->id();
    const auto                  primariesId = resource->m_settings->getPrimaries()->id();
    WP<const CImageDescription> weak        = resource->m_settings;
    resource.reset();
    wl_display_destroy_clients(m_display.get());
    m_client = nullptr;
    EXPECT_TRUE(weak.expired());
    EXPECT_FALSE(CImageDescription::from(id));
    EXPECT_FALSE(CPrimaries::from(primariesId));
}
