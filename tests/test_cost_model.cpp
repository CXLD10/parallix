#include <gtest/gtest.h>

#include <unistd.h>

#include <fstream>

#include "parallix/hardware.h"
#include "parallix/pipeline.h"
#include "parallix/schedule.h"
#include "parallix/witness.h"

using namespace parallix;

namespace {

Analysis Analyze(const std::string& text) {
  std::string path = "build/cost_kernel_" + std::to_string(::getpid()) + ".plx";
  { std::ofstream(path) << text; }
  ParsedSource parsed;
  std::string error;
  EXPECT_TRUE(ParseSourceFile(path, &parsed, &error)) << error;
  Analysis a;
  EXPECT_TRUE(AnalyzeKernelDecl(std::move(parsed.program.kernels[0]), &a, &error)) << error;
  return a;
}

HardwareInfo Cpus(int n) {
  HardwareInfo hw;
  hw.logical_cpus = n;
  hw.l1d_bytes = 48 * 1024;
  hw.l2_bytes = 1024 * 1024;
  hw.l3_bytes = 24 * 1024 * 1024;
  return hw;
}

const char* kAdd = "kernel vadd(A: f32[N], B: f32[N]) -> C: f32[N] { auto i in 0..N { C[i] = A[i] + B[i] } }";
const char* kPairs =
    "kernel pairs(a: i32[N], target: i32) -> count: i64 { reduce(+) i in 0..N { auto j in i+1..N { "
    "count += select(a[i] + a[j] == target, 1, 0) } } }";

}  // namespace

TEST(CostModel, TinyProblemIsDeclinedWithAReason) {
  Analysis a = Analyze(kAdd);
  Profitability p = EstimateProfitability(a.kernel, a.pir, a.schedule, Cpus(16), {{"N", 64}});
  EXPECT_FALSE(p.profitable);
  EXPECT_NE(p.reason.find("not profitable at N=64"), std::string::npos) << p.reason;
  EXPECT_NE(p.reason.find("below the threshold"), std::string::npos) << p.reason;
}

TEST(CostModel, LargeComputeBoundProblemIsParallelizedAcrossManyThreads) {
  Analysis a = Analyze(kPairs);
  Profitability p = EstimateProfitability(a.kernel, a.pir, a.schedule, Cpus(16), {{"N", 100000}});
  EXPECT_TRUE(p.profitable) << p.reason;
  EXPECT_GE(p.threads, 8);
  EXPECT_LE(p.threads, 16);
  EXPECT_FALSE(p.memory_bound);
}

TEST(CostModel, TriangularIterationCountIsExactForModestSizes) {
  Analysis a = Analyze(kPairs);
  Profitability p = EstimateProfitability(a.kernel, a.pir, a.schedule, Cpus(4), {{"N", 1000}});
  EXPECT_DOUBLE_EQ(p.iterations, 1000.0 * 999.0 / 2.0);
}

TEST(CostModel, SingleCpuNeverParallelizes) {
  Analysis a = Analyze(kPairs);
  Profitability p = EstimateProfitability(a.kernel, a.pir, a.schedule, Cpus(1), {{"N", 100000}});
  EXPECT_FALSE(p.profitable);
  EXPECT_EQ(p.threads, 1);
  EXPECT_NE(p.reason.find("only 1 logical CPU"), std::string::npos) << p.reason;
}

TEST(CostModel, UnsafeLoopIsNotApplicable) {
  Analysis a = Analyze("kernel fib(dp: i32[N]) -> dp: i32[N] { auto i in 2..N { dp[i] = dp[i-1] + dp[i-2] } }");
  Profitability p = EstimateProfitability(a.kernel, a.pir, a.schedule, Cpus(8), {{"N", 1000000}});
  EXPECT_FALSE(p.profitable);
  EXPECT_NE(p.reason.find("not safe"), std::string::npos);
}

TEST(CostModel, ThreadChoiceNeverShrinksAsTheProblemGrows) {
  Analysis a = Analyze(kPairs);
  int prev = 0;
  for (long long n : {2000, 20000, 200000, 2000000}) {
    Profitability p = EstimateProfitability(a.kernel, a.pir, a.schedule, Cpus(20), {{"N", n}});
    EXPECT_GE(p.threads, prev) << n;
    prev = p.threads;
  }
}

TEST(CostModel, StreamingLoopIsFlaggedMemoryBound) {
  Analysis a = Analyze(kAdd);
  Profitability p = EstimateProfitability(a.kernel, a.pir, a.schedule, Cpus(20), {{"N", 100000000}});
  EXPECT_TRUE(p.memory_bound);
  EXPECT_LE(p.threads, 20);
}

TEST(Hardware, DetectionReturnsUsableNumbers) {
  HardwareInfo hw = DetectHardware();
  EXPECT_GE(hw.logical_cpus, 1);
  EXPECT_GT(hw.l1d_bytes, 0);
  EXPECT_GT(hw.l2_bytes, 0);
  EXPECT_GT(hw.l3_bytes, 0);
  EXPECT_FALSE(hw.source.empty());
}
