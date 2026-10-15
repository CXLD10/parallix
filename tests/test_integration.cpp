// Integration tests: these actually shell out to compile and run generated
// code.

#include <filesystem>
#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

#include "parallix/codegen.h"
#include "parallix/compiler_driver.h"
#include "parallix/harness.h"
#include "parallix/pipeline.h"
#include "parallix/verify.h"
#include "parallix/lexer.h"
#include "parallix/lowering.h"
#include "parallix/parser.h"
#include "parallix/schedule.h"
#include "parallix/sema.h"

using namespace parallix;

// A directory unique to the running test, so tests can run in parallel.
inline std::string UniqueTestDir() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  return std::string(info ? info->test_suite_name() : "x") + "_" + (info ? info->name() : "x");
}

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string ScratchDir() {
  std::filesystem::path dir = std::filesystem::path("build") / "test_generated" / UniqueTestDir();
  std::filesystem::create_directories(dir);
  return dir.string();
}

}  // namespace

namespace {

Analysis AnalyzeExample(const std::string& path) {
  ParsedSource parsed;
  std::string error;
  EXPECT_TRUE(ParseSourceFile(path, &parsed, &error)) << error;
  Analysis a;
  EXPECT_TRUE(AnalyzeKernelDecl(std::move(parsed.program.kernels[0]), &a, &error)) << error;
  return a;
}

ValidationReport Validate(const std::string& path, bool tsan = false) {
  ValidationOptions opts;
  opts.tsan = tsan;
  opts.work_dir = ScratchDir();
  return RunValidation(AnalyzeExample(path), opts);
}

void ExpectFullCoverage(const ValidationReport& r) {
  ASSERT_TRUE(r.built) << r.error;
  EXPECT_EQ(r.order_mismatches, 0);
  EXPECT_EQ(r.par_mismatches, 0);
  // reversed + 3 permutations, 3 sizes, 3 seeds
  EXPECT_EQ(r.order_runs, 4 * 3 * 3);
  // every thread count x 20 repetitions x 3 sizes x 3 seeds
  EXPECT_EQ(r.par_runs, static_cast<long long>(r.threads.size()) * 20 * 3 * 3);
  EXPECT_GE(r.threads.size(), 4u);  // 1, 2, 4, 8 and the machine maximum (deduplicated)
  EXPECT_EQ(r.threads.front(), 1);
}

}  // namespace

TEST(Integration, T1IsValidatedAcrossOrdersThreadsSeedsAndSizes) {
  ValidationReport r = Validate("examples/t1_vector_add.plx");
  ExpectFullCoverage(r);
}

TEST(Integration, T3ReductionIsValidatedWithinTolerance) {
  ValidationReport r = Validate("examples/t3_reduce_sum.plx");
  ExpectFullCoverage(r);
}

TEST(Integration, T4StencilIsValidatedAcrossOrdersThreadsSeedsAndSizes) {
  ValidationReport r = Validate("examples/t4_stencil5.plx");
  ExpectFullCoverage(r);
}

TEST(Integration, CertificateTextStatesExactlyWhatWasCovered) {
  Analysis a = AnalyzeExample("examples/t1_vector_add.plx");
  ValidationOptions opts;
  opts.tsan = false;
  opts.work_dir = ScratchDir();
  ValidationReport r = RunValidation(a, opts);
  std::string text = FormatValidation(a, r);
  EXPECT_NE(text.find("CERTIFICATE: vector_add STATICALLY SAFE, EMPIRICALLY CORROBORATED"),
            std::string::npos) << text;
  EXPECT_NE(text.find("36 runs, 0 mismatches"), std::string::npos) << text;
  EXPECT_NE(text.find("3 sizes (37,101,257)"), std::string::npos) << text;
  EXPECT_NE(text.find("not a proof for all inputs"), std::string::npos) << text;
  EXPECT_NE(text.find("ThreadSanitizer -- not requested"), std::string::npos) << text;
}

