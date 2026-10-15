#include "parallix/dependence.h"

#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

#include "parallix/lexer.h"
#include "parallix/lowering.h"
#include "parallix/parser.h"
#include "parallix/sema.h"

using namespace parallix;

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

PIRKernel LowerFile(const std::string& path) {
  std::string src = ReadFile(path);
  Lexer lexer(src);
  Parser parser(lexer.Tokenize());
  Program program = parser.ParseProgram();
  Sema sema;
  sema.AnalyzeKernel(program.kernels[0]);
  return LowerKernel(program.kernels[0]);
}

}  // namespace

TEST(Dependence, T1VectorAddIsSafe) {
  PIRKernel pir = LowerFile("examples/t1_vector_add.plx");
  DependenceReport report = AnalyzeDependence(pir);
  EXPECT_EQ(report.verdict, Verdict::SAFE);
  for (const auto& pr : report.pairs) {
    EXPECT_FALSE(pr.dependent);
  }
}

TEST(Dependence, T2PrefixLikeIsUnsafeRawDistanceOne) {
  PIRKernel pir = LowerFile("examples/t2_prefix_like.plx");
  DependenceReport report = AnalyzeDependence(pir);
  EXPECT_EQ(report.verdict, Verdict::UNSAFE);

  bool found_raw_distance_one = false;
  for (const auto& pr : report.pairs) {
    if (pr.dependent && pr.type == DependenceType::RAW) {
      ASSERT_TRUE(pr.distance_known);
      EXPECT_EQ(pr.distance, 1);
      found_raw_distance_one = true;
    }
  }
  EXPECT_TRUE(found_raw_distance_one);
}

TEST(Dependence, T3ReduceSumIsSafeAsReduction) {
  PIRKernel pir = LowerFile("examples/t3_reduce_sum.plx");
  DependenceReport report = AnalyzeDependence(pir);
  EXPECT_EQ(report.verdict, Verdict::SAFE_AS_REDUCTION);
  // A[i] is read-only in this kernel; no array reference pair should be
  // classified as dependent at all (the licensed dependency is scalar).
  for (const auto& pr : report.pairs) {
    EXPECT_FALSE(pr.dependent);
  }
}

TEST(Dependence, T4Stencil5IsSafe) {
  PIRKernel pir = LowerFile("examples/t4_stencil5.plx");
  DependenceReport report = AnalyzeDependence(pir);
  EXPECT_EQ(report.verdict, Verdict::SAFE);
  for (const auto& pr : report.pairs) {
    EXPECT_FALSE(pr.dependent);
  }
}

// ---- self output dependence (a write that ignores the parallel loop index) --

namespace {
DependenceReport AnalyzeSource(const std::string& src) {
  Lexer lexer(src);
  Parser parser(lexer.Tokenize());
  Program program = parser.ParseProgram();
  return AnalyzeDependence(LowerKernel(program.kernels[0]));
}
}  // namespace

TEST(DependenceSelfWrite, ConstantSubscriptWriteIsUnsafe) {
  DependenceReport r =
      AnalyzeSource("kernel k(A: f32[N]) -> C: f32[N] { auto i in 0..N { C[0] = A[i] } }");
  EXPECT_EQ(r.verdict, Verdict::UNSAFE);
  ASSERT_FALSE(r.pairs.empty());
  EXPECT_EQ(r.pairs.back().type, DependenceType::WAW);
  EXPECT_NE(r.pairs.back().reasoning.find("output dependence"), std::string::npos);
}

TEST(DependenceSelfWrite, InnerIndexOnlyWriteUnderOuterLoopIsUnsafe) {
  DependenceReport r = AnalyzeSource(
      "kernel k(A: f32[N,M]) -> C: f32[M] { auto i in 0..N { auto j in 0..M { C[j] = A[i, j] } } }");
  EXPECT_EQ(r.verdict, Verdict::UNSAFE);
}

TEST(DependenceSelfWrite, WriteUsingOuterIndexStaysSafe) {
  EXPECT_EQ(AnalyzeSource("kernel k(A: f32[N,M]) -> C: f32[N,M] { auto i in 0..N { "
                          "auto j in 0..M { C[i, j] = A[i, j] } } }")
                .verdict,
            Verdict::SAFE);
}

TEST(DependenceSelfWrite, ReductionAccumulatorIsNotAnArrayWrite) {
  EXPECT_EQ(AnalyzeSource("kernel k(A: f32[N]) -> s: f32 { reduce(+) i in 0..N { s += A[i] } }")
                .verdict,
            Verdict::SAFE_AS_REDUCTION);
}

// ---- distance-0 pairs on the parallel index are not loop-carried -------------

TEST(DependenceDistanceZero, InPlaceElementwiseUpdateIsSafe) {
  EXPECT_EQ(AnalyzeSource("kernel k(A: f32[N], B: f32[N]) -> B: f32[N] { auto i in 0..N { "
                          "B[i] = B[i] + A[i] } }")
                .verdict,
            Verdict::SAFE);
}

TEST(DependenceDistanceZero, NonZeroDistanceStaysUnsafe) {
  EXPECT_EQ(AnalyzeSource("kernel k(A: f32[N]) -> A: f32[N] { auto i in 1..N { A[i] = A[i-1] } }").verdict,
            Verdict::UNSAFE);
}

TEST(DependenceDistanceZero, ZeroDistanceOnAnInnerIndexIsStillCarriedByTheOuterLoop) {
  // A[j] is re-written for every i, so iterations of i conflict.
  EXPECT_EQ(AnalyzeSource("kernel k(B: f32[N,M]) -> A: f32[M] { auto i in 0..N { auto j in 0..M { "
                          "A[j] = A[j] + B[i, j] } } }")
                .verdict,
            Verdict::UNSAFE);
}

TEST(DependenceDistanceZero, FixedElementReadModifyWriteStaysUnsafe) {
  EXPECT_EQ(AnalyzeSource("kernel k(B: f32[N]) -> A: f32[N] { auto i in 0..N { A[0] = A[0] + B[i] } }").verdict,
            Verdict::UNSAFE);
}

TEST(DependenceDistanceZero, TwoDimensionalInPlaceUpdateIsSafe) {
  EXPECT_EQ(AnalyzeSource("kernel k(B: f32[N,M]) -> A: f32[N,M] { auto i in 0..N { auto j in 0..M { "
                          "A[i, j] = A[i, j] + B[i, j] } } }")
                .verdict,
            Verdict::SAFE);
}
