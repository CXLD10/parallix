#include <gtest/gtest.h>

#include "parallix/version.h"

TEST(Version, IsNotEmpty) {
  EXPECT_STRNE(parallix::version(), "");
}
