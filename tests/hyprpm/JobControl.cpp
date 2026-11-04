#include "../../hyprpm/src/helpers/JobControl.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sys/wait.h>

using namespace NJobControl;

TEST(HyprpmJobControl, ParsesOnlySupportedRange) {
    EXPECT_EQ(parse("1"), 1);
    EXPECT_EQ(parse("256"), 256);
    EXPECT_FALSE(parse(""));
    EXPECT_FALSE(parse("0"));
    EXPECT_FALSE(parse("-1"));
    EXPECT_FALSE(parse("+1"));
    EXPECT_FALSE(parse(" 1"));
    EXPECT_FALSE(parse("1 "));
    EXPECT_FALSE(parse("1x"));
    EXPECT_FALSE(parse("257"));
    EXPECT_FALSE(parse(std::to_string(std::numeric_limits<size_t>::max()) + "0"));
}

TEST(HyprpmJobControl, PreservesDefaultBuildCommand) {
    EXPECT_TRUE(buildEnvironment(0).empty());
    EXPECT_EQ(pluginBuildCommand("/tmp/plugin", "PKG_CONFIG_PATH=\"/tmp/pkgconfig\"", "first && second", 0),
              "cd /tmp/plugin && PKG_CONFIG_PATH=\"/tmp/pkgconfig\" first && second");
    EXPECT_EQ(headersInstallCommand("/tmp/source", "/tmp/headers", 0),
              "(make -C '/tmp/source' PREFIX='/tmp/headers' installheaders && chmod -R 644 '/tmp/headers' && find '/tmp/headers' -type d -exec chmod a+x {} \\;); "
              "install_status=$?; rm -rf -- '/tmp/source'; cleanup_status=$?; "
              "if [ \"$install_status\" -ne 0 ]; then exit \"$install_status\"; fi; exit \"$cleanup_status\"");
}

TEST(HyprpmJobControl, AppliesLimitToEntireBuildStepAndHeaders) {
    const auto ENVIRONMENT = buildEnvironment(4) + "PKG_CONFIG_PATH=\"/tmp/pkgconfig\"";

    EXPECT_EQ(ENVIRONMENT, "export CMAKE_BUILD_PARALLEL_LEVEL=4 MAKEFLAGS=-j4; PKG_CONFIG_PATH=\"/tmp/pkgconfig\"");
    EXPECT_EQ(pluginBuildCommand("/tmp/plugin", ENVIRONMENT, "first && second", 4),
              "cd /tmp/plugin && (export CMAKE_BUILD_PARALLEL_LEVEL=4 MAKEFLAGS=-j4; PKG_CONFIG_PATH=\"/tmp/pkgconfig\" first && second)");
    EXPECT_EQ(headersInstallCommand("/tmp/source", "/tmp/headers", 4),
              "(make -j4 -C '/tmp/source' PREFIX='/tmp/headers' installheaders && chmod -R 644 '/tmp/headers' && find '/tmp/headers' -type d -exec chmod a+x {} \\;); "
              "install_status=$?; rm -rf -- '/tmp/source'; cleanup_status=$?; "
              "if [ \"$install_status\" -ne 0 ]; then exit \"$install_status\"; fi; exit \"$cleanup_status\"");
}

class HyprpmHeaderInstall : public ::testing::Test {
  protected:
    std::filesystem::path m_directory;

    void                  SetUp() override {
        auto       pattern   = (std::filesystem::temp_directory_path() / "hyprpm-install-test-XXXXXX").string();
        const auto directory = mkdtemp(pattern.data());
        ASSERT_NE(directory, nullptr);
        m_directory = directory;
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(m_directory, ec);
        EXPECT_FALSE(ec) << ec.message();
    }

    void checkInstall(bool fail) {
        const auto source  = m_directory / "source with spaces";
        const auto headers = m_directory / "headers with spaces";
        std::filesystem::create_directory(source);
        const auto tools = m_directory / "tools";
        std::filesystem::create_directory(tools);
        {
            // Production runs chmod as root. Keep directory traversal available
            // in this unprivileged shell test; make, find and rm remain real.
            std::ofstream chmod(tools / "chmod");
            chmod << "#!/bin/sh\nexit 0\n";
            ASSERT_TRUE(chmod.good());
        }
        std::filesystem::permissions(tools / "chmod", std::filesystem::perms::owner_all);
        {
            std::ofstream makefile(source / "Makefile");
            makefile << ".PHONY: installheaders\ninstallheaders:\n"
                        "\tmkdir -p build/CMakeFiles/Progress \"$(PREFIX)/include\"\n"
                        "\ttouch build/CMakeFiles/Progress/98 \"$(PREFIX)/include/example.hpp\"\n";
            if (fail)
                makefile << "\texit 37\n";
            ASSERT_TRUE(makefile.good());
        }
        const auto status = std::system(("PATH='" + tools.string() + "':/usr/bin:/bin; " + headersInstallCommand(source.string(), headers.string(), 2)).c_str());
        ASSERT_NE(status, -1);
        ASSERT_TRUE(WIFEXITED(status));
        // GNU make reports recipe errors as 2, regardless of the recipe's exit code.
        EXPECT_EQ(WEXITSTATUS(status), fail ? 2 : 0);
        EXPECT_FALSE(std::filesystem::exists(source));
        EXPECT_TRUE(std::filesystem::is_regular_file(headers / "include/example.hpp"));
    }
};

TEST_F(HyprpmHeaderInstall, RemovesBuildTreeAndPreservesInstalledHeaders) {
    checkInstall(false);
}

TEST_F(HyprpmHeaderInstall, CleansAfterFailureAndPreservesInstallerFailureStatus) {
    checkInstall(true);
}
