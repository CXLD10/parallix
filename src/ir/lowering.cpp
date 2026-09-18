#include "parallix/lowering.h"

#include <algorithm>
#include <sstream>

namespace parallix {

namespace {

std::string ExprToString(const Expr& expr);

std::string ExprToString(const Expr& expr) {
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
        out << ExprToString(*ref.subscripts[i]);
      }
      out << "]";
      return out.str();
    }
    case ExprKind::BINARY: {
      const auto& bin = static_cast<const BinaryExpr&>(expr);
      return ExprToString(*bin.lhs) + " " + ToString(bin.op) + " " + ExprToString(*bin.rhs);
    }
  }
  return "<?>";
}

// Merges `b`'s terms into `a` (combining coefficients for shared names,
// dropping any that cancel to zero).
void MergeTerms(std::vector<std::pair<std::string, long long>>& a,
                 const std::vector<std::pair<std::string, long long>>& b, long long sign) {
  for (const auto& [name, coeff] : b) {
    bool found = false;
    for (auto& term : a) {
      if (term.first == name) {
        term.second += sign * coeff;
        found = true;
        break;
      }
    }
    if (!found) a.push_back({name, sign * coeff});
  }
  a.erase(std::remove_if(a.begin(), a.end(), [](const auto& t) { return t.second == 0; }),
          a.end());
}

// Extracts the affine form of an expression that Sema has already validated
// as affine (specs/02 rule 1): sum(coefficient * index/dim) + constant.
AffineExpr ExtractAffine(const Expr& expr) {
  switch (expr.kind) {
    case ExprKind::INT_LITERAL:
      return AffineExpr{{}, static_cast<const IntLiteralExpr&>(expr).value};
    case ExprKind::VAR_REF: {
      const auto& ref = static_cast<const VarRefExpr&>(expr);
      // Sema guarantees this is a bare identifier here, never a subscripted
      // array read (those are rejected as non-affine before lowering runs).
      return AffineExpr{{{ref.name, 1}}, 0};
    }
    case ExprKind::BINARY: {
      const auto& bin = static_cast<const BinaryExpr&>(expr);
      AffineExpr lhs = ExtractAffine(*bin.lhs);
      AffineExpr rhs = ExtractAffine(*bin.rhs);
      switch (bin.op) {
        case BinaryOp::ADD: {
          AffineExpr result = lhs;
          MergeTerms(result.terms, rhs.terms, 1);
          result.constant += rhs.constant;
          return result;
        }
        case BinaryOp::SUB: {
          AffineExpr result = lhs;
          MergeTerms(result.terms, rhs.terms, -1);
          result.constant -= rhs.constant;
          return result;
        }
        case BinaryOp::MUL: {
          // Sema guarantees at least one side has no terms (a compile-time
          // constant subtree).
          const AffineExpr& varying = lhs.terms.empty() ? rhs : lhs;
          const AffineExpr& constant_side = lhs.terms.empty() ? lhs : rhs;
          AffineExpr result;
          for (const auto& [name, coeff] : varying.terms) {
            result.terms.push_back({name, coeff * constant_side.constant});
          }
          result.constant = varying.constant * constant_side.constant;
          return result;
        }
        case BinaryOp::DIV: {
          // Sema guarantees the divisor has no terms.
          AffineExpr result;
          for (const auto& [name, coeff] : lhs.terms) {
            result.terms.push_back({name, coeff / rhs.constant});
          }
          result.constant = lhs.constant / rhs.constant;
          return result;
        }
      }
      return AffineExpr{};
    }
  }
  return AffineExpr{};
}

