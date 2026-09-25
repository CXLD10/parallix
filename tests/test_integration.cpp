// Integration tests: these actually shell out to compile and run generated
// code (specs/06's acceptance criteria explicitly call for this, not just
// structural string checks on the source).

#include <filesystem>
#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

#include "parallix/codegen.h"
#include "parallix/compiler_driver.h"
#include "parallix/harness.h"
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

std::string ScratchDir() {
  std::filesystem::path dir = std::filesystem::path("build") / "test_generated";
  std::filesystem::create_directories(dir);
  return dir.string();
}

// Runs the whole M7-M10 pipeline for one example file and returns the
// compile+run result of its harness (or smoke test).
CompileRunResult BuildAndRunHarness(const std::string& example_path, const std::string& tag,
                                    CodegenResult* gen_out = nullptr,
                                    HarnessResult* harness_out = nullptr) {
  std::string src = ReadFile(example_path);
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
  HarnessResult harness = GenerateHarness(kernel, pir, schedule, gen);

  std::string dir = ScratchDir();
  std::string c_path = dir + "/" + tag + "_harness.c";
  std::string bin_path = dir + "/" + tag + "_harness_bin";
  {
    std::ofstream out(c_path);
    out << harness.c_source;
  }
  if (gen_out) *gen_out = gen;
  if (harness_out) *harness_out = harness;
  return CompileAndRun(c_path, bin_path);
}

}  // namespace

TEST(Integration, T1CompilesRunsAndCorroborates) {
  CompileRunResult result = BuildAndRunHarness("examples/t1_vector_add.plx", "t1");
  ASSERT_TRUE(result.compiler_found) << result.error;
  ASSERT_TRUE(result.compiled) << result.compile_output;
  ASSERT_TRUE(result.ran);
  EXPECT_EQ(result.exit_code, 0) << result.run_output;
  EXPECT_NE(result.run_output.find("CERTIFICATE: vector_add STATICALLY SAFE, EMPIRICALLY "
                                    "CORROBORATED"),
            std::string::npos)
      << result.run_output;
}

TEST(Integration, T3CompilesRunsAndCorroborates) {
  CompileRunResult result = BuildAndRunHarness("examples/t3_reduce_sum.plx", "t3");
  ASSERT_TRUE(result.compiler_found) << result.error;
  ASSERT_TRUE(result.compiled) << result.compile_output;
  ASSERT_TRUE(result.ran);
  EXPECT_EQ(result.exit_code, 0) << result.run_output;
  EXPECT_NE(result.run_output.find("CERTIFICATE: reduce_sum STATICALLY SAFE, EMPIRICALLY "
                                    "CORROBORATED"),
            std::string::npos)
      << result.run_output;
}

TEST(Integration, T4CompilesRunsAndCorroborates) {
  CompileRunResult result = BuildAndRunHarness("examples/t4_stencil5.plx", "t4");
  ASSERT_TRUE(result.compiler_found) << result.error;
  ASSERT_TRUE(result.compiled) << result.compile_output;
  ASSERT_TRUE(result.ran);
  EXPECT_EQ(result.exit_code, 0) << result.run_output;
  EXPECT_NE(result.run_output.find("CERTIFICATE: stencil5 STATICALLY SAFE, EMPIRICALLY "
                                    "CORROBORATED"),
            std::string::npos)
      << result.run_output;
}

TEST(Integration, T2SequentialOnlyCompilesAndRunsWithNoCertificateAttempt) {
  CodegenResult gen;
  HarnessResult harness;
  CompileRunResult result = BuildAndRunHarness("examples/t2_prefix_like.plx", "t2", &gen, &harness);
  ASSERT_TRUE(result.compiler_found) << result.error;
  ASSERT_TRUE(result.compiled) << result.compile_output;
  ASSERT_TRUE(result.ran);
  EXPECT_EQ(result.exit_code, 0) << result.run_output;

  EXPECT_TRUE(gen.par_function_name.empty());
  EXPECT_FALSE(harness.attempts_certificate);
  EXPECT_EQ(result.run_output.find("CERTIFICATE"), std::string::npos);
  EXPECT_NE(result.run_output.find("SMOKE TEST: prefix_like"), std::string::npos);
}

TEST(Integration, T5AndT6NeverReachSchedulingOrCodegen) {
  for (const std::string& path :
       {std::string("examples/t5_non_affine.plx"), std::string("examples/t6_undeclared.plx")}) {
    std::string src = ReadFile(path);
    Lexer lexer(src);
    Parser parser(lexer.Tokenize());
    Program program = parser.ParseProgram();
    Sema sema;
    EXPECT_THROW(sema.AnalyzeKernel(program.kernels[0]), SemaError) << path;
    // Never reaching lowering/scheduling/codegen for a semantically-rejected
    // kernel is exactly what makes it safe to skip generating and running
    // any code for it — nothing downstream is ever invoked.
  }
}
