// Witness construction and counterexamples for UNSAFE kernels.

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "parallix/pipeline.h"
#include "parallix/verify.h"
#include "parallix/witness.h"

using namespace parallix;

// A directory unique to the running test, so tests can run in parallel.
inline std::string UniqueTestDir() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  return std::string(info ? info->test_suite_name() : "x") + "_" + (info ? info->name() : "x");
}

namespace {

std::string Scratch() {
  std::filesystem::path dir = std::filesystem::path("build") / "test_generated" / UniqueTestDir();
  std::filesystem::create_directories(dir);
  return dir.string();
}

Analysis AnalyzeText(const std::string& text) {
  std::string path = Scratch() + "/refute_kernel.plx";
  { std::ofstream(path) << text; }
  ParsedSource parsed;
  std::string error;
  EXPECT_TRUE(ParseSourceFile(path, &parsed, &error)) << error;
  Analysis a;
  EXPECT_TRUE(AnalyzeKernelDecl(std::move(parsed.program.kernels[0]), &a, &error)) << error;
  return a;
}

const char* kFib = "kernel fib(dp: i32[N]) -> dp: i32[N] { auto i in 2..N { dp[i] = dp[i-1] + dp[i-2] } }";

long long Get(const std::vector<std::pair<std::string, long long>>& v, const std::string& k) {
  for (const auto& p : v) {
    if (p.first == k) return p.second;
  }
  return -999;
}

}  // namespace

TEST(Witness, FibonacciRecurrenceNamesTheTwoIterations) {
  Analysis a = AnalyzeText(kFib);
  std::vector<Witness> ws = FindWitnesses(a.pir, a.report, a.gen.dim_params, 12);
  ASSERT_FALSE(ws.empty());
  const Witness& w = ws.front();
  ASSERT_TRUE(w.found) << w.note;
  EXPECT_EQ(w.type, DependenceType::RAW);
  EXPECT_EQ(w.array, "dp");
  EXPECT_EQ(Get(w.first_iter, "i"), 2);   // writes dp[2]
  EXPECT_EQ(Get(w.second_iter, "i"), 3);  // reads dp[2] through dp[i - 1]
  EXPECT_EQ(w.element, std::vector<long long>({2}));
  EXPECT_NE(w.explanation.find("RAW"), std::string::npos);
}

TEST(Witness, DistanceTwoPairGetsItsOwnWitness) {
  Analysis a = AnalyzeText(kFib);
  std::vector<Witness> ws = FindWitnesses(a.pir, a.report, a.gen.dim_params, 12);
  bool saw_gap_two = false;
  for (const Witness& w : ws) {
    if (w.found && Get(w.second_iter, "i") - Get(w.first_iter, "i") == 2) saw_gap_two = true;
  }
  EXPECT_TRUE(saw_gap_two);
}

TEST(Witness, SelfOutputDependenceIsWaw) {
  Analysis a = AnalyzeText("kernel k(A: f32[N]) -> C: f32[N] { auto i in 0..N { C[0] = A[i] } }");
  std::vector<Witness> ws = FindWitnesses(a.pir, a.report, a.gen.dim_params, 8);
  ASSERT_FALSE(ws.empty());
  ASSERT_TRUE(ws.back().found);
  EXPECT_EQ(ws.back().type, DependenceType::WAW);
  EXPECT_EQ(ws.back().element, std::vector<long long>({0}));
}

TEST(Witness, SafeKernelHasNone) {
  Analysis a = AnalyzeText("kernel k(A: f32[N], B: f32[N]) -> C: f32[N] { auto i in 0..N { C[i] = A[i] + B[i] } }");
  EXPECT_TRUE(FindWitnesses(a.pir, a.report, a.gen.dim_params, 8).empty());
}

TEST(Witness, TwoDimensionalConflictCarriedByTheOuterLoop) {
  Analysis a = AnalyzeText(
      "kernel k(A: f32[N,M]) -> A: f32[N,M] { auto i in 1..N { auto j in 0..M { A[i, j] = A[i-1, j] } } }");
  std::vector<Witness> ws = FindWitnesses(a.pir, a.report, a.gen.dim_params, 8);
  ASSERT_FALSE(ws.empty());
  ASSERT_TRUE(ws.front().found);
  EXPECT_EQ(Get(ws.front().second_iter, "i") - Get(ws.front().first_iter, "i"), 1);
  EXPECT_EQ(Get(ws.front().first_iter, "j"), Get(ws.front().second_iter, "j"));
}

TEST(Witness, DimsFollowTheHarnessFormula) {
  auto d = DimsForSize({"N", "M", "K"}, 100);
  EXPECT_EQ(d["N"], 100);
  EXPECT_EQ(d["M"], 100 / 2 + 4);
  EXPECT_EQ(d["K"], 100 / 4 + 4);
}

TEST(Refutation, FibonacciReversedOrderDivergesAtTheFirstDependentIndex) {
  Analysis a = AnalyzeText(kFib);
  RefutationOptions opts;
  opts.work_dir = Scratch();
  RefutationReport r = RunRefutation(a, opts);
  ASSERT_TRUE(r.built) << r.error;
  EXPECT_TRUE(r.diverged);
  EXPECT_EQ(r.index, 3);  // dp[2] is computed from untouched dp[0], dp[1] in either order
  EXPECT_NE(r.sequential_value, r.reversed_value);
  EXPECT_EQ(r.location, "dp[3]");
  EXPECT_TRUE(r.Refuted());
  EXPECT_FALSE(r.forced);
  std::string text = FormatRefutation(a, r);
  EXPECT_NE(text.find("CERTIFICATE: REFUTED"), std::string::npos) << text;
  EXPECT_NE(text.find("first diverging index 3"), std::string::npos) << text;
  EXPECT_NE(text.find("WITNESS: loop-carried RAW"), std::string::npos) << text;
  EXPECT_EQ(text.find("FORCED PARALLEL"), std::string::npos);
}

TEST(Refutation, ForcedParallelIsReportedAsNondeterministic) {
  Analysis a = AnalyzeText(kFib);
  RefutationOptions opts;
  opts.work_dir = Scratch();
  opts.force_parallel = true;
  opts.size = 4000;
  opts.forced_reps = 50;
  RefutationReport r = RunRefutation(a, opts);
  ASSERT_TRUE(r.built) << r.error;
  EXPECT_TRUE(r.forced);
  EXPECT_EQ(r.forced_runs, 50);
  EXPECT_LE(r.forced_differing, 50);
  std::string text = FormatRefutation(a, r);
  EXPECT_NE(text.find("nondeterministic"), std::string::npos) << text;
  EXPECT_NE(text.find("CERTIFICATE: REFUTED"), std::string::npos) << text;
}

TEST(Refutation, SelfWriteRaceIsRefutedByReversal) {
  Analysis a = AnalyzeText("kernel k(A: i32[N]) -> C: i32[N] { auto i in 0..N { C[0] = A[i] } }");
  RefutationOptions opts;
  opts.work_dir = Scratch();
  RefutationReport r = RunRefutation(a, opts);
  ASSERT_TRUE(r.built) << r.error;
  EXPECT_TRUE(r.Refuted()) << FormatRefutation(a, r);
  EXPECT_EQ(r.location, "C[0]");
}
