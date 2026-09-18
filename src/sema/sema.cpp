#include "parallix/sema.h"

#include <sstream>

namespace parallix {

namespace {

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
      std::ostringstream out;
      out << ExprToString(*bin.lhs) << " " << ToString(bin.op) << " " << ExprToString(*bin.rhs);
      return out.str();
    }
  }
  return "<?>";
}

struct AffineInfo {
  bool affine;
  bool depends_on_index;
};

AffineInfo AnalyzeAffine(const Expr& expr, const SymbolTable& table) {
  switch (expr.kind) {
    case ExprKind::INT_LITERAL:
      return {true, false};
    case ExprKind::VAR_REF: {
      const auto& ref = static_cast<const VarRefExpr&>(expr);
      if (!ref.subscripts.empty()) {
        // An array read nested inside a subscript/bound expression is never
        // affine: it is not a linear combination of indices and constants.
        return {false, false};
      }
      return {true, table.IsLoopIndex(ref.name)};
    }
    case ExprKind::BINARY: {
      const auto& bin = static_cast<const BinaryExpr&>(expr);
      AffineInfo lhs = AnalyzeAffine(*bin.lhs, table);
      AffineInfo rhs = AnalyzeAffine(*bin.rhs, table);
      bool depends = lhs.depends_on_index || rhs.depends_on_index;
      bool affine;
      if (bin.op == BinaryOp::ADD || bin.op == BinaryOp::SUB) {
        affine = lhs.affine && rhs.affine;
      } else {
        // MUL or DIV: affine only if at least one side is index-independent
        // (specs/02 rule 1).
        affine = lhs.affine && rhs.affine && !(lhs.depends_on_index && rhs.depends_on_index);
      }
      return {affine, depends};
    }
  }
  return {false, false};
}

bool ExprReferencesName(const Expr& expr, const std::string& name) {
  switch (expr.kind) {
    case ExprKind::INT_LITERAL:
      return false;
    case ExprKind::VAR_REF: {
      const auto& ref = static_cast<const VarRefExpr&>(expr);
      if (ref.name == name) return true;
      for (const auto& sub : ref.subscripts) {
        if (ExprReferencesName(*sub, name)) return true;
      }
      return false;
    }
    case ExprKind::BINARY: {
      const auto& bin = static_cast<const BinaryExpr&>(expr);
      return ExprReferencesName(*bin.lhs, name) || ExprReferencesName(*bin.rhs, name);
    }
  }
  return false;
}

}  // namespace

void Sema::AnalyzeKernel(const KernelDecl& kernel, SymbolTable* table_out) {
  kernel_name_ = kernel.name;
  SymbolTable table;
  for (const Param& param : kernel.params) {
    table.DeclareBinding(param, SymbolKind::PARAM);
  }
  table.DeclareBinding(kernel.ret, SymbolKind::RETURN);

  CheckStmtList(kernel.body, table, /*inside_reduction=*/false, ReduceOp::ADD);

  if (table_out != nullptr) *table_out = table;
}

void Sema::ResolveIdentifiers(const Expr& expr, const SymbolTable& table) {
  switch (expr.kind) {
    case ExprKind::INT_LITERAL:
      return;
    case ExprKind::VAR_REF: {
      const auto& ref = static_cast<const VarRefExpr&>(expr);
      if (table.Lookup(ref.name) == nullptr) {
        throw SemaError("undeclared identifier '" + ref.name + "' referenced in kernel '" +
                         kernel_name_ +
                         "': it is neither a parameter, a return binding, an array "
                         "dimension, nor an enclosing loop index (specs/02 rule 4)");
      }
      for (const auto& sub : ref.subscripts) {
        ResolveIdentifiers(*sub, table);
      }
      return;
    }
    case ExprKind::BINARY: {
      const auto& bin = static_cast<const BinaryExpr&>(expr);
      ResolveIdentifiers(*bin.lhs, table);
      ResolveIdentifiers(*bin.rhs, table);
      return;
    }
  }
}

void Sema::CheckShapes(const Expr& expr, const SymbolTable& table) {
  switch (expr.kind) {
    case ExprKind::INT_LITERAL:
      return;
    case ExprKind::VAR_REF: {
      const auto& ref = static_cast<const VarRefExpr&>(expr);
      const Symbol* sym = table.Lookup(ref.name);
      // Already validated by ResolveIdentifiers before CheckShapes runs.
      size_t declared_dims =
          (sym->kind == SymbolKind::PARAM || sym->kind == SymbolKind::RETURN)
              ? sym->type.dims.size()
              : 0;
      if (ref.subscripts.size() != declared_dims) {
        throw SemaError("shape mismatch in kernel '" + kernel_name_ + "': '" + ref.name +
                         "' is declared with " + std::to_string(declared_dims) +
                         " dimension(s) but referenced with " +
                         std::to_string(ref.subscripts.size()) +
                         " subscript(s) in `" + ExprToString(ref) + "` (specs/02 rule 2)");
      }
      for (const auto& sub : ref.subscripts) {
        CheckShapes(*sub, table);
      }
      return;
    }
    case ExprKind::BINARY: {
      const auto& bin = static_cast<const BinaryExpr&>(expr);
      CheckShapes(*bin.lhs, table);
      CheckShapes(*bin.rhs, table);
      return;
    }
  }
}