AccessFunction ToAccessFunction(const Expr& subscript) {
  AffineExpr affine = ExtractAffine(subscript);
  AccessFunction access;
  access.constant = affine.constant;
  if (affine.terms.empty()) {
    return access;  // index-independent dimension (coefficient 0, no index_var)
  }
  if (affine.terms.size() > 1) {
    throw LoweringError(
        "Phase 1 PIR only supports a single loop index per array-dimension subscript; "
        "found multiple indices in `" +
        ExprToString(subscript) + "` (out of scope for this phase, see specs/03)");
  }
  access.index_var = affine.terms[0].first;
  access.coefficient = affine.terms[0].second;
  return access;
}

// Collects every array read/write appearing anywhere within `expr` (used for
// an assignment's rhs, which may combine several array reads).
void CollectReferences(const Expr& expr, RefKind kind, std::vector<Reference>& out) {
  switch (expr.kind) {
    case ExprKind::INT_LITERAL:
      return;
    case ExprKind::VAR_REF: {
      const auto& ref = static_cast<const VarRefExpr&>(expr);
      if (!ref.subscripts.empty()) {
        Reference reference;
        reference.array_name = ref.name;
        reference.kind = kind;
        reference.loc = SourceLoc{ref.line, ref.column};
        for (const auto& sub : ref.subscripts) {
          reference.access.push_back(ToAccessFunction(*sub));
        }
        out.push_back(std::move(reference));
      }
      return;
    }
    case ExprKind::BINARY: {
      const auto& bin = static_cast<const BinaryExpr&>(expr);
      CollectReferences(*bin.lhs, kind, out);
      CollectReferences(*bin.rhs, kind, out);
      return;
    }
  }
}

void LowerStmt(const Stmt& stmt, PIRKernel& kernel);

void LowerStmtList(const std::vector<StmtPtr>& stmts, PIRKernel& kernel) {
  for (const auto& stmt : stmts) LowerStmt(*stmt, kernel);
}

void LowerStmt(const Stmt& stmt, PIRKernel& kernel) {
  switch (stmt.kind) {
    case StmtKind::LOOP: {
      const auto& loop = static_cast<const LoopStmt&>(stmt);
      LoopLevel level;
      level.index_var = loop.index_var;
      level.lower = ExtractAffine(*loop.lower);
      level.upper = ExtractAffine(*loop.upper);
      level.mode = loop.mode;
      kernel.loop_nest.push_back(std::move(level));
      LowerStmtList(loop.body, kernel);
      return;
    }
    case StmtKind::REDUCTION: {
      const auto& red = static_cast<const ReductionStmt&>(stmt);
      LoopLevel level;
      level.index_var = red.index_var;
      level.lower = ExtractAffine(*red.lower);
      level.upper = ExtractAffine(*red.upper);
      // A reduce(op) loop carries a sequential dependency by construction
      // (specs/04 step 5) — there is no source-level mode keyword for it.
      level.mode = LoopMode::SEQ;
      kernel.loop_nest.push_back(std::move(level));
      kernel.is_reduction = true;
      kernel.reduce_op = red.op;
      LowerStmtList(red.body, kernel);
      return;
    }
    case StmtKind::ASSIGN: {
      const auto& assign = static_cast<const AssignStmt&>(stmt);
      if (assign.lvalue->IsScalar()) {
        if (kernel.is_reduction && kernel.reduce_accumulator.empty()) {
          kernel.reduce_accumulator = assign.lvalue->name;
        }
      } else {
        Reference write;
        write.array_name = assign.lvalue->name;
        write.kind = RefKind::WRITE;
        write.loc = SourceLoc{assign.lvalue->line, assign.lvalue->column};
        for (const auto& sub : assign.lvalue->subscripts) {
          write.access.push_back(ToAccessFunction(*sub));
        }
        kernel.references.push_back(std::move(write));
      }
      CollectReferences(*assign.rhs, RefKind::READ, kernel.references);
      return;
    }
  }
}

}  // namespace

PIRKernel LowerKernel(const KernelDecl& kernel) {
  PIRKernel pir;
  pir.name = kernel.name;
  LowerStmtList(kernel.body, pir);
  return pir;
}

}  // namespace parallix
