#include <config/shared/monitor/MonitorRule.hpp>
#include <gtest/gtest.h>

TEST(MonitorReferenceLuminance, DefaultsRemainOptInAndChangesAreSoft) {
    Config::CMonitorRule original, changed;
    EXPECT_EQ(original.m_referenceLuminance, -1);
    EXPECT_EQ(original.compare(changed), Config::COMPARISON_FULL_MATCH);
    changed.m_referenceLuminance = 450;
    EXPECT_EQ(original.compare(changed), Config::COMPARISON_SOFT_MISMATCH);
}
