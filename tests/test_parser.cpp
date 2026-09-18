#include "parallix/parser.h"

#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

#include "parallix/lexer.h"

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

TEST(Parser, T1VectorAddShape) {
  Program program = ParseFile("examples/t1_vector_add.plx");
  ASSERT_EQ(program.kernels.size(), 1u);
  const KernelDecl& k = program.kernels[0];
  EXPECT_EQ(k.name, "vector_add");
  ASSERT_EQ(k.params.size(), 2u);
  EXPECT_EQ(k.params[0].name, "A");
  EXPECT_EQ(k.params[0].type.base, TypeBase::F32);
  ASSERT_EQ(k.params[0].type.dims.size(), 1u);
  EXPECT_TRUE(k.params[0].type.dims[0].is_ident);
  EXPECT_EQ(k.params[0].type.dims[0].ident, "N");
  EXPECT_EQ(k.params[1].name, "B");
  EXPECT_EQ(k.ret.name, "C");
  EXPECT_EQ(k.ret.type.base, TypeBase::F32);

  ASSERT_EQ(k.body.size(), 1u);
  ASSERT_EQ(k.body[0]->kind, StmtKind::LOOP);
  const auto& loop = static_cast<const LoopStmt&>(*k.body[0]);
  EXPECT_EQ(loop.mode, LoopMode::AUTO);
  EXPECT_EQ(loop.index_var, "i");
  ASSERT_EQ(loop.body.size(), 1u);
  ASSERT_EQ(loop.body[0]->kind, StmtKind::ASSIGN);
  const auto& assign = static_cast<const AssignStmt&>(*loop.body[0]);
  EXPECT_EQ(assign.lvalue->name, "C");
  ASSERT_EQ(assign.lvalue->subscripts.size(), 1u);
  EXPECT_EQ(assign.op, AssignOp::SET);
  ASSERT_EQ(assign.rhs->kind, ExprKind::BINARY);
  const auto& rhs = static_cast<const BinaryExpr&>(*assign.rhs);
  EXPECT_EQ(rhs.op, BinaryOp::ADD);
}

TEST(Parser, T2PrefixLikeShape) {
  Program program = ParseFile("examples/t2_prefix_like.plx");
  ASSERT_EQ(program.kernels.size(), 1u);
  const KernelDecl& k = program.kernels[0];
  EXPECT_EQ(k.name, "prefix_like");
  ASSERT_EQ(k.params.size(), 2u);
  EXPECT_EQ(k.ret.name, "A");

  ASSERT_EQ(k.body.size(), 1u);
  const auto& loop = static_cast<const LoopStmt&>(*k.body[0]);
  EXPECT_EQ(loop.mode, LoopMode::AUTO);
  ASSERT_EQ(loop.body.size(), 1u);
  const auto& assign = static_cast<const AssignStmt&>(*loop.body[0]);
  EXPECT_EQ(assign.lvalue->name, "A");
  ASSERT_EQ(assign.rhs->kind, ExprKind::BINARY);
  const auto& rhs = static_cast<const BinaryExpr&>(*assign.rhs);
  ASSERT_EQ(rhs.lhs->kind, ExprKind::VAR_REF);
  const auto& read_a = static_cast<const VarRefExpr&>(*rhs.lhs);
  EXPECT_EQ(read_a.name, "A");
  ASSERT_EQ(read_a.subscripts.size(), 1u);
  ASSERT_EQ(read_a.subscripts[0]->kind, ExprKind::BINARY);
  const auto& sub = static_cast<const BinaryExpr&>(*read_a.subscripts[0]);
  EXPECT_EQ(sub.op, BinaryOp::SUB);
}

TEST(Parser, T3ReduceSumShape) {
  Program program = ParseFile("examples/t3_reduce_sum.plx");
  ASSERT_EQ(program.kernels.size(), 1u);
  const KernelDecl& k = program.kernels[0];
  EXPECT_EQ(k.name, "reduce_sum");
  ASSERT_EQ(k.params.size(), 1u);
  EXPECT_EQ(k.ret.name, "s");
  EXPECT_TRUE(k.ret.type.IsScalar());

  ASSERT_EQ(k.body.size(), 1u);
  ASSERT_EQ(k.body[0]->kind, StmtKind::REDUCTION);
  const auto& red = static_cast<const ReductionStmt&>(*k.body[0]);
  EXPECT_EQ(red.op, ReduceOp::ADD);
  EXPECT_EQ(red.index_var, "i");
  ASSERT_EQ(red.body.size(), 1u);
  const auto& assign = static_cast<const AssignStmt&>(*red.body[0]);
  EXPECT_EQ(assign.lvalue->name, "s");
  EXPECT_TRUE(assign.lvalue->IsScalar());
  EXPECT_EQ(assign.op, AssignOp::ADD_SET);
}

TEST(Parser, T4Stencil5Shape) {
  Program program = ParseFile("examples/t4_stencil5.plx");
  ASSERT_EQ(program.kernels.size(), 1u);
  const KernelDecl& k = program.kernels[0];
  EXPECT_EQ(k.name, "stencil5");
  ASSERT_EQ(k.params.size(), 1u);
  EXPECT_EQ(k.params[0].type.dims.size(), 2u);
  EXPECT_EQ(k.ret.name, "B");

  ASSERT_EQ(k.body.size(), 1u);
  ASSERT_EQ(k.body[0]->kind, StmtKind::LOOP);
  const auto& outer = static_cast<const LoopStmt&>(*k.body[0]);
  EXPECT_EQ(outer.mode, LoopMode::PARALLEL);
  EXPECT_EQ(outer.index_var, "i");
  ASSERT_EQ(outer.body.size(), 1u);
  ASSERT_EQ(outer.body[0]->kind, StmtKind::LOOP);
  const auto& inner = static_cast<const LoopStmt&>(*outer.body[0]);
  EXPECT_EQ(inner.mode, LoopMode::VECTORIZE);
  EXPECT_EQ(inner.index_var, "j");
  ASSERT_EQ(inner.body.size(), 1u);
  const auto& assign = static_cast<const AssignStmt&>(*inner.body[0]);
  EXPECT_EQ(assign.lvalue->name, "B");
  ASSERT_EQ(assign.lvalue->subscripts.size(), 2u);
}

// T5 and T6 are syntactically well-formed — only semantic analysis (M3)
// rejects them. This distinction matters (specs/02) and is tested
// explicitly here: parsing must succeed for both.
TEST(Parser, T5NonAffineParsesSuccessfully) {
  EXPECT_NO_THROW({
    Program program = ParseFile("examples/t5_non_affine.plx");
    ASSERT_EQ(program.kernels.size(), 1u);
    EXPECT_EQ(program.kernels[0].name, "non_affine");
  });
}

TEST(Parser, T6UndeclaredParsesSuccessfully) {
  EXPECT_NO_THROW({
    Program program = ParseFile("examples/t6_undeclared.plx");
    ASSERT_EQ(program.kernels.size(), 1u);
    EXPECT_EQ(program.kernels[0].name, "undeclared_ref");
  });
}

TEST(Parser, RejectsGarbageInput) {
  Lexer lexer("kernel foo(A: f32[N]) -> B: f32[N] { 1 + }");
  Parser parser(lexer.Tokenize());
  EXPECT_THROW(parser.ParseProgram(), ParseError);
}
