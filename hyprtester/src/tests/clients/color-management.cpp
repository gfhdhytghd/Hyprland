#include "../../hyprctlCompat.hpp"
#include "../../shared.hpp"
#include "../shared.hpp"
#include "build.hpp"
#include "tests.hpp"
#include <hyprutils/os/Process.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <algorithm>
#include <csignal>
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

TEST_CASE(colorManagementFeedbackSurvivesLastOutputRemoval) {
    // Retain the old monitor across output destruction, as in physical unplugging.
    // Its weak surface/window references must stay valid when FALLBACK is added.
    std::vector<std::string> outputs;
    const auto               monitors = getFromSocket("/monitors");
    for (size_t pos = 0; (pos = monitors.find("Monitor ", pos)) != std::string::npos;) {
        pos += 8;
        const auto end = monitors.find(" (ID ", pos);
        ASSERT(end != std::string::npos, true);
        const auto name = monitors.substr(pos, end - pos);
        if (name != "FALLBACK") {
            ASSERT(name.starts_with("HEADLESS-"), true);
            outputs.push_back(name);
        }
        pos = end;
    }
    ASSERT(std::ranges::find(outputs, std::string{"HEADLESS-1"}) != outputs.end(), true);
    const CScopeGuard restore([&] {
        getFromSocket("/eval hl.plugin.test.detached_monitor_feedback('clear')");
        Tests::killAllWindows();
        for (const auto& output : outputs)
            getFromSocket(std::format("/output create headless {}", output));
        getFromSocket("/reload");
        Tests::sync();
    });
    OK(getFromSocket("/eval hl.config({ render = { cm_auto_hdr = 1 } })"));
    for (const auto& output : outputs) {
        if (output == "HEADLESS-1")
            continue;
        OK(getFromSocket(std::format("/output remove {}", output)));
    }
    OK(getFromSocket("/dispatch hl.dsp.focus({ monitor = 'HEADLESS-1' })"));
    CProcess process(std::format("{}/color-management", binaryDir), std::vector<std::string>{"feedback-live"});
    process.addEnv("WAYLAND_DISPLAY", WLDISPLAY);
    ASSERT(process.runAsync(), true);
    const CScopeGuard stopClient([&] { kill(process.pid(), SIGKILL); });
    bool              mapped = false;
    for (int i = 0; i < 50; ++i) {
        if (getFromSocket("/clients").contains("class: cm-feedback-live")) {
            mapped = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT(mapped, true);
    OK(getFromSocket("/eval hl.plugin.test.detached_monitor_feedback('retain')"));
    OK(getFromSocket("/output remove HEADLESS-1"));
    bool fallback = false;
    for (int i = 0; i < 50; ++i) {
        if (getFromSocket("/monitors all").contains("Monitor FALLBACK (ID ")) {
            fallback = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT(fallback, true);
    OK(getFromSocket("/eval hl.plugin.test.detached_monitor_feedback('check')"));
    ASSERT_CONTAINS(getFromSocket("/version"), "Hyprland");
    // Reconnect while feedback is still active, and verify that the same client
    // remains mapped after both preferred-description transitions.
    OK(getFromSocket("/output create headless HEADLESS-1"));
    Tests::sync();
    ASSERT_CONTAINS(getFromSocket("/clients"), "class: cm-feedback-live");
    ASSERT_CONTAINS(getFromSocket("/version"), "Hyprland");
}
