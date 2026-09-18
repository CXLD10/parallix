#include <fstream>
#include <iostream>
#include <sstream>

#include "parallix/ast.h"
#include "parallix/dependence.h"
#include "parallix/lexer.h"
#include "parallix/lowering.h"
#include "parallix/parser.h"
#include "parallix/sema.h"

// The CLI driver: lex -> parse -> sema -> lower to PIR -> dependence
// analysis, printing exactly the output contract in specs/05-phase1-plan.md.

namespace parallix {
namespace {

std::string RenderTokens(const std::vector<Token>& tokens) {
  std::ostringstream out;
  bool first = true;
  for (const Token& tok : tokens) {
    if (tok.kind == TokenKind::END_OF_FILE) continue;
    if (!first) out << " ";
    first = false;
    out << ToString(tok.kind);
    if (tok.kind == TokenKind::IDENT || tok.kind == TokenKind::INT_LITERAL ||
        tok.kind == TokenKind::FLOAT_LITERAL) {
      out << "(" << tok.lexeme << ")";
    }
  }
  return out.str();
}

std::string RenderExpr(const Expr& expr) {
  switch (expr.kind) {
    case ExprKind::INT_LITERAL:
      return std::to_string(static_cast<const IntLiteralExpr&>(expr).value);
    case ExprKind::VAR_REF: {
      const auto& ref = static_cast<const VarRefExpr&>(expr);
      if (ref.subscripts.empty()) return ref.name;
      std::ostringstream out;
      out << ref.name << "[";
      for (size_t i = 0; i < ref.subscripts.size(); ++i) {
        if (i > 0) out << ", ";
        out << RenderExpr(*ref.subscripts[i]);
      }
      out << "]";
      return out.str();
    }
    case ExprKind::BINARY: {
      const auto& bin = static_cast<const BinaryExpr&>(expr);
      return RenderExpr(*bin.lhs) + " " + ToString(bin.op) + " " + RenderExpr(*bin.rhs);
    }
  }
  return "<?>";
}

std::string RenderType(const TypeNode& type) {
  std::ostringstream out;
  out << ToString(type.base);
  if (!type.dims.empty()) {
    out << "[";
    for (size_t i = 0; i < type.dims.size(); ++i) {
      if (i > 0) out << ", ";
      const Dim& dim = type.dims[i];
      out << (dim.is_ident ? dim.ident : std::to_string(dim.int_value));
    }
    out << "]";
  }
  return out.str();
}

std::string RenderStmt(const Stmt& stmt) {
  switch (stmt.kind) {
    case StmtKind::LOOP: {
      const auto& loop = static_cast<const LoopStmt&>(stmt);
      std::ostringstream out;
      out << ToString(loop.mode) << " " << loop.index_var << " in " << RenderExpr(*loop.lower)
          << ".." << RenderExpr(*loop.upper) << " { ";
      for (size_t i = 0; i < loop.body.size(); ++i) {
        if (i > 0) out << "; ";
        out << RenderStmt(*loop.body[i]);
      }
      out << " }";
      return out.str();
    }
    case StmtKind::REDUCTION: {
      const auto& red = static_cast<const ReductionStmt&>(stmt);
      std::ostringstream out;
      out << "reduce(" << ToString(red.op) << ") " << red.index_var << " in "
          << RenderExpr(*red.lower) << ".." << RenderExpr(*red.upper) << " { ";
      for (size_t i = 0; i < red.body.size(); ++i) {
        if (i > 0) out << "; ";
        out << RenderStmt(*red.body[i]);
      }
      out << " }";
      return out.str();
    }
    case StmtKind::ASSIGN: {
      const auto& assign = static_cast<const AssignStmt&>(stmt);
      return RenderExpr(*assign.lvalue) + " " + ToString(assign.op) + " " +
             RenderExpr(*assign.rhs);
    }
  }
  return "<?>";
}

std::string RenderKernel(const KernelDecl& kernel) {
  std::ostringstream out;
  out << "kernel " << kernel.name << "(";
  for (size_t i = 0; i < kernel.params.size(); ++i) {
    if (i > 0) out << ", ";
    out << kernel.params[i].name << ": " << RenderType(kernel.params[i].type);
  }
  out << ") -> " << kernel.ret.name << ": " << RenderType(kernel.ret.type) << " { ";
  for (size_t i = 0; i < kernel.body.size(); ++i) {
    if (i > 0) out << "; ";
    out << RenderStmt(*kernel.body[i]);
  }
  out << " }";
  return out.str();
}

std::string RenderAccessFunction(const AccessFunction& access) {
  if (access.index_var.empty()) return std::to_string(access.constant);
  std::ostringstream out;
  if (access.coefficient == 1) {
    out << access.index_var;
  } else if (access.coefficient == -1) {
    out << "-" << access.index_var;
  } else {
    out << access.coefficient << "*" << access.index_var;
  }
  if (access.constant > 0) out << " + " << access.constant;
  if (access.constant < 0) out << " - " << -access.constant;
  return out.str();
}

std::string RenderReference(const Reference& ref) {
  std::ostringstream out;
  out << ref.array_name << "[";
  for (size_t i = 0; i < ref.access.size(); ++i) {
    if (i > 0) out << ", ";
    out << RenderAccessFunction(ref.access[i]);
  }
  out << "](" << ToString(ref.kind) << ")";
  return out.str();
}

void RunKernel(const KernelDecl& kernel_ast, const std::vector<Token>& tokens) {
  std::cout << "TOKENS: " << RenderTokens(tokens) << "\n";
  std::cout << "AST: " << RenderKernel(kernel_ast) << "\n";

  Sema sema;
  try {
    sema.AnalyzeKernel(kernel_ast);
  } catch (const SemaError& e) {
    std::cout << "SEMANTIC ERROR: " << e.what() << "\n";
    return;
  }
  std::cout << "SEMANTIC INFO: kernel '" << kernel_ast.name
            << "' passed affine-boundedness, shape, reduction-validation, and "
               "symbol-resolution checks (specs/02, rules 1-4).\n";

  PIRKernel pir = LowerKernel(kernel_ast);

  std::ostringstream loops;
  loops << pir.loop_nest.size() << " loop" << (pir.loop_nest.size() == 1 ? "" : "s");
  if (!pir.loop_nest.empty()) {
    loops << " -- ";
    for (size_t i = 0; i < pir.loop_nest.size(); ++i) {
      if (i > 0) loops << "; ";
      const LoopLevel& level = pir.loop_nest[i];
      loops << level.index_var << " (" << ToString(level.mode) << ") in ["
            << level.lower.ToString() << ", " << level.upper.ToString() << ")";
    }
  }
  std::cout << "LOOP ANALYSIS: " << loops.str() << "\n";

  DependenceReport report = AnalyzeDependence(pir);
  std::ostringstream dep;
  if (report.pairs.empty()) {
    dep << "no array reference pairs to compare";
    if (pir.is_reduction) {
      dep << " (the only reduction-related dependency is on the scalar accumulator '"
          << pir.reduce_accumulator << "', which is not a PIR array reference; see Step 5)";
    }
  } else {
    dep << "comparing " << report.pairs.size() << " reference pair(s): ";
    for (size_t i = 0; i < report.pairs.size(); ++i) {
      if (i > 0) dep << "; ";
      const PairResult& pr = report.pairs[i];
      dep << RenderReference(pir.references[pr.ref_a_index]) << " vs "
          << RenderReference(pir.references[pr.ref_b_index]) << ": " << pr.reasoning;
    }
  }
  std::cout << "DEPENDENCE ANALYSIS: " << dep.str() << "\n";

  std::cout << "PARALLELIZATION: " << ToString(report.verdict) << ", " << report.verdict_reason
            << "\n";
}

}  // namespace
}  // namespace parallix

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: parallix <file.plx>\n";
    return 1;
  }

  std::ifstream in(argv[1]);
  if (!in) {
    std::cerr << "error: could not open '" << argv[1] << "'\n";
    return 1;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  std::string source = buf.str();

  try {
    parallix::Lexer lexer(source);
    std::vector<parallix::Token> tokens = lexer.Tokenize();

    parallix::Parser parser(tokens);
    parallix::Program program = parser.ParseProgram();

    for (const parallix::KernelDecl& kernel : program.kernels) {
      parallix::RunKernel(kernel, tokens);
    }
  } catch (const parallix::LexError& e) {
    std::cerr << "lex error: " << e.what() << "\n";
    return 1;
  } catch (const parallix::ParseError& e) {
    std::cerr << "parse error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