void Sema::CheckSubscriptsAffine(const Expr& expr, const SymbolTable& table) {
  switch (expr.kind) {
    case ExprKind::INT_LITERAL:
      return;
    case ExprKind::VAR_REF: {
      const auto& ref = static_cast<const VarRefExpr&>(expr);
      for (const auto& sub : ref.subscripts) {
        AffineInfo info = AnalyzeAffine(*sub, table);
        if (!info.affine) {
          throw SemaError("non-affine subscript `" + ExprToString(*sub) +
                           "` in array reference `" + ExprToString(ref) +
                           "`: subscripts must be a linear combination of loop indices "
                           "and constants (specs/02 rule 1)");
        }
        CheckSubscriptsAffine(*sub, table);
      }
      return;
    }
    case ExprKind::BINARY: {
      const auto& bin = static_cast<const BinaryExpr&>(expr);
      CheckSubscriptsAffine(*bin.lhs, table);
      CheckSubscriptsAffine(*bin.rhs, table);
      return;
    }
  }
}

void Sema::CheckBoundAffine(const Expr& bound, const SymbolTable& table,
                             const std::string& loop_var, const char* which) {
  AffineInfo info = AnalyzeAffine(bound, table);
  if (!info.affine) {
    throw SemaError("non-affine " + std::string(which) + " bound `" + ExprToString(bound) +
                     "` for loop index '" + loop_var + "' in kernel '" + kernel_name_ +
                     "': loop bounds must be a linear combination of enclosing loop "
                     "indices and constants (specs/02 rule 1)");
  }
  // A bound may itself contain array-ref subscripts in principle; validate them too.
  CheckSubscriptsAffine(bound, table);
}

void Sema::ValidateReductionAssign(const AssignStmt& assign, ReduceOp op) {
  if (!assign.lvalue->IsScalar()) return;  // only scalar accumulators are reduction-checked
  const std::string& name = assign.lvalue->name;
  // '+=' denotes accumulation by construction even though the rhs doesn't
  // textually repeat the lvalue's name (`s += A[i]` means `s = s + A[i]`).
  bool self_referential =
      assign.op == AssignOp::ADD_SET || ExprReferencesName(*assign.rhs, name);
  if (!self_referential) {
    return;  // plain initialization (e.g. `s = 0`), not an accumulation step
  }

  bool valid = false;
  if (op == ReduceOp::ADD) {
    valid = assign.op == AssignOp::ADD_SET ||
            (assign.op == AssignOp::SET && assign.rhs->kind == ExprKind::BINARY &&
             static_cast<const BinaryExpr&>(*assign.rhs).op == BinaryOp::ADD);
  } else if (op == ReduceOp::MUL) {
    valid = assign.op == AssignOp::SET && assign.rhs->kind == ExprKind::BINARY &&
            static_cast<const BinaryExpr&>(*assign.rhs).op == BinaryOp::MUL;
  } else {
    // MIN/MAX: this grammar has no comparison or min/max call syntax, so no
    // arithmetic combination can ever legitimately represent it. Any
    // self-referential combining assignment here is therefore a mismatch —
    // a documented simplification (constitution rule 5), not a silent gap.
    valid = false;
  }

  if (!valid) {
    throw SemaError("reduction operator mismatch in kernel '" + kernel_name_ + "': '" + name +
                     "' is combined with '" + ToString(assign.op) + "' inside reduce(" +
                     ToString(op) + ") { }, but only the declared operator '" + ToString(op) +
                     "' is licensed there (specs/02 rule 3)");
  }
}

void Sema::CheckStmtList(const std::vector<StmtPtr>& stmts, SymbolTable& table,
                          bool inside_reduction, ReduceOp enclosing_op) {
  for (const auto& stmt : stmts) {
    CheckStmt(*stmt, table, inside_reduction, enclosing_op);
  }
}

void Sema::CheckStmt(const Stmt& stmt, SymbolTable& table, bool inside_reduction,
                      ReduceOp enclosing_op) {
  switch (stmt.kind) {
    case StmtKind::LOOP: {
      const auto& loop = static_cast<const LoopStmt&>(stmt);
      ResolveIdentifiers(*loop.lower, table);
      ResolveIdentifiers(*loop.upper, table);
      CheckBoundAffine(*loop.lower, table, loop.index_var, "lower");
      CheckBoundAffine(*loop.upper, table, loop.index_var, "upper");

      table.PushLoopIndex(loop.index_var);
      CheckStmtList(loop.body, table, inside_reduction, enclosing_op);
      table.PopLoopIndex();
      return;
    }
    case StmtKind::REDUCTION: {
      const auto& red = static_cast<const ReductionStmt&>(stmt);
      ResolveIdentifiers(*red.lower, table);
      ResolveIdentifiers(*red.upper, table);
      CheckBoundAffine(*red.lower, table, red.index_var, "lower");
      CheckBoundAffine(*red.upper, table, red.index_var, "upper");

      table.PushLoopIndex(red.index_var);
      CheckStmtList(red.body, table, /*inside_reduction=*/true, red.op);
      table.PopLoopIndex();
      return;
    }
    case StmtKind::ASSIGN: {
      const auto& assign = static_cast<const AssignStmt&>(stmt);
      ResolveIdentifiers(*assign.lvalue, table);
      ResolveIdentifiers(*assign.rhs, table);
      CheckShapes(*assign.lvalue, table);
      CheckShapes(*assign.rhs, table);
      CheckSubscriptsAffine(*assign.lvalue, table);
      CheckSubscriptsAffine(*assign.rhs, table);
      if (inside_reduction) {
        ValidateReductionAssign(assign, enclosing_op);
      }
      return;
    }
  }
}

}  // namespace parallix
