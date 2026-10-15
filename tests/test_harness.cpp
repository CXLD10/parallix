#include "parallix/harness.h"

#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

#include "parallix/codegen.h"
#include "parallix/lexer.h"
#include "parallix/lowering.h"
#include "parallix/parser.h"
#include "parallix/schedule.h"
#include "parallix/sema.h"

using namespace parallix;

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

HarnessResult HarnessForFile(const std::string& path) {
  std::string src = ReadFile(path);
  Lexer lexer(src);
  Parser parser(lexer.Tokenize());
  Program program = parser.ParseProgram();
  KernelDecl kernel = std::move(program.kernels[0]);
  Sema sema;
  sema.AnalyzeKernel(kernel);
  PIRKernel pir = LowerKernel(kernel);
  DependenceReport report = AnalyzeDependence(pir);
  Schedule schedule = GenerateSchedule(pir, report);
  CodegenResult gen = GenerateCCode(kernel, pir, schedule);
  return GenerateHarness(kernel, pir, schedule, gen);
}

}  // namespace

TEST(Harness, MapKernelComparesExactlyAndHasAllCommands) {
  HarnessResult h = HarnessForFile("examples/t1_vector_add.plx");
  EXPECT_TRUE(h.attempts_certificate);
  EXPECT_NE(h.c_source.find("plx_cmd_validate"), std::string::npos);
  EXPECT_NE(h.c_source.find("plx_cmd_refute"), std::string::npos);
  EXPECT_NE(h.c_source.find("plx_cmd_bench"), std::string::npos);
  EXPECT_NE(h.c_source.find("vector_add_ord("), std::string::npos);
  EXPECT_NE(h.c_source.find("vector_add_par("), std::string::npos);
  EXPECT_NE(h.c_source.find("[k] != got->C[k]"), std::string::npos);
  EXPECT_EQ(h.c_source.find("1e-4 * den"), std::string::npos);
}

TEST(Harness, FloatSumReductionUsesRelativeTolerance) {
  HarnessResult h = HarnessForFile("examples/t3_reduce_sum.plx");
  EXPECT_NE(h.c_source.find("1e-4 * den"), std::string::npos);
  EXPECT_NE(h.c_source.find("fabs"), std::string::npos);
}

TEST(Harness, StencilMapIsExact) {
  HarnessResult h = HarnessForFile("examples/t4_stencil5.plx");
  EXPECT_NE(h.c_source.find("[k] != got->B[k]"), std::string::npos);
  EXPECT_EQ(h.c_source.find("1e-4 * den"), std::string::npos);
}

TEST(Harness, UnsafeKernelHasNoParallelCallButKeepsOrderedVariant) {
  HarnessResult h = HarnessForFile("examples/t2_prefix_like.plx");
  EXPECT_FALSE(h.attempts_certificate);
  EXPECT_EQ(h.c_source.find("prefix_like_par("), std::string::npos);
  EXPECT_NE(h.c_source.find("prefix_like_ord("), std::string::npos);
  EXPECT_NE(h.c_source.find("#define PLX_HAS_PAR 0"), std::string::npos);
}
