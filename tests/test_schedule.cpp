#include "parallix/schedule.h"

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

TEST(Schedule, SafeVerdictSchedulesParallelOuter) {
  PIRKernel pir = LowerFile("examples/t1_vector_add.plx");
  DependenceReport report = AnalyzeDependence(pir);
  ASSERT_EQ(report.verdict, Verdict::SAFE);

  Schedule schedule = GenerateSchedule(pir, report);
  EXPECT_EQ(schedule.kind, ScheduleKind::PARALLEL_OUTER);
  EXPECT_EQ(schedule.parallel_loop_index, 0u);
  EXPECT_FALSE(schedule.reason.empty());
}

TEST(Schedule, UnsafeVerdictSchedulesSequentialOnly) {
  PIRKernel pir = LowerFile("examples/t2_prefix_like.plx");
  DependenceReport report = AnalyzeDependence(pir);
  ASSERT_EQ(report.verdict, Verdict::UNSAFE);

  Schedule schedule = GenerateSchedule(pir, report);
  EXPECT_EQ(schedule.kind, ScheduleKind::SEQUENTIAL_ONLY);
  EXPECT_FALSE(schedule.reason.empty());
}

TEST(Schedule, ReductionVerdictSchedulesParallelReductionOnAccumulator) {
  PIRKernel pir = LowerFile("examples/t3_reduce_sum.plx");
  DependenceReport report = AnalyzeDependence(pir);
  ASSERT_EQ(report.verdict, Verdict::SAFE_AS_REDUCTION);

  Schedule schedule = GenerateSchedule(pir, report);
  EXPECT_EQ(schedule.kind, ScheduleKind::PARALLEL_REDUCTION);
  EXPECT_EQ(schedule.parallel_loop_index, 0u);
  // The reason should name the actual accumulator and op so a human reading
  // SCHEDULE: output can see what will end up in the reduction() clause.
  EXPECT_NE(schedule.reason.find(pir.reduce_accumulator), std::string::npos);
  EXPECT_NE(schedule.reason.find(ToString(pir.reduce_op)), std::string::npos);
}

TEST(Schedule, MultiLoopSafeKernelSchedulesOutermostLevel) {
  PIRKernel pir = LowerFile("examples/t4_stencil5.plx");
  DependenceReport report = AnalyzeDependence(pir);
  ASSERT_EQ(report.verdict, Verdict::SAFE);
  ASSERT_GE(pir.loop_nest.size(), 2u);

  Schedule schedule = GenerateSchedule(pir, report);
  EXPECT_EQ(schedule.kind, ScheduleKind::PARALLEL_OUTER);
  EXPECT_EQ(schedule.parallel_loop_index, 0u);
  EXPECT_EQ(pir.loop_nest[schedule.parallel_loop_index].index_var, "i");
}
