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

TEST(Harness, T1ExactEqualityCertificate) {
  HarnessResult h = HarnessForFile("examples/t1_vector_add.plx");
  EXPECT_TRUE(h.attempts_certificate);
  EXPECT_NE(h.c_source.find("!="), std::string::npos);
  EXPECT_NE(h.c_source.find("CERTIFICATE: vector_add STATICALLY SAFE, EMPIRICALLY CORROBORATED"),
            std::string::npos);
  EXPECT_NE(h.c_source.find("vector_add_seq("), std::string::npos);
  EXPECT_NE(h.c_source.find("vector_add_par("), std::string::npos);
}

TEST(Harness, T4ExactEqualityCertificate) {
  HarnessResult h = HarnessForFile("examples/t4_stencil5.plx");
  EXPECT_TRUE(h.attempts_certificate);
  EXPECT_NE(h.c_source.find("!="), std::string::npos);
  EXPECT_NE(h.c_source.find("CERTIFICATE: stencil5 STATICALLY SAFE, EMPIRICALLY CORROBORATED"),
            std::string::npos);
}

TEST(Harness, T3EpsilonRelativeToleranceCertificate) {
  HarnessResult h = HarnessForFile("examples/t3_reduce_sum.plx");
  EXPECT_TRUE(h.attempts_certificate);
  EXPECT_NE(h.c_source.find("rel"), std::string::npos);
  EXPECT_NE(h.c_source.find("fabs"), std::string::npos);
  EXPECT_NE(h.c_source.find("CERTIFICATE: reduce_sum STATICALLY SAFE, EMPIRICALLY CORROBORATED"),
            std::string::npos);
  // No exact "!=" element comparison — reduction uses tolerance, not
  // bit-exact equality (specs/06 section 4).
  EXPECT_EQ(h.c_source.find("_seq[idx] !="), std::string::npos);
}

TEST(Harness, T2UnsafeKernelHasNoCertificateOnlySmokeTest) {
  HarnessResult h = HarnessForFile("examples/t2_prefix_like.plx");
  EXPECT_FALSE(h.attempts_certificate);
  EXPECT_EQ(h.c_source.find("CERTIFICATE"), std::string::npos);
  EXPECT_NE(h.c_source.find("SMOKE TEST: prefix_like"), std::string::npos);
  EXPECT_NE(h.c_source.find("prefix_like_seq("), std::string::npos);
  EXPECT_EQ(h.c_source.find("prefix_like_par"), std::string::npos);
}
