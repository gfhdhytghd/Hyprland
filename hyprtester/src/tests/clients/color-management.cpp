#include "../../hyprctlCompat.hpp"
#include "../../shared.hpp"
#include "../shared.hpp"
#include "build.hpp"
#include "tests.hpp"
#include <hyprutils/os/Process.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <chrono>
#include <thread>
#include <future>

using namespace Hyprutils::OS;
using namespace Hyprutils::Utils;

TEST_CASE(colorManagementRejectsInvalidRequests) {
    for (const auto mode : {"surface-inert", "failed-info", "failed-set", "empty", "missing-tf", "missing-primaries", "master-zero", "master-inverted", "repeat-master",
                            "repeat-cll", "repeat-fall", "repeat-tf", "fall-exceeds-cll", "cll-exceeds-master", "tf-power", "valid-copy", "pq-ignored-max"}) {
        CProcess process(std::format("{}/color-management", binaryDir), std::vector<std::string>{mode});
        process.addEnv("WAYLAND_DISPLAY", WLDISPLAY);
        ASSERT(process.runSync(), true);
        EXPECT(process.exitCode(), 0);
        ASSERT_CONTAINS(getFromSocket("/version"), "Hyprland");
    }
}

TEST_CASE(colorManagementInertFeedbackSurvivesOutputChanges) {
    const CScopeGuard restore([] { getFromSocket("/reload"); });
    auto              result = std::async(std::launch::async, [] {
        CProcess process(std::format("{}/color-management", binaryDir), std::vector<std::string>{"feedback-inert"});
        process.addEnv("WAYLAND_DISPLAY", WLDISPLAY);
        return process.runSync() ? process.exitCode() : -1;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    OK(getFromSocket("/eval hl.monitor({ output = 'HEADLESS-1', cm = 'hdr', supports_hdr = 1, supports_wide_color = 1, reference_luminance = 450 })"));
    ASSERT(result.wait_for(std::chrono::seconds(10)) == std::future_status::ready, true);
    EXPECT(result.get(), 0);
    ASSERT_CONTAINS(getFromSocket("/version"), "Hyprland");
}

TEST_CASE(colorManagementReferenceLuminanceIsPerOutputAndOptIn) {
    const CScopeGuard restore([] { getFromSocket("/reload"); });
    for (const int reference : {450, 675, -1}) {
        OK(getFromSocket(std::format("/eval hl.monitor({{ output = 'HEADLESS-1', cm = 'hdr', supports_hdr = 1, supports_wide_color = 1, reference_luminance = {} }})", reference)));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CProcess process(std::format("{}/color-management", binaryDir), std::vector<std::string>{std::format("output-reference={}", reference > 0 ? reference : 203)});
        process.addEnv("WAYLAND_DISPLAY", WLDISPLAY);
        ASSERT(process.runSync(), true);
        EXPECT(process.exitCode(), 0);
    }
}
