#include "parallix/codegen.h"

#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

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

struct Compiled {
  KernelDecl kernel;
  PIRKernel pir;
  DependenceReport report;
  Schedule schedule;
  CodegenResult gen;
};

Compiled CompileFile(const std::string& path) {
  std::string src = ReadFile(path);
  Lexer lexer(src);
  Parser parser(lexer.Tokenize());
  Program program = parser.ParseProgram();
  Compiled c;
  c.kernel = std::move(program.kernels[0]);
  Sema sema;
  sema.AnalyzeKernel(c.kernel);
  c.pir = LowerKernel(c.kernel);
  c.report = AnalyzeDependence(c.pir);
  c.schedule = GenerateSchedule(c.pir, c.report);
  c.gen = GenerateCCode(c.kernel, c.pir, c.schedule);
  return c;
}

}  // namespace

TEST(Codegen, T1VectorAddSignatureAndPragma) {
  Compiled c = CompileFile("examples/t1_vector_add.plx");
  EXPECT_EQ(c.gen.seq_function_name, "vector_add_seq");
  EXPECT_EQ(c.gen.par_function_name, "vector_add_par");

  EXPECT_NE(c.gen.c_source.find("void vector_add_seq(const float* A, const float* B, "
                                 "float* C, int N)"),
            std::string::npos);
  EXPECT_NE(c.gen.c_source.find("void vector_add_par(const float* A, const float* B, "
                                 "float* C, int N)"),
            std::string::npos);
  // Sequential function must not contain a pragma; the parallel one must.
  size_t seq_pos = c.gen.c_source.find("vector_add_seq");
  size_t par_pos = c.gen.c_source.find("vector_add_par");
  ASSERT_NE(seq_pos, std::string::npos);
  ASSERT_NE(par_pos, std::string::npos);
  std::string seq_body = c.gen.c_source.substr(seq_pos, par_pos - seq_pos);
  EXPECT_EQ(seq_body.find("#pragma omp"), std::string::npos);
  EXPECT_NE(c.gen.c_source.find("#pragma omp parallel for"), std::string::npos);
  // Row-major/flat indexing for a 1-D array is just the bare index.
  EXPECT_NE(c.gen.c_source.find("C[i] = A[i] + B[i];"), std::string::npos);
}

TEST(Codegen, T2PrefixLikeOnlySequentialInPlaceNotDuplicated) {
  Compiled c = CompileFile("examples/t2_prefix_like.plx");
  ASSERT_EQ(c.schedule.kind, ScheduleKind::SEQUENTIAL_ONLY);
  EXPECT_EQ(c.gen.seq_function_name, "prefix_like_seq");
  EXPECT_TRUE(c.gen.par_function_name.empty());
  EXPECT_EQ(c.gen.c_source.find("#pragma omp"), std::string::npos);

  // A is both input and (in-place) output: exactly one non-const A*, never
  // duplicated as a second parameter.
  EXPECT_NE(c.gen.c_source.find("void prefix_like_seq(float* A, const float* B, int N)"),
            std::string::npos);
  size_t first_A = c.gen.c_source.find("A,");
  size_t second_A = c.gen.c_source.find("A,", first_A + 1);
  // Only one "A," should appear inside the signature parameter list.
  size_t sig_end = c.gen.c_source.find(")");
  EXPECT_TRUE(second_A == std::string::npos || second_A > sig_end);
}

TEST(Codegen, T3ReduceSumHasReductionClause) {
  Compiled c = CompileFile("examples/t3_reduce_sum.plx");
  ASSERT_EQ(c.schedule.kind, ScheduleKind::PARALLEL_REDUCTION);
  EXPECT_EQ(c.gen.seq_function_name, "reduce_sum_seq");
  EXPECT_EQ(c.gen.par_function_name, "reduce_sum_par");
  EXPECT_NE(c.gen.c_source.find("float reduce_sum_seq(const float* A, int N)"), std::string::npos);
  EXPECT_NE(c.gen.c_source.find("#pragma omp parallel for reduction(+:s)"), std::string::npos);
  EXPECT_NE(c.gen.c_source.find("return s;"), std::string::npos);
}

TEST(Codegen, T4Stencil5RowMajorAndOuterLoopOnlyPragma) {
  Compiled c = CompileFile("examples/t4_stencil5.plx");
  ASSERT_EQ(c.schedule.kind, ScheduleKind::PARALLEL_OUTER);
  EXPECT_NE(c.gen.c_source.find("void stencil5_seq(const float* A, float* B, int N, int M)"),
            std::string::npos);
  EXPECT_NE(c.gen.c_source.find("void stencil5_par(const float* A, float* B, int N, int M)"),
            std::string::npos);

  // Only one pragma total: the outer `i` loop, not the inner `j` loop.
  size_t par_pos = c.gen.c_source.find("stencil5_par");
  std::string par_body = c.gen.c_source.substr(par_pos);
  size_t first_pragma = par_body.find("#pragma omp parallel for");
  ASSERT_NE(first_pragma, std::string::npos);
  size_t for_i = par_body.find("for (int i");
  size_t for_j = par_body.find("for (int j");
  ASSERT_NE(for_i, std::string::npos);
  ASSERT_NE(for_j, std::string::npos);
  EXPECT_LT(first_pragma, for_i);
  EXPECT_LT(for_i, for_j);
  size_t second_pragma = par_body.find("#pragma omp", first_pragma + 1);
  EXPECT_EQ(second_pragma, std::string::npos);

  // Row-major flattening: B[i, j] -> B[i * M + j] (or equivalent parenthesization).
  EXPECT_NE(c.gen.c_source.find("* M"), std::string::npos);
}
