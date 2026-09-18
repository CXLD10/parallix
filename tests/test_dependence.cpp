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
