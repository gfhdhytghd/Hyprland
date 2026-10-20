#include <managers/input/InputPopupState.hpp>
#include <gtest/gtest.h>

TEST(InputPopupState, UnmappedPopupCannotBlockEvenWithFocusAndStaleGeometry) {
    SInputPopupState popup{.mapped = true, .focused = true, .box = {10, 10, 100, 50}};
    const CBox       monitor{0, 0, 1920, 1080};

    EXPECT_TRUE(popup.visible());
    EXPECT_TRUE(popup.visibleOnMonitor(monitor));
    popup.mapped = false;
    EXPECT_FALSE(popup.visible());
    EXPECT_FALSE(popup.visibleOnMonitor(monitor));
    popup.mapped = true;
    EXPECT_TRUE(popup.visibleOnMonitor(monitor));
}

TEST(InputPopupState, UnfocusedPopupCannotBlock) {
    const SInputPopupState popup{.mapped = true, .focused = false, .box = {10, 10, 100, 50}};
    EXPECT_FALSE(popup.visible());
    EXPECT_FALSE(popup.visibleOnMonitor({0, 0, 1920, 1080}));
}

TEST(InputPopupState, PopupBlocksEveryOverlappedMonitor) {
    const SInputPopupState popup{.mapped = true, .focused = true, .box = {1850, 500, 200, 100}};
    EXPECT_TRUE(popup.visibleOnMonitor({0, 0, 1920, 1080}));
    EXPECT_TRUE(popup.visibleOnMonitor({1920, 0, 1920, 1080}));
    EXPECT_FALSE(popup.visibleOnMonitor({3840, 0, 1920, 1080}));
}

TEST(InputPopupState, PopupOnNeighborDoesNotBlockOwnerMonitor) {
    const SInputPopupState popup{.mapped = true, .focused = true, .box = {1950, 500, 200, 100}};
    EXPECT_FALSE(popup.visibleOnMonitor({0, 0, 1920, 1080}));
    EXPECT_TRUE(popup.visibleOnMonitor({1920, 0, 1920, 1080}));
}

TEST(InputPopupState, LogicalCoordinatesSupportNegativeOriginsAndFractionalEdges) {
    const SInputPopupState popup{.mapped = true, .focused = true, .box = {-0.5, 100, 40, 50}};
    EXPECT_TRUE(popup.visibleOnMonitor({-1280, 0, 1280, 720}));
    EXPECT_TRUE(popup.visibleOnMonitor({0, 0, 1536, 864}));
}

TEST(InputPopupState, TouchingEdgesDoNotBlock) {
    const SInputPopupState popup{.mapped = true, .focused = true, .box = {1920, 1080, 100, 50}};
    EXPECT_FALSE(popup.visibleOnMonitor({0, 0, 1920, 1080}));
    EXPECT_FALSE(popup.visibleOnMonitor({0, 1080, 1920, 1080}));
    EXPECT_FALSE(popup.visibleOnMonitor({1920, 0, 1920, 1080}));
}

TEST(InputPopupState, EmptyPopupDoesNotBlock) {
    for (const CBox box : {CBox{10, 10, 0, 50}, CBox{10, 10, 100, 0}, CBox{}}) {
        const SInputPopupState popup{.mapped = true, .focused = true, .box = box};
        EXPECT_FALSE(popup.visibleOnMonitor({0, 0, 1920, 1080}));
    }
}
