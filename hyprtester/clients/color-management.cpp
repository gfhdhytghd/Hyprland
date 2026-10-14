#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <print>
#include <string>
#include <thread>
#include <vector>
#include <wayland-client.h>
#include <wayland.hpp>
#include <color-management-v1.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/memory/WeakPtr.hpp>
#include <hyprutils/memory/Casts.hpp>

using namespace Hyprutils::Memory;

class CDisplay {
  public:
    CDisplay() : m_display(wl_display_connect(nullptr)) {}
    ~CDisplay() {
        if (m_display)
            wl_display_disconnect(m_display);
    }
    wl_display* get() {
        return m_display;
    }

  private:
    wl_display* m_display = nullptr;
};

static int run(wl_display* display, const std::string& mode) {
    const auto                              separator       = mode.find('@');
    const auto                              requestedOutput = separator == std::string::npos ? std::string("HEADLESS-1") : mode.substr(separator + 1);
    auto                                    registry        = makeShared<CCWlRegistry>(rc<wl_proxy*>(wl_display_get_registry(display)));
    CSharedPointer<CCWlCompositor>          compositor;
    CSharedPointer<CCWlOutput>              output;
    std::vector<CSharedPointer<CCWlOutput>> outputs;
    CSharedPointer<CCWpColorManagerV1>      manager;
    registry->setGlobal([&](CCWlRegistry* r, uint32_t name, const char* interface, uint32_t version) {
        if (std::strcmp(interface, "wl_compositor") == 0)
            compositor = makeShared<CCWlCompositor>(sc<wl_proxy*>(wl_registry_bind(rc<wl_registry*>(r->resource()), name, &wl_compositor_interface, 4)));
        else if (std::strcmp(interface, "wl_output") == 0 && (!output || mode.starts_with("output-reference="))) {
            auto resource = makeShared<CCWlOutput>(sc<wl_proxy*>(wl_registry_bind(rc<wl_registry*>(r->resource()), name, &wl_output_interface, std::min(version, 4U))));
            outputs.push_back(resource);
            if (mode.starts_with("output-reference="))
                resource->setName([&, weak = CWeakPointer<CCWlOutput>(resource)](CCWlOutput*, const char* name) {
                    if (requestedOutput == name)
                        output = weak.lock();
                });
            else
                output = resource;
        } else if (std::strcmp(interface, "wp_color_manager_v1") == 0)
            manager = makeShared<CCWpColorManagerV1>(sc<wl_proxy*>(wl_registry_bind(rc<wl_registry*>(r->resource()), name, &wp_color_manager_v1_interface, 1)));
    });
    if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0 || !manager || !compositor || !output)
        return 2;

    auto                                                surface   = makeShared<CCWlSurface>(compositor->sendCreateSurface());
    auto                                                cmSurface = makeShared<CCWpColorManagementSurfaceV1>(manager->sendGetSurface(surface->resource()));
    auto                                                feedback  = makeShared<CCWpColorManagementSurfaceFeedbackV1>(manager->sendGetSurfaceFeedback(surface->resource()));
    auto                                                cmOutput  = makeShared<CCWpColorManagementOutputV1>(manager->sendGetOutput(output->resource()));
    CSharedPointer<CCWpImageDescriptionV1>              description;
    CSharedPointer<CCWpImageDescriptionInfoV1>          information;
    CSharedPointer<CCWpImageDescriptionCreatorParamsV1> creator;
    std::string                                         expectedInterface;
    uint32_t                                            expectedError = 0;
    bool                                                ready = false, failed = false;

    if (mode.starts_with("output-reference=")) {
        const auto expected = std::stoul(mode.substr(17));
        uint32_t   actual   = 0;
        description         = makeShared<CCWpImageDescriptionV1>(cmOutput->sendGetImageDescription());
        description->setReady([&](CCWpImageDescriptionV1*, uint32_t) { ready = true; });
        if (wl_display_roundtrip(display) < 0 || !ready)
            return 10;
        information = makeShared<CCWpImageDescriptionInfoV1>(description->sendGetInformation());
        information->setLuminances([&](CCWpImageDescriptionInfoV1*, uint32_t, uint32_t max, uint32_t reference) {
            actual = reference;
            if (max != 10000)
                actual = 0;
        });
        if (wl_display_roundtrip(display) < 0 || actual != expected)
            return 11;
        return 0;
    }

    if (mode == "feedback-inert" || mode == "surface-inert") {
        surface->sendDestroy();
        surface.reset();
        if (wl_display_roundtrip(display) < 0)
            return 3;
        if (mode == "feedback-inert") {
            // Keep the inert feedback alive while the tester changes the output description.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (wl_display_roundtrip(display) < 0)
                return 4;
            description       = makeShared<CCWpImageDescriptionV1>(feedback->sendGetPreferred());
            expectedInterface = "wp_color_management_surface_feedback_v1";
            expectedError     = WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_ERROR_INERT;
        } else {
            cmSurface->sendUnsetImageDescription();
            expectedInterface = "wp_color_management_surface_v1";
            expectedError     = WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT;
        }
    } else if (mode == "failed-info" || mode == "failed-set") {
        output->sendRelease();
        output.reset();
        outputs.clear();
        description = makeShared<CCWpImageDescriptionV1>(cmOutput->sendGetImageDescription());
        description->setFailed([&](CCWpImageDescriptionV1*, wpImageDescriptionV1Cause cause, const char*) { failed = cause == WP_IMAGE_DESCRIPTION_V1_CAUSE_NO_OUTPUT; });
        if (wl_display_roundtrip(display) < 0 || !failed)
            return 5;
        if (mode == "failed-info") {
            information       = makeShared<CCWpImageDescriptionInfoV1>(description->sendGetInformation());
            expectedInterface = "wp_image_description_v1";
            expectedError     = WP_IMAGE_DESCRIPTION_V1_ERROR_NOT_READY;
        } else {
            cmSurface->sendSetImageDescription(description.get(), WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
            expectedInterface = "wp_color_management_surface_v1";
            expectedError     = WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_IMAGE_DESCRIPTION;
        }
    } else {
        creator           = makeShared<CCWpImageDescriptionCreatorParamsV1>(manager->sendCreateParametricCreator());
        expectedInterface = "wp_image_description_creator_params_v1";
        if (mode != "missing-tf" && mode != "empty")
            creator->sendSetTfNamed(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
        if (mode != "missing-primaries" && mode != "empty")
            creator->sendSetPrimariesNamed(WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
        if (mode == "empty" || mode == "missing-tf" || mode == "missing-primaries")
            expectedError = WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INCOMPLETE_SET;
        else if (mode == "master-zero" || mode == "master-inverted") {
            creator->sendSetMasteringLuminance(mode == "master-zero" ? 0 : 10, 0);
            expectedError = WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE;
        } else if (mode == "repeat-master" || mode == "repeat-cll" || mode == "repeat-fall" || mode == "repeat-tf") {
            if (mode == "repeat-master") {
                creator->sendSetMasteringLuminance(0, 1000);
                creator->sendSetMasteringLuminance(0, 1000);
            } else if (mode == "repeat-cll") {
                creator->sendSetMaxCll(1000);
                creator->sendSetMaxCll(1000);
            } else if (mode == "repeat-fall") {
                creator->sendSetMaxFall(400);
                creator->sendSetMaxFall(400);
            } else
                creator->sendSetTfNamed(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
            expectedError = WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET;
        } else if (mode == "fall-exceeds-cll" || mode == "cll-exceeds-master") {
            creator->sendSetMasteringLuminance(0, 1000);
            creator->sendSetMaxCll(mode == "cll-exceeds-master" ? 1001 : 400);
            creator->sendSetMaxFall(500);
            expectedError = WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE;
        } else if (mode == "tf-power") {
            creator->sendSetTfPower(22000);
            expectedError = WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE;
        } else if (mode == "valid-copy" || mode == "pq-ignored-max")
            expectedInterface.clear();
        else
            return 6;
        if (mode == "pq-ignored-max")
            creator->sendSetLuminances(50, 0, 203);
        description = makeShared<CCWpImageDescriptionV1>(creator->sendCreate());
        description->setReady([&](CCWpImageDescriptionV1*, uint32_t) { ready = true; });
    }

    const auto result = wl_display_roundtrip(display);
    if (expectedInterface.empty()) {
        if (result < 0 || !ready)
            return 7;
        cmSurface->sendSetImageDescription(description.get(), WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
        description->sendDestroy();
        description.reset();
        surface->sendCommit();
        return wl_display_roundtrip(display) < 0 ? 8 : 0;
    }
    const wl_interface* interface = nullptr;
    uint32_t            id        = 0;
    const auto          error     = wl_display_get_protocol_error(display, &interface, &id);
    if (result >= 0 || wl_display_get_error(display) != EPROTO || !interface || expectedInterface != interface->name || error != expectedError) {
        std::println(stderr, "{}: expected {} error {}, got {} error {}", mode, expectedInterface, expectedError, interface ? interface->name : "none", error);
        return 9;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2)
        return 1;
    CDisplay display;
    if (!display.get())
        return 1;
    return run(display.get(), argv[1]);
}
