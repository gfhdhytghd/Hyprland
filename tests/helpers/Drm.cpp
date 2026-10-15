#include <helpers/Drm.hpp>

#include <gtest/gtest.h>

#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

TEST(Helpers, drmDevIDFromFDCharacterDevice) {
    const int FD = open("/dev/null", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(FD, 0);

    struct stat stat = {};
    ASSERT_EQ(fstat(FD, &stat), 0);

    const auto devID = DRM::devIDFromFD(FD);
    EXPECT_TRUE(devID.has_value());
    EXPECT_EQ(*devID, stat.st_rdev);

    close(FD);
}

TEST(Helpers, drmDevIDFromFDRejectsRegularFiles) {
    auto      path = (std::filesystem::temp_directory_path() / "hyprland-drm-testXXXXXX").string();
    const int FD   = mkstemp(path.data());
    ASSERT_GE(FD, 0);

    EXPECT_FALSE(DRM::devIDFromFD(FD).has_value());

    close(FD);
    unlink(path.c_str());
}
