// Comparisons, select, min/max, i64, integer and min/max reductions: lexing,
// parsing, named semantic errors, code generation, and real compile-and-run.

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "parallix/codegen.h"
#include "parallix/compiler_driver.h"
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

Program ParseSource(const std::string& src) {
  Lexer lexer(src);
  Parser parser(lexer.Tokenize());
  return parser.ParseProgram();
}

// Returns the SemaError message, or "" if the kernel is accepted.
std::string SemaMessage(const std::string& src) {
  Program program = ParseSource(src);
  Sema sema;
  try {
    sema.AnalyzeKernel(program.kernels[0]);
  } catch (const SemaError& e) {
    return e.what();
  }
  return "";
}

struct Built {
  KernelDecl kernel;
  PIRKernel pir;
  DependenceReport report;
  Schedule schedule;
  CodegenResult gen;
};

Built Build(const std::string& src) {
  Program program = ParseSource(src);
  Built b;
  b.kernel = std::move(program.kernels[0]);
  Sema sema;
  sema.AnalyzeKernel(b.kernel);
  b.pir = LowerKernel(b.kernel);
  b.report = AnalyzeDependence(b.pir);
  b.schedule = GenerateSchedule(b.pir, b.report);
  b.gen = GenerateCCode(b.kernel, b.pir, b.schedule);
  return b;
}

// Compiles gen.c_source together with `main_body` and returns the program's output.
std::string CompileAndRunWith(const Built& b, const std::string& tag,
                              const std::string& main_body) {
  std::filesystem::path dir = std::filesystem::path("build") / "test_generated" / UniqueTestDir();
  std::filesystem::create_directories(dir);
  std::string c_path = (dir / (tag + ".c")).string();
  std::string bin_path = (dir / (tag + "_bin")).string();
  {
    std::ofstream out(c_path);
    out << "#include <stdio.h>\n#include <stdlib.h>\n" << b.gen.c_source << "\nint main(void) {\n"
        << main_body << "\n}\n";
  }
  CompileRunResult r = CompileAndRun(c_path, bin_path);
  EXPECT_TRUE(r.compiled) << r.compile_output << "\n" << b.gen.c_source;
  EXPECT_EQ(r.exit_code, 0) << r.run_output;
  return r.run_output;
}

const char* kPairs = R"(
kernel count_pairs(a: i32[N], target: i32) -> count: i64 {
    reduce(+) i in 0..N {
        auto j in i+1..N {
            count += select(a[i] + a[j] == target, 1, 0)
        }
    }
}
)";

}  // namespace

// ---- lexer -----------------------------------------------------------------

TEST(DslLexer, ComparisonTokens) {
  Lexer lexer("== != < <= > >= = select i64");
  std::vector<Token> toks = lexer.Tokenize();
  std::vector<TokenKind> want = {TokenKind::EQ,     TokenKind::NE,     TokenKind::LT,
                                 TokenKind::LE,     TokenKind::GT,     TokenKind::GE,
                                 TokenKind::ASSIGN, TokenKind::SELECT, TokenKind::I64,
                                 TokenKind::END_OF_FILE};
  ASSERT_EQ(toks.size(), want.size());
  for (size_t i = 0; i < want.size(); ++i) EXPECT_EQ(toks[i].kind, want[i]) << i;
}

TEST(DslLexer, LoneBangIsAnErrorThatSuggestsNotEqual) {
  Lexer lexer("a ! b");
  try {
    lexer.Tokenize();
    FAIL() << "expected LexError";
  } catch (const LexError& e) {
    EXPECT_NE(std::string(e.what()).find("!="), std::string::npos);
  }
}

// ---- parser ----------------------------------------------------------------

TEST(DslParser, ComparisonAndSelectParse) {
  EXPECT_NO_THROW(ParseSource(kPairs));
}

TEST(DslParser, ComparisonBindsLooserThanArithmetic) {
  Program p = ParseSource(
      "kernel k(A: i32[N]) -> B: i32[N] { auto i in 0..N { B[i] = A[i] + 1 < A[i] * 2 } }");
  const auto& loop = static_cast<const LoopStmt&>(*p.kernels[0].body[0]);
  const auto& assign = static_cast<const AssignStmt&>(*loop.body[0]);
  ASSERT_EQ(assign.rhs->kind, ExprKind::BINARY);
  EXPECT_EQ(static_cast<const BinaryExpr&>(*assign.rhs).op, BinaryOp::LT);
}

