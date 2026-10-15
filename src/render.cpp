#include "parallix/render.h"

#include <sstream>

namespace parallix {

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
    case ExprKind::CALL: {
      const auto& call = static_cast<const CallExpr&>(expr);
      std::string out = std::string(ToString(call.callee)) + "(";
      for (size_t i = 0; i < call.args.size(); ++i) {
        if (i > 0) out += ", ";
        out += RenderExpr(*call.args[i]);
      }
      return out + ")";
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


}  // namespace parallix