TEST(Integration, UnsafeKernelIsNotValidated) {
  Analysis a = AnalyzeExample("examples/t2_prefix_like.plx");
  ValidationOptions opts;
  opts.work_dir = ScratchDir();
  ValidationReport r = RunValidation(a, opts);
  EXPECT_FALSE(r.built);
  EXPECT_NE(r.error.find("UNSAFE"), std::string::npos);
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

namespace {
const char* kFib = "kernel fib(dp: i32[N]) -> dp: i32[N] { auto i in 2..N { dp[i] = dp[i-1] + dp[i-2] } }";

Analysis AnalyzeText(const std::string& text) {
  std::string path = ScratchDir() + "/inline_kernel.plx";
  { std::ofstream(path) << text; }
  return AnalyzeExample(path);
}

// Forces a parallel variant onto a kernel the analysis marked UNSAFE.
Analysis Forced(Analysis a) {
  a.schedule.kind = ScheduleKind::PARALLEL_OUTER;
  a.schedule.parallel_loop_index = 0;
  a.schedule.reason = "forced for testing";
  a.gen = GenerateWith(a, a.schedule);
  return a;
}
}  // namespace

TEST(Integration, ValidatorCatchesAForcedParallelUnsafeKernel) {
  Analysis a = AnalyzeText(kFib);
  ASSERT_EQ(a.report.verdict, Verdict::UNSAFE);
  Analysis forced = Forced(std::move(a));
  ValidationOptions opts;
  opts.tsan = false;
  opts.work_dir = ScratchDir();
  ValidationReport r = RunValidation(forced, opts);
  ASSERT_TRUE(r.built) << r.error;
  EXPECT_GT(r.order_mismatches, 0) << "reversed/permuted orders must expose the recurrence";
  EXPECT_FALSE(r.Passed());
  EXPECT_NE(FormatValidation(forced, r).find("CERTIFICATE MISMATCH"), std::string::npos);
}

TEST(Integration, ThreadSanitizerStepIsCleanOrSkippedWithAReasonNeverSilentlyAbsent) {
  ValidationReport r = Validate("examples/t1_vector_add.plx", /*tsan=*/true);
  ASSERT_TRUE(r.built) << r.error;
  EXPECT_TRUE(r.tsan_status == "clean" || r.tsan_status == "skipped") << r.tsan_status;
  EXPECT_FALSE(r.tsan_detail.empty());
}

TEST(Integration, GeneratedHarnessesCompileWithoutWarnings) {
  const char* kernels[] = {"examples/t1_vector_add.plx", "examples/t2_prefix_like.plx",
                           "examples/t3_reduce_sum.plx", "examples/t4_stencil5.plx",
                           "examples/count_pairs.plx", "examples/climbing_stairs.plx"};
  for (const char* path : kernels) {
    Analysis a = AnalyzeExample(path);
    BuiltHarness h = BuildHarness(a, a.gen, a.schedule, ScratchDir(), "warn_check", "-Wall -Wextra -Werror");
    EXPECT_TRUE(h.compile.ok) << path << ":\n" << h.compile.output;
  }
}

TEST(Integration, InPlaceElementwiseKernelIsValidatedNotJustAssumedSafe) {
  Analysis a = AnalyzeText("kernel bump(A: f32[N], B: f32[N]) -> B: f32[N] { auto i in 0..N { B[i] = B[i] + A[i] } }");
  ASSERT_EQ(a.report.verdict, Verdict::SAFE);
  ValidationOptions opts;
  opts.tsan = false;
  opts.work_dir = ScratchDir();
  ValidationReport r = RunValidation(a, opts);
  ASSERT_TRUE(r.built) << r.error;
  EXPECT_TRUE(r.Passed()) << FormatValidation(a, r);
}

TEST(Integration, MissingOpenMpIsDisclosedInTheCertificate) {
  Analysis a = AnalyzeExample("examples/t1_vector_add.plx");
  ValidationReport r;
  r.built = true;
  r.openmp = false;
  r.has_par = true;
  r.sizes = {37};
  r.seeds = {1};
  r.threads = {1, 2};
  r.reps = 20;
  r.perms = 3;
  std::string text = FormatValidation(a, r);
  EXPECT_NE(text.find("WARNING: built without OpenMP"), std::string::npos) << text;
}