TEST(DslParser, ChainedComparisonIsRejectedByName) {
  try {
    ParseSource("kernel k(A: i32[N]) -> B: i32[N] { auto i in 0..N { B[i] = A[i] < 1 < 2 } }");
    FAIL() << "expected ParseError";
  } catch (const ParseError& e) {
    EXPECT_NE(std::string(e.what()).find("E_CHAINED_COMPARISON"), std::string::npos) << e.what();
  }
}

TEST(DslParser, WrongArityIsRejectedByName) {
  try {
    ParseSource("kernel k(A: i32[N]) -> B: i32[N] { auto i in 0..N { B[i] = select(A[i], 1) } }");
    FAIL() << "expected ParseError";
  } catch (const ParseError& e) {
    EXPECT_NE(std::string(e.what()).find("E_CALL_ARITY"), std::string::npos) << e.what();
  }
}

// ---- semantic analysis -----------------------------------------------------

TEST(DslSema, PairCountKernelIsAccepted) { EXPECT_EQ(SemaMessage(kPairs), ""); }

TEST(DslSema, ComparisonInSubscriptIsRejectedByName) {
  std::string m = SemaMessage(
      "kernel k(A: i32[N]) -> B: i32[N] { auto i in 0..N { B[i] = A[(i < 3)] } }");
  EXPECT_NE(m.find("E_COMPARISON_IN_SUBSCRIPT"), std::string::npos) << m;
  EXPECT_NE(m.find("i < 3"), std::string::npos) << m;
}

TEST(DslSema, SelectInSubscriptIsRejectedByName) {
  std::string m = SemaMessage(
      "kernel k(A: i32[N]) -> B: i32[N] { auto i in 0..N { B[i] = A[select(i < 3, i, 0)] } }");
  EXPECT_NE(m.find("E_SELECT_IN_SUBSCRIPT"), std::string::npos) << m;
}

TEST(DslSema, MaxInSubscriptIsRejectedByName) {
  std::string m =
      SemaMessage("kernel k(A: i32[N]) -> B: i32[N] { auto i in 0..N { B[i] = A[max(i, 1)] } }");
  EXPECT_NE(m.find("E_MAX_IN_SUBSCRIPT"), std::string::npos) << m;
}

TEST(DslSema, ComparisonInLoopBoundIsRejectedByName) {
  std::string m = SemaMessage(
      "kernel k(A: i32[N]) -> B: i32[N] { auto i in 0..(N < 5) { B[i] = A[i] } }");
  EXPECT_NE(m.find("E_COMPARISON_IN_BOUND"), std::string::npos) << m;
}

TEST(DslSema, SelectInLoopBoundIsRejectedByName) {
  std::string m = SemaMessage(
      "kernel k(A: i32[N]) -> B: i32[N] { auto i in 0..select(N < 5, N, 5) { B[i] = A[i] } }");
  EXPECT_NE(m.find("E_SELECT_IN_BOUND"), std::string::npos) << m;
}

TEST(DslSema, SelectConditionMustBeAComparison) {
  std::string m = SemaMessage(
      "kernel k(A: i32[N]) -> B: i32[N] { auto i in 0..N { B[i] = select(A[i], 1, 0) } }");
  EXPECT_NE(m.find("E_SELECT_CONDITION"), std::string::npos) << m;
}

TEST(DslSema, ComparisonAsAValueIsAllowed) {
  EXPECT_EQ(SemaMessage("kernel k(A: i32[N]) -> B: i32[N] { auto i in 0..N { B[i] = A[i] > 3 } }"),
            "");
}

TEST(DslSema, MaxReductionAcceptsBothArgumentOrders) {
  EXPECT_EQ(SemaMessage("kernel k(A: f32[N]) -> m: f32 { reduce(max) i in 0..N { m = max(m, A[i]) } }"),
            "");
  EXPECT_EQ(SemaMessage("kernel k(A: f32[N]) -> m: f32 { reduce(max) i in 0..N { m = max(A[i], m) } }"),
            "");
}

TEST(DslSema, MinReductionWithMaxBodyIsAMismatch) {
  std::string m = SemaMessage(
      "kernel k(A: f32[N]) -> m: f32 { reduce(min) i in 0..N { m = max(m, A[i]) } }");
  EXPECT_NE(m.find("E_REDUCTION_OPERATOR_MISMATCH"), std::string::npos) << m;
}

