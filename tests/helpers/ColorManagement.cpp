#include <helpers/cm/ColorManagement.hpp>
#include <render/types.hpp>
#include <gtest/gtest.h>

using namespace NColorManagement;

TEST(ColorManagement, GenericFilmChannelOrder) {
    const auto& p = getPrimaries(CM_PRIMARIES_GENERIC_FILM);
    EXPECT_DOUBLE_EQ(p.red.x, 0.681);
    EXPECT_DOUBLE_EQ(p.green.x, 0.243);
    EXPECT_DOUBLE_EQ(p.blue.x, 0.145);
}
