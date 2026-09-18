#include "parallix/lowering.h"

#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

#include "parallix/lexer.h"
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

void ExpectAccess(const AccessFunction& access, const std::string& index_var,
                   long long coefficient, long long constant) {
  EXPECT_EQ(access.index_var, index_var);
  EXPECT_EQ(access.coefficient, coefficient);
  EXPECT_EQ(access.constant, constant);
}

}  // namespace

TEST(Lowering, T1VectorAdd) {
  PIRKernel pir = LowerFile("examples/t1_vector_add.plx");

  ASSERT_EQ(pir.loop_nest.size(), 1u);
  EXPECT_EQ(pir.loop_nest[0].index_var, "i");
  EXPECT_EQ(pir.loop_nest[0].mode, LoopMode::AUTO);
  EXPECT_EQ(pir.loop_nest[0].lower.constant, 0);
  EXPECT_TRUE(pir.loop_nest[0].lower.terms.empty());
  ASSERT_EQ(pir.loop_nest[0].upper.terms.size(), 1u);
  EXPECT_EQ(pir.loop_nest[0].upper.terms[0].first, "N");
  EXPECT_EQ(pir.loop_nest[0].upper.terms[0].second, 1);

  ASSERT_EQ(pir.references.size(), 3u);
  EXPECT_EQ(pir.references[0].array_name, "C");
  EXPECT_EQ(pir.references[0].kind, RefKind::WRITE);
  ASSERT_EQ(pir.references[0].access.size(), 1u);
  ExpectAccess(pir.references[0].access[0], "i", 1, 0);

  EXPECT_EQ(pir.references[1].array_name, "A");
  EXPECT_EQ(pir.references[1].kind, RefKind::READ);
  ExpectAccess(pir.references[1].access[0], "i", 1, 0);

  EXPECT_EQ(pir.references[2].array_name, "B");
  EXPECT_EQ(pir.references[2].kind, RefKind::READ);
  ExpectAccess(pir.references[2].access[0], "i", 1, 0);

  EXPECT_FALSE(pir.is_reduction);
}

TEST(Lowering, T2PrefixLikeReadIsShiftedByMinusOne) {
  PIRKernel pir = LowerFile("examples/t2_prefix_like.plx");

  ASSERT_EQ(pir.loop_nest.size(), 1u);
  EXPECT_EQ(pir.loop_nest[0].index_var, "i");
  EXPECT_EQ(pir.loop_nest[0].lower.constant, 1);

  ASSERT_EQ(pir.references.size(), 3u);
  EXPECT_EQ(pir.references[0].array_name, "A");
  EXPECT_EQ(pir.references[0].kind, RefKind::WRITE);
  ExpectAccess(pir.references[0].access[0], "i", 1, 0);  // A[i]

  EXPECT_EQ(pir.references[1].array_name, "A");
  EXPECT_EQ(pir.references[1].kind, RefKind::READ);
  ExpectAccess(pir.references[1].access[0], "i", 1, -1);  // A[i-1] -> (a=1, b=-1)

  EXPECT_EQ(pir.references[2].array_name, "B");
  ExpectAccess(pir.references[2].access[0], "i", 1, 0);  // B[i]
}

TEST(Lowering, T3ReduceSumIsMarkedAsReduction) {
  PIRKernel pir = LowerFile("examples/t3_reduce_sum.plx");

  ASSERT_EQ(pir.loop_nest.size(), 1u);
  EXPECT_EQ(pir.loop_nest[0].index_var, "i");
  EXPECT_EQ(pir.loop_nest[0].mode, LoopMode::SEQ);

  EXPECT_TRUE(pir.is_reduction);
  EXPECT_EQ(pir.reduce_op, ReduceOp::ADD);
  EXPECT_EQ(pir.reduce_accumulator, "s");

  ASSERT_EQ(pir.references.size(), 1u);
  EXPECT_EQ(pir.references[0].array_name, "A");
  EXPECT_EQ(pir.references[0].kind, RefKind::READ);
  ExpectAccess(pir.references[0].access[0], "i", 1, 0);
}

TEST(Lowering, T4Stencil5TwoDAccessFunctions) {
  PIRKernel pir = LowerFile("examples/t4_stencil5.plx");

  ASSERT_EQ(pir.loop_nest.size(), 2u);
  EXPECT_EQ(pir.loop_nest[0].index_var, "i");
  EXPECT_EQ(pir.loop_nest[0].mode, LoopMode::PARALLEL);
  EXPECT_EQ(pir.loop_nest[1].index_var, "j");
  EXPECT_EQ(pir.loop_nest[1].mode, LoopMode::VECTORIZE);
  // upper bound N-1 -> AffineExpr{terms: {N: 1}, constant: -1}
  ASSERT_EQ(pir.loop_nest[0].upper.terms.size(), 1u);
  EXPECT_EQ(pir.loop_nest[0].upper.terms[0].first, "N");
  EXPECT_EQ(pir.loop_nest[0].upper.terms[0].second, 1);
  EXPECT_EQ(pir.loop_nest[0].upper.constant, -1);

  ASSERT_EQ(pir.references.size(), 6u);
  EXPECT_EQ(pir.references[0].array_name, "B");
  EXPECT_EQ(pir.references[0].kind, RefKind::WRITE);
  ASSERT_EQ(pir.references[0].access.size(), 2u);
  ExpectAccess(pir.references[0].access[0], "i", 1, 0);
  ExpectAccess(pir.references[0].access[1], "j", 1, 0);

  // A[i-1,j], A[i+1,j], A[i,j-1], A[i,j+1], A[i,j], in source order.
  ASSERT_EQ(pir.references[1].array_name, "A");
  ExpectAccess(pir.references[1].access[0], "i", 1, -1);
  ExpectAccess(pir.references[1].access[1], "j", 1, 0);

  ExpectAccess(pir.references[2].access[0], "i", 1, 1);
  ExpectAccess(pir.references[2].access[1], "j", 1, 0);

  ExpectAccess(pir.references[3].access[0], "i", 1, 0);
  ExpectAccess(pir.references[3].access[1], "j", 1, -1);

  ExpectAccess(pir.references[4].access[0], "i", 1, 0);
  ExpectAccess(pir.references[4].access[1], "j", 1, 1);

  ExpectAccess(pir.references[5].access[0], "i", 1, 0);
  ExpectAccess(pir.references[5].access[1], "j", 1, 0);
}