TEST(DslSema, MaxReductionThatOverwritesIsAMismatch) {
  std::string m =
      SemaMessage("kernel k(A: f32[N]) -> m: f32 { reduce(max) i in 0..N { m = A[i] } }");
  EXPECT_NE(m.find("E_REDUCTION_OPERATOR_MISMATCH"), std::string::npos) << m;
}

TEST(DslSema, ScalarWriteOutsideReductionIsRejected) {
  std::string m =
      SemaMessage("kernel k(A: f32[N]) -> s: f32 { auto i in 0..N { s += A[i] } }");
  EXPECT_NE(m.find("E_SCALAR_WRITE_OUTSIDE_REDUCTION"), std::string::npos) << m;
}

TEST(DslSema, WritingAnInputIsRejected) {
  std::string m =
      SemaMessage("kernel k(A: f32[N]) -> B: f32[N] { auto i in 0..N { A[i] = 1 } }");
  EXPECT_NE(m.find("E_ASSIGN_TARGET"), std::string::npos) << m;
}

TEST(DslSema, NestedReductionIsRejected) {
  std::string m = SemaMessage(
      "kernel k(A: f32[N]) -> s: f32 { auto i in 0..N { reduce(+) j in 0..N { s += A[j] } } }");
  EXPECT_NE(m.find("E_REDUCTION_NOT_OUTERMOST"), std::string::npos) << m;
}

// ---- lowering --------------------------------------------------------------

TEST(DslLowering, ReferencesInsideSelectAndCompareAreCollected) {
  Built b = Build(
      "kernel k(A: i32[N], B: i32[N]) -> C: i32[N] { auto i in 0..N { "
      "C[i] = select(A[i] < B[i], A[i], B[i]) } }");
  int reads = 0, writes = 0;
  for (const Reference& r : b.pir.references) (r.kind == RefKind::READ ? reads : writes)++;
  EXPECT_EQ(writes, 1);
  EXPECT_EQ(reads, 4);
  EXPECT_EQ(b.report.verdict, Verdict::SAFE);
}

TEST(DslLowering, TriangularBoundIsAffineInTheOuterIndex) {
  Built b = Build(kPairs);
  ASSERT_EQ(b.pir.loop_nest.size(), 2u);
  EXPECT_EQ(b.pir.loop_nest[1].lower.ToString(), "i + 1");
  EXPECT_EQ(b.report.verdict, Verdict::SAFE_AS_REDUCTION);
  EXPECT_EQ(b.schedule.kind, ScheduleKind::PARALLEL_REDUCTION);
}

// ---- codegen text ----------------------------------------------------------

TEST(DslCodegen, SelectAndComparisonRenderAsParenthesizedC) {
  Built b = Build(kPairs);
  EXPECT_NE(b.gen.c_source.find("count += ((a[i] + a[j] == target) ? 1 : 0);"), std::string::npos)
      << b.gen.c_source;
  EXPECT_NE(b.gen.c_source.find("long long count_pairs_seq(const int* a, int target, int N)"),
            std::string::npos)
      << b.gen.c_source;
  EXPECT_NE(b.gen.c_source.find("#pragma omp parallel for schedule(dynamic, 4) reduction(+:count)"), std::string::npos);
}

TEST(DslCodegen, ParenthesesFollowPrecedence) {
  Built b = Build(
      "kernel k(A: f32[N], B: f32[N]) -> C: f32[N] { auto i in 0..N { "
      "C[i] = (A[i] + B[i]) * 2 - (A[i] - B[i]) } }");
  EXPECT_NE(b.gen.c_source.find("C[i] = (A[i] + B[i]) * 2 - (A[i] - B[i]);"), std::string::npos)
      << b.gen.c_source;
}

TEST(DslCodegen, MaxReductionUsesOpenMpMaxClause) {
  Built b = Build("kernel k(A: f32[N]) -> m: f32 { reduce(max) i in 0..N { m = max(m, A[i]) } }");
  EXPECT_NE(b.gen.c_source.find("reduction(max:m)"), std::string::npos) << b.gen.c_source;
  EXPECT_NE(b.gen.c_source.find("#define PLX_MAX"), std::string::npos);
  EXPECT_NE(b.gen.c_source.find("m = PLX_MAX(m, A[i]);"), std::string::npos);
}

// ---- compile and run -------------------------------------------------------

