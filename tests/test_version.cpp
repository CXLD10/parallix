#include <gtest/gtest.h>

#include "parallix/version.h"

TEST(Version, IsNotEmpty) {
  EXPECT_STRNE(parallix::version(), "");
}

TEST(Version, IsTheReleaseVersion) { EXPECT_STREQ(parallix::version(), "1.0.0"); }
