#include "parallix/sema.h"

#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

#include "parallix/lexer.h"
#include "parallix/parser.h"

using namespace parallix;

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

Program ParseFile(const std::string& path) {
  std::string src = ReadFile(path);
  Lexer lexer(src);
  Parser parser(lexer.Tokenize());
  return parser.ParseProgram();
}

}  // namespace

TEST(Sema, T1VectorAddPassesCleanly) {
  Program program = ParseFile("examples/t1_vector_add.plx");
  Sema sema;
  EXPECT_NO_THROW(sema.AnalyzeKernel(program.kernels[0]));
}

TEST(Sema, T2PrefixLikePassesCleanly) {
  Program program = ParseFile("examples/t2_prefix_like.plx");
  Sema sema;
  EXPECT_NO_THROW(sema.AnalyzeKernel(program.kernels[0]));
}

TEST(Sema, T3ReduceSumPassesCleanly) {
  Program program = ParseFile("examples/t3_reduce_sum.plx");
  Sema sema;
  EXPECT_NO_THROW(sema.AnalyzeKernel(program.kernels[0]));
}

TEST(Sema, T4Stencil5PassesCleanly) {
  Program program = ParseFile("examples/t4_stencil5.plx");
  Sema sema;
  EXPECT_NO_THROW(sema.AnalyzeKernel(program.kernels[0]));
}

TEST(Sema, T5NonAffineRejectedNamingSubscript) {
  Program program = ParseFile("examples/t5_non_affine.plx");
  Sema sema;
  try {
    sema.AnalyzeKernel(program.kernels[0]);
    FAIL() << "expected SemaError for non-affine subscript";
  } catch (const SemaError& e) {
    std::string msg = e.what();
    EXPECT_NE(msg.find("i * i"), std::string::npos) << msg;
    EXPECT_NE(msg.find("non-affine"), std::string::npos) << msg;
  }
}

TEST(Sema, T6UndeclaredRejectedNamingIdentifier) {
  Program program = ParseFile("examples/t6_undeclared.plx");
  Sema sema;
  try {
    sema.AnalyzeKernel(program.kernels[0]);
    FAIL() << "expected SemaError for undeclared identifier";
  } catch (const SemaError& e) {
    std::string msg = e.what();
    EXPECT_NE(msg.find("'Z'"), std::string::npos) << msg;
    EXPECT_NE(msg.find("undeclared"), std::string::npos) << msg;
  }
}

TEST(Sema, ShapeMismatchRejected) {
  std::string src =
      "kernel bad_shape(A: f32[N]) -> B: f32[N] {\n"
      "    auto i in 0..N {\n"
      "        B[i] = A[i, i]\n"
      "    }\n"
      "}\n";
  Lexer lexer(src);
  Parser parser(lexer.Tokenize());
  Program program = parser.ParseProgram();
  Sema sema;
  EXPECT_THROW(sema.AnalyzeKernel(program.kernels[0]), SemaError);
}

TEST(Sema, ReductionOperatorMismatchRejected) {
  std::string src =
      "kernel bad_reduce(A: f32[N]) -> s: f32 {\n"
      "    reduce(+) i in 0..N {\n"
      "        s = A[i] * s\n"
      "    }\n"
      "}\n";
  Lexer lexer(src);
  Parser parser(lexer.Tokenize());
  Program program = parser.ParseProgram();
  Sema sema;
  EXPECT_THROW(sema.AnalyzeKernel(program.kernels[0]), SemaError);
}