TEST(DslIntegration, PairCountMatchesHandComputedAnswer) {
  Built b = Build(kPairs);
  // a = {1,2,3,4,5,6}, target 7: pairs (1,6) (2,5) (3,4) -> 3.
  std::string out = CompileAndRunWith(b, "pairs_known",
      "int a[6] = {1,2,3,4,5,6};\n"
      "long long s = count_pairs_seq(a, 7, 6);\n"
      "long long p = count_pairs_par(a, 7, 6);\n"
      "printf(\"seq=%lld par=%lld\\n\", s, p);\n");
  EXPECT_NE(out.find("seq=3 par=3"), std::string::npos) << out;
}

TEST(DslIntegration, PairCountSeqAndParAgreeOnLargerInput) {
  Built b = Build(kPairs);
  std::string out = CompileAndRunWith(b, "pairs_big",
      "int n = 3001; int* a = malloc(n * sizeof(int));\n"
      "for (int i = 0; i < n; i++) a[i] = (i * 7919) % 211;\n"
      "long long s = count_pairs_seq(a, 100, n);\n"
      "long long p = count_pairs_par(a, 100, n);\n"
      "printf(\"%s %lld\\n\", s == p && s > 0 ? \"EQUAL\" : \"DIFFER\", s);\n");
  EXPECT_NE(out.find("EQUAL"), std::string::npos) << out;
}

TEST(DslIntegration, MaxAndMinReductionsAreExact) {
  Built mx = Build("kernel kmax(A: f32[N]) -> m: f32 { reduce(max) i in 0..N { m = max(m, A[i]) } }");
  std::string out = CompileAndRunWith(mx, "red_max",
      "float a[5] = {-3.5f, 9.25f, 0.0f, 9.0f, -100.0f};\n"
      "printf(\"%.2f %.2f\\n\", kmax_seq(a, 5), kmax_par(a, 5));\n");
  EXPECT_NE(out.find("9.25 9.25"), std::string::npos) << out;

  Built mn = Build("kernel kmin(A: i32[N]) -> m: i32 { reduce(min) i in 0..N { m = min(m, A[i]) } }");
  out = CompileAndRunWith(mn, "red_min",
      "int a[5] = {4, -7, 12, 3, -2};\n"
      "printf(\"%d %d\\n\", kmin_seq(a, 5), kmin_par(a, 5));\n");
  EXPECT_NE(out.find("-7 -7"), std::string::npos) << out;
}

TEST(DslIntegration, ElementwiseSelectClampsValues) {
  Built b = Build(
      "kernel clamp(A: i32[N]) -> B: i32[N] { auto i in 0..N { "
      "B[i] = select(A[i] > 10, 10, select(A[i] < 0, 0, A[i])) } }");
  EXPECT_EQ(b.report.verdict, Verdict::SAFE);
  std::string out = CompileAndRunWith(b, "clamp",
      "int a[5] = {-4, 3, 10, 11, 99}; int s[5], p[5];\n"
      "clamp_seq(a, s, 5); clamp_par(a, p, 5);\n"
      "for (int i = 0; i < 5; i++) printf(\"%d/%d \", s[i], p[i]);\n");
  EXPECT_NE(out.find("0/0 3/3 10/10 10/10 10/10"), std::string::npos) << out;
}

TEST(DslIntegration, I32MultiplicationWrapsInsteadOfTrapping) {
  Built b = Build("kernel sq(A: i32[N]) -> B: i32[N] { auto i in 0..N { B[i] = A[i] * A[i] } }");
  std::string out = CompileAndRunWith(b, "wrap",
      "int a[1] = {2147483647}, r[1];\n"
      "sq_seq(a, r, 1);\n"
      "printf(\"r=%d\\n\", r[0]);\n");
  EXPECT_NE(out.find("r=1"), std::string::npos) << out;
}

TEST(DslCodegen, RectangularNestKeepsTheDefaultSchedule) {
  Built b = Build("kernel k(A: f32[N,M]) -> B: f32[N,M] { auto i in 0..N { auto j in 0..M { B[i, j] = A[i, j] } } }");
  EXPECT_EQ(b.schedule.omp_schedule, "");
  EXPECT_NE(b.gen.c_source.find("#pragma omp parallel for\n"), std::string::npos);
}

TEST(DslCodegen, TriangularNestUsesDynamicScheduling) {
  Built b = Build(kPairs);
  EXPECT_EQ(b.schedule.omp_schedule, "dynamic, 4");
}
